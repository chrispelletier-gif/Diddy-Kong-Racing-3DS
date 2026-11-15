/* The libcurl setup the bug report (bugreport.c) and the updater (3ds/update_3ds.c) share (header only). */
#ifndef PORT_CURL_COMMON_H
#define PORT_CURL_COMMON_H

#include <curl/curl.h>
#include <string.h>

/* The request's address, headers, user agent, where the response goes, the error text, the timeout, and the root
 * certificates: caFile (a file) if set, otherwise caPem (the PEM text itself) if set, otherwise libcurl's default. */
static inline void curl_common_setup(CURL *curl, const char *url, struct curl_slist *headers, const char *userAgent,
                                     size_t (*onData)(char *, size_t, size_t, void *), void *userData,
                                     char *errorBuffer, const char *caFile, const char *caPem) {
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, userData);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (caFile != NULL) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, caFile);
    } else if (caPem != NULL) {
        struct curl_blob blob;
        blob.data = (void *) caPem;
        blob.len = strlen(caPem);
        blob.flags = CURL_BLOB_COPY;
        curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
    }
}

#endif
