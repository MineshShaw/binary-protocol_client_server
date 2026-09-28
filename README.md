# Binary HTTP client/server

A small C11 implementation of a binary, HTTP/1.0-inspired file-transfer protocol over TCP. It includes a static-file server (`bserve`), a command-line client (`bcurl`), shared framing/header-codec code, and CTest unit tests.

## Build and run

```sh
cmake -S . -B cmake-build-local
cmake --build cmake-build-local
ctest --test-dir cmake-build-local --output-on-failure

./cmake-build-local/bserve ./tests/dummy_files/www 9000
./cmake-build-local/bcurl [-v] localhost:9000/index.html
```

`bcurl` defaults to GET and can issue HEAD or POST requests. POST sends a file as the request body; `bserve` echoes that body without writing to the filesystem:

```sh
./cmake-build-local/bcurl -X HEAD localhost:9000/index.html
./cmake-build-local/bcurl -X POST --data upload.bin localhost:9000/echo
```

`-v` prints a hexdump of each complete frame sent and received to standard error. The client opens one IPv4 TCP connection and sends its request and reads the response on that connection; IPv6 literals are not supported. The server keeps each connection open after each response and can process subsequent requests on it.

See [PROTOCOL.md](PROTOCOL.md) for the wire specification, unknown-frame rule, and an annotated request/response hexdump.
