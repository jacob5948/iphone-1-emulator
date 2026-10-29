/*
 * netrelay — the far end of the browser phone's virtual Wi-Fi network.
 *
 * A browser cannot open raw sockets, so the emulator in the page sends the
 * guest's Ethernet frames over a WebSocket, one frame per binary message, and
 * this relay feeds them to a private user-mode NAT (libslirp). Replies come
 * back the same way. Each connection is served by its own forked process with
 * its own NAT, so phones never share state and a crash takes down one phone.
 *
 * SAFETY. A relay reachable by strangers is an open proxy into whatever its
 * host can reach, so by default the guest may only talk to public IPv4
 * addresses (net_slirp.h, public_only) and connections are capped. Use
 * --allow-origin to accept only pages from your own site, and put TLS in
 * front (wss://) when the page itself is served over https.
 *
 *   netrelay [--bind 127.0.0.1] [--port 8090] [--max-clients 16]
 *            [--allow-origin https://example.com]... [--allow-private]
 *            [--host-loopback] [--idle-seconds 900]
 *
 * --host-loopback (local development only) also lets the phone reach this
 * machine's own services at 10.0.2.2, e.g. a test page on localhost.
 */
#define _POSIX_C_SOURCE 200809L
#include "net_slirp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_ORIGINS 8
#define MAX_MESSAGE 65536u

static struct {
    const char *bind_addr;
    int port, max_clients, idle_seconds;
    const char *origins[MAX_ORIGINS];
    int norigins;
    bool allow_private, host_loopback;
} cfg = { "127.0.0.1", 8090, 16, 900, {0}, 0, false, false };

static volatile sig_atomic_t children;

/* ------------------------------------------------------------ SHA-1 ------ */

typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } sha1_t;

static uint32_t rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

static void sha1_block(sha1_t *c, const uint8_t *p) {
    uint32_t w[80], a = c->h[0], b = c->h[1], d = c->h[3], e = c->h[4], cc = c->h[2];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 |
               (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | (~b & d);          k = 0x5a827999u; }
        else if (i < 40) { f = b ^ cc ^ d;                    k = 0x6ed9eba1u; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8f1bbcdcu; }
        else             { f = b ^ cc ^ d;                    k = 0xca62c1d6u; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void sha1(const uint8_t *p, size_t n, uint8_t out[20]) {
    sha1_t c = { { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u }, 0, {0}, 0 };
    c.len = (uint64_t)n * 8u;
    while (n >= 64) { sha1_block(&c, p); p += 64; n -= 64; }
    uint8_t tail[128] = {0};
    memcpy(tail, p, n);
    tail[n] = 0x80;
    size_t total = n + 9 <= 64 ? 64 : 128;
    for (int i = 0; i < 8; i++) tail[total - 1 - i] = (uint8_t)(c.len >> (8 * i));
    sha1_block(&c, tail);
    if (total == 128) sha1_block(&c, tail + 64);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[4*i+j] = (uint8_t)(c.h[i] >> (24 - 8*j));
}

static void base64(const uint8_t *p, size_t n, char *out) {
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i+1] << 8 : 0) |
                     (i + 2 < n ? p[i+2] : 0);
        out[o++] = t[v >> 18 & 63];
        out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < n ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[o] = 0;
}

/* ------------------------------------------------------------ helpers ---- */

static bool write_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        b += w; n -= (size_t)w;
    }
    return true;
}

/* Case-insensitive header lookup in a NUL-terminated request head. */
static bool header(const char *req, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    for (const char *p = strstr(req, "\r\n"); p && p[2]; p = strstr(p + 2, "\r\n")) {
        const char *line = p + 2;
        if (strncasecmp(line, name, nl) || line[nl] != ':') continue;
        line += nl + 1;
        while (*line == ' ') line++;
        size_t i = 0;
        while (line[i] && line[i] != '\r' && i + 1 < cap) { out[i] = line[i]; i++; }
        out[i] = 0;
        return true;
    }
    return false;
}

static void http_reply(int fd, const char *status, const char *body) {
    char b[512];
    int n = snprintf(b, sizeof b, "HTTP/1.1 %s\r\nContent-Type: text/plain\r\n"
                     "Content-Length: %zu\r\nAccess-Control-Allow-Origin: *\r\n"
                     "Connection: close\r\n\r\n%s", status, strlen(body), body);
    write_all(fd, b, (size_t)n);
}

/* ------------------------------------------------------------ websocket -- */

typedef struct {
    int fd;
    uint8_t in[2 * MAX_MESSAGE + 16];
    size_t in_n;
    uint8_t msg[MAX_MESSAGE];
    size_t msg_n;
    net_slirp_t *net;
    time_t last_activity;
} client_t;

static bool ws_send(int fd, int opcode, const uint8_t *p, size_t n) {
    uint8_t h[10];
    size_t hl;
    h[0] = (uint8_t)(0x80 | opcode);
    if (n < 126) { h[1] = (uint8_t)n; hl = 2; }
    else if (n < 65536) { h[1] = 126; h[2] = (uint8_t)(n >> 8); h[3] = (uint8_t)n; hl = 4; }
    else { h[1] = 127; for (int i = 0; i < 8; i++) h[2+i] = (uint8_t)((uint64_t)n >> (56 - 8*i)); hl = 10; }
    return write_all(fd, h, hl) && (!n || write_all(fd, p, n));
}

static void to_browser(void *ctx, const uint8_t *frame, size_t len) {
    client_t *c = ctx;
    if (!ws_send(c->fd, 2, frame, len)) _exit(0);
}

/* Consume complete frames from c->in. Returns false when the peer closed. */
static bool ws_receive(client_t *c) {
    for (;;) {
        if (c->in_n < 2) return true;
        uint8_t *p = c->in;
        bool fin = p[0] & 0x80, masked = p[1] & 0x80;
        int op = p[0] & 0x0f;
        uint64_t len = p[1] & 0x7f;
        size_t hl = 2;
        if (len == 126) { if (c->in_n < 4) return true; len = (uint64_t)p[2] << 8 | p[3]; hl = 4; }
        else if (len == 127) {
            if (c->in_n < 10) return true;
            len = 0; for (int i = 0; i < 8; i++) len = len << 8 | p[2+i];
            hl = 10;
        }
        if (!masked || len > MAX_MESSAGE) return false;   /* clients must mask */
        if (c->in_n < hl + 4 + len) return true;
        uint8_t *mask = p + hl, *data = p + hl + 4;
        for (uint64_t i = 0; i < len; i++) data[i] ^= mask[i & 3];
        if (op == 8) { ws_send(c->fd, 8, NULL, 0); return false; }
        if (op == 9) ws_send(c->fd, 10, data, (size_t)len);
        else if (op == 0 || op == 2) {
            if (c->msg_n + len > MAX_MESSAGE) return false;
            memcpy(c->msg + c->msg_n, data, (size_t)len);
            c->msg_n += (size_t)len;
            if (fin) {
                net_slirp_input(c->net, c->msg, c->msg_n);
                c->msg_n = 0;
            }
        }
        size_t used = hl + 4 + (size_t)len;
        memmove(c->in, c->in + used, c->in_n - used);
        c->in_n -= used;
        c->last_activity = time(NULL);
    }
}

static void serve(int fd) {
    char req[8192];
    size_t n = 0;
    while (n + 1 < sizeof req) {
        ssize_t r = read(fd, req + n, sizeof req - 1 - n);
        if (r <= 0) return;
        n += (size_t)r;
        req[n] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (!strstr(req, "\r\n\r\n")) return;
    if (!strncmp(req, "GET /health", 11)) { http_reply(fd, "200 OK", "ok\n"); return; }

    char key[128], origin[256] = "", upgrade[64] = "";
    if (!header(req, "Upgrade", upgrade, sizeof upgrade) || strcasecmp(upgrade, "websocket") ||
        !header(req, "Sec-WebSocket-Key", key, sizeof key)) {
        http_reply(fd, "426 Upgrade Required", "iPhone virtual Wi-Fi relay: connect with a WebSocket.\n");
        return;
    }
    header(req, "Origin", origin, sizeof origin);
    if (cfg.norigins) {
        bool ok = false;
        for (int i = 0; i < cfg.norigins; i++) ok |= !strcmp(origin, cfg.origins[i]);
        if (!ok) { http_reply(fd, "403 Forbidden", "origin not allowed\n"); return; }
    }

    char cat[256], accept[64];
    uint8_t digest[20];
    snprintf(cat, sizeof cat, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    sha1((const uint8_t *)cat, strlen(cat), digest);
    base64(digest, sizeof digest, accept);
    char resp[256];
    int rl = snprintf(resp, sizeof resp, "HTTP/1.1 101 Switching Protocols\r\n"
                      "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    if (!write_all(fd, resp, (size_t)rl)) return;

    static client_t c;
    c.fd = fd;
    c.last_activity = time(NULL);
    net_slirp_policy_t policy = { .public_only = !cfg.allow_private,
                                  .host_loopback = cfg.host_loopback };
    c.net = net_slirp_new(to_browser, &c, &policy);
    if (!c.net) return;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    fprintf(stderr, "netrelay[%d]: phone connected from origin '%s'\n", (int)getpid(), origin);

    for (;;) {
        struct pollfd pf = { .fd = fd, .events = POLLIN };
        net_slirp_poll(c.net, 100, &pf, 1);
        if (pf.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (pf.revents & POLLIN) {
            if (c.in_n >= sizeof c.in) break;
            ssize_t r = read(fd, c.in + c.in_n, sizeof c.in - c.in_n);
            if (r <= 0) break;
            c.in_n += (size_t)r;
            if (!ws_receive(&c)) break;
        }
        if (time(NULL) - c.last_activity > cfg.idle_seconds) break;
    }
    net_slirp_stats_t st = net_slirp_stats(c.net);
    fprintf(stderr, "netrelay[%d]: phone left (%llu out, %llu in, %llu filtered)\n",
            (int)getpid(), (unsigned long long)st.to_net,
            (unsigned long long)st.to_guest, (unsigned long long)st.filtered);
    net_slirp_free(c.net);
}

static void reap(int sig) {
    (void)sig;
    int saved = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) children--;
    errno = saved;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bind") && i + 1 < argc) cfg.bind_addr = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) cfg.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-clients") && i + 1 < argc) cfg.max_clients = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--idle-seconds") && i + 1 < argc) cfg.idle_seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--allow-origin") && i + 1 < argc && cfg.norigins < MAX_ORIGINS)
            cfg.origins[cfg.norigins++] = argv[++i];
        else if (!strcmp(argv[i], "--allow-private")) cfg.allow_private = true;
        else if (!strcmp(argv[i], "--host-loopback")) cfg.allow_private = cfg.host_loopback = true;
        else {
            fprintf(stderr, "usage: %s [--bind ADDR] [--port N] [--max-clients N]\n"
                    "       [--allow-origin URL]... [--allow-private] [--host-loopback]\n"
                    "       [--idle-seconds N]\n", argv[0]);
            return 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = { .sa_handler = reap, .sa_flags = SA_RESTART | SA_NOCLDSTOP };
    sigaction(SIGCHLD, &sa, NULL);

    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)cfg.port) };
    if (s < 0 || inet_pton(AF_INET, cfg.bind_addr, &a.sin_addr) != 1) {
        fprintf(stderr, "netrelay: bad bind address %s\n", cfg.bind_addr);
        return 1;
    }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 16)) {
        perror("netrelay: listen");
        return 1;
    }
    fprintf(stderr, "netrelay: listening on ws://%s:%d/ (%s destinations, %d phones max)\n",
            cfg.bind_addr, cfg.port, cfg.allow_private ? "all" : "public-only",
            cfg.max_clients);
    for (;;) {
        int fd = accept(s, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; perror("accept"); continue; }
        if (children >= cfg.max_clients) {
            http_reply(fd, "503 Service Unavailable", "relay is full\n");
            close(fd);
            continue;
        }
        pid_t pid = fork();
        if (pid == 0) { close(s); serve(fd); close(fd); _exit(0); }
        if (pid > 0) children++;
        close(fd);
    }
}
