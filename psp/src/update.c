#include "update.h"

// Links and messages are cut to fit their buffers on purpose
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <pspkernel.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <pspthreadman.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "audio.h"
#include "certs.h"
#include "net.h"
#include "settings.h"
#include "ui.h"
#include "version.h"

#define REPO "retcon26/retcons-mod" // GitHub forwards the old name, psp-sandbox
#define ASSET "EBOOT.PBP"
#define LOG_PATH "data/update.log"
#define JSON_MAX (64 * 1024) // the release's JSON; its assets come before the long notes
#define URL_MAX 4096         // GitHub's download links carry a long signature
#define STALL_US 30000000    // nothing sent or received this long: give up
#define MAX_REDIRECTS 5

#define C_TEXT 0xFF303030
#define C_DIM 0xFF707070
#define C_GOOD 0xFF30A030
#define C_BAD 0xFF3030C0
#define C_BAR 0xFFE6A03D

typedef enum {
    ST_IDLE,
    ST_IN_GAME,     // a game is on: check from the main menu
    ST_WIFI,        // connecting to the access point
    ST_CHECKING,    // asking GitHub (the worker)
    ST_LATEST,      // nothing newer
    ST_AVAILABLE,   // a newer engine: X installs it
    ST_NEEDS_DATA,  // newer, but it needs data built on a PC
    ST_DOWNLOADING, // the worker
    ST_INSTALLED,
    ST_FAILED,      // msg says why (or what a test download got)
    ST_STOPPING,    // backing out: waiting for the worker to stop
} State;

enum { JOB_CHECK, JOB_DOWNLOAD, JOB_GET };

static struct {
    char eboot[256]; // the EBOOT.PBP the update replaces
    int in_game;
    volatile int state;
    volatile int busy;    // the worker is running
    volatile int cancel;  // 1: Circle, 2: stalled
    volatile int sock;    // the worker's socket, so a stall or Circle can cut it off
    volatile SceInt64 last_io;
    volatile int got;     // bytes downloaded
    SceUID thread;
    int job, next_job;    // next_job: what starts once Wi-Fi is up
    int net_ours;         // this page started the Wi-Fi
    char msg[192];
    // The newer release
    char tag[32], title[64];
    char url[URL_MAX];
    int size, data, have_sha;
    unsigned char sha[32];
    char notes[640];
    // Tests (autoload.txt)
    char repo[64], pretend[24], get_url[256], asset[96];
    int autoget;
} U = {.thread = -1, .sock = -1};

// --- Log -----------------------------------------------------------------------------------------

static void ulog(const char *fmt, ...) {
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// Sets the message shown and logs it. Returns 0, for "return fail(...)".
static int fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(U.msg, sizeof(U.msg), fmt, ap);
    va_end(ap);
    ulog("error: %s", U.msg);
    return 0;
}

static const char *current_version(void) {
    return U.pretend[0] ? U.pretend : GAME_VERSION;
}

// "v1.9.0" or "1.9" -> 1009000; 0 if it isn't a version
static int version_number(const char *s) {
    int a = 0, b = 0, c = 0;
    if (*s == 'v' || *s == 'V') s++;
    if (sscanf(s, "%d.%d.%d", &a, &b, &c) < 1) return 0;
    return a * 1000000 + b * 1000 + c;
}

// --- TLS -----------------------------------------------------------------------------------------

static mbedtls_x509_crt g_ca;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_ssl_config g_conf;

static void touch(void) {
    U.last_io = sceKernelGetSystemTimeWide();
}

// Seeds the random generator. User mode can't read the PSP's hardware random source, so this hashes timer
// jitter around thread switches with the clock and our address. That's weak, but only privacy depends on
// it: whether the download is genuine rests on GitHub's certificate, which an attacker can't sign.
static int gather_entropy(void *p, unsigned char *out, size_t len) {
    static uint32_t counter;
    size_t done = 0;
    while (done < len) {
        mbedtls_sha256_context h;
        unsigned char d[32];
        mbedtls_sha256_init(&h);
        mbedtls_sha256_starts_ret(&h, 0);
        for (int i = 0; i < 24; i++) {
            SceInt64 t = sceKernelGetSystemTimeWide();
            mbedtls_sha256_update_ret(&h, (const unsigned char *)&t, sizeof(t));
            sceKernelDelayThread(i & 3); // gives way: the wake-up comes back at a slightly random time
        }
        time_t now = time(NULL);
        counter++;
        mbedtls_sha256_update_ret(&h, (const unsigned char *)&now, sizeof(now));
        mbedtls_sha256_update_ret(&h, (const unsigned char *)&counter, sizeof(counter));
        mbedtls_sha256_update_ret(&h, net_my_mac(), 6);
        mbedtls_sha256_finish_ret(&h, d);
        mbedtls_sha256_free(&h);
        size_t n = len - done < sizeof(d) ? len - done : sizeof(d);
        memcpy(out + done, d, n);
        done += n;
    }
    return 0;
}

// This build's date, from __DATE__ ("Oct 16 2026") as year * 10000 + month * 100 + day
static int build_date(void) {
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int day = 1, year = 2026;
    sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
    const char *m = strstr(months, mon);
    return year * 10000 + (m ? (int)(m - months) / 3 + 1 : 1) * 100 + day;
}

// The PSP's clock is often wrong (it starts again from years ago when the battery runs flat) and GitHub's
// certificates last about 90 days, so the dates aren't checked against it: a certificate that isn't valid
// yet is accepted (only whoever it was issued to can use it), and an expired one only fails if it had run
// out by the time this game was built. Everything else (signatures, the trusted roots, the name) still counts.
static int verify_dates(void *p, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    *flags &= ~(uint32_t)MBEDTLS_X509_BADCERT_FUTURE;
    const mbedtls_x509_time *t = &crt->valid_to;
    if ((*flags & MBEDTLS_X509_BADCERT_EXPIRED) && t->year * 10000 + t->mon * 100 + t->day >= build_date()) {
        *flags &= ~(uint32_t)MBEDTLS_X509_BADCERT_EXPIRED;
    }
    return 0;
}

static int tls_init(void) {
    mbedtls_x509_crt_init(&g_ca);
    mbedtls_ctr_drbg_init(&g_drbg);
    mbedtls_ssl_config_init(&g_conf);
    int r = mbedtls_x509_crt_parse(&g_ca, (const unsigned char *)k_root_certs, sizeof(k_root_certs));
    if (r < 0) return fail("Couldn't read the built-in certificates (-0x%04X)", -r);
    static const char pers[] = "RetconsMod update";
    r = mbedtls_ctr_drbg_seed(&g_drbg, gather_entropy, NULL, (const unsigned char *)pers, sizeof(pers));
    if (r) return fail("Couldn't start the random generator (-0x%04X)", -r);
    r = mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
    if (r) return fail("Couldn't set up TLS (-0x%04X)", -r);
    // TLS 1.2 with forward secrecy and an AEAD cipher, which GitHub offers; nothing else (tls_trim.c
    // leaves the other ciphers out of the game)
    static const int suites[] = {
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,       MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,       MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
        0,
    };
    mbedtls_ssl_conf_ciphersuites(&g_conf, suites);
    mbedtls_ssl_conf_min_version(&g_conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_verify(&g_conf, verify_dates, NULL);
    mbedtls_ssl_conf_ca_chain(&g_conf, &g_ca, NULL);
    mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &g_drbg);
    return 1;
}

static void tls_free(void) {
    mbedtls_ssl_config_free(&g_conf);
    mbedtls_ctr_drbg_free(&g_drbg);
    mbedtls_x509_crt_free(&g_ca);
}

// --- HTTPS ---------------------------------------------------------------------------------------

typedef struct {
    int sock;
    int ssl_on;
    mbedtls_ssl_context ssl;
    unsigned char buf[4096];
    int pos, len, eof, err;
} Conn;

static Conn g_conn;
static char g_line[URL_MAX + 64];
static char g_req[URL_MAX + 512];

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    Conn *c = ctx;
    if (U.cancel) return MBEDTLS_ERR_NET_SEND_FAILED;
    int r = (int)sceNetInetSend(c->sock, buf, len, 0);
    if (r < 0) return MBEDTLS_ERR_NET_SEND_FAILED;
    touch();
    return r;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    Conn *c = ctx;
    if (U.cancel) return MBEDTLS_ERR_NET_RECV_FAILED;
    int r = (int)sceNetInetRecv(c->sock, buf, len, 0);
    if (r < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
    touch();
    return r; // 0: the server closed the connection
}

static void conn_close(Conn *c) {
    if (c->ssl_on) {
        mbedtls_ssl_free(&c->ssl);
        c->ssl_on = 0;
    }
    if (c->sock >= 0) {
        U.sock = -1;
        sceNetInetClose(c->sock);
        c->sock = -1;
    }
}

static int conn_open(Conn *c, const char *host) {
    memset(c, 0, sizeof(*c));
    c->sock = -1;
    char rbuf[1024];
    int rid;
    struct in_addr ip;
    if (sceNetResolverCreate(&rid, rbuf, sizeof(rbuf)) < 0) return fail("Couldn't start DNS");
    int r = sceNetResolverStartNtoA(rid, host, &ip, 5, 3);
    sceNetResolverDelete(rid);
    if (r < 0) return fail("Couldn't look up %s: is the access point online?", host);
    if (U.cancel) return 0;
    c->sock = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
    if (c->sock < 0) return fail("Couldn't open a connection (%d)", sceNetInetGetErrno());
    U.sock = c->sock;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(443);
    sa.sin_addr = ip;
    touch();
    if (sceNetInetConnect(c->sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        return fail("Couldn't connect to %s (%d)", host, sceNetInetGetErrno());
    }
    mbedtls_ssl_init(&c->ssl);
    c->ssl_on = 1;
    if ((r = mbedtls_ssl_setup(&c->ssl, &g_conf)) != 0) return fail("TLS setup failed (-0x%04X)", -r);
    mbedtls_ssl_set_hostname(&c->ssl, host);
    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
    while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) break;
    }
    if (r == 0) return 1;
    if (U.cancel) return 0;
    uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
    if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        char why[256];
        time_t now = time(NULL);
        mbedtls_x509_crt_verify_info(why, sizeof(why), "  ", flags);
        ulog("certificate check failed (flags %x) at %s%s", (unsigned int)flags, ctime(&now), why);
        const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&c->ssl);
        for (; peer; peer = peer->next) {
            char subject[128];
            mbedtls_x509_dn_gets(subject, sizeof(subject), &peer->subject);
            ulog("  cert %s, valid %04d-%02d-%02d to %04d-%02d-%02d", subject, peer->valid_from.year,
                 peer->valid_from.mon, peer->valid_from.day, peer->valid_to.year, peer->valid_to.mon, peer->valid_to.day);
        }
    }
    if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && (flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE))) {
        return fail("%s sent a certificate that ran out before this version was made", host);
    }
    if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) return fail("Couldn't trust %s's certificate (%x)", host, flags);
    return fail("Secure connection to %s failed (-0x%04X)", host, -r);
}

static int conn_fill(Conn *c) {
    if (c->eof) return 0;
    for (;;) {
        int r = mbedtls_ssl_read(&c->ssl, c->buf, sizeof(c->buf));
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF) {
            c->eof = 1;
            return 0;
        }
        if (r < 0) {
            c->eof = 1;
            c->err = r;
            return -1;
        }
        c->pos = 0;
        c->len = r;
        return r;
    }
}

static int conn_getc(Conn *c) {
    if (c->pos >= c->len && conn_fill(c) <= 0) return -1;
    return c->buf[c->pos++];
}

// A line without its CR LF (cut to fit); 0 at the end of the stream
static int conn_line(Conn *c, char *out, int max) {
    int n = 0, ch;
    while ((ch = conn_getc(c)) >= 0 && ch != '\n') {
        if (ch != '\r' && n < max - 1) out[n++] = (char)ch;
    }
    out[n] = 0;
    return ch >= 0 || n > 0;
}

static int conn_read(Conn *c, unsigned char *out, int max) {
    if (c->pos >= c->len && conn_fill(c) <= 0) return c->err ? -1 : 0;
    int n = c->len - c->pos;
    if (n > max) n = max;
    memcpy(out, c->buf + c->pos, (size_t)n);
    c->pos += n;
    return n;
}

static int conn_write(Conn *c, const char *s, int n) {
    while (n > 0) {
        int r = mbedtls_ssl_write(&c->ssl, (const unsigned char *)s, (size_t)n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) return 0;
        s += r;
        n -= r;
    }
    return 1;
}

typedef int (*Sink)(const unsigned char *data, int n); // 0 stops the download (it set the message)

// Up to n bytes of the body (n < 0: to the end) into the sink
static int read_body(Conn *c, int n, Sink sink) {
    unsigned char chunk[2048];
    while (n != 0) {
        int want = n < 0 || n > (int)sizeof(chunk) ? (int)sizeof(chunk) : n;
        int r = conn_read(c, chunk, want);
        if (r < 0) return U.cancel ? 0 : fail("The download broke off (-0x%04X)", -c->err);
        if (r == 0) return n < 0 ? 1 : (U.cancel ? 0 : fail("The download broke off early"));
        if (!sink(chunk, r)) return 0;
        if (n > 0) n -= r;
    }
    return 1;
}

// GET an https:// URL, following redirects; a 200's body goes to the sink. status: the final HTTP status.
static int https_get(const char *start_url, const char *accept, Sink sink, int *status) {
    static char url[URL_MAX];
    snprintf(url, sizeof(url), "%s", start_url);
    *status = 0;
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        if (strncmp(url, "https://", 8) != 0) return fail("Not a secure link: %.60s", url);
        char host[128];
        const char *h = url + 8, *slash = strchr(h, '/');
        int hl = slash ? (int)(slash - h) : (int)strlen(h);
        if (hl <= 0 || hl >= (int)sizeof(host)) return fail("Bad link: %.60s", url);
        memcpy(host, h, (size_t)hl);
        host[hl] = 0;
        const char *path = slash ? slash : "/";
        ulog("GET https://%s%.80s%s", host, path, strlen(path) > 80 ? "..." : "");
        Conn *c = &g_conn;
        if (!conn_open(c, host)) {
            conn_close(c);
            return 0;
        }
        int api = strcmp(host, "api.github.com") == 0;
        int n = snprintf(g_req, sizeof(g_req),
                         "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: RetconsMod/%s (PSP)\r\nAccept: %s\r\n%s"
                         "Connection: close\r\n\r\n",
                         path, host, GAME_VERSION, accept, api ? "X-GitHub-Api-Version: 2022-11-28\r\n" : "");
        if (n >= (int)sizeof(g_req) || !conn_write(c, g_req, n)) {
            conn_close(c);
            return U.cancel ? 0 : fail("Couldn't send the request to %s", host);
        }
        // Status line and headers
        if (!conn_line(c, g_line, sizeof(g_line)) || sscanf(g_line, "HTTP/%*d.%*d %d", status) != 1) {
            conn_close(c);
            return U.cancel ? 0 : fail("%s didn't answer properly", host);
        }
        int length = -1, chunked = 0;
        static char location[URL_MAX];
        location[0] = 0;
        while (conn_line(c, g_line, sizeof(g_line)) && g_line[0]) {
            if (strncasecmp(g_line, "Content-Length:", 15) == 0) length = atoi(g_line + 15);
            if (strncasecmp(g_line, "Transfer-Encoding:", 18) == 0 && strstr(g_line, "chunked")) chunked = 1;
            if (strncasecmp(g_line, "Location:", 9) == 0) {
                const char *v = g_line + 9;
                while (*v == ' ') v++;
                if (*v == '/') {
                    snprintf(location, sizeof(location), "https://%s%s", host, v);
                } else {
                    snprintf(location, sizeof(location), "%s", v);
                }
            }
        }
        ulog("  HTTP %d, %d bytes%s", *status, length, chunked ? " (chunked)" : "");
        if (*status >= 300 && *status < 400 && location[0]) {
            conn_close(c);
            snprintf(url, sizeof(url), "%s", location);
            continue;
        }
        int ok = 1;
        if (*status == 200) {
            if (chunked) {
                for (;;) {
                    if (!conn_line(c, g_line, sizeof(g_line))) {
                        ok = U.cancel ? 0 : fail("The download broke off early");
                        break;
                    }
                    int size = (int)strtol(g_line, NULL, 16);
                    if (size <= 0) break;
                    if (!(ok = read_body(c, size, sink))) break;
                    conn_line(c, g_line, sizeof(g_line)); // the CR LF after the chunk
                }
            } else {
                ok = read_body(c, length, sink);
            }
        }
        conn_close(c);
        return ok;
    }
    return fail("Too many redirects");
}

// --- The release ---------------------------------------------------------------------------------

static char *g_json;
static int g_json_len;

static int json_sink(const unsigned char *d, int n) {
    if (g_json_len + n > JSON_MAX - 1) n = JSON_MAX - 1 - g_json_len;
    memcpy(g_json + g_json_len, d, (size_t)n);
    g_json_len += n;
    g_json[g_json_len] = 0;
    return 1;
}

// Plain text for the page: the few non-ASCII characters notes use, simplified
static int put_char(char *out, int o, int max, unsigned int cp) {
    const char *s = NULL;
    char one[2] = {0, 0};
    if (cp == 0x2013 || cp == 0x2014 || cp == 0x2212) {
        s = "-";
    } else if (cp == 0x2018 || cp == 0x2019) {
        s = "'";
    } else if (cp == 0x201C || cp == 0x201D) {
        s = "\"";
    } else if (cp == 0x2026) {
        s = "...";
    } else if (cp == 0x2022 || cp == 0xD7) {
        s = cp == 0xD7 ? "x" : "*";
    } else if (cp >= 0x20 && cp < 0x7F) {
        one[0] = (char)cp;
        s = one;
    } else if (cp == '\n') {
        s = "\n";
    } else if (cp >= 0x80) {
        s = "?";
    }
    for (; s && *s && o < max - 1; s++) out[o++] = *s;
    return o;
}

// The string value of "key" at or after p, unescaped into out. Returns the end of the value, or NULL if
// there's no such key or its value isn't a string (null).
static const char *json_str(const char *p, const char *key, char *out, int max) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *k = strstr(p, pat);
    if (!k) return NULL;
    const char *v = k + strlen(pat);
    while (*v == ' ' || *v == '\n' || *v == '\t') v++;
    if (*v++ != ':') return NULL;
    while (*v == ' ' || *v == '\n' || *v == '\t') v++;
    if (*v++ != '"') return NULL;
    int o = 0;
    while (*v && *v != '"') {
        unsigned int cp = (unsigned char)*v++;
        if (cp == '\\') {
            char e = *v++;
            if (e == 'n') {
                cp = '\n';
            } else if (e == 't') {
                cp = ' ';
            } else if (e == 'r') {
                continue;
            } else if (e == 'u') {
                cp = (unsigned int)strtol((char[]){v[0], v[1], v[2], v[3], 0}, NULL, 16);
                v += 4;
            } else {
                cp = (unsigned char)e; // \" \\ \/
            }
        } else if (cp >= 0xC0) {
            // UTF-8
            int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : 1;
            cp &= 0x3F >> extra;
            while (extra-- > 0 && ((unsigned char)*v & 0xC0) == 0x80) cp = (cp << 6) | ((unsigned char)*v++ & 0x3F);
        }
        o = put_char(out, o, max, cp);
    }
    out[o] = 0;
    return v;
}

static int json_int(const char *p, const char *key, int *out) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *k = strstr(p, pat);
    if (!k) return 0;
    const char *v = k + strlen(pat);
    while (*v == ' ' || *v == ':') v++;
    if (*v < '0' || *v > '9') return 0;
    *out = atoi(v);
    return 1;
}

// The release notes for the page: no Markdown bold or code marks, no HTML comments, no blank lines
static void tidy_notes(const char *in) {
    char *o = U.notes;
    int n = 0, max = (int)sizeof(U.notes);
    for (const char *s = in; *s && n < max - 1;) {
        if (strncmp(s, "<!--", 4) == 0) {
            const char *e = strstr(s, "-->");
            s = e ? e + 3 : s + strlen(s);
            continue;
        }
        if (strncmp(s, "**", 2) == 0) {
            s += 2;
            continue;
        }
        if (*s == '`' || (*s == '#' && (s == in || s[-1] == '\n'))) {
            s++;
            continue;
        }
        if (*s == '\n' && (n == 0 || o[n - 1] == '\n')) {
            s++;
            continue;
        }
        o[n++] = *s++;
    }
    while (n > 0 && (o[n - 1] == '\n' || o[n - 1] == ' ')) n--;
    o[n] = 0;
}

static int parse_release(void) {
    char *j = g_json;
    const char *p = json_str(j, "tag_name", U.tag, sizeof(U.tag));
    if (!p) return fail("GitHub's answer had no version in it");
    if (!json_str(p, "name", U.title, sizeof(U.title))) U.title[0] = 0;
    U.url[0] = 0;
    U.size = 0;
    U.have_sha = 0;
    U.data = 1;
    U.notes[0] = 0;
    // The EBOOT.PBP among its assets; its fields end at browser_download_url
    char *assets = strstr(j, "\"assets\"");
    char name[64];
    for (char *a = assets; a && (a = (char *)json_str(a, "name", name, sizeof(name))) != NULL;) {
        if (strcmp(name, U.asset[0] ? U.asset : ASSET) != 0) continue;
        char *end = strstr(a, "\"browser_download_url\"");
        if (!end || !json_str(end, "browser_download_url", U.url, sizeof(U.url))) break;
        char keep = *end;
        *end = 0;
        json_int(a, "size", &U.size);
        char digest[80];
        if (json_str(a, "digest", digest, sizeof(digest)) && strncmp(digest, "sha256:", 7) == 0 && strlen(digest) == 71) {
            for (int i = 0; i < 32; i++) {
                unsigned int b;
                sscanf(digest + 7 + i * 2, "%2x", &b);
                U.sha[i] = (unsigned char)b;
            }
            U.have_sha = 1;
        }
        *end = keep;
        break;
    }
    static char body[2048];
    if (json_str(assets ? assets : j, "body", body, sizeof(body))) {
        const char *d = strstr(body, "<!-- data ");
        if (d) U.data = atoi(d + 10);
        tidy_notes(body);
    }
    ulog("latest: %s \"%s\", data %d, %s %d bytes%s", U.tag, U.title, U.data, U.url[0] ? "EBOOT" : "no EBOOT", U.size,
         U.have_sha ? ", sha256 given" : "");
    return 1;
}

static int do_check(void) {
    g_json = malloc(JSON_MAX);
    if (!g_json) return fail("Not enough memory to check");
    g_json_len = 0;
    g_json[0] = 0;
    char url[160];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", U.repo[0] ? U.repo : REPO);
    int status, ok = https_get(url, "application/vnd.github+json", json_sink, &status);
    if (ok && status == 404) {
        ok = fail("No versions have been published on GitHub yet");
    } else if (ok && (status == 403 || status == 429)) {
        ok = fail("GitHub is busy (too many checks from this network): try again later");
    } else if (ok && status != 200) {
        ok = fail("GitHub answered %d", status);
    }
    if (ok) ok = parse_release();
    free(g_json);
    g_json = NULL;
    if (!ok) return ST_FAILED;
    int cur = version_number(current_version()), latest = version_number(U.tag);
    if (latest <= cur) return ST_LATEST;
    if (U.data > GAME_DATA_VERSION) return ST_NEEDS_DATA;
    if (!U.url[0]) {
        fail("Version %s is out, but it has no %s to download", U.tag, ASSET);
        return ST_FAILED;
    }
    return ST_AVAILABLE;
}

// --- Download and install ------------------------------------------------------------------------

static FILE *g_out;
static mbedtls_sha256_context g_sha;

static int file_sink(const unsigned char *d, int n) {
    mbedtls_sha256_update_ret(&g_sha, d, (size_t)n);
    if ((int)fwrite(d, 1, (size_t)n, g_out) != n) return fail("Couldn't write to the Memory Stick (is it full?)");
    U.got += n;
    return 1;
}

// path's folder plus name ("ms0:/PSP/GAME/X/EBOOT.PBP" -> "ms0:/PSP/GAME/X/EBOOT.NEW")
static void beside(const char *path, const char *name, char *out, int max) {
    const char *slash = strrchr(path, '/');
    int dl = slash ? (int)(slash - path) + 1 : 0;
    snprintf(out, (size_t)max, "%.*s%s", dl, path, name);
}

static int copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb");
    if (!a) return 0;
    FILE *b = fopen(to, "wb");
    if (!b) {
        fclose(a);
        return 0;
    }
    static unsigned char buf[16384];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), a)) > 0) {
        if (fwrite(buf, 1, n, b) != n) ok = 0;
    }
    fclose(a);
    if (fclose(b) != 0) ok = 0;
    return ok;
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

// The new EBOOT goes where the running one is; the running one stays as EBOOT.OLD
static int install(const char *fresh) {
    char old[300];
    beside(U.eboot, "EBOOT.OLD", old, sizeof(old));
    remove(old);
    if (!file_exists(U.eboot)) {
        if (rename(fresh, U.eboot) == 0) return 1;
        return fail("Couldn't put the new version in place");
    }
    if (rename(U.eboot, old) == 0) {
        if (rename(fresh, U.eboot) == 0) return 1;
        rename(old, U.eboot);
        return fail("Couldn't put the new version in place (the old one is still there)");
    }
    // Renaming the running EBOOT refused: copy over it instead
    ulog("rename refused; copying");
    if (!copy_file(U.eboot, old)) return fail("Couldn't keep a copy of this version");
    if (!copy_file(fresh, U.eboot)) {
        copy_file(old, U.eboot);
        return fail("Couldn't write the new version");
    }
    remove(fresh);
    return 1;
}

static int do_download(int install_it) {
    char fresh[300];
    beside(U.eboot, "EBOOT.NEW", fresh, sizeof(fresh));
    g_out = fopen(fresh, "wb");
    if (!g_out) {
        fail("Couldn't write to the Memory Stick");
        return ST_FAILED;
    }
    setvbuf(g_out, NULL, _IOFBF, 32768);
    mbedtls_sha256_init(&g_sha);
    mbedtls_sha256_starts_ret(&g_sha, 0);
    U.got = 0;
    int status, ok = https_get(install_it ? U.url : U.get_url, "application/octet-stream", file_sink, &status);
    if (fclose(g_out) != 0 && ok) ok = fail("Couldn't finish writing to the Memory Stick");
    g_out = NULL;
    unsigned char sha[32];
    mbedtls_sha256_finish_ret(&g_sha, sha);
    mbedtls_sha256_free(&g_sha);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", sha[i]);
    if (ok && status != 200) ok = fail("The download failed (HTTP %d)", status);
    if (ok) ulog("downloaded %d bytes, sha256 %s", U.got, hex);
    if (ok && !install_it) {
        snprintf(U.msg, sizeof(U.msg), "Test download: %d bytes, SHA-256 %.16s...", U.got, hex);
        return ST_FAILED;
    }
    if (ok && U.size > 0 && U.got != U.size) ok = fail("The download is the wrong size (%d of %d bytes)", U.got, U.size);
    if (ok && U.have_sha && memcmp(sha, U.sha, 32) != 0) ok = fail("The download is damaged (its checksum is wrong)");
    if (ok) {
        unsigned char magic[4] = {0};
        FILE *f = fopen(fresh, "rb");
        if (f) {
            if (fread(magic, 1, 4, f) != 4) magic[0] = 1;
            fclose(f);
        }
        if (memcmp(magic, "\0PBP", 4) != 0 && !U.asset[0]) ok = fail("The download isn't a PSP game");
    }
    if (ok) ok = install(fresh);
    if (!ok) {
        remove(fresh);
        return ST_FAILED;
    }
    ulog("installed %s as %s", U.tag, U.eboot);
    return ST_INSTALLED;
}

// --- The worker ----------------------------------------------------------------------------------

static int worker(SceSize args, void *argp) {
    int result = ST_FAILED;
    if (tls_init()) {
        if (U.job == JOB_CHECK) result = do_check();
        if (U.job == JOB_DOWNLOAD) result = do_download(1);
        if (U.job == JOB_GET) result = do_download(0);
    }
    tls_free();
    if (U.cancel == 2) {
        fail("The connection stalled: try again");
        result = ST_FAILED;
    }
    if (U.cancel != 1) U.state = result;
    U.busy = 0;
    return 0;
}

static void start_job(int job) {
    U.cancel = 0;
    U.got = 0;
    U.job = job;
    U.msg[0] = 0;
    touch();
    U.state = job == JOB_CHECK ? ST_CHECKING : ST_DOWNLOADING;
    U.busy = 1;
    // Below the main thread's priority (32), so the menu keeps drawing through the TLS maths
    U.thread = sceKernelCreateThread("update", worker, 40, 64 * 1024, PSP_THREAD_ATTR_USER, NULL);
    if (U.thread < 0 || sceKernelStartThread(U.thread, 0, NULL) < 0) {
        U.busy = 0;
        U.thread = -1;
        fail("Couldn't start the download");
        U.state = ST_FAILED;
    }
}

// Cuts the worker's connection, so whatever it's waiting for fails at once
static void cut(void) {
    int s = U.sock;
    if (s >= 0) sceNetInetShutdown(s, 2);
}

static void stop_net(void) {
    if (U.net_ours) net_stop();
    U.net_ours = 0;
}

// Starts (or retries) the check: Wi-Fi first
static void begin(int job) {
    U.msg[0] = 0;
    U.next_job = job;
    if (U.in_game) {
        U.state = ST_IN_GAME;
        return;
    }
    if (net_active() && net_mode() == NET_MODE_INET) {
        start_job(job);
        return;
    }
    net_stop();
    net_set_mode(NET_MODE_INET, 0);
    if (net_start() < 0) {
        fail("%s", net_status());
        U.state = ST_FAILED;
        return;
    }
    U.net_ours = 1;
    U.state = ST_WIFI;
}

// --- Page ----------------------------------------------------------------------------------------

void update_init(const char *eboot) {
    int n = eboot ? (int)strlen(eboot) : 0;
    if (n >= 9 && n < (int)sizeof(U.eboot) && strcasecmp(eboot + n - 9, "EBOOT.PBP") == 0) {
        snprintf(U.eboot, sizeof(U.eboot), "%s", eboot);
    } else {
        snprintf(U.eboot, sizeof(U.eboot), "EBOOT.PBP"); // in the game's folder, the working directory
    }
}

void update_set_in_game(int in_game) {
    U.in_game = in_game;
}

void update_page_open(void) {
    FILE *f = fopen(LOG_PATH, "w");
    if (f) {
        time_t now = time(NULL);
        fprintf(f, "Retcon's Mod %s update check, %s", current_version(), ctime(&now));
        fprintf(f, "EBOOT: %s\n", U.eboot);
        fclose(f);
    }
    if (U.busy) {
        U.state = U.job == JOB_CHECK ? ST_CHECKING : ST_DOWNLOADING; // still going from before
        return;
    }
    begin(U.get_url[0] ? JOB_GET : JOB_CHECK);
}

static void reap(void) {
    if (U.thread >= 0 && !U.busy) {
        sceKernelWaitThreadEnd(U.thread, NULL);
        sceKernelDeleteThread(U.thread);
        U.thread = -1;
    }
}

int update_page_update(const InputState *in) {
    reap();
    if (U.busy && !U.cancel && sceKernelGetSystemTimeWide() - U.last_io > STALL_US) {
        U.cancel = 2;
        cut();
    }
    if (U.state == ST_WIFI) {
        NetState s = net_poll();
        if (s == NET_READY) {
            start_job(U.next_job);
        } else if (s == NET_FAILED) {
            fail("%s", net_status());
            U.state = ST_FAILED;
            stop_net();
        }
    }
    if (U.state == ST_STOPPING) {
        if (U.busy) return 0;
        stop_net();
        U.state = ST_IDLE;
        return 1;
    }
    if (U.autoget && U.state == ST_AVAILABLE) start_job(JOB_DOWNLOAD);
    int click = snd_find("menu_move");
    if (in->pressed & (PSP_CTRL_CIRCLE | PSP_CTRL_START)) {
        if (g_settings.sound) snd_play(click, 0.4f, 0.0f);
        if (U.busy) {
            U.cancel = 1;
            U.state = ST_STOPPING;
            cut();
            return 0;
        }
        stop_net();
        U.state = ST_IDLE;
        return 1;
    }
    if (in->pressed & PSP_CTRL_CROSS) {
        if (U.state == ST_AVAILABLE) {
            start_job(JOB_DOWNLOAD);
        } else if (U.state == ST_FAILED || U.state == ST_LATEST) {
            begin(U.get_url[0] ? JOB_GET : JOB_CHECK);
        } else if (U.state == ST_INSTALLED) {
            settings_save();
            stop_net();
            sceKernelExitGame(); // back to the XMB; starting the game again runs the new version
        } else {
            return 0;
        }
        if (g_settings.sound) snd_play(click, 0.4f, 0.0f);
    }
    return 0;
}

// Word-wrapped text from y down, no lower than bottom. Returns the y below it.
static float wrap(float x, float y, float w, float bottom, unsigned int colour, const char *s) {
    int lh = ui_line_height(FONT_SMALL);
    char line[128];
    while (*s && y + lh <= bottom) {
        // As many words as fit, up to a newline
        int n = 0, fit = 0;
        while (s[n] && s[n] != '\n' && n < (int)sizeof(line) - 1) {
            line[n] = s[n];
            line[n + 1] = 0;
            if (ui_text_width(FONT_SMALL, line) > w) break;
            n++;
            if (s[n] == ' ' || s[n] == 0 || s[n] == '\n') fit = n;
        }
        if (!fit) fit = n > 0 ? n : 1; // one word longer than the line: cut it
        memcpy(line, s, (size_t)fit);
        line[fit] = 0;
        ui_text(FONT_SMALL, x, y, colour, ALIGN_LEFT, line);
        y += lh;
        s += fit;
        while (*s == ' ') s++;
        if (*s == '\n') s++;
    }
    return y;
}

static const char *dots(void) {
    static const char *const k[4] = {"", ".", "..", "..."};
    return k[(sceKernelGetSystemTimeWide() / 400000) % 4];
}

void update_page_draw(float x, float y, float w, float h) {
    ui_rrect(x, y, w, h, 4, 0xF0E8E8E8);
    ui_gradient(x, y, w, 22, 0xFF8C6A3D, 0xFF6E5230);
    ui_text(FONT_BOLD, x + 8, y + 5, 0xFFFFFFFF, ALIGN_LEFT, "Updates");
    float tx = x + 12, tw = w - 24, ty = y + 30, bottom = y + h - 22;
    int lh = ui_line_height(FONT_SMALL);
    ui_textf(FONT_SMALL, tx, ty, C_DIM, ALIGN_LEFT, "This version: %s", current_version());
    ty += lh + 6;
    const char *hint = "O: back";
    char buf[192];
    switch (U.state) {
    case ST_IN_GAME:
        wrap(tx, ty, tw, bottom, C_TEXT, "Check for updates from the main menu's Options, outside a game.");
        break;
    case ST_WIFI:
        snprintf(buf, sizeof(buf), "Connecting to Wi-Fi%s", dots());
        ty = wrap(tx, ty, tw, bottom, C_TEXT, buf);
        wrap(tx, ty + 4, tw, bottom, C_DIM, "Uses the first connection in Network Settings. The WLAN switch needs to be on.");
        hint = "O: cancel";
        break;
    case ST_CHECKING:
        snprintf(buf, sizeof(buf), "Checking GitHub for a newer version%s", dots());
        wrap(tx, ty, tw, bottom, C_TEXT, buf);
        hint = "O: cancel";
        break;
    case ST_LATEST:
        ui_text(FONT_BOLD, tx, ty, C_GOOD, ALIGN_LEFT, "You have the latest version.");
        ty += ui_line_height(FONT_BOLD) + 4;
        if (U.tag[0]) {
            snprintf(buf, sizeof(buf), "The newest on GitHub is %s.", U.tag[0] == 'v' ? U.tag + 1 : U.tag);
            wrap(tx, ty, tw, bottom, C_DIM, buf);
        }
        hint = "X: check again    O: back";
        break;
    case ST_AVAILABLE:
    case ST_NEEDS_DATA:
        snprintf(buf, sizeof(buf), "Version %s is out", U.tag[0] == 'v' ? U.tag + 1 : U.tag);
        if (U.state == ST_AVAILABLE && U.size > 0) {
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " (%d KB)", (U.size + 1023) / 1024);
        }
        ui_text(FONT_BOLD, tx, ty, C_TEXT, ALIGN_LEFT, buf);
        ty += ui_line_height(FONT_BOLD) + 4;
        if (U.state == ST_NEEDS_DATA) {
            ty = wrap(tx, ty, tw, bottom, C_BAD,
                      "It needs new game data, which is built on a PC from your own games. Update on your PC: "
                      "rebuild the data and the game, then copy them over.");
            ty += 4;
        }
        wrap(tx, ty, tw, bottom, C_DIM, U.notes[0] ? U.notes : U.title);
        if (U.state == ST_AVAILABLE) hint = "X: download and install    O: back";
        break;
    case ST_DOWNLOADING: {
        snprintf(buf, sizeof(buf), "Downloading %s%s", U.tag[0] == 'v' ? U.tag + 1 : U.tag, dots());
        ui_text(FONT_SMALL, tx, ty, C_TEXT, ALIGN_LEFT, buf);
        ty += lh + 6;
        int total = U.size > 0 ? U.size : 0;
        float f = total > 0 ? (float)U.got / (float)total : 0.0f;
        if (f > 1.0f) f = 1.0f;
        ui_rrect(tx, ty, tw, 12, 3, 0xFFC8C8C8);
        if (f > 0.0f) ui_rrect(tx, ty, tw * f, 12, 3, C_BAR);
        ty += 18;
        if (total > 0) {
            ui_textf(FONT_SMALL, tx, ty, C_DIM, ALIGN_LEFT, "%d of %d KB", U.got / 1024, (total + 1023) / 1024);
        } else {
            ui_textf(FONT_SMALL, tx, ty, C_DIM, ALIGN_LEFT, "%d KB", U.got / 1024);
        }
        hint = "O: cancel";
        break;
    }
    case ST_INSTALLED:
        snprintf(buf, sizeof(buf), "Version %s is installed.", U.tag[0] == 'v' ? U.tag + 1 : U.tag);
        ui_text(FONT_BOLD, tx, ty, C_GOOD, ALIGN_LEFT, buf);
        ty += ui_line_height(FONT_BOLD) + 4;
        wrap(tx, ty, tw, bottom, C_TEXT,
             "Quit and start Retcon's Mod again to play it. This version is kept as EBOOT.OLD in the game's "
             "folder: rename it back to EBOOT.PBP to go back.");
        hint = "X: quit    O: back";
        break;
    case ST_FAILED:
        wrap(tx, ty, tw, bottom, C_BAD, U.msg[0] ? U.msg : "Something went wrong");
        hint = "X: try again    O: back";
        break;
    case ST_STOPPING:
        snprintf(buf, sizeof(buf), "Stopping%s", dots());
        ui_text(FONT_SMALL, tx, ty, C_TEXT, ALIGN_LEFT, buf);
        hint = "";
        break;
    default:
        break;
    }
    ui_text(FONT_SMALL, x + 10, y + h - 18, 0xFF606060, ALIGN_LEFT, hint);
}

// --- Tests ---------------------------------------------------------------------------------------

int update_command(const char *line) {
    char s[256];
    if (sscanf(line, "updaterepo %63s", s) == 1) {
        snprintf(U.repo, sizeof(U.repo), "%s", s);
    } else if (sscanf(line, "updatever %23s", s) == 1) {
        snprintf(U.pretend, sizeof(U.pretend), "%s", s);
    } else if (sscanf(line, "updatetarget %255s", s) == 1) {
        snprintf(U.eboot, sizeof(U.eboot), "%s", s);
    } else if (sscanf(line, "updateget %255s", s) == 1) {
        snprintf(U.get_url, sizeof(U.get_url), "%s", s);
        if (U.state == ST_WIFI) U.next_job = JOB_GET;
    } else if (sscanf(line, "updateasset %95s", s) == 1) {
        snprintf(U.asset, sizeof(U.asset), "%s", s);
    } else if (strncmp(line, "updateauto", 10) == 0) {
        U.autoget = 1;
    } else {
        return 0;
    }
    return 1;
}
