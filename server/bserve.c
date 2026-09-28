#include "frame.h"
#include "hexdump.h"
#include "hpack_lite.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_verbose = 0;

static void usage(const char *argv0) {
    fprintf(stderr, "Usage: %s [-v] <root_directory> <port>\n", argv0);
}

static int path_is_safe(const char *rel) {
    if (!rel || rel[0] == '\0') {
        return 0;
    }
    if (strstr(rel, "..") != NULL) {
        return 0;
    }
    return 1;
}

static char *join_root(const char *root, const char *url_path) {
    const char *rel = url_path;
    if (rel[0] == '/') {
        rel++;
    }
    if (rel[0] == '\0') {
        rel = "index.html";
    }
    if (!path_is_safe(rel)) {
        return NULL;
    }
    size_t n = strlen(root) + 1 + strlen(rel) + 1;
    char *out = malloc(n);
    if (!out) {
        return NULL;
    }
    snprintf(out, n, "%s/%s", root, rel);
    return out;
}

static int send_status(int fd, uint32_t request_id, const char *status,
                       const char *extra_name, const char *extra_value,
                       uint8_t flags, int verbose) {
    HpackHeaderList hdrs;
    hpack_list_init(&hdrs);
    if (hpack_list_add(&hdrs, ":status", status) != 0) {
        hpack_list_free(&hdrs);
        return -1;
    }
    if (hpack_list_add(&hdrs, "server", "bserve") != 0) {
        hpack_list_free(&hdrs);
        return -1;
    }
    if (extra_name && extra_value) {
        if (hpack_list_add(&hdrs, extra_name, extra_value) != 0) {
            hpack_list_free(&hdrs);
            return -1;
        }
    }
    uint8_t *payload = NULL;
    uint32_t plen = 0;
    if (hpack_encode(&hdrs, &payload, &plen) != 0) {
        hpack_list_free(&hdrs);
        return -1;
    }
    hpack_list_free(&hdrs);

    FrameHeader fh = {
        .length = plen,
        .type = FRAME_TYPE_HEADERS,
        .flags = flags,
        .request_id = request_id
    };
    if (verbose) {
        hexdump_frame(stderr, "outgoing", &fh, payload);
    }
    int rc = frame_write_full(fd, &fh, payload);
    free(payload);
    return rc;
}

static int send_data(int fd, uint32_t request_id, const uint8_t *data,
                     uint32_t len, uint8_t flags, int verbose) {
    FrameHeader fh = {
        .length = len,
        .type = FRAME_TYPE_DATA,
        .flags = flags,
        .request_id = request_id
    };
    if (verbose) {
        hexdump_frame(stderr, "outgoing", &fh, data);
    }
    return frame_write_full(fd, &fh, data);
}

static int slurp_file(const char *path, uint8_t **out, uint32_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    long sz = ftell(fp);
    if (sz < 0 || (unsigned long)sz > FRAME_U24_MAX) {
        fclose(fp);
        return -1;
    }
    rewind(fp);
    uint8_t *buf = malloc(sz == 0 ? 1 : (size_t)sz);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    *out = buf;
    *out_len = (uint32_t)sz;
    return 0;
}

static int handle_headers(int fd, const FrameHeader *hdr, const uint8_t *payload,
                          const char *root) {
    HpackHeaderList list;
    if (hpack_decode(payload, hdr->length, &list) != 0) {
        return send_status(fd, hdr->request_id, "400", NULL, NULL,
                           FRAME_FLAG_END_STREAM, g_verbose);
    }

    const char *path = hpack_list_get(&list, ":path");
    if (!path) {
        hpack_list_free(&list);
        return send_status(fd, hdr->request_id, "400", NULL, NULL,
                           FRAME_FLAG_END_STREAM, g_verbose);
    }

    char *full = join_root(root, path);
    hpack_list_free(&list);
    if (!full) {
        return send_status(fd, hdr->request_id, "400", NULL, NULL,
                           FRAME_FLAG_END_STREAM, g_verbose);
    }

    struct stat st;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
        free(full);
        return send_status(fd, hdr->request_id, "404", NULL, NULL,
                           FRAME_FLAG_END_STREAM, g_verbose);
    }

    uint8_t *body = NULL;
    uint32_t blen = 0;
    if (slurp_file(full, &body, &blen) != 0) {
        free(full);
        return send_status(fd, hdr->request_id, "404", NULL, NULL,
                           FRAME_FLAG_END_STREAM, g_verbose);
    }
    free(full);

    char clen[32];
    snprintf(clen, sizeof(clen), "%u", blen);
    if (send_status(fd, hdr->request_id, "200", "content-length", clen,
                    0, g_verbose) != 0) {
        free(body);
        return -1;
    }
    int rc = send_data(fd, hdr->request_id, body, blen, FRAME_FLAG_END_STREAM,
                       g_verbose);
    free(body);
    return rc;
}

static void serve_connection(int fd, const char *root) {
    for (;;) {
        FrameHeader hdr;
        if (frame_header_read(fd, &hdr) != 0) {
            return;
        }

        uint8_t *payload = NULL;
        if (hdr.length > 0) {
            payload = malloc(hdr.length);
            if (!payload) {
                return;
            }
            if (frame_read_exact(fd, payload, hdr.length) != 0) {
                free(payload);
                return;
            }
        }

        if (g_verbose) {
            hexdump_frame(stderr, "incoming", &hdr, payload);
        }

        if (!frame_type_known(hdr.type)) {
            /* Unknown type: payload already consumed; continue to next frame. */
            if (g_verbose) {
                fprintf(stderr, "skipping unknown frame type 0x%02x (%u bytes)\n",
                        hdr.type, hdr.length);
            }
            free(payload);
            continue;
        }

        int rc = 0;
        if (hdr.type == FRAME_TYPE_HEADERS) {
            rc = handle_headers(fd, &hdr, payload, root);
        } else if (hdr.type == FRAME_TYPE_DATA) {
            /* GET-style protocol: stray DATA with no HEADERS is malformed. */
            rc = send_status(fd, hdr.request_id, "400", NULL, NULL,
                             FRAME_FLAG_END_STREAM, g_verbose);
        } else if (hdr.type == FRAME_TYPE_ERROR) {
            /* Peer error notification: acknowledge nothing, stay connected. */
            rc = 0;
        }
        free(payload);
        if (rc != 0) {
            return;
        }
    }
}

int main(int argc, char **argv) {
    int argi = 1;
    if (argi < argc && strcmp(argv[argi], "-v") == 0) {
        g_verbose = 1;
        argi++;
    }
    if (argc - argi != 2) {
        usage(argv[0]);
        return 1;
    }

    const char *root = argv[argi];
    const char *port_str = argv[argi + 1];
    char *end = NULL;
    long port = strtol(port_str, &end, 10);
    if (!end || *end != '\0' || port <= 0 || port > 65535) {
        fprintf(stderr, "invalid port: %s\n", port_str);
        return 1;
    }

    struct stat st;
    if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "root directory not found: %s\n", root);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        perror("socket");
        return 1;
    }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(srv);
        return 1;
    }
    if (listen(srv, 16) != 0) {
        perror("listen");
        close(srv);
        return 1;
    }

    fprintf(stderr, "bserve listening on %ld, root=%s\n", port, root);

    for (;;) {
        int cfd = accept(srv, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }
        serve_connection(cfd, root);
        close(cfd);
    }

    close(srv);
    return 0;
}
