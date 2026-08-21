#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "http.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

/* Split "https://host[:port]/path" (or http://) into malloc'd host/path, port,
 * and a tls flag. */
static int url_parse(const char *url, char **host, int *port, char **path, int *tls) {
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        *tls = 1;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
        *tls = 0;
    } else {
        return -1;
    }

    const char *hstart = p;
    while (*p && *p != ':' && *p != '/') p++;
    size_t hlen = (size_t)(p - hstart);
    if (hlen == 0) return -1;
    *host = (char *)malloc(hlen + 1);
    if (!*host) return -1;
    memcpy(*host, hstart, hlen);
    (*host)[hlen] = 0;

    *port = *tls ? 443 : 80;
    if (*p == ':') {
        p++;
        *port = atoi(p);
        while (*p && *p != '/') p++;
    }
    *path = *p == '/' ? strdup(p) : strdup("/");
    if (!*path) { free(*host); return -1; }
    return 0;
}

#ifdef _WIN32

static WCHAR *to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    WCHAR *w = (WCHAR *)malloc(sizeof(WCHAR) * (size_t)n);
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static int post_winhttp(const char *host, int port, const char *path,
                        const char *api_key, const char *body,
                        long *status, char **resp, int tls) {
    HINTERNET hs = WinHttpOpen(L"igor/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) return -1;

    WCHAR *whost = to_wide(host);
    WCHAR *wpath = to_wide(path);
    if (!whost || !wpath) { free(whost); free(wpath); WinHttpCloseHandle(hs); return -1; }

    HINTERNET hc = WinHttpConnect(hs, whost, (INTERNET_PORT)port, 0);
    if (!hc) { free(whost); free(wpath); WinHttpCloseHandle(hs); return -1; }

    DWORD flags = tls ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hr = WinHttpOpenRequest(hc, L"POST", wpath, NULL, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hr) { WinHttpCloseHandle(hc); free(whost); free(wpath); WinHttpCloseHandle(hs); return -1; }

    char headers8[1024];
    snprintf(headers8, sizeof(headers8),
             "Content-Type: application/json\r\nAuthorization: Bearer %s\r\n", api_key);
    WCHAR *wheaders = to_wide(headers8);

    DWORD bodylen = (DWORD)strlen(body);
    BOOL ok = WinHttpSendRequest(hr,
                                 wheaders ? wheaders : WINHTTP_NO_ADDITIONAL_HEADERS,
                                 wheaders ? (DWORD)-1 : 0,
                                 (LPVOID)body, bodylen, bodylen, 0);
    if (!ok || !WinHttpReceiveResponse(hr, NULL)) {
        free(wheaders); WinHttpCloseHandle(hr); WinHttpCloseHandle(hc);
        free(whost); free(wpath); WinHttpCloseHandle(hs); return -1;
    }

    DWORD sc = 0, scsize = sizeof(sc);
    WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &sc, &scsize, WINHTTP_NO_HEADER_INDEX);
    *status = (long)sc;

    size_t cap = 8192, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        free(wheaders); WinHttpCloseHandle(hr); WinHttpCloseHandle(hc);
        free(whost); free(wpath); WinHttpCloseHandle(hs); return -1;
    }

    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hr, &avail) && avail > 0) {
        if (len + avail + 1 > cap) {
            while (len + avail + 1 > cap) cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); free(wheaders); WinHttpCloseHandle(hr); WinHttpCloseHandle(hc); free(whost); free(wpath); WinHttpCloseHandle(hs); return -1; }
            buf = nb;
        }
        DWORD rd = 0;
        if (!WinHttpReadData(hr, buf + len, avail, &rd)) break;
        len += rd;
        if (rd == 0) break;
    }
    buf[len] = 0;
    *resp = buf;

    free(wheaders);
    WinHttpCloseHandle(hr);
    WinHttpCloseHandle(hc);
    free(whost);
    free(wpath);
    WinHttpCloseHandle(hs);
    return 0;
}

#else /* POSIX */

static int header_has(const char *s, const char *end, const char *needle) {
    size_t nl = strlen(needle);
    for (const char *q = s; q + nl <= end; q++) {
        int m = 1;
        for (size_t i = 0; i < nl; i++) {
            char a = q[i];
            char b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) { m = 0; break; }
        }
        if (m) return 1;
    }
    return 0;
}

static char *dechunk(const char *body, size_t len) {
    size_t cap = len + 1, out_len = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    const char *p = body;
    const char *end = body + len;
    while (p < end) {
        size_t sz = 0;
        while (p < end) {
            char c = *p;
            if (c >= '0' && c <= '9') sz = sz * 16 + (size_t)(c - '0');
            else if (c >= 'a' && c <= 'f') sz = sz * 16 + (size_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') sz = sz * 16 + (size_t)(c - 'A' + 10);
            else break;
            p++;
        }
        while (p < end && *p != '\n') p++;
        if (p < end) p++;
        if (sz == 0) break;
        if (out_len + sz + 1 > cap) {
            while (out_len + sz + 1 > cap) cap *= 2;
            char *nb = (char *)realloc(out, cap);
            if (!nb) { free(out); return NULL; }
            out = nb;
        }
        if (p + sz > end) { free(out); return NULL; }
        memcpy(out + out_len, p, sz);
        out_len += sz;
        p += sz;
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;
    }
    out[out_len] = 0;
    return out;
}

static int post_posix(const char *host, int port, const char *path,
                      const char *api_key, const char *body,
                      long *status, char **resp, int tls) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;

    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;
    if (tls) {
        ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) { close(fd); return -1; }
        /* Demo agent: skip certificate verification. */
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
        ssl = SSL_new(ctx);
        if (!ssl) { SSL_CTX_free(ctx); close(fd); return -1; }
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, host);
        if (SSL_connect(ssl) != 1) { SSL_free(ssl); SSL_CTX_free(ctx); close(fd); return -1; }
    }

    char auth[1024];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s\r\n", api_key);
    char reqhdr[4096];
    snprintf(reqhdr, sizeof(reqhdr),
             "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n%s"
             "Content-Length: %zu\r\nConnection: close\r\n\r\n",
             path, host, auth, strlen(body));

    int ok;
    if (tls) {
        ok = SSL_write(ssl, reqhdr, strlen(reqhdr)) > 0 &&
             SSL_write(ssl, body, strlen(body)) > 0;
    } else {
        ok = send(fd, reqhdr, strlen(reqhdr), 0) > 0 &&
             send(fd, body, strlen(body), 0) > 0;
    }
    if (!ok) {
        if (ssl) SSL_free(ssl);
        if (ctx) SSL_CTX_free(ctx);
        close(fd);
        return -1;
    }

    size_t cap = 8192, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        if (ssl) SSL_free(ssl);
        if (ctx) SSL_CTX_free(ctx);
        close(fd);
        return -1;
    }

    char tmp[4096];
    ssize_t n;
    for (;;) {
        n = tls ? SSL_read(ssl, tmp, sizeof(tmp)) : recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        if (len + (size_t)n + 1 > cap) {
            while (len + (size_t)n + 1 > cap) cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); if (ssl) SSL_free(ssl); if (ctx) SSL_CTX_free(ctx); close(fd); return -1; }
            buf = nb;
        }
        memcpy(buf + len, tmp, (size_t)n);
        len += (size_t)n;
    }
    buf[len] = 0;

    char *bodyp = strstr(buf, "\r\n\r\n");
    if (!bodyp) {
        free(buf);
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
        if (ctx) SSL_CTX_free(ctx);
        close(fd);
        return -1;
    }
    bodyp += 4;

    long sc = 0;
    if (strncmp(buf, "HTTP/", 5) == 0) sc = strtol(buf + 9, NULL, 10);
    *status = sc;

    if (header_has(buf, bodyp - 4, "chunked")) {
        *resp = dechunk(bodyp, strlen(bodyp));
    } else {
        *resp = strdup(bodyp);
    }

    free(buf);
    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
    if (ctx) SSL_CTX_free(ctx);
    close(fd);
    return *resp ? 0 : -1;
}

#endif

int http_post(const char *url, const char *api_key, const char *body,
              long *status, char **resp) {
    char *host = NULL, *path = NULL;
    int port = 0, tls = 0;
    if (url_parse(url, &host, &port, &path, &tls) != 0) return -1;

    int rc;
#ifdef _WIN32
    rc = post_winhttp(host, port, path, api_key, body, status, resp, tls);
#else
    rc = post_posix(host, port, path, api_key, body, status, resp, tls);
#endif

    free(host);
    free(path);
    return rc;
}
