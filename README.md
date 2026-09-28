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

The build places the two submission binaries at the repository root, so the
required commands are `./bserve` and `./bcurl`.

## Manually test client and server

1. Generate a small document root and test file:

   ```sh
   mkdir -p ./demo-root
   printf 'OK\n' > ./demo-root/proof.txt
   ```

   The repository also includes equivalent fixtures under
   `tests/dummy_files/`.

2. In terminal 1, launch the server with the generated document root:

   ```sh
   ./bserve ./demo-root 9000
   ```

   The server prints every complete request and response frame to standard
   error. Each dump includes raw bytes, the fixed-header/payload boundary,
   and offsets for each compressed header.

3. In terminal 2, fetch the generated proof resource:

   ```sh
   ./bcurl -v localhost:9000/proof.txt
   ```

   `bcurl` makes one TCP connection, sends a binary GET request, parses the binary response on that same connection, and writes the raw response body (`OK\n`) to standard output. With `-v`, it also prints full annotated sent/received frame dumps to standard error. Compare the exchange to [the annotated proof](PROTOCOL.md#annotated-exchange).

4. Try the other supported methods:

   ```sh
   ./bcurl -v -X HEAD localhost:9000/proof.txt
   ./bcurl -v -X POST --data ./tests/dummy_files/upload.bin localhost:9000/echo
   ```

   HEAD returns headers with no body. POST is a safe echo operation; the fixture bytes are returned unchanged and never written to disk. Use `-v` with either command to inspect its binary frames.

5. Verify missing-file handling and the nonzero client exit status:

   ```sh
   ./bcurl -v localhost:9000/does-not-exist
   echo $?
   ```

   The response body is `Not Found` and the exit status is nonzero for the 404. Stop the server with Ctrl-C.

6. Verify malformed-path handling and the binary 400 response:

   ```sh
   ./bcurl -v localhost:9000/../not-allowed
   echo $?
   ```

   The server returns `Bad Request` and `bcurl` exits nonzero. No text HTTP
   request line or text header block is sent in either case.

Each connection carries one logical request (HEADERS plus optional POST DATA frames). The server sends the response and leaves that socket open until the client closes it. Unknown frame types are skipped by consuming exactly their declared payload length; see `PROTOCOL.md` for framing, header mapping, and byte-level parsing.
