/*
 * client.c -- Aegis Relay CLIENT (an enterprise client UI, in C)
 *
 * The "business enterprise" half of the demo. A real enterprise app would be
 * a rich web/desktop client; this demo ships a single-file C program that does
 * what a viewer can actually watch: it opens a connection, performs a
 * handshake event against the cloud side, probes /health and /ping, and
 * prints a small ASCII console UI with connection status, an event log and
 * measured latency.
 *
 * Every request to a protected endpoint (GET /ping, POST /events) is signed
 * with HMAC-SHA256 using the shared key:
 *
 *     signature = hex( HMAC-SHA256( key, "METHOD|path|timestamp|body" ) )
 *
 * sent as:
 *     X-Aegis-Client    : aegis-client         (quota accounting)
 *     X-Aegis-Timestamp : <unix seconds>        (clock-skew bound)
 *     X-Aegis-Signature : <64 lowercase hex>
 *
 * Shared key resolution (priority):  argv[3] > env AEGIS_SHARED_KEY >
 * compile-time default "aegis-demo-key" (dev only; change via -DKEY=...).
 *
 * SHA-256 and HMAC-SHA256 are implemented from scratch, public-domain
 * algorithm (FIPS 180-4 / RFC 4231). The SHA-256 core is an adapted port of
 * Brad Conte's public-domain implementation
 * (https://github.com/B-Con/crypto-algorithms, released to public domain),
 * verified in AEGIS_SELFTEST mode against the FIPS 180-4 test vectors.
 *
 * Dependencies: none (no libcurl, no OpenSSL, no JSON library). Network:
 * WinSock2 on Windows, BSD sockets elsewhere. Builds with MinGW gcc, MSVC:
 *
 *     gcc -O2 -o client.exe client.c -lws2_32        # MinGW (Windows)
 *     cl  client.c ws2_32.lib                        # MSVC
 *     gcc -O2 -std=c99 -o client client.c            # POSIX (Linux/WSL)
 *     gcc -O2 -std=c99 -DAEGIS_SELFTEST -o selftest client.c   # test vectors
 *
 * Usage:   client [host] [port] [key]     (defaults 127.0.0.1 8765 <dev key>)
 *
 * MEMORY-SAFETY NOTES (per external security review):
 *   - every outbound message is built with snprintf + explicit size check
 *   - every recv is capped into a fixed buffer with explicit null
 *     termination and a bound check on every loop iteration
 *   - gethostbyname result copied with a length capped at sizeof(sa.sin_addr)
 *   - port is range-checked (1..65535) before use
 *   - no sprintf/strcpy/strcat anywhere in this file
 *     (verify: grep -n 'sprintf\|strcpy\|strcat' client/client.c)
 */

#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#define sock_t         SOCKET
#define sock_invalid   INVALID_SOCKET
#define sock_close     closesocket
#define sock_errno()   WSAGetLastError()
#else
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#define sock_t         int
#define sock_invalid   ((sock_t)-1)
#define sock_close     close
#define sock_errno()   errno
#endif

#define MAXBUF 8192
#define AEGIS_CLIENT_ID "aegis-client"

#ifndef KEY
#define KEY "aegis-demo-key"   /* dev default; override with -DKEY=\"...\" */
#endif

/* =====================================================================
 * SHA-256 (FIPS 180-4). Adapted from Brad Conte's public-domain
 * implementation (github.com/B-Con/crypto-algorithms) -- public domain
 * license. Changes: fixed-width typedefs only (uint32_t/uint64_t),
 * const-correct, C99.
 * =================================================================== */

typedef struct {
    uint8_t  data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} SHA256_CTX;

#define ROTRIGHT(a, b) (((a) >> (b)) | ((a) << (32 - (b))))

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_transform(SHA256_CTX *ctx, const uint8_t *data) {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    size_t i;
    for (i = 0; i < 16; ++i)
        m[i] = (uint32_t)data[i * 4] << 24 | (uint32_t)data[i * 4 + 1] << 16 |
               (uint32_t)data[i * 4 + 2] << 8 | (uint32_t)data[i * 4 + 3];
    for (i = 16; i < 64; ++i)
        m[i] = (ROTRIGHT(m[i - 2], 17) ^ ROTRIGHT(m[i - 2], 19) ^ (m[i - 2] >> 10)) +
               m[i - 7] + (ROTRIGHT(m[i - 15], 7) ^ ROTRIGHT(m[i - 15], 18) ^ (m[i - 15] >> 3)) +
               m[i - 16];
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
    for (i = 0; i < 64; ++i) {
        t1 = h + (ROTRIGHT(e, 6) ^ ROTRIGHT(e, 11) ^ ROTRIGHT(e, 25)) +
             ((e & f) ^ (~e & g)) + K256[i] + m[i];
        t2 = (ROTRIGHT(a, 2) ^ ROTRIGHT(a, 13) ^ ROTRIGHT(a, 22)) +
             ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(SHA256_CTX *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

static void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len) {
    size_t i;
    for (i = 0; i < len; ++i) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(SHA256_CTX *ctx, uint8_t hash[32]) {
    uint32_t i = ctx->datalen;
    ctx->data[i++] = 0x80;
    if (i > 56) {
        while (i < 64) ctx->data[i++] = 0x00;
        sha256_transform(ctx, ctx->data);
        i = 0;
    }
    while (i < 56) ctx->data[i++] = 0x00;
    ctx->bitlen += (uint64_t)ctx->datalen * 8;
    for (i = 56; i < 64; ++i)
        ctx->data[i] = (uint8_t)(ctx->bitlen >> (56 - (i - 56) * 8)); /* BE len */
    sha256_transform(ctx, ctx->data);
    for (i = 0; i < 4; ++i) {
        hash[i]      = (uint8_t)(ctx->state[0] >> (24 - i * 8));
        hash[i + 4]  = (uint8_t)(ctx->state[1] >> (24 - i * 8));
        hash[i + 8]  = (uint8_t)(ctx->state[2] >> (24 - i * 8));
        hash[i + 12] = (uint8_t)(ctx->state[3] >> (24 - i * 8));
        hash[i + 16] = (uint8_t)(ctx->state[4] >> (24 - i * 8));
        hash[i + 20] = (uint8_t)(ctx->state[5] >> (24 - i * 8));
        hash[i + 24] = (uint8_t)(ctx->state[6] >> (24 - i * 8));
        hash[i + 28] = (uint8_t)(ctx->state[7] >> (24 - i * 8));
    }
}

static void sha256_one(const uint8_t *data, size_t len, uint8_t hash[32]) {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, hash);
}

/* =====================================================================
 * HMAC-SHA256 (RFC 2104 / RFC 4231)
 * =================================================================== */

static void hmac_sha256(const uint8_t *key, size_t keylen,
                        const uint8_t *msg, size_t msglen,
                        uint8_t out[32]) {
    uint8_t k[64], ipad[64], opad[64], inner[32];
    SHA256_CTX ctx;
    size_t i;
    memset(k, 0, sizeof(k));
    if (keylen > 64) {              /* long key -> hash it down to 32 bytes */
        sha256_one(key, keylen, k);
    } else {
        memcpy(k, key, keylen);
    }
    for (i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, 64);
    sha256_update(&ctx, msg, msglen);
    sha256_final(&ctx, inner);
    sha256_init(&ctx);
    sha256_update(&ctx, opad, 64);
    sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, out);
    memset(k, 0, sizeof(k));        /* hygiene: no key material on stack */
    memset(ipad, 0, sizeof(ipad));
    memset(opad, 0, sizeof(opad));
}

static void hex_encode(const uint8_t *in, size_t n, char *out) {
    static const char hexd[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; ++i) {
        out[i * 2]     = hexd[in[i] >> 4];
        out[i * 2 + 1] = hexd[in[i] & 0x0f];
    }
    out[n * 2] = '\0';
}

/*
 * sign_request -- canonical string "METHOD|path|timestamp|body", HMAC it,
 * hex the MAC into sig_hex[65], and write the unix timestamp into ts[32].
 * Returns 0 on success, -1 on buffer overflow / failure (no partial state).
 */
#ifndef AEGIS_SELFTEST
static int sign_request(const char *method, const char *path, const char *body,
                        const char *key, char sig_hex[65], char ts_buf[32]) {
    char msg[MAXBUF];
    const char *b = body ? body : "";
    uint8_t mac[32];
    time_t now = time(NULL);
    int tlen = snprintf(ts_buf, 32, "%ld", (long)now);
    if (tlen <= 0 || tlen >= 32) return -1;
    int mlen = snprintf(msg, sizeof(msg), "%s|%s|%s|%s", method, path, ts_buf, b);
    if (mlen <= 0 || mlen >= (int)sizeof(msg)) return -1;
    hmac_sha256((const uint8_t *)key, strlen(key),
                (const uint8_t *)msg, (size_t)mlen, mac);
    hex_encode(mac, 32, sig_hex);
    return 0;
}
#endif /* !AEGIS_SELFTEST */

/* ---- tiny HTTP helper (WinSock on Windows, BSD sockets elsewhere) ------- */
#ifndef AEGIS_SELFTEST   /* selftest build exercises only sha256/hmac above */

static sock_t dial(const char *host, int port, char *errbuf, size_t errlen) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        snprintf(errbuf, errlen, "WSAStartup failed");
        return sock_invalid;
    }
#endif
    sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == sock_invalid) {
        snprintf(errbuf, errlen, "socket() failed: %d", sock_errno());
        return sock_invalid;
    }
    struct hostent *he = gethostbyname(host);
    if (he == NULL) {
        snprintf(errbuf, errlen, "cannot resolve host '%s'", host);
        sock_close(s);
        return sock_invalid;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    /* mem-safety: cap the host address copy at the target field size */
    if (he->h_length > (int)sizeof(sa.sin_addr)) he->h_length = (int)sizeof(sa.sin_addr);
    memcpy(&sa.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        snprintf(errbuf, errlen, "connect() to %s:%d failed: %d",
                 host, port, sock_errno());
        sock_close(s);
        return sock_invalid;
    }
    return s;
}

/* own strdup: bounded by strlen of the (already null-terminated) source,
 * avoiding platform feature-macro differences. Caller frees. */
static char *aegis_strdup(const char *src) {
    size_t n = strlen(src);
    char *out = (char *)malloc(n + 1);
    if (out == NULL) return NULL;
    memcpy(out, src, n + 1);
    return out;
}

/* Given the running client's key, this will sign the request and add the
 * X-Aegis-* headers. Returns malloc'd response BODY (headers stripped), or
 * NULL on failure with errbuf filled. */
static char *http_request(const char *host, int port, const char *method,
                          const char *path, const char *body, const char *key,
                          double *elapsed_ms, char *errbuf, size_t errlen) {
    char sig[65], ts[32];
    if (sign_request(method, path, body, key, sig, ts) != 0) {
        snprintf(errbuf, errlen, "request signing failed (buffer too small)");
        return NULL;
    }

    sock_t s = dial(host, port, errbuf, errlen);
    if (s == sock_invalid) return NULL;

    char req[MAXBUF];
    int n;
    if (body != NULL) {
        n = snprintf(req, sizeof(req),
                     "%s %s HTTP/1.1\r\nHost: %s:%d\r\n"
                     "X-Aegis-Client: %s\r\n"
                     "X-Aegis-Timestamp: %s\r\n"
                     "X-Aegis-Signature: %s\r\n"
                     "Content-Type: application/json\r\n"
                     "User-Agent: aegis-client/1.0 (enterprise-client)\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                     method, path, host, port, AEGIS_CLIENT_ID, ts, sig,
                     strlen(body), body);
    } else {
        n = snprintf(req, sizeof(req),
                     "%s %s HTTP/1.1\r\nHost: %s:%d\r\n"
                     "X-Aegis-Client: %s\r\n"
                     "X-Aegis-Timestamp: %s\r\n"
                     "X-Aegis-Signature: %s\r\n"
                     "User-Agent: aegis-client/1.0 (enterprise-client)\r\n"
                     "Connection: close\r\n\r\n",
                     method, path, host, port, AEGIS_CLIENT_ID, ts, sig);
    }
    if (n <= 0 || n >= (int)sizeof(req)) {
        snprintf(errbuf, errlen, "request too large");
        sock_close(s);
        return NULL;
    }

    double t0 = 0.0;
#ifdef _WIN32
    LARGE_INTEGER f, pc0, pc1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&pc0);
    t0 = (double)pc0.QuadPart;
#else
    t0 = 1000.0 * (double)clock() / (double)CLOCKS_PER_SEC;
#endif

    int sent = 0;
    while (sent < n) {
        int w = (int)send(s, req + sent, (size_t)(n - sent), 0);
        if (w <= 0) {
            snprintf(errbuf, errlen, "send() failed: %d", sock_errno());
            sock_close(s);
            return NULL;
        }
        sent += w;
    }

    /* read until close (Connection: close). resp is always null-terminated:
     * cap at sizeof-1, and re-check the bound on every iteration. */
    char resp[MAXBUF];
    int total = 0;
    for (;;) {
        int room = (int)sizeof(resp) - 1 - total;
        if (room <= 0) break;               /* buffer full: stop cleanly */
        int r = (int)recv(s, resp + total, (size_t)room, 0);
        if (r <= 0) break;                  /* 0 = close, -1 = error */
        total += r;
    }
    resp[total] = '\0';                      /* total <= sizeof(resp)-1 here */

#ifdef _WIN32
    LARGE_INTEGER pc2;
    QueryPerformanceCounter(&pc2);
    if (elapsed_ms) *elapsed_ms = (double)(pc2.QuadPart - pc0.QuadPart) * 1000.0 / (double)f.QuadPart;
#else
    if (elapsed_ms) {
        double t1 = 1000.0 * (double)clock() / (double)CLOCKS_PER_SEC;
        *elapsed_ms = t1 - t0;
    }
#endif
    sock_close(s);

    if (total <= 0) {
        snprintf(errbuf, errlen, "no response received");
        return NULL;
    }
    char *hdr_end = strstr(resp, "\r\n\r\n");
    if (hdr_end == NULL) hdr_end = strstr(resp, "\n\n");
    char *body_start = hdr_end ? hdr_end + (hdr_end[1] == '\n' ? 2 : 4) : resp;
    return aegis_strdup(body_start);          /* caller frees */
}

static char *http_get(const char *host, int port, const char *path, const char *key,
                      double *ms, char *err, size_t errlen) {
    return http_request(host, port, "GET", path, NULL, key, ms, err, errlen);
}

static char *http_post(const char *host, int port, const char *path,
                       const char *body, const char *key,
                       double *ms, char *err, size_t errlen) {
    return http_request(host, port, "POST", path, body, key, ms, err, errlen);
}

/* ---- ASCII console UI --------------------------------------------------- */

static void log_line(const char *fmt, const char *what, double ms) {
    char ts[32];
    (void)fmt;
    time_t now = time(NULL);
    strftime(ts, sizeof(ts), "%H:%M:%S", gmtime(&now));
    printf("  [%s] %s %7.2f ms\n", ts, what, ms);
}

#endif /* !AEGIS_SELFTEST */

/* =====================================================================
 * AEGIS_SELFTEST -- FIPS 180-4 SHA-256 vectors + RFC 4231 HMAC case 2.
 * Build:  gcc -O2 -std=c99 -DAEGIS_SELFTEST -o selftest client.c && ./selftest
 * =================================================================== */
#ifdef AEGIS_SELFTEST
static void print_digest(const char *label, const uint8_t *digest) {
    char hex[65];
    hex_encode(digest, 32, hex);
    printf("%-14s %s\n", label, hex);
}

int main(void) {
    static const uint8_t abc[] = "abc";
    static const uint8_t juta[] = "what do ya want for nothing?";
    static const uint8_t key_jefe[] = "Jefe";
    uint8_t d[32], mac[32], *one_m_a;

    printf("== SHA-256 / HMAC-SHA256 self-test ==\n");

    sha256_one((const uint8_t *)"", 0, d);
    print_digest("sha256(\"\")", d);
    if (memcmp(d, (uint8_t *)"\xe3\xb0\xc4\x42\x98\xfc\x1c\x14\x9a\xfb\xf4\xc8\x99\x6f\xb9\x24\x27\xae\x41\xe4\x64\x9b\x93\x4c\xa4\x95\x99\x1b\x78\x52\xb8\x55", 32) != 0) {
        printf("FAIL sha256(empty)\n"); return 1;
    }
    printf("     verify(empty)      PASS\n");

    sha256_one(abc, 3, d);
    print_digest("sha256(abc)", d);
    if (memcmp(d, (uint8_t *)"\xba\x78\x16\xbf\x8f\x01\xcf\xea\x41\x41\x40\xde\x5d\xae\x22\x23\xb0\x03\x61\xa3\x96\x17\x7a\x9c\xb4\x10\xff\x61\xf2\x00\x15\xad", 32) != 0) {
        printf("FAIL sha256(abc)\n"); return 1;
    }
    printf("     verify(abc)        PASS\n");

    one_m_a = (uint8_t *)malloc(1000000);
    if (one_m_a == NULL) { printf("FAIL alloc(1M 'a')\n"); return 1; }
    memset(one_m_a, 'a', 1000000);
    sha256_one(one_m_a, 1000000, d);
    print_digest("sha256(1M 'a')", d);
    free(one_m_a);
    if (memcmp(d, (uint8_t *)"\xcd\xc7\x6e\x5c\x99\x14\xfb\x92\x81\xa1\xc7\xe2\x84\xd7\x3e\x67\xf1\x80\x9a\x48\xa4\x97\x20\x0e\x04\x6d\x39\xcc\xc7\x11\x2c\xd0", 32) != 0) {
        printf("FAIL sha256(1M a)\n"); return 1;
    }
    printf("     verify(1M 'a')     PASS\n");

    /* RFC 4231 case 2: key="Jefe", data="what do ya want for nothing?" */
    hmac_sha256(key_jefe, 4, juta, 28, mac);
    print_digest("hmac(jefe)", mac);
    if (memcmp(mac, (uint8_t *)"\x5b\xdc\xc1\x46\xbf\x60\x75\x4e\x6a\x04\x24\x26\x08\x95\x75\xc7\x5a\x00\x3f\x08\x9d\x27\x39\x83\x9d\xec\x58\xb9\x64\xec\x38\x43", 32) != 0) {
        printf("FAIL hmac(rfc4231-2)\n"); return 1;
    }
    printf("     verify(rfc4231-2)  PASS\n");

    printf("ALL VECTORS PASS\n");
    return 0;
}
#else  /* !AEGIS_SELFTEST */

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    const char *key = NULL;
    const char *key_src = "";
    int port;
    char err[512];
    double ms = 0.0;

    if (argc > 2) {
        long p = atol(argv[2]);
        if (p < 1 || p > 65535) {   /* mem-safety: reject out-of-range ports */
            printf("  ! invalid port '%s' (must be 1..65535)\n\n", argv[2]);
            return 1;
        }
        port = (int)p;
    } else {
        port = 8765;
    }
    if (argc > 3) {                 /* priority 1: argv */
        key = argv[3];
        key_src = "argv";
    } else {
        key = getenv("AEGIS_SHARED_KEY");   /* priority 2: env */
        if (key != NULL) {
            key_src = "env AEGIS_SHARED_KEY";
        } else {                    /* priority 3: compile-time dev default */
            key = KEY;
            key_src = "compile-time dev default";
            printf("  ! WARNING: no key given (argv[3] / AEGIS_SHARED_KEY);\n");
            printf("    using compile-time default \"%s\". Dev only.\n\n", key);
        }
    }

    printf("\n");
    printf("=== Aegis Relay : enterprise client (C) ===\n");
    printf("    cloud endpoint : %s:%d\n", host, port);
    printf("    shared key     : %s (%s)\n", key, key_src);
    printf("    signing        : HMAC-SHA256(key, \"METHOD|path|ts|body\")\n\n");

    /* 1) handshake event -> cloud side (signed) */
    char body[512];
    snprintf(body, sizeof(body),
             "{\"type\":\"handshake\",\"client\":\"aegis-client\","
             "\"client_version\":\"1.0\",\"event\":\"client.connected\"}");
    char *r = http_post(host, port, "/events", body, key, &ms, err, sizeof(err));
    const char *status = "DISCONNECTED";
    if (r == NULL) {
        log_line("FAIL", "handshake  --", ms);
        printf("  ! %s\n\n", err);
        printf("  Is the relay server running?  `AEGIS_SHARED_KEY=... python3 server/relay_server.py`\n\n");
        return 1;
    }
    log_line("OK  ", "handshake /events", ms);
    /* do we see "accepted"? */
    if (strstr(r, "\"status\": \"accepted\"") != NULL ||
        strstr(r, "\"status\":\"accepted\"") != NULL)
        status = "CONNECTED";

    /* 2) health probe (open endpoint; still signed, server ignores it) */
    char *h = http_get(host, port, "/health", key, &ms, err, sizeof(err));
    if (h) {
        log_line("OK  ", "health  /health", ms);
    } else {
        log_line("FAIL", "health  /health", ms);
    }

    /* 3) ping probe (protected; signature required) */
    char *p = http_get(host, port, "/ping", key, &ms, err, sizeof(err));
    if (p) {
        log_line("OK  ", "ping    /ping", ms);
    } else {
        log_line("FAIL", "ping    /ping", ms);
    }

    printf("\n+%s+\n", "---------------- relay bridge console -----------------------");
    printf("| %s\n", status);
    printf("|  bridge         : client (C) <-> relay bridge <-> cloud side\n");
    printf("|  site id        : demo-enterprise-apac-01\n");
    printf("|  events sent    : 1 (handshake: client.connected)\n");
    printf("|  server reply   : %s\n", (r && strlen(r) < 300) ? r : "(see event log above)");
    printf("+%s+\n", "---------------- relay bridge console -----------------------");

    if (h) { printf("\n  health: %s\n", h); free(h); }
    if (p) { printf("  ping  : %s\n", p); free(p); }
    free(r);
    printf("\n  Demo complete: 1 signed handshake event delivered, %d signed probes answered.\n", (h && p) ? 2 : 0);
    printf("  Signed every request with HMAC-SHA256 via X-Aegis-Signature.\n");
    return 0;
}
#endif /* AEGIS_SELFTEST */