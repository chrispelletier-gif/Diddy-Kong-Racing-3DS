/* Bug reports as GitHub issues (see bugreport.h). Platform independent apart from the caller's network setup. */
#include "bugreport.h"
#include "curl_common.h"
#include "plat.h"
#include "strbuf.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* GitHub refuses bodies over 65536 characters; this leaves room for the wrapper text. */
#define BASE64_PER_COMMENT 56000
#define BASE64_LINE 76

/* Appends text as the inside of a JSON string. */
static void sb_add_json(StrBuf *sb, const char *text) {
    const unsigned char *p;
    for (p = (const unsigned char *) text; *p; p++) {
        char escaped[8];
        switch (*p) {
            case '"':
                sb_add(sb, "\\\"");
                break;
            case '\\':
                sb_add(sb, "\\\\");
                break;
            case '\n':
                sb_add(sb, "\\n");
                break;
            case '\r':
                sb_add(sb, "\\r");
                break;
            case '\t':
                sb_add(sb, "\\t");
                break;
            default:
                if (*p < 0x20) {
                    snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
                    sb_add(sb, escaped);
                } else {
                    sb_add_n(sb, (const char *) p, 1);
                }
                break;
        }
    }
}

static size_t write_response(char *data, size_t size, size_t count, void *user) {
    StrBuf *sb = (StrBuf *) user;
    // Only the start of a response is looked at; the rest is dropped to save memory.
    if (sb->length < 16384) {
        sb_add_n(sb, data, size * count);
    }
    return size * count;
}

/*
 * DKR_NET_REUSE (default 1): one libcurl handle serves every request of a report, so the connection to api.github.com
 * and its TLS session are kept between them. A report is an issue plus a comment per 56 KB of attachment (twenty or
 * more for a full one), and each new connection cost a TCP connect and a TLS handshake: a second or more on the 3DS,
 * several on an Old 3DS. With 0 each request makes its own connection, as before. The updater already reuses its
 * handle (update_3ds.c http_get).
 */
static CURL *sCurl;
static int sReuse;

static CURL *curl_take(void) {
    if (!sReuse) {
        return curl_easy_init();
    }
    if (sCurl == NULL) {
        sCurl = curl_easy_init();
    } else {
        curl_easy_reset(sCurl); // Keeps the open connection and the TLS session.
    }
    return sCurl;
}

static void curl_give_back(CURL *curl) {
    if (!sReuse) {
        curl_easy_cleanup(curl);
    }
}

static void curl_done(void) {
    if (sCurl != NULL) {
        curl_easy_cleanup(sCurl);
        sCurl = NULL;
    }
}

/* Sends JSON to apiUrl + path with method (NULL: POST). Returns the HTTP status (0 if the request failed), with the
 * response in response. */
static long send_json(const BugReportConfig *config, const char *method, const char *path, const char *json,
                      StrBuf *response, char *error, size_t errorSize) {
    CURL *curl = curl_take();
    struct curl_slist *headers = NULL;
    char url[512], auth[512], curlError[CURL_ERROR_SIZE];
    long status = 0;
    CURLcode code;

    if (curl == NULL) {
        snprintf(error, errorSize, "out of memory (curl)");
        return 0;
    }
    snprintf(url, sizeof(url), "%s%s", config->apiUrl, path);
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", config->token);
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
    curlError[0] = '\0';
    curl_common_setup(curl, url, headers, config->userAgent != NULL ? config->userAgent : "dkr-bugreport",
                      write_response, response, curlError, config->caFile, config->caPem);
    if (method != NULL) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    }
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long) strlen(json));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    code = curl_easy_perform(curl);
    if (code != CURLE_OK) {
        snprintf(error, errorSize, "%s", curlError[0] != '\0' ? curlError : curl_easy_strerror(code));
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }
    curl_slist_free_all(headers);
    curl_give_back(curl);
    return status;
}

/* Copies the string value of "key" in a JSON response (the first one, which for an issue is the issue's own). */
static int json_find_string(const char *json, const char *key, char *out, size_t outSize) {
    char pattern[64];
    const char *p, *end;
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = (json != NULL) ? strstr(json, pattern) : NULL;
    if (p == NULL) {
        return 0;
    }
    p = strchr(p + strlen(pattern), '"');
    if (p == NULL) {
        return 0;
    }
    p++;
    end = strchr(p, '"');
    if (end == NULL || (size_t) (end - p) >= outSize) {
        return 0;
    }
    memcpy(out, p, end - p);
    out[end - p] = '\0';
    return 1;
}

static void describe_failure(long status, const StrBuf *response, const char *curlError, char *result,
                             size_t resultSize) {
    char message[160];
    if (status == 0) {
        snprintf(result, resultSize, "could not reach GitHub: %s", curlError);
    } else if (json_find_string(response->data, "message", message, sizeof(message))) {
        snprintf(result, resultSize, "GitHub said %ld: %s", status, message);
    } else {
        snprintf(result, resultSize, "GitHub said %ld", status);
    }
}

/* gzip compresses data. Returns a malloc'd buffer, or NULL. */
static unsigned char *gzip(const unsigned char *data, unsigned int size, unsigned int *outSize) {
    z_stream stream;
    uLong bound;
    unsigned char *out;

    memset(&stream, 0, sizeof(stream));
    if (deflateInit2(&stream, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return NULL;
    }
    bound = deflateBound(&stream, size) + 32;
    out = malloc(bound);
    if (out == NULL) {
        deflateEnd(&stream);
        return NULL;
    }
    stream.next_in = (Bytef *) data;
    stream.avail_in = size;
    stream.next_out = out;
    stream.avail_out = bound;
    if (deflate(&stream, Z_FINISH) != Z_STREAM_END) {
        deflateEnd(&stream);
        free(out);
        return NULL;
    }
    *outSize = stream.total_out;
    deflateEnd(&stream);
    return out;
}

static char *base64(const unsigned char *data, unsigned int size, size_t *outLength) {
    static const char sDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t length = ((size + 2) / 3) * 4;
    char *out = malloc(length + 1);
    unsigned int i;
    char *p = out;

    if (out == NULL) {
        return NULL;
    }
    for (i = 0; i + 2 < size; i += 3) {
        unsigned int v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        *p++ = sDigits[v >> 18];
        *p++ = sDigits[(v >> 12) & 63];
        *p++ = sDigits[(v >> 6) & 63];
        *p++ = sDigits[v & 63];
    }
    if (i < size) {
        unsigned int v = data[i] << 16;
        if (i + 1 < size) {
            v |= data[i + 1] << 8;
        }
        *p++ = sDigits[v >> 18];
        *p++ = sDigits[(v >> 12) & 63];
        *p++ = (i + 1 < size) ? sDigits[(v >> 6) & 63] : '=';
        *p++ = '=';
    }
    *p = '\0';
    *outLength = length;
    return out;
}

static long post_comment(const BugReportConfig *config, const char *commentsPath, const char *text, char *result,
                         size_t resultSize) {
    StrBuf json = { 0 }, response = { 0 };
    char curlError[CURL_ERROR_SIZE];
    long status;

    sb_add(&json, "{\"body\":\"");
    sb_add_json(&json, text);
    sb_add(&json, "\"}");
    if (json.failed) {
        sb_free(&json);
        snprintf(result, resultSize, "out of memory");
        return 0;
    }
    curlError[0] = '\0';
    status = send_json(config, NULL, commentsPath, json.data, &response, curlError, sizeof(curlError));
    if (status != 201) {
        describe_failure(status, &response, curlError, result, resultSize);
    }
    sb_free(&json);
    sb_free(&response);
    return status;
}

/* Posts one comment of a file's attachment: text is that part's base64 (a whole number of 4-character groups but the
 * last). Returns 0, or -1 with result saying why. */
static int post_part(const BugReportConfig *config, const char *commentsPath, const BugReportFile *file, int part,
                     int parts, const char *text, size_t length, BugReportProgress progress, char *result,
                     size_t resultSize) {
    StrBuf comment = { 0 };
    char line[256];
    size_t offset;

    if (progress != NULL) {
        snprintf(line, sizeof(line), "Sending %s (%d of %d)", file->name, part + 1, parts);
        progress(line);
    }
    snprintf(line, sizeof(line),
             "<details><summary>Attachment %s, part %d of %d</summary>\n\n"
             "<!-- dkr-attachment name=%s part=%d parts=%d size=%u encoding=gzip+base64 -->\n\n```\n",
             file->name, part + 1, parts, file->name, part + 1, parts, file->size);
    sb_add(&comment, line);
    for (offset = 0; offset < length; offset += BASE64_LINE) {
        size_t n = (length - offset < BASE64_LINE) ? length - offset : BASE64_LINE;
        sb_add_n(&comment, text + offset, n);
        sb_add(&comment, "\n");
    }
    sb_add(&comment, "```\n\n</details>\n");
    if (comment.failed || post_comment(config, commentsPath, comment.data, result, resultSize) != 201) {
        if (comment.failed) {
            snprintf(result, resultSize, "out of memory");
        }
        sb_free(&comment);
        return -1;
    }
    sb_free(&comment);
    return 0;
}

/*
 * DKR_NET_STREAM (default 1): a file attached by its path is read from the SD card and compressed a piece at a time,
 * so a report never holds a whole file, its gzip copy and its base64 copy at once. On an Old 3DS after a long switch
 * bench the heap has about 2 MB free in pieces: the results files (350-550 KB each) could not be loaded, and the
 * network's own buffer (net_3ds_begin, 256 KB to 1 MB) did not fit after them, so the whole report failed (runs 52
 * and 54). The file is compressed twice: once to count its parts (each comment's header says how many), then again
 * to send them. zlib gives the same bytes both times; only the first size bytes are read, so a log that grows
 * meanwhile still matches. A smaller window (8 KB, about 50 KB of zlib state instead of about 260 KB) costs a little
 * compression.
 */
#define STREAM_IN 16384
#define PACKED_PER_COMMENT (BASE64_PER_COMMENT / 4 * 3) /* Whole 3-byte groups, so the parts' base64 joins up. */

typedef struct Packer {
    z_stream z;
    FILE *file;
    unsigned int left; /* Bytes of the file still to read. */
    int done;
    unsigned char in[STREAM_IN];
} Packer;

static int packer_open(Packer *p, const BugReportFile *file) {
    memset(&p->z, 0, sizeof(p->z));
    p->left = file->size;
    p->done = 0;
    p->file = fopen(file->path, "rb");
    if (p->file == NULL) {
        return -1;
    }
    if (deflateInit2(&p->z, 6, Z_DEFLATED, 13 + 16, 6, Z_DEFAULT_STRATEGY) != Z_OK) {
        fclose(p->file);
        p->file = NULL;
        return -1;
    }
    return 0;
}

static void packer_close(Packer *p) {
    if (p->file != NULL) {
        deflateEnd(&p->z);
        fclose(p->file);
        p->file = NULL;
    }
}

/* Fills out with the next compressed bytes (fewer only at the end); *got is how many. -1 if the file can't be read. */
static int packer_read(Packer *p, unsigned char *out, unsigned int outSize, unsigned int *got) {
    p->z.next_out = out;
    p->z.avail_out = outSize;
    while (p->z.avail_out > 0 && !p->done) {
        int status;
        if (p->z.avail_in == 0 && p->left > 0) {
            size_t want = p->left < STREAM_IN ? p->left : STREAM_IN;
            if (fread(p->in, 1, want, p->file) != want) {
                return -1; // The file got shorter
            }
            p->left -= (unsigned int) want;
            p->z.next_in = p->in;
            p->z.avail_in = (uInt) want;
        }
        status = deflate(&p->z, p->left == 0 && p->z.avail_in == 0 ? Z_FINISH : Z_NO_FLUSH);
        if (status == Z_STREAM_END) {
            p->done = 1;
        } else if (status != Z_OK && status != Z_BUF_ERROR) {
            return -1;
        }
    }
    *got = outSize - p->z.avail_out;
    return 0;
}

/* Returns as post_file does. */
static int post_file_streamed(const BugReportConfig *config, const char *commentsPath, const BugReportFile *file,
                              unsigned int *budget, BugReportProgress progress, char *result, size_t resultSize) {
    Packer *packer = malloc(sizeof(Packer));
    unsigned char *packed = malloc(PACKED_PER_COMMENT);
    unsigned int packedSize = 0, got = 0, sent = 0;
    int parts, part, outcome = 0;

    if (packer == NULL || packed == NULL) {
        free(packer);
        free(packed);
        snprintf(result, resultSize, "out of memory");
        return -1;
    }
    // First pass: the compressed size, for the budget and the number of parts.
    if (packer_open(packer, file) != 0) {
        outcome = 1;
        goto out;
    }
    do {
        if (packer_read(packer, packed, PACKED_PER_COMMENT, &got) != 0) {
            packer_close(packer);
            outcome = 1;
            goto out;
        }
        packedSize += got;
    } while (got == PACKED_PER_COMMENT);
    packer_close(packer);
    if (packedSize > *budget) {
        outcome = 1;
        goto out;
    }
    *budget -= packedSize;
    parts = (int) ((packedSize + PACKED_PER_COMMENT - 1) / PACKED_PER_COMMENT);
    if (parts == 0) {
        parts = 1;
    }
    // Second pass: each comment's share, compressed again and sent.
    if (packer_open(packer, file) != 0) {
        snprintf(result, resultSize, "could not open %s again", file->name);
        outcome = -1;
        goto out;
    }
    for (part = 0; part < parts; part++) {
        size_t textLength = 0;
        char *text;
        if (packer_read(packer, packed, PACKED_PER_COMMENT, &got) != 0 ||
            (part + 1 < parts && got != PACKED_PER_COMMENT)) {
            snprintf(result, resultSize, "%s changed while it was sent", file->name);
            outcome = -1;
            break;
        }
        sent += got;
        text = base64(packed, got, &textLength);
        if (text == NULL) {
            snprintf(result, resultSize, "out of memory");
            outcome = -1;
            break;
        }
        outcome = post_part(config, commentsPath, file, part, parts, text, textLength, progress, result, resultSize);
        free(text);
        if (outcome != 0) {
            break;
        }
    }
    if (outcome == 0 && sent != packedSize) {
        snprintf(result, resultSize, "%s changed while it was sent", file->name);
        outcome = -1;
    }
    packer_close(packer);
out:
    free(packer);
    free(packed);
    return outcome;
}

/* Posts one file as comments. Returns 0 on success, 1 if it was too big, -1 if posting failed (result says why). */
static int post_file(const BugReportConfig *config, const char *commentsPath, const BugReportFile *file,
                     unsigned int *budget, BugReportProgress progress, char *result, size_t resultSize) {
    unsigned int packedSize = 0;
    unsigned char *packed;
    size_t textLength = 0, offset;
    char *text;
    int parts, part;

    if (file->data == NULL) {
        return post_file_streamed(config, commentsPath, file, budget, progress, result, resultSize);
    }
    packed = gzip(file->data, file->size, &packedSize);
    if (packed == NULL) {
        return 1;
    }
    if (packedSize > *budget) {
        free(packed);
        return 1;
    }
    *budget -= packedSize;
    text = base64(packed, packedSize, &textLength);
    free(packed);
    if (text == NULL) {
        return 1;
    }
    parts = (int) ((textLength + BASE64_PER_COMMENT - 1) / BASE64_PER_COMMENT);
    if (parts == 0) {
        parts = 1;
    }
    for (part = 0, offset = 0; part < parts; part++) {
        size_t end = offset + BASE64_PER_COMMENT;
        if (end > textLength) {
            end = textLength;
        }
        if (post_part(config, commentsPath, file, part, parts, text + offset, end - offset, progress, result,
                      resultSize) != 0) {
            free(text);
            return -1;
        }
        offset = end;
    }
    free(text);
    return 0;
}

static int send_report(const BugReportConfig *config, const char *issue, const char *title, const char *body,
                       const BugReportFile *files, int fileCount, unsigned int maxAttachmentBytes,
                       BugReportProgress progress, char *result, size_t resultSize) {
    StrBuf json = { 0 }, response = { 0 }, skipped = { 0 };
    char path[256], number[32], url[256], curlError[CURL_ERROR_SIZE];
    unsigned int budget = maxAttachmentBytes;
    long status;
    int i;

    if (issue != NULL && issue[0] != '\0') {
        // An issue made earlier (a long test run's progress): the body becomes a comment on it, and the title replaces
        // its title.
        snprintf(number, sizeof(number), "%s", issue);
        snprintf(url, sizeof(url), "https://github.com/%s/issues/%s", config->repo, number);
        snprintf(path, sizeof(path), "/repos/%s/issues/%s/comments", config->repo, number);
        if (progress != NULL) {
            progress("Adding to the issue");
        }
        if (post_comment(config, path, body, result, resultSize) != 201) {
            return -1;
        }
        if (title != NULL) {
            char issuePath[256];
            sb_add(&json, "{\"title\":\"");
            sb_add_json(&json, title);
            sb_add(&json, "\"}");
            snprintf(issuePath, sizeof(issuePath), "/repos/%s/issues/%s", config->repo, number);
            if (!json.failed) {
                send_json(config, "PATCH", issuePath, json.data, &response, curlError, sizeof(curlError));
            }
            sb_free(&json);
            sb_free(&response);
        }
        goto attach;
    }
    if (progress != NULL) {
        progress("Creating the issue");
    }
    sb_add(&json, "{\"title\":\"");
    sb_add_json(&json, title);
    sb_add(&json, "\",\"body\":\"");
    sb_add_json(&json, body);
    sb_add(&json, "\",\"labels\":[\"");
    sb_add_json(&json, config->label);
    sb_add(&json, "\"]}");
    if (json.failed) {
        sb_free(&json);
        snprintf(result, resultSize, "out of memory");
        return -1;
    }
    snprintf(path, sizeof(path), "/repos/%s/issues", config->repo);
    curlError[0] = '\0';
    status = send_json(config, NULL, path, json.data, &response, curlError, sizeof(curlError));
    sb_free(&json);
    if (status != 201) {
        describe_failure(status, &response, curlError, result, resultSize);
        sb_free(&response);
        return -1;
    }
    // "number" is not a string; take the issue number from the end of its web address instead.
    if (!json_find_string(response.data, "html_url", url, sizeof(url)) || strrchr(url, '/') == NULL) {
        sb_free(&response);
        snprintf(result, resultSize, "GitHub's answer had no issue address");
        return -1;
    }
    sb_free(&response);
    snprintf(number, sizeof(number), "%s", strrchr(url, '/') + 1);
    snprintf(path, sizeof(path), "/repos/%s/issues/%s/comments", config->repo, number);

attach:
    for (i = 0; i < fileCount; i++) {
        int outcome;
        if (files[i].data == NULL && files[i].path == NULL) {
            continue;
        }
        outcome = post_file(config, path, &files[i], &budget, progress, result, resultSize);
        if (outcome < 0) {
            // The issue exists; say where, and what did not make it.
            char reason[256];
            snprintf(reason, sizeof(reason), "%s", result);
            snprintf(result, resultSize, "%s (sending %s: %s)", url, files[i].name, reason);
            sb_free(&skipped);
            return 1;
        }
        if (outcome > 0) {
            char line[160];
            snprintf(line, sizeof(line), "- %s (%u bytes)\n", files[i].name, files[i].size);
            sb_add(&skipped, line);
        }
    }
    if (skipped.length > 0) {
        StrBuf text = { 0 };
        sb_add(&text, "These files were too big to attach and are still on the SD card in `sdmc:/3ds/dkr/`:\n\n");
        sb_add(&text, skipped.data);
        if (!text.failed) {
            post_comment(config, path, text.data, curlError, sizeof(curlError));
        }
        sb_free(&text);
    }
    sb_free(&skipped);
    snprintf(result, resultSize, "%s", url);
    return 0;
}

int bugreport_send_to(const BugReportConfig *config, const char *issue, const char *title, const char *body,
                      const BugReportFile *files, int fileCount, unsigned int maxAttachmentBytes,
                      BugReportProgress progress, char *result, size_t resultSize) {
    int status;
    sReuse = plat_switch("DKR_NET_REUSE", 1) != 0;
    status = send_report(config, issue, title, body, files, fileCount, maxAttachmentBytes, progress, result, resultSize);
    curl_done();
    return status;
}
