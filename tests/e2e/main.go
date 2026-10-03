package main

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"reflect"
	"strconv"
	"strings"
	"time"
)

func main() {
	port := "17000"
	if len(os.Args) == 2 {
		port = os.Args[1]
	}

	connection, reader, err := connect("127.0.0.1:" + port)
	must(err)
	expect(connection, reader, "OK", "SET", "e2e:ordinary", "value")
	expect(connection, reader, "value", "GET", "e2e:ordinary")
	expect(connection, reader, "OK", "MULTI")
	expect(connection, reader, "QUEUED", "SET", "e2e:transaction", "scoped")
	expect(connection, reader, "QUEUED", "GET", "e2e:transaction")
	expect(connection, reader, []any{"OK", "scoped"}, "EXEC")
	must(connection.Close())

	connection, reader, err = connect("127.0.0.1:" + port)
	must(err)
	defer connection.Close()
	expect(connection, reader, "scoped", "GET", "e2e:transaction")
	expect(connection, reader, int64(2), "DEL", "e2e:ordinary", "e2e:transaction")

	fmt.Println("SeaProxy end-to-end test passed")
}

func connect(address string) (net.Conn, *bufio.Reader, error) {
	deadline := time.Now().Add(15 * time.Second)
	for {
		connection, err := net.DialTimeout("tcp", address, time.Second)
		if err == nil {
			must(connection.SetDeadline(time.Now().Add(5 * time.Second)))
			return connection, bufio.NewReader(connection), nil
		}
		if time.Now().After(deadline) {
			return nil, nil, fmt.Errorf("connect to SeaProxy: %w", err)
		}
		time.Sleep(100 * time.Millisecond)
	}
}

func run(connection net.Conn, reader *bufio.Reader, arguments ...string) (any, error) {
	var request strings.Builder
	fmt.Fprintf(&request, "*%d\r\n", len(arguments))
	for _, argument := range arguments {
		fmt.Fprintf(&request, "$%d\r\n%s\r\n", len(argument), argument)
	}
	if _, err := io.WriteString(connection, request.String()); err != nil {
		return nil, err
	}
	return readResponse(reader)
}

func readResponse(reader *bufio.Reader) (any, error) {
	marker, err := reader.ReadByte()
	if err != nil {
		return nil, err
	}
	line, err := readLine(reader)
	if err != nil {
		return nil, err
	}

	switch marker {
	case '+':
		return line, nil
	case '-':
		return nil, errors.New(line)
	case ':':
		return strconv.ParseInt(line, 10, 64)
	case '$':
		length, err := strconv.Atoi(line)
		if err != nil {
			return nil, err
		}
		if length == -1 {
			return nil, nil
		}
		value := make([]byte, length+2)
		if _, err := io.ReadFull(reader, value); err != nil {
			return nil, err
		}
		if string(value[length:]) != "\r\n" {
			return nil, errors.New("invalid bulk string terminator")
		}
		return string(value[:length]), nil
	case '*':
		length, err := strconv.Atoi(line)
		if err != nil {
			return nil, err
		}
		values := make([]any, length)
		for index := range values {
			values[index], err = readResponse(reader)
			if err != nil {
				return nil, err
			}
		}
		return values, nil
	default:
		return nil, fmt.Errorf("unsupported RESP marker %q", marker)
	}
}

func readLine(reader *bufio.Reader) (string, error) {
	line, err := reader.ReadString('\n')
	if err != nil {
		return "", err
	}
	if !strings.HasSuffix(line, "\r\n") {
		return "", errors.New("invalid RESP line terminator")
	}
	return strings.TrimSuffix(line, "\r\n"), nil
}

func must(err error) {
	if err != nil {
		panic(err)
	}
}

func expect(connection net.Conn, reader *bufio.Reader, expected any, arguments ...string) {
	actual, err := run(connection, reader, arguments...)
	must(err)
	if !reflect.DeepEqual(actual, expected) {
		panic(fmt.Sprintf("expected %#v, got %#v", expected, actual))
	}
}
