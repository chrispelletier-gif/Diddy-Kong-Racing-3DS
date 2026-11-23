/* A stand-in for libcurl, for host tests of the bug report sender (bugreport_stream_test.c). Each request's body is
 * handed to bugreport_test_request(), whose answer is the HTTP status; the response is a fixed issue address. */
#ifndef STUB_CURL_H
#define STUB_CURL_H

#include <stdarg.h>
#include <stddef.h>
#include <string.h>

typedef void CURL;
typedef int CURLcode;
typedef int CURLoption;
typedef int CURLINFO;
#define CURLE_OK 0
#define CURL_ERROR_SIZE 256
#define CURL_BLOB_COPY 1
enum {
    CURLOPT_URL, CURLOPT_HTTPHEADER, CURLOPT_USERAGENT, CURLOPT_WRITEFUNCTION, CURLOPT_WRITEDATA,
    CURLOPT_ERRORBUFFER, CURLOPT_CONNECTTIMEOUT, CURLOPT_NOSIGNAL, CURLOPT_CAINFO, CURLOPT_CAINFO_BLOB,
    CURLOPT_CUSTOMREQUEST, CURLOPT_POSTFIELDS, CURLOPT_POSTFIELDSIZE, CURLOPT_TIMEOUT
};
#define CURLINFO_RESPONSE_CODE 0
struct curl_slist {
    int unused;
};
struct curl_blob {
    void *data;
    size_t len;
    unsigned int flags;
};

long bugreport_test_request(const char *body);

typedef size_t (*StubWrite)(char *, size_t, size_t, void *);
static StubWrite sStubWrite;
static void *sStubWriteData;
static const char *sStubBody;
static long sStubStatus;
static int sStubCurl;

static inline CURL *curl_easy_init(void) {
    return &sStubCurl;
}
static inline void curl_easy_reset(CURL *curl) {
    (void) curl;
}
static inline void curl_easy_cleanup(CURL *curl) {
    (void) curl;
}
static inline struct curl_slist *curl_slist_append(struct curl_slist *list, const char *text) {
    static struct curl_slist sList;
    (void) list;
    (void) text;
    return &sList;
}
static inline void curl_slist_free_all(struct curl_slist *list) {
    (void) list;
}
static inline CURLcode curl_easy_setopt(CURL *curl, int option, ...) {
    va_list args;
    (void) curl;
    va_start(args, option);
    if (option == CURLOPT_WRITEFUNCTION) {
        sStubWrite = va_arg(args, StubWrite);
    } else if (option == CURLOPT_WRITEDATA) {
        sStubWriteData = va_arg(args, void *);
    } else if (option == CURLOPT_POSTFIELDS) {
        sStubBody = va_arg(args, const char *);
    }
    va_end(args);
    return CURLE_OK;
}
static inline CURLcode curl_easy_perform(CURL *curl) {
    static char sAnswer[] = "{\"html_url\":\"https://github.com/o/r/issues/7\"}";
    (void) curl;
    sStubStatus = bugreport_test_request(sStubBody);
    sStubWrite(sAnswer, 1, strlen(sAnswer), sStubWriteData);
    return CURLE_OK;
}
static inline CURLcode curl_easy_getinfo(CURL *curl, int info, long *out) {
    (void) curl;
    (void) info;
    *out = sStubStatus;
    return CURLE_OK;
}
static inline const char *curl_easy_strerror(CURLcode code) {
    (void) code;
    return "stub";
}
#endif
