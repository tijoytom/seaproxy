#include "resp.hh"

#include <charconv>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace seaproxy::resp {
namespace {

constexpr std::size_t max_nesting_depth = 128;
constexpr std::size_t max_aggregate_items = 1'000'000;

std::optional<std::size_t> find_crlf(
        std::string_view buffer,
        std::size_t start) {
    const auto position = buffer.find("\r\n", start);
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    return position;
}

std::optional<std::size_t> parse_count(std::string_view text) {
    std::int64_t value = 0;
    const auto [end, error] =
            std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error("RESP length is not a valid integer");
    }
    if (value == -1) {
        return std::nullopt;
    }
    if (value < -1) {
        throw std::runtime_error("RESP length cannot be less than -1");
    }
    return static_cast<std::size_t>(value);
}

std::optional<std::size_t> parse_frame(
        std::string_view buffer,
        std::size_t start,
        std::size_t depth);

std::optional<std::size_t> parse_blob(
        std::string_view buffer,
        std::size_t start) {
    const auto header_end = find_crlf(buffer, start + 1);
    if (!header_end) {
        return std::nullopt;
    }
    const auto length = parse_count(buffer.substr(
            start + 1, *header_end - start - 1));
    if (!length) {
        return *header_end + 2;
    }
    if (*length > max_frame_size) {
        throw std::runtime_error("RESP blob exceeds the 64 MiB limit");
    }
    const auto data_start = *header_end + 2;
    if (*length > std::numeric_limits<std::size_t>::max() - data_start - 2) {
        throw std::runtime_error("RESP blob length overflow");
    }
    const auto data_end = data_start + *length;
    const auto frame_end = data_end + 2;
    if (frame_end > buffer.size()) {
        return std::nullopt;
    }
    if (buffer.substr(data_end, 2) != "\r\n") {
        throw std::runtime_error("RESP blob is not terminated by CRLF");
    }
    return frame_end;
}

std::optional<std::size_t> parse_aggregate(
        std::string_view buffer,
        std::size_t start,
        std::size_t depth) {
    const auto header_end = find_crlf(buffer, start + 1);
    if (!header_end) {
        return std::nullopt;
    }
    auto item_count = parse_count(buffer.substr(
            start + 1, *header_end - start - 1));
    if (!item_count) {
        return *header_end + 2;
    }
    if (buffer[start] == '%' || buffer[start] == '|') {
        if (*item_count > max_aggregate_items / 2) {
            throw std::runtime_error("RESP aggregate length overflow");
        }
        *item_count *= 2;
    }
    if (*item_count > max_aggregate_items) {
        throw std::runtime_error("RESP aggregate has too many items");
    }

    auto position = *header_end + 2;
    for (std::size_t index = 0; index < *item_count; ++index) {
        const auto end = parse_frame(buffer, position, depth + 1);
        if (!end) {
            return std::nullopt;
        }
        position = *end;
    }
    return position;
}

std::optional<std::size_t> parse_frame(
        std::string_view buffer,
        std::size_t start,
        std::size_t depth) {
    if (depth > max_nesting_depth) {
        throw std::runtime_error("RESP nesting exceeds the supported limit");
    }
    if (start >= buffer.size()) {
        return std::nullopt;
    }

    switch (buffer[start]) {
    case '+':
    case '-':
    case ':':
    case ',':
    case '(':
    case '#':
    case '_': {
        const auto end = find_crlf(buffer, start + 1);
        return end ? std::optional<std::size_t>(*end + 2) : std::nullopt;
    }
    case '$':
    case '!':
    case '=':
        return parse_blob(buffer, start);
    case '*':
    case '~':
    case '>':
    case '%':
    case '|':
        return parse_aggregate(buffer, start, depth);
    default: {
        const auto end = find_crlf(buffer, start);
        return end ? std::optional<std::size_t>(*end + 2) : std::nullopt;
    }
    }
}

std::optional<std::string_view> array_argument(
        std::string_view frame,
        std::size_t index) {
    const auto header_end = find_crlf(frame, 1);
    if (!header_end) {
        return std::nullopt;
    }
    const auto count = parse_count(frame.substr(1, *header_end - 1));
    if (!count || index >= *count) {
        return std::nullopt;
    }

    auto position = *header_end + 2;
    for (std::size_t item = 0; item <= index; ++item) {
        if (position >= frame.size()) {
            return std::nullopt;
        }
        if (frame[position] == '$') {
            const auto length_end = find_crlf(frame, position + 1);
            if (!length_end) {
                return std::nullopt;
            }
            const auto length = parse_count(frame.substr(
                    position + 1, *length_end - position - 1));
            if (!length) {
                return std::nullopt;
            }
            const auto data_start = *length_end + 2;
            if (*length > frame.size() - data_start) {
                return std::nullopt;
            }
            const auto argument = frame.substr(data_start, *length);
            if (item == index) {
                return argument;
            }
            position = data_start + *length + 2;
        } else if (frame[position] == '+') {
            const auto argument_end = find_crlf(frame, position + 1);
            if (!argument_end) {
                return std::nullopt;
            }
            const auto argument =
                    frame.substr(position + 1, *argument_end - position - 1);
            if (item == index) {
                return argument;
            }
            position = *argument_end + 2;
        } else {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::string_view> inline_argument(
        std::string_view frame,
        std::size_t index) {
    const auto line_end = find_crlf(frame, 0);
    if (!line_end) {
        return std::nullopt;
    }
    const auto line = frame.substr(0, *line_end);
    std::size_t position = 0;
    std::size_t current = 0;
    while (position < line.size()) {
        while (position < line.size() &&
               (line[position] == ' ' || line[position] == '\t')) {
            ++position;
        }
        const auto start = position;
        while (position < line.size() &&
               line[position] != ' ' && line[position] != '\t') {
            ++position;
        }
        if (start != position) {
            if (current == index) {
                return line.substr(start, position - start);
            }
            ++current;
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::size_t> frame_length(std::string_view buffer) {
    if (buffer.empty()) {
        return std::nullopt;
    }
    return parse_frame(buffer, 0, 0);
}

std::optional<std::string_view> command_argument(
        std::string_view frame,
        std::size_t index) {
    return frame.starts_with('*')
            ? array_argument(frame, index)
            : inline_argument(frame, index);
}

std::optional<std::string_view> command_name(std::string_view frame) {
    const auto command = command_argument(frame, 0);
    if (!command || command->empty()) {
        return std::nullopt;
    }
    for (const unsigned char byte : *command) {
        if (byte > 0x7f) {
            return std::nullopt;
        }
    }
    return command;
}

std::optional<std::vector<std::string_view>> command_arguments(
        std::string_view frame) {
    std::vector<std::string_view> arguments;
    for (std::size_t index = 0;; ++index) {
        auto argument = command_argument(frame, index);
        if (!argument) {
            break;
        }
        arguments.push_back(*argument);
    }
    if (arguments.empty()) {
        return std::nullopt;
    }
    return arguments;
}

}  // namespace seaproxy::resp
