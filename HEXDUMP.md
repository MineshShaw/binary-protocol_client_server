# Annotated Binary Exchange

This is a complete `GET /proof.txt` exchange from host `localhost`; the included file `tests/dummy_files/www/proof.txt` contains exactly `OK\n`. Dumps include every byte in each frame. Offsets are relative to the start of each frame. The Date value is fixed here to make the example reproducible; a running server uses the current time.

## Request HEADERS frame

51 bytes total: 8-byte fixed header and a 43-byte compressed-header payload.

```text
00000000  00 00 2b 01 01 00 00 01  01 00 03 47 45 54 02 00
00000010  0a 2f 70 72 6f 6f 66 2e  74 78 74 06 00 09 6c 6f
00000020  63 61 6c 68 6f 73 74 07  00 05 62 63 75 72 6c 05
00000030  00 01 30
```

Bytes 0-2: payload length `0x2b` (43). Byte 3: type `0x01` HEADERS. Byte 4: flags `0x01` END_STREAM (no request body). Bytes 5-7: request ID 1. The fixed header ends at byte 7; the compressed header block starts at byte 8.

| Field | Index byte | Value-length bytes | Value bytes |
|---|---:|---:|---|
| `:method` | 8 (`0x01`) | 9-10 = 3 | 11-13 = `GET` |
| `:path` | 14 (`0x02`) | 15-16 = 10 | 17-26 = `/proof.txt` |
| `host` | 27 (`0x06`) | 28-29 = 9 | 30-38 = `localhost` |
| `user-agent` | 39 (`0x07`) | 40-41 = 5 | 42-46 = `bcurl` |
| `content-length` | 47 (`0x05`) | 48-49 = 1 | 50 = `0` |

The numbered header fields follow the static name mapping in `PROTOCOL.md`; there is no request DATA frame.

## Response HEADERS frame

85 bytes total: 8-byte fixed header and a 77-byte compressed-header payload.

```text
00000000  00 00 4d 01 00 00 00 01  03 00 03 32 30 30 05 00
00000010  01 33 04 00 0a 74 65 78  74 2f 70 6c 61 69 6e 09
00000020  00 1d 4d 6f 6e 2c 20 30  31 20 4a 61 6e 20 32 30
00000030  32 34 20 30 30 3a 30 30  3a 30 30 20 47 4d 54 08
00000040  00 06 62 73 65 72 76 65  0a 00 0a 6b 65 65 70 2d
00000050  61 6c 69 76 65
```

Bytes 0-2 declare payload length 77; byte 3 HEADERS; byte 4 flags 0 (DATA follows); bytes 5-7 request ID 1. Starting at byte 8, the indexed fields are `:status` index 3 = `200`, `content-length` index 5 = `3`, `content-type` index 4 = `text/plain`, `date` index 9 = `Mon, 01 Jan 2024 00:00:00 GMT`, `server` index 8 = `bserve`, and `connection` index 10 = `keep-alive`. Every value is preceded by its 16-bit big-endian byte length.

## Response DATA frame

```text
00000000  00 00 03 02 01 00 00 01  4f 4b 0a
```

Bytes 0-2: payload length 3. Byte 3: DATA (`0x02`). Byte 4: END_STREAM (`0x01`). Bytes 5-7: request ID 1. The raw payload begins at byte 8: `4f 4b 0a` = `OK\n`.
