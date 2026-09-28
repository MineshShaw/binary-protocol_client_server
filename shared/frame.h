#ifndef FRAME_H
#define FRAME_H

#include <stddef.h>
#include <stdint.h>

/*
 * The 8-Byte Protocol — on-wire frame header (network / big-endian byte order).
 *
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                      Length (24)              |    Type (8)   |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |    Flags (8)  |                 Request ID (24)               |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * Length and Request ID are 24-bit unsigned integers. They are stored in
 * host-side structs as uint32_t, but only the low 24 bits are valid on the
 * wire. Encoding uses shifts and 0xFF masks so each octet is isolated:
 *
 *   high byte  = (value >> 16) & 0xFF   bits 23..16
 *   mid  byte  = (value >>  8) & 0xFF   bits 15..8
 *   low  byte  =  value        & 0xFF   bits  7..0
 *
 * Decoding reverses the process with left-shifts and bitwise OR:
 *
 *   value = ((b0 << 16) | (b1 << 8) | b2) & 0x00FFFFFFu
 *
 * The 0x00FFFFFF mask guarantees bits 31..24 stay zero even if a uint32_t
 * is wider than the field.
 */

#define FRAME_HEADER_SIZE 8u

#define FRAME_TYPE_HEADERS 0x01u
#define FRAME_TYPE_DATA    0x02u
#define FRAME_TYPE_ERROR   0x03u

#define FRAME_FLAG_END_STREAM 0x01u

/* 24-bit field maxima: 2^24 - 1 = 16,777,215. */
#define FRAME_U24_MAX 0x00FFFFFFu

typedef struct {
    uint32_t length;     /* payload size in bytes; only bits 23..0 are on the wire */
    uint8_t  type;       /* 0x01 HEADERS, 0x02 DATA, 0x03 ERROR; others must be skipped */
    uint8_t  flags;      /* bit 0 = END_STREAM */
    uint32_t request_id; /* correlates request/response; only bits 23..0 are on the wire */
} FrameHeader;

/* Returns 1 if type is HEADERS, DATA, or ERROR. */
int frame_type_known(uint8_t type);

/*
 * Pack hdr into exactly 8 bytes. Returns 0 on success, -1 if length or
 * request_id exceeds 24 bits (i.e. value > FRAME_U24_MAX).
 */
int frame_header_encode(const FrameHeader *hdr, uint8_t out[FRAME_HEADER_SIZE]);

/* Unpack 8 wire bytes into hdr. Always succeeds for any 8-byte input. */
void frame_header_decode(const uint8_t in[FRAME_HEADER_SIZE], FrameHeader *hdr);

/* Blocking helpers: read/write exactly n bytes. Return 0 on success, -1 on EOF/error. */
int frame_read_exact(int fd, void *buf, size_t n);
int frame_write_exact(int fd, const void *buf, size_t n);

int frame_header_read(int fd, FrameHeader *hdr);
int frame_header_write(int fd, const FrameHeader *hdr);

/*
 * Consume exactly `length` payload bytes and discard them. This is the
 * required behaviour when Type is unrecognized: do not close the connection;
 * advance to the next frame.
 */
int frame_skip_payload(int fd, uint32_t length);

/* Encode header + payload and write both. payload may be NULL if length == 0. */
int frame_write_full(int fd, const FrameHeader *hdr, const void *payload);

#endif /* FRAME_H */
