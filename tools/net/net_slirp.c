/*
 * libslirp glue for the virtual Wi-Fi network. See net_slirp.h.
 */
#define _POSIX_C_SOURCE 200809L
#include "net_slirp.h"

#include <arpa/inet.h>
#include <poll.h>
#include <slirp/libslirp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#define MAX_TIMERS  32u
#define MAX_POLL    512u

typedef struct {
    bool          used, armed;
    int64_t       expire_ms;
    SlirpTimerId  id;
    void         *cb_opaque;
} net_timer_t;

struct net_slirp {
    Slirp              *slirp;
    net_slirp_out_fn    out;
    void               *ctx;
    net_slirp_policy_t  policy;
    net_timer_t         timers[MAX_TIMERS];
    struct pollfd       fds[MAX_POLL];
    unsigned            nfds, nextra;
    net_slirp_stats_t   stats;
};

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

/* ------------------------------------------------------------ callbacks -- */

static ssize_t cb_send(const void *buf, size_t len, void *opaque) {
    net_slirp_t *n = opaque;
    n->stats.to_guest++;
    if (n->out) n->out(n->ctx, buf, len);
    return (ssize_t)len;
}

static void cb_guest_error(const char *msg, void *opaque) {
    net_slirp_t *n = opaque;
    if (n->policy.log) n->policy.log(n->ctx, msg);
}

static int64_t cb_clock(void *opaque) { (void)opaque; return now_ns(); }

static void *cb_timer_new_opaque(SlirpTimerId id, void *cb_opaque, void *opaque) {
    net_slirp_t *n = opaque;
    for (unsigned i = 0; i < MAX_TIMERS; i++) {
        if (n->timers[i].used) continue;
        n->timers[i] = (net_timer_t){ .used = true, .id = id, .cb_opaque = cb_opaque };
        return &n->timers[i];
    }
    return NULL;
}

static void cb_timer_free(void *timer, void *opaque) {
    (void)opaque;
    if (timer) memset(timer, 0, sizeof(net_timer_t));
}

static void cb_timer_mod(void *timer, int64_t expire_ms, void *opaque) {
    (void)opaque;
    net_timer_t *t = timer;
    if (!t) return;
    t->armed = true;
    t->expire_ms = expire_ms;
}

static void cb_poll_fd(int fd, void *opaque) { (void)fd; (void)opaque; }
static void cb_notify(void *opaque) { (void)opaque; }

static const SlirpCb callbacks = {
    .send_packet = cb_send,
    .guest_error = cb_guest_error,
    .clock_get_ns = cb_clock,
    .timer_free = cb_timer_free,
    .timer_mod = cb_timer_mod,
    .register_poll_fd = cb_poll_fd,
    .unregister_poll_fd = cb_poll_fd,
    .notify = cb_notify,
    .timer_new_opaque = cb_timer_new_opaque,
};

/* ------------------------------------------------------------ lifecycle -- */

net_slirp_t *net_slirp_new(net_slirp_out_fn out, void *ctx,
                           const net_slirp_policy_t *policy) {
    net_slirp_t *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->out = out;
    n->ctx = ctx;
    if (policy) n->policy = *policy;

    SlirpConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.version = 4;
    cfg.in_enabled = true;
    inet_pton(AF_INET, "10.0.2.0", &cfg.vnetwork);
    inet_pton(AF_INET, "255.255.255.0", &cfg.vnetmask);
    inet_pton(AF_INET, "10.0.2.2", &cfg.vhost);
    inet_pton(AF_INET, "10.0.2.15", &cfg.vdhcp_start);
    inet_pton(AF_INET, "10.0.2.3", &cfg.vnameserver);
    cfg.in6_enabled = false;
    cfg.vhostname = "iphone";
    cfg.if_mtu = 1500;
    cfg.if_mru = 1500;
    cfg.disable_host_loopback = !n->policy.host_loopback;
    n->slirp = slirp_new(&cfg, &callbacks, n);
    if (!n->slirp) { free(n); return NULL; }
    return n;
}

void net_slirp_free(net_slirp_t *n) {
    if (!n) return;
    if (n->slirp) slirp_cleanup(n->slirp);
    free(n);
}

/* ------------------------------------------------------------ filtering -- */

static bool private_v4(uint32_t a) {
    uint8_t b0 = (uint8_t)(a >> 24), b1 = (uint8_t)(a >> 16);
    if (a == 0xffffffffu) return false;                 /* DHCP broadcast */
    if (a == 0x0a000203u) return false;                 /* 10.0.2.3, our DNS */
    return b0 == 0 || b0 == 10 || b0 == 127 || b0 >= 224 ||
           (b0 == 100 && (b1 & 0xc0) == 64) ||          /* 100.64/10 */
           (b0 == 169 && b1 == 254) ||
           (b0 == 172 && (b1 & 0xf0) == 16) ||
           (b0 == 192 && b1 == 168) ||
           (b0 == 192 && b1 == 0) ||                    /* 192.0.0/24, 192.0.2/24 */
           (b0 == 198 && (b1 & 0xfe) == 18);            /* 198.18/15 */
}

static bool allowed(const net_slirp_t *n, const uint8_t *f, size_t len) {
    if (!n->policy.public_only) return true;
    if (len < 14u) return false;
    uint16_t type = (uint16_t)(f[12] << 8 | f[13]);
    if (type == 0x0806) return true;                    /* ARP stays local */
    if (type != 0x0800 || len < 34u) return false;      /* IPv4 only */
    uint32_t dst = (uint32_t)f[30] << 24 | (uint32_t)f[31] << 16 |
                   (uint32_t)f[32] << 8 | f[33];
    return !private_v4(dst);
}

void net_slirp_input(net_slirp_t *n, const uint8_t *frame, size_t len) {
    if (!n || !frame || len > 65535u) return;
    if (!allowed(n, frame, len)) { n->stats.filtered++; return; }
    n->stats.to_net++;
    slirp_input(n->slirp, frame, (int)len);
}

/* ------------------------------------------------------------ polling ---- */

static int add_poll(int fd, int events, void *opaque) {
    net_slirp_t *n = opaque;
    if (n->nfds >= MAX_POLL) return -1;
    short ev = 0;
    if (events & SLIRP_POLL_IN)  ev |= POLLIN;
    if (events & SLIRP_POLL_OUT) ev |= POLLOUT;
    if (events & SLIRP_POLL_PRI) ev |= POLLPRI;
    n->fds[n->nfds] = (struct pollfd){ .fd = fd, .events = ev };
    return (int)n->nfds++;
}

static int get_revents(int idx, void *opaque) {
    net_slirp_t *n = opaque;
    if (idx < 0 || (unsigned)idx >= n->nfds) return 0;
    short r = n->fds[idx].revents;
    int out = 0;
    if (r & POLLIN)  out |= SLIRP_POLL_IN;
    if (r & POLLOUT) out |= SLIRP_POLL_OUT;
    if (r & POLLPRI) out |= SLIRP_POLL_PRI;
    if (r & POLLERR) out |= SLIRP_POLL_ERR;
    if (r & POLLHUP) out |= SLIRP_POLL_HUP;
    return out;
}

static void run_timers(net_slirp_t *n) {
    int64_t now = now_ns() / 1000000ll;
    for (unsigned i = 0; i < MAX_TIMERS; i++) {
        net_timer_t *t = &n->timers[i];
        if (!t->used || !t->armed || t->expire_ms > now) continue;
        t->armed = false;
        slirp_handle_timer(n->slirp, t->id, t->cb_opaque);
    }
}

int net_slirp_poll(net_slirp_t *n, int timeout_ms,
                   struct pollfd *extra, unsigned nextra) {
    uint32_t timeout = timeout_ms < 0 ? UINT32_MAX : (uint32_t)timeout_ms;
    n->nfds = 0;
    slirp_pollfds_fill(n->slirp, &timeout, add_poll, n);
    /* The caller's descriptors ride along at the end of the same poll. */
    unsigned base = n->nfds;
    for (unsigned i = 0; i < nextra && n->nfds < MAX_POLL; i++)
        n->fds[n->nfds++] = extra[i];
    /* Never sleep past the next slirp timer. */
    int64_t now = now_ns() / 1000000ll;
    for (unsigned i = 0; i < MAX_TIMERS; i++) {
        const net_timer_t *t = &n->timers[i];
        if (!t->used || !t->armed) continue;
        int64_t d = t->expire_ms - now;
        if (d < 0) d = 0;
        if ((uint64_t)d < timeout) timeout = (uint32_t)d;
    }
    int wait = timeout == UINT32_MAX ? -1 : (int)timeout;
    if (timeout_ms >= 0 && wait > timeout_ms) wait = timeout_ms;
    int r = poll(n->fds, n->nfds, wait);
    for (unsigned i = 0; i < nextra && base + i < n->nfds; i++)
        extra[i].revents = n->fds[base + i].revents;
    slirp_pollfds_poll(n->slirp, r < 0, get_revents, n);
    run_timers(n);
    return r;
}

net_slirp_stats_t net_slirp_stats(const net_slirp_t *n) {
    return n->stats;
}
