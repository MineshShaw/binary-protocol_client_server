#include "frame.h"
#include "hexdump.h"

#include <stdio.h>
#include <stdlib.h>
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

    const uint8_t headers_payload[] = {1, 0, 3, 'G', 'E', 'T'};
    FrameHeader headers_frame = {
        .length = sizeof(headers_payload),
        .type = FRAME_TYPE_HEADERS,
        .flags = FRAME_FLAG_END_STREAM,
        .request_id = 1
    };
    FILE *dump = tmpfile();
    if (!dump) {
        return fail("tmpfile for annotated hexdump");
    }
    hexdump_frame_annotated(dump, "test", &headers_frame, headers_payload);
    if (fflush(dump) != 0 || fseek(dump, 0, SEEK_SET) != 0) {
        fclose(dump);
        return fail("rewind annotated hexdump");
    }
    char annotation[2048];
    size_t annotation_len = fread(annotation, 1, sizeof(annotation) - 1, dump);
    annotation[annotation_len] = '\0';
    fclose(dump);
    if (!strstr(annotation, "fixed header [0..7], payload begins at byte 8")
        || !strstr(annotation, "index byte 8 = 0x01")
        || !strstr(annotation, "value-length bytes [9..10] = 3")
        || !strstr(annotation, "value bytes [11..13] = \"GET\"")) {
        return fail("annotated hexdump byte ranges");
    }

    printf("test_frame: ok\n");
    return 0;
}
