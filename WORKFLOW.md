# Development Workflow and AI-Assisted Engineering Disclosure

## Purpose

This document records how **GBLink ESP32** was developed.

The project was not independently engineered from first principles by the repository owner. Instead, it was created through an iterative, AI-assisted development process using **ChatGPT**, with existing open-source projects as technical source material.

The repository owner primarily acted as:

- project initiator
- requirements owner
- hardware integrator
- test operator
- source of physical measurements and logs
- reviewer of proposed behavior
- decision-maker for project direction
- validator of whether generated builds actually worked on real hardware

ChatGPT primarily acted as:

- source-code analyst
- protocol researcher/interpreter
- implementation generator
- debugging assistant
- architecture proposer
- code modifier
- documentation author

The implementation also derives substantially from the existing GPL-licensed **GB-Link Firmware**, **Celio-Link Firmware lineage**, and **GBLink Netplay Bridge**. This was therefore not a clean-room protocol implementation.

This file exists to make that development history explicit.

---

# High-Level Development Model

The project followed a repeated loop:

```text
User defines goal or reports observed behavior
                    |
                    v
ChatGPT analyzes source material, logs, and protocol behavior
                    |
                    v
ChatGPT proposes a hypothesis or generates a new Arduino sketch
                    |
                    v
User compiles/flashes the sketch to real ESP32 hardware
                    |
                    v
User tests against a physical GBA and/or RetroArch/gpSP
                    |
                    v
User returns serial output, failures, and observed game behavior
                    |
                    v
ChatGPT revises the implementation
                    |
                    +----------------------+
                                           |
                                           v
                                  Repeat until working
```

The physical hardware and software behavior were therefore validated by actual user testing, but most code-generation and protocol reasoning were performed through ChatGPT.

---

# Upstream Technical Sources

The implementation was developed by studying and adapting behavior from the following projects.

## GB-Link Firmware

https://github.com/GB-Link/GBLink-Firmware

This provided important reference material for:

- Game Boy Advance serial link behavior
- Pokémon Generation III multiplayer communication
- link-state behavior
- physical-side protocol handling
- timing-sensitive interaction with original hardware
- the design principle of decoupling strict console timing from slower external communication

GB-Link Firmware itself shares lineage with Celio-Link/Celio-Firmware.

## Celio-Link Firmware

https://github.com/Celio-Link/Celio-Firmware

This is relevant because the GB-Link firmware history derives from the Celio firmware family.

The physical-link portions of this project should therefore be viewed as part of that larger GPL-derived code and design lineage rather than as an independently discovered protocol.

## GBLink Netplay Bridge

https://github.com/GB-Link/gblink-netplay-bridge

This was a major source for the network side of the implementation, including:

- RetroArch netplay framing
- RANP message handling
- gpSP MPK1 messages
- bridge state behavior
- translating physical link activity into emulator-side link activity

The upstream project metadata identifies **Ashton Herron** as its author.

## gpSP

https://github.com/libretro/gpsp

gpSP is the emulator core on the other side of the connection and implements the Pokémon Generation III link-cable functionality consumed by this bridge.

## RetroArch

https://github.com/libretro/RetroArch

RetroArch provides the network transport/session framework used by gpSP netplay.

---

# Development Timeline

The following timeline summarizes the major development stages represented by the ChatGPT conversations that led to the current implementation.

Dates are based on the development conversations and are intended as an engineering history rather than a formal release history.

---

# Phase 1 — Establishing Physical GBA Communication

## Initial Goal

The original goal was broader than the final implementation:

> Connect a physical Pokémon FireRed Game Boy Advance to an emulator and eventually eliminate the need for conventional Nintendo wireless hardware or a separate PC bridge.

The available hardware was an ESP32-based **2.8-inch Cheap Yellow Display (CYD)** using an **ESP32-32E N4**.

The GBA link connector was wired directly to GPIO.

The user supplied the physical wiring and performed the real-hardware tests.

### Confirmed Wiring

| GBA signal | ESP32 GPIO |
|---|---:|
| SO | GPIO19 |
| SI | GPIO23 |
| SC | GPIO18 |
| SD | GPIO27 |
| GND | GND |

The ESP32 was powered independently by USB. GBA link-port power was not used as the ESP32 supply.

---

## Passive Observation Before Active Emulation

The first debugging approach was intentionally conservative.

Instead of immediately attempting to emulate the other GBA, the development process first focused on observing the physical link and determining whether the selected pins and timing were correct.

ChatGPT proposed test sketches.

The user:

1. compiled them in Arduino IDE;
2. flashed them to the CYD;
3. connected the physical GBA;
4. navigated through Pokémon menus;
5. returned serial captures.

Early activity in the Pokémon Wireless Club produced clock estimates around several hundred kilohertz.

This initially appeared useful, but analysis suggested that the game was probing the Wireless Adapter/RFU path rather than the conventional multiplayer cable protocol.

The test procedure was therefore redirected to the **Cable Club**.

This was an important example of the development method:

- ChatGPT formed a hypothesis from the signal logs.
- The user changed the physical test procedure.
- New measurements determined whether the hypothesis was useful.

---

# Phase 2 — Building a Working Pokémon Gen III Physical Peer

Once Cable Club activity was isolated, the work shifted from passive capture to active participation as a GBA link peer.

The implementation was iterated through a large number of experimental sketches.

These versions should be viewed as disposable engineering probes rather than polished releases.

---

## Timing Calibration

A recurring timing configuration emerged:

```text
CPU clock:   240 MHz
BIT_CYCLES:  2083
EDGE_COMP:   approximately 781 cycles in early successful physical tests
```

Later bridge work refined compensation behavior further, but the 2083-cycle bit interval became an important baseline.

The user supplied logs showing that the ESP32 could communicate reliably enough to enter Pokémon's normal link protocol.

---

## Important Physical Protocol Words

Several recurring values were identified through source study and real-hardware traces.

Examples include:

```text
D15E
B9A0
8FFF
5FFF
```

These values were not discovered solely by ChatGPT or solely by measurement.

Their interpretation came from a combination of:

- upstream GB-Link/Celio implementation behavior;
- protocol source analysis;
- serial captures from the user's physical GBA;
- repeated modification and validation.

A key lesson from this phase was that individual values could not always be treated as universal commands. Their meaning depended on the current Pokémon link state.

---

# Phase 3 — TradeSetup Experiments

One major intermediate milestone was reaching Pokémon's trade interface with a locally emulated peer before the final network bridge existed.

## v1.9 Family

A v1.9-series physical implementation reached a significant milestone.

A successful test reported approximately:

```text
6,929 transfers
1 invalid transfer
0 timeouts
```

It successfully traversed:

- both setup blocks;
- six movement exchanges;
- the closing `5FFF` exchange;
- without requiring the previous fallback behavior.

The user then reported that the physical game reached the **trade terminal** before later crashing.

That observation was extremely important.

It demonstrated that the physical-side exchange was sufficiently valid for Pokémon itself to accept the earlier setup protocol.

The failure was therefore no longer merely "link communication does not work."

The problem had moved deeper into the **TradeConnection** stage.

This is characteristic of the overall development process: game behavior on real hardware was treated as a functional test, not merely serial output.

---

# Phase 4 — TradeConnection Reverse Engineering

The next iterations attempted to progress beyond TradeSetup.

ChatGPT generated experimental handlers based on upstream behavior and observed data.

The user repeatedly tested them.

---

## v2.0 Direction

The v2.0 experiment retained the known-working physical timing and added behavior around:

```text
5FFF -> reconnect behavior
B9A0 -> delayed transition
8FFF
link type 0x1122
LinkPlayer exchange
party blocks
mail data
ribbon data
```

Placeholder Pokémon data was used during some experiments because the goal at that point was not to create a useful fake trade partner.

The goal was to determine which transport/state sequence FireRed expected.

This experiment showed that the setup phase could close correctly, while the connection initialization still failed to complete.

Repeated frames containing values such as:

```text
CAFE 0011
```

also became useful diagnostic landmarks.

---

# Phase 5 — DDDD / AABB / CCDD Experimental Probes

The next series of sketches intentionally stopped trying to guess the entire trade protocol.

Instead, the code was modified to observe smaller state transitions.

This included versions around the v3.x series.

Examples of observed negotiation included sequences such as:

```text
AABB -> DDDD
BBBB -> BBBB -> CCDD
```

The implementation used bounded retry behavior so that a malformed or missed exchange did not permanently wedge the ESP32.

One version allowed a limited number of failed commands followed by a short recovery interval.

The user repeatedly returned traces showing that:

- LinkPlayer exchange could complete;
- local party blocks could progress;
- peer-side data capture could still be incomplete;
- a valid full trade had not yet been achieved.

This stage is important to the project history because ChatGPT was not simply producing a correct solution in one pass.

Many generated protocol hypotheses were incomplete or wrong.

The user functioned as the physical validation loop that rejected those hypotheses.

---

# Phase 6 — Changing the Goal from a Fake Peer to a Real Bridge

The most important architectural shift occurred when the project stopped trying to fully emulate a second Pokémon cartridge locally.

Instead, the target became:

> Use the ESP32 only as the physical GBA endpoint and directly bridge that link to a real gpSP instance running the other game.

The user explicitly directed the project toward:

- no PC-hosted translation service;
- same-network emulator connectivity;
- Arduino IDE;
- use of the already-proven physical pins/timing;
- preservation of the known-good GBA communication behavior.

This dramatically simplified the conceptual role of the ESP32.

The emulator could supply the actual remote Pokémon game state.

The ESP32 only needed to transport and translate it correctly.

---

# Phase 7 — Recreating the GB-Link + Netplay Bridge Stack Inside the ESP32

The architecture was then reframed around the upstream GB-Link ecosystem.

The conventional path is approximately:

```text
Physical GBA
    |
GB-Link hardware/firmware
    |
USB
    |
Computer running GBLink Netplay Bridge
    |
RetroArch / gpSP
```

The new target was:

```text
Physical GBA
    |
ESP32 CYD
    |
Wi-Fi / TCP
    |
RetroArch / gpSP
```

The ESP32 therefore had to combine two previously separate responsibilities:

1. timing-sensitive physical GBA communication;
2. RetroArch/gpSP network-bridge behavior.

---

# Architectural Decision: Do Not Tunnel Raw GPIO Timing Over Wi-Fi

An important design decision was to avoid trying to reproduce individual GBA clock/data edges over Wi-Fi.

That would make physical correctness dependent on network latency and jitter.

Instead, the design follows the same general principle seen in GB-Link:

- satisfy the GBA's immediate timing locally;
- decode complete link rounds locally;
- move complete logical frames through queues;
- translate those frames to/from emulator network packets asynchronously.

This became the core architecture of the final project.

---

# Phase 8 — RetroArch / RANP Implementation

ChatGPT analyzed the GBLink Netplay Bridge and generated ESP32 equivalents of the network protocol.

The resulting implementation includes behavior corresponding to RetroArch session setup such as:

```text
NICK
INFO
SYNC
PLAY
MODE
NETPACKET
```

and the associated RANP framing.

The user did not manually derive these packet structures.

They were incorporated into the generated code through source analysis and adaptation of the upstream bridge implementation.

---

# Phase 9 — gpSP MPK1 Implementation

The bridge also needed to speak the gpSP Pokémon link protocol transported inside RetroArch NETPACKET messages.

This resulted in support for behavior including:

```text
MPK1
PREINIT
HANDSHAKE
CONNECTED
HAS_DATA
```

The final sketch therefore maintains separate but related states for:

- the physical GBA;
- the remote gpSP peer;
- whether an active Pokémon multiplayer section exists.

This state-machine behavior is strongly derived from the GBLink Netplay Bridge architecture and then adapted for ESP32 execution.

---

# Phase 10 — First Combined ESP32 Bridge Builds

The user requested complete Arduino sketches that could be copied directly into Arduino IDE.

ChatGPT generated the combined bridge code.

The process immediately encountered normal generated-code issues.

One example was:

```text
error: 'LinkFrame' does not name a type
```

in:

```cpp
void printFrame(const LinkFrame &frame)
```

The source ordering/prototype problem was then corrected and a new sketch generated.

This is an important part of the disclosure:

**The generated code was not assumed to be correct simply because ChatGPT produced it.**

It was repeatedly:

- compiled;
- rejected by the compiler;
- corrected;
- flashed;
- tested;
- rejected by hardware behavior;
- modified again.

---

# Phase 11 — Verifying the Physical/Network Handshake

Successful traces eventually demonstrated a physical progression including:

```text
D15E
B9A0
8FFF
```

and entry into packet/command mode.

The logs also showed the network/MPK side reaching:

```text
CONNECTED
```

This was the first strong evidence that both halves of the recreated bridge were operating at the same time.

---

# Phase 12 — CRC and Timing Debugging

CRC failures became another significant debugging topic.

At one point, failed CRC values differed by exactly:

```text
0x8000
```

This pattern suggested a single high-order-bit sampling/timing issue rather than random corruption.

The user supplied repeated logs.

ChatGPT analyzed those patterns and altered receive/timing logic.

The receive architecture eventually settled on an important split:

- immediate GPIO interrupt behavior for time-critical transmit response;
- ESP32 RMT capture as the authoritative receive/decode path.

This allowed the CPU to respond quickly while still obtaining a stable complete received word/frame.

---

# Phase 13 — Network/Physical Queue Decoupling

Real network transport does not operate at deterministic GBA timing.

The final design therefore uses ordered buffering between the two domains.

Important later fixes included:

- fixed-size remote frame queues;
- preserving strict frame order;
- only emitting MPK traffic when an active physical section exists;
- pacing outbound MPK traffic;
- heartbeat behavior;
- suppressing meaningless all-zero frames where appropriate.

One implementation used a large ordered FIFO rather than trying to consume network data directly from an interrupt.

This was a practical adaptation to Wi-Fi jitter and task scheduling on ESP32.

---

# Phase 14 — Startup Ordering Problems

Another class of bugs appeared when the two endpoints reached readiness in different orders.

For example:

- the emulator could reach MPK handshake state before the physical GBA;
- the GBA could reach its physical ready state before the emulator.

Early implementations implicitly assumed one ordering.

Real testing showed that assumption was invalid.

Later revisions therefore preserved readiness state across the transition rather than discarding it.

One particularly important fix involved retaining a previously observed peer-ready condition rather than requiring it to occur again after another state transition.

This allowed either side to arrive first.

---

# Phase 15 — PREINIT and Section Lifecycle

Another subtle issue involved what should happen between multiplayer sessions.

The final behavior includes a **silent PREINIT** concept.

The network side should not continuously send state/data merely because a TCP connection exists.

Instead, Pokémon section activity controls when MPK communication becomes active.

Related fixes included:

- preventing ordinary `5FFF` traffic from incorrectly resetting the whole network state;
- distinguishing section completion from connection loss;
- resetting physical state without unnecessarily destroying the RetroArch session.

---

# Phase 16 — Room Exit Handling

Room-exit behavior required its own debugging.

An observed marker included:

```text
CAFE 0017
```

Immediate teardown could truncate legitimate final traffic.

The implementation was therefore changed to delay cleanup long enough for outstanding frames to drain.

This is another example where the final implementation reflects observed hardware/game behavior rather than a purely theoretical state machine.

---

# Phase 17 — Re-Handshake Detection

A physical GBA can begin a new Cable Club session without necessarily recreating the whole network connection.

The final bridge therefore detects a new physical handshake and resets the correct physical/section state while preserving the appropriate higher-level connection state.

Without this behavior, a second trade/battle session could inherit stale information from the previous one.

---

# Phase 18 — Final Confirmed Behavior

The resulting implementation was reported working with:

- Pokémon FireRed
- Pokémon LeafGreen
- Pokémon Recharged Yellow

Recharged Yellow is an Emerald-based ROM hack.

The project therefore demonstrated the original design goal:

> A physical GBA can participate in Pokémon Generation III cable communication with an emulator through a single ESP32 CYD, without a computer running the normal GBLink Netplay Bridge service.

---

# What the Repository Owner Contributed

The statement that the owner "did not engineer this personally" should not be interpreted to mean there was no meaningful human contribution.

The owner supplied the project's real-world constraints and performed the work that an AI model could not independently perform.

This included:

## Goal Definition

The owner defined the actual desired product:

- physical GBA;
- ESP32 CYD;
- no desktop bridge;
- Wi-Fi connectivity;
- RetroArch/gpSP;
- Pokémon Generation III trade and battle interoperability.

## Hardware Selection

The owner selected and physically used the ESP32-32E CYD.

## Wiring

The owner established and validated the physical GBA connector wiring.

## Test Execution

Every meaningful hardware conclusion required the owner to:

- compile the generated sketches;
- flash the ESP32;
- connect the hardware;
- operate the physical Pokémon game;
- operate the emulator;
- trigger specific in-game link behavior;
- collect logs;
- report whether the GBA froze, advanced, disconnected, traded, or otherwise behaved correctly.

## Experimental Direction

The owner frequently constrained ChatGPT's proposed direction.

Examples included requiring:

- Arduino IDE compatibility;
- use of the proven GPIO mapping;
- use of previous sketches only as reference where requested;
- elimination of the computer-hosted bridge;
- same-network emulator support;
- full copy/paste sketches rather than fragments.

## Validation

The owner determined whether a proposed implementation actually worked.

A ChatGPT response saying that a protocol should work was not treated as proof.

Real GBA behavior was the acceptance test.

---

# What ChatGPT Contributed

ChatGPT was responsible for most of the implementation-level work.

This included:

## Source Analysis

ChatGPT studied and interpreted:

- GB-Link Firmware;
- GBLink Netplay Bridge;
- gpSP/RetroArch behavior where necessary;
- the user's prior sketches;
- serial logs;
- state-machine output.

## Architecture

ChatGPT proposed the combined architecture that placed:

- physical GBA communication;
- network server;
- RetroArch protocol;
- MPK1 translation

on the same ESP32.

## Code Generation

ChatGPT generated the Arduino sketches used throughout development.

This included both:

- temporary experimental probes;
- the eventual integrated implementation.

## Debugging Hypotheses

ChatGPT proposed explanations for:

- wireless-vs-cable behavior;
- handshake failures;
- state ordering;
- CRC mismatches;
- incomplete packet captures;
- queueing problems;
- room-exit handling;
- re-handshake behavior.

Some hypotheses were correct.

Some were disproven by the user's testing and replaced.

## Code Repair

ChatGPT corrected:

- compile failures;
- declaration ordering problems;
- state-machine errors;
- queue behavior;
- timing-related implementation details;
- connection sequencing.

## Documentation

ChatGPT later produced:

- extensive in-source comments;
- `README.md`;
- GPL/provenance guidance;
- `NOTICE.md`;
- this `WORKFLOW.md`.

---

# What the Upstream Projects Contributed

Neither the owner nor ChatGPT should be represented as having invented the complete system independently.

The implementation is substantially enabled by previous open-source engineering.

## GB-Link / Celio-Link Lineage

Provided the physical-side knowledge and implementations necessary to understand and reproduce GBA link behavior.

## GBLink Netplay Bridge

Provided the major conceptual and implementation basis for connecting GB-Link-style physical link states to RetroArch/gpSP MPK1 networking.

## RetroArch / gpSP

Provided the emulator network and link-protocol endpoint that this ESP32 implementation interoperates with.

The final project is therefore best described as:

> An AI-assisted ESP32 port/integration of existing GPL-licensed GB-Link ecosystem concepts and implementation behavior, guided and validated by the repository owner on real hardware.

---

# Division of Responsibility

A concise way to describe authorship is:

| Area | Primary role |
|---|---|
| Product idea / desired outcome | Repository owner |
| Hardware possession and assembly | Repository owner |
| GBA wiring | Repository owner |
| Physical testing | Repository owner |
| Emulator testing | Repository owner |
| Logs and observations | Repository owner |
| Acceptance/rejection of iterations | Repository owner |
| GB-Link/Celio foundational work | Upstream contributors |
| Netplay bridge foundational work | GB-Link Netplay Bridge contributors |
| Protocol/source interpretation | Primarily ChatGPT |
| ESP32 architecture proposal | Primarily ChatGPT |
| Arduino code generation | Primarily ChatGPT |
| Iterative code modifications | Primarily ChatGPT |
| Debugging hypotheses | Primarily ChatGPT |
| Final documentation | ChatGPT, directed by repository owner |

This is intentionally more precise than claiming either:

> "I engineered the entire project."

or:

> "ChatGPT autonomously built the entire project."

Neither statement accurately describes what occurred.

---

# Nature of the AI-Assisted Process

The process resembled a human-operated automated engineering loop more than conventional solo programming.

The user repeatedly supplied external reality to the model.

ChatGPT had no direct ability to:

- electrically probe the GBA;
- flash the ESP32;
- see the GBA screen unless the user described or shared it;
- know whether a generated timing routine actually worked;
- determine whether the game entered the trade room without user feedback.

The user therefore functioned as the model's physical interface to the system.

Conversely, the user was not manually designing every state transition, packet field, timing handler, or network message.

Those tasks were largely delegated to ChatGPT and adapted from existing open-source source material.

---

# Why So Many Iterations Were Necessary

This project crosses several domains that make one-shot generated code unrealistic:

- undocumented or partially documented console behavior;
- strict physical timing;
- ESP32 interrupt latency;
- emulator implementation details;
- TCP/Wi-Fi latency;
- protocol state ordering;
- existing GPL-derived implementation behavior;
- game-specific expectations.

A sketch could be logically plausible and still fail because of:

- one clock edge;
- one missed bit;
- a late GPIO transition;
- an unexpected state ordering;
- a queue underrun;
- one value interpreted in the wrong context.

The development process therefore treated ChatGPT output as an experimental candidate rather than authoritative code.

---

# Examples of Failed or Incomplete Iterations

The history intentionally includes failures because they explain how the final result emerged.

Examples include:

- initial Wireless Club traces that were not the desired cable protocol;
- physical peers that reached setup but not the trade connection;
- generated sketches that compiled but caused the GBA to stop later in the sequence;
- incomplete party-block capture;
- experimental `DDDD`/`AABB` state probes that did not complete a trade;
- compiler failures such as `LinkFrame does not name a type`;
- CRC errors differing by `0x8000`;
- assumptions about which endpoint would become ready first;
- premature teardown at the end of a room;
- stale state between link sessions.

Each of these produced information used in the next generated revision.

---

# Verification Standard Used During Development

A change was considered meaningful only when supported by one or more of:

1. successful compilation;
2. stable serial traces;
3. expected GBA protocol progression;
4. the physical Pokémon game advancing to the next screen/state;
5. successful RetroArch/gpSP connection;
6. successful trade/battle operation.

This distinction matters because AI-generated reasoning alone was not used as proof of physical correctness.

---

# Current Code Provenance

The current sketch combines several categories of work.

## Clearly Upstream-Derived or Adapted

Examples include:

- Generation III link-state behavior;
- physical handshake concepts;
- RANP framing;
- RetroArch session commands;
- NETPACKET transport;
- MPK1 packet structure;
- MPK state concepts;
- physical-to-emulator bridge behavior.

## ESP32-Specific Adaptation

Examples include:

- ESP32 GPIO implementation;
- ESP32 RMT receive path;
- cycle-counter timing;
- interrupt-driven transmit response;
- Wi-Fi SoftAP;
- TCP server running directly on the ESP32;
- fixed ESP32-side queues;
- diagnostics;
- integrated physical/network lifecycle.

## Iterative Project-Specific Fixes

Examples include:

- silent PREINIT;
- preserving early peer readiness;
- preserving active section state;
- ordered remote frame buffering;
- MPK pacing;
- room-exit delay;
- re-handshake detection;
- zero-frame suppression;
- handling different endpoint startup orders.

---

# Suggested Public Disclosure

A short version suitable for the README or repository description is:

> This project was developed through an iterative AI-assisted workflow using ChatGPT. I defined the goal, assembled and wired the hardware, supplied existing GB-Link source material and test logs, performed all physical GBA/RetroArch testing, and directed each iteration. ChatGPT performed most of the source analysis, protocol reasoning, code generation, debugging proposals, and documentation. The implementation is substantially derived from the GPL-licensed GB-Link/Celio-Link firmware lineage and GBLink Netplay Bridge.

A still shorter version is:

> AI-assisted development: hardware integration, requirements, testing, and direction by the repository owner; source analysis, implementation generation, and iterative debugging primarily performed with ChatGPT using the GPL-licensed GB-Link ecosystem as the technical foundation.

---

# Why This Disclosure Is Included

This repository includes this document for three reasons.

## Transparency

Readers should know how the code was actually produced.

## Reproducibility

The iteration history explains why apparently unusual timing, state, queue, and teardown logic exists.

## Attribution

The current implementation is the result of three layers of contribution:

1. prior open-source GB-Link/Celio-Link/Netplay Bridge engineering;
2. ChatGPT-assisted analysis and code generation;
3. user-directed hardware integration and repeated real-world validation.

Removing any one of those layers would give an incomplete picture of the project.

---

# Final Summary

GBLink ESP32 was not produced as a conventional solo engineering project.

The repository owner supplied the objective, constraints, physical hardware, wiring, test execution, logs, validation, and repeated direction.

ChatGPT analyzed the upstream projects and user-supplied evidence, proposed architectures, generated Arduino implementations, interpreted failures, and repeatedly modified the code.

The upstream GB-Link/Celio-Link and GBLink Netplay Bridge projects supplied much of the underlying protocol and architectural foundation.

The working result emerged through many cycles of:

```text
direction
-> generated implementation
-> physical test
-> failure or partial success
-> logs
-> analysis
-> revised implementation
```

The final code should therefore be understood as an **AI-assisted, user-directed, hardware-validated derivative implementation**, not as a claim that the repository owner personally authored or independently engineered every subsystem.
