# Virtual Wi-Fi network

The emulated iPhone has a working Wi-Fi card: the original Marvell 88W8686
driver loads its firmware, Settings → Wi-Fi lists a network called
**iPhone Virtual Wi-Fi**, and the phone can join it. What lies behind that
access point is up to the host:

- **Native harness** — `iphone1 --net user` connects it to the host's network
  through [libslirp](https://gitlab.freedesktop.org/slirp/libslirp), the same
  user-mode NAT QEMU uses. No root, no TAP device.
- **Browser** — a web page cannot open raw sockets, so the page sends the
  guest's Ethernet frames over a WebSocket to **netrelay**, which runs the
  same NAT on a server.

Either way the guest sees an ordinary DHCP network: address 10.0.2.15,
gateway 10.0.2.2, DNS 10.0.2.3.

## Build

libslirp is optional; the targets below are only built when CMake finds it.

```bash
sudo apt-get install libslirp-dev
```

```bash
cmake -S . -B build && cmake --build build -j
```

This produces `build/tools/iphone1` with `--net user` support and
`build/tools/netrelay`.

## Native harness

```bash
build/tools/iphone1 --board m68 --kernel web/firmware/kernel_1A543a.macho \
  --devicetree web/firmware/devicetree_m68ap.bin --ram-mb 256 \
  --load home.snap --net user --net-pcap wifi.pcap --steps 20000000000
```

`--net-pcap` writes every frame crossing the virtual network to a file
Wireshark opens.

## Browser relay

```bash
build/tools/netrelay --port 8090
```

A page served from `localhost` uses `ws://localhost:8090/` automatically, so
`npm run serve` plus a local relay is a complete setup. Elsewhere, choose the
relay with the **Wi-Fi** button under the phone, or open the page with
`?relay=wss://relay.example.com/`. The choice is remembered in the browser;
`?relay=off` forgets it. A site can also set a default with
`<meta name="iphone-relay" content="wss://relay.example.com/">`; the GitHub
Pages workflow adds it from the repository variable `RELAY_URL`.

Each connection gets its own forked process and its own NAT, so phones never
share connections, and a crash affects one phone.

### Running a public relay

A relay that strangers can reach is an open proxy into whatever its host can
reach. netrelay is restrictive by default:

- The guest may only reach **public IPv4 addresses**. Private, loopback,
  link-local, carrier-grade NAT, multicast and reserved ranges are dropped
  (DHCP broadcast and the virtual DNS server excepted). The host's own
  loopback is never reachable through the gateway address.
  `--allow-private` lifts this for a relay you run only for yourself.
- `--allow-origin https://your.site` accepts WebSockets only from pages on
  your site. Repeat it for several origins.
- `--max-clients N` (default 16) caps concurrent phones;
  `--idle-seconds N` (default 900) disconnects silent ones.

Pages served over https must reach the relay over `wss://`. Put netrelay
behind any TLS-terminating proxy that passes WebSockets through, for example
Caddy:

```
relay.example.com {
    reverse_proxy 127.0.0.1:8090
}
```

`GET /health` answers `ok` for load-balancer checks.

## What works and what does not

- Plain HTTP pages load in the original Safari. Most modern HTTPS sites
  require TLS versions and ciphers that the 2007 SSL stack does not offer,
  so they fail to connect. That is a property of the guest, not the network.
- The access point is open (no password). Only one network is advertised.
- A restored checkpoint reconnects the relay; the guest's old TCP
  connections are gone, as they would be after real Wi-Fi drops out.
- Without a relay the access point is off the air, so Settings shows no
  networks rather than one that cannot reach anything.
