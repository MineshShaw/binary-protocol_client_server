#ifndef HPACK_LITE_H
#define HPACK_LITE_H

#include <stddef.h>
#include <stdint.h>

/*
 * HPACK-lite — static-table + literal-name header block encoding.
 *
 * Dictionary header (index 1..10):
 *   [1 byte index] [2 byte value length, big-endian] [value bytes]
 *
 * Dynamic / literal header (index 0x00):
 *   [0x00] [1 byte name length] [name bytes]
 *          [2 byte value length, big-endian] [value bytes]
 *
 * The 2-byte length is packed the same way as the 16-bit half of a 24-bit
 * field: high = (len >> 8) & 0xFF, low = len & 0xFF.
 */

#define HPACK_INDEX_DYNAMIC     0x00u
#define HPACK_STATIC_TABLE_SIZE 10u
#define HPACK_VALUE_LEN_MAX     0xFFFFu
#define HPACK_NAME_LEN_MAX      0xFFu

enum {
    HPACK_IDX_METHOD = 1,
    HPACK_IDX_PATH = 2,
    HPACK_IDX_STATUS = 3,
    HPACK_IDX_CONTENT_TYPE = 4,
    HPACK_IDX_CONTENT_LENGTH = 5,
    HPACK_IDX_HOST = 6,
    HPACK_IDX_USER_AGENT = 7,
    HPACK_IDX_SERVER = 8,
    HPACK_IDX_DATE = 9,
    HPACK_IDX_CONNECTION = 10
};

typedef struct {
    char *name;
    char *value;
} HpackHeader;

typedef struct {
    HpackHeader *items;
    size_t count;
    size_t capacity;
} HpackHeaderList;

/* Static table name for index 1..10, or NULL if out of range. */
const char *hpack_static_name(uint8_t index);

/* Reverse lookup: 1..10 if name matches the static table, else 0 (dynamic). */
uint8_t hpack_static_index(const char *name);

void hpack_list_init(HpackHeaderList *list);
void hpack_list_free(HpackHeaderList *list);

/* Copies name and value. Returns 0 on success, -1 on OOM. */
int hpack_list_add(HpackHeaderList *list, const char *name, const char *value);

/* First matching value, or NULL. */
const char *hpack_list_get(const HpackHeaderList *list, const char *name);

/*
 * Encode list into a newly malloc'd buffer. *out_len is set to the payload
 * size. Returns 0 on success, -1 on error. Caller frees *out.
 */
int hpack_encode(const HpackHeaderList *list, uint8_t **out, uint32_t *out_len);

/*
 * Decode a HEADERS payload. Returns 0 on success, -1 if the block is
 * truncated or otherwise malformed.
 */
int hpack_decode(const uint8_t *buf, uint32_t len, HpackHeaderList *list);

#endif /* HPACK_LITE_H */
