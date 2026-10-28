# LinkBus v1 — protocol

LinkBus is how a GBA talks to anything plugged into its link port. The module (ESP, RP2040, whatever) is always the **SIO clock master**; the GBA is always the **Normal-mode 32-bit slave**. The same three wires first boot the GBA (multiboot) and then carry packets, so every module ships its own ROM and the GBA needs no cartridge.

```
module (master)                                 GBA (slave)
  SC  ───────────── clock ─────────────────►   SC  pin 5
  SI  ───────────── data to GBA ───────────►   SI  pin 3
  SO  ◄──────────── data from GBA ──────────   SO  pin 2
  GND ────────────────────────────────────────  GND pin 6
```

## 1. Physical / timing

| | |
|---|---|
| Mode | GBA Normal mode, 32-bit, external clock (`SIOCNT = 0x1000 \| IRQ`) |
| Bit order | MSB first. SC idles **high**. GBA drives SO on the falling edge, samples SI on the rising edge. |
| Clock | ~200 kHz bit-banged (`bitHalfUs = 2`). The GBA slave is happy far faster; slow is for long, unshielded cables. |
| Gap between words | ≥ 300 µs while data moves (`gapBusyUs`), 2 ms when idle (`gapIdleUs`). This is the GBA's time to take the IRQ and re-arm. |
| Throughput | 3 bytes/word → ~10 KB/s busy, ~1.5 KB/s idle-polling. Plenty for UI traffic. |

### Why Normal-mode SPI and not UART after boot

The earlier wardriver plan was multiboot followed by UART. LinkBus deliberately stays in Normal mode for three reasons:

- **Same wires, same direction.** Multiboot already uses SC/SI/SO in Normal mode, so there's no pin re-muxing. UART would need a different pin pair on the module side.
- **The ESP8266's only full UART stays free.** In the CNC pendant it goes to GRBL; in the wardriver it stays free for a GPS module.
- **Master-clocked means flow control for free.** The GBA can never be flooded mid-frame, and "was this word received?" has a definite answer (below).

## 2. Word layer

Every transfer exchanges one 32-bit word in each direction:

```
 31      24 23     16 15      8 7       0
┌──────────┬─────────┬─────────┬─────────┐
│ 0xA0 | n │ byte 0  │ byte 1  │ byte 2  │   n = 0..3 valid bytes
└──────────┴─────────┴─────────┴─────────┘
```

- **Valid word:** the header's top six bits are `101000`. Anything else is not LinkBus. That covers `0xFFFFFFFF` (SO pulled up, GBA not armed or absent), `0x7202....` (multiboot BIOS), and noise.
- **Reliability (module → GBA).** The GBA only answers with a valid word if it was armed, meaning it actually received the word. When the module gets an invalid reply to a word that carried data, it **retransmits that word**. The simulation recovered about 1,600 such unarmed transfers with zero data loss.
- **Reliability (GBA → module).** The GBA only commits bytes into a word it has armed, and the module always reads whatever it clocks, so nothing is lost in this direction.
- **Idle words (n = 0) are frame boundaries.** Senders queue whole frames atomically, so an idle word can never fall inside a frame. On the first idle word after data, receivers reset their packet parser. This kills stuck half-frames after a glitch.
- **Link loss.** 400 consecutive invalid replies take the module back to PROBING. On the GBA side, 30 frames (0.5 s) without a completed transfer shows NO LINK.

## 3. Packet layer

```
┌──────┬─────┬──────┬───────────────────┬──────┐
│ 0x7E │ LEN │ TYPE │ PAYLOAD (LEN-1)   │ CRC8 │      LEN = 1..241
└──────┴─────┴──────┴───────────────────┴──────┘
```

- **CRC-8:** polynomial 0x07, initial value 0x00, computed over LEN, TYPE and PAYLOAD.
- **No byte stuffing.** Resync relies on the length field, the CRC and idle-word boundaries.
- **Byte order:** all integers are little-endian. Strings are NUL-terminated.

Shared implementation: `shared/linkbus_proto/linkbus_proto.{h,c}`. It's plain C99, and the GBA and the ESP compile the same file.

## 4. Boot / discovery (module side)

```
PROBING ──(word 0x6202 answered 0x7202xxxx)──► BOOTING ─► upload ROM ─► PROBING
   │                                                                      │
   └──(answered with a LinkBus word: ROM already running)──► ONLINE ◄─────┘
ONLINE ──(BIOS reply, or 400 invalid replies)──► PROBING
```

- **Getting the GBA into multiboot.** Power the GBA with **no cartridge**. With a cartridge in, hold Start+Select during the boot logo instead.
- **Already running.** If the GBA is already running the module's ROM (for example after the ESP reboots, or the cable is re-seated), the module goes straight to ONLINE without re-uploading.
- **Upload.** The module streams the ROM from its own flash (`/rom/*.gba` in LittleFS), at most 256 KiB. The pendant ROM is about 12 KB and uploads in about 0.4 s.
- **On reaching ONLINE**, the module sends `HELLO`, and repeats it every 3 s.

## 5. Messages

`0x00–0x0F` are common to every module. `0x10–0x3F` go GBA → module and `0x40–0x7F` go module → GBA; both of those ranges are module-specific.

### Common

| Type | Dir | Payload |
|---|---|---|
| `0x01 HELLO` | M→G | u8 proto(=1), u16 module_id, u16 fw_version, char name[] |
| `0x02 HELLO_REQ` | G→M | — |
| `0x03 LOG` | M→G | u8 level (0 info / 1 warn / 2 error), char text[] |
| `0x04 PING` / `0x05 PONG` | both | echoed payload |
| `0x06 LIST_REQ` | G→M | u8 kind |
| `0x07 LIST_ENTRY` | M→G | u8 kind, u8 index, u8 total, u32 arg, char name[] |
| `0x08 LIST_END` | M→G | u8 kind, u8 total |
| `0x09 LIST_RUN` | G→M | u8 kind, u8 index |

Lists are generic (files, macros, Wi-Fi networks, anything) and the module paces them to fit its TX buffer.

### Module IDs

| ID | Module |
|---|---|
| `0x0001` | CNC pendant |
| `0x0002` | Wardriver (reserved) |
| `0x7F00–0x7FFF` | experiments / bench hacks |

### CNC pendant (`0x0001`)

| Type | Dir | Payload |
|---|---|---|
| `0x10 RT` | G→M | u8 GRBL realtime byte: `!` `~` `?` `0x85` `0x84` `0x90–0xA1`. `0x18` is turned into a safe stop. |
| `0x11 LINE` | G→M | char line[]. Refused while a job runs. |
| `0x12 JOG_STEP` | G→M | u8 axis (0 X, 1 Y, 2 Z), i32 dist_µm, u16 feed |
| `0x13 JOG_CONT` | G→M | i8 dx, i8 dy, i8 dz (−1/0/1), u16 feed. **Heartbeat: resend every 50 ms while held.** |
| `0x14 JOG_STOP` | G→M | — |
| `0x15 JOB_STOP` | G→M | — : feed hold → wait for a fresh `Hold:0`/Idle report → soft reset (position kept) |
| `0x40 STATUS` | M→G | 37 bytes: state, sub, mpos[3] µm, wpos[3] µm, feed, spindle, flags, rsv, progress‰, override % feed/rapid/spindle. Receivers accept ≥ 34 (fw 1.0 sends no overrides → treat as 100). |
| `0x41 JOB_EVENT` | M→G | u8 event (started / done / aborted / error / alarm / refused), u16 arg |

List kinds: `0` = G-code files (arg = size in bytes), `1` = macros (arg bit0 = needs confirmation).

### Safety behaviour (enforced by the module, not trusted to the GBA)

- **Continuous-jog dead man.** If no `JOG_CONT` arrives for 150 ms, the module sends jog cancel `0x85`. A pulled cable, a crashed GBA or a released button all stop motion.
- **Bounded jog look-ahead.** Jog segments are 100 ms each, at most 2 ahead of the machine. The planner never holds more than about 0.2 s of jog travel.
- **Feed clamps.** Jog feeds are capped per axis (`JOG_MAX_FEED_XY`, `JOG_MAX_FEED_Z`).
- **Busy machine.** Jogs, lines and macros are refused while a job runs. Files only start from Idle or Check mode.
- **Job errors.** A GRBL `error:` during a job triggers a safe stop (hold, then reset). An `ALARM` ends the job.
- **Character counting.** The module streams with GRBL character counting, so its 128-byte RX buffer is never overrun (tested at a maximum of 127).

## 6. Writing a new module

1. **Pick an ID** in `0x7Fxx` and define its message types in `linkbus_ids.h` (or in your own header using the same ranges).
2. **GBA ROM:** add `gba/common/{gba.h,lb_gba.c,gfx.c,font8x8.c}` and `shared/linkbus_proto`. Call `lb_init()`, call `lb_irq()` from your IRQ handler, call `lb_poll()` once per frame, and send with `lb_send()`.
3. **Module firmware:** use `esp/lib/LinkBus` (`LinkMaster`). Call `begin(pins, fs, romPath, id, ver, name)` and `loop()`, register `onPacket()`, and send with `send()`.
4. **Build the ROM as multiboot** (`-specs=gba_mb.specs`), then drop it into the module's `/rom/`.
