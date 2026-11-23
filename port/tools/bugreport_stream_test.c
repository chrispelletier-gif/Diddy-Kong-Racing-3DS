/*
 * Host test of the bug report's attachments (port/src/plat/bugreport.c), with a stand-in libcurl (stub_curl/). A file
 * is sent from memory and the same file by its path (DKR_NET_STREAM, read and compressed a piece at a time); each is
 * put back together the way port/tools/bugreport_fetch.py does (the parts' base64 joined, then gunzipped) and must
 * match the original. The streamed file also grows while it is sent, as log.txt can: only its first bytes count.
 *   gcc -Iport/tools/stub_curl -Iport/src/plat -o t port/tools/bugreport_stream_test.c port/src/plat/bugreport.c -lz
 */
#include "bugreport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

int plat_switch(const char *name, int fallback) {
    (void) name;
    return fallback;
}

#define MAX_PARTS 64
static char *sParts[2][MAX_PARTS]; // Each file's parts' base64, by part number
static int sPartCount[2], sDeclared[2];
static const char *sGrowPath;

static int decode_base64(const char *in, size_t length, unsigned char *out) {
    static const char sDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned int value = 0;
    int bits = 0, n = 0;
    size_t i;
    for (i = 0; i < length; i++) {
        const char *d = strchr(sDigits, in[i]);
        if (in[i] == '=' || d == NULL || in[i] == '\0') {
            continue;
        }
        value = (value << 6) | (unsigned int) (d - sDigits);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (unsigned char) (value >> bits);
        }
    }
    return n;
}

/* Every request of the report: the issue, then the attachment comments ({"body":"...```\n<base64 lines>```..."}). */
long bugreport_test_request(const char *body) {
    const char *marker = strstr(body, "dkr-attachment name=");
    char name[64];
    int part, parts, which;
    const char *start, *end;
    char *text;
    size_t n = 0;

    if (marker == NULL) {
        return 201;
    }
    if (sGrowPath != NULL) {
        // The streamed file grows between its two passes and between its parts.
        FILE *f = fopen(sGrowPath, "ab");
        fputs("grown meanwhile\n", f);
        fclose(f);
    }
    sscanf(marker, "dkr-attachment name=%63s part=%d parts=%d", name, &part, &parts);
    which = strcmp(name, "memory.txt") == 0 ? 0 : 1;
    start = strstr(marker, "```\\n") + 5;
    end = strstr(start, "```");
    text = malloc((size_t) (end - start) + 1);
    for (; start < end; start++) {
        if (start[0] == '\\' && start[1] == 'n') {
            start++;
            continue;
        }
        text[n++] = *start;
    }
    text[n] = '\0';
    sParts[which][part - 1] = text;
    sPartCount[which]++;
    sDeclared[which] = parts;
    return 201;
}

static int check(int which, const unsigned char *original, unsigned int size) {
    static char joined[4 << 20];
    static unsigned char packed[3 << 20], unpacked[4 << 20];
    size_t length = 0;
    int i, packedSize;
    z_stream z;

    if (sPartCount[which] != sDeclared[which] || sDeclared[which] == 0) {
        printf("file %d: %d parts arrived, %d declared\n", which, sPartCount[which], sDeclared[which]);
        return 1;
    }
    for (i = 0; i < sDeclared[which]; i++) {
        size_t n = strlen(sParts[which][i]);
        memcpy(joined + length, sParts[which][i], n);
        length += n;
    }
    packedSize = decode_base64(joined, length, packed);
    memset(&z, 0, sizeof(z));
    inflateInit2(&z, 15 + 16);
    z.next_in = packed;
    z.avail_in = (uInt) packedSize;
    z.next_out = unpacked;
    z.avail_out = sizeof(unpacked);
    if (inflate(&z, Z_FINISH) != Z_STREAM_END || z.total_out != size || memcmp(unpacked, original, size) != 0) {
        printf("file %d: %lu bytes back, %u sent, %s\n", which, z.total_out, size,
               z.total_out == size ? "bytes differ" : "sizes differ");
        inflateEnd(&z);
        return 1;
    }
    inflateEnd(&z);
    printf("file %d: %u bytes in %d parts, back intact\n", which, size, sDeclared[which]);
    return 0;
}

int main(void) {
    const char *path = "bugreport_stream_test.tmp";
    unsigned int size = 300000, i, seed = 1;
    unsigned char *data = malloc(size);
    BugReportConfig config = { "token", "o/r", "https://api.example", "label", NULL, NULL, "test" };
    BugReportFile files[2];
    char result[300];
    FILE *f;
    int status, failed;

    // Text that compresses to several comments (as the results files do): lines of numbers with some noise.
    for (i = 0; i < size; i++) {
        seed = seed * 1103515245u + 12345u;
        data[i] = (i % 61 == 60) ? '\n' : (unsigned char) ('0' + ((seed >> 16) % 10)) ;
    }
    f = fopen(path, "wb");
    fwrite(data, 1, size, f);
    fclose(f);
    memset(files, 0, sizeof(files));
    files[0].name = "memory.txt";
    files[0].data = data;
    files[0].size = size;
    files[1].name = "streamed.txt";
    files[1].path = path;
    files[1].size = size;
    sGrowPath = path;
    status = bugreport_send_to(&config, NULL, "title", "body", files, 2, 900 * 1024, NULL, result, sizeof(result));
    remove(path);
    if (status != 0) {
        printf("send failed: %d %s\n", status, result);
        return 1;
    }
    failed = check(0, data, size) | check(1, data, size);
    if (sDeclared[1] < 2) {
        printf("streamed file fit one part; the test needs several\n");
        failed = 1;
    }
    printf("%s\n", failed ? "bug report attachments: FAILED" : "bug report attachments: ok");
    return failed;
}
