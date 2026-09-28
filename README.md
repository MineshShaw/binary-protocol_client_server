# HTTP, in Binary

A small C11 course project implementing a custom HTTP/1.0-inspired protocol over TCP. The wire format is entirely binary: fixed-size frame headers and length-delimited compressed header fields, with raw bytes in DATA frames. No HTTP request line or text-formatted HTTP headers are sent.

## Project layout

* `server/bserve.c` — TCP server and static-file handler.
* `client/bcurl.c` — one-connection request client.
* `shared/frame.*` — fixed 8-byte frame header and exact socket I/O.
* `shared/hpack_lite.*` — ten-name static header table and literal-name codec.
* `shared/hexdump.*` — raw and annotated frame dumps.
* `tests/` — codec tests and files used in the manual walkthrough.
* `PROTOCOL.md` — concise wire specification and a complete annotated exchange.

The project requires CMake 3.14+, a C11 compiler, and POSIX sockets; it has no external libraries.

## Compile and test

From the repository root:

```sh
cmake -S . -B cmake-build-local
cmake --build cmake-build-local
ctest --test-dir cmake-build-local --output-on-failure
```

## Manually test client and server

1. In terminal 1, launch the server with the included document root:

   ```sh
   ./cmake-build-local/bserve ./tests/dummy_files/www 9000
   ```

   The server prints every complete incoming request frame to standard error. Its dump includes the raw frame bytes, the fixed-header/payload boundary, and offsets for each compressed header.

2. In terminal 2, fetch the included proof resource:

   ```sh
   ./cmake-build-local/bcurl -v localhost:9000/proof.txt
   ```

   `bcurl` makes one TCP connection, sends a binary GET request, parses the binary response on that same connection, and writes the raw response body (`OK\n`) to standard output. With `-v`, it also prints full annotated sent/received frame dumps to standard error. Compare the exchange to [the annotated proof](PROTOCOL.md#annotated-exchange).

3. Try the other supported methods:

   ```sh
   ./cmake-build-local/bcurl -X HEAD localhost:9000/index.html
   ./cmake-build-local/bcurl -X POST --data ./tests/dummy_files/upload.bin localhost:9000/echo
   ```

   HEAD returns headers with no body. POST is a safe echo operation; the fixture bytes are returned unchanged and never written to disk. Use `-v` with either command to inspect its binary frames.

4. Verify missing-file handling and the nonzero client exit status:

   ```sh
   ./cmake-build-local/bcurl localhost:9000/does-not-exist
   echo $?
   ```

   The response body is `Not Found` and the exit status is nonzero for the 404. Stop the server with Ctrl-C.

Each connection carries one logical request (HEADERS plus optional POST DATA frames). The server sends the response and leaves that socket open until the client closes it. Unknown frame types are skipped by consuming exactly their declared payload length; see `PROTOCOL.md` for framing, header mapping, and byte-level parsing.
