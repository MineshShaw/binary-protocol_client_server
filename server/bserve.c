#define _XOPEN_SOURCE 700

#include "frame.h"
#include "hexdump.h"
#include "hpack_lite.h"

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int g_verbose;

typedef struct {
    FrameHeader header;
    uint8_t *payload;
} ReceivedFrame;

static void usage(const char *program) {
    fprintf(stderr, "Usage: %s [-v] <root_directory> <port>\n", program);
}

static void free_frame(ReceivedFrame *frame) {
    free(frame->payload);
    frame->payload = NULL;
}

static int read_frame(int fd, ReceivedFrame *frame) {
    frame->payload = NULL;
    if (frame_header_read(fd, &frame->header) != 0) {
        return -1;
    }
    if (frame->header.length > 0) {
        frame->payload = malloc(frame->header.length);
        if (!frame->payload
            || frame_read_exact(fd, frame->payload, frame->header.length) != 0) {
            free_frame(frame);
            return -1;
        }
    }
    if (g_verbose) {
        hexdump_frame(stderr, "incoming", &frame->header, frame->payload);
    }
    return 0;
}

static int send_frame(int fd, const FrameHeader *header, const void *payload) {
    if (g_verbose) {
        hexdump_frame(stderr, "outgoing", header, payload);
    }
    return frame_write_full(fd, header, payload);
}

static int add_header(HpackHeaderList *headers, const char *name, const char *value) {
    return hpack_list_add(headers, name, value);
}

static int send_response(int fd, uint32_t request_id, const char *status,
                         const char *content_type, const uint8_t *body,
                         uint32_t body_len, int head_only) {
    char length[32];
    snprintf(length, sizeof(length), "%u", body_len);

    time_t now = time(NULL);
    struct tm tm_utc;
    char date[64];
    if (now == (time_t)-1 || !gmtime_r(&now, &tm_utc)
        || strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm_utc) == 0) {
        return -1;
    }

    HpackHeaderList headers;
    hpack_list_init(&headers);
    if (add_header(&headers, ":status", status) != 0
        || add_header(&headers, "content-length", length) != 0
        || add_header(&headers, "content-type", content_type) != 0
        || add_header(&headers, "date", date) != 0
        || add_header(&headers, "server", "bserve") != 0
        || add_header(&headers, "connection", "keep-alive") != 0) {
        hpack_list_free(&headers);
        return -1;
    }
    uint8_t *encoded = NULL;
    uint32_t encoded_len = 0;
    if (hpack_encode(&headers, &encoded, &encoded_len) != 0) {
        hpack_list_free(&headers);
        return -1;
    }
    hpack_list_free(&headers);

    FrameHeader header = {
        .length = encoded_len,
        .type = FRAME_TYPE_HEADERS,
        .flags = (body_len == 0 || head_only) ? FRAME_FLAG_END_STREAM : 0,
        .request_id = request_id
    };
    int result = send_frame(fd, &header, encoded);
    free(encoded);
    if (result != 0 || body_len == 0 || head_only) {
        return result;
    }

    FrameHeader data_header = {
        .length = body_len,
        .type = FRAME_TYPE_DATA,
        .flags = FRAME_FLAG_END_STREAM,
        .request_id = request_id
    };
    return send_frame(fd, &data_header, body);
}

static int header_count(const HpackHeaderList *headers, const char *name) {
    int count = 0;
    for (size_t i = 0; i < headers->count; i++) {
        if (strcmp(headers->items[i].name, name) == 0) {
            count++;
        }
    }
    return count;
}

static int parse_content_length(const HpackHeaderList *headers, uint32_t *length) {
    const char *value = hpack_list_get(headers, "content-length");
    if (header_count(headers, "content-length") != 1 || !value || *value == '\0') {
        return -1;
    }
    uint64_t result = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (!isdigit(*p)) {
            return -1;
        }
        uint64_t digit = (uint64_t)(*p - '0');
        if (result > (FRAME_U24_MAX - digit) / 10) {
            return -1;
        }
        result = result * 10 + digit;
    }
    *length = (uint32_t)result;
    return 0;
}

static int read_request_body(int fd, uint32_t request_id, uint32_t expected,
                             uint8_t **body_out) {
    uint8_t *body = malloc(expected == 0 ? 1 : expected);
    if (!body) {
        return -1;
    }
    uint32_t received = 0;
    int ended = expected == 0;
    while (!ended) {
        ReceivedFrame frame;
        if (read_frame(fd, &frame) != 0) {
            free(body);
            return -1;
        }
        if (!frame_type_known(frame.header.type)) {
            free_frame(&frame);
            continue;
        }
        if (frame.header.type != FRAME_TYPE_DATA
            || frame.header.request_id != request_id
            || (frame.header.flags & ~FRAME_FLAG_END_STREAM) != 0
            || frame.header.length > expected - received) {
            free_frame(&frame);
            free(body);
            return -1;
        }
        if (frame.header.length > 0) {
            memcpy(body + received, frame.payload, frame.header.length);
            received += frame.header.length;
        }
        ended = (frame.header.flags & FRAME_FLAG_END_STREAM) != 0;
        free_frame(&frame);
        if (ended && received != expected) {
            free(body);
            return -1;
        }
    }
    *body_out = body;
    return 0;
}

static int path_is_safe(const char *relative) {
    if (!relative || *relative == '\0') {
        return 0;
    }
    const char *segment = relative;
    for (const char *p = relative; ; p++) {
        if (*p == '/' || *p == '\0') {
            size_t size = (size_t)(p - segment);
            if (size == 0 || (size == 1 && segment[0] == '.')
                || (size == 2 && segment[0] == '.' && segment[1] == '.')) {
                return 0;
            }
            if (*p == '\0') {
                break;
            }
            segment = p + 1;
        }
    }
    return 1;
}

static char *resolve_file(const char *root, const char *url_path) {
    if (!url_path || url_path[0] != '/') {
        errno = EINVAL;
        return NULL;
    }
    const char *relative = url_path + 1;
    size_t relative_len = strcspn(relative, "?#");
    if (relative_len == 0) {
        relative = "index.html";
        relative_len = strlen(relative);
    }
    char *relative_path = malloc(relative_len + 1);
    if (!relative_path) {
        errno = ENOMEM;
        return NULL;
    }
    memcpy(relative_path, relative, relative_len);
    relative_path[relative_len] = '\0';
    if (!path_is_safe(relative_path)) {
        free(relative_path);
        errno = EINVAL;
        return NULL;
    }

    size_t size = strlen(root) + relative_len + 2;
    char *candidate = malloc(size);
    if (!candidate) {
        free(relative_path);
        errno = ENOMEM;
        return NULL;
    }
    snprintf(candidate, size, "%s/%s", root, relative_path);
    free(relative_path);

    char *resolved = realpath(candidate, NULL);
    free(candidate);
    if (!resolved) {
        return NULL;
    }
    size_t root_len = strlen(root);
    if (strncmp(root, resolved, root_len) != 0
        || (root_len > 1 && resolved[root_len] != '/' && resolved[root_len] != '\0')) {
        free(resolved);
        errno = EINVAL;
        return NULL;
    }
    return resolved;
}

static const char *content_type_for(const char *path) {
    const char *extension = strrchr(path, '.');
    if (!extension) return "application/octet-stream";
    if (strcmp(extension, ".html") == 0 || strcmp(extension, ".htm") == 0) return "text/html";
    if (strcmp(extension, ".txt") == 0) return "text/plain";
    if (strcmp(extension, ".css") == 0) return "text/css";
    if (strcmp(extension, ".js") == 0) return "application/javascript";
    if (strcmp(extension, ".json") == 0) return "application/json";
    if (strcmp(extension, ".png") == 0) return "image/png";
    if (strcmp(extension, ".jpg") == 0 || strcmp(extension, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(extension, ".gif") == 0) return "image/gif";
    return "application/octet-stream";
}

static int load_file(const char *path, uint8_t **body, uint32_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        errno = EIO;
        return -1;
    }
    long size = ftell(file);
    if (size < 0 || (unsigned long)size > FRAME_U24_MAX) {
        fclose(file);
        errno = EFBIG;
        return -1;
    }
    rewind(file);
    uint8_t *buffer = malloc(size == 0 ? 1 : (size_t)size);
    if (!buffer || (size > 0 && fread(buffer, 1, (size_t)size, file) != (size_t)size)) {
        free(buffer);
        fclose(file);
        errno = EIO;
        return -1;
    }
    fclose(file);
    *body = buffer;
    *length = (uint32_t)size;
    return 0;
}

static int respond_error(int fd, uint32_t id, const char *status,
                         const char *message, int head_only) {
    return send_response(fd, id, status, "text/plain", (const uint8_t *)message,
                         (uint32_t)strlen(message), head_only);
}

static int handle_request(int fd, const char *root, ReceivedFrame *frame) {
    uint32_t request_id = frame->header.request_id;
    if (frame->header.type != FRAME_TYPE_HEADERS || request_id == 0
        || (frame->header.flags & ~FRAME_FLAG_END_STREAM) != 0) {
        return respond_error(fd, request_id, "400", "Bad Request\n", 0);
    }

    HpackHeaderList headers;
    if (hpack_decode(frame->payload, frame->header.length, &headers) != 0) {
        return respond_error(fd, request_id, "400", "Bad Request\n", 0);
    }
    const char *method = hpack_list_get(&headers, ":method");
    const char *path = hpack_list_get(&headers, ":path");
    const char *content_type = hpack_list_get(&headers, "content-type");
    uint32_t content_length = 0;
    int head_only = method && strcmp(method, "HEAD") == 0;
    if (header_count(&headers, ":method") != 1
        || header_count(&headers, ":path") != 1
        || !method || !path || parse_content_length(&headers, &content_length) != 0
        || (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0
            && strcmp(method, "POST") != 0)) {
        hpack_list_free(&headers);
        return respond_error(fd, request_id, "400", "Bad Request\n", head_only);
    }

    int headers_ended = (frame->header.flags & FRAME_FLAG_END_STREAM) != 0;
    if ((strcmp(method, "POST") == 0 && headers_ended != (content_length == 0))
        || (strcmp(method, "POST") != 0 && (!headers_ended || content_length != 0))) {
        hpack_list_free(&headers);
        return respond_error(fd, request_id, "400", "Bad Request\n", head_only);
    }

    uint8_t *request_body = NULL;
    if (content_length > 0
        && read_request_body(fd, request_id, content_length, &request_body) != 0) {
        hpack_list_free(&headers);
        return respond_error(fd, request_id, "400", "Bad Request\n", head_only);
    }

    if (strcmp(method, "POST") == 0) {
        int result = send_response(fd, request_id, "200",
                                   content_type ? content_type : "application/octet-stream",
                                   request_body, content_length, 0);
        hpack_list_free(&headers);
        free(request_body);
        return result;
    }

    char *file_path = resolve_file(root, path);
    hpack_list_free(&headers);
    if (!file_path) {
        const char *status = (errno == ENOENT || errno == ENOTDIR) ? "404"
                           : (errno == EINVAL) ? "400" : "500";
        const char *message = strcmp(status, "404") == 0 ? "Not Found\n"
                            : strcmp(status, "400") == 0 ? "Bad Request\n"
                            : "Internal Server Error\n";
        return respond_error(fd, request_id, status, message, head_only);
    }
    struct stat stat_buffer;
    if (stat(file_path, &stat_buffer) != 0) {
        int missing = errno == ENOENT || errno == ENOTDIR;
        free(file_path);
        return respond_error(fd, request_id, missing ? "404" : "500",
                             missing ? "Not Found\n" : "Internal Server Error\n",
                             head_only);
    }
    if (!S_ISREG(stat_buffer.st_mode)) {
        free(file_path);
        return respond_error(fd, request_id, "404", "Not Found\n", head_only);
    }

    uint8_t *body = NULL;
    uint32_t body_len = 0;
    if (load_file(file_path, &body, &body_len) != 0) {
        int missing = errno == ENOENT || errno == ENOTDIR;
        free(file_path);
        return respond_error(fd, request_id, missing ? "404" : "500",
                             missing ? "Not Found\n" : "Internal Server Error\n",
                             head_only);
    }
    const char *response_type = content_type_for(file_path);
    free(file_path);
    int result = send_response(fd, request_id, "200", response_type, body,
                               body_len, head_only);
    free(body);
    return result;
}

static void serve_connection(int fd, const char *root) {
    for (;;) {
        ReceivedFrame frame;
        if (read_frame(fd, &frame) != 0) {
            return;
        }
        if (!frame_type_known(frame.header.type)) {
            if (g_verbose) {
                fprintf(stderr, "skipping unknown frame type 0x%02x (%u bytes)\n",
                        frame.header.type, frame.header.length);
            }
            free_frame(&frame);
            continue;
        }
        int result = handle_request(fd, root, &frame);
        free_frame(&frame);
        if (result != 0) {
            return;
        }
    }
}

int main(int argc, char **argv) {
    int index = 1;
    if (index < argc && strcmp(argv[index], "-v") == 0) {
        g_verbose = 1;
        index++;
    }
    if (argc - index != 2) {
        usage(argv[0]);
        return 1;
    }

    char *root = realpath(argv[index], NULL);
    if (!root) {
        perror("root directory");
        return 1;
    }
    struct stat root_stat;
    if (stat(root, &root_stat) != 0 || !S_ISDIR(root_stat.st_mode)) {
        fprintf(stderr, "root directory is not a directory: %s\n", root);
        free(root);
        return 1;
    }

    char *end = NULL;
    errno = 0;
    long port = strtol(argv[index + 1], &end, 10);
    if (errno != 0 || end == argv[index + 1] || *end != '\0'
        || port <= 0 || port > 65535) {
        fprintf(stderr, "invalid port: %s\n", argv[index + 1]);
        free(root);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%ld", port);
    struct addrinfo *addresses = NULL;
    int gai = getaddrinfo(NULL, port_text, &hints, &addresses);
    if (gai != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(gai));
        free(root);
        return 1;
    }

    int server = -1;
    for (struct addrinfo *address = addresses; address; address = address->ai_next) {
        server = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (server < 0) {
            continue;
        }
        int enabled = 1;
        (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        if (bind(server, address->ai_addr, address->ai_addrlen) == 0
            && listen(server, 16) == 0) {
            break;
        }
        close(server);
        server = -1;
    }
    freeaddrinfo(addresses);
    if (server < 0) {
        perror("bind/listen");
        free(root);
        return 1;
    }

    fprintf(stderr, "bserve listening on %ld, root=%s\n", port, root);
    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }
        serve_connection(client, root);
        close(client);
    }
    close(server);
    free(root);
    return 0;
}
