/*
 * S5L8900 SDIO host controller and the Marvell 88W8686 Wi-Fi card behind it.
 *
 * THE SPLIT. Apple's AppleS5L8900XSDIO drives the controller registers below;
 * IOSDIOFamily enumerates the card through it with CMD5/3/7/52; and
 * AppleMRVL868x then downloads the Marvell helper and main firmware over
 * CMD53 and talks to that firmware with the libertas-style command, event and
 * data protocol. The card's own ARM9 firmware is NOT executed: what the host
 * can observe of it (requested download lengths, the 0xfedc ready marker,
 * command responses, events and data packets) is modelled directly in
 * core/src/soc/wlan8686.c. Everything below the card's SDIO function is a
 * virtual radio: one open access point whose traffic is plain Ethernet
 * handed to the host through s5l8900_set_net_tx()/s5l8900_net_rx().
 *
 * WHERE THE REGISTER MEANINGS COME FROM. The controller offsets and the
 * command-descriptor encoding were read out of the genuine 1A543a kernel and
 * confirmed with cold-boot MMIO traces (plans/networking.md). In short:
 *
 *   +0x00 CTRL      bit0 enable, bit2 four-bit bus
 *   +0x04 DCTRL     bits0-1 FIFO reset, 0x10 start the armed data phase
 *   +0x08 CMD       bits0-5 index, 0x40 response, 0x80 write, 0x100 data,
 *                   bits16-18 response type, bit31 execute
 *   +0x0c ARG       command argument
 *   +0x10 STATE     0x10 while a command is in flight
 *   +0x14 STAC      write-one-to-clear acknowledgement of +0x18
 *   +0x18 STATUS    bit0 ready, bit2 sent, bit4 command done, bit5 data done,
 *                   bit15 response error
 *   +0x20..0x2c     response words
 *   +0x38 IRQ       write-one-to-clear: bit0 command/DMA done, bit1 card
 *   +0x3c IRQMASK
 *   +0x44/48/4c     DMA physical address, block length, block count
 *
 * Commands complete inside the store that starts them, the same policy i2c
 * and spi use here: the guest polls +0x18 for them. A data phase and a new
 * card interrupt cause do NOT: they land SDIO_COMPLETION_TICKS later. The
 * driver starts a transfer and then sleeps on its completion; an interrupt
 * that arrives before it has gone to sleep wakes nobody, and it then sleeps
 * out its whole timeout (about 80 ms of guest time per firmware frame, which
 * is what an instantaneous model measured). Real hardware takes tens of
 * microseconds, so the driver always wins that race there.
 *
 * Copyright (c) 2026 the iphone-1 contributors. GPL-2.0-or-later.
 */
#ifndef S5LBOX_SDIO_H
#define S5LBOX_SDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define S5L8900_SDIO_BASE   0x38d00000u
#define S5L8900_IRQ_SDIO    42u          /* /arm-io/sdio interrupts = {0x2a} */

/* The Wi-Fi MAC the boot path writes into the device tree's zeroed
 * /arm-io/sdio local-mac-address: locally administered, unicast. Apple's
 * driver refuses an all-zero address. */
#define S5L8900_M68_WIFI_MAC \
    ((const uint8_t[6]){ 0x02, 0x00, 0x00, 0x00, 0x01, 0x01 })

#define SDIO_CTRL       0x00u
#define SDIO_DCTRL      0x04u
#define SDIO_CMD        0x08u
#define SDIO_ARG        0x0cu
#define SDIO_STATE      0x10u
#define SDIO_STAC       0x14u
#define SDIO_STATUS     0x18u
#define SDIO_RESP0      0x20u
#define SDIO_CLKDIV     0x30u
#define SDIO_CSR        0x34u
#define SDIO_IRQ        0x38u
#define SDIO_IRQMASK    0x3cu
#define SDIO_BADDR      0x44u
#define SDIO_BLKLEN     0x48u
#define SDIO_NUMBLK     0x4cu

#define SDIO_STATUS_READY   0x0001u
#define SDIO_STATUS_SENT    0x0004u
#define SDIO_STATUS_DONE    0x0010u
#define SDIO_STATUS_DATA    0x0020u
#define SDIO_STATUS_ERROR   0x8000u

#define SDIO_IRQ_DONE       0x1u
#define SDIO_IRQ_CARD       0x2u

/* Upload (card -> host) packets wait here until the driver reads them. A slot
 * holds the four-byte SDIO header plus the largest packet the firmware sends:
 * an RxPD and a 1500-byte Ethernet payload with its LLC/SNAP header, or a scan
 * response. Sixteen slots absorb a burst without growing. */
#define WLAN_UPLOAD_SLOTS   16u
#define WLAN_UPLOAD_MAX     2048u

#define WLAN_SSID_MAX       32u

typedef enum {
    WLAN_FW_HELPER = 0,     /* receiving length-prefixed helper frames      */
    WLAN_FW_MAIN_HEADER,    /* helper running, next request is a 16-byte header */
    WLAN_FW_MAIN_BODY,      /* next request is the body that header named   */
    WLAN_FW_RUNNING         /* firmware "booted": 0xfedc and the command API */
} wlan_fw_stage_t;

typedef struct {
    /* ---- controller ---- */
    uint32_t ctrl, dctrl, cmd, arg, status;
    uint32_t resp[4];
    uint32_t clkdiv, csr, irq, irqmask;
    uint32_t baddr, blklen, numblk;
    bool     card_irq_seen;     /* card level at the last refresh, for the edge */
    /* Timebase ticks until the started data phase reports completion. */
    uint32_t data_ticks;

    /* The data phase a CMD53 armed, waiting for DCTRL 0x10. */
    bool     data_armed, data_write, data_block;
    uint8_t  data_function;
    uint32_t data_count;        /* blocks (block mode) or bytes */

    /* ---- card: SDIO common layer ---- */
    bool     present;           /* attached by the board, M68 only */
    uint8_t  card_state;        /* 0 idle, 1 ready, 2 standby, 3 selected */
    uint16_t rca;
    uint8_t  io_enable, int_enable, bus_if;
    uint16_t fn_block_size[2];

    /* ---- card: function 1 (88W8686 SDIO interface) ---- */
    uint8_t  host_config;       /* +0x03, bit1 HOST_POWER_UP */
    uint8_t  int_mask;          /* +0x04 */
    uint8_t  int_status;        /* +0x05, UPLD bit0 / DNLD bit1 */
    /* Causes raised but not yet visible, and the ticks until they are. */
    uint8_t  int_pending;
    uint32_t int_ticks;
    uint8_t  fw_stage;          /* wlan_fw_stage_t */
    uint16_t fw_request;        /* +0x10/+0x11 during download */
    uint32_t fw_body_len;       /* logical length the last header named */
    uint32_t fw_helper_bytes, fw_main_bytes, fw_frames;

    /* ---- the firmware's view of the radio ---- */
    uint8_t  mac[6];
    uint16_t mac_control;
    bool     radio_on;
    bool     deep_sleep;        /* DEEP_SLEEP accepted, awaiting HOST_POWER_UP */
    bool     associated;
    uint8_t  ap_bssid[6];
    uint8_t  ap_ssid[WLAN_SSID_MAX];
    uint8_t  ap_ssid_len;
    uint8_t  ap_channel;
    int8_t   ap_rssi;           /* dBm */
    bool     ap_enabled;        /* host policy: is the virtual AP on the air */

    /* ---- upload queue ---- */
    uint32_t up_head, up_count;
    uint16_t up_len[WLAN_UPLOAD_SLOTS];
    uint8_t  up_data[WLAN_UPLOAD_SLOTS][WLAN_UPLOAD_MAX];

    /* ---- diagnostics (serialised like the rest; they are cheap) ---- */
    uint64_t commands, unknown_commands, tx_frames, rx_frames, rx_dropped;
    uint64_t data_errors;
    uint16_t last_unknown_command;
} s5l_sdio_t;

/*
 * What the device needs from its machine for one call: guest physical memory
 * for the DMA phase, and the host's Ethernet sink. Built by machine.c per
 * access and never stored, so nothing here is a pointer that a snapshot
 * would have to account for.
 */
typedef struct {
    uint8_t *(*ram)(void *ctx, uint32_t pa, uint32_t len);
    /* Called after the device stores into `ram`'s bytes, so execution caches
     * that watch guest memory see a DMA write as they see a CPU store. */
    void     (*ram_written)(void *ctx, const uint8_t *host, uint32_t len);
    void     (*net_tx)(void *ctx, const uint8_t *frame, size_t len);
    void      *ctx;
    void      *net_ctx;
    bool       trace;
} s5l_sdio_env_t;

void     s5l_sdio_reset(s5l_sdio_t *s, bool present, const uint8_t mac[6]);
uint32_t s5l_sdio_read(s5l_sdio_t *s, uint32_t off, const s5l_sdio_env_t *env);
void     s5l_sdio_write(s5l_sdio_t *s, uint32_t off, uint32_t val,
                        const s5l_sdio_env_t *env);
/* /arm-io/sdio `function-device_reset`: GPIO 0x0607. The driver pulses it
 * before (re)enumerating the card; the card loses its firmware and SDIO
 * state on either edge. A s5l_gpio_watch() callback. */
#define S5L8900_M68_WIFI_RESET_PIN 0x0607u
void     s5l_sdio_reset_pin(void *ctx, bool level);

/* Advance pending completions by `tb` timebase ticks. */
void     s5l_sdio_tick(s5l_sdio_t *s, uint32_t tb);
/* Timebase ticks until the next pending completion, or 0 if none. */
uint32_t s5l_sdio_next_event(const s5l_sdio_t *s);
/* Recompute IRQ bit1 from the card and report the controller's VIC level. */
bool     s5l_sdio_irq(s5l_sdio_t *s);

/* About 13 us at the 6 MHz timebase: a 512-byte block at 4-bit 25 MHz. */
#define SDIO_COMPLETION_TICKS 80u

/* ---- card internals shared with wlan8686.c ---- */
void     s5l_sdio_trace(const s5l_sdio_env_t *env, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
bool     s5l_wlan_upload(s5l_sdio_t *s, uint16_t type, const uint8_t *payload,
                         size_t len);
void     s5l_wlan_download(s5l_sdio_t *s, const uint8_t *buf, size_t len,
                           const s5l_sdio_env_t *env);
uint16_t s5l_wlan_scratch(const s5l_sdio_t *s);
uint8_t  s5l_wlan_status(const s5l_sdio_t *s);
void     s5l_wlan_host_config(s5l_sdio_t *s, uint8_t value);
void     s5l_wlan_upload_consumed(s5l_sdio_t *s);
bool     s5l_wlan_card_irq(const s5l_sdio_t *s);

/* Host -> guest: one Ethernet II frame from the virtual network. Returns
 * false when the link is down or the upload queue is full (counted). */
bool     s5l_wlan_rx_frame(s5l_sdio_t *s, const uint8_t *frame, size_t len);
/* Host policy: take the virtual AP on or off the air. Turning it off while
 * the guest is associated reports a disassociation. */
void     s5l_wlan_set_ap(s5l_sdio_t *s, bool enabled);

#endif
