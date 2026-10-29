/*
 * The virtual Wi-Fi network's far end: a user-mode NAT (libslirp) that turns
 * the guest's Ethernet frames into ordinary host sockets.
 *
 * The guest sees a 10.0.2.0/24 network: gateway 10.0.2.2, DNS 10.0.2.3 and a
 * DHCP pool from 10.0.2.15. The host's own loopback is reachable through the
 * gateway address only when the policy asks for it, and IPv6 is off because
 * iPhone OS 1.0 does not use it.
 *
 * Used by the native harness (tools/iphone1.c, --net user) and by the
 * browser relay (tools/net/netrelay.c). Neither the emulator core nor the
 * relay protocol knows about slirp; both just move Ethernet frames.
 */
#ifndef IPHONE1_NET_SLIRP_H
#define IPHONE1_NET_SLIRP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct net_slirp net_slirp_t;

/* A frame for the guest. Called from net_slirp_poll() and net_slirp_input(). */
typedef void (*net_slirp_out_fn)(void *ctx, const uint8_t *frame, size_t len);

typedef struct {
    /*
     * Refuse guest traffic to private, loopback, link-local, carrier-NAT,
     * multicast and reserved IPv4 destinations (DHCP broadcast and the
     * virtual network's own DNS server excepted). A relay that strangers can
     * reach must set this, or a guest could probe the relay's own network.
     * DNS answers are not filtered; the connection that follows is.
     */
    bool public_only;
    /*
     * Let the guest reach the host's own loopback services through the
     * gateway address 10.0.2.2 (slirp's default). Handy for a native harness
     * testing against a local web server; never for a shared relay.
     */
    bool host_loopback;
    /* Optional diagnostic sink for slirp's guest-error messages. */
    void (*log)(void *ctx, const char *msg);
} net_slirp_policy_t;

net_slirp_t *net_slirp_new(net_slirp_out_fn out, void *ctx,
                           const net_slirp_policy_t *policy);
void         net_slirp_free(net_slirp_t *n);

/* One Ethernet II frame from the guest. */
void         net_slirp_input(net_slirp_t *n, const uint8_t *frame, size_t len);

/*
 * Service host sockets and timers, waiting at most `timeout_ms` (0 polls and
 * returns). Extra descriptors the caller wants watched in the same poll can be
 * passed in `extra`/`nextra`; their revents are filled in on return.
 */
struct pollfd;
int          net_slirp_poll(net_slirp_t *n, int timeout_ms,
                            struct pollfd *extra, unsigned nextra);

/* Counters for diagnostics. */
typedef struct {
    uint64_t to_net, to_guest, filtered;
} net_slirp_stats_t;
net_slirp_stats_t net_slirp_stats(const net_slirp_t *n);

#endif
