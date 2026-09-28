#include "frame.h"
#include "hexdump.h"
#include "hpack_lite.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [-v] [-X GET|HEAD|POST] [--data file] <host>:<port>/<path>\n",
            argv0);
}

static int parse_host_port(const char *spec, char **host, char **port,
                           const char **rest) {
    const char *colon = strrchr(spec, ':');
    if (!colon || colon == spec || colon[1] == '\0') {
        return -1;
    }
    char *end = NULL;
    errno = 0;
    long number = strtol(colon + 1, &end, 10);
    if (errno != 0 || end == colon + 1 || number <= 0 || number > 65535
        || (*end != '\0' && *end != '/')) {
        return -1;
    }

    size_t host_len = (size_t)(colon - spec);
    size_t port_len = (size_t)(end - (colon + 1));
    *host = malloc(host_len + 1);
    *port = malloc(port_len + 1);
    if (!*host || !*port) {
        free(*host);
        free(*port);
        return -1;
    }
    memcpy(*host, spec, host_len);
    (*host)[host_len] = '\0';
    memcpy(*port, colon + 1, port_len);
    (*port)[port_len] = '\0';
    *rest = end;
    return 0;
}

static int connect_tcp(const char *host, const char *port) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *addresses = NULL;
    int result = getaddrinfo(host, port, &hints, &addresses);
    if (result != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(result));
        return -1;
    }

    struct addrinfo *address = addresses;
    int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd >= 0 && connect(fd, address->ai_addr, address->ai_addrlen) != 0) {
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}

static uint8_t *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || (unsigned long)size > FRAME_U24_MAX) {
        fclose(file);
        fprintf(stderr, "request body exceeds the protocol frame limit\n");
        return NULL;
    }
    rewind(file);
    uint8_t *data = malloc(size == 0 ? 1 : (size_t)size);
    if (!data || (size > 0 && fread(data, 1, (size_t)size, file) != (size_t)size)) {
        free(data);
        fclose(file);
        fprintf(stderr, "failed to read request body\n");
        return NULL;
    }
    fclose(file);
    *length = (size_t)size;
    return data;
}

static int send_frame(int fd, const FrameHeader *header, const void *payload,
                      int verbose) {
    if (verbose) {
        hexdump_frame(stderr, "outgoing", header, payload);
    }
    return frame_write_full(fd, header, payload);
}

static int send_request(int fd, const char *host, const char *path,
                       const char *method, const uint8_t *body, size_t body_len,
                       int verbose) {
    char content_length[32];
    snprintf(content_length, sizeof(content_length), "%zu", body_len);

    HpackHeaderList headers;
    hpack_list_init(&headers);
    if (hpack_list_add(&headers, ":method", method) != 0
        || hpack_list_add(&headers, ":path", path) != 0
        || hpack_list_add(&headers, "host", host) != 0
        || hpack_list_add(&headers, "user-agent", "bcurl") != 0
        || hpack_list_add(&headers, "content-length", content_length) != 0
        || (body_len > 0
            && hpack_list_add(&headers, "content-type", "application/octet-stream") != 0)) {
        hpack_list_free(&headers);
        return -1;
    }

    uint8_t *payload = NULL;
    uint32_t payload_len = 0;
    if (hpack_encode(&headers, &payload, &payload_len) != 0) {
        hpack_list_free(&headers);
        return -1;
    }
    hpack_list_free(&headers);

    FrameHeader header = {
        .length = payload_len,
        .type = FRAME_TYPE_HEADERS,
        .flags = body_len == 0 ? FRAME_FLAG_END_STREAM : 0,
        .request_id = 1
    };
    int result = send_frame(fd, &header, payload, verbose);
    free(payload);
    if (result != 0) {
        return -1;
    }

    if (body_len > 0) {
        FrameHeader data_header = {
            .length = (uint32_t)body_len,
            .type = FRAME_TYPE_DATA,
            .flags = FRAME_FLAG_END_STREAM,
            .request_id = 1
        };
        return send_frame(fd, &data_header, body, verbose);
    }
    return 0;
}

static int read_frame(int fd, FrameHeader *header, uint8_t **payload) {
    *payload = NULL;
    if (frame_header_read(fd, header) != 0) {
        return -1;
    }
    if (header->length == 0) {
        return 0;
    }
    *payload = malloc(header->length);
    if (!*payload || frame_read_exact(fd, *payload, header->length) != 0) {
        free(*payload);
        *payload = NULL;
        return -1;
    }
    return 0;
}

static size_t header_count(const HpackHeaderList *headers, const char *name) {
    size_t count = 0;
    for (size_t i = 0; i < headers->count; i++) {
        if (strcmp(headers->items[i].name, name) == 0) {
            count++;
        }
    }
    return count;
}

static int read_response(int fd, int verbose, const char *method) {
    int got_headers = 0;
    int status_code = 0;
    int ended = 0;
    uint64_t body_bytes = 0;
    uint64_t expected_length = UINT64_MAX;

    while (!ended) {
        FrameHeader header;
        uint8_t *payload = NULL;
        if (read_frame(fd, &header, &payload) != 0) {
            fprintf(stderr, "connection closed before response END_STREAM\n");
            return 2;
        }
        if (verbose) {
            hexdump_frame(stderr, "incoming", &header, payload);
        }
        if (!frame_type_known(header.type)) {
            free(payload);
            continue;
        }
        if ((header.flags & ~FRAME_FLAG_END_STREAM) != 0 || header.request_id != 1) {
            free(payload);
            fprintf(stderr, "malformed response frame\n");
            return 2;
        }

        if (header.type == FRAME_TYPE_HEADERS) {
            HpackHeaderList fields;
            if (got_headers || hpack_decode(payload, header.length, &fields) != 0) {
                free(payload);
                fprintf(stderr, "malformed response headers\n");
                return 2;
            }
            const char *status = hpack_list_get(&fields, ":status");
            const char *length = hpack_list_get(&fields, "content-length");
            if (header_count(&fields, ":status") != 1
                || header_count(&fields, "content-length") != 1
                || !status || !length || strlen(status) != 3
                || status[0] < '1' || status[0] > '5'
                || status[1] < '0' || status[1] > '9'
                || status[2] < '0' || status[2] > '9') {
                hpack_list_free(&fields);
                free(payload);
                fprintf(stderr, "response is missing a valid :status\n");
                return 2;
            }
            status_code = (status[0] - '0') * 100 + (status[1] - '0') * 10
                        + (status[2] - '0');
            char *end = NULL;
            errno = 0;
            unsigned long long value = strtoull(length, &end, 10);
            if (errno != 0 || end == length || *end != '\0') {
                hpack_list_free(&fields);
                free(payload);
                fprintf(stderr, "invalid response Content-Length\n");
                return 2;
            }
            expected_length = value;
            got_headers = 1;
            hpack_list_free(&fields);
        } else if (header.type == FRAME_TYPE_DATA) {
            if (!got_headers || strcmp(method, "HEAD") == 0) {
                free(payload);
                fprintf(stderr, "unexpected response DATA frame\n");
                return 2;
            }
            if (header.length > 0
                && fwrite(payload, 1, header.length, stdout) != header.length) {
                free(payload);
                perror("stdout");
                return 2;
            }
            body_bytes += header.length;
            fflush(stdout);
        } else {
            free(payload);
            fprintf(stderr, "server sent an ERROR frame\n");
            return 2;
        }
        ended = (header.flags & FRAME_FLAG_END_STREAM) != 0;
        free(payload);
    }

    if (!got_headers || (strcmp(method, "HEAD") != 0
                         && expected_length != UINT64_MAX
                         && body_bytes != expected_length)) {
        fprintf(stderr, "response ended with incomplete headers or body\n");
        return 2;
    }
    return status_code >= 400 ? 1 : 0;
}

int main(int argc, char **argv) {
    int verbose = 0;
    const char *method = "GET";
    const char *body_path = NULL;
    int index = 1;
    while (index < argc && argv[index][0] == '-') {
        if (strcmp(argv[index], "-v") == 0) {
            verbose = 1;
            index++;
        } else if (strcmp(argv[index], "-X") == 0 && index + 1 < argc) {
            method = argv[index + 1];
            index += 2;
        } else if (strcmp(argv[index], "--data") == 0 && index + 1 < argc) {
            body_path = argv[index + 1];
            index += 2;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (index + 1 != argc
        || (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0
            && strcmp(method, "POST") != 0)
        || (body_path && strcmp(method, "POST") != 0)) {
        usage(argv[0]);
        return 2;
    }

    char *host = NULL;
    char *port = NULL;
    const char *rest = NULL;
    if (parse_host_port(argv[index], &host, &port, &rest) != 0) {
        fprintf(stderr, "invalid host:port/path: %s\n", argv[index]);
        return 2;
    }
    const char *path = *rest == '\0' ? "/" : rest;
    uint8_t *body = NULL;
    size_t body_len = 0;
    if (body_path) {
        body = read_file(body_path, &body_len);
        if (!body) {
            free(host);
            free(port);
            return 2;
        }
    }
    if ((strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) && body_len != 0) {
        fprintf(stderr, "--data is only valid with POST\n");
        free(body);
        free(host);
        free(port);
        return 2;
    }

    int fd = connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "failed to connect to %s:%s\n", host, port);
        free(body);
        free(host);
        free(port);
        return 2;
    }
    int result = send_request(fd, host, path, method, body, body_len, verbose) == 0
               ? read_response(fd, verbose, method) : 2;
    if (result == 2) {
        fprintf(stderr, "request or response failed\n");
    }
    close(fd);
    free(body);
    free(host);
    free(port);
    return result;
}
