# Binary HTTP Protocol

The protocol maps a small HTTP/1.0 request/response model onto TCP using only binary frames. It sends no HTTP request line, CRLF separators, or text-formatted header block. Multi-byte integers are unsigned big-endian; header names and values are text carried inside length-prefixed binary fields, and DATA bytes are unmodified.

## Frame

Each frame starts with this fixed 8-byte header:

| Frame bytes | Width | Field | Definition |
|---|---:|---|---|
| 0-2 | 24 bits | Length | Payload bytes following the header (0..16,777,215). |
| 3 | 8 bits | Type | `0x01` HEADERS, `0x02` DATA, `0x03` ERROR. |
| 4 | 8 bits | Flags | Bit 0 END_STREAM; remaining bits reserved and zero. |
| 5-7 | 24 bits | Request ID | Correlates response with request; request ID 0 is invalid. |

The 24-bit payload length bounds memory and permits up to 16 MiB minus one per frame. An 8-bit type/flags pair supports extensions without enlarging the header. A 24-bit request ID fits this compact header while identifying concurrent/future exchanges.

TCP carries one request per connection: one HEADERS frame and, for a POST body, one or more DATA frames. END_STREAM marks the last frame. GET and HEAD have no body and set END_STREAM on HEADERS. A response is HEADERS, then optional DATA; HEADERS has END_STREAM for an empty body or HEAD response. Content-Length is the entity size, including for HEAD (the matching GET size). The server sends its response and leaves the socket open until the client closes it.

## Compressed headers and HTTP mapping

Header fields are consecutive in the HEADERS payload:

* Static name: `[index:8][value_length:16][value bytes]`.
* Literal name: `[0:8][name_length:8][name bytes][value_length:16][value bytes]`.

Lengths are byte counts, big-endian. Value length is at most 65,535; literal name length is 1..255. The static name table is fixed:

| Index | Header name | Index | Header name |
|---:|---|---:|---|
| 1 | `:method` | 6 | `host` |
| 2 | `:path` | 7 | `user-agent` |
| 3 | `:status` | 8 | `server` |
| 4 | `content-type` | 9 | `date` |
| 5 | `content-length` | 10 | `connection` |

Methods are the values `GET`, `HEAD`, and `POST`. Status is a three-digit text value inside the binary field: `200` OK, `400` Bad Request, `404` Not Found, `500` Internal Server Error. Additional standard or custom names use the literal form. GET/HEAD serve files under the configured root; POST is a safe echo endpoint and does not write files. Responses include status, content-length, content-type, date, server, and connection fields.

## Forward compatibility and validation

On an unknown Type, a receiver **MUST consume and discard exactly Length payload bytes**, then resume at the next frame header. It MUST NOT parse the unknown payload or close the connection solely because the type is unknown. This is the version-2 extension rule.

Receivers reject truncated/malformed known frames, reserved flag bits, invalid compressed fields, mismatched request IDs, and inconsistent Content-Length. A complete malformed request receives a binary 400 response; a missing file receives 404. `bcurl` exits nonzero for 4xx/5xx. The client opens exactly one TCP connection per invocation.

## Hexdump proof

See [HEXDUMP.md](HEXDUMP.md) for a complete GET/200 request-response exchange and byte-by-byte field ranges.
