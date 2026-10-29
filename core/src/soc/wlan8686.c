/*
 * The Marvell 88W8686 as its host driver sees it: helper and main firmware
 * download, then the firmware's command / event / data protocol, answered
 * for one virtual open access point.
 *
 * NOTHING HERE RUNS MARVELL CODE. The real card executes the helper, which
 * pulls the main image in header/body pieces through the length it publishes
 * at function-1 +0x10, and then the main firmware, which owns the radio. Only
 * the bytes that cross the SDIO function are observable to iPhone OS, so
 * those are what is modelled: the download handshake reproduces what the
 * genuine helper requests (plans/networking.md records the 233 bodies and
 * the command-4 handoff of the 1A543a image), and the running firmware is a
 * table of command responses with the layouts Linux's libertas driver, which
 * speaks to the same firmware family, documents.
 *
 * Packets on the virtual network are Ethernet II at the host boundary. The
 * firmware's own framing is applied here: TxPD in, RxPD plus 802.3/LLC out,
 * exactly as the radio firmware would hand a received 802.11 data frame up.
 *
 * Copyright (c) 2026 the iphone-1 contributors. GPL-2.0-or-later.
 */
#include "sdio.h"
#include <string.h>

#define trace s5l_sdio_trace

#define UP_TYPE_DATA    0u
#define UP_TYPE_CMD     1u
#define UP_TYPE_EVENT   3u

#define F1_INT_UPLD     0x01u
#define F1_INT_DNLD     0x02u

#define CMD_GET_HW_SPEC         0x0003u
#define CMD_802_11_RESET        0x0005u
#define CMD_802_11_SCAN         0x0006u
#define CMD_802_11_GET_LOG      0x000bu
#define CMD_802_11_AUTHENTICATE 0x0011u
#define CMD_802_11_ASSOCIATE_OLD 0x0012u
#define CMD_802_11_GET_STAT     0x0014u
#define CMD_802_3_GET_STAT      0x0015u
#define CMD_802_11_SNMP_MIB     0x0016u
#define CMD_802_11_RADIO_CONTROL 0x001cu
#define CMD_802_11_RF_CHANNEL   0x001du
#define CMD_802_11_RF_TX_POWER  0x001eu
#define CMD_802_11_RSSI         0x001fu
#define CMD_802_11_DEAUTHENTICATE 0x0024u
#define CMD_802_11_REASSOCIATE  0x0025u
#define CMD_802_11_DISASSOCIATE 0x0026u
#define CMD_MAC_CONTROL         0x0028u
#define CMD_802_11_MAC_ADDRESS  0x004du
#define CMD_802_11_ASSOCIATE    0x0050u
#define CMD_GET_TSF             0x0080u

#define CMD_802_11_DEEP_SLEEP   0x003eu

#define EVENT_LINK_LOST_NO_SCAN 3u
#define EVENT_DEEP_SLEEP_AWAKE  16u

#define F1_STATUS_IO_RDY        0x08u
#define F1_STATUS_DL_RDY        0x01u
#define HOST_POWER_UP           0x02u

#define TLV_SSID                0x0000u
#define TLV_CHANLIST            0x0101u
#define TLV_TSFTIMESTAMP        0x0113u

#define TXPD_LEN    24u
#define RXPD_LEN    24u

static uint16_t ld16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t ld32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void st16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void st32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ------------------------------------------------------------ interrupt -- */

/* The cause becomes visible SDIO_COMPLETION_TICKS later; see sdio.h. */
static void raise(s5l_sdio_t *s, uint8_t cause) {
    s->int_pending |= cause;
    if (!s->int_ticks) s->int_ticks = SDIO_COMPLETION_TICKS;
}

bool s5l_wlan_card_irq(const s5l_sdio_t *s) {
    return (s->int_enable & 0x01u) && (s->int_enable & 0x02u) &&
           (s->int_status & s->int_mask) != 0;
}

/* ------------------------------------------------------------ uploads ---- */

bool s5l_wlan_upload(s5l_sdio_t *s, uint16_t type, const uint8_t *payload,
                     size_t len) {
    if (s->up_count >= WLAN_UPLOAD_SLOTS || len + 4u > WLAN_UPLOAD_MAX)
        return false;
    uint32_t slot = (s->up_head + s->up_count) % WLAN_UPLOAD_SLOTS;
    uint8_t *p = s->up_data[slot];
    st16(p, (uint16_t)(len + 4u));
    st16(p + 2, type);
    if (len) memcpy(p + 4, payload, len);
    s->up_len[slot] = (uint16_t)(len + 4u);
    s->up_count++;
    raise(s, F1_INT_UPLD);
    return true;
}

void s5l_wlan_upload_consumed(s5l_sdio_t *s) {
    if (!s->up_count) return;
    s->up_len[s->up_head] = 0;
    s->up_head = (s->up_head + 1u) % WLAN_UPLOAD_SLOTS;
    s->up_count--;
    if (s->up_count) raise(s, F1_INT_UPLD);
}

uint16_t s5l_wlan_scratch(const s5l_sdio_t *s) {
    if (s->fw_stage != WLAN_FW_RUNNING) return 0;
    if (s->up_count) return s->up_len[s->up_head];
    return 0xfedcu;
}

static void event(s5l_sdio_t *s, uint32_t id) {
    uint8_t b[4];
    st32(b, id);
    s5l_wlan_upload(s, UP_TYPE_EVENT, b, sizeof b);
}

/* Function-1 +0x20. Downloads complete inside the CMD53 that carries them,
 * so the card is always ready for the next one, except before the host has
 * powered it for the download and while the firmware is in deep sleep. */
uint8_t s5l_wlan_status(const s5l_sdio_t *s) {
    bool ready = s->fw_stage == WLAN_FW_RUNNING ? !s->deep_sleep
                                                : (s->host_config & HOST_POWER_UP);
    return ready ? (F1_STATUS_IO_RDY | F1_STATUS_DL_RDY) : F1_STATUS_IO_RDY;
}

/* Function-1 +0x03. HOST_POWER_UP powers the card for the download and,
 * once the firmware runs, is how the host wakes it from deep sleep: the
 * firmware answers with a DEEP_SLEEP_AWAKE event, after which the host
 * clears the register again (libertas if_sdio_exit_deep_sleep). */
void s5l_wlan_host_config(s5l_sdio_t *s, uint8_t value) {
    s->host_config = value;
    if ((value & HOST_POWER_UP) && s->deep_sleep) {
        s->deep_sleep = false;
        event(s, EVENT_DEEP_SLEEP_AWAKE);
        /* Awake with an empty download buffer. AppleMRVL868x sends nothing
         * it has queued while the card slept until this arrives. */
        raise(s, F1_INT_DNLD);
    }
}

/* ------------------------------------------------------------ the AP ----- */

/* 802.11b/g rates, basic rates flagged with 0x80. */
static const uint8_t ap_rates[8] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
static const uint8_t ap_ext_rates[4] = { 0x30, 0x48, 0x60, 0x6c };

/* The tagged elements of the AP's beacon; returns bytes written. */
static size_t ap_ies(const s5l_sdio_t *s, uint8_t *p) {
    size_t n = 0;
    p[n++] = 0; p[n++] = s->ap_ssid_len;
    memcpy(p + n, s->ap_ssid, s->ap_ssid_len); n += s->ap_ssid_len;
    p[n++] = 1; p[n++] = sizeof ap_rates;
    memcpy(p + n, ap_rates, sizeof ap_rates); n += sizeof ap_rates;
    p[n++] = 3; p[n++] = 1; p[n++] = s->ap_channel;
    p[n++] = 5; p[n++] = 4; p[n++] = 0; p[n++] = 1; p[n++] = 0; p[n++] = 0;
    p[n++] = 42; p[n++] = 1; p[n++] = 0;
    p[n++] = 50; p[n++] = sizeof ap_ext_rates;
    memcpy(p + n, ap_ext_rates, sizeof ap_ext_rates); n += sizeof ap_ext_rates;
    return n;
}

static uint64_t ap_tsf(const s5l_sdio_t *s) {
    /* Any monotonic value will do; the guest only compares beacons. */
    return 0x100000ull + s->commands * 1024u;
}

/* Does a TLV list name an SSID other than ours? An empty or absent SSID TLV
 * is a wildcard scan. */
static bool ssid_filter_excludes(const s5l_sdio_t *s, const uint8_t *tlv,
                                 size_t len) {
    size_t i = 0;
    while (i + 4u <= len) {
        uint16_t type = ld16(tlv + i), l = ld16(tlv + i + 2);
        if (i + 4u + l > len) break;
        if (type == TLV_SSID && l &&
            (l != s->ap_ssid_len || memcmp(tlv + i + 4, s->ap_ssid, l)))
            return true;
        i += 4u + l;
    }
    return false;
}

/* A scan hears the AP only on its channel. A channel-list TLV holds 7-byte
 * entries: radio type, channel, scan type, min and max dwell. AppleMRVL868x
 * scans one channel per command; no list means every channel. */
static bool channel_listed(const s5l_sdio_t *s, const uint8_t *tlv, size_t len) {
    size_t i = 0;
    while (i + 4u <= len) {
        uint16_t type = ld16(tlv + i), l = ld16(tlv + i + 2);
        if (i + 4u + l > len) break;
        if (type == TLV_CHANLIST) {
            for (size_t e = 0; e + 7u <= l; e += 7u)
                if (tlv[i + 4 + e + 1] == s->ap_channel) return true;
            return false;
        }
        i += 4u + l;
    }
    return true;
}

/* ------------------------------------------------------------ commands --- */

/*
 * `b` is the command body (after the 8-byte cmd_header) and holds `len`
 * request bytes; responses are written in place into a buffer of `cap`
 * bytes. Returns the response body length.
 */
static size_t scan(s5l_sdio_t *s, uint8_t *b, size_t len, size_t cap) {
    /* Request: bsstype(1) bssid(6) TLVs. Response: bssdescriptsize(2)
     * nr_sets(1) descriptors, then a TSF TLV with one u64 per set. */
    bool listed = s->ap_enabled;
    if (listed && len > 7u && (ssid_filter_excludes(s, b + 7, len - 7u) ||
                               !channel_listed(s, b + 7, len - 7u)))
        listed = false;
    uint8_t ies[96];
    size_t ie_len = ap_ies(s, ies);
    size_t desc = 2u + 6u + 1u + 8u + 2u + 2u + ie_len;
    if (cap < 3u + desc + 4u + 8u) listed = false;
    size_t n = 3;
    if (listed) {
        uint8_t *d = b + n;
        uint64_t tsf = ap_tsf(s);
        st16(d, (uint16_t)(desc - 2u));
        memcpy(d + 2, s->ap_bssid, 6);
        /* libertas reads this byte as a positive attenuation in dB. */
        d[8] = (uint8_t)(-s->ap_rssi);
        for (unsigned i = 0; i < 8; i++) d[9 + i] = (uint8_t)(tsf >> (8 * i));
        st16(d + 17, 100);                      /* beacon interval, TU */
        st16(d + 19, 0x0001u);                  /* ESS, open */
        memcpy(d + 21, ies, ie_len);
        n += desc;
        st16(b + n, TLV_TSFTIMESTAMP);
        st16(b + n + 2, 8);
        for (unsigned i = 0; i < 8; i++) b[n + 4 + i] = (uint8_t)(tsf >> (8 * i));
        n += 12;
    }
    st16(b, (uint16_t)(listed ? desc : 0));
    b[2] = listed ? 1 : 0;
    return n;
}

static size_t associate(s5l_sdio_t *s, uint8_t *b, size_t len, size_t cap,
                        const s5l_sdio_env_t *env) {
    /* Request: bssid(6) capability(2) listen(2) beacon(2) dtim(1) TLVs.
     * Response: capability(2) status(2) aid(2) IEs. */
    bool ok = s->ap_enabled && len >= 6u &&
              !memcmp(b, s->ap_bssid, 6);
    if (ok && len > 13u && ssid_filter_excludes(s, b + 13, len - 13u)) ok = false;
    trace(env, "associate %s", ok ? "accepted" : "refused");
    s->associated = ok;
    st16(b, 0x0001u);
    st16(b + 2, ok ? 0u : 1u);
    st16(b + 4, 0xc001u);
    size_t n = 6;
    if (cap >= n + 2u + sizeof ap_rates) {
        b[n++] = 1; b[n++] = sizeof ap_rates;
        memcpy(b + n, ap_rates, sizeof ap_rates); n += sizeof ap_rates;
    }
    return n;
}

static void fw_command(s5l_sdio_t *s, const uint8_t *req, size_t len,
                       const s5l_sdio_env_t *env) {
    uint8_t r[1024];
    if (len < 8u) return;
    uint16_t cmd = ld16(req), size = ld16(req + 2);
    if (size < 8u || size > len) size = (uint16_t)len;
    if (size > sizeof r) size = sizeof r;
    memcpy(r, req, size);
    uint8_t *b = r + 8;
    size_t blen = size - 8u, cap = sizeof r - 8u, out = blen;
    uint16_t action = blen >= 2u ? ld16(b) : 0;

    s->commands++;
    switch (cmd) {
    case CMD_802_11_DEEP_SLEEP:
        /* Asleep at once, with no reply and no download-ready interrupt
         * until the host raises HOST_POWER_UP. AppleMRVL868x retires this
         * command itself as soon as it is sent (it decrements its
         * outstanding-command count at c0385cac, as libertas completes it in
         * lbs_submit_command); an interrupt now would make its idle path
         * queue another sleep, wake the card to send it, and loop. */
        s->deep_sleep = true;
        trace(env, "firmware command 0x%04x: deep sleep", cmd);
        return;
    case CMD_GET_HW_SPEC:
        if (cap < 38u) break;
        memset(b, 0, 38);
        st16(b + 0, 2);                         /* hwifversion */
        st16(b + 2, 0x0002u);                   /* hardware version */
        st16(b + 4, 1);                         /* nr_txpd */
        st16(b + 6, 32);                        /* nr_mcast_adr */
        memcpy(b + 8, s->mac, 6);
        st16(b + 14, 0x10);                     /* region: FCC */
        st16(b + 16, 1);                        /* nr_antenna */
        st32(b + 18, 0x05000010u);              /* firmware release */
        out = 38;
        break;
    case CMD_802_11_MAC_ADDRESS:
        if (blen >= 8u) {
            if (action == 1u) memcpy(s->mac, b + 2, 6);
            else memcpy(b + 2, s->mac, 6);
        }
        break;
    case CMD_MAC_CONTROL:
        s->mac_control = action;
        break;
    case CMD_802_11_RADIO_CONTROL:
        if (blen >= 4u && action == 1u) s->radio_on = (ld16(b + 2) & 1u) != 0;
        else if (blen >= 4u) st16(b + 2, s->radio_on ? 1u : 0u);
        break;
    case CMD_802_11_RESET:
        s->associated = false;
        break;
    case CMD_802_11_SCAN:
        /* The radio is off until RADIO_CONTROL says otherwise only if the
         * driver ever turned it off; firmware boots with it on. */
        out = scan(s, b, blen, cap);
        break;
    case CMD_802_11_ASSOCIATE:
    case CMD_802_11_ASSOCIATE_OLD:
    case CMD_802_11_REASSOCIATE:
        out = associate(s, b, blen, cap, env);
        break;
    case CMD_802_11_AUTHENTICATE:
        break;
    case CMD_802_11_DEAUTHENTICATE:
    case CMD_802_11_DISASSOCIATE:
        s->associated = false;
        break;
    case CMD_802_11_RSSI:
        if (cap < 8u) break;
        st16(b + 0, (uint16_t)(95 + s->ap_rssi));   /* SNR over a -95 dBm floor */
        st16(b + 2, 95);
        st16(b + 4, (uint16_t)(95 + s->ap_rssi));
        st16(b + 6, 95);
        out = blen > 8u ? blen : 8u;
        break;
    case CMD_802_11_RF_CHANNEL:
        if (blen >= 4u && action == 0u)
            st16(b + 2, s->associated ? s->ap_channel : 1u);
        break;
    case CMD_802_11_GET_LOG:
    case CMD_802_11_GET_STAT:
    case CMD_802_3_GET_STAT:
        if (blen) memset(b, 0, blen);
        break;
    case CMD_GET_TSF:
        if (cap >= 8u) {
            uint64_t tsf = ap_tsf(s);
            for (unsigned i = 0; i < 8; i++) b[i] = (uint8_t)(tsf >> (8 * i));
            if (out < 8u) out = 8;
        }
        break;
    case CMD_802_11_SNMP_MIB:
    case CMD_802_11_RF_TX_POWER:
        /* Accept the setting; a GET returns the request's own buffer. */
        break;
    default:
        s->unknown_commands++;
        s->last_unknown_command = cmd;
        trace(env, "firmware command 0x%04x (%u body bytes) answered by echo",
              cmd, (unsigned)blen);
        break;
    }
    trace(env, "firmware command 0x%04x seq %u -> %u bytes", cmd,
          (unsigned)ld16(req + 4), (unsigned)(out + 8u));
    /* The one response whose code is not cmd|0x8000: ASSOCIATE (0x0050) is
     * answered as 0x8012, the original ASSOCIATE's code (libertas
     * CMD_RET_802_11_ASSOCIATE). AppleMRVL868x dispatches only 0x8012 to
     * its join handler; 0x8050 is an unknown response to it. */
    uint16_t ret = cmd == CMD_802_11_ASSOCIATE ? 0x8012u : (uint16_t)(cmd | 0x8000u);
    st16(r, ret);
    st16(r + 2, (uint16_t)(out + 8u));
    st16(r + 6, 0);                             /* result: success */
    if (!s5l_wlan_upload(s, UP_TYPE_CMD, r, out + 8u))
        trace(env, "upload queue full: response 0x%04x lost", cmd);
}

/* ------------------------------------------------------------ data ------- */

static void fw_tx(s5l_sdio_t *s, const uint8_t *p, size_t len,
                  const s5l_sdio_env_t *env) {
    if (len < TXPD_LEN) return;
    uint32_t loc = ld32(p + 8);
    uint16_t plen = ld16(p + 12);
    if (loc < TXPD_LEN || loc > len || plen > len - loc || plen < 14u) {
        trace(env, "malformed TxPD: location %u length %u in %u", loc, plen,
              (unsigned)len);
        return;
    }
    const uint8_t *f = p + loc;
    uint8_t eth[1600];
    size_t n = plen;
    /* 802.3 with an RFC 1042 SNAP header becomes Ethernet II. */
    if (ld16(f + 12) < 0x0600u && plen >= 22u &&
        f[14] == 0xaa && f[15] == 0xaa && f[16] == 0x03 &&
        f[17] == 0 && f[18] == 0 && f[19] == 0) {
        memcpy(eth, f, 12);
        memcpy(eth + 12, f + 20, plen - 20u);
        n = plen - 8u;
    } else {
        if (n > sizeof eth) return;
        memcpy(eth, f, n);
    }
    if (!s->associated) return;
    s->tx_frames++;
    if (env && env->net_tx) env->net_tx(env->net_ctx, eth, n);
}

bool s5l_wlan_rx_frame(s5l_sdio_t *s, const uint8_t *f, size_t len) {
    uint8_t b[RXPD_LEN + 1600];
    if (!s->present || s->fw_stage != WLAN_FW_RUNNING || !s->associated ||
        len < 14u || len > 1514u) {
        s->rx_dropped++;
        return false;
    }
    memset(b, 0, RXPD_LEN);
    uint8_t *pkt = b + RXPD_LEN;
    size_t n;
    if (ld16(f + 12) >= 0x0600u) {
        /* Ethernet II -> 802.3 + LLC/SNAP, as the radio delivers it. */
        static const uint8_t snap[6] = { 0xaa, 0xaa, 0x03, 0, 0, 0 };
        memcpy(pkt, f, 12);
        st16(pkt + 12, 0);
        memcpy(pkt + 14, snap, 6);
        memcpy(pkt + 20, f + 12, len - 12u);
        n = len + 8u;
        pkt[12] = (uint8_t)((n - 14u) >> 8);   /* 802.3 length is big-endian */
        pkt[13] = (uint8_t)(n - 14u);
    } else {
        memcpy(pkt, f, len);
        n = len;
    }
    st16(b + 0, 0x0001u);                       /* status: OK */
    b[2] = (uint8_t)(95 + s->ap_rssi);          /* SNR */
    st16(b + 4, (uint16_t)n);                   /* packet length */
    b[6] = 95;                                  /* noise floor */
    b[7] = 11;                                  /* rate index: 54 Mb/s */
    st32(b + 8, RXPD_LEN);                      /* packet offset */
    if (!s5l_wlan_upload(s, UP_TYPE_DATA, b, RXPD_LEN + n)) {
        s->rx_dropped++;
        return false;
    }
    s->rx_frames++;
    return true;
}

void s5l_wlan_set_ap(s5l_sdio_t *s, bool enabled) {
    s->ap_enabled = enabled;
    if (!enabled && s->associated) {
        s->associated = false;
        event(s, EVENT_LINK_LOST_NO_SCAN);
    }
}

/* ------------------------------------------------------------ download --- */

void s5l_wlan_download(s5l_sdio_t *s, const uint8_t *buf, size_t len,
                       const s5l_sdio_env_t *env) {
    s->fw_frames++;
    switch (s->fw_stage) {
    case WLAN_FW_HELPER: {
        /* 64-byte frames: le32 payload length, then up to 60 helper bytes.
         * A zero length ends the helper, which then starts and asks for the
         * first 16-byte header of the main image. */
        uint32_t n = len >= 4u ? ld32(buf) : 0;
        if (n) {
            s->fw_helper_bytes += n;
        } else {
            s->fw_stage = WLAN_FW_MAIN_HEADER;
            s->fw_request = 16;
            trace(env, "helper received: %u bytes in %u frames",
                  s->fw_helper_bytes, s->fw_frames);
        }
        break;
    }
    case WLAN_FW_MAIN_HEADER: {
        /* [command, address, length, check]. Command 1 copies `length`
         * bytes (the last four are its checksum); command 4 jumps. */
        uint32_t c = len >= 16u ? ld32(buf) : 0;
        if (c == 4u) {
            s->fw_stage = WLAN_FW_RUNNING;
            s->fw_request = 0;
            s->radio_on = true;
            trace(env, "main firmware received: %u bytes; firmware running",
                  s->fw_main_bytes);
        } else if (c == 1u && ld32(buf + 8) && ld32(buf + 8) <= 0xfffeu) {
            s->fw_body_len = ld32(buf + 8);
            s->fw_request = (uint16_t)s->fw_body_len;
            s->fw_stage = WLAN_FW_MAIN_BODY;
        } else {
            /* The genuine helper's error indication is an odd request,
             * which the host treats as a failed download. */
            trace(env, "unexpected main-image header command %u", c);
            s->fw_request = 17;
        }
        break;
    }
    case WLAN_FW_MAIN_BODY:
        s->fw_main_bytes += s->fw_body_len;
        s->fw_request = 16;
        s->fw_stage = WLAN_FW_MAIN_HEADER;
        break;
    case WLAN_FW_RUNNING: {
        if (len < 4u) break;
        uint16_t size = ld16(buf), type = ld16(buf + 2);
        if (size < 4u || size > len) size = (uint16_t)(len > 0xffffu ? 0xffffu : len);
        if (type == UP_TYPE_CMD) fw_command(s, buf + 4, size - 4u, env);
        else if (type == UP_TYPE_DATA) fw_tx(s, buf + 4, size - 4u, env);
        else trace(env, "download of unknown type %u", type);
        /* The buffer is free for the next download: AppleMRVL868x sends its
         * next queued command from this interrupt. A card that has just gone
         * to sleep raises nothing. */
        if (!s->deep_sleep) raise(s, F1_INT_DNLD);
        break;
    }
    default:
        break;
    }
}
