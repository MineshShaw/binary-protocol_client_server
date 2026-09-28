#include "hexdump.h"
#include "hpack_lite.h"

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

static void print_quoted(FILE *fp, const uint8_t *bytes, size_t length) {
    fputc('"', fp);
    for (size_t i = 0; i < length; i++) {
        unsigned char c = bytes[i];
        if (c == '\\' || c == '"') {
            fputc('\\', fp);
            fputc(c, fp);
        } else if (isprint(c)) {
            fputc(c, fp);
        } else {
            fprintf(fp, "\\x%02x", c);
        }
    }
    fputc('"', fp);
}

static void annotate_headers(FILE *fp, const uint8_t *payload, uint32_t length) {
    HpackHeaderList decoded;
    if (hpack_decode(payload, length, &decoded) != 0) {
        fprintf(fp, "  malformed compressed header block (%u payload bytes)\n", length);
        return;
    }
    hpack_list_free(&decoded);

    uint32_t offset = 0;
    while (offset < length) {
        uint32_t field_start = offset;
        uint8_t index = payload[offset++];
        const uint8_t *name_bytes = NULL;
        size_t name_length = 0;
        const char *static_name = NULL;
        uint32_t name_length_offset = 0;
        uint32_t name_start = 0;
        if (index == HPACK_INDEX_DYNAMIC) {
            if (offset >= length) {
                break;
            }
            name_length_offset = offset;
            name_length = payload[offset++];
            if (name_length == 0 || name_length > length - offset) {
                break;
            }
            name_start = offset;
            name_bytes = payload + offset;
            offset += (uint32_t)name_length;
        } else {
            static_name = hpack_static_name(index);
            if (!static_name) {
                break;
            }
            name_bytes = (const uint8_t *)static_name;
            name_length = strlen(static_name);
        }

        if (length - offset < 2) {
            break;
        }
        uint32_t value_length_offset = offset;
        uint32_t value_length =
            ((uint32_t)payload[offset] << 8) | (uint32_t)payload[offset + 1];
        offset += 2;
        if (value_length > length - offset) {
            break;
        }
        uint32_t value_start = offset;

        fprintf(fp, "  field starts at frame byte %u; index byte %u = 0x%02x ",
                field_start + FRAME_HEADER_SIZE, field_start + FRAME_HEADER_SIZE,
                index);
        if (static_name) {
            fprintf(fp, "(static name ");
            print_quoted(fp, name_bytes, name_length);
            fprintf(fp, ")");
        } else {
            fprintf(fp, "; name-length byte %u = %zu; name bytes [%u..%u] = ",
                    name_length_offset + FRAME_HEADER_SIZE, name_length,
                    name_start + FRAME_HEADER_SIZE,
                    name_start + (uint32_t)name_length - 1 + FRAME_HEADER_SIZE);
            print_quoted(fp, name_bytes, name_length);
        }
        fprintf(fp, "; value-length bytes [%u..%u] = %u",
                value_length_offset + FRAME_HEADER_SIZE,
                value_length_offset + 1 + FRAME_HEADER_SIZE, value_length);
        if (value_length > 0) {
            fprintf(fp, "; value bytes [%u..%u] = ",
                    value_start + FRAME_HEADER_SIZE,
                    value_start + value_length - 1 + FRAME_HEADER_SIZE);
        } else {
            fprintf(fp, "; value is empty");
        }
        print_quoted(fp, payload + offset, value_length);
        fputc('\n', fp);
        offset += value_length;
    }
}

void hexdump_frame_annotated(FILE *fp, const char *direction,
                             const FrameHeader *hdr, const void *payload) {
    uint8_t raw[FRAME_HEADER_SIZE];
    if (!fp || frame_header_encode(hdr, raw) != 0) {
        if (fp) {
            fprintf(fp, "%s: invalid frame header\n",
                    direction ? direction : "frame");
        }
        return;
    }
    if (hdr->length > 0 && !payload) {
        fprintf(fp, "%s: missing frame payload\n",
                direction ? direction : "frame");
        return;
    }

    size_t total = FRAME_HEADER_SIZE + (size_t)hdr->length;
    uint8_t *blob = malloc(total);
    if (!blob) {
        fprintf(fp, "%s: unable to allocate frame dump\n",
                direction ? direction : "frame");
        return;
    }
    memcpy(blob, raw, FRAME_HEADER_SIZE);
    if (hdr->length > 0) {
        memcpy(blob + FRAME_HEADER_SIZE, payload, hdr->length);
    }

    fprintf(fp, "%s complete frame (%zu bytes)\n",
            direction ? direction : "frame", total);
    hexdump(fp, blob, total);
    fprintf(fp, "  parsing boundary: fixed header [0..7], payload begins at byte 8\n");
    fprintf(fp, "  frame bytes [0..2]: payload length = %u\n", hdr->length);
    fprintf(fp, "  frame byte [3]: type = 0x%02x", hdr->type);
    if (hdr->type == FRAME_TYPE_HEADERS) {
        fprintf(fp, " (HEADERS)\n");
    } else if (hdr->type == FRAME_TYPE_DATA) {
        fprintf(fp, " (DATA)\n");
    } else if (hdr->type == FRAME_TYPE_ERROR) {
        fprintf(fp, " (ERROR)\n");
    } else {
        if (hdr->length > 0) {
            fprintf(fp, " (unknown; skip frame bytes [8..%zu])\n", total - 1);
        } else {
            fprintf(fp, " (unknown; empty payload to skip)\n");
        }
    }
    fprintf(fp, "  frame byte [4]: flags = 0x%02x%s\n", hdr->flags,
            (hdr->flags & FRAME_FLAG_END_STREAM) ? " (END_STREAM set)" : "");
    fprintf(fp, "  frame bytes [5..7]: request ID = %u\n", hdr->request_id);

    if (hdr->type == FRAME_TYPE_HEADERS) {
        fprintf(fp, "  compressed header block starts at frame byte 8\n");
        annotate_headers(fp, payload, hdr->length);
    } else if (hdr->type == FRAME_TYPE_DATA || hdr->type == FRAME_TYPE_ERROR) {
        fprintf(fp, "  raw payload starts at frame byte 8 (length=%u)\n",
                hdr->length);
    }
    fputc('\n', fp);
    free(blob);
}