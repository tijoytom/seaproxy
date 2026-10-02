#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace seaproxy::resp {

constexpr std::size_t max_frame_size = 64 * 1024 * 1024;

class CommandParser {
public:
    explicit CommandParser(std::string_view frame);

    std::optional<std::string_view> next();
    std::optional<std::size_t> remaining() const noexcept;
    bool valid() const noexcept;

private:
    std::string_view _frame;
    std::size_t _position{0};
    std::size_t _inline_end{0};
    std::size_t _remaining{0};
    bool _array{false};
    bool _valid{true};
};

std::optional<std::size_t> frame_length(std::string_view buffer);
std::optional<std::string_view> command_name(std::string_view frame);
std::optional<std::string_view> command_argument(
        std::string_view frame,
        std::size_t index);
std::optional<std::vector<std::string_view>> command_arguments(
        std::string_view frame);

}  // namespace seaproxy::resp
