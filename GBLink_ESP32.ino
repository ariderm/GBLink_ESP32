/*
 * GBLink ESP32
 * Game Boy Advance <-> RetroArch/gpSP Generation 3 link bridge
 *
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is a modified/derived work incorporating and adapting code,
 * protocol handling, state-machine behavior, and design from:
 *
 *   GB-Link / Celio-Link Firmware
 *   https://github.com/GB-Link/GBLink-Firmware
 *   https://github.com/Celio-Link/Celio-Firmware
 *
 *   GBLink Netplay Bridge
 *   https://github.com/GB-Link/gblink-netplay-bridge
 *
 * The GBLink Netplay Bridge identifies Ashton Herron <ashton@gblink.io>
 * as its author in upstream project metadata. GB-Link Firmware is a fork
 * of Celio-Link/Celio-Firmware and shares its core codebase; copyright in
 * those portions remains with the respective upstream authors and
 * contributors.
 *
 * ESP32-specific adaptations in this project include the CYD/GPIO port,
 * ESP32 RMT receive path, interrupt-driven transmit/timing implementation,
 * Wi-Fi SoftAP/TCP transport, queue integration, diagnostics, and additional
 * handshake/section lifecycle fixes.
 *
 * This version contains modifications made in 2026.
 * Copyright in original modifications remains with the GBLink ESP32
 * contributors who authored them.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * See LICENSE for the complete license text and NOTICE.md for detailed
 * attribution and source provenance.
 */

/*
  ======================================================================
   ESP32 GBA <-> RetroArch/gpSP Bridge v1.3b

   STARTUP ORDER / EARLY HANDSHAKE FIX
   +
   GB-LINK STYLE SILENT INTER-SECTION PREINIT
   +
   CLEAN ROOM EXIT
   +
   ORDERED MPK QUEUES
   +
   NATIVE INTERRUPT-DRIVEN RMT RX / CRC

   ESP32-32E / CYD
  ======================================================================

   v1.3b FIXES
   ----------------------------------------------------------------------

   FIX #1 - PLAY MUST NOT KILL AN ALREADY-LIVE PHYSICAL SECTION

   v1.3a could do:

       physical GBA B9A0
           ↓
       sectionLive = true
       local MPK = HANDSHAKE
       emitter = enabled

       ...then RetroArch reaches PLAY...

       CMD_PLAY
           ↓
       emitter = false        <-- WRONG

   Result:

       sectionLive = YES
       local MPK = 1
       emitter = SILENT

   v1.3b:

       if PLAY starts and sectionLive == true:
           keep/enable MPK emitter
           force current local state advertisement

       if PLAY starts and sectionLive == false:
           remain silent until physical B9A0

   ----------------------------------------------------------------------

   FIX #2 - REMEMBER PEER STATE-1 REGARDLESS OF ORDER

   Either side may reach handshake first.

   Remote first:

       remote MPK state 1
           ↓
       peerReadySeen = true
           ↓
       physical B9A0 later
           ↓
       local state 1 sent
       B9A0 response can arm

   Physical first:

       physical B9A0
           ↓
       sectionLive = true
       local state 1 sent
           ↓
       remote MPK state 1 later
           ↓
       peerReadySeen = true
       B9A0 response arms

   IMPORTANT:
   starting a physical section DOES NOT clear a previously observed
   peerReadySeen anymore.

   ----------------------------------------------------------------------

   PRESERVED FROM v1.3a
   ----------------------------------------------------------------------
   - Silent PREINIT between physical sections
   - No state-0 MPK packet after ordinary 5FFF close
   - MPK emitter enabled only for active physical sections
   - Native ESP-IDF RMT authoritative RX
   - Immediate GPIO TX ISR
   - CRC = authoritative RX + actual TX
   - Initial post-8FFF CRC = B9A0
   - 256-entry ordered remote FIFO
   - One remote round consumed per physical frame
   - ~15ms GBA -> gpSP data pacing
   - ~17ms status heartbeat while emitter active
   - No 66ms USB batching
   - FF02 / FF06 / FF07 filtering
   - 5FFF remains legitimate protocol payload
   - CAFE 0017 marks room exit; does not immediately reset
   - Room exit waits for RX 5FFF + TX 5FFF
   - >18 connected B9A0 physical re-handshake detection
   - Zero physical frames suppressed toward gpSP

   ----------------------------------------------------------------------
   WIRING

       GBA SO  -> GPIO19
       GBA SI  <- GPIO23
       GBA SD <-> GPIO27
       GBA SC <-> GPIO18
       GBA GND -> GND

       GBA link pin 1 disconnected.

   ----------------------------------------------------------------------
   SERIAL

       D = diagnostics
       C = clear statistics
       R = full bridge reset
       X = disconnect RetroArch
  ======================================================================
*/
/*
  ======================================================================
   PROJECT DOCUMENTATION / ARCHITECTURE OVERVIEW
  ======================================================================

   PURPOSE
   ----------------------------------------------------------------------
   This sketch is a self-contained bridge between:

       Original Game Boy Advance hardware
                    |
            GBA Link Port / SIO
                    |
              ESP32-32E (CYD)
                    |
              2.4 GHz Wi-Fi
                    |
       RetroArch netplay + gpSP emulator

   The ESP32 replaces the two-device arrangement normally used by the
   GB-Link project (USB link adapter + computer-side netplay bridge).
   It performs both jobs locally:

     1. It behaves like a link-cable peer to the physical GBA using the
        GBA serial/link signals and the timing expected by Pokemon Gen 3.

     2. It behaves like a RetroArch/gpSP netplay peer on TCP port 55435,
        including the MPK1 packet format used by gpSP's Pokemon Gen 3
        link-cable implementation.

   No desktop bridge service is required. The emulator connects directly
   to the Wi-Fi access point hosted by this ESP32.


   RELATIONSHIP TO THE UPSTREAM GB-LINK PROJECTS
   ----------------------------------------------------------------------
   This implementation follows the same overall problem domain and wire/
   network behaviors exposed by the following projects:

     GB-Link Firmware
       https://github.com/GB-Link/GBLink-Firmware

     GBLink Netplay Bridge
       https://github.com/GB-Link/gblink-netplay-bridge

   The upstream firmware normally uses an RP2040 as a USB-to-link-cable
   adapter. The upstream desktop bridge then translates adapter status and
   GBA data into RetroArch/gpSP netplay traffic. This sketch moves those
   responsibilities into one ESP32 program and uses Wi-Fi instead of USB.

   The code below should therefore be read as four cooperating layers:

     +---------------------------+
     | RetroArch / RANP TCP      |  Netplay connection and commands
     +---------------------------+
     | gpSP MPK1 bridge          |  Link state + 8-word Pokemon rounds
     +---------------------------+
     | Section/lifecycle logic   |  Handshake, room exit, reconnects
     +---------------------------+
     | GBA electrical/timing I/O |  GPIO ISR + RMT receive decoder
     +---------------------------+


   TESTED HARDWARE / SOFTWARE
   ----------------------------------------------------------------------
   Hardware used by this project:

     - ESP32-32E N4 "Cheap Yellow Display" / CYD board
     - GBA/SP link-port connector wired to the CYD's exposed SPI header

   Pin mapping used by this sketch:

       GBA signal      ESP32 GPIO      CYD silk label
       ------------------------------------------------
       SO (GBA out)    GPIO19          IO19 / MISO
       SI (GBA in)     GPIO23          IO23 / MOSI
       SC (clock)      GPIO18          IO18 / SCK
       SD (select)     GPIO27          IO27 / CS
       GND             GND             GND
       Link pin 1      Not connected

   Confirmed by the project author to work with:

     - Pokemon FireRed
     - Pokemon LeafGreen
     - Recharged Yellow (Emerald-based ROM hack)

   Upstream GBLink/gpSP software supports additional Gen 3 titles, but a
   title should not be described as verified on this ESP32 implementation
   until it has actually been tested here.


   WHY THIS CODE IS TIMING-SENSITIVE
   ----------------------------------------------------------------------
   A GBA link transfer is not ordinary UART or SPI traffic. Pokemon Gen 3
   expects very specific link-cable behavior and timing. Network delivery,
   in contrast, is bursty and nondeterministic. Directly coupling TCP
   receive timing to the GBA pins would eventually fail.

   The bridge therefore deliberately decouples the two sides:

     - Interrupt handlers and the RMT peripheral service the GBA side.
     - Ring buffers absorb timing differences between GBA and Wi-Fi.
     - The foreground loop parses TCP and emits MPK1 packets at controlled
       intervals rather than from interrupt context.

   In particular, do not casually add Serial printing, heap allocation,
   Wi-Fi calls, or other slow operations inside IRAM_ATTR functions or the
   RMT callback. Those paths are part of the real-time link implementation.


   HIGH-LEVEL PHYSICAL PROTOCOL FLOW
   ----------------------------------------------------------------------
   The physical-side state machine cycles through three phases:

       PHY_HANDSHAKE -> PHY_CRC -> PHY_COMMAND -> PHY_CRC -> ...

   Important 16-bit values used by the observed Gen 3 cable exchange:

       D15E  Handshake disabled / peer not yet permitted to continue
       B9A0  Slave-side handshake / readiness word
       8FFF  Master handshake; transitions into packet traffic
       5FFF  Ready/section close word when observed in the proper context
       CAFE 0017  Room-exit marker used by the game protocol

   Once connected, a logical game round contains eight 16-bit command/data
   words, with CRC transactions between rounds. The RMT path is treated as
   authoritative for receive data/CRC because it captures edge timing with
   less ISR latency than pure GPIO sampling.


   MPK1 / NETWORK FLOW
   ----------------------------------------------------------------------
   gpSP encapsulates Pokemon link traffic in MPK1 messages. This sketch
   tracks three logical MPK states:

       0 = PREINIT    No active physical section
       1 = HANDSHAKE  A physical partner is waiting to establish the link
       2 = CONNECTED  Link negotiation completed; game traffic is active

   Data-bearing MPK1 packets carry one eight-word GBA round. Incoming
   rounds are queued in order and consumed no faster than one round per
   physical GBA frame. Outgoing physical frames are queued separately and
   paced toward gpSP so brief Wi-Fi/TCP bursts do not disturb cable timing.


   SECTION LIFECYCLE AND THE "SILENT PREINIT" RULE
   ----------------------------------------------------------------------
   Pokemon repeatedly opens and closes link sections while entering rooms,
   selecting trade/battle actions, and returning to menus. A key behavior
   of this version is that MPK state 0 is *not* continuously broadcast
   between ordinary physical sections. Instead the emitter stays silent
   until the GBA starts the next physical handshake.

   This prevents stale PREINIT messages from racing a new handshake on the
   emulator side. The v1.3b logic also remembers a peer's state-1 message
   if it arrives before the physical GBA reaches B9A0, so connection order
   is intentionally tolerant in either direction.


   CONCURRENCY MODEL
   ----------------------------------------------------------------------
   There are three execution contexts:

     A. GPIO interrupt context
        - SC falling-edge interrupt arms a transfer.
        - SD falling-edge interrupt performs immediate timing-critical TX
          and records CPU-side observations.

     B. RMT receive callback context
        - Decodes the incoming serial waveform.
        - Produces authoritative received words and frame CRC information.
        - Queues complete local frames without blocking.

     C. Arduino foreground loop
        - Accepts/parses RetroArch TCP traffic.
        - Moves data into/out of network queues.
        - Performs lifecycle resets and paced MPK transmissions.
        - Prints diagnostics and services serial console commands.

   Variables shared with interrupt/callback code are marked volatile. Ring
   buffers are intentionally fixed-size to avoid dynamic allocation in the
   timing-sensitive data paths.


   IMPORTANT MAINTENANCE RULES
   ----------------------------------------------------------------------
   1. Preserve the exact GBA timing paths unless testing with logic-analyzer
      traces. A refactor that looks cleaner can easily break hardware.

   2. Do not replace the ordered FIFOs with "latest packet wins" storage.
      Pokemon link rounds are sequential and cannot safely be reordered.

   3. Keep reserved remote status words (FF02/FF06/FF07) filtered from the
      physical game-data stream; 5FFF is intentionally *not* globally
      filtered because it can be legitimate payload depending on context.

   4. Do not clear peerReadySeen when a physical section starts. State 1
      may have arrived from gpSP first; retaining it is a v1.3b fix.

   5. A CAFE 0017 marker indicates room exit but is not itself sufficient
      to tear everything down. The bridge waits for the matching close
      progression so it does not reset in the middle of valid traffic.

   6. Wi-Fi sleep is disabled because latency jitter is more harmful here
      than the extra power consumption.

   7. Bluetooth is stopped because it is unused and shares the ESP32 radio
      resources with Wi-Fi.


   SERIAL DIAGNOSTICS
   ----------------------------------------------------------------------
       D / d  Print diagnostics and protocol statistics
       C / c  Clear accumulated statistics
       R / r  Reset bridge state without rebooting the ESP32
       X / x  Disconnect the active RetroArch TCP client

   When debugging a regression, capture both the serial diagnostics and a
   logic-analyzer trace of SC/SD/SI/SO whenever possible. The diagnostic
   counters are designed to distinguish electrical/timing failures from
   queueing or network-state failures.

  ======================================================================
*/


#include <Arduino.h>
#include <WiFi.h>
#include "esp_bt.h"

#include "esp_arduino_version.h"
#include "esp_idf_version.h"

#include "driver/rmt_rx.h"
#include "driver/rmt_common.h"

#include "esp_err.h"
#include "esp_attr.h"

#include "soc/gpio_reg.h"
#include "soc/soc.h"

#if ESP_ARDUINO_VERSION_MAJOR < 3
#error "v1.3b requires Arduino-ESP32 core 3.x / ESP-IDF 5.x."
#endif

// ======================================================================
// Pin assignments for the four GBA link signals. On this CYD revision these
// pins are conveniently exposed on the 4-pin SPI connector. SO and SC are
// inputs from the GBA; SI is driven toward the GBA; SD is bidirectional in
// practice and is switched between drive and release states by fast GPIO code.

// GPIO
// ======================================================================

#define PIN_GBA_SO 19
#define PIN_GBA_SI 23
#define PIN_GBA_SC 18
#define PIN_GBA_SD 27

#define SO_MASK (1UL << PIN_GBA_SO)
#define SC_MASK (1UL << PIN_GBA_SC)
#define SD_MASK (1UL << PIN_GBA_SD)

// ======================================================================
// Timing constants are expressed in CPU cycles because the critical cable
// responses are shorter than normal Arduino scheduling granularity. The code
// assumes a 240 MHz CPU. EDGE_COMP_CYCLES compensates for software latency
// between the observed edge and the point at which the next TX bit is driven.

// Physical timing
// ======================================================================

#define CPU_HZ_VALUE 240000000UL
#define LINK_BAUD     115200UL

#define BIT_CYCLES \
    (CPU_HZ_VALUE / LINK_BAUD)

#define EDGE_COMP_CYCLES \
    ((BIT_CYCLES * 3UL) / 8UL)

#define INTER_SLOT_US 3UL

#define INTER_SLOT_CYCLES \
    ((CPU_HZ_VALUE / 1000000UL) * INTER_SLOT_US)

#define SC_TIMEOUT_US 500UL

#define SC_TIMEOUT_CYCLES \
    ((CPU_HZ_VALUE / 1000000UL) * SC_TIMEOUT_US)

#define SC_DIAG_ADVANCE_CYCLES 1220UL

// ======================================================================
// ESP-IDF RMT is used as a hardware-assisted waveform recorder. GPIO ISR
// sampling remains useful for immediate response timing, but RMT is the
// authoritative decoder for received words and CRC calculations.

// Native RMT
// ======================================================================

#define RMT_RESOLUTION_HZ 10000000UL

#define RMT_SIGNAL_MIN_NS 50UL
#define RMT_SIGNAL_MAX_NS 200000UL

#define RMT_MEM_SYMBOLS    192
#define RMT_BUFFER_SYMBOLS 192

DRAM_ATTR rmt_symbol_word_t rmtRxBuffer[RMT_BUFFER_SYMBOLS];

DRAM_ATTR rmt_receive_config_t rmtReceiveConfig =
{
    .signal_range_min_ns = RMT_SIGNAL_MIN_NS,
    .signal_range_max_ns = RMT_SIGNAL_MAX_NS
};

rmt_channel_handle_t rmtRxChannel = nullptr;

// ======================================================================
// Pokemon Gen 3 link-layer constants observed/used by the GB-Link protocol.
// These are protocol words, not arbitrary magic values; changing them will
// prevent the GBA and emulator from completing link negotiation.

// GBA link protocol
// ======================================================================

#define LINK_MASTER_HANDSHAKE  0x8FFF
#define LINK_SLAVE_HANDSHAKE   0xB9A0
#define LINK_HANDSHAKE_DISABLE 0xD15E

#define LINK_READY_CLOSE       0x5FFF

#define LINK_EXIT_WORD0        0xCAFE
#define LINK_EXIT_WORD1        0x0017

#define RESERVED_STATUS_1      0xFF02
#define RESERVED_STATUS_2      0xFF06
#define RESERVED_STATUS_3      0xFF07

#define REHANDSHAKE_B9A0_COUNT 18

// ======================================================================
// RetroArch netplay (RANP) constants and identity. The ESP32 opens its own
// Wi-Fi AP and listens on RetroArch's conventional netplay TCP port 55435,
// acting as the host-side peer that gpSP expects to see.

// RetroArch
// ======================================================================

static const char *WIFI_SSID = "GBLink-ESP32";
static const char *HOST_NICK = "GBLink-ESP32";
// This SSID is intentionally open unless the surrounding environment adds its
// own isolation. For public/repeated deployments, consider adding an AP password
// only after confirming it does not change the desired emulator workflow.


static const uint16_t NETPLAY_PORT = 55435;

static const uint32_t NETPLAY_MAGIC = 0x52414E50UL;

static const uint32_t CMD_DISCONNECT = 0x00000002UL;
static const uint32_t CMD_NICK       = 0x00000020UL;
static const uint32_t CMD_INFO       = 0x00000022UL;
static const uint32_t CMD_SYNC       = 0x00000023UL;
static const uint32_t CMD_PLAY       = 0x00000025UL;
static const uint32_t CMD_MODE       = 0x00000026UL;
static const uint32_t CMD_NETPACKET  = 0x00000048UL;

// ======================================================================
// gpSP's MPK1 envelope carries the Pokemon link state and optionally one
// eight-word multiplayer round. MPK1_HAS_DATA marks a state message that also
// contains payload words.

// MPK1
// ======================================================================

static const uint32_t MPK1_MAGIC =
    0x4D504B31UL;

static const uint32_t MPK1_HAS_DATA =
    0x80000000UL;

static const uint16_t MPK_PREINIT   = 0;
static const uint16_t MPK_HANDSHAKE = 1;
static const uint16_t MPK_CONNECTED = 2;

#define REMOTE_STATE_STALE_MS 4000UL

// ======================================================================
// Network pacing intentionally throttles status and data transmission. The
// GBA side runs from hardware timing; the network side must not flood gpSP with
// bursts merely because multiple frames became available at once.

// Network pacing
// ======================================================================

#define MPK_DATA_EMIT_MS 15UL
#define MPK_STATUS_MS    17UL

WiFiServer netplayServer(NETPLAY_PORT);
WiFiClient netplayClient;

// ======================================================================
// Direct GPIO-register helpers. These bypass digitalRead/digitalWrite because
// those Arduino abstractions add too much and too-variable latency inside the
// physical link ISR path.

// Fast GPIO
// ======================================================================

/** Fast ISR-safe read of the GBA SO (serial-out) input. */

static inline bool IRAM_ATTR readSO()
{
    return
        (REG_READ(GPIO_IN_REG) & SO_MASK) != 0;
}

/** Fast ISR-safe read of the GBA SC (serial clock) input. */

static inline bool IRAM_ATTR readSC()
{
    return
        (REG_READ(GPIO_IN_REG) & SC_MASK) != 0;
}

/** Fast ISR-safe read of the GBA SD/select line. */

static inline bool IRAM_ATTR readSD()
{
    return
        (REG_READ(GPIO_IN_REG) & SD_MASK) != 0;
}

/** Drive SD low using direct GPIO registers for deterministic latency. */

static inline void IRAM_ATTR sdLow()
{
    REG_WRITE(
        GPIO_OUT_W1TC_REG,
        SD_MASK
    );

    REG_WRITE(
        GPIO_ENABLE_W1TS_REG,
        SD_MASK
    );
}

/** Drive SD high using direct GPIO registers for deterministic latency. */

static inline void IRAM_ATTR sdHigh()
{
    REG_WRITE(
        GPIO_OUT_W1TS_REG,
        SD_MASK
    );

    REG_WRITE(
        GPIO_ENABLE_W1TS_REG,
        SD_MASK
    );
}

/** Release SD so the external/GBA side can control the line without contention. */

static inline void IRAM_ATTR sdRelease()
{
    REG_WRITE(
        GPIO_ENABLE_W1TC_REG,
        SD_MASK
    );
}

// ======================================================================
// The Xtensa cycle counter provides sub-microsecond timing at 240 MHz. Cycle
// arithmetic is unsigned so natural wraparound remains safe for short delays.

// CPU cycle counter
// ======================================================================

/** Return the current CPU cycle counter used for sub-microsecond scheduling. */

static inline uint32_t IRAM_ATTR cycles()
{
    uint32_t value;

    asm volatile(
        "rsr.ccount %0"
        : "=a"(value)
    );

    return value;
}

/** Busy-wait until a target cycle count; valid only for very short ISR delays. */

static inline void IRAM_ATTR waitUntil(
    uint32_t target
)
{
    while (
        (int32_t)(
            cycles() -
            target
        ) < 0
    )
    {
    }
}

// ======================================================================
// RetroArch protocol fields are serialized in network/big-endian byte order.
// Keep byte conversion explicit so the packet format is independent of the
// ESP32's native little-endian representation.

// Endian helpers
// ======================================================================

/** Decode one network-order 32-bit field from a byte buffer. */

uint32_t readBE32(
    const uint8_t *p
)
{
    return
        ((uint32_t)p[0] << 24) |
        ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) |
        ((uint32_t)p[3]);
}

/** Decode one network-order 16-bit field from a byte buffer. */

uint16_t readBE16(
    const uint8_t *p
)
{
    return
        ((uint16_t)p[0] << 8) |
        ((uint16_t)p[1]);
}

/** Encode one 32-bit value in network/big-endian byte order. */

void writeBE32(
    uint8_t *p,
    uint32_t value
)
{
    p[0] = (value >> 24) & 0xFF;
    p[1] = (value >> 16) & 0xFF;
    p[2] = (value >> 8) & 0xFF;
    p[3] = value & 0xFF;
}

/** Encode one 16-bit value in network/big-endian byte order. */

void writeBE16(
    uint8_t *p,
    uint16_t value
)
{
    p[0] = (value >> 8) & 0xFF;
    p[1] = value & 0xFF;
}

// ======================================================================
// RANP uses fixed-width text fields for nick/core/content metadata. These
// helpers safely write/read those fields without depending on null termination.

// Fixed strings
// ======================================================================

/** Copy a C string into a fixed-width protocol field with deterministic padding. */

void writeFixedString(
    uint8_t *dst,
    size_t size,
    const char *text
)
{
    memset(
        dst,
        0,
        size
    );

    size_t len =
        strlen(text);

    if (len >= size)
    {
        len =
            size - 1;
    }

    memcpy(
        dst,
        text,
        len
    );
}

/** Convert a fixed-width protocol text field into an Arduino String for logging. */

String readFixedString(
    const uint8_t *src,
    size_t size
)
{
    String result;

    for (
        size_t i = 0;
        i < size;
        i++
    )
    {
        if (src[i] == 0)
        {
            break;
        }

        if (
            src[i] >= 32 &&
            src[i] <= 126
        )
        {
            result +=
                (char)src[i];
        }
        else
        {
            result += '?';
        }
    }

    return result;
}

// ======================================================================
// PhysicalState describes which transaction the GBA link expects next. The
// ISR/RMT paths update these fields; foreground code observes them to decide
// when MPK state changes and section events should occur.

// Physical state
// ======================================================================

enum PhysicalState : uint8_t
{
    PHY_HANDSHAKE = 0,
    PHY_CRC,
    PHY_COMMAND
};

volatile uint8_t physicalState =
    PHY_HANDSHAKE;

volatile uint8_t commandIndex = 0;

volatile bool handshakeEnabled = false;

volatile bool physicalConnected = false;

volatile bool physicalJustConnected = false;

volatile bool sectionLive = false;

// ======================================================================
// Bridge state shared between physical and network halves. peerReadySeen is
// intentionally sticky across ordering races: gpSP may advertise HANDSHAKE
// before the physical GBA does. writeGate controls when remote payload is safe
// to feed into the physical stream.

// MPK / bridge state
// ======================================================================

volatile uint16_t localMpkState =
    MPK_PREINIT;

volatile uint16_t remoteMpkState =
    MPK_PREINIT;

volatile bool peerReadySeen = false;

volatile bool writeGate = false;

volatile bool mpkEmitterEnabled = false;

uint32_t remoteMpkLastSeenMs = 0;

// ======================================================================
// Room-exit tracking separates 'an exit marker was observed' from 'the close
// handshake has actually completed'. This avoids resetting on CAFE 0017 alone.

// Room exit
// ======================================================================

volatile bool exitRoomSeen = false;

volatile bool roomExitComplete = false;

volatile uint32_t exitMarkerCount = 0;

uint32_t exitCompletedCount = 0;

// ======================================================================
// Interrupt/callback code cannot perform full resets safely, so it raises
// lightweight pending flags here. The foreground loop services those events.

// Section events
// ======================================================================

volatile bool physicalRehandshakePending = false;

volatile bool networkRehandshakePending = false;

volatile bool sectionClosePending = false;

volatile bool rxReadyCloseSeen = false;

volatile bool txReadyCloseSeen = false;

volatile uint32_t physicalB9A0Run = 0;

volatile bool flushLocalNowRequested = false;

// ======================================================================
// Lifecycle counters make reconnect behavior observable without adding logging
// to the time-critical ISR path.

// Lifecycle diagnostics
// ======================================================================

uint32_t sectionResetCount = 0;

uint32_t physicalRehandshakeCount = 0;

uint32_t networkRehandshakeCount = 0;

uint32_t closeResetCount = 0;

uint32_t staleResetCount = 0;

// ======================================================================
// v1.3b-specific counters record emitter and ordering decisions. These are
// particularly useful when one peer reaches the Cable Club before the other.

// v1.3b emitter / ordering diagnostics
// ======================================================================

uint32_t emitterEnableCount = 0;

uint32_t emitterDisableCount = 0;

uint32_t silentPreinitCount = 0;

uint32_t playWithLiveSectionCount = 0;

uint32_t playWhileIdleCount = 0;

uint32_t handshakeStateSends = 0;

uint32_t connectedStateSends = 0;

uint32_t activeStatusSends = 0;

uint32_t remotePreinitSeenCount = 0;

uint32_t remotePreinitWhileHandshake = 0;

uint32_t remoteHandshakeSeenCount = 0;

uint32_t remoteHandshakeRememberedIdle = 0;

uint32_t remoteHandshakeAcceptedLive = 0;

// ======================================================================
// Minimal synchronization state shared by the SC and SD GPIO interrupts.

// GPIO ISR state
// ======================================================================

volatile bool transferArmed = false;

volatile bool transactionBusy = false;

volatile uint32_t latestScIrqCycle = 0;

// ======================================================================
// Physical-link counters are diagnostic only; they do not drive protocol logic.

// Physical statistics
// ======================================================================

volatile uint32_t physicalTransfers = 0;

volatile uint32_t physicalFrames = 0;

volatile uint32_t framingErrors = 0;

volatile uint32_t timeoutTransfers = 0;

volatile uint32_t soHandoffs = 0;

volatile uint32_t soFallbacks = 0;

volatile uint32_t rxB9A0 = 0;

volatile uint32_t rx8FFF = 0;

volatile uint32_t txD15E = 0;

volatile uint32_t txB9A0 = 0;

// ======================================================================
// Measures SC-to-SD timing to identify edge-order or ISR-latency regressions.

// ISR timing
// ======================================================================

volatile uint32_t scSdSamples = 0;

volatile uint32_t scSdLast = 0;

volatile uint32_t scSdMin =
    0xFFFFFFFFUL;

volatile uint32_t scSdMax = 0;

volatile uint64_t scSdTotal = 0;

// ======================================================================
// CRC state is generated from authoritative received data plus the actual word
// transmitted. The first CRC after the 8FFF transition is a special B9A0 case.

// CRC
// ======================================================================

volatile uint16_t nextCalculatedCrc =
    LINK_SLAVE_HANDSHAKE;

volatile bool calculatedCrcReady = false;

volatile bool initialHandshakeCrcPending = false;

volatile uint32_t crcTransactions = 0;

volatile uint32_t crcInitialB9A0 = 0;

volatile uint32_t crcCalculatedTx = 0;

volatile uint32_t crcNotReady = 0;

volatile uint32_t crcFallbackSc = 0;

volatile uint16_t lastPhysicalCrcTx = 0;

volatile uint16_t lastIncomingSdCrc = 0;

volatile uint16_t lastIncomingScCrc = 0;

// ======================================================================
// Circular history of CPU-side observations. This is retained for post-failure
// inspection and RMT-vs-CPU comparison, not for primary protocol decoding.

// CPU history
// ======================================================================

#define CPU_HISTORY_SIZE 512

volatile uint32_t cpuHistSeq[
    CPU_HISTORY_SIZE
];

volatile uint16_t cpuHistSd[
    CPU_HISTORY_SIZE
];

volatile uint16_t cpuHistSc[
    CPU_HISTORY_SIZE
];

volatile uint16_t cpuHistTx[
    CPU_HISTORY_SIZE
];

volatile uint8_t cpuHistState[
    CPU_HISTORY_SIZE
];

volatile uint8_t cpuHistCmdIndex[
    CPU_HISTORY_SIZE
];

volatile uint32_t totalCpuSequence = 0;

// ======================================================================
// Ordered FIFO of complete eight-word rounds received from RetroArch/gpSP. One
// queued round is latched per physical frame so network bursts cannot reorder or
// overwrite game traffic.

// Remote queue
// ======================================================================

#define REMOTE_QUEUE_SIZE 256
#define REMOTE_WARN_DEPTH 64
// Ring buffers leave one slot unused so head == tail can unambiguously mean empty.


volatile uint16_t remoteQueue[
    REMOTE_QUEUE_SIZE
][8];

volatile uint8_t remoteHead = 0;

volatile uint8_t remoteTail = 0;

volatile uint32_t remoteRoundsReceived = 0;

volatile uint32_t remoteRoundsQueued = 0;

volatile uint32_t remoteRoundsConsumed = 0;

volatile uint32_t remoteRoundsDropped = 0;

volatile uint32_t remoteReservedFiltered = 0;

volatile uint32_t remoteWarnEvents = 0;

volatile bool remoteAboveWarning = false;

volatile uint16_t remoteMaxDepth = 0;

// ======================================================================
// Small staging FIFO used before the physical write gate opens. Early network
// packets are preserved rather than discarded while the two sides finish their
// handshake ordering.

// Pending pre-gate queue
// ======================================================================

#define PENDING_QUEUE_SIZE 64

uint16_t pendingQueue[
    PENDING_QUEUE_SIZE
][8];

uint8_t pendingHead = 0;

uint8_t pendingTail = 0;

uint32_t pendingQueued = 0;

uint32_t pendingDropped = 0;

uint32_t pendingClearedAtGateOpen = 0;

// ======================================================================
// Snapshot of the single remote round currently being consumed by the GBA ISR.
// Once latched, it remains stable for the entire eight-word physical frame.

// Active remote frame
// ======================================================================

volatile uint16_t remoteActiveWords[8] =
{
    0,0,0,0,0,0,0,0
};

volatile bool remoteActiveValid = false;

volatile uint32_t physicalFramesRemoteData = 0;

volatile uint32_t physicalFramesZeroFill = 0;

// ======================================================================
// Queue-depth helpers are split into ISR-safe and foreground variants so the
// timing path can inspect FIFO state without calling non-IRAM Arduino code.

// Queue helpers
// ======================================================================

/** Return remote FIFO depth without leaving ISR-safe operations. */

static inline uint16_t IRAM_ATTR remoteDepthISR()
{
    return
        (uint8_t)(
            remoteHead -
            remoteTail
        );
}

/** Foreground wrapper for current remote FIFO depth. */

uint16_t remoteDepth()
{
    return
        remoteDepthISR();
}

/** Return the number of rounds waiting in the pre-gate staging FIFO. */

uint8_t pendingDepth()
{
    if (
        pendingHead >=
        pendingTail
    )
    {
        return
            pendingHead -
            pendingTail;
    }

    return
        PENDING_QUEUE_SIZE -
        pendingTail +
        pendingHead;
}

// ======================================================================
// Enqueue one complete gpSP round. Overflow is counted and rejected rather than
// silently overwriting unread traffic, because ordering is semantically required.

// Queue remote
// ======================================================================

/** Queue an ordered eight-word round received from the emulator.
 *  Returns false if the FIFO is full; unread rounds are never overwritten. */

bool queueRemoteRound(
    const uint16_t *words
)
{
    uint8_t head =
        remoteHead;

    uint8_t next =
        head + 1;

    if (next == remoteTail)
    {
        remoteRoundsDropped++;

        return false;
    }

    for (
        uint8_t i = 0;
        i < 8;
        i++
    )
    {
        remoteQueue[head][i] =
            words[i];
    }

    __sync_synchronize();

    remoteHead =
        next;

    remoteRoundsQueued++;

    uint16_t depth =
        remoteDepth();

    if (
        depth >
        remoteMaxDepth
    )
    {
        remoteMaxDepth =
            depth;
    }

    if (
        depth >=
        REMOTE_WARN_DEPTH
    )
    {
        if (!remoteAboveWarning)
        {
            remoteAboveWarning = true;

            remoteWarnEvents++;
        }
    }
    else
    {
        remoteAboveWarning = false;
    }

    return true;
}

// ======================================================================
// Store an early remote round until writeGate opens. This queue is intentionally
// smaller because it should only absorb short handshake-ordering windows.

// Pending queue
// ======================================================================

/** Queue an emulator round that arrived before the physical write gate opened. */

bool queuePendingRound(
    const uint16_t *words
)
{
    uint8_t head =
        pendingHead;

    uint8_t next =
        head + 1;

    if (
        next >=
        PENDING_QUEUE_SIZE
    )
    {
        next = 0;
    }

    if (
        next ==
        pendingTail
    )
    {
        pendingDropped++;

        return false;
    }

    for (
        uint8_t i = 0;
        i < 8;
        i++
    )
    {
        pendingQueue[head][i] =
            words[i];
    }

    pendingHead =
        next;

    pendingQueued++;

    return true;
}

// ======================================================================
// Queue reset helpers are called at section boundaries and hard resets to ensure
// no payload from a previous room/session leaks into the next one.

// Clear queues
// ======================================================================

/** Discard all staged pre-gate rounds at a section/session boundary. */

void clearPendingRemote()
{
    uint8_t count =
        pendingDepth();

    pendingTail =
        pendingHead;

    pendingClearedAtGateOpen +=
        count;
}

/** Discard all active remote rounds and clear associated high-water state. */

void clearRemoteQueue()
{
    noInterrupts();

    remoteTail =
        remoteHead;

    remoteActiveValid =
        false;

    for (
        uint8_t i = 0;
        i < 8;
        i++
    )
    {
        remoteActiveWords[i] =
            0;
    }

    interrupts();
}

// ======================================================================
// Called from the physical timing path at a frame boundary. It selects exactly
// one remote round and exposes it as remoteActive[] for the next eight data words.

// Latch one remote frame per physical round
// ======================================================================

/** Latch at most one queued remote round for the next physical eight-word frame. */

static void IRAM_ATTR latchRemoteFrameISR()
{
    for (
        uint8_t i = 0;
        i < 8;
        i++
    )
    {
        remoteActiveWords[i] =
            0;
    }

    remoteActiveValid =
        false;

    if (!writeGate)
    {
        physicalFramesZeroFill++;

        return;
    }

    uint8_t tail =
        remoteTail;

    uint8_t head =
        remoteHead;

    if (tail == head)
    {
        physicalFramesZeroFill++;

        return;
    }

    for (
        uint8_t i = 0;
        i < 8;
        i++
    )
    {
        remoteActiveWords[i] =
            remoteQueue[tail][i];
    }

    __sync_synchronize();

    tail++;

    remoteTail =
        tail;

    remoteActiveValid =
        true;

    remoteRoundsConsumed++;

    physicalFramesRemoteData++;
}

// ======================================================================
// RMT counters describe decoder health: callback activity, symbol counts, decode
// successes/failures, and receive re-arming behavior.

// RMT counters
// ======================================================================

volatile uint32_t rmtCallbacks = 0;

volatile uint32_t rmtParentValid = 0;

volatile uint32_t rmtParentInvalid = 0;

volatile uint32_t rmtRearmOk = 0;

volatile uint32_t rmtRearmFail = 0;

volatile int32_t rmtLastRearmError = 0;

volatile uint32_t rmtNoCpuRecord = 0;

volatile uint32_t rmtAge0Mappings = 0;

volatile uint32_t rmtDuplicateSeq = 0;

volatile uint32_t rmtHandshakeWords = 0;

volatile uint32_t rmtCrcWords = 0;

volatile uint32_t rmtCommandWords = 0;

volatile uint32_t rmtCompleteRounds = 0;

volatile uint32_t rmtIncompleteRounds = 0;

volatile uint32_t rmtChecksumPublished = 0;

// ======================================================================
// Comparison statistics quantify disagreements between the RMT decoder and the
// software/CPU samplers. A rising mismatch count is a strong timing diagnostic.

// RMT vs CPU
// ======================================================================

volatile uint32_t rmtCompared = 0;

volatile uint32_t rmtEqSd = 0;

volatile uint32_t rmtDiffSd = 0;

volatile uint32_t rmtSd8000Only = 0;

volatile uint32_t rmtEqSc = 0;

volatile uint32_t rmtDiffSc = 0;

volatile uint32_t rmtSc8000Only = 0;

// ======================================================================
// Storage for the most recently decoded authoritative RMT frame and its sequence.

// RMT authoritative frame
// ======================================================================

volatile uint16_t rmtFrameWords[8] =
{
    0,0,0,0,0,0,0,0
};

volatile uint16_t rmtFrameTxWords[8] =
{
    0,0,0,0,0,0,0,0
};

volatile uint8_t rmtFrameMask = 0;

volatile uint16_t rmtFrameChecksum = 0;

volatile uint32_t rmtLastAcceptedSeq = 0;

// ======================================================================
// Local FIFO containing complete eight-word frames captured from the physical
// GBA and awaiting MPK1 transmission to gpSP.

// Local GBA -> gpSP queue
// ======================================================================

#define LOCAL_QUEUE_SIZE 128

volatile uint16_t localQueue[
    LOCAL_QUEUE_SIZE
][8];

volatile uint8_t localHead = 0;

volatile uint8_t localTail = 0;

volatile uint32_t localQueued = 0;

volatile uint32_t localDropped = 0;

// Local queue entries are complete frames, not individual words. Keeping frame
// granularity prevents a network send from observing a partially-written frame.

uint32_t localSent = 0;

uint32_t localImmediateSent = 0;

volatile uint32_t localZeroFrames = 0;

volatile uint8_t localMaxDepth = 0;

// ======================================================================
// Helpers for the local GBA->network FIFO. Zero frames are intentionally
// suppressed because idle/empty physical rounds should not become game traffic.

// Local queue helpers
// ======================================================================

/** Return GBA->network FIFO depth from timing-sensitive context. */

static inline uint8_t IRAM_ATTR localDepthISR()
{
    uint8_t h =
        localHead;

    uint8_t t =
        localTail;

    if (h >= t)
    {
        return
            h - t;
    }

    return
        LOCAL_QUEUE_SIZE -
        t +
        h;
}

/** Foreground wrapper for current local FIFO depth. */

uint8_t localDepth()
{
    return
        localDepthISR();
}

/** Test whether the authoritative RMT frame contains only 0x0000 words. */

static bool IRAM_ATTR rmtFrameAllZeroISR()
{
    for (
        uint8_t i = 0;
        i < 8;
        i++
    )
    {
        if (
            rmtFrameWords[i] !=
            0
        )
        {
            return false;
        }
    }

    return true;
}

/** Copy the completed authoritative RMT frame into the local outbound FIFO. */

static bool IRAM_ATTR queueRmtFrameFromISR()
{
    if (rmtFrameAllZeroISR())
    {
        localZeroFrames++;

        return true;
    }

    uint8_t head =
        localHead;

    uint8_t next =
        head + 1;

    if (
        next >=
        LOCAL_QUEUE_SIZE
    )
    {
        next = 0;
    }

    if (
        next ==
        localTail
    )
    {
        localDropped++;

        return false;
    }

    for (
        uint8_t i = 0;
        i < 8;
        i++
    )
    {
        localQueue[head][i] =
            rmtFrameWords[i];
    }

    __sync_synchronize();

    localHead =
        next;

    localQueued++;

    uint8_t depth =
        localDepthISR();

    if (
        depth >
        localMaxDepth
    )
    {
        localMaxDepth =
            depth;
    }

    return true;
}

// ======================================================================
// Compact recent-frame log retained for diagnostics without printing from ISR.

// Recent frame log
// ======================================================================

#define FRAME_LOG_SIZE 32

volatile uint32_t frameLogWrite = 0;

volatile uint32_t frameLogCount = 0;

volatile uint32_t frameLogSeq[
    FRAME_LOG_SIZE
];

volatile uint16_t frameLogChecksum[
    FRAME_LOG_SIZE
];

volatile uint16_t frameLogRx[
    FRAME_LOG_SIZE
][8];

volatile uint16_t frameLogTx[
    FRAME_LOG_SIZE
][8];

// ======================================================================
// TCP/MPK transmission and receive counters used to distinguish netplay parsing
// errors from physical link errors.

// Network statistics
// ======================================================================

uint32_t tcpConnections = 0;

uint32_t netpacketRx = 0;

uint32_t netpacketTx = 0;

uint32_t mpkRx = 0;

uint32_t mpkTx = 0;

uint32_t mpkDataRx = 0;

uint32_t mpkDataTx = 0;

uint32_t remoteNoPeerDiscard = 0;

uint32_t remoteGatePending = 0;

uint16_t lastRemoteStatePrinted =
    0xFFFF;

uint16_t lastLocalStateSent =
    0xFFFF;

uint32_t lastMpkDataTxMs = 0;

uint32_t lastAnyMpkTxMs = 0;

bool wasTcpConnected = false;

// ======================================================================
// Records one CPU-side transaction snapshot into a fixed circular history.

// CPU history writer
// ======================================================================

static uint32_t IRAM_ATTR recordCpuTransferISR(
    uint16_t sdWord,
    uint16_t scWord,
    uint16_t txWord,
    uint8_t state,
    uint8_t cmd
)
{
    uint32_t seq =
        ++totalCpuSequence;

    uint16_t slot =
        seq %
        CPU_HISTORY_SIZE;

    cpuHistSd[slot] =
        sdWord;

    cpuHistSc[slot] =
        scWord;

    cpuHistTx[slot] =
        txWord;

    cpuHistState[slot] =
        state;

    cpuHistCmdIndex[slot] =
        cmd;

    __sync_synchronize();

    cpuHistSeq[slot] =
        seq;

    return seq;
}

// ======================================================================
// Selects the handshake word driven toward the GBA. Before the remote peer is
// ready the bridge returns D15E; once handshakeEnabled is true it returns B9A0.

// Normal handshake TX
// ======================================================================

/** Return D15E or B9A0 according to whether the peer handshake is permitted. */

static uint16_t IRAM_ATTR getNormalTxWord()
{
    if (
        physicalState ==
        PHY_HANDSHAKE
    )
    {
        if (handshakeEnabled)
        {
            txB9A0++;

            return
                LINK_SLAVE_HANDSHAKE;
        }

        txD15E++;

        return
            LINK_HANDSHAKE_DISABLE;
    }

    return 0;
}

// ======================================================================
// Core physical state-machine transition function. It consumes one completed
// GBA-side word and advances HANDSHAKE -> CRC -> eight COMMAND words -> CRC.

// Physical protocol FSM
// ======================================================================

/** Advance the physical protocol FSM after one authoritative received word. */

static void IRAM_ATTR protocolWordComplete(
    uint16_t sdWord
)
{
    if (
        physicalState ==
        PHY_HANDSHAKE
    )
    {
        if (
            sdWord ==
            LINK_SLAVE_HANDSHAKE
        )
        {
            rxB9A0++;

            if (!sectionLive)
            {
                sectionLive = true;

                roomExitComplete = false;

                localMpkState =
                    MPK_HANDSHAKE;

                mpkEmitterEnabled =
                    true;

                /*
                   v1.3b IMPORTANT:

                   DO NOT clear peerReadySeen here.

                   If gpSP already sent state 1 before the physical GBA
                   started, that observation remains valid.
                */

                writeGate = false;

                if (peerReadySeen)
                {
                    handshakeEnabled =
                        true;
                }
                else
                {
                    handshakeEnabled =
                        false;
                }
            }
            else if (peerReadySeen)
            {
                handshakeEnabled =
                    true;
            }
        }

        if (
            sdWord ==
            LINK_MASTER_HANDSHAKE
        )
        {
            rx8FFF++;

            physicalConnected =
                true;

            physicalJustConnected =
                true;

            physicalState =
                PHY_CRC;

            commandIndex = 0;

            initialHandshakeCrcPending =
                true;

            calculatedCrcReady =
                false;

            localMpkState =
                MPK_CONNECTED;
        }

        return;
    }

    if (
        physicalState ==
        PHY_CRC
    )
    {
        commandIndex = 0;

        physicalState =
            PHY_COMMAND;

        return;
    }

    commandIndex++;

    if (
        commandIndex >= 8
    )
    {
        physicalFrames++;

        commandIndex = 0;

        physicalState =
            PHY_CRC;

        remoteActiveValid =
            false;
    }
}

// ======================================================================
// SC falling edge marks the start of a transfer opportunity. Keep this ISR
// extremely small: record timing, arm the SD-side handler, and release SD.

// SC ISR
// ======================================================================

/** GPIO ISR: arm a transaction on SC falling edge and release SD immediately. */

void IRAM_ATTR scFallingISR()
{
    latestScIrqCycle =
        cycles();

    transferArmed = true;

    sdRelease();
}

// SD falling edge is the immediate-response ISR. It handles the tightest timing
// requirements, selects the outgoing word/bit stream, and records CPU-side data.
// Heavy decoding and network work must never be added here.

// ======================================================================
// SD ISR
// ======================================================================

/** GPIO ISR: perform the timing-critical SD-side response for one transfer. */

void IRAM_ATTR sdFallingISR()
{
    uint32_t sdIrqEntry =
        cycles();

    if (
        !transferArmed ||
        transactionBusy ||
        readSC()
    )
    {
        return;
    }

    transactionBusy = true;

    transferArmed = false;

    uint8_t stateBefore =
        physicalState;

    uint8_t cmdBefore =
        commandIndex;

    uint32_t scIrq =
        latestScIrqCycle;

    uint32_t scSd =
        sdIrqEntry -
        scIrq;

    scSdLast =
        scSd;

    if (
        scSd <
        scSdMin
    )
    {
        scSdMin =
            scSd;
    }

    if (
        scSd >
        scSdMax
    )
    {
        scSdMax =
            scSd;
    }

    scSdTotal +=
        scSd;

    scSdSamples++;

    uint32_t sdParentStart =
        sdIrqEntry -
        EDGE_COMP_CYCLES;

    uint32_t scParentStart =
        scIrq -
        SC_DIAG_ADVANCE_CYCLES;

    uint32_t target =
        sdParentStart +
        BIT_CYCLES / 2;

    waitUntil(target);

    uint8_t sdStart =
        readSD()
            ? 1
            : 0;

    uint16_t sdWord = 0;

    uint16_t scWord = 0;

    for (
        uint8_t bit = 0;
        bit < 16;
        bit++
    )
    {
        uint32_t sdTarget =
            sdParentStart +
            BIT_CYCLES *
                (bit + 1) +
            BIT_CYCLES / 2;

        uint32_t scTarget =
            scParentStart +
            BIT_CYCLES *
                (bit + 1) +
            BIT_CYCLES / 2;

        uint8_t sdLevel = 0;

        uint8_t scLevel = 0;

        if (
            (int32_t)(
                sdTarget -
                scTarget
            ) >= 0
        )
        {
            waitUntil(
                scTarget
            );

            scLevel =
                readSD()
                    ? 1
                    : 0;

            waitUntil(
                sdTarget
            );

            sdLevel =
                readSD()
                    ? 1
                    : 0;
        }
        else
        {
            waitUntil(
                sdTarget
            );

            sdLevel =
                readSD()
                    ? 1
                    : 0;

            waitUntil(
                scTarget
            );

            scLevel =
                readSD()
                    ? 1
                    : 0;
        }

        if (sdLevel)
        {
            sdWord |=
                (
                    (uint16_t)1
                    << bit
                );
        }

        if (scLevel)
        {
            scWord |=
                (
                    (uint16_t)1
                    << bit
                );
        }
    }

    target =
        sdParentStart +
        BIT_CYCLES * 17 +
        BIT_CYCLES / 2;

    waitUntil(target);

    uint8_t sdStop =
        readSD()
            ? 1
            : 0;

    bool framingOkay =
        (
            sdStart == 0 &&
            sdStop == 1
        );

    // ------------------------------------------------------------------
    // TX selection
    // ------------------------------------------------------------------

    uint16_t txWord = 0;

    if (
        stateBefore ==
        PHY_CRC
    )
    {
        crcTransactions++;

        lastIncomingSdCrc =
            sdWord;

        lastIncomingScCrc =
            scWord;

        latchRemoteFrameISR();

        if (
            initialHandshakeCrcPending
        )
        {
            txWord =
                LINK_SLAVE_HANDSHAKE;

            initialHandshakeCrcPending =
                false;

            crcInitialB9A0++;
        }
        else if (
            calculatedCrcReady
        )
        {
            txWord =
                nextCalculatedCrc;

            calculatedCrcReady =
                false;

            crcCalculatedTx++;
        }
        else
        {
            txWord =
                scWord;

            crcNotReady++;

            crcFallbackSc++;
        }

        lastPhysicalCrcTx =
            txWord;
    }
    else if (
        stateBefore ==
        PHY_COMMAND
    )
    {
        if (
            cmdBefore <
            8
        )
        {
            txWord =
                remoteActiveWords[
                    cmdBefore
                ];
        }
    }
    else
    {
        txWord =
            getNormalTxWord();
    }

    uint32_t parentEnd =
        sdParentStart +
        BIT_CYCLES * 18;

    uint32_t fallbackDeadline =
        parentEnd +
        INTER_SLOT_CYCLES;

    waitUntil(
        parentEnd
    );

    bool handoff = false;

    uint32_t childStart = 0;

    if (!readSO())
    {
        handoff = true;

        childStart =
            parentEnd;
    }
    else
    {
        while (
            (int32_t)(
                cycles() -
                fallbackDeadline
            ) < 0
        )
        {
            if (!readSO())
            {
                handoff = true;

                childStart =
                    cycles();

                break;
            }
        }
    }

    if (handoff)
    {
        soHandoffs++;
    }
    else
    {
        soFallbacks++;

        childStart =
            fallbackDeadline;
    }

    waitUntil(
        childStart
    );

    sdLow();

    for (
        uint8_t bit = 0;
        bit < 16;
        bit++
    )
    {
        target =
            childStart +
            BIT_CYCLES *
                (bit + 1);

        waitUntil(
            target
        );

        if (
            txWord &
            (
                (uint16_t)1
                << bit
            )
        )
        {
            sdHigh();
        }
        else
        {
            sdLow();
        }
    }

    target =
        childStart +
        BIT_CYCLES * 17;

    waitUntil(
        target
    );

    sdHigh();

    target =
        childStart +
        BIT_CYCLES * 18;

    waitUntil(
        target
    );

    sdRelease();

    uint32_t timeoutStart =
        cycles();

    bool timedOut = false;

    while (!readSC())
    {
        if (
            (uint32_t)(
                cycles() -
                timeoutStart
            ) >
            SC_TIMEOUT_CYCLES
        )
        {
            timedOut = true;

            break;
        }
    }

    physicalTransfers++;

    recordCpuTransferISR(
        sdWord,
        scWord,
        txWord,
        stateBefore,
        cmdBefore
    );

    if (!framingOkay)
    {
        framingErrors++;
    }

    if (timedOut)
    {
        timeoutTransfers++;
    }

    if (!timedOut)
    {
        protocolWordComplete(
            sdWord
        );
    }

    transactionBusy = false;
}

// Converts expected serial bit positions into RMT tick locations. Sampling near
// bit centers makes decoding more tolerant of edge jitter than sampling edges.

// ======================================================================
// RMT timing helpers
// ======================================================================

/** Expected RMT sample position for the serial start-bit center. */

static inline uint32_t IRAM_ATTR rmtStartCenterTicksISR()
{
    return
        RMT_RESOLUTION_HZ /
        (2UL * LINK_BAUD);
}

/** Expected RMT sample position for the center of one data bit. */

static inline uint32_t IRAM_ATTR rmtDataCenterTicksISR(
    uint8_t bit
)
{
    return
        (uint32_t)(
            (
                (
                    (uint64_t)(
                        2UL * bit +
                        3UL
                    )
                ) *
                RMT_RESOLUTION_HZ
            ) /
            (
                2ULL *
                LINK_BAUD
            )
        );
}

/** Expected RMT sample position for the serial stop-bit center. */

static inline uint32_t IRAM_ATTR rmtStopCenterTicksISR()
{
    return
        (uint32_t)(
            (
                35ULL *
                RMT_RESOLUTION_HZ
            ) /
            (
                2ULL *
                LINK_BAUD
            )
        );
}

/** Calculate total duration represented by an RMT symbol buffer. */

static uint32_t IRAM_ATTR rmtDurationISR(
    const rmt_symbol_word_t *symbols,
    size_t count
)
{
    uint32_t total = 0;

    for (
        size_t i = 0;
        i < count;
        i++
    )
    {
        total +=
            symbols[i].duration0;

        total +=
            symbols[i].duration1;
    }

    return total;
}

static bool IRAM_ATTR rmtLevelAtISR(
    const rmt_symbol_word_t *symbols,
    size_t count,
    uint32_t target,
    uint8_t *levelOut
)
{
    uint32_t cursor = 0;

    for (
        size_t i = 0;
        i < count;
        i++
    )
    {
        uint16_t d0 =
            symbols[i].duration0;

        uint8_t l0 =
            symbols[i].level0;

        uint16_t d1 =
            symbols[i].duration1;

        uint8_t l1 =
            symbols[i].level1;

        if (d0)
        {
            if (
                target <
                cursor + d0
            )
            {
                *levelOut =
                    l0;

                return true;
            }

            cursor +=
                d0;
        }

        if (d1)
        {
            if (
                target <
                cursor + d1
            )
            {
                *levelOut =
                    l1;

                return true;
            }

            cursor +=
                d1;
        }
    }

    return false;
}

/** Locate the first falling edge that represents the start of the captured word. */

static bool IRAM_ATTR rmtFirstFallISR(
    const rmt_symbol_word_t *symbols,
    size_t count,
    uint32_t *startOut
)
{
    uint32_t cursor = 0;

    bool previousValid =
        false;

    uint8_t previousLevel =
        1;

    for (
        size_t i = 0;
        i < count;
        i++
    )
    {
        uint16_t d0 =
            symbols[i].duration0;

        uint8_t l0 =
            symbols[i].level0;

        uint16_t d1 =
            symbols[i].duration1;

        uint8_t l1 =
            symbols[i].level1;

        if (d0)
        {
            if (
                l0 == 0 &&
                (
                    !previousValid ||
                    previousLevel == 1
                )
            )
            {
                *startOut =
                    cursor;

                return true;
            }

            previousValid =
                true;

            previousLevel =
                l0;

            cursor +=
                d0;
        }

        if (d1)
        {
            if (
                l1 == 0 &&
                (
                    !previousValid ||
                    previousLevel == 1
                )
            )
            {
                *startOut =
                    cursor;

                return true;
            }

            previousValid =
                true;

            previousLevel =
                l1;

            cursor +=
                d1;
        }
    }

    return false;
}

/** Decode one complete 16-bit parent word from captured RMT symbols. */

static bool IRAM_ATTR rmtDecodeParentISR(
    const rmt_symbol_word_t *symbols,
    size_t count,
    uint16_t *wordOut
)
{
    if (
        !symbols ||
        count == 0
    )
    {
        return false;
    }

    uint32_t startTick = 0;

    if (
        !rmtFirstFallISR(
            symbols,
            count,
            &startTick
        )
    )
    {
        return false;
    }

    uint32_t duration =
        rmtDurationISR(
            symbols,
            count
        );

    if (
        startTick +
        rmtStopCenterTicksISR() >=
        duration
    )
    {
        return false;
    }

    uint8_t level = 1;

    if (
        !rmtLevelAtISR(
            symbols,
            count,
            startTick +
                rmtStartCenterTicksISR(),
            &level
        ) ||
        level != 0
    )
    {
        return false;
    }

    uint16_t word = 0;

    for (
        uint8_t bit = 0;
        bit < 16;
        bit++
    )
    {
        if (
            !rmtLevelAtISR(
                symbols,
                count,
                startTick +
                    rmtDataCenterTicksISR(
                        bit
                    ),
                &level
            )
        )
        {
            return false;
        }

        if (level)
        {
            word |=
                (
                    (uint16_t)1
                    << bit
                );
        }
    }

    if (
        !rmtLevelAtISR(
            symbols,
            count,
            startTick +
                rmtStopCenterTicksISR(),
            &level
        ) ||
        level != 1
    )
    {
        return false;
    }

    *wordOut =
        word;

    return true;
}

// ======================================================================
// Scores the hardware RMT decode against the parallel CPU observations. The RMT
// result remains authoritative; this comparison exists to expose drift/failures.

// RMT vs CPU
// ======================================================================

/** Compare RMT authoritative decode with SD/SC CPU samplers for diagnostics. */

static void IRAM_ATTR scoreRmtVsCpuISR(
    uint16_t rmtWord,
    uint16_t sdWord,
    uint16_t scWord
)
{
    rmtCompared++;

    uint16_t sdX =
        rmtWord ^
        sdWord;

    uint16_t scX =
        rmtWord ^
        scWord;

    if (sdX == 0)
    {
        rmtEqSd++;
    }
    else
    {
        rmtDiffSd++;

        if (
            sdX ==
            0x8000
        )
        {
            rmtSd8000Only++;
        }
    }

    if (scX == 0)
    {
        rmtEqSc++;
    }
    else
    {
        rmtDiffSc++;

        if (
            scX ==
            0x8000
        )
        {
            rmtSc8000Only++;
        }
    }
}

// ======================================================================
// Records recently completed authoritative RMT frames for human diagnostics.

// Frame log
// ======================================================================

/** Append one authoritative frame/checksum to the recent diagnostic log. */

static void IRAM_ATTR logRmtFrameISR(
    uint32_t seq,
    uint16_t checksum
)
{
    uint32_t slot =
        frameLogWrite %
        FRAME_LOG_SIZE;

    frameLogSeq[slot] =
        seq;

    frameLogChecksum[slot] =
        checksum;

    for (
        uint8_t i = 0;
        i < 8;
        i++
    )
    {
        frameLogRx[slot][i] =
            rmtFrameWords[i];

        frameLogTx[slot][i] =
            rmtFrameTxWords[i];
    }

    __sync_synchronize();

    frameLogWrite++;

    frameLogCount++;
}

// Detects protocol-significant patterns only after a complete frame exists. This
// includes room-exit/close handling and re-handshake detection that should not be
// inferred from isolated words.

// ======================================================================
// Completed frame special cases
// ======================================================================

/** Inspect a completed physical frame for close/exit/re-handshake conditions. */

static void IRAM_ATTR inspectCompletedFrameISR()
{
    uint16_t rx0 =
        rmtFrameWords[0];

    uint16_t rx1 =
        rmtFrameWords[1];

    uint16_t tx0 =
        rmtFrameTxWords[0];

    uint16_t tx1 =
        rmtFrameTxWords[1];

    bool rxExit =
        (
            rx0 ==
                LINK_EXIT_WORD0 &&
            rx1 ==
                LINK_EXIT_WORD1
        );

    bool txExit =
        (
            tx0 ==
                LINK_EXIT_WORD0 &&
            tx1 ==
                LINK_EXIT_WORD1
        );

    if (
        (rxExit || txExit) &&
        !exitRoomSeen
    )
    {
        exitRoomSeen =
            true;

        exitMarkerCount++;
    }

    if (
        rx0 ==
        LINK_READY_CLOSE
    )
    {
        rxReadyCloseSeen =
            true;
    }

    if (
        tx0 ==
        LINK_READY_CLOSE
    )
    {
        txReadyCloseSeen =
            true;
    }

    if (
        rxReadyCloseSeen &&
        txReadyCloseSeen
    )
    {
        sectionClosePending =
            true;

        flushLocalNowRequested =
            true;
    }
}

// ESP-IDF RMT receive callback. It decodes the captured waveform, advances the
// physical FSM with authoritative data, calculates CRC information, queues full
// GBA frames, and immediately re-arms RMT reception.

// ======================================================================
// Native RMT receive callback
// ======================================================================

/** RMT RX completion callback: decode, CRC, queue, state-update, then re-arm RX. */

static bool IRAM_ATTR rmtRxDoneISR(
    rmt_channel_handle_t channel,
    const rmt_rx_done_event_data_t *edata,
    void *userData
)
{
    rmtCallbacks++;

    uint16_t rmtWord = 0;

    bool valid =
        rmtDecodeParentISR(
            edata->received_symbols,
            edata->num_symbols,
            &rmtWord
        );

    if (valid)
    {
        rmtParentValid++;

        if (
            physicalConnected &&
            rmtWord ==
                LINK_SLAVE_HANDSHAKE
        )
        {
            physicalB9A0Run++;

            if (
                physicalB9A0Run >
                REHANDSHAKE_B9A0_COUNT
            )
            {
                physicalRehandshakePending =
                    true;

                physicalB9A0Run = 0;
            }
        }
        else
        {
            physicalB9A0Run = 0;
        }

        uint32_t seq =
            totalCpuSequence;

        if (seq != 0)
        {
            uint16_t historySlot =
                seq %
                CPU_HISTORY_SIZE;

            if (
                cpuHistSeq[
                    historySlot
                ] ==
                seq
            )
            {
                if (
                    seq ==
                    rmtLastAcceptedSeq
                )
                {
                    rmtDuplicateSeq++;
                }
                else
                {
                    rmtLastAcceptedSeq =
                        seq;

                    rmtAge0Mappings++;

                    uint16_t sdWord =
                        cpuHistSd[
                            historySlot
                        ];

                    uint16_t scWord =
                        cpuHistSc[
                            historySlot
                        ];

                    uint16_t txWord =
                        cpuHistTx[
                            historySlot
                        ];

                    uint8_t state =
                        cpuHistState[
                            historySlot
                        ];

                    uint8_t cmd =
                        cpuHistCmdIndex[
                            historySlot
                        ];

                    scoreRmtVsCpuISR(
                        rmtWord,
                        sdWord,
                        scWord
                    );

                    if (
                        state ==
                        PHY_HANDSHAKE
                    )
                    {
                        rmtHandshakeWords++;
                    }

                    else if (
                        state ==
                        PHY_CRC
                    )
                    {
                        rmtCrcWords++;

                        if (
                            rmtFrameMask !=
                            0
                        )
                        {
                            rmtIncompleteRounds++;

                            rmtFrameMask = 0;

                            rmtFrameChecksum = 0;
                        }
                    }

                    else if (
                        state ==
                            PHY_COMMAND &&
                        cmd < 8
                    )
                    {
                        rmtCommandWords++;

                        rmtFrameWords[cmd] =
                            rmtWord;

                        rmtFrameTxWords[cmd] =
                            txWord;

                        rmtFrameMask |=
                            (
                                (uint8_t)1
                                << cmd
                            );

                        rmtFrameChecksum =
                            (uint16_t)(
                                rmtFrameChecksum +
                                rmtWord +
                                txWord
                            );

                        if (
                            cmd ==
                            7
                        )
                        {
                            if (
                                rmtFrameMask ==
                                0xFF
                            )
                            {
                                rmtCompleteRounds++;

                                nextCalculatedCrc =
                                    rmtFrameChecksum;

                                __sync_synchronize();

                                calculatedCrcReady =
                                    true;

                                rmtChecksumPublished++;

                                queueRmtFrameFromISR();

                                inspectCompletedFrameISR();

                                logRmtFrameISR(
                                    seq,
                                    rmtFrameChecksum
                                );
                            }
                            else
                            {
                                rmtIncompleteRounds++;
                            }

                            rmtFrameMask = 0;

                            rmtFrameChecksum = 0;
                        }
                    }
                }
            }
            else
            {
                rmtNoCpuRecord++;
            }
        }
        else
        {
            rmtNoCpuRecord++;
        }
    }
    else
    {
        rmtParentInvalid++;
    }

    esp_err_t rearm =
        rmt_receive(
            channel,
            rmtRxBuffer,
            sizeof(rmtRxBuffer),
            &rmtReceiveConfig
        );

    if (
        rearm ==
        ESP_OK
    )
    {
        rmtRearmOk++;
    }
    else
    {
        rmtRearmFail++;

        rmtLastRearmError =
            (int32_t)rearm;
    }

    return false;
}

// Allocates/configures the ESP-IDF RMT RX channel for the GBA SO waveform and
// registers rmtRxDoneISR as the completion callback.

// ======================================================================
// Native RMT setup
// ======================================================================

/** Configure the native ESP-IDF RMT receiver and start the first capture. */

bool setupNativeRmt()
{
    rmt_rx_channel_config_t rxConfig = {};

    rxConfig.gpio_num =
        (gpio_num_t)PIN_GBA_SD;

    rxConfig.clk_src =
        RMT_CLK_SRC_DEFAULT;

    rxConfig.resolution_hz =
        RMT_RESOLUTION_HZ;

    rxConfig.mem_block_symbols =
        RMT_MEM_SYMBOLS;

    rxConfig.intr_priority =
        3;

    rxConfig.flags.invert_in =
        0;

    rxConfig.flags.with_dma =
        0;

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
    rxConfig.flags.io_loop_back =
        0;
#endif

    esp_err_t err =
        rmt_new_rx_channel(
            &rxConfig,
            &rmtRxChannel
        );

    if (
        err !=
        ESP_OK
    )
    {
        Serial.printf(
            "[RMT] rmt_new_rx_channel failed: %d\n",
            (int)err
        );

        return false;
    }

    rmt_rx_event_callbacks_t callbacks = {};

    callbacks.on_recv_done =
        rmtRxDoneISR;

    err =
        rmt_rx_register_event_callbacks(
            rmtRxChannel,
            &callbacks,
            nullptr
        );

    if (
        err !=
        ESP_OK
    )
    {
        Serial.printf(
            "[RMT] callback registration failed: %d\n",
            (int)err
        );

        return false;
    }

    err =
        rmt_enable(
            rmtRxChannel
        );

    if (
        err !=
        ESP_OK
    )
    {
        Serial.printf(
            "[RMT] enable failed: %d\n",
            (int)err
        );

        return false;
    }

    err =
        rmt_receive(
            rmtRxChannel,
            rmtRxBuffer,
            sizeof(rmtRxBuffer),
            &rmtReceiveConfig
        );

    if (
        err !=
        ESP_OK
    )
    {
        Serial.printf(
            "[RMT] initial receive failed: %d\n",
            (int)err
        );

        return false;
    }

    return true;
}

// Netplay state machine tracks whether the TCP peer is awaiting the RANP header,
// command exchange, or active play.

// ======================================================================
// RetroArch connection state
// ======================================================================
// The ESP32 acts as a TCP server for the direct emulator connection.


enum NetplayState : uint8_t
{
    NP_WAIT_CLIENT = 0,
    NP_HEADER,
    NP_NICK,
    NP_INFO,
    NP_AFTER_SYNC,
    NP_PLAYING
};

NetplayState netplayState =
    NP_WAIT_CLIENT;

#define RA_HEADER_SIZE 24
#define RA_RX_BUFFER_SIZE 4096

uint8_t clientHeader[
    RA_HEADER_SIZE
];

size_t clientHeaderReceived = 0;

uint8_t raRxBuffer[
    RA_RX_BUFFER_SIZE
];

size_t raRxLength = 0;

uint8_t capturedInfo[68];

String clientNick = "";

String clientCore = "";

String clientCoreVersion = "";

uint32_t contentCRC = 0;

// Low-level RANP send helpers. TCP_NODELAY is used elsewhere so protocol/status
// packets are not deliberately held by Nagle buffering.

// ======================================================================
// Network TX
// ======================================================================

/** Write a complete byte sequence to the connected RetroArch TCP peer. */

bool sendRaw(
    const uint8_t *data,
    size_t length
)
{
    if (
        !netplayClient ||
        !netplayClient.connected()
    )
    {
        return false;
    }

    return
        netplayClient.write(
            data,
            length
        ) ==
        length;
}

/** Serialize and send one RetroArch netplay command with its payload. */

bool sendCommand(
    uint32_t command,
    const uint8_t *payload,
    uint32_t length
)
{
    uint8_t header[8];

    writeBE32(
        header,
        command
    );

    writeBE32(
        header + 4,
        length
    );

    if (
        !sendRaw(
            header,
            8
        )
    )
    {
        return false;
    }

    if (
        length &&
        payload
    )
    {
        return
            sendRaw(
                payload,
                length
            );
    }

    return true;
}

/** Wrap application data as a RetroArch NETPACKET from the given sender id. */

bool sendNetpacket(
    uint32_t sender,
    const uint8_t *payload,
    uint32_t length
)
{
    uint8_t header[12];

    writeBE32(
        header,
        CMD_NETPACKET
    );

    writeBE32(
        header + 4,
        length
    );

    writeBE32(
        header + 8,
        sender
    );

    if (
        !sendRaw(
            header,
            12
        )
    )
    {
        return false;
    }

    if (
        !sendRaw(
            payload,
            length
        )
    )
    {
        return false;
    }

    netpacketTx++;

    return true;
}

/** Build/send a gpSP MPK1 state message, optionally carrying eight GBA words. */

bool sendMpk1(
    uint16_t state,
    const uint16_t *words
)
{
    if (
        netplayState !=
            NP_PLAYING ||
        !mpkEmitterEnabled
    )
    {
        return false;
    }

    uint8_t packet[24];

    memset(
        packet,
        0,
        sizeof(packet)
    );

    writeBE32(
        packet,
        MPK1_MAGIC
    );

    uint32_t flags =
        state;

    if (words)
    {
        flags |=
            MPK1_HAS_DATA;
    }

    writeBE32(
        packet + 4,
        flags
    );

    if (words)
    {
        for (
            uint8_t i = 0;
            i < 8;
            i++
        )
        {
            writeBE16(
                packet +
                    8 +
                    i * 2,
                words[i]
            );
        }
    }

    if (
        !sendNetpacket(
            0,
            packet,
            24
        )
    )
    {
        return false;
    }

    mpkTx++;

    if (words)
    {
        mpkDataTx++;

        lastMpkDataTxMs =
            millis();
    }

    lastAnyMpkTxMs =
        millis();

    return true;
}

// RANP startup helpers build the minimum identity/negotiation messages needed to
// appear to RetroArch as a compatible gpSP netplay peer.

// ======================================================================
// RANP setup
// ======================================================================

/** Send the server side of RetroArch's initial netplay header. */

bool sendServerHeader()
{
    uint8_t out[24];

    memcpy(
        out,
        clientHeader,
        24
    );

    writeBE32(
        out + 12,
        0
    );

    return
        sendRaw(
            out,
            24
        );
}

/** Advertise this bridge's configured netplay nickname. */

bool sendNick()
{
    uint8_t payload[32];

    writeFixedString(
        payload,
        32,
        HOST_NICK
    );

    return
        sendCommand(
            CMD_NICK,
            payload,
            32
        );
}

/** Request the peer's core/content information during RANP negotiation. */

bool sendInfoRequest()
{
    return
        sendCommand(
            CMD_INFO,
            nullptr,
            0
        );
}

/** Re-send the captured/constructed core information required by the handshake. */

bool sendCapturedInfo()
{
    return
        sendCommand(
            CMD_INFO,
            capturedInfo,
            68
        );
}

/** Send RetroArch synchronization metadata for this emulated netplay peer. */

bool sendSync()
{
    uint8_t payload[184];

    memset(
        payload,
        0,
        sizeof(payload)
    );

    writeBE32(
        payload + 0,
        0
    );

    writeBE32(
        payload + 4,
        1
    );

    writeBE32(
        payload + 8,
        5
    );

    writeBE32(
        payload + 12,
        5
    );

    char nick[33];

    memset(
        nick,
        0,
        sizeof(nick)
    );

    clientNick.toCharArray(
        nick,
        sizeof(nick)
    );

    writeFixedString(
        payload + 152,
        32,
        nick
    );

    return
        sendCommand(
            CMD_SYNC,
            payload,
            184
        );
}

/** Tell RetroArch the session is entering active PLAY mode. */

bool sendPlayingMode()
{
    uint8_t payload[60];

    memset(
        payload,
        0,
        sizeof(payload)
    );

    writeBE32(
        payload + 0,
        0
    );

    writeBE16(
        payload + 4,
        0xC000
    );

    writeBE16(
        payload + 6,
        1
    );

    writeBE32(
        payload + 8,
        1
    );

    char nick[33];

    memset(
        nick,
        0,
        sizeof(nick)
    );

    clientNick.toCharArray(
        nick,
        sizeof(nick)
    );

    writeFixedString(
        payload + 28,
        32,
        nick
    );

    return
        sendCommand(
            CMD_MODE,
            payload,
            60
        );
}

// Linear receive buffer for TCP stream parsing. TCP is not message-framed, so
// commands may arrive split across reads or coalesced together; parseRetroArch()
// consumes only complete records.

// ======================================================================
// RX buffer
// ======================================================================

/** Remove already-parsed bytes from the front of the TCP receive buffer. */

void consumeRx(
    size_t count
)
{
    if (
        count >=
        raRxLength
    )
    {
        raRxLength = 0;

        return;
    }

    memmove(
        raRxBuffer,
        raRxBuffer + count,
        raRxLength - count
    );

    raRxLength -=
        count;
}

// Emergency/local-tail drain used when section lifecycle requires already-captured
// GBA frames to be emitted before state is reset.

// ======================================================================
// Local tail flush
// ======================================================================

/** Immediately drain captured local GBA frames before a lifecycle transition. */

void flushLocalQueueImmediately()
{
    if (
        netplayState !=
            NP_PLAYING ||
        !netplayClient.connected() ||
        !mpkEmitterEnabled
    )
    {
        return;
    }

    while (
        localTail !=
        localHead
    )
    {
        uint8_t tail =
            localTail;

        uint16_t words[8];

        for (
            uint8_t i = 0;
            i < 8;
            i++
        )
        {
            words[i] =
                localQueue[tail][i];
        }

        if (
            !sendMpk1(
                localMpkState,
                words
            )
        )
        {
            return;
        }

        tail++;

        if (
            tail >=
            LOCAL_QUEUE_SIZE
        )
        {
            tail = 0;
        }

        localTail =
            tail;

        localSent++;

        localImmediateSent++;
    }
}

// Central section reset. This clears per-section queues/protocol state while
// optionally preserving a peer handshake that arrived early. Keeping reset logic
// centralized is critical to avoiding subtly different reconnect paths.

// ======================================================================
// Section reset
// ======================================================================

/** Reset all state that belongs to one Pokemon link section.
 *  preservePeerHandshake keeps an already-seen remote state 1 across ordering races. */

void performSectionReset(
    uint8_t reason,
    bool preservePeerHandshake
)
{
    if (transactionBusy)
    {
        return;
    }

    bool finalRoomExit =
        (
            reason == 3 &&
            exitRoomSeen
        );

    flushLocalQueueImmediately();

    noInterrupts();

    writeGate = false;

    remoteTail =
        remoteHead;

    remoteActiveValid =
        false;

    for (
        uint8_t i = 0;
        i < 8;
        i++
    )
    {
        remoteActiveWords[i] =
            0;
    }

    physicalConnected =
        false;

    physicalJustConnected =
        false;

    sectionLive =
        false;

    physicalState =
        PHY_HANDSHAKE;

    commandIndex = 0;

    handshakeEnabled =
        false;

    calculatedCrcReady =
        false;

    initialHandshakeCrcPending =
        false;

    rmtFrameMask =
        0;

    rmtFrameChecksum =
        0;

    physicalB9A0Run =
        0;

    rxReadyCloseSeen =
        false;

    txReadyCloseSeen =
        false;

    localMpkState =
        MPK_PREINIT;

    mpkEmitterEnabled =
        false;

    if (finalRoomExit)
    {
        roomExitComplete =
            true;
    }

    if (preservePeerHandshake)
    {
        peerReadySeen =
            true;

        remoteMpkState =
            MPK_HANDSHAKE;
    }
    else
    {
        peerReadySeen =
            false;
    }

    physicalRehandshakePending =
        false;

    networkRehandshakePending =
        false;

    sectionClosePending =
        false;

    flushLocalNowRequested =
        false;

    exitRoomSeen =
        false;

    lastLocalStateSent =
        MPK_PREINIT;

    interrupts();

    clearPendingRemote();

    sectionResetCount++;

    emitterDisableCount++;

    silentPreinitCount++;

    if (reason == 1)
    {
        physicalRehandshakeCount++;

        Serial.println(
            "[SECTION] reset: physical B9A0 re-handshake"
        );
    }
    else if (reason == 2)
    {
        networkRehandshakeCount++;

        Serial.println(
            "[SECTION] reset: fresh remote MPK handshake"
        );
    }
    else if (reason == 3)
    {
        closeResetCount++;

        if (finalRoomExit)
        {
            exitCompletedCount++;

            Serial.println(
                "[SECTION] final room exit completed after 5FFF/5FFF"
            );

            Serial.println(
                "[SECTION] local PREINIT is SILENT"
            );
        }
        else
        {
            Serial.println(
                "[SECTION] reset: normal 5FFF close handshake"
            );

            Serial.println(
                "[SECTION] local PREINIT is SILENT"
            );
        }
    }
    else if (reason == 5)
    {
        staleResetCount++;

        Serial.println(
            "[SECTION] reset: remote MPK state stale"
        );
    }

    sdRelease();
}

// Converts ISR-raised pending flags into safe foreground resets/actions.

// ======================================================================
// Deferred section events
// ======================================================================

/** Service reset/close/re-handshake flags raised by ISR/callback code. */

void serviceSectionEvents()
{
    if (transactionBusy)
    {
        return;
    }

    if (flushLocalNowRequested)
    {
        flushLocalNowRequested =
            false;

        flushLocalQueueImmediately();
    }

    if (sectionClosePending)
    {
        performSectionReset(
            3,
            false
        );

        return;
    }

    if (physicalRehandshakePending)
    {
        performSectionReset(
            1,
            false
        );

        return;
    }

    if (networkRehandshakePending)
    {
        performSectionReset(
            2,
            true
        );

        return;
    }
}

// Logs and applies changes to whether the MPK emitter is active. The emitter is
// deliberately silent during inter-section PREINIT.

// ======================================================================
// Emitter transition diagnostics
// ======================================================================

/** Apply/log MPK emitter enable/disable transitions in foreground context. */

void serviceEmitterTransitions()
{
    static bool previousEnabled =
        false;

    bool current =
        mpkEmitterEnabled;

    if (
        current &&
        !previousEnabled
    )
    {
        emitterEnableCount++;

        Serial.println(
            "[MPK] emitter ENABLED for physical section"
        );
    }

    if (
        !current &&
        previousEnabled
    )
    {
        Serial.println(
            "[MPK] emitter DISABLED between sections"
        );
    }

    previousEnabled =
        current;
}

// Handles the moment the physical GBA completes 8FFF negotiation and becomes
// connected. This synchronizes network-side state without doing TCP work in ISR.

// ======================================================================
// Physical 8FFF event
// ======================================================================

/** Convert a physical 8FFF-complete event into the matching network state. */

void servicePhysicalConnectEvent()
{
    if (!physicalJustConnected)
    {
        return;
    }

    noInterrupts();

    physicalJustConnected =
        false;

    remoteTail =
        remoteHead;

    remoteActiveValid =
        false;

    writeGate =
        (
            peerReadySeen &&
            mpkEmitterEnabled
        );

    interrupts();

    clearPendingRemote();

    Serial.println(
        "[SECTION] physical 8FFF -> CONNECTED"
    );

    Serial.printf(
        "[SECTION] write gate -> %s\n",
        writeGate
            ? "OPEN"
            : "CLOSED"
    );

    Serial.println(
        "[SECTION] pre-connect remote data cleared"
    );
}

// Certain gpSP words are link/status control values rather than Pokemon payload.
// They are filtered before entering the physical remote-data FIFO.

// ======================================================================
// Reserved remote command
// ======================================================================

/** Return true for gpSP control/status words that must not reach the game payload. */

bool isReservedRemoteCommand(
    uint16_t firstWord
)
{
    return
        firstWord ==
            RESERVED_STATUS_1 ||
        firstWord ==
            RESERVED_STATUS_2 ||
        firstWord ==
            RESERVED_STATUS_3;
}

// Parses one gpSP MPK1 message, updates remote state, remembers early HANDSHAKE
// state, and routes an optional eight-word payload to pending/active queues.

// ======================================================================
// MPK1 RX
// ======================================================================

/** Parse one MPK1 packet and route state/payload into the physical-side queues. */

void handleMpk1(
    const uint8_t *packet,
    uint32_t length
)
{
    if (length != 24)
    {
        return;
    }

    if (
        readBE32(packet) !=
        MPK1_MAGIC
    )
    {
        return;
    }

    mpkRx++;

    uint32_t flags =
        readBE32(
            packet + 4
        );

    uint16_t state =
        flags &
        0xFFFF;

    bool hasData =
        (
            flags &
            MPK1_HAS_DATA
        ) !=
        0;

    uint16_t previousState =
        remoteMpkState;

    remoteMpkState =
        state;

    remoteMpkLastSeenMs =
        millis();

    if (
        state !=
        lastRemoteStatePrinted
    )
    {
        Serial.printf(
            "[NET] remote MPK state -> %u\n",
            state
        );

        lastRemoteStatePrinted =
            state;
    }

    // ==================================================================
    // REMOTE PREINIT
    // ==================================================================

    if (
        state ==
        MPK_PREINIT
    )
    {
        remotePreinitSeenCount++;

        peerReadySeen =
            false;

        handshakeEnabled =
            false;

        writeGate =
            false;

        clearRemoteQueue();

        clearPendingRemote();

        if (
            sectionLive &&
            physicalState ==
                PHY_HANDSHAKE
        )
        {
            remotePreinitWhileHandshake++;
        }
    }

    // ==================================================================
    // REMOTE HANDSHAKE
    // ==================================================================

    if (
        state ==
        MPK_HANDSHAKE
    )
    {
        remoteHandshakeSeenCount++;

        /*
           v1.3b IMPORTANT:

           Remember state-1 even if the physical section hasn't started.
        */

        if (!peerReadySeen)
        {
            peerReadySeen =
                true;

            if (
                !sectionLive ||
                !mpkEmitterEnabled
            )
            {
                remoteHandshakeRememberedIdle++;
            }
            else
            {
                remoteHandshakeAcceptedLive++;
            }
        }

        /*
           If the physical section is already connected and the peer
           unexpectedly falls back to state-1, that's a genuine
           re-handshake event.
        */

        if (
            previousState !=
                MPK_HANDSHAKE &&
            (
                physicalConnected ||
                writeGate
            )
        )
        {
            networkRehandshakePending =
                true;
        }

        /*
           Only physical action waits for a physical section.
        */

        if (
            sectionLive &&
            mpkEmitterEnabled &&
            physicalState ==
                PHY_HANDSHAKE
        )
        {
            handshakeEnabled =
                true;
        }
    }

    if (!hasData)
    {
        return;
    }

    mpkDataRx++;

    remoteRoundsReceived++;

    uint16_t words[8];

    for (
        uint8_t i = 0;
        i < 8;
        i++
    )
    {
        words[i] =
            readBE16(
                packet +
                8 +
                i * 2
            );
    }

    if (
        isReservedRemoteCommand(
            words[0]
        )
    )
    {
        remoteReservedFiltered++;

        return;
    }

    if (
        !sectionLive ||
        !mpkEmitterEnabled
    )
    {
        remoteNoPeerDiscard++;

        return;
    }

    if (
        state ==
        MPK_PREINIT
    )
    {
        remoteNoPeerDiscard++;

        return;
    }

    if (
        peerReadySeen &&
        !writeGate
    )
    {
        queuePendingRound(
            words
        );

        remoteGatePending++;

        return;
    }

    if (
        writeGate &&
        physicalConnected &&
        localMpkState ==
            MPK_CONNECTED
    )
    {
        queueRemoteRound(
            words
        );

        return;
    }

    remoteNoPeerDiscard++;
}

// Extracts MPK1 data from a RetroArch NETPACKET command and hands it to the MPK
// parser after validating sender/length framing.

// ======================================================================
// NETPACKET RX
// ======================================================================

/** Parse the payload of one RetroArch NETPACKET and dispatch embedded MPK1. */

void handleNetpacket(
    const uint8_t *packet,
    uint32_t length
)
{
    netpacketRx++;

    if (length == 24)
    {
        handleMpk1(
            packet + 12,
            length
        );
    }
}

// Handles individual RetroArch netplay commands (nickname, core info, sync, play,
// mode, netpacket, disconnect). PLAY ordering is deliberately section-aware.

// ======================================================================
// RetroArch commands
// ======================================================================

/** Execute one fully framed RetroArch command. */

void handleRaCommand(
    uint32_t command,
    const uint8_t *payload,
    uint32_t length
)
{
    if (
        netplayState ==
        NP_NICK
    )
    {
        if (
            command !=
                CMD_NICK ||
            length != 32
        )
        {
            netplayClient.stop();

            return;
        }

        clientNick =
            readFixedString(
                payload,
                32
            );

        Serial.printf(
            "[RA] NICK \"%s\"\n",
            clientNick.c_str()
        );

        sendInfoRequest();

        netplayState =
            NP_INFO;

        return;
    }

    if (
        netplayState ==
        NP_INFO
    )
    {
        if (
            command !=
                CMD_INFO ||
            length != 68
        )
        {
            netplayClient.stop();

            return;
        }

        memcpy(
            capturedInfo,
            payload,
            68
        );

        contentCRC =
            readBE32(
                payload
            );

        clientCore =
            readFixedString(
                payload + 4,
                32
            );

        clientCoreVersion =
            readFixedString(
                payload + 36,
                32
            );

        Serial.printf(
            "[RA] INFO CRC=%08lX Core=\"%s\" Version=\"%s\"\n",
            contentCRC,
            clientCore.c_str(),
            clientCoreVersion.c_str()
        );

        sendCapturedInfo();

        delay(5);

        sendSync();

        netplayState =
            NP_AFTER_SYNC;

        return;
    }

    if (
        command ==
        CMD_INFO
    )
    {
        return;
    }

    if (
        command ==
        CMD_PLAY
    )
    {
        Serial.println(
            "[RA] PLAY"
        );

        if (sendPlayingMode())
        {
            netplayState =
                NP_PLAYING;

            lastMpkDataTxMs =
                millis();

            lastAnyMpkTxMs =
                millis();

            Serial.println();

            Serial.println(
                "===================================================="
            );

            Serial.println(
                " v1.3b STARTUP ORDER FIX"
            );

            Serial.println(
                " SILENT INTER-SECTION PREINIT"
            );

            Serial.println(
                " NATIVE RMT CRC ENGINE"
            );

            Serial.println(
                "===================================================="
            );

            /*
               v1.3b CRITICAL FIX:

               If the GBA already started B9A0 before PLAY completed,
               DO NOT turn the emitter back off.
            */

            if (sectionLive)
            {
                mpkEmitterEnabled =
                    true;

                /*
                   Force immediate advertisement of the current local
                   state now that NP_PLAYING exists.
                */

                lastLocalStateSent =
                    0xFFFF;

                playWithLiveSectionCount++;

                Serial.println(
                    "[MPK] PLAY entered with physical section already live"
                );

                Serial.printf(
                    "[MPK] current local state = %u\n",
                    localMpkState
                );

                if (
                    peerReadySeen &&
                    physicalState ==
                        PHY_HANDSHAKE
                )
                {
                    handshakeEnabled =
                        true;

                    Serial.println(
                        "[MPK] peer state-1 already remembered; physical handshake armed"
                    );
                }
            }
            else
            {
                mpkEmitterEnabled =
                    false;

                lastLocalStateSent =
                    MPK_PREINIT;

                playWhileIdleCount++;

                Serial.println(
                    "[MPK] waiting for physical B9A0 before enabling emitter"
                );
            }
        }

        return;
    }

    if (
        command ==
        CMD_DISCONNECT
    )
    {
        netplayClient.stop();
    }
}

// Stream parser for buffered RetroArch commands. Because TCP has no packet
// boundaries, this loop validates command lengths and waits for incomplete data.

// ======================================================================
// RetroArch parser
// ======================================================================

/** Parse as many complete RANP commands as currently exist in rxBuffer. */

void parseRetroArch()
{
    while (
        raRxLength >=
        8
    )
    {
        uint32_t command =
            readBE32(
                raRxBuffer
            );

        uint32_t length =
            readBE32(
                raRxBuffer + 4
            );

        if (
            command ==
            CMD_NETPACKET
        )
        {
            size_t total =
                12 +
                length;

            if (
                raRxLength <
                total
            )
            {
                return;
            }

            handleNetpacket(
                raRxBuffer,
                length
            );

            consumeRx(
                total
            );

            continue;
        }

        size_t total =
            8 +
            length;

        if (
            raRxLength <
            total
        )
        {
            return;
        }

        handleRaCommand(
            command,
            raRxBuffer + 8,
            length
        );

        consumeRx(
            total
        );
    }
}

// Pulls available bytes from the active TCP client and invokes the RANP parser.

// ======================================================================
// TCP receive
// ======================================================================

/** Read TCP bytes from RetroArch into rxBuffer and parse complete commands. */

void receiveRetroArch()
{
    while (
        netplayClient.connected() &&
        netplayClient.available()
    )
    {
        size_t room =
            RA_RX_BUFFER_SIZE -
            raRxLength;

        if (room == 0)
        {
            netplayClient.stop();

            return;
        }

        int count =
            netplayClient.read(
                raRxBuffer +
                    raRxLength,
                room
            );

        if (
            count <= 0
        )
        {
            break;
        }

        raRxLength +=
            count;
    }

    parseRetroArch();
}

// Reads/validates the initial RetroArch netplay header before normal commands are
// accepted. Invalid peers are disconnected rather than feeding malformed traffic.

// ======================================================================
// RANP header
// ======================================================================

/** Receive and validate the initial fixed RetroArch/RANP connection header. */

void receiveRetroArchHeader()
{
    while (
        netplayClient.available() &&
        clientHeaderReceived <
            RA_HEADER_SIZE
    )
    {
        int count =
            netplayClient.read(
                clientHeader +
                    clientHeaderReceived,
                RA_HEADER_SIZE -
                    clientHeaderReceived
            );

        if (
            count <= 0
        )
        {
            return;
        }

        clientHeaderReceived +=
            count;
    }

    if (
        clientHeaderReceived <
        RA_HEADER_SIZE
    )
    {
        return;
    }

    Serial.printf(
        "[RA] HEADER magic=%08lX protocol=%lu impl=%08lX\n",
        readBE32(clientHeader),
        readBE32(clientHeader + 16),
        readBE32(clientHeader + 20)
    );

    if (
        readBE32(clientHeader) !=
        NETPLAY_MAGIC
    )
    {
        netplayClient.stop();

        return;
    }

    sendServerHeader();

    sendNick();

    netplayState =
        NP_NICK;
}

// Accepts one RetroArch client. The bridge is intentionally single-peer because
// this implementation models one physical GBA connected to one emulator partner.

// ======================================================================
// Accept client
// ======================================================================

/** Accept and initialize a new single RetroArch client connection. */

void acceptRetroArch()
{
    if (
        !netplayServer.hasClient()
    )
    {
        return;
    }

    WiFiClient incoming =
        netplayServer.available();

    if (
        netplayClient &&
        netplayClient.connected()
    )
    {
        incoming.stop();

        return;
    }

    netplayClient.stop();

    netplayClient =
        incoming;

    netplayClient.setNoDelay(
        true
    );

    tcpConnections++;

    clientHeaderReceived = 0;

    raRxLength = 0;

    /*
       Do not destroy an already-running physical handshake here.
       The physical side may have begun before the TCP client arrived.
    */

    remoteMpkState =
        MPK_PREINIT;

    remoteMpkLastSeenMs =
        millis();

    peerReadySeen =
        false;

    writeGate =
        false;

    lastRemoteStatePrinted =
        0xFFFF;

    clearRemoteQueue();

    clearPendingRemote();

    netplayState =
        NP_HEADER;

    Serial.println();

    Serial.println(
        "===================================================="
    );

    Serial.printf(
        " RETROARCH CONNECTION #%lu\n",
        tcpConnections
    );

    Serial.printf(
        " Remote: %s:%u\n",
        netplayClient
            .remoteIP()
            .toString()
            .c_str(),
        netplayClient
            .remotePort()
    );

    Serial.println(
        "===================================================="
    );
}

// Foreground MPK scheduler. It sends state transitions immediately, permits the
// physical handshake when both sides are ready, emits queued GBA frames at the
// configured pace, and sends periodic status heartbeats while active.

// ======================================================================
// Active MPK emitter
// ======================================================================

/** Emit state/data/heartbeats toward gpSP without violating configured pacing. */

void servicePacedEmitter()
{
    if (
        netplayState !=
            NP_PLAYING ||
        !mpkEmitterEnabled
    )
    {
        return;
    }

    uint32_t now =
        millis();

    // ------------------------------------------------------------------
    // State transition
    // ------------------------------------------------------------------

    if (
        localMpkState !=
        lastLocalStateSent
    )
    {
        if (
            sendMpk1(
                localMpkState,
                nullptr
            )
        )
        {
            Serial.printf(
                "[GBA] local MPK state -> %u\n",
                localMpkState
            );

            if (
                localMpkState ==
                MPK_HANDSHAKE
            )
            {
                handshakeStateSends++;
            }

            if (
                localMpkState ==
                MPK_CONNECTED
            )
            {
                connectedStateSends++;
            }

            lastLocalStateSent =
                localMpkState;
        }

        return;
    }

    // ------------------------------------------------------------------
    // Physical handshake permission
    // ------------------------------------------------------------------

    if (
        sectionLive &&
        peerReadySeen &&
        physicalState ==
            PHY_HANDSHAKE
    )
    {
        handshakeEnabled =
            true;
    }

    if (
        !peerReadySeen &&
        physicalState ==
            PHY_HANDSHAKE
    )
    {
        handshakeEnabled =
            false;
    }

    // ------------------------------------------------------------------
    // Paced GBA data
    // ------------------------------------------------------------------

    if (
        localTail !=
        localHead
    )
    {
        if (
            now -
                lastMpkDataTxMs >=
            MPK_DATA_EMIT_MS
        )
        {
            uint8_t tail =
                localTail;

            uint16_t words[8];

            for (
                uint8_t i = 0;
                i < 8;
                i++
            )
            {
                words[i] =
                    localQueue[tail][i];
            }

            if (
                sendMpk1(
                    localMpkState,
                    words
                )
            )
            {
                tail++;

                if (
                    tail >=
                    LOCAL_QUEUE_SIZE
                )
                {
                    tail = 0;
                }

                localTail =
                    tail;

                localSent++;
            }
        }

        return;
    }

    // ------------------------------------------------------------------
    // Status heartbeat while section emitter is active
    // ------------------------------------------------------------------

    if (
        now -
            lastAnyMpkTxMs >=
        MPK_STATUS_MS
    )
    {
        if (
            sendMpk1(
                localMpkState,
                nullptr
            )
        )
        {
            activeStatusSends++;
        }
    }
}

// If an active peer stops advertising MPK state for too long, reset the section
// instead of leaving the physical GBA indefinitely attached to stale network state.

// ======================================================================
// Peer stale detection
// ======================================================================

/** Reset an active section if the remote MPK state has become stale. */

void serviceRemoteStateExpiry()
{
    if (
        !sectionLive ||
        !mpkEmitterEnabled
    )
    {
        return;
    }

    if (
        remoteMpkState ==
        MPK_PREINIT
    )
    {
        return;
    }

    uint32_t now =
        millis();

    if (
        now -
            remoteMpkLastSeenMs >
        REMOTE_STATE_STALE_MS
    )
    {
        performSectionReset(
            5,
            false
        );

        remoteMpkLastSeenMs =
            now;
    }
}

// Human-readable conversion used only by diagnostics/startup printing.

// ======================================================================
// Utility
// ======================================================================

/** Convert CPU cycles to microseconds for human-readable diagnostics. */

double cyclesToUs(
    uint64_t value
)
{
    return
        (double)value /
        240.0;
}

// Comprehensive snapshot of bridge health. This function intentionally runs only
// from the foreground serial command path, never from an ISR.

// ======================================================================
// Diagnostics
// ======================================================================

/** Print a detailed bridge/protocol/timing/queue diagnostic snapshot. */

void printDiagnostics()
{
    double scSdAverage =
        scSdSamples
            ? (
                cyclesToUs(
                    scSdTotal
                ) /
                scSdSamples
            )
            : 0.0;

    double sdPct =
        rmtCompared
            ? (
                (double)rmtEqSd *
                100.0 /
                rmtCompared
            )
            : 0.0;

    double scPct =
        rmtCompared
            ? (
                (double)rmtEqSc *
                100.0 /
                rmtCompared
            )
            : 0.0;

    Serial.println();

    Serial.println(
        "===================================================="
    );

    Serial.println(
        " v1.3b STARTUP ORDER FIX"
    );

    Serial.println(
        "===================================================="
    );

    Serial.printf(
        "Physical connected:   %s\n",
        physicalConnected
            ? "YES"
            : "NO"
    );

    Serial.printf(
        "Section live:         %s\n",
        sectionLive
            ? "YES"
            : "NO"
    );

    Serial.printf(
        "MPK emitter:          %s\n",
        mpkEmitterEnabled
            ? "ENABLED"
            : "SILENT"
    );

    Serial.printf(
        "Peer ready seen:      %s\n",
        peerReadySeen
            ? "YES"
            : "NO"
    );

    Serial.printf(
        "Write gate:           %s\n",
        writeGate
            ? "OPEN"
            : "CLOSED"
    );

    Serial.printf(
        "Handshake enabled:    %s\n",
        handshakeEnabled
            ? "YES"
            : "NO"
    );

    Serial.printf(
        "Physical state:       %u\n",
        physicalState
    );

    Serial.printf(
        "Local MPK state:      %u\n",
        localMpkState
    );

    Serial.printf(
        "Remote MPK state:     %u\n",
        remoteMpkState
    );

    Serial.println();

    // ------------------------------------------------------------------
    // Startup ordering
    // ------------------------------------------------------------------

    Serial.println(
        "--- STARTUP ORDER ---"
    );

    Serial.printf(
        "PLAY w/live section:  %lu\n",
        playWithLiveSectionCount
    );

    Serial.printf(
        "PLAY while idle:      %lu\n",
        playWhileIdleCount
    );

    Serial.printf(
        "Remote HS seen:       %lu\n",
        remoteHandshakeSeenCount
    );

    Serial.printf(
        "Remote HS idle first: %lu\n",
        remoteHandshakeRememberedIdle
    );

    Serial.printf(
        "Remote HS live:       %lu\n",
        remoteHandshakeAcceptedLive
    );

    Serial.println();

    // ------------------------------------------------------------------
    // MPK emitter
    // ------------------------------------------------------------------

    Serial.println(
        "--- MPK SECTION EMITTER ---"
    );

    Serial.printf(
        "Emitter enables:      %lu\n",
        emitterEnableCount
    );

    Serial.printf(
        "Emitter disables:     %lu\n",
        emitterDisableCount
    );

    Serial.printf(
        "Silent PREINITs:      %lu\n",
        silentPreinitCount
    );

    Serial.printf(
        "State-1 sends:        %lu\n",
        handshakeStateSends
    );

    Serial.printf(
        "State-2 sends:        %lu\n",
        connectedStateSends
    );

    Serial.printf(
        "Active heartbeats:    %lu\n",
        activeStatusSends
    );

    Serial.println();

    // ------------------------------------------------------------------
    // Peer state
    // ------------------------------------------------------------------

    Serial.println(
        "--- PEER STATE SYNC ---"
    );

    Serial.printf(
        "Remote PREINIT seen:  %lu\n",
        remotePreinitSeenCount
    );

    Serial.printf(
        "PREINIT during HS:    %lu\n",
        remotePreinitWhileHandshake
    );

    Serial.println();

    // ------------------------------------------------------------------
    // Room exit
    // ------------------------------------------------------------------

    Serial.println(
        "--- ROOM EXIT ---"
    );

    Serial.printf(
        "CAFE0017 seen:        %s\n",
        exitRoomSeen
            ? "YES"
            : "NO"
    );

    Serial.printf(
        "Exit marker count:    %lu\n",
        exitMarkerCount
    );

    Serial.printf(
        "RX 5FFF seen:         %s\n",
        rxReadyCloseSeen
            ? "YES"
            : "NO"
    );

    Serial.printf(
        "TX 5FFF seen:         %s\n",
        txReadyCloseSeen
            ? "YES"
            : "NO"
    );

    Serial.printf(
        "Exit completed:       %lu\n",
        exitCompletedCount
    );

    Serial.println();

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    Serial.println(
        "--- SECTION LIFECYCLE ---"
    );

    Serial.printf(
        "Section resets:       %lu\n",
        sectionResetCount
    );

    Serial.printf(
        "Physical rehandshake: %lu\n",
        physicalRehandshakeCount
    );

    Serial.printf(
        "Network rehandshake:  %lu\n",
        networkRehandshakeCount
    );

    Serial.printf(
        "5FFF close resets:    %lu\n",
        closeResetCount
    );

    Serial.printf(
        "Peer stale resets:    %lu\n",
        staleResetCount
    );

    Serial.printf(
        "B9A0 run:             %lu\n",
        physicalB9A0Run
    );

    Serial.println();

    // ------------------------------------------------------------------
    // Physical
    // ------------------------------------------------------------------

    Serial.println(
        "--- PHYSICAL ---"
    );

    Serial.printf(
        "Transfers:            %lu\n",
        physicalTransfers
    );

    Serial.printf(
        "Frames:               %lu\n",
        physicalFrames
    );

    Serial.printf(
        "Framing errors:       %lu\n",
        framingErrors
    );

    Serial.printf(
        "Timeouts:             %lu\n",
        timeoutTransfers
    );

    Serial.printf(
        "SO handoffs:          %lu\n",
        soHandoffs
    );

    Serial.printf(
        "SO fallbacks:         %lu\n",
        soFallbacks
    );

    Serial.println();

    // ------------------------------------------------------------------
    // gpSP -> GBA queue
    // ------------------------------------------------------------------

    Serial.println(
        "--- gpSP -> GBA QUEUES ---"
    );

    Serial.printf(
        "DATA received:        %lu\n",
        remoteRoundsReceived
    );

    Serial.printf(
        "Main queued:          %lu\n",
        remoteRoundsQueued
    );

    Serial.printf(
        "Consumed:             %lu\n",
        remoteRoundsConsumed
    );

    Serial.printf(
        "Main dropped:         %lu\n",
        remoteRoundsDropped
    );

    Serial.printf(
        "Main depth:           %u\n",
        remoteDepth()
    );

    Serial.printf(
        "Main max depth:       %u\n",
        remoteMaxDepth
    );

    Serial.printf(
        "Depth>=64 events:     %lu\n",
        remoteWarnEvents
    );

    Serial.printf(
        "Pending queued:       %lu\n",
        pendingQueued
    );

    Serial.printf(
        "Pending depth:        %u\n",
        pendingDepth()
    );

    Serial.printf(
        "Pending dropped:      %lu\n",
        pendingDropped
    );

    Serial.printf(
        "Pending cleared:      %lu\n",
        pendingClearedAtGateOpen
    );

    Serial.printf(
        "Gate-pending DATA:    %lu\n",
        remoteGatePending
    );

    Serial.printf(
        "No-peer discard:      %lu\n",
        remoteNoPeerDiscard
    );

    Serial.printf(
        "Reserved filtered:    %lu\n",
        remoteReservedFiltered
    );

    Serial.printf(
        "Frames remote DATA:   %lu\n",
        physicalFramesRemoteData
    );

    Serial.printf(
        "Frames zero filler:   %lu\n",
        physicalFramesZeroFill
    );

    Serial.println();

    // ------------------------------------------------------------------
    // Native RMT
    // ------------------------------------------------------------------

    Serial.println(
        "--- NATIVE RMT ISR ---"
    );

    Serial.printf(
        "Callbacks:            %lu\n",
        rmtCallbacks
    );

    Serial.printf(
        "Parent valid:         %lu\n",
        rmtParentValid
    );

    Serial.printf(
        "Parent invalid:       %lu\n",
        rmtParentInvalid
    );

    Serial.printf(
        "Age0 mappings:        %lu\n",
        rmtAge0Mappings
    );

    Serial.printf(
        "No CPU record:        %lu\n",
        rmtNoCpuRecord
    );

    Serial.printf(
        "Duplicate seq:        %lu\n",
        rmtDuplicateSeq
    );

    Serial.printf(
        "Rearm OK:             %lu\n",
        rmtRearmOk
    );

    Serial.printf(
        "Rearm FAIL:           %lu\n",
        rmtRearmFail
    );

    Serial.printf(
        "Last rearm error:     %ld\n",
        (long)rmtLastRearmError
    );

    Serial.println();

    // ------------------------------------------------------------------
    // CRC
    // ------------------------------------------------------------------

    Serial.println(
        "--- CRC ---"
    );

    Serial.printf(
        "CRC transactions:     %lu\n",
        crcTransactions
    );

    Serial.printf(
        "Initial B9A0:         %lu\n",
        crcInitialB9A0
    );

    Serial.printf(
        "RMT calculated TX:    %lu\n",
        crcCalculatedTx
    );

    Serial.printf(
        "CRC NOT READY:        %lu\n",
        crcNotReady
    );

    Serial.printf(
        "SC fallback:          %lu\n",
        crcFallbackSc
    );

    Serial.printf(
        "Checksum published:   %lu\n",
        rmtChecksumPublished
    );

    Serial.printf(
        "Next calculated CRC:  %04X\n",
        nextCalculatedCrc
    );

    Serial.printf(
        "Last physical CRC TX: %04X\n",
        lastPhysicalCrcTx
    );

    Serial.printf(
        "Incoming SD CRC:      %04X\n",
        lastIncomingSdCrc
    );

    Serial.printf(
        "Incoming SC CRC:      %04X\n",
        lastIncomingScCrc
    );

    Serial.println();

    // ------------------------------------------------------------------
    // RMT assembly
    // ------------------------------------------------------------------

    Serial.println(
        "--- RMT FRAME ASSEMBLY ---"
    );

    Serial.printf(
        "Handshake words:      %lu\n",
        rmtHandshakeWords
    );

    Serial.printf(
        "CRC words:            %lu\n",
        rmtCrcWords
    );

    Serial.printf(
        "Command words:        %lu\n",
        rmtCommandWords
    );

    Serial.printf(
        "Complete rounds:      %lu\n",
        rmtCompleteRounds
    );

    Serial.printf(
        "Incomplete rounds:    %lu\n",
        rmtIncompleteRounds
    );

    Serial.printf(
        "Current mask:         %02X\n",
        rmtFrameMask
    );

    Serial.println();

    // ------------------------------------------------------------------
    // RMT vs CPU
    // ------------------------------------------------------------------

    Serial.println(
        "--- RMT vs CPU ---"
    );

    Serial.printf(
        "Compared:             %lu\n",
        rmtCompared
    );

    Serial.printf(
        "RMT == SD:            %lu  %.2f%%\n",
        rmtEqSd,
        sdPct
    );

    Serial.printf(
        "RMT != SD:            %lu\n",
        rmtDiffSd
    );

    Serial.printf(
        "SD bit15-only:        %lu\n",
        rmtSd8000Only
    );

    Serial.printf(
        "RMT == SC:            %lu  %.2f%%\n",
        rmtEqSc,
        scPct
    );

    Serial.printf(
        "RMT != SC:            %lu\n",
        rmtDiffSc
    );

    Serial.printf(
        "SC bit15-only:        %lu\n",
        rmtSc8000Only
    );

    Serial.println();

    // ------------------------------------------------------------------
    // CPU timing
    // ------------------------------------------------------------------

    Serial.println(
        "--- CPU ISR TIMING ---"
    );

    Serial.printf(
        "SC->SD samples:       %lu\n",
        scSdSamples
    );

    Serial.printf(
        "Last:                 %.3f us\n",
        cyclesToUs(
            scSdLast
        )
    );

    Serial.printf(
        "Min:                  %.3f us\n",
        scSdSamples
            ? cyclesToUs(
                scSdMin
            )
            : 0.0
    );

    Serial.printf(
        "Max:                  %.3f us\n",
        cyclesToUs(
            scSdMax
        )
    );

    Serial.printf(
        "Average:              %.3f us\n",
        scSdAverage
    );

    Serial.println();

    // ------------------------------------------------------------------
    // GBA -> gpSP
    // ------------------------------------------------------------------

    Serial.println(
        "--- GBA -> gpSP QUEUE ---"
    );

    Serial.printf(
        "Nonzero queued:       %lu\n",
        localQueued
    );

    Serial.printf(
        "Sent paced:           %lu\n",
        localSent -
            localImmediateSent
    );

    Serial.printf(
        "Sent immediate:       %lu\n",
        localImmediateSent
    );

    Serial.printf(
        "Physical zero frames: %lu\n",
        localZeroFrames
    );

    Serial.printf(
        "Dropped:              %lu\n",
        localDropped
    );

    Serial.printf(
        "Depth:                %u\n",
        localDepth()
    );

    Serial.printf(
        "Max depth:            %u\n",
        localMaxDepth
    );

    Serial.println();

    // ------------------------------------------------------------------
    // Recent frames
    // ------------------------------------------------------------------

    Serial.println(
        "--- RECENT FULL-DUPLEX FRAMES ---"
    );

    uint32_t available =
        frameLogCount;

    uint8_t show =
        available > 16
            ? 16
            : (uint8_t)available;

    uint32_t write =
        frameLogWrite;

    for (
        uint8_t n = 0;
        n < show;
        n++
    )
    {
        int32_t slot =
            (int32_t)write -
            1 -
            n;

        while (
            slot < 0
        )
        {
            slot +=
                FRAME_LOG_SIZE;
        }

        slot %=
            FRAME_LOG_SIZE;

        Serial.printf(
            "F%lu seq=%lu CRC=%04X\n",
            available - n,
            frameLogSeq[slot],
            frameLogChecksum[slot]
        );

        Serial.print(
            " RX:"
        );

        for (
            uint8_t i = 0;
            i < 8;
            i++
        )
        {
            Serial.printf(
                " %04X",
                frameLogRx[slot][i]
            );
        }

        Serial.println();

        Serial.print(
            " TX:"
        );

        for (
            uint8_t i = 0;
            i < 8;
            i++
        )
        {
            Serial.printf(
                " %04X",
                frameLogTx[slot][i]
            );
        }

        Serial.println();
    }

    Serial.println(
        "===================================================="
    );
}

// Clears diagnostic counters while preserving live protocol/session operation.

// ======================================================================
// Clear diagnostics
// ======================================================================

/** Zero diagnostic counters without intentionally tearing down the live session. */

void clearDiagnostics()
{
    noInterrupts();

    scSdSamples = 0;
    scSdLast = 0;
    scSdMin = 0xFFFFFFFFUL;
    scSdMax = 0;
    scSdTotal = 0;

    rmtCallbacks = 0;
    rmtParentValid = 0;
    rmtParentInvalid = 0;
    rmtRearmOk = 0;
    rmtRearmFail = 0;
    rmtLastRearmError = 0;

    rmtNoCpuRecord = 0;
    rmtAge0Mappings = 0;
    rmtDuplicateSeq = 0;

    rmtHandshakeWords = 0;
    rmtCrcWords = 0;
    rmtCommandWords = 0;
    rmtCompleteRounds = 0;
    rmtIncompleteRounds = 0;
    rmtChecksumPublished = 0;

    rmtCompared = 0;
    rmtEqSd = 0;
    rmtDiffSd = 0;
    rmtSd8000Only = 0;
    rmtEqSc = 0;
    rmtDiffSc = 0;
    rmtSc8000Only = 0;

    crcTransactions = 0;
    crcInitialB9A0 = 0;
    crcCalculatedTx = 0;
    crcNotReady = 0;
    crcFallbackSc = 0;

    remoteRoundsReceived = 0;
    remoteRoundsQueued = 0;
    remoteRoundsConsumed = 0;
    remoteRoundsDropped = 0;

    remoteReservedFiltered = 0;

    remoteWarnEvents = 0;
    remoteAboveWarning = false;

    remoteMaxDepth =
        remoteDepthISR();

    physicalFramesRemoteData = 0;
    physicalFramesZeroFill = 0;

    localQueued = 0;
    localDropped = 0;

    localMaxDepth =
        localDepthISR();

    localZeroFrames = 0;

    frameLogWrite = 0;
    frameLogCount = 0;

    exitMarkerCount = 0;

    interrupts();

    localSent = 0;
    localImmediateSent = 0;

    pendingQueued = 0;
    pendingDropped = 0;
    pendingClearedAtGateOpen = 0;

    remoteGatePending = 0;
    remoteNoPeerDiscard = 0;

    sectionResetCount = 0;
    physicalRehandshakeCount = 0;
    networkRehandshakeCount = 0;
    closeResetCount = 0;
    staleResetCount = 0;

    exitCompletedCount = 0;

    emitterEnableCount = 0;
    emitterDisableCount = 0;
    silentPreinitCount = 0;

    playWithLiveSectionCount = 0;
    playWhileIdleCount = 0;

    handshakeStateSends = 0;
    connectedStateSends = 0;
    activeStatusSends = 0;

    remotePreinitSeenCount = 0;
    remotePreinitWhileHandshake = 0;

    remoteHandshakeSeenCount = 0;
    remoteHandshakeRememberedIdle = 0;
    remoteHandshakeAcceptedLive = 0;

    Serial.println(
        "[TEST] diagnostics cleared"
    );
}

// Full software reset of bridge/network-protocol state. GPIO/RMT/Wi-Fi hardware
// remains configured, making this useful for iterative testing without rebooting.

// ======================================================================
// Full reset
// ======================================================================

/** Return the bridge to a clean initial protocol state while keeping services up. */

void resetBridge()
{
    noInterrupts();

    physicalState =
        PHY_HANDSHAKE;

    commandIndex = 0;

    handshakeEnabled = false;

    physicalConnected = false;

    physicalJustConnected = false;

    sectionLive = false;

    localMpkState =
        MPK_PREINIT;

    remoteMpkState =
        MPK_PREINIT;

    peerReadySeen = false;

    writeGate = false;

    mpkEmitterEnabled = false;

    transferArmed = false;

    transactionBusy = false;

    calculatedCrcReady = false;

    initialHandshakeCrcPending = false;

    nextCalculatedCrc =
        LINK_SLAVE_HANDSHAKE;

    rmtFrameMask = 0;

    rmtFrameChecksum = 0;

    rmtLastAcceptedSeq = 0;

    physicalB9A0Run = 0;

    physicalRehandshakePending = false;

    networkRehandshakePending = false;

    sectionClosePending = false;

    exitRoomSeen = false;

    roomExitComplete = false;

    rxReadyCloseSeen = false;

    txReadyCloseSeen = false;

    flushLocalNowRequested = false;

    localHead = 0;
    localTail = 0;

    remoteHead = 0;
    remoteTail = 0;

    remoteActiveValid = false;

    for (
        uint8_t i = 0;
        i < 8;
        i++
    )
    {
        remoteActiveWords[i] = 0;
    }

    interrupts();

    pendingHead = 0;
    pendingTail = 0;

    lastLocalStateSent =
        MPK_PREINIT;

    lastRemoteStatePrinted =
        0xFFFF;

    remoteMpkLastSeenMs =
        millis();

    sdRelease();

    Serial.println(
        "[BRIDGE] full reset - SD released"
    );
}

// ======================================================================
// Hardware and service initialization. Order matters: configure the physical link
// and RMT first, then Wi-Fi/netplay, then reset all runtime protocol state.

// Setup
// ======================================================================

/** Arduino entry point: initialize serial, GPIO/RMT, Wi-Fi AP, and netplay server. */

void setup()
{
    Serial.begin(
        115200
    );

    delay(
        1000
    );

    btStop();

    pinMode(
        PIN_GBA_SO,
        INPUT
    );

    pinMode(
        PIN_GBA_SC,
        INPUT
    );

    pinMode(
        PIN_GBA_SI,
        OUTPUT
    );

    digitalWrite(
        PIN_GBA_SI,
        LOW
    );

    pinMode(
        PIN_GBA_SD,
        OUTPUT
    );

    digitalWrite(
        PIN_GBA_SD,
        HIGH
    );

    sdRelease();

    attachInterrupt(
        digitalPinToInterrupt(
            PIN_GBA_SC
        ),
        scFallingISR,
        FALLING
    );

    attachInterrupt(
        digitalPinToInterrupt(
            PIN_GBA_SD
        ),
        sdFallingISR,
        FALLING
    );

    bool rmtOK =
        setupNativeRmt();

    WiFi.mode(
        WIFI_AP
    );

    WiFi.setSleep(
        false
    );

    bool wifiOK =
        WiFi.softAP(
            WIFI_SSID
        );

    netplayServer.begin();

    netplayServer.setNoDelay(
        true
    );

    resetBridge();

    Serial.println();

    Serial.println(
        "===================================================="
    );

    Serial.println(
        " ESP32 GBA <-> gpSP v1.3b"
    );

    Serial.println(
        " STARTUP ORDER / EARLY HANDSHAKE FIX"
    );

    Serial.println(
        " SILENT INTER-SECTION PREINIT"
    );

    Serial.println(
        " NATIVE RMT CRC ENGINE"
    );

    Serial.println(
        "===================================================="
    );

    Serial.printf(
        "BIT_CYCLES:       %lu\n",
        (uint32_t)BIT_CYCLES
    );

    Serial.printf(
        "EDGE_COMP:        %lu cycles / %.3f us\n",
        (uint32_t)EDGE_COMP_CYCLES,
        cyclesToUs(
            EDGE_COMP_CYCLES
        )
    );

    Serial.printf(
        "SC decoder:       -%lu cycles / -%.3f us\n",
        (uint32_t)SC_DIAG_ADVANCE_CYCLES,
        cyclesToUs(
            SC_DIAG_ADVANCE_CYCLES
        )
    );

    Serial.printf(
        "RMT interrupt:    %s\n",
        rmtOK
            ? "READY"
            : "FAILED"
    );

    Serial.printf(
        "Remote FIFO:      %u slots (%u usable)\n",
        REMOTE_QUEUE_SIZE,
        REMOTE_QUEUE_SIZE - 1
    );

    Serial.println();

    Serial.printf(
        "WiFi:             %s\n",
        wifiOK
            ? "READY"
            : "FAILED"
    );

    Serial.printf(
        "SSID:             %s\n",
        WIFI_SSID
    );

    Serial.printf(
        "IP:               %s\n",
        WiFi.softAPIP()
            .toString()
            .c_str()
    );

    Serial.printf(
        "TCP:              %u\n",
        NETPLAY_PORT
    );

    Serial.println();

    Serial.println(
        "v1.3b ORDERING RULES:"
    );

    Serial.println(
        " physical B9A0 before PLAY = preserved"
    );

    Serial.println(
        " PLAY + live section        = emitter stays ON"
    );

    Serial.println(
        " remote state 1 before GBA  = remembered"
    );

    Serial.println(
        " physical B9A0 after state1 = handshake may arm immediately"
    );

    Serial.println(
        " PREINIT between sections   = SILENT"
    );

    Serial.println(
        " remote state 0             = passive / gate closed"
    );

    Serial.println(
        " RMT / CRC / FIFO           = unchanged"
    );

    Serial.println();

    Serial.println(
        "D = diagnostics"
    );

    Serial.println(
        "C = clear stats"
    );

    Serial.println(
        "R = reset"
    );

    Serial.println(
        "X = disconnect"
    );

    Serial.println();

    Serial.println(
        "Waiting for GBA and RetroArch..."
    );
}

// ======================================================================
// Main loop
// Cooperative foreground scheduler. Timing-critical cable work is interrupt/RMT
// driven; loop() handles deferred lifecycle events, TCP parsing, MPK pacing, and
// operator diagnostics without blocking.

// ======================================================================

/** Arduino foreground scheduler for deferred protocol, TCP, emitter, and CLI work. */

void loop()
{
    serviceSectionEvents();

    serviceEmitterTransitions();

    servicePhysicalConnectEvent();

    serviceRemoteStateExpiry();

    acceptRetroArch();

    bool connected =
        (
            netplayClient &&
            netplayClient.connected()
        );

    if (connected)
    {
        if (
            netplayState ==
            NP_HEADER
        )
        {
            receiveRetroArchHeader();
        }
        else
        {
            receiveRetroArch();
        }
    }

    serviceSectionEvents();

    serviceEmitterTransitions();

    servicePhysicalConnectEvent();

    serviceRemoteStateExpiry();

    servicePacedEmitter();

    serviceSectionEvents();

    serviceEmitterTransitions();

    if (
        wasTcpConnected &&
        !connected
    )
    {
        Serial.println(
            "[RA] disconnected"
        );

        netplayState =
            NP_WAIT_CLIENT;

        remoteMpkState =
            MPK_PREINIT;

        peerReadySeen =
            false;

        handshakeEnabled =
            false;

        writeGate =
            false;

        /*
           Preserve physical section state itself. If the GBA remains in
           B9A0 search mode, a new RetroArch connection may attach to it.
        */

        if (!sectionLive)
        {
            mpkEmitterEnabled =
                false;

            localMpkState =
                MPK_PREINIT;

            lastLocalStateSent =
                MPK_PREINIT;
        }
        else
        {
            mpkEmitterEnabled =
                true;

            lastLocalStateSent =
                0xFFFF;
        }

        clearRemoteQueue();

        clearPendingRemote();

        lastRemoteStatePrinted =
            0xFFFF;
    }

    wasTcpConnected =
        connected;

    if (
        Serial.available()
    )
    {
        char c =
            Serial.read();

        if (
            c == 'D' ||
            c == 'd'
        )
        {
            printDiagnostics();
        }

        else if (
            c == 'C' ||
            c == 'c'
        )
        {
            clearDiagnostics();
        }

        else if (
            c == 'R' ||
            c == 'r'
        )
        {
            resetBridge();
        }

        else if (
            c == 'X' ||
            c == 'x'
        )
        {
            if (
                netplayClient &&
                netplayClient.connected()
            )
            {
                netplayClient.stop();
            }
        }
    }

    delay(0);
}
