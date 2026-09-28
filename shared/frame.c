#include "frame.h"

#include <errno.h>
#include <unistd.h>

int frame_type_known(uint8_t type) {
    return type == FRAME_TYPE_HEADERS
        || type == FRAME_TYPE_DATA
        || type == FRAME_TYPE_ERROR;
}

int frame_header_encode(const FrameHeader *hdr, uint8_t out[FRAME_HEADER_SIZE]) {
    if (!hdr || !out) {
        return -1;
    }
    /* Reject values that would not fit in 24 bits. Bits 31..24 must be zero. */
    if ((hdr->length & ~FRAME_U24_MAX) != 0
        || (hdr->request_id & ~FRAME_U24_MAX) != 0) {
        return -1;
    }

    uint32_t length = hdr->length & FRAME_U24_MAX;
    uint32_t reqid  = hdr->request_id & FRAME_U24_MAX;

    /*
     * Length occupies bytes 0..2, most-significant octet first.
     * Example: length = 0x000ABCDE
     *   out[0] = 0xAB = (length >> 16) & 0xFF
     *   out[1] = 0xCD = (length >>  8) & 0xFF
     *   out[2] = 0xDE =  length        & 0xFF
     */
    out[0] = (uint8_t)((length >> 16) & 0xFFu);
    out[1] = (uint8_t)((length >>  8) & 0xFFu);
    out[2] = (uint8_t)( length        & 0xFFu);

    out[3] = hdr->type;
    out[4] = hdr->flags;

    /* Request ID occupies bytes 5..7, same 24-bit big-endian packing. */
    out[5] = (uint8_t)((reqid >> 16) & 0xFFu);
    out[6] = (uint8_t)((reqid >>  8) & 0xFFu);
    out[7] = (uint8_t)( reqid        & 0xFFu);

    return 0;
}

void frame_header_decode(const uint8_t in[FRAME_HEADER_SIZE], FrameHeader *hdr) {
    /*
     * Rebuild the 24-bit integer by placing each octet in its bit slot:
     *   bits 23..16 <- in[0] shifted left 16
     *   bits 15..8  <- in[1] shifted left  8
     *   bits  7..0  <- in[2]
     * OR combines the non-overlapping fields; the final mask zeros bits 31..24.
     */
    hdr->length = ((uint32_t)in[0] << 16)
                | ((uint32_t)in[1] <<  8)
                |  (uint32_t)in[2];
    hdr->length &= FRAME_U24_MAX;

    hdr->type  = in[3];
    hdr->flags = in[4];

    hdr->request_id = ((uint32_t)in[5] << 16)
                    | ((uint32_t)in[6] <<  8)
                    |  (uint32_t)in[7];
    hdr->request_id &= FRAME_U24_MAX;
}

int frame_read_exact(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0) {
            return -1; /* EOF */
        }
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

int frame_write_exact(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    size_t put = 0;
    while (put < n) {
        ssize_t w = write(fd, p + put, n - put);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (w == 0) {
            return -1;
        }
        put += (size_t)w;
    }
    return 0;
}

int frame_header_read(int fd, FrameHeader *hdr) {
    uint8_t raw[FRAME_HEADER_SIZE];
    if (frame_read_exact(fd, raw, FRAME_HEADER_SIZE) != 0) {
        return -1;
    }
    frame_header_decode(raw, hdr);
    return 0;
}

int frame_header_write(int fd, const FrameHeader *hdr) {
    uint8_t raw[FRAME_HEADER_SIZE];
    if (frame_header_encode(hdr, raw) != 0) {
        return -1;
    }
    return frame_write_exact(fd, raw, FRAME_HEADER_SIZE);
}

int frame_skip_payload(int fd, uint32_t length) {
    uint8_t sink[4096];
    uint32_t remaining = length;
    while (remaining > 0) {
        uint32_t chunk = remaining < sizeof(sink) ? remaining : (uint32_t)sizeof(sink);
        if (frame_read_exact(fd, sink, chunk) != 0) {
            return -1;
        }
        remaining -= chunk;
    }
    return 0;
}

int frame_write_full(int fd, const FrameHeader *hdr, const void *payload) {
    if (frame_header_write(fd, hdr) != 0) {
        return -1;
    }
    if (hdr->length == 0) {
        return 0;
    }
    if (!payload) {
        return -1;
    }
    return frame_write_exact(fd, payload, hdr->length);
}
