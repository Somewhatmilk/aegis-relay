/*
 * fake_webui.c -- relay-bridge-demo CLIENT (a pretend enterprise WebUI, in C)
 *
 * The "business enterprise" half of the demo. A real enterprise app would be
 * a rich web/desktop client; this demo ships a single-file C program that does
 * what a viewer can actually watch: it opens a connection, performs a
 * handshake event against the cloud side, probes /health and /ping, and
 * prints a small ASCII console UI with connection status, an event log and
 * measured latency.
 *
 * Dependencies: plain WinSock2 (ws2_32). No libcurl, no JSON library, no
 * frameworks. Builds with MinGW gcc or MSVC cl:
 *
 *     gcc -O2 -o fake_webui.exe fake_webui.c -lws2_32
 *     cl  fake_webui.c ws2_32.lib
 *
 * Usage:   fake_webui.exe [host] [port]      (defaults 127.0.0.1 8765)
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _MSC_VER
#define DEMO_STRDUP _strdup
#else
#define DEMO_STRDUP strdup
#endif

#define MAXBUF 8192

/* ---- tiny HTTP helper over WinSock ------------------------------------ */

static SOCKET dial(const char *host, int port, char *errbuf, size_t errlen) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        snprintf(errbuf, errlen, "WSAStartup failed");
        return INVALID_SOCKET;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        snprintf(errbuf, errlen, "socket() failed: %d", WSAGetLastError());
        return INVALID_SOCKET;
    }
    struct hostent *he = gethostbyname(host);
    if (he == NULL) {
        snprintf(errbuf, errlen, "cannot resolve host '%s'", host);
        closesocket(s);
        return INVALID_SOCKET;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    memcpy(&sa.sin_addr, he->h_addr, he->h_length);
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == SOCKET_ERROR) {
        snprintf(errbuf, errlen, "connect() to %s:%d failed: %d",
                 host, port, WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

/* Returns malloc'd response BODY (headers stripped), or NULL on failure,
 * with errbuf filled. */
static char *http_request(const char *host, int port, const char *method,
                          const char *path, const char *body,
                          double *elapsed_ms, char *errbuf, size_t errlen) {
    SOCKET s = dial(host, port, errbuf, errlen);
    if (s == INVALID_SOCKET) return NULL;

    char req[MAXBUF];
    int n;
    if (body != NULL) {
        n = snprintf(req, sizeof(req),
                     "%s %s HTTP/1.1\r\nHost: %s:%d\r\n"
                     "Content-Type: application/json\r\n"
                     "User-Agent: fake-webui/1.0 (enterprise-client demo)\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                     method, path, host, port, strlen(body), body);
    } else {
        n = snprintf(req, sizeof(req),
                     "%s %s HTTP/1.1\r\nHost: %s:%d\r\n"
                     "User-Agent: fake-webui/1.0 (enterprise-client demo)\r\n"
                     "Connection: close\r\n\r\n",
                     method, path, host, port);
    }
    if (n <= 0 || n >= (int)sizeof(req)) {
        snprintf(errbuf, errlen, "request too large");
        closesocket(s);
        return NULL;
    }

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    int sent = 0;
    while (sent < n) {
        int w = send(s, req + sent, n - sent, 0);
        if (w <= 0) {
            snprintf(errbuf, errlen, "send() failed: %d", WSAGetLastError());
            closesocket(s);
            return NULL;
        }
        sent += w;
    }

    /* read until close (Connection: close) */
    char resp[MAXBUF];
    int total = 0;
    for (;;) {
        int r = recv(s, resp + total, (int)sizeof(resp) - 1 - total, 0);
        if (r <= 0) break;               /* 0 = close, -1 = error */
        total += r;
        if (total >= (int)sizeof(resp) - 1) break;
    }
    QueryPerformanceCounter(&t1);
    if (elapsed_ms) *elapsed_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    closesocket(s);

    resp[total] = '\0';
    if (total <= 0) {
        snprintf(errbuf, errlen, "no response received");
        return NULL;
    }
    char *hdr_end = strstr(resp, "\r\n\r\n");
    if (hdr_end == NULL) hdr_end = strstr(resp, "\n\n");
    char *body_start = hdr_end ? hdr_end + (hdr_end[1] == '\n' ? 2 : 4) : resp;
    return DEMO_STRDUP(body_start); /* caller frees */
}

static char *http_get(const char *host, int port, const char *path,
                      double *ms, char *err, size_t errlen) {
    return http_request(host, port, "GET", path, NULL, ms, err, errlen);
}

static char *http_post(const char *host, int port, const char *path,
                       const char *body, double *ms, char *err, size_t errlen) {
    return http_request(host, port, "POST", path, body, ms, err, errlen);
}

/* ---- ASCII console UI --------------------------------------------------- */

static void log_line(const char *fmt, const char *what, double ms) {
    char ts[32];
    (void)fmt;
    time_t now = time(NULL);
    strftime(ts, sizeof(ts), "%H:%M:%S", gmtime(&now));
    printf("  [%s] %s %7.2f ms\n", ts, what, ms);
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : 8765;
    char err[512];
    double ms = 0.0;

    printf("\n");
    printf("=== relay-bridge-demo : fake enterprise WebUI client (C) ===\n");
    printf("    cloud endpoint : %s:%d\n\n", host, port);

    /* 1) handshake event -> cloud side */
    char body[512];
    snprintf(body, sizeof(body),
             "{\"type\":\"handshake\",\"client\":\"fake-webui\","
             "\"client_version\":\"1.0\",\"event\":\"client.connected\"}");
    char *r = http_post(host, port, "/events", body, &ms, err, sizeof(err));
    const char *status = "DISCONNECTED";
    if (r == NULL) {
        log_line("FAIL", "handshake  --", ms); /* ms may be undefined; keep simple */
        printf("  ! %s\n\n", err);
        printf("  Is the fake cloud server running?  `python3 server/fake_server.py`\n\n");
        return 1;
    }
    log_line("OK  ", "handshake /events", ms);
    /* do we see "accepted"? */
    if (strstr(r, "\"status\": \"accepted\"") != NULL ||
        strstr(r, "\"status\":\"accepted\"") != NULL)
        status = "CONNECTED";

    /* 2) health probe */
    char *h = http_get(host, port, "/health", &ms, err, sizeof(err));
    if (h) {
        log_line("OK  ", "health  /health", ms);
    } else {
        log_line("FAIL", "health  /health", ms);
    }

    /* 3) ping probe */
    char *p = http_get(host, port, "/ping", &ms, err, sizeof(err));
    if (p) {
        log_line("OK  ", "ping    /ping", ms);
    } else {
        log_line("FAIL", "ping    /ping", ms);
    }

    printf("\n+%s+\n", "---------------- relay bridge console -----------------------");
    printf("| %s\n", status);
    printf("|  bridge         : fake_webui.exe (C) <-> relay bridge <-> fake cloud\n");
    printf("|  site id        : demo-enterprise-apac-01\n");
    printf("|  events sent    : 1 (handshake: client.connected)\n");
    printf("|  server reply   : %s\n", (r && strlen(r) < 300) ? r : "(see event log above)");
    printf("+%s+\n", "---------------- relay bridge console -----------------------");

    if (h) { printf("\n  health: %s\n", h); free(h); }
    if (p) { printf("  ping  : %s\n", p); free(p); }
    free(r);
    printf("\n  Demo complete: 1 handshake event delivered, %d probes answered.\n", (h && p) ? 2 : 0);
    printf("  Next:  build.sh then run `dist/fake_webui.exe` -- see README.md.\n");
    return 0;
}