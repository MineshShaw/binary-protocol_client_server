#include "frame.h"
#include "hexdump.h"
#include "hpack_lite.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [-v] <host>:<port>/<path>\n"
            "       %s [-v] --raw <file.bin> <host>:<port>\n",
            argv0, argv0);
}

static int parse_host_port(const char *spec, char **host, char **port, const char **rest) {
    /* spec is host:port or host:port/path. IPv6 is not supported in this parser. */
    const char *colon = strrchr(spec, ':');
    if (!colon || colon == spec) {
        return -1;
    }
    const char *p = colon + 1;
    if (*p == '\0') {
        return -1;
    }
    char *end = NULL;
    long n = strtol(p, &end, 10);
    if (end == p || n <= 0 || n > 65535) {
        return -1;
    }
    size_t hlen = (size_t)(colon - spec);
    *host = malloc(hlen + 1);
    if (!*host) {
        return -1;
    }
    memcpy(*host, spec, hlen);
    (*host)[hlen] = '\0';

    size_t plen = (size_t)(end - p);
    *port = malloc(plen + 1);
    if (!*port) {
        free(*host);
        return -1;
    }
    memcpy(*port, p, plen);
    (*port)[plen] = '\0';
    *rest = end;
    return 0;
}

static int connect_tcp(const char *host, const char *port) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, port, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(gai));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static uint8_t *read_entire_file(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror(path);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    uint8_t *buf = malloc(sz == 0 ? 1 : (size_t)sz);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *out_len = (size_t)sz;
    return buf;
}

static int send_get(int fd, const char *host, const char *path, int verbose) {
    HpackHeaderList list;
    hpack_list_init(&list);
    if (hpack_list_add(&list, ":method", "GET") != 0
        || hpack_list_add(&list, ":path", path) != 0
        || hpack_list_add(&list, "host", host) != 0
        || hpack_list_add(&list, "user-agent", "bcurl") != 0) {
        hpack_list_free(&list);
        return -1;
    }
    uint8_t *payload = NULL;
    uint32_t plen = 0;
    if (hpack_encode(&list, &payload, &plen) != 0) {
        hpack_list_free(&list);
        return -1;
    }
    hpack_list_free(&list);

    FrameHeader hdr = {
        .length = plen,
        .type = FRAME_TYPE_HEADERS,
        .flags = FRAME_FLAG_END_STREAM,
        .request_id = 1
    };
    if (verbose) {
        hexdump_frame(stderr, "outgoing", &hdr, payload);
    }
    int rc = frame_write_full(fd, &hdr, payload);
    free(payload);
    return rc;
}

static int status_is_error(const char *status) {
    if (!status || status[0] == '\0') {
        return 1;
    }
    return status[0] == '4' || status[0] == '5';
}

static int read_responses(int fd, int verbose, uint32_t expect_id) {
    int saw_end = 0;
    int exit_err = 0;
    int saw_headers = 0;

    while (!saw_end) {
        FrameHeader hdr;
        if (frame_header_read(fd, &hdr) != 0) {
            fprintf(stderr, "connection closed before END_STREAM\n");
            return 2;
        }

        uint8_t *payload = NULL;
        if (hdr.length > 0) {
            payload = malloc(hdr.length);
            if (!payload) {
                return 2;
            }
            if (frame_read_exact(fd, payload, hdr.length) != 0) {
                free(payload);
                fprintf(stderr, "truncated payload\n");
                return 2;
            }
        }

        if (verbose) {
            hexdump_frame(stderr, "incoming", &hdr, payload);
        }

        if (!frame_type_known(hdr.type)) {
            /* Skip rule: payload already drained; wait for the next frame. */
            if (verbose) {
                fprintf(stderr, "skipping unknown frame type 0x%02x\n", hdr.type);
            }
            free(payload);
            continue;
        }

        if (expect_id != 0 && hdr.request_id != expect_id) {
            /* Pipelining: ignore frames for other request IDs. */
            free(payload);
            continue;
        }

        if (hdr.type == FRAME_TYPE_HEADERS) {
            HpackHeaderList list;
            if (hpack_decode(payload, hdr.length, &list) != 0) {
                free(payload);
                fprintf(stderr, "malformed HEADERS payload\n");
                return 2;
            }
            const char *status = hpack_list_get(&list, ":status");
            if (verbose && status) {
                fprintf(stderr, "status: %s\n", status);
            }
            if (status_is_error(status)) {
                exit_err = 1;
            }
            saw_headers = 1;
            hpack_list_free(&list);
        } else if (hdr.type == FRAME_TYPE_DATA) {
            if (hdr.length > 0) {
                fwrite(payload, 1, hdr.length, stdout);
                fflush(stdout);
            }
        } else if (hdr.type == FRAME_TYPE_ERROR) {
            exit_err = 1;
            if (hdr.length > 0) {
                fwrite(payload, 1, hdr.length, stderr);
            }
        }

        if (hdr.flags & FRAME_FLAG_END_STREAM) {
            saw_end = 1;
        }
        free(payload);
    }

    if (!saw_headers && verbose) {
        fprintf(stderr, "warning: stream ended without HEADERS\n");
    }
    return exit_err ? 1 : 0;
}

int main(int argc, char **argv) {
    int verbose = 0;
    int argi = 1;
    if (argi < argc && strcmp(argv[argi], "-v") == 0) {
        verbose = 1;
        argi++;
    }

    int raw_mode = 0;
    const char *raw_path = NULL;
    if (argi < argc && strcmp(argv[argi], "--raw") == 0) {
        raw_mode = 1;
        argi++;
        if (argi >= argc) {
            usage(argv[0]);
            return 2;
        }
        raw_path = argv[argi++];
    }

    if (argi >= argc) {
        usage(argv[0]);
        return 2;
    }

    char *host = NULL;
    char *port = NULL;
    const char *rest = NULL;
    if (parse_host_port(argv[argi], &host, &port, &rest) != 0) {
        fprintf(stderr, "invalid host:port[/path]: %s\n", argv[argi]);
        return 2;
    }

    int fd = connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "failed to connect to %s:%s\n", host, port);
        free(host);
        free(port);
        return 2;
    }

    int rc;
    if (raw_mode) {
        size_t n = 0;
        uint8_t *raw = read_entire_file(raw_path, &n);
        if (!raw) {
            close(fd);
            free(host);
            free(port);
            return 2;
        }
        if (verbose) {
            hexdump_labeled(stderr, "outgoing raw", raw, n);
        }
        rc = frame_write_exact(fd, raw, n);
        free(raw);
        if (rc != 0) {
            fprintf(stderr, "failed to send raw bytes\n");
            close(fd);
            free(host);
            free(port);
            return 2;
        }
        /* Raw fuzzing still reads the server's reply on the same connection. */
        rc = read_responses(fd, verbose, 0);
    } else {
        const char *path = (*rest == '\0') ? "/" : rest;
        if (send_get(fd, host, path, verbose) != 0) {
            fprintf(stderr, "failed to send request\n");
            close(fd);
            free(host);
            free(port);
            return 2;
        }
        rc = read_responses(fd, verbose, 1);
    }

    close(fd);
    free(host);
    free(port);
    return rc;
}
