#include "hexdump.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void hexdump(FILE *fp, const void *data, size_t len) {
    const uint8_t *p = data;
    for (size_t off = 0; off < len; off += 16) {
        fprintf(fp, "%08zx ", off);
        size_t n = len - off;
        if (n > 16) {
            n = 16;
        }
        for (size_t i = 0; i < 16; i++) {
            if (i == 8) {
                fputc(' ', fp);
            }
            if (i < n) {
                fprintf(fp, " %02x", p[off + i]);
            } else {
                fputs("   ", fp);
            }
        }
        fputs("  |", fp);
        for (size_t i = 0; i < n; i++) {
            unsigned char c = p[off + i];
            fputc(isprint(c) ? c : '.', fp);
        }
        fputs("|\n", fp);
    }
    if (len == 0) {
        fprintf(fp, "%08zx\n", (size_t)0);
    }
}

void hexdump_labeled(FILE *fp, const char *label, const void *data, size_t len) {
    fprintf(fp, "%s (%zu bytes)\n", label ? label : "dump", len);
    hexdump(fp, data, len);
}

void hexdump_frame(FILE *fp, const char *direction, const FrameHeader *hdr,
                   const void *payload) {
    uint8_t raw[FRAME_HEADER_SIZE];
    if (frame_header_encode(hdr, raw) != 0) {
        fprintf(fp, "%s: invalid frame header (24-bit overflow)\n", direction);
        return;
    }
    size_t total = FRAME_HEADER_SIZE + (size_t)hdr->length;
    uint8_t *blob = malloc(total == 0 ? 1 : total);
    if (!blob) {
        return;
    }
    memcpy(blob, raw, FRAME_HEADER_SIZE);
    if (hdr->length > 0 && payload) {
        memcpy(blob + FRAME_HEADER_SIZE, payload, hdr->length);
    }
    fprintf(fp, "%s frame type=0x%02x flags=0x%02x id=%u length=%u\n",
            direction ? direction : "frame",
            hdr->type, hdr->flags, hdr->request_id, hdr->length);
    hexdump(fp, blob, total);
    free(blob);
}