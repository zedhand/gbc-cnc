# LinkBus hardware: link cable, coupler, modules

## 1. GBA link port

Looking into the GBA's socket:

```
   ┌───────────────┐
   │  1   3   5    │     1  VCC 3.3 V (GBA's own rail — sense only, do not draw power)
   │    2   4   6  │     2  SO   data out of the GBA
    ╲_____________╱      3  SI   data into the GBA
                         4  SD   (unused by LinkBus; reserved)
                         5  SC   clock (driven by the module)
                         6  GND
```

**Wire colours differ between cables, so always buzz out your plug with a meter.** Some aftermarket cables omit SD; LinkBus doesn't need it.

**Which consoles work:**

| Console | Works? |
|---|---|
| GBA, GBA SP, Game Boy Player | Yes. Same port, 3.3 V logic in GBA mode. |
| Game Boy Micro | Yes, with a Micro-style link plug (different connector, same signals). |
| DS / DS Lite | **No.** The DS hardware has no GBA link port, so a DS-Lite-based **Game Boy Macro** can't use LinkBus modules. |
| Game Boy Color | **No.** GBC mode has no multiboot and uses 5 V link levels. Everything here assumes a GBA. |

## 2. The coupler (one cable, swappable modules)

Cut a GBA link cable and keep the end with the GBA plug, about 20–40 cm. Terminate it in a keyed 6-pin connector. Every module then carries the mating half.

**Prototype connector:** JST-PH 2.0 mm, 6 pin. It's keyed, cheap, easy to buy in Canada, and the female housing goes on the cable.

**Later:** a 6-pin magnetic pogo pair, if you want snap-on modules.

| Coupler pin | Signal | Direction | Module side |
|---|---|---|---|
| 1 | GND | — | GND |
| 2 | SC | module → GBA | GPIO, 220 Ω series, 10 k pull-up to 3.3 V |
| 3 | SI | module → GBA | GPIO, 220 Ω series |
| 4 | SO | GBA → module | GPIO input, 220 Ω series, 10 k pull-up to 3.3 V |
| 5 | SD | reserved | leave unconnected |
| 6 | VGBA | GBA → module | optional "GBA is on" sense: 10 k to a GPIO. Never back-feed. |

**Rules every module must follow:**

1. **Self-powered.** The GBA's 3.3 V pin can't feed an ESP's ~300 mA Wi-Fi peaks. Back-feeding it with the GBA off can also latch up the GBA.
2. **3.3 V logic.** ESP-family parts connect directly through the series resistors. 5 V MCUs need level shifting.
3. **One module at a time.** Each module is the bus master; there's no hub.
4. **The SC pull-up matters.** It keeps the clock idle-high while the module is booting, so a running GBA doesn't see phantom clock edges.

## 3. CNC pendant module (ESP-12F)

```
                  ┌───────────────────┐
  5 V ──► AMS1117-3.3 (or AP2112K) ─► 3V3          100 µF + 100 nF at the module
                  │ ESP-12F           │
  coupler SC ◄─220Ω── GPIO14          │
  coupler SI ◄─220Ω── GPIO13          │
  coupler SO ──220Ω─► GPIO12          │
                  │ GPIO1 (TX) ───────┼──────────────► GRBL RX
                  │ GPIO3 (RX) ◄── 2k2 ─┬─────────── GRBL TX (5 V)
                  │                    3k3
                  │                     └── GND
                  │ GPIO2  ── debug log (Serial1, 115200, TX only)
                  │ EN, RST ── 10 k up · GPIO0 10 k up · GPIO2 10 k up · GPIO15 10 k down
                  └───────────────────┘
```

- **GRBL TX divider.** 2.2 k over 3.3 k turns 5 V into 3.0 V at GPIO3.
- **GRBL RX.** The ESP's 3.3 V TX into a 5 V ATmega RX reads as HIGH (the threshold is 0.6 × VCC = 3.0 V). This works on most boards; if it's flaky, add a 74AHCT1G125 buffer.
- **Power.** Take 5 V from the controller's header **only if** it can supply about 300 mA. Otherwise use a separate USB 5 V.
- **Flashing.** GPIO1/GPIO3 are also the ESP's programming UART, so unplug the GRBL wires for the first USB flash. After that, flash over Wi-Fi with `pio run -e ota -t upload`.

### Finding GRBL's serial on the Genmitsu board

The 3020 Pro Max controller is a GRBL board with a USB-serial chip in front of the MCU's UART. Look in this order:

1. **The offline-controller header.** Many GRBL boards in this class have an 8-pin header for the hand-held offline controller, carrying 5 V, GND, TX, RX and a few spare lines. Confirm with a meter (TX idles at 5 V) before connecting anything.
2. **The MCU's UART pins directly.** On an ATmega328P (TQFP) that's pin 30 (PD0/RXD) and pin 31 (PD1/TXD).

**Don't run a PC sender over USB at the same time.** The USB-serial chip also drives the MCU's RX. Most boards put a ~1 k resistor in that path so an external TX can win, but check for it (measure from the USB chip's TXD to MCU RXD). If there's no resistor, add one or cut in a diode-OR before connecting the ESP.

**Probing:** the "Probe Z" macro uses GRBL's probe input (A5 on 328P boards). For PCBs, clip the probe lead to the copper and the bit acts as the contact. With `PROBE_THICKNESS = 0`, Z0 ends up on the copper surface.

## 4. Bill of materials (pendant module)

| Part | Qty | Note |
|---|---|---|
| ESP-12F | 1 | you have these |
| AMS1117-3.3 or AP2112K-3.3 | 1 | + 10 µF in/out |
| 100 µF electrolytic, 100 nF ceramic | 1 each | at ESP VCC |
| 220 Ω | 3 | link series resistors |
| 10 k | 6 | pull-ups/downs |
| 2.2 k + 3.3 k | 1 each | GRBL TX divider |
| JST-PH 6-pin pair | 1 | coupler |
| JST-XH or Dupont 4-pin | 1 | to GRBL header (5 V, GND, TX, RX) |
| sacrificed GBA link cable | 1 | GBA plug end |
| Momentary switch | optional | a real E-stop is **separate**, see below |

**E-stop:** a hardware switch that cuts motor/spindle power. The GBA's feed hold and stop are conveniences, not safety devices.

A single-sided board for this fits nicely on the 3020: 0805 passives, ESP-12F castellations on the board edge, and the JST footprints through-hole.
