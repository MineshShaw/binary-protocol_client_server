#include "hpack_lite.h"

#include <stdlib.h>
#include <string.h>

static const char *const k_static_table[HPACK_STATIC_TABLE_SIZE + 1] = {
    NULL, /* index 0 is the dynamic/literal marker, not a name */
    ":method",
    ":path",
    ":status",
    "content-type",
    "content-length",
    "host",
    "accept",
    "user-agent",
    "connection",
    "server"
};

const char *hpack_static_name(uint8_t index) {
    if (index == 0 || index > HPACK_STATIC_TABLE_SIZE) {
        return NULL;
    }
    return k_static_table[index];
}

uint8_t hpack_static_index(const char *name) {
    if (!name) {
        return HPACK_INDEX_DYNAMIC;
    }
    for (uint8_t i = 1; i <= HPACK_STATIC_TABLE_SIZE; i++) {
        if (strcmp(k_static_table[i], name) == 0) {
            return i;
        }
    }
    return HPACK_INDEX_DYNAMIC;
}

void hpack_list_init(HpackHeaderList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void hpack_list_free(HpackHeaderList *list) {
    if (!list) {
        return;
    }
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].name);
        free(list->items[i].value);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

int hpack_list_add(HpackHeaderList *list, const char *name, const char *value) {
    if (!list || !name || !value) {
        return -1;
    }
    if (list->count == list->capacity) {
        size_t cap = list->capacity == 0 ? 8 : list->capacity * 2;
        HpackHeader *grown = realloc(list->items, cap * sizeof(*grown));
        if (!grown) {
            return -1;
        }
        list->items = grown;
        list->capacity = cap;
    }
    char *n = strdup(name);
    char *v = strdup(value);
    if (!n || !v) {
        free(n);
        free(v);
        return -1;
    }
    list->items[list->count].name = n;
    list->items[list->count].value = v;
    list->count++;
    return 0;
}

const char *hpack_list_get(const HpackHeaderList *list, const char *name) {
    if (!list || !name) {
        return NULL;
    }
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].name, name) == 0) {
            return list->items[i].value;
        }
    }
    return NULL;
}

static int buf_append(uint8_t **buf, uint32_t *len, uint32_t *cap,
                      const void *src, uint32_t n) {
    if (*len + n < *len) {
        return -1; /* overflow */
    }
    if (*len + n > *cap) {
        uint32_t ncap = *cap == 0 ? 64 : *cap;
        while (ncap < *len + n) {
            if (ncap > (1u << 30)) {
                return -1;
            }
            ncap *= 2;
        }
        uint8_t *grown = realloc(*buf, ncap);
        if (!grown) {
            return -1;
        }
        *buf = grown;
        *cap = ncap;
    }
    memcpy(*buf + *len, src, n);
    *len += n;
    return 0;
}

int hpack_encode(const HpackHeaderList *list, uint8_t **out, uint32_t *out_len) {
    if (!list || !out || !out_len) {
        return -1;
    }
    uint8_t *buf = NULL;
    uint32_t len = 0;
    uint32_t cap = 0;

    for (size_t i = 0; i < list->count; i++) {
        const char *name = list->items[i].name;
        const char *value = list->items[i].value;
        size_t vlen = strlen(value);
        if (vlen > HPACK_VALUE_LEN_MAX) {
            free(buf);
            return -1;
        }

        uint8_t index = hpack_static_index(name);
        if (buf_append(&buf, &len, &cap, &index, 1) != 0) {
            free(buf);
            return -1;
        }

        if (index == HPACK_INDEX_DYNAMIC) {
            size_t nlen = strlen(name);
            if (nlen > HPACK_NAME_LEN_MAX) {
                free(buf);
                return -1;
            }
            uint8_t nlen8 = (uint8_t)nlen;
            if (buf_append(&buf, &len, &cap, &nlen8, 1) != 0
                || buf_append(&buf, &len, &cap, name, (uint32_t)nlen) != 0) {
                free(buf);
                return -1;
            }
        }

        /* 16-bit big-endian value length: high octet then low octet. */
        uint16_t v16 = (uint16_t)vlen;
        uint8_t vbytes[2];
        vbytes[0] = (uint8_t)((v16 >> 8) & 0xFFu);
        vbytes[1] = (uint8_t)( v16       & 0xFFu);
        if (buf_append(&buf, &len, &cap, vbytes, 2) != 0
            || buf_append(&buf, &len, &cap, value, (uint32_t)vlen) != 0) {
            free(buf);
            return -1;
        }
    }

    *out = buf ? buf : malloc(1);
    if (!*out) {
        return -1;
    }
    *out_len = len;
    return 0;
}

static int read_bytes(const uint8_t *buf, uint32_t len, uint32_t *off,
                      void *dst, uint32_t n) {
    if (*off + n < *off || *off + n > len) {
        return -1;
    }
    memcpy(dst, buf + *off, n);
    *off += n;
    return 0;
}

int hpack_decode(const uint8_t *buf, uint32_t len, HpackHeaderList *list) {
    if (!buf || !list) {
        return -1;
    }
    hpack_list_init(list);
    uint32_t off = 0;
    while (off < len) {
        uint8_t index;
        if (read_bytes(buf, len, &off, &index, 1) != 0) {
            hpack_list_free(list);
            return -1;
        }

        char *name = NULL;
        if (index == HPACK_INDEX_DYNAMIC) {
            uint8_t nlen;
            if (read_bytes(buf, len, &off, &nlen, 1) != 0) {
                hpack_list_free(list);
                return -1;
            }
            name = malloc((size_t)nlen + 1);
            if (!name || read_bytes(buf, len, &off, name, nlen) != 0) {
                free(name);
                hpack_list_free(list);
                return -1;
            }
            name[nlen] = '\0';
        } else {
            const char *static_name = hpack_static_name(index);
            if (!static_name) {
                hpack_list_free(list);
                return -1;
            }
            name = strdup(static_name);
            if (!name) {
                hpack_list_free(list);
                return -1;
            }
        }

        uint8_t vbytes[2];
        if (read_bytes(buf, len, &off, vbytes, 2) != 0) {
            free(name);
            hpack_list_free(list);
            return -1;
        }
        /* Inverse of the 16-bit pack: (high << 8) | low. */
        uint32_t vlen = ((uint32_t)vbytes[0] << 8) | (uint32_t)vbytes[1];
        char *value = malloc((size_t)vlen + 1);
        if (!value || read_bytes(buf, len, &off, value, vlen) != 0) {
            free(name);
            free(value);
            hpack_list_free(list);
            return -1;
        }
        value[vlen] = '\0';

        int rc = hpack_list_add(list, name, value);
        free(name);
        free(value);
        if (rc != 0) {
            hpack_list_free(list);
            return -1;
        }
    }
    return 0;
}
