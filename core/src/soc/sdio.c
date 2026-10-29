/*
 * S5L8900 SDIO host controller, plus the SDIO-visible half of the 88W8686:
 * card state, the function-0 CCCR/FBR/CIS space, the function-1 registers and
 * firmware download. The firmware's command/data protocol lives in
 * wlan8686.c. See core/include/sdio.h for the register map and its sources.
 *
 * Copyright (c) 2026 the iphone-1 contributors. GPL-2.0-or-later.
 */
#include "sdio.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { CARD_IDLE, CARD_READY, CARD_STANDBY, CARD_SELECTED };

/* One I/O function, operating range 3.2-3.4 V (OCR bits 20-21), the values
 * the isolated enumeration gate used and Apple accepted. */
#define CARD_OCR            0x00060000u
#define CARD_FUNCTIONS      1u
#define CARD_RCA            1u
#define CARD_IO_PORT        0x10000u    /* function-1 data port */

/* R5 response flags (SDIO simplified spec 1.10, 5.2). */
#define R5_STATE_CMD        0x1000u
#define R5_ERROR            0x0800u
#define R5_FUNCTION         0x0200u
#define R5_OUT_OF_RANGE     0x0100u

/* Function-1 registers (Linux libertas if_sdio.h agrees with every offset the
 * genuine driver touches). */
#define F1_IOPORT0          0x00u
#define F1_CONFIG           0x03u
#define F1_INT_MASK         0x04u
#define F1_INT_STATUS       0x05u
#define F1_RD_BASE          0x10u
#define F1_STATUS           0x20u
#define F1_SCRATCH          0x34u
#define F1_FW_STATUS        0x40u
#define F1_INT_UPLD         0x01u
#define F1_INT_DNLD         0x02u
#define WLAN_FIRMWARE_OK    0xfedcu

/* A minimal standard tuple chain: MANFID 02df:9103 (Marvell 8686), FUNCID
 * network, FUNCE 512-byte blocks at 25 MHz. Apple's driver matches on the
 * provider class rather than these bytes; they are here so a CIS walk finds a
 * well-formed chain instead of garbage. */
static const uint8_t common_cis[] = {
    0x15, 6, 1, 0, 'E', 'M', 0, 0xff,
    0x20, 4, 0xdf, 0x02, 0x03, 0x91,
    0x21, 2, 0x0c, 0,
    0x22, 4, 0, 0, 2, 0x32,
    0xff
};
#define CIS_BASE 0x1200u

void s5l_sdio_trace(const s5l_sdio_env_t *env, const char *fmt, ...) {
    va_list ap;
    if (!env || !env->trace) return;
    va_start(ap, fmt);
    fprintf(stderr, "[sdio] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
#define trace s5l_sdio_trace

void s5l_sdio_reset(s5l_sdio_t *s, bool present, const uint8_t mac[6]) {
    memset(s, 0, sizeof *s);
    s->present = present;
    if (mac) memcpy(s->mac, mac, 6);
    /* The virtual access point. Locally administered BSSID, channel 6. */
    static const uint8_t bssid[6] = { 0x02, 0x1a, 0x11, 0x00, 0x00, 0x01 };
    static const char ssid[] = "iPhone Virtual Wi-Fi";
    memcpy(s->ap_bssid, bssid, 6);
    s->ap_ssid_len = (uint8_t)(sizeof ssid - 1u);
    memcpy(s->ap_ssid, ssid, s->ap_ssid_len);
    s->ap_channel = 6;
    s->ap_rssi = -45;
    s->ap_enabled = true;
}

static void card_reset(s5l_sdio_t *s) {
    s->card_state = CARD_IDLE;
    s->rca = 0;
    s->io_enable = s->int_enable = s->bus_if = 0;
    s->fn_block_size[0] = s->fn_block_size[1] = 0;
    s->host_config = s->int_mask = s->int_status = s->int_pending = 0;
    s->int_ticks = s->data_ticks = 0;
    s->fw_stage = WLAN_FW_HELPER;
    s->fw_request = 0;
    s->fw_body_len = 0;
    s->fw_helper_bytes = s->fw_main_bytes = s->fw_frames = 0;
    s->associated = false;
    s->radio_on = false;
    s->deep_sleep = false;
    s->mac_control = 0;
    s->up_head = s->up_count = 0;
    s->data_armed = false;
}

void s5l_sdio_reset_pin(void *ctx, bool level) {
    s5l_sdio_t *s = ctx;
    (void)level;
    if (s && s->present) card_reset(s);
}

/* ------------------------------------------------------------ CMD52 ------ */

static uint8_t f0_read(s5l_sdio_t *s, uint32_t a, bool *known) {
    *known = true;
    switch (a) {
    case 0x00: return 0x00;                         /* CCCR/SDIO revision */
    case 0x02: return s->io_enable;
    case 0x03: return s->io_enable & 0x02u;         /* function 1 ready at once */
    case 0x04: return s->int_enable;
    case 0x05: return s5l_wlan_card_irq(s) ? 0x02u : 0x00u;
    case 0x06: return 0;
    case 0x07: return s->bus_if;
    case 0x08: return 0x00;                         /* card capabilities */
    case 0x09: return (uint8_t)CIS_BASE;
    case 0x0a: return (uint8_t)(CIS_BASE >> 8);
    case 0x0b: return (uint8_t)(CIS_BASE >> 16);
    case 0x10: return (uint8_t)s->fn_block_size[0];
    case 0x11: return (uint8_t)(s->fn_block_size[0] >> 8);
    case 0x100: return 0x07;                        /* standard WLAN interface */
    case 0x109: return (uint8_t)CIS_BASE;
    case 0x10a: return (uint8_t)(CIS_BASE >> 8);
    case 0x10b: return (uint8_t)(CIS_BASE >> 16);
    case 0x110: return (uint8_t)s->fn_block_size[1];
    case 0x111: return (uint8_t)(s->fn_block_size[1] >> 8);
    default: break;
    }
    if (a >= 0x200u && a <= 0x700u && !(a & 0xffu)) return 0;  /* absent fns */
    if (a >= CIS_BASE && a < CIS_BASE + sizeof common_cis)
        return common_cis[a - CIS_BASE];
    *known = false;
    return 0;
}

static bool f0_write(s5l_sdio_t *s, uint32_t a, uint8_t v) {
    switch (a) {
    case 0x02: s->io_enable = v & 0x02u; return true;
    case 0x04: s->int_enable = v & 0x03u; return true;
    case 0x06: if (v & 0x08u) card_reset(s); return true;   /* RES */
    case 0x07: s->bus_if = v; return true;
    case 0x10: s->fn_block_size[0] = (uint16_t)((s->fn_block_size[0] & 0xff00u) | v); return true;
    case 0x11: s->fn_block_size[0] = (uint16_t)((s->fn_block_size[0] & 0x00ffu) | (v << 8)); return true;
    case 0x110: s->fn_block_size[1] = (uint16_t)((s->fn_block_size[1] & 0xff00u) | v); return true;
    case 0x111: s->fn_block_size[1] = (uint16_t)((s->fn_block_size[1] & 0x00ffu) | (v << 8)); return true;
    default: return false;
    }
}

static uint8_t f1_read(s5l_sdio_t *s, uint32_t a, bool *known) {
    *known = true;
    switch (a) {
    case F1_IOPORT0:     return (uint8_t)CARD_IO_PORT;
    case F1_IOPORT0 + 1: return (uint8_t)(CARD_IO_PORT >> 8);
    case F1_IOPORT0 + 2: return (uint8_t)(CARD_IO_PORT >> 16);
    case F1_CONFIG:      return s->host_config;
    case F1_INT_MASK:    return s->int_mask;
    case F1_INT_STATUS:  return s->int_status;
    case 0x06: case 0x07: return 0;
    case F1_RD_BASE:     return (uint8_t)s->fw_request;
    case F1_RD_BASE + 1: return (uint8_t)(s->fw_request >> 8);
    case F1_STATUS:      return s5l_wlan_status(s);
    case F1_SCRATCH:     return (uint8_t)s5l_wlan_scratch(s);
    case F1_SCRATCH + 1: return (uint8_t)(s5l_wlan_scratch(s) >> 8);
    case F1_FW_STATUS:
        return s->fw_stage == WLAN_FW_RUNNING ? (uint8_t)WLAN_FIRMWARE_OK : 0;
    case F1_FW_STATUS + 1:
        return s->fw_stage == WLAN_FW_RUNNING ? (uint8_t)(WLAN_FIRMWARE_OK >> 8) : 0;
    default:
        *known = false;
        return 0;
    }
}

static bool f1_write(s5l_sdio_t *s, uint32_t a, uint8_t v) {
    switch (a) {
    case F1_CONFIG:     s5l_wlan_host_config(s, v); return true;
    case F1_INT_MASK:   s->int_mask = v & 0x0fu; return true;
    case F1_INT_STATUS:
        /* Write zero to clear: libertas acknowledges with ~cause, and Apple's
         * driver follows the same firmware contract. A pending upload is
         * re-raised by s5l_wlan_card_irq() until its packet has been read. */
        s->int_status &= v;
        return true;
    default: return false;
    }
}

static uint32_t cmd52(s5l_sdio_t *s, uint32_t arg, const s5l_sdio_env_t *env) {
    bool write = (arg >> 31) & 1u, raw = (arg >> 27) & 1u, known = true;
    unsigned fn = (arg >> 28) & 7u;
    uint32_t a = (arg >> 9) & 0x1ffffu;
    uint8_t v = (uint8_t)arg, out;
    uint32_t r = R5_STATE_CMD;

    if (fn > CARD_FUNCTIONS) return r | R5_FUNCTION;
    if (write) {
        bool ok = fn ? f1_write(s, a, v) : f0_write(s, a, v);
        if (!ok) {
            trace(env, "CMD52 write to unmodelled f%u+0x%x = 0x%02x", fn, a, v);
            return r | R5_OUT_OF_RANGE;
        }
        if (!raw) return r | v;
    }
    out = fn ? f1_read(s, a, &known) : f0_read(s, a, &known);
    if (!known) {
        trace(env, "CMD52 read of unmodelled f%u+0x%x", fn, a);
        return r | R5_OUT_OF_RANGE;
    }
    return r | out;
}

/* ------------------------------------------------------------ commands --- */

static bool card_command(s5l_sdio_t *s, unsigned idx, uint32_t arg,
                         uint32_t *resp, const s5l_sdio_env_t *env) {
    if (!s->present) return false;
    switch (idx) {
    case 0:
        card_reset(s);
        *resp = 0;
        return true;
    case 5:
        *resp = (CARD_FUNCTIONS << 28) | CARD_OCR;
        if (arg & CARD_OCR) {
            if (s->card_state < CARD_READY) s->card_state = CARD_READY;
        }
        if (s->card_state >= CARD_READY) *resp |= 0x80000000u;
        return true;
    case 3:
        if (s->card_state < CARD_READY) return false;
        s->rca = CARD_RCA;
        if (s->card_state < CARD_STANDBY) s->card_state = CARD_STANDBY;
        *resp = (uint32_t)s->rca << 16;
        return true;
    case 7:
        if (s->card_state < CARD_STANDBY) return false;
        if ((arg >> 16) != s->rca) {
            s->card_state = CARD_STANDBY;
            return false;
        }
        s->card_state = CARD_SELECTED;
        *resp = 15u << 9;              /* SDIO-only R1: CURRENT_STATE 15 */
        return true;
    case 52:
        if (s->card_state != CARD_SELECTED) return false;
        *resp = cmd52(s, arg, env);
        return true;
    case 53: {
        if (s->card_state != CARD_SELECTED) return false;
        unsigned fn = (arg >> 28) & 7u;
        uint32_t port = (arg >> 9) & 0x1ffffu;
        *resp = R5_STATE_CMD;
        if (fn != 1u || port != CARD_IO_PORT) {
            trace(env, "CMD53 to f%u port 0x%x refused (arg 0x%08x)", fn, port, arg);
            *resp |= fn != 1u ? R5_FUNCTION : R5_OUT_OF_RANGE;
            return true;
        }
        s->data_armed = true;
        s->data_write = (arg >> 31) & 1u;
        s->data_block = (arg >> 27) & 1u;
        s->data_function = (uint8_t)fn;
        s->data_count = arg & 0x1ffu;
        if (!s->data_count) s->data_count = 512u;
        return true;
    }
    default:
        trace(env, "unmodelled SD command %u arg 0x%08x", idx, arg);
        return false;
    }
}

static void execute(s5l_sdio_t *s, const s5l_sdio_env_t *env) {
    uint32_t resp = 0;
    unsigned idx = s->cmd & 63u;
    memset(s->resp, 0, sizeof s->resp);
    s->status |= SDIO_STATUS_SENT;
    if (card_command(s, idx, s->arg, &resp, env)) {
        s->resp[0] = resp;
        s->status |= SDIO_STATUS_DONE;
    } else {
        s->status |= SDIO_STATUS_ERROR;
    }
    s->cmd &= ~0x80000000u;
}

/* ------------------------------------------------------------ data ------- */

static void data_phase(s5l_sdio_t *s, const s5l_sdio_env_t *env) {
    uint32_t len;
    if (!s->data_armed) {
        trace(env, "DCTRL start with no CMD53 armed");
        s->data_errors++;
        return;
    }
    s->data_armed = false;
    if (s->data_block) {
        len = s->blklen * s->numblk;
        if (!len) len = s->data_count * s->fn_block_size[1];
    } else {
        len = s->data_count;
    }
    uint8_t *p = (env && env->ram && len) ? env->ram(env->ctx, s->baddr, len) : NULL;
    if (!p || len > 0x10000u) {
        trace(env, "DMA refused: pa 0x%08x len %u write %u", s->baddr, len, (unsigned)s->data_write);
        s->data_errors++;
        s->status |= SDIO_STATUS_ERROR;
        s->irq |= SDIO_IRQ_DONE;
        return;
    }
    if (s->data_write) {
        s5l_wlan_download(s, p, len, env);
    } else {
        memset(p, 0, len);
        if (s->up_count) {
            uint16_t n = s->up_len[s->up_head];
            memcpy(p, s->up_data[s->up_head], n < len ? n : len);
            if (n > len)
                trace(env, "upload of %u bytes truncated to %u", n, len);
            s5l_wlan_upload_consumed(s);
        } else {
            trace(env, "CMD53 read of %u bytes with nothing queued", len);
        }
        if (env->ram_written) env->ram_written(env->ctx, p, len);
    }
    s->data_ticks = SDIO_COMPLETION_TICKS;
}

void s5l_sdio_tick(s5l_sdio_t *s, uint32_t tb) {
    if (!tb) return;
    if (s->data_ticks) {
        if (tb >= s->data_ticks) {
            s->data_ticks = 0;
            s->status |= SDIO_STATUS_DATA;
            s->irq |= SDIO_IRQ_DONE;
        } else {
            s->data_ticks -= tb;
        }
    }
    if (s->int_ticks) {
        if (tb >= s->int_ticks) {
            s->int_ticks = 0;
            s->int_status |= s->int_pending;
            s->int_pending = 0;
            /* A fresh cause is a fresh edge even if an older one is still
             * unacknowledged; see s5l_sdio_irq(). */
            s->card_irq_seen = false;
        } else {
            s->int_ticks -= tb;
        }
    }
}

uint32_t s5l_sdio_next_event(const s5l_sdio_t *s) {
    uint32_t a = s->data_ticks, b = s->int_ticks;
    if (!a) return b;
    if (!b) return a;
    return a < b ? a : b;
}

/* ------------------------------------------------------------ registers -- */

uint32_t s5l_sdio_read(s5l_sdio_t *s, uint32_t off, const s5l_sdio_env_t *env) {
    (void)env;
    if (off >= SDIO_RESP0 && off < SDIO_RESP0 + 16u)
        return s->resp[(off - SDIO_RESP0) / 4u];
    switch (off) {
    case SDIO_CTRL:    return s->ctrl;
    case SDIO_DCTRL:   return s->dctrl;
    case SDIO_CMD:     return s->cmd;
    case SDIO_ARG:     return s->arg;
    case SDIO_STATE:   return 0;           /* never busy: see sdio.h */
    case SDIO_STATUS:
        return s->status |
            (((s->ctrl & 1u) && !(s->dctrl & 3u)) ? SDIO_STATUS_READY : 0u);
    case SDIO_CLKDIV:  return s->clkdiv;
    case SDIO_CSR:     return s->csr;
    case SDIO_IRQ:     return s->irq;
    case SDIO_IRQMASK: return s->irqmask;
    case SDIO_BADDR:   return s->baddr;
    case SDIO_BLKLEN:  return s->blklen;
    case SDIO_NUMBLK:  return s->numblk;
    default:           return 0;
    }
}

void s5l_sdio_write(s5l_sdio_t *s, uint32_t off, uint32_t val,
                    const s5l_sdio_env_t *env) {
    switch (off) {
    case SDIO_CTRL:
        s->ctrl = val;
        if (!(val & 1u)) s->data_armed = false;
        break;
    case SDIO_DCTRL:
        if (val & 0x10u) {
            s->dctrl = val;
            data_phase(s, env);
            s->dctrl &= ~0x10u;
        } else {
            s->dctrl = val;
            if (val & 3u) s->data_armed = false;   /* FIFO reset */
        }
        break;
    case SDIO_CMD:
        s->cmd = val;
        if ((val & 0x80000000u) && (s->ctrl & 1u)) execute(s, env);
        break;
    case SDIO_ARG:     s->arg = val; break;
    case SDIO_STAC:    s->status &= ~val; break;
    case SDIO_CLKDIV:  s->clkdiv = val; break;
    case SDIO_CSR:     s->csr = val; break;
    case SDIO_IRQ:     s->irq &= ~val; break;
    case SDIO_IRQMASK: s->irqmask = val; break;
    case SDIO_BADDR:   s->baddr = val; break;
    case SDIO_BLKLEN:  s->blklen = val; break;
    case SDIO_NUMBLK:  s->numblk = val; break;
    default:
        trace(env, "write to unmodelled controller +0x%02x = 0x%08x", off, val);
        break;
    }
}

/*
 * The card interrupt is a level inside the card (pending function-1 cause,
 * unmasked, function interrupts enabled in the CCCR) but the controller
 * LATCHES it into IRQ bit1 on the rising edge, which Apple's filter then
 * acknowledges with a write-one-to-clear. Re-latching while the level stays
 * high would re-enter the filter before the driver's work loop has run and
 * drained the cause; an edge is what lets that work loop run at all.
 */
bool s5l_sdio_irq(s5l_sdio_t *s) {
    bool level = s->present && s5l_wlan_card_irq(s);
    if (level && !s->card_irq_seen) s->irq |= SDIO_IRQ_CARD;
    s->card_irq_seen = level;
    return (s->irq & s->irqmask) != 0;
}
