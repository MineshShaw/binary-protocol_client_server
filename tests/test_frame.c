#include "frame.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fail(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

int main(void) {
    FrameHeader hdr = {
        .length = 0x00ABCDEF,
        .type = FRAME_TYPE_HEADERS,
        .flags = FRAME_FLAG_END_STREAM,
        .request_id = 0x00012345
    };

    uint8_t raw[FRAME_HEADER_SIZE];
    if (frame_header_encode(&hdr, raw) != 0) {
        return fail("encode");
    }

    /* Length 0xABCDEF -> AB CD EF in the first three bytes. */
    if (raw[0] != 0xAB || raw[1] != 0xCD || raw[2] != 0xEF) {
        return fail("length packing");
    }
    if (raw[3] != FRAME_TYPE_HEADERS || raw[4] != FRAME_FLAG_END_STREAM) {
        return fail("type/flags");
    }
    if (raw[5] != 0x01 || raw[6] != 0x23 || raw[7] != 0x45) {
        return fail("request id packing");
    }

    FrameHeader back;
    frame_header_decode(raw, &back);
    if (back.length != hdr.length || back.type != hdr.type
        || back.flags != hdr.flags || back.request_id != hdr.request_id) {
        return fail("round trip");
    }

    FrameHeader too_wide = hdr;
    too_wide.length = 0x01000000; /* bit 24 set: must be rejected */
    if (frame_header_encode(&too_wide, raw) == 0) {
        return fail("24-bit overflow not rejected");
    }

    /* Skip logic: write payload on a pipe, skip it, then read a sentinel. */
    int fds[2];
    if (pipe(fds) != 0) {
        return fail("pipe");
    }
    uint8_t payload[] = {0x11, 0x22, 0x33, 0x44, 0xAA};
    if (write(fds[1], payload, sizeof(payload)) != (ssize_t)sizeof(payload)) {
        return fail("pipe write");
    }
    if (frame_skip_payload(fds[0], 4) != 0) {
        return fail("skip");
    }
    uint8_t last = 0;
    if (read(fds[0], &last, 1) != 1 || last != 0xAA) {
        return fail("skip left extra bytes");
    }
    close(fds[0]);
    close(fds[1]);

    if (frame_type_known(0x99) || !frame_type_known(FRAME_TYPE_DATA)) {
        return fail("type known");
    }

    printf("test_frame: ok\n");
    return 0;
}
