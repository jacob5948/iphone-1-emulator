# Wi-Fi: Marvell 88W8686 over SDIO, bridged to the host

**Status (2026-10-29): working in the production phone.** The genuine
`AppleS5L8900XSDIO` and `AppleMRVL868x` drivers enumerate the card, download
the original helper and main firmware, and bring up the Wi-Fi interface.
Settings → Wi-Fi lists **iPhone Virtual Wi-Fi**, the phone joins it, gets an
address over DHCP, and the original Safari loads web pages through it. This was
verified natively (`iphone1 --net user`) and in the browser through the
WebSocket relay, both from the new default phone. No Apple binary is patched.

The earlier isolated investigation that this builds on is in
[networking.md](networking.md) (milestones 057–077).

## How it fits together

```
guest TCP/IP ── AppleMRVL868x ── IOSDIOFamily ── AppleS5L8900XSDIO
                                                        │ MMIO 0x38d00000, VIC1 line 10
                                       core/src/soc/sdio.c      controller + SDIO card layer
                                       core/src/soc/wlan8686.c  firmware download + firmware protocol
                                                        │ Ethernet II frames
                                  s5l8900_set_net_tx() / s5l8900_net_rx()
                       ┌────────────────────────────────┴───────────────────────────┐
     native: tools/iphone1.c --net user                 browser: web/emu_api.c + emu-worker.js
     tools/net/net_slirp.c (libslirp NAT)                 WebSocket ── tools/net/netrelay.c
                                                                       (per-phone libslirp NAT)
```

- **Controller** (`sdio.c`): the register map derived from the driver and
  cold-boot traces (see `core/include/sdio.h`). Commands complete inside the
  store that executes them. A data phase completes, and a new card interrupt
  cause becomes visible, `SDIO_COMPLETION_TICKS` (80 timebase ticks, about
  13 µs) later; both are declared as a WFI wake source. IRQ bit 0 is
  command/DMA completion, bit 1 the card interrupt, latched on each new cause.
  DMA stores are reported through `ram_written` like CPU stores.
- **Card** (`sdio.c` + `wlan8686.c`): CMD0/3/5/7/52/53, CCCR/FBR/CIS, the
  function-1 registers libertas documents (`+0x03` HOST_POWER_UP, `+0x04/05`
  interrupt mask/status, write-zero-to-clear, `+0x10/11` requested download
  length, `+0x20` IO_RDY|DL_RDY, `+0x34/35` scratch = upload length or
  `0xfedc`). The reset GPIO `0x0607` power-cycles the card.
- **Firmware**: never executed. The helper stream is counted, then the main
  image is pulled in 16-byte-header / body pieces exactly as the genuine helper
  requests them until the command-4 handoff; then the card answers the
  firmware command protocol (libertas layouts) for one open access point:
  BSSID `02:1a:11:00:00:01`, channel 6, −45 dBm. Data downloads are TxPD +
  Ethernet; received frames are uploaded as RxPD + 802.3/LLC-SNAP, as the
  real radio delivers them.
- **Device tree**: the template's `/arm-io/sdio` has an all-zero MAC and
  all-zero `tx-calibration`; the driver rejects both. The OS 1.0 boot path
  (`bringup_os1.c`) writes MAC `02:00:00:00:01:01` and renames the calibration
  property in place, which the driver accepts as "no calibration data".

## What the genuine driver needed (and how each was found)

All of these come from the driver's own behaviour, mostly by booting with
`marvel-debug=0xffffffff` in the boot arguments (the driver then logs every
command, response and event to the serial console) and disassembling the
prelinked `AppleMRVL868x`:

1. **DEEP_SLEEP (0x003e) is not answered, and the sleeping card raises no
   download-ready interrupt.** The driver retires the command itself (it
   decrements its outstanding count at `c0385cac`). Answering it, or raising
   DNLD, makes its idle path queue another sleep, wake the card to send it,
   and loop thousands of times.
2. **Waking needs both an event and DNLD.** The host writes HOST_POWER_UP to
   function-1 `+0x03`; the card answers with event 16 (DEEP_SLEEP_AWAKE) and a
   download-ready interrupt. The driver sends queued commands only from the
   DNLD interrupt (it sets `+0x627` and calls the dispatcher at `c03830b8`);
   without it a Settings scan is queued forever behind a sleeping card.
3. **ASSOCIATE (0x0050) is answered as 0x8012**, not `0x8050`. The response
   dispatcher's jump table (`c0387ce8`) sends only `0x8012` to the join
   handler; `0x8050` is an unknown response and the join fails with
   "Unable to join the network".
4. **Scans are per channel.** Each SCAN carries a channel-list TLV (`0x0101`)
   with one channel; the AP answers only on its own channel.
5. **No interrupt before the waiter sleeps.** Completion of a data phase is
   delayed slightly; with instantaneous completion the driver can miss the
   wakeup. (In practice the main-image download speed is governed by the boot
   arguments, below.)

Commands the driver uses that are answered by echoing the request with result
0 include MAC_REG/RF_REG access (0x0019/0x001b), INACTIVITY_TIMEOUT (0x0067),
SUBSCRIBE_EVENT (0x0075), HOST_SLEEP_CFG (0x0043), ENABLE_RSN (0x002f),
SET_WEP (0x0013) and PS_MODE (0x0021). They are counted as `unknown_commands`
in the harness's `wifi:` summary line.

## Host-side behaviour that matters (2026-10-29, after first use)

Reported as freezes when unlocking and loading pages; both were reproduced
natively and fixed on the host side, with no change to the card or the
snapshot format.

- **Receive backpressure.** The card queues 16 received frames. A page
  arrives in bursts larger than that; frames handed to a full card were
  dropped, and each loss cost TCP a retransmission timeout (a 338 KB page:
  3 drops, 4 retransmissions). Hosts now hold frames until
  `s5l8900_net_rx_ready()` (`emu_net_rx_ready()` in the browser) says the card
  has room: the same page then arrives with 0 drops and 0 retransmissions.
- **Keep the access point on the air.** Taking it away from a joined phone
  makes the driver sweep all channels for it (36 scans in a burst) and then
  stay disconnected when it returns. The browser used to do exactly that at
  every start-up until the relay connected, i.e. during unlock. The access
  point is now on whenever a relay is chosen, including while the socket
  connects or reconnects; it is off only when no relay is chosen.
- **Settings scans while open.** The Settings Wi-Fi page rescans every few
  seconds of guest time (11 channels each), as real iPhones do; leaving the
  page stops it. Power-save enter/exit commands accompany each scan.

## Boot timing

With the browser's boot arguments (`debug=0x8 serial=3 rd=md0 -v`) the whole
firmware download finishes by about 0.69 billion instructions and the home
screen appears by about 2 billion. Without `serial=3`,
`IOAccessoryPortSerial::serialThread` spins on uart0 in kernel mode and each of
the 467 main-image transfers waits about 33 million instructions behind it,
so the download alone takes about 8 billion. Native experiments should
therefore always pass the documented `--cmdline`.

## Using it

- Native: `iphone1 ... --net user [--net-pcap FILE]`. libslirp is optional at
  build time (`libslirp-dev`); the harness lets the guest reach the host's own
  services at 10.0.2.2.
- Browser: run `tools/net/netrelay` somewhere reachable and choose it with the
  **Wi-Fi** button (or `?relay=wss://…`). A page on localhost uses
  `ws://localhost:8090/`. See [tools/net/README.md](../tools/net/README.md),
  including the restrictions a public relay needs.
- Join from Settings → Wi-Fi, or from the "Select a Wi-Fi Network" prompt the
  OS shows when an app needs the network.

## Snapshots and saved phones

The card's state is an optional `SDIO` section between `STUB` and `END`,
written only once the guest has touched the controller, so every existing
checkpoint and every machine that never used the card still reads and writes
byte-identical v39 files. Restoring a file without the section leaves the card
as the board attached it, never enumerated.

Phones saved before this change booted without the card, and the driver gave
up on it then, so **they have no Wi-Fi until Reset** (export first). The new
default phone, `web/firmware/home_1A543a_wifi.snap.gz.part*`, was cold-booted
with the card present. `web/manifest.json` now loads it, and all other
firmware, from the site itself (`firmware/...`) rather than from
`iphone.files.garden`, which this fork cannot upload to.

## Verification

- `core/tests/test_sdio.c`: 73 checks through the machine's own bus with the
  driver's descriptors, covering enumeration, helper and main download, the
  command round trip, per-channel scan and join, frames in both directions,
  deep sleep and wake, the reset pin and snapshot round trips.
- All 43 native suites, 124 JavaScript unit tests, the WASM smoke test and
  the interpreter/JIT whole-machine replay on the new snapshot pass.
- Cold boot → Settings → Wi-Fi → join → Safari → `http://10.0.2.2:8000/`
  rendered, natively and in Chromium through `netrelay`; DHCP, ARP and the
  HTTP exchange are visible in `--net-pcap` captures.

## Not done yet

- Only one open network is advertised; WPA (the AP side of the EAPOL
  handshake) is not modelled.
- IEEE power-save (PS_MODE) is accepted but no PS_SLEEP/PS_AWAKE events are
  generated.
- Modern HTTPS sites need TLS the 2007 SSL stack does not offer; a TLS
  compatibility gateway is separate work.
- The relay has no bandwidth limit beyond the process-per-phone cap.
