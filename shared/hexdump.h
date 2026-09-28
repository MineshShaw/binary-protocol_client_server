#ifndef HEXDUMP_H
#define HEXDUMP_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#include "frame.h"

/*
 * Canonical 16-byte hexdump:
 *   00000000  48 65 6c 6c 6f 20 77 6f  72 6c 64 21 00 00 00 00  |Hello world!....|
 *
 * bserve always dumps incoming frames; bcurl uses the detailed frame dump in
 * verbose mode. The raw hexdump helper is also used for received entity data.
 */

void hexdump(FILE *fp, const void *data, size_t len);
void hexdump_labeled(FILE *fp, const char *label, const void *data, size_t len);

/* Dump header bytes plus payload as a single frame blob. */
void hexdump_frame(FILE *fp, const char *direction, const FrameHeader *hdr,
                   const void *payload);

/*
 * Dump a complete frame and annotate its fixed header and payload structure.
 * HEADERS payloads include byte ranges for each compressed field.
 */
void hexdump_frame_annotated(FILE *fp, const char *direction,
                             const FrameHeader *hdr, const void *payload);

#endif /* HEXDUMP_H */
