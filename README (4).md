# GBLink ESP32

An all-in-one **Game Boy Advance ↔ RetroArch/gpSP netplay bridge** built around an ESP32-32E "Cheap Yellow Display" (CYD).

This project recreates the core behavior of the GB-Link hardware/firmware and GBLink Netplay Bridge in a single ESP32 device. Instead of using:

```text
GBA
 |
GB-Link USB adapter
 |
PC running GBLink Netplay Bridge
 |
RetroArch / gpSP
```

this project reduces the path to:

```text
GBA
 |
ESP32 CYD
 |  Wi-Fi
RetroArch / gpSP
```

The ESP32 directly handles both:

1. the timing-sensitive Game Boy Advance link-port protocol; and
2. the RetroArch/gpSP network protocol used for Pokémon Generation 3 cable play.

No desktop bridge service is required during normal use.

---

## Project Status

The current implementation is confirmed working with:

- Pokémon FireRed
- Pokémon LeafGreen
- Pokémon Recharged Yellow, an Emerald-based ROM hack

The upstream GBLink Netplay Bridge documents support for the following Generation 3 titles:

- Pokémon FireRed
- Pokémon LeafGreen
- Pokémon Ruby
- Pokémon Sapphire
- Pokémon Emerald

Ruby, Sapphire, and unmodified Emerald have not necessarily been validated on this ESP32 implementation unless explicitly noted in this repository.

---

## What This Project Does

The ESP32 performs two jobs that are normally separated between the GB-Link adapter firmware and the desktop GBLink Netplay Bridge.

### Physical GBA link interface

The ESP32 connects directly to the Game Boy Advance link port and participates in the serial protocol used by Pokémon Generation 3.

The implementation includes:

- link handshake handling
- packet/command exchange
- CRC exchange
- eight-word Pokémon multiplayer rounds
- room-entry and room-exit handling
- physical re-handshake detection
- timing-sensitive transmit behavior
- RMT-based receive decoding
- interrupt-driven GPIO handling

### RetroArch / gpSP netplay interface

The ESP32 also behaves as a RetroArch-compatible network peer.

It implements the portions of the RetroArch netplay protocol required for gpSP Generation 3 Pokémon link mode, including:

- RetroArch/RANP connection setup
- nickname exchange
- core/content information exchange
- synchronization
- play-state negotiation
- NETPACKET messages
- gpSP MPK1 state messages
- MPK1 Pokémon link payload transport

The ESP32 creates its own Wi-Fi access point and listens for the emulator on TCP port:

```text
55435
```

Default SSID:

```text
GBLink-ESP32
```

The access point is currently open by design.

---

## Architecture

```text
+------------------------------------------------------+
|                    RetroArch / gpSP                  |
|                                                      |
|  Link Cable Connectivity: Pokemon Gen3 cable mode    |
+---------------------------+--------------------------+
                            |
                            | Wi-Fi / TCP 55435
                            |
+---------------------------v--------------------------+
|                     ESP32-32E CYD                    |
|                                                      |
|  RetroArch / RANP network protocol                   |
|  gpSP MPK1 link protocol                             |
|  Pokémon Gen 3 section / handshake state machine     |
|  Ordered network/physical frame queues               |
|  CRC handling                                        |
|  GPIO interrupt-driven transmit                      |
|  ESP32 RMT receive decoder                           |
+---------------------------+--------------------------+
                            |
                            | GBA Link Port
                            |
+---------------------------v--------------------------+
|                    Game Boy Advance                  |
|                                                      |
|         Physical Pokémon Generation 3 game           |
+------------------------------------------------------+
```

The network side and physical-link side are intentionally decoupled.

The GBA link interface has strict timing requirements, while Wi-Fi and TCP are nondeterministic. Incoming network rounds are therefore queued and consumed by the physical side at the appropriate point in the GBA protocol instead of directly tying network timing to link-port timing.

---

## Hardware

The current build uses an:

- ESP32-32E N4
- 2.8-inch ESP32 "Cheap Yellow Display" / CYD
- GBA/SP link-port connector
- standard wiring between the exposed CYD SPI connector and the GBA link port

The display itself is not required for the current bridge logic. The CYD is primarily being used as a convenient ESP32 development board with exposed GPIO.

---

## Wiring

Current pin mapping:

| GBA signal | ESP32 GPIO | CYD label | Direction |
|---|---:|---|---|
| SO | GPIO19 | IO19 / MISO | GBA → ESP32 |
| SI | GPIO23 | IO23 / MOSI | ESP32 → GBA |
| SC | GPIO18 | IO18 / SCK | Bidirectional/protocol clock |
| SD | GPIO27 | IO27 / CS | Bidirectional/protocol control |
| GND | GND | GND | Ground |
| Link pin 1 | — | — | Not connected |

### Important

Verify your own CYD revision before wiring.

There are several boards sold under the "Cheap Yellow Display" name, and GPIO exposure or onboard peripheral usage can differ between revisions.

Do not assume that another CYD revision has the same connector pinout.

---

## Software Requirements

### ESP32 side

The firmware is intended to be compiled using the Arduino IDE with ESP32 board support.

At minimum you need:

- Arduino IDE
- Espressif ESP32 Arduino core
- ESP32-32E compatible board configuration

The sketch uses ESP32-specific functionality including:

- Wi-Fi AP mode
- hardware cycle counter timing
- native ESP-IDF RMT support
- GPIO interrupts
- IRAM interrupt handlers

Because of this, it is not intended to be portable to a generic Arduino board without substantial changes.

---

## RetroArch / gpSP Configuration

The emulator side must use the **gpSP** core.

For FireRed, LeafGreen, and Emerald-based games, explicitly configure:

```text
Link Cable Connectivity = Link Cable - Pokemon Gen3 mode
```

Do not rely on the automatic option for these games.

The upstream GBLink Netplay Bridge notes that gpSP's automatic mode can select the Wireless Adapter/RFU implementation for Emerald, FireRed, and LeafGreen instead of the physical cable protocol.

That mode is incompatible with this bridge.

Connect RetroArch/gpSP to the ESP32's netplay host address on:

```text
TCP 55435
```

The ESP32 prints its access-point IP address to the serial console during startup.

Typical ESP32 soft-AP configurations use `192.168.4.1`, but use the address shown by the firmware rather than assuming it.

---

## Basic Use

1. Flash `GBLink_ESP32.ino` to the ESP32.
2. Connect the GBA link-port wiring.
3. Power the ESP32.
4. Open the serial monitor at:

   ```text
   115200 baud
   ```

5. Confirm that RMT and Wi-Fi initialize successfully.
6. Connect the emulator device to:

   ```text
   GBLink-ESP32
   ```

7. Configure RetroArch to use the gpSP core.
8. Set gpSP to Pokémon Generation 3 link-cable mode.
9. Connect RetroArch netplay to the ESP32's IP address on port `55435`.
10. On the physical GBA and emulator, enter the Cable Club / multiplayer area as normal.
11. Trade or battle.

The physical GBA and emulator do not have to reach the multiplayer attendant at exactly the same moment. The bridge preserves handshake state so either side can arrive first.

---

## Serial Commands

The firmware exposes several diagnostic commands over the Arduino serial monitor.

| Command | Function |
|---|---|
| `D` | Print diagnostics |
| `C` | Clear statistics |
| `R` | Reset the complete bridge state |
| `X` | Disconnect the active RetroArch connection |

These commands are useful when debugging physical-link timing, queue behavior, or emulator connection issues.

---

## Protocol Notes

Several 16-bit values are important to the physical Pokémon Generation 3 link exchange.

| Value | Role |
|---|---|
| `D15E` | Handshake / peer-not-ready response |
| `B9A0` | Link readiness / handshake word |
| `8FFF` | Transition into packet traffic |
| `5FFF` | Ready / section-close behavior depending on protocol state |
| `CAFE 0017` | Observed room-exit marker |

These values must be interpreted according to the current protocol state. For example, `5FFF` is legitimate protocol data and should not simply be treated as a universal disconnect marker.

The firmware also contains handling for:

- silent PREINIT between physical sessions
- ordered remote MPK frame queues
- early emulator handshake
- early physical-GBA handshake
- delayed room teardown
- physical re-handshake detection
- suppression of empty/zero physical frames where appropriate

---

## Timing Design

The GBA side is highly timing-sensitive.

The firmware therefore separates responsibilities between:

### GPIO interrupt path

Used where immediate response timing is required.

### ESP32 RMT receive path

Used as the authoritative receive decoder for captured GBA data and CRC behavior.

### Foreground loop

Used for:

- Wi-Fi
- TCP parsing
- RetroArch command processing
- MPK packet generation
- logging
- non-time-critical state processing

Avoid placing the following inside timing-critical interrupt paths:

- `Serial.print()`
- Wi-Fi operations
- dynamic memory allocation
- blocking delays
- TCP operations
- other high-latency library calls

Small changes in interrupt timing can break otherwise valid GBA communication.

---

## Why This Exists

GB-Link already provides an excellent hardware and software ecosystem for connecting original Game Boy hardware to modern systems.

This project explores a slightly different hardware architecture:

> Can the USB adapter and desktop translation layer be collapsed into one inexpensive ESP32 device?

For Pokémon Generation 3, the answer is yes.

The ESP32 directly reproduces the relevant physical-link behavior while simultaneously implementing the network-side behavior required by RetroArch/gpSP.

This makes it possible for a physical GBA to trade or battle with an emulator using only the ESP32 bridge between them.

---

## Upstream Projects, Provenance, and Attribution

This project is a **derivative work**, not a clean-room implementation. Substantial portions of its protocol behavior, state-machine design, packet handling, and architecture were derived from the GB-Link/Celio-Link firmware family and the GBLink Netplay Bridge.

### GB-Link / Celio-Link Firmware

Primary upstream repositories:

- https://github.com/GB-Link/GBLink-Firmware
- https://github.com/Celio-Link/Celio-Firmware

The GB-Link firmware repository identifies itself as a fork of Celio-Link/Celio-Firmware, and the two projects document that they share the same core codebase while being maintained separately.

This ESP32 project derives or adapts portions of the following behavior from that firmware family:

- Game Boy Advance multiplayer SIO behavior
- Generation 3 Pokémon physical-link handshake behavior
- physical link state/section handling
- timing-sensitive link sequencing
- the design principle of decoupling strict GBA timing from slower host/network transport
- bridge-oriented handling of GBA data between a physical console and a remote peer

Copyright in those upstream-derived portions remains with the respective GB-Link/Celio-Link authors and contributors.

### GBLink Netplay Bridge

Repository:

- https://github.com/GB-Link/gblink-netplay-bridge

The upstream project metadata identifies:

```text
Ashton Herron <ashton@gblink.io>
```

as the author of the GBLink Netplay Bridge.

This ESP32 project derives or adapts portions of the following behavior from that project:

- RetroArch/RANP framing and command handling
- NICK / INFO / SYNC / PLAY session setup
- NETPACKET transport
- gpSP MPK1 framing
- MPK1 state handling
- translation between physical GBA link state and gpSP multiplayer state
- packet ordering and session-state concepts used to connect a real GBA to a gpSP peer

### Related upstream software

The project also interoperates with and references:

- gpSP: https://github.com/libretro/gpsp
- RetroArch: https://github.com/libretro/RetroArch
- GB-Link organization: https://github.com/GB-Link

### ESP32-specific work

The ESP32 port adds or substantially adapts functionality for:

- ESP32-32E / CYD hardware
- direct GPIO connection to the GBA link port
- ESP32 RMT receive decoding
- cycle-counter-based timing
- interrupt-driven transmit behavior
- Wi-Fi SoftAP operation
- direct TCP hosting for the RetroArch peer
- ordered physical/network queue integration
- serial diagnostics and instrumentation
- additional startup-order, re-handshake, queue, and room-exit fixes

For a more detailed source-provenance statement, see [`NOTICE.md`](NOTICE.md).

---

## Relationship to GB-Link

This repository is an independent ESP32 derivative/port built from the GB-Link/Celio-Link ecosystem and the GBLink Netplay Bridge. It is **not an official GB-Link product** unless the upstream maintainers explicitly state otherwise.

The project name is descriptive of compatibility and ancestry and should not be interpreted as an endorsement by GB-Link, Celio-Link, libretro, Nintendo, Game Freak, or The Pokémon Company.

---

## License

This repository is distributed under the **GNU General Public License version 3 only**.

SPDX identifier:

```text
GPL-3.0-only
```

The repository includes the complete license text in [`LICENSE`](LICENSE).

This license choice is intentional because this project is substantially derived from GPL-3.0-licensed upstream work, including:

- GB-Link Firmware / Celio-Link Firmware
- GBLink Netplay Bridge

When redistributing this project or a modified version, preserve applicable upstream copyright and license notices, identify modified versions as modified, and provide the corresponding source as required by GPLv3.

The main source file includes a provenance/license header, and [`NOTICE.md`](NOTICE.md) records the major upstream sources and the portions of this implementation that derive from them.

Where an upstream source file contains a more specific copyright notice, that notice remains applicable to code copied or adapted from that file.

GNU GPL v3:

- https://www.gnu.org/licenses/gpl-3.0.html

This README summarizes project licensing for convenience and is not legal advice.

---

## Contributing

Contributions are welcome, particularly in the following areas:

- testing Pokémon Ruby
- testing Pokémon Sapphire
- testing stock Pokémon Emerald
- testing additional Emerald-based ROM hacks
- additional CYD board revisions
- improved diagnostics
- display-based connection/status UI
- Wi-Fi configuration UI
- reconnect robustness
- protocol documentation
- packet captures and timing measurements

Because the physical GBA protocol is timing-sensitive, changes to ISR, RMT, GPIO, or handshake logic should be tested on real hardware before being merged.

Please include the following with timing-related changes when possible:

- game/version tested
- physical console model
- ESP32 board revision
- serial diagnostics
- behavior before the change
- behavior after the change

---

## Known Limitations

Current limitations include:

- only one active RetroArch client is expected
- Wi-Fi credentials are currently hardcoded
- the default AP is open
- no display UI is currently required or provided
- support is currently focused on Pokémon Generation 3 cable mode
- compatibility outside the tested games should be considered experimental
- timing behavior may vary between ESP32 board/core revisions

---

## Development Warning

This code contains several behaviors that may initially look unnecessary but exist because of observed link-protocol ordering and timing behavior.

In particular, be careful when changing:

- `peerReadySeen`
- physical section startup
- `PREINIT`
- MPK emitter enable/disable behavior
- `B9A0` handling
- `5FFF` handling
- room-exit behavior
- CRC timing
- RMT decoding
- remote frame queue ordering
- ISR timing compensation

A cleanup that appears logically simpler can easily reintroduce ordering bugs where:

- RetroArch reaches PLAY before the physical GBA is ready;
- the GBA reaches `B9A0` before the emulator reaches MPK handshake state;
- a room closes while queued frames remain;
- the next Cable Club session inherits stale state; or
- network jitter changes the physical-link response timing.

Read the architecture comments in `GBLink_ESP32.ino` before modifying these paths.

---

## Disclaimer

Pokémon, Game Boy Advance, Game Boy, Nintendo, and related names are trademarks of their respective owners.

This is an unofficial interoperability and preservation-oriented open-source project.

It is not affiliated with, sponsored by, or endorsed by Nintendo, The Pokémon Company, Game Freak, RetroArch, libretro, or GB-Link.

