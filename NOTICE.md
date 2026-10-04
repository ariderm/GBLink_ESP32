# Attribution and Source Provenance

GBLink ESP32 is a derivative work that combines and adapts functionality from the GB-Link/Celio-Link firmware family and the GBLink Netplay Bridge, together with ESP32-specific implementation work.

This file records the major upstream sources used by this project and the portions of the ESP32 implementation that are derived from or substantially based on them.

## License

This project is distributed under the GNU General Public License version 3 only (`GPL-3.0-only`). See [`LICENSE`](LICENSE) for the complete license text.

Copyright in upstream-derived portions remains with the original copyright holders. Copyright in later modifications remains with the contributors who authored those modifications.

## 1. GB-Link / Celio-Link Firmware

Upstream repositories:

- https://github.com/GB-Link/GBLink-Firmware
- https://github.com/Celio-Link/Celio-Firmware

License: GNU General Public License v3.

The GB-Link firmware repository is a fork of Celio-Link/Celio-Firmware. The upstream documentation states that the firmware originally stems from the Celio Firmware and that the GB-Link and Celio-Link repositories share the same core codebase while remaining separately maintained.

Because of that shared history, this project does not assign a single individual copyright holder to all firmware-derived code. Copyright in those portions remains with the respective Celio-Link, GB-Link, and other upstream contributors.

### Portions of GBLink ESP32 derived from or substantially based on this firmware family

The following areas of `GBLink_ESP32.ino` are derived from, adapted from, or directly informed by the upstream firmware architecture and protocol implementation:

- Game Boy Advance multiplayer SIO behavior
- Generation 3 Pokémon link handshake and section behavior
- physical-link state transitions
- handling of link/control values used during Gen 3 communication
- the split between timing-critical physical link handling and slower transport processing
- buffering/relay concepts used to decouple GBA timing from remote transport
- physical link reinitialization and session lifecycle concepts

The ESP32 implementation does not preserve the RP2040/Zephyr hardware layer directly. Instead, the hardware-facing portion has been rewritten/adapted around ESP32 GPIO, interrupts, cycle timing, and RMT.

## 2. GBLink Netplay Bridge

Upstream repository:

- https://github.com/GB-Link/gblink-netplay-bridge

License: GNU General Public License v3.

The upstream `package.json` identifies the project author as:

- Ashton Herron <ashton@gblink.io>

### Portions of GBLink ESP32 derived from or substantially based on the Netplay Bridge

The following areas of `GBLink_ESP32.ino` are derived from, adapted from, or directly informed by the GBLink Netplay Bridge:

- RetroArch/RANP network framing
- network command identifiers and command processing
- NICK / INFO / SYNC / PLAY connection sequencing
- NETPACKET transport behavior
- gpSP MPK1 packet framing
- MPK1 state values and transitions
- translation between a physical GBA link session and a gpSP multiplayer peer
- preservation and ordering of multiplayer packet/state information across the physical/network boundary

The original GBLink Netplay Bridge is a desktop application. GBLink ESP32 moves the relevant bridge behavior onto the ESP32 so a separate PC-side bridge service is not required during normal operation.

## 3. ESP32-Specific Modifications and New Work

The following areas are ESP32-specific adaptations or later modifications in this project rather than direct copies of the RP2040/desktop implementations:

- ESP32-32E / Cheap Yellow Display hardware port
- CYD GPIO pin mapping
- direct GPIO GBA link-port interface
- native ESP-IDF RMT receive path
- interrupt-driven transmit path
- CPU cycle-counter timing and compensation
- Wi-Fi SoftAP setup
- direct TCP listener on the ESP32
- physical/network FIFO integration
- diagnostics and serial instrumentation
- startup-order fixes
- preservation of peer handshake state when the network peer arrives first
- preservation of active physical sections when RetroArch enters PLAY
- silent inter-section PREINIT behavior
- room-exit teardown behavior
- physical re-handshake detection
- queue and pacing changes used by the ESP32 implementation

These modifications are marked as modifications made in 2026 in the source header, consistent with GPLv3 section 5(a).

## 4. Related Projects / Interoperability References

GBLink ESP32 is designed to interoperate with the following projects, but this notice does not imply that their source code is necessarily incorporated into this repository unless otherwise stated above:

### gpSP

- https://github.com/libretro/gpsp

The ESP32 bridge speaks the multiplayer/link protocol expected by the gpSP core.

### RetroArch

- https://github.com/libretro/RetroArch

The ESP32 bridge implements the relevant RetroArch netplay transport/session behavior needed to appear as a peer to gpSP.

## 5. No Upstream Endorsement

GBLink ESP32 is not an official GB-Link, Celio-Link, RetroArch, libretro, Nintendo, Game Freak, or The Pokémon Company product.

References to upstream project names are for attribution, provenance, compatibility, and interoperability purposes.

## 6. Preservation of More-Specific Notices

If a future contributor copies or adapts code from a particular upstream source file that contains its own copyright or license header, that header should be preserved in the resulting source or otherwise retained in a legally appropriate form.

This NOTICE file is intended to supplement, not replace, copyright and license notices contained in upstream source files.
