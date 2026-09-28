#include "hpack_lite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

int main(void) {
    if (strcmp(hpack_static_name(1), ":method") != 0) {
        return fail("static :method");
    }
    if (hpack_static_index("content-type") != 4) {
        return fail("index content-type");
    }
    if (hpack_static_index("x-custom") != 0) {
        return fail("dynamic name should be index 0");
    }

    HpackHeaderList list;
    hpack_list_init(&list);
    if (hpack_list_add(&list, ":method", "GET") != 0
        || hpack_list_add(&list, ":path", "/index.html") != 0
        || hpack_list_add(&list, "x-trace", "abc") != 0) {
        return fail("list add");
    }

    uint8_t *enc = NULL;
    uint32_t elen = 0;
    if (hpack_encode(&list, &enc, &elen) != 0) {
        return fail("encode");
    }

    /* First field: index 1, value length 3, "GET" */
    if (elen < 6 || enc[0] != 1 || enc[1] != 0x00 || enc[2] != 0x03) {
        return fail("dictionary encoding prefix");
    }
    if (memcmp(enc + 3, "GET", 3) != 0) {
        return fail("GET value");
    }

    HpackHeaderList decoded;
    if (hpack_decode(enc, elen, &decoded) != 0) {
        return fail("decode");
    }
    const char *m = hpack_list_get(&decoded, ":method");
    const char *p = hpack_list_get(&decoded, ":path");
    const char *x = hpack_list_get(&decoded, "x-trace");
    if (!m || strcmp(m, "GET") != 0) {
        return fail(":method round trip");
    }
    if (!p || strcmp(p, "/index.html") != 0) {
        return fail(":path round trip");
    }
    if (!x || strcmp(x, "abc") != 0) {
        return fail("dynamic header round trip");
    }

    /* Truncated block must fail. */
    HpackHeaderList bad;
    if (hpack_decode(enc, 2, &bad) == 0) {
        return fail("truncated decode should fail");
    }

    hpack_list_free(&list);
    hpack_list_free(&decoded);
    free(enc);

    printf("test_hpack: ok\n");
    return 0;
}
