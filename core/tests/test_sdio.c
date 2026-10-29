/*
 * The SDIO controller and the 88W8686 Wi-Fi card, driven through the M68
 * machine's own bus exactly as AppleS5L8900XSDIO and AppleMRVL868x drive
 * them: the command descriptors, arguments and register offsets below are the
 * values the genuine driver wrote in cold-boot MMIO traces
 * (plans/networking.md), written as literals so the test cannot agree with
 * the model merely by sharing its constants.
 */
#include "board.h"
#include "snapshot.h"
#include "soc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) g_pass++; \
    else { \
        g_fail++; \
        printf("  FAIL %s:%d: ", __func__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

#define SDIO     0x38d00000u
#define BUF_PA   0x08100000u     /* a DMA buffer in M68 DRAM */

static s5l8900_t m;

static uint32_t rd(uint32_t off) { return m.bus.read32(m.bus.ctx, SDIO + off); }
static void wr(uint32_t off, uint32_t v) { m.bus.write32(m.bus.ctx, SDIO + off, v); }
static void settle(void) { for (int i = 0; i < 200; i++) s5l8900_tick(&m, 400u); }

/* Program, poll ready, execute, acknowledge: c04314b0..c04315bc. */
static bool command(uint32_t desc, uint32_t arg, uint32_t *resp) {
    wr(0x0c, arg);
    wr(0x08, desc);
    if (!(rd(0x18) & 1u)) return false;
    wr(0x08, rd(0x08) | 0x80000000u);
    uint32_t st = rd(0x18);
    wr(0x14, st);
    if (resp) *resp = rd(0x20);
    return (st & 0x10u) && !(st & 0x8000u);
}

static uint8_t cmd52(bool write, unsigned fn, uint32_t addr, uint8_t v) {
    uint32_t r = 0;
    uint32_t arg = (write ? 0x80000000u : 0) | (uint32_t)fn << 28 | addr << 9 | v;
    if (!command(0x00050074u, arg, &r)) return 0xee;
    return (uint8_t)r;
}

/* A block-mode CMD53 on function 1's port, then the DMA phase. */
static bool cmd53(bool write, unsigned blocks, unsigned blklen) {
    wr(0x48, blklen);
    wr(0x4c, blocks);
    wr(0x44, BUF_PA);
    wr(0x04, 3); wr(0x04, 0);
    uint32_t arg = (write ? 0x80000000u : 0) | 0x18000000u | (0x10000u << 9) | blocks;
    if (!command(write ? 0x000501f5u : 0x00050175u, arg, NULL)) return false;
    wr(0x3c, 1);
    wr(0x04, 0x10);
    settle();
    uint32_t irq = rd(0x38), st = rd(0x18);
    wr(0x38, irq & 1u);         /* completeDMA acknowledges only its own bit */
    wr(0x14, st);
    wr(0x3c, 3);
    return (irq & 1u) && (st & 0x20u);
}

static uint8_t *buf(void) { return m.ram + (BUF_PA - m.ram_base); }

static void boot_machine(void) {
    s5l8900_free(&m);
    if (!s5l_board_init(&s5l_board_m68, &m, 0)) { printf("board init failed\n"); exit(1); }
    wr(0x30, 4); wr(0x04, 3); wr(0x04, 0); wr(0x00, 1);
    wr(0x14, 0xffffffffu); wr(0x38, 0xffffffffu); wr(0x3c, 3); wr(0x34, 2);
}

static void enumerate(void) {
    uint32_t r = 0;
    CHECK(command(0x00040045u, 0, &r) && r == 0x10060000u, "CMD5(0) -> %08x", r);
    CHECK(command(0x00040045u, 0x00060000u, &r) && r == 0x90060000u, "CMD5(ocr) -> %08x", r);
    CHECK(command(0x00060043u, 0, &r) && r == 0x00010000u, "CMD3 -> %08x", r);
    CHECK(command(0x00010047u, 0x00010000u, &r) && r == 0x00001e00u, "CMD7 -> %08x", r);
    CHECK(cmd52(false, 0, 0x100, 0) == 7, "function 1 is a WLAN interface");
    cmd52(true, 0, 2, 2);                          /* IOE1 */
    CHECK(cmd52(false, 0, 3, 0) == 2, "IOR1 follows IOE1");
    cmd52(true, 0, 4, 3);                          /* IEN master + function 1 */
    cmd52(true, 1, 3, 2);                          /* HOST_POWER_UP */
    cmd52(true, 0, 0x110, 0x20); cmd52(true, 0, 0x111, 0);
}

static void download_firmware(void) {
    /* Helper: 64-byte frames with a le32 length prefix, then a 32-byte
     * zero terminator. */
    for (int i = 0; i < 3; i++) {
        memset(buf(), 0, 64);
        buf()[0] = 60;
        memset(buf() + 4, 0x40 + i, 60);
        CHECK(cmd53(true, 2, 32), "helper frame %d", i);
    }
    memset(buf(), 0, 32);
    CHECK(cmd53(true, 1, 32), "helper terminator");
    CHECK(cmd52(false, 1, 0x20, 0) == 9, "card ready for the main image");
    CHECK(cmd52(false, 1, 0x10, 0) == 16 && cmd52(false, 1, 0x11, 0) == 0,
          "helper asks for a 16-byte header");
    /* One body of 512 bytes, then the command-4 handoff. */
    memset(buf(), 0, 32);
    buf()[0] = 1; buf()[4] = 0x00; buf()[5] = 0x10; buf()[6] = 0x00; buf()[7] = 0xc0;
    buf()[8] = 0x00; buf()[9] = 0x02;
    CHECK(cmd53(true, 1, 32), "main header");
    CHECK(cmd52(false, 1, 0x10, 0) == 0x00 && cmd52(false, 1, 0x11, 0) == 0x02,
          "helper asks for the 512-byte body");
    CHECK(cmd53(true, 16, 32), "main body");
    CHECK(cmd52(false, 1, 0x10, 0) == 16, "and then the next header");
    memset(buf(), 0, 32);
    buf()[0] = 4;
    CHECK(cmd53(true, 1, 32), "handoff header");
    CHECK(cmd52(false, 1, 0x10, 0) == 0 && cmd52(false, 1, 0x11, 0) == 0,
          "the handoff requests nothing more");
    CHECK(cmd52(false, 1, 0x34, 0) == 0xdc && cmd52(false, 1, 0x35, 0) == 0xfe,
          "firmware-ready marker 0xfedc");
    cmd52(true, 0, 0x110, 0x40); cmd52(true, 0, 0x111, 0x01);   /* 320 */
    cmd52(true, 1, 4, 3);                                       /* UPLD|DNLD */
}

/* Send one firmware command and read back its response into `out`. */
static size_t fw_command(const uint8_t *body, size_t len, uint16_t cmd,
                         uint8_t *out) {
    static uint16_t seq;
    memset(buf(), 0, 320);
    uint16_t total = (uint16_t)(len + 12u);
    buf()[0] = (uint8_t)total; buf()[1] = (uint8_t)(total >> 8);
    buf()[2] = 1;
    buf()[4] = (uint8_t)cmd; buf()[5] = (uint8_t)(cmd >> 8);
    buf()[6] = (uint8_t)(len + 8u);
    buf()[8] = (uint8_t)seq; seq++;
    memcpy(buf() + 12, body, len);
    if (!cmd53(true, 1, 320)) return 0;
    settle();
    if (!(rd(0x38) & 2u)) return 0;               /* card interrupt latched */
    wr(0x38, 2);
    uint8_t cause = cmd52(false, 1, 5, 0);
    cmd52(true, 1, 5, (uint8_t)~cause);
    if (!(cause & 1u)) return 0;
    uint16_t n = (uint16_t)(cmd52(false, 1, 0x34, 0) | cmd52(false, 1, 0x35, 0) << 8);
    if (!cmd53(false, 1, 320)) return 0;
    memcpy(out, buf(), n);
    return n;
}

static void test_enumeration_and_download(void) {
    boot_machine();
    CHECK(rd(0x18) & 1u, "controller ready after enable");
    enumerate();
    download_firmware();
}

static void test_commands_scan_and_join(void) {
    uint8_t r[512], body[64];
    memset(body, 0, sizeof body);
    size_t n = fw_command(body, 8, 0x004d, r);   /* MAC_ADDRESS GET */
    CHECK(n == 20 && r[2] == 1 && r[4] == 0x4d && r[5] == 0x80 && r[10] == 0 && r[11] == 0,
          "MAC_ADDRESS answered with cmd|0x8000, result 0 (n=%zu)", n);
    CHECK(n == 20 && !memcmp(r + 14, S5L8900_M68_WIFI_MAC, 6), "the board's MAC");

    memset(body, 0, sizeof body);
    body[0] = 3;                                  /* any BSS type, wildcard */
    n = fw_command(body, 7, 0x0006, r);
    CHECK(n > 40 && r[4] == 0x06 && r[5] == 0x80 && r[14] == 1, "scan finds one network");
    const uint8_t *bss = r + 15;
    CHECK(!memcmp(bss + 2, m.sdio.ap_bssid, 6), "with the AP's BSSID");
    CHECK(bss[21] == 0 && bss[22] == 20 && !memcmp(bss + 23, "iPhone Virtual Wi-Fi", 20),
          "and its SSID element");

    /* One channel per scan, as AppleMRVL868x sends them. */
    static const uint8_t chan1[] = { 3, 0,0,0,0,0,0, 0x01,0x01, 7,0, 0, 1, 0, 0,0, 0x6e,0 };
    uint8_t chan6[sizeof chan1];
    memcpy(chan6, chan1, sizeof chan1); chan6[12] = 6;
    n = fw_command(chan1, sizeof chan1, 0x0006, r);
    CHECK(n >= 15 && r[14] == 0, "nothing heard on channel 1");
    n = fw_command(chan6, sizeof chan6, 0x0006, r);
    CHECK(n > 40 && r[14] == 1, "the AP answers on channel 6");

    memset(body, 0, sizeof body);
    memcpy(body, m.sdio.ap_bssid, 6);
    n = fw_command(body, 13, 0x0050, r);
    CHECK(n >= 18 && r[4] == 0x12 && r[5] == 0x80, "answered as 0x8012");
    CHECK(n >= 18 && r[14] == 0 && r[15] == 0, "association status 0");
    CHECK(s5l8900_wifi_associated(&m), "the card is associated");
}

static uint8_t g_tx[1600];
static size_t g_tx_len;
static unsigned g_tx_count;
static void capture_tx(void *ctx, const uint8_t *f, size_t len) {
    (void)ctx;
    memcpy(g_tx, f, len); g_tx_len = len; g_tx_count++;
}

static void test_frames_cross_in_both_directions(void) {
    s5l8900_set_net_tx(&m, capture_tx, NULL);
    /* Guest -> network: TxPD then Ethernet II. */
    memset(buf(), 0, 320);
    uint8_t *p = buf();
    uint16_t len = 4 + 24 + 60;
    p[0] = (uint8_t)len; p[2] = 0;                /* type 0: data */
    p[4 + 8] = 24;                                /* tx_packet_location */
    p[4 + 12] = 60;                               /* tx_packet_length */
    uint8_t *eth = p + 4 + 24;
    memset(eth, 0xff, 6); memcpy(eth + 6, S5L8900_M68_WIFI_MAC, 6);
    eth[12] = 0x08; eth[13] = 0x00; eth[14] = 0x45;
    CHECK(cmd53(true, 1, 320), "data download");
    CHECK(g_tx_count == 1 && g_tx_len == 60 && g_tx[12] == 0x08 && g_tx[14] == 0x45,
          "Ethernet II frame reached the host (%u, %zu)", g_tx_count, g_tx_len);

    /* Network -> guest: RxPD then 802.3 + LLC/SNAP. */
    uint8_t in[64];
    memset(in, 0, sizeof in);
    memcpy(in, S5L8900_M68_WIFI_MAC, 6);
    in[6] = 0x52; in[12] = 0x08; in[13] = 0x06; in[14] = 0x00; in[15] = 0x01;
    CHECK(s5l8900_net_rx_ready(&m), "an associated card with room is ready");
    CHECK(s5l8900_net_rx(&m, in, sizeof in), "frame accepted while associated");
    settle();
    CHECK(rd(0x38) & 2u, "card interrupt for the received frame");
    wr(0x38, 2);
    uint8_t cause = cmd52(false, 1, 5, 0);
    cmd52(true, 1, 5, (uint8_t)~cause);
    uint16_t n = (uint16_t)(cmd52(false, 1, 0x34, 0) | cmd52(false, 1, 0x35, 0) << 8);
    CHECK((cause & 1u) && n == 4 + 24 + 64 + 8, "upload length %u", n);
    CHECK(cmd53(false, 1, 320), "upload read");
    const uint8_t *u = buf();
    CHECK(u[2] == 0 && u[4 + 8] == 24, "type data, packet after the RxPD");
    const uint8_t *f = u + 4 + 24;
    CHECK(!memcmp(f, S5L8900_M68_WIFI_MAC, 6) && f[12] == 0 && f[13] == 64 - 14 + 8 &&
          f[14] == 0xaa && f[15] == 0xaa && f[16] == 0x03 && f[20] == 0x08 && f[21] == 0x06,
          "802.3 length and SNAP header carry the ARP ethertype");
}

static void test_a_full_card_is_not_ready(void) {
    uint8_t in[64];
    memset(in, 0, sizeof in);
    in[12] = 0x08;
    unsigned taken = 0;
    while (s5l8900_net_rx_ready(&m) && taken < 100u) {
        CHECK(s5l8900_net_rx(&m, in, sizeof in), "accepted while ready");
        taken++;
    }
    CHECK(taken == WLAN_UPLOAD_SLOTS, "ready for exactly the free slots (%u)", taken);
    uint64_t dropped = m.sdio.rx_dropped;
    CHECK(!s5l8900_net_rx(&m, in, sizeof in) && m.sdio.rx_dropped == dropped + 1u,
          "a frame forced on a full card is dropped and counted");
    /* Drain what the guest would read, so later tests start empty. */
    while (m.sdio.up_count) {
        settle();
        wr(0x38, rd(0x38));
        cmd52(true, 1, 5, 0);
        cmd53(false, 1, 320);
    }
    CHECK(s5l8900_net_rx_ready(&m), "ready again once the guest has read them");
}

static void test_deep_sleep_and_wake(void) {
    uint8_t r[512];
    memset(buf(), 0, 320);
    buf()[0] = 12; buf()[2] = 1; buf()[4] = 0x3e; buf()[6] = 8;
    CHECK(cmd53(true, 1, 320), "DEEP_SLEEP download");
    settle();
    CHECK(m.sdio.up_count == 0 && !(m.sdio.int_status & 2u),
          "deep sleep is neither answered nor followed by download-ready");
    CHECK(cmd52(false, 1, 0x20, 0) == 8, "a sleeping card is not ready for downloads");
    wr(0x38, rd(0x38));
    cmd52(true, 1, 5, 0);
    cmd52(true, 1, 3, 2);                          /* HOST_POWER_UP wakes it */
    settle();
    CHECK(rd(0x38) & 2u, "wake raises a card interrupt");
    wr(0x38, 2);
    cmd52(true, 1, 5, 0);
    uint16_t n = (uint16_t)(cmd52(false, 1, 0x34, 0) | cmd52(false, 1, 0x35, 0) << 8);
    CHECK(n == 8 && cmd53(false, 1, 320), "event upload");
    memcpy(r, buf(), 8);
    CHECK(r[2] == 3 && r[4] == 16, "DEEP_SLEEP_AWAKE event");
    cmd52(true, 1, 3, 0);
    CHECK(cmd52(false, 1, 0x20, 0) == 9, "awake card is ready again");
}

static void test_reset_pin_clears_the_card(void) {
    s5l_sdio_reset_pin(&m.sdio, false);
    CHECK(m.sdio.fw_stage == WLAN_FW_HELPER && m.sdio.card_state == 0 && !m.sdio.associated,
          "reset forgets firmware, selection and association");
    uint32_t r = 0;
    CHECK(command(0x00040045u, 0, &r) && r == 0x10060000u, "the card enumerates afresh");
}

static void test_snapshots(void) {
    /* A machine whose guest never touched the card writes no SDIO section. */
    static s5l8900_t fresh;
    if (!s5l_board_init(&s5l_board_m68, &fresh, 0)) { g_fail++; return; }
    size_t a = 0, b = 0;
    uint8_t *untouched = NULL, *used = NULL;
    CHECK(snapshot_save_mem(&fresh, &untouched, &a) == SNAP_OK, "save untouched");

    boot_machine();
    enumerate();
    download_firmware();
    CHECK(snapshot_save_mem(&m, &used, &b) == SNAP_OK, "save enumerated");
    CHECK(b > a, "the enumerated phone carries an SDIO section (%zu > %zu)", b, a);

    /* Round trip: firmware state survives. */
    CHECK(snapshot_load_mem(&fresh, used, b) == SNAP_OK, "load enumerated");
    CHECK(fresh.sdio.fw_stage == WLAN_FW_RUNNING && fresh.sdio.card_state == 3,
          "firmware and selection restored");
    /* And a section-less file resets a running card to unenumerated. */
    CHECK(snapshot_load_mem(&fresh, untouched, a) == SNAP_OK, "load untouched");
    CHECK(fresh.sdio.present && fresh.sdio.fw_stage == WLAN_FW_HELPER &&
          fresh.sdio.card_state == 0, "card back to its board-attached state");
    free(untouched); free(used);
    s5l8900_free(&fresh);
}

int main(void) {
    printf("SDIO controller and 88W8686 Wi-Fi card tests\n");
    test_enumeration_and_download();
    test_commands_scan_and_join();
    test_frames_cross_in_both_directions();
    test_a_full_card_is_not_ready();
    test_deep_sleep_and_wake();
    test_reset_pin_clears_the_card();
    test_snapshots();
    s5l8900_free(&m);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
