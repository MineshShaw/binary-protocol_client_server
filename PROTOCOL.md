# Binary HTTP Protocol

This protocol carries a deliberately small HTTP/1.0-style request/response model over a persistent TCP connection. Every byte on the connection belongs to a framed binary message; HTTP request lines, CRLF delimiters, and text-formatted HTTP header blocks are never transmitted. All multi-byte integers are unsigned and big-endian. Header names/values are text without NUL bytes, carried only as length-delimited fields within binary frames; DATA is binary-safe.

## Frame format

Every frame begins with this fixed 8-byte header:

| Bytes | Width | Field | Meaning |
|---|---:|---|---|
| 0-2 | 24 bits | Length | Number of payload bytes; max 16,777,215 |
| 3 | 8 bits | Type | `0x01` HEADERS, `0x02` DATA, `0x03` ERROR |
| 4 | 8 bits | Flags | Bit 0 is END_STREAM; all other bits are reserved |
| 5-7 | 24 bits | Request ID | Correlates request and response; zero is invalid for requests |

Length and request ID are 24-bit fields to keep the header compact while permitting practical payloads and multiple correlated messages. The fixed size lets a receiver locate the payload and next frame without delimiters.

HEADERS payloads encode consecutive fields in the HPACK-lite form below. DATA payloads contain unmodified entity bytes. END_STREAM marks the last frame in a message. A bodyless request sets END_STREAM on HEADERS; a request with a body sends DATA frame(s), with END_STREAM on the last one. A response contains HEADERS followed by DATA when it has a body; HEAD or an empty body ends on HEADERS. Content-Length is the entity size (for HEAD, the size the corresponding GET would return).

## Header block encoding

Each field is encoded consecutively:

* Static name: `[1-byte index][2-byte value length][value bytes]`.
* Literal name: `[0x00][1-byte name length][name bytes][2-byte value length][value bytes]`.

Both lengths are big-endian; values are limited to 65,535 bytes and literal names to 255 bytes. The static table is fixed:

| Index | Name | Index | Name |
|---:|---|---:|---|
| 1 | `:method` | 6 | `host` |
| 2 | `:path` | 7 | `user-agent` |
| 3 | `:status` | 8 | `server` |
| 4 | `content-type` | 9 | `date` |
| 5 | `content-length` | 10 | `connection` |

These names are the ten fields used by this implementation. Method values are `GET`, `HEAD`, or `POST`; response status values include `200`, `400`, `404`, and `500`. Other names (for example, `x-request-id`) use the literal form.

## Request and response behavior

The client sends one HEADERS request frame with method, absolute path, host, user-agent, and content-length. GET and HEAD require Content-Length `0`. POST carries the declared number of bytes in DATA frames. The server maps GET/HEAD paths under the configured root directory, serves raw file bytes, and returns 404 when no regular file exists. HEAD returns no body. To give POST deterministic behavior without writing to the document root, this sample server responds 200 with the submitted body unchanged; Content-Type is copied from the request or defaults to `application/octet-stream`.

Responses include `:status`, `content-length`, `content-type`, `date`, `server`, and `connection: keep-alive`. A 4xx or 5xx response causes `bcurl` to exit nonzero. Malformed complete requests receive 400. The server leaves the TCP connection open after a response; EOF or transport failure ends that connection. A `bcurl` invocation sends one request on one TCP connection and never opens a second connection.

After the complete response is received, `bcurl` writes the entity body to standard output as a 16-byte-per-line hex/ASCII hexdump (offset, hexadecimal octets, and printable-byte column). In verbose mode, the hexdump of each complete on-wire frame is additionally written to standard error.

### Unknown types (forward compatibility)

When a receiver sees a Type it does not recognize, it **MUST read and discard exactly Length payload bytes**, then resume parsing at the next 8-byte frame header. It MUST NOT interpret the unknown payload or close the connection merely because the type is unknown. Unknown-frame flags are ignored. This permits future frame types to pass through older peers.

## Annotated exchange

Example: GET `/index.html` from `localhost`, with the server returning `OK\n`. All offsets below are relative to the start of each frame. Request ID is 1.

**Request HEADERS frame (52 bytes total, 44-byte payload):**

```text
00000000  00 00 2c 01 01 00 00 01  01 00 03 47 45 54 02 00
00000010  0b 2f 69 6e 64 65 78 2e  68 74 6d 6c 06 00 09 6c
00000020  6f 63 61 6c 68 6f 73 74  07 00 05 62 63 75 72 6c
00000030  05 00 01 30
```

The first eight bytes are Length `00 00 2c` (44), Type `01` (HEADERS), Flags `01` (END_STREAM; no request body), and Request ID `00 00 01`. The payload then encodes index 1 with value `GET`; index 2 with `/index.html`; index 6 with `localhost`; index 7 with `bcurl`; and index 5 with value `0` (Content-Length). Each field's two-byte value length immediately follows its name index.

**Response HEADERS frame (85 bytes total, 77-byte payload):**

```text
00000000  00 00 4d 01 00 00 00 01  03 00 03 32 30 30 05 00
00000010  01 33 04 00 0a 74 65 78  74 2f 70 6c 61 69 6e 09
00000020  00 1d 4d 6f 6e 2c 20 30  31 20 4a 61 6e 20 32 30
00000030  32 34 20 30 30 3a 30 30  3a 30 30 20 47 4d 54 08
00000040  00 06 62 73 65 72 76 65  0a 00 0a 6b 65 65 70 2d
00000050  61 6c 69 76 65
```

The frame header says Length `00 00 4d` (77), Type HEADERS, Flags `00` (DATA follows), Request ID 1. The payload is status `200` (index 3), Content-Length `3` (index 5), Content-Type `text/plain` (index 4), Date `Mon, 01 Jan 2024 00:00:00 GMT` (index 9), Server `bserve` (index 8), and Connection `keep-alive` (index 10).

**Response DATA frame (11 bytes total):**

```text
00000000  00 00 03 02 01 00 00 01  4f 4b 0a
```

Length is 3, Type is DATA, Flags is END_STREAM, Request ID is 1; payload bytes `4f 4b 0a` are `OK\n`.
