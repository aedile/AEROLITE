# AEROLITE

**Atari's 1979 Asteroids, emulated on an ESP32-C6 Fiesta medal.** A 6502, the
Digital Vector Generator, and a synthesiser standing in for a board full of
discrete analogue sound — running at the cabinet's 61.5 Hz with every frame
drawn.

A San Antonio Fiesta medal is a collectible pin. This one plays Asteroids.

---

## 🎮 Quick Start Guide

### How to Play

You hold the medal upright, like a phone.

| Control | What it does |
|---|---|
| **Tilt left / right** | Rotate the ship |
| **Tilt away from you** | Thrust |
| **Middle button, tap** | Fire |
| **Middle button, hold** | Hyperspace |
| **Power button, short press** | Insert a coin and start |
| **Power button, hold 1 second** | Power off |

Tilt is measured against **however you are holding it right now**. A neutral
pose is captured on the first sensor reading and again every time you coin up,
so if the ship rotates on its own, press the power button to re-centre.

The button is passed through as a level, exactly as the cabinet wires it — the
game does its own edge detection and fires once per press.

### Charging

USB-C. Holding the power button for a second cuts the battery rail; do that
before storing it.

### Troubleshooting

**The ship spins by itself.** Coin up to re-capture the neutral pose.

**It's black and white.** That is correct. Arcade Asteroids was a genuine
black-and-white vector monitor. The colour you may remember is the Atari 2600
port, which was a raster console and coloured things in.

**The small text is hard to read.** Also somewhat correct. The cabinet was a
horizontal 4:3 monitor, and holding that shape on an upright 240×280 panel
leaves the copyright line about four pixels tall. Strokes are anti-aliased to
claw back what can be clawed back. Filling more of the panel means stretching
the picture vertically; that trade is one constant in `main/render.cpp`.

---

## 🔨 Building Your Own

You need a **Waveshare ESP32-C6-LCD-1.69**, the Asteroids ROM set, Docker, and
esptool.

```sh
git clone https://github.com/aedile/AEROLITE.git
cd AEROLITE
python3 tools/convert_roms.py /path/to/asteroid
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.3.4 \
    idf.py -B build_docker build
cd build_docker && esptool --chip esp32c6 -p /dev/cu.usbmodemXXXX \
    -b 460800 write_flash @flash_args
```

Flashing has to run **from inside `build_docker`** — the paths in `flash_args`
are relative to it — and **from the host**, because Docker Desktop on macOS
cannot reach USB.

### The ROMs

Not included. You need MAME's `asteroid` set: `035145-04e.ef2`,
`035144-04e.h2`, `035143-02.j2` (program) and `035127-02.np3` (vector ROM). The
converter checks every CRC.

---

## 🔬 Technical Details

### The original hardware

One 6502 at 1.512 MHz and a Digital Vector Generator. **There is no frame
buffer and no graphics ROM.** The CPU builds a display list in 2 KB of vector
RAM; the DVG walks it and steers the beam. What this emulator's core produces
is therefore a list of line segments, not a raster.

The interrupt is a periodic NMI at 246.09 Hz, and the display refreshes on every
fourth one — so a frame is exactly 24576 CPU cycles.

### The vector generator

The DVG's instruction set is fourteen opcodes over 16-bit words: draw, load
absolute position, jump, call, return, halt, and a one-word short vector that
borrows three scale bits from the top bits of its own delta fields.

MAME simulates it at gate level, driven by its 256-byte state PROM, because that
is the honest thing to do. This does it functionally. The one part that needs
care is the vector walk itself: on the real board each vector is stepped by a
pair of cascaded 7497 rate multipliers, so over `fin` clocks the x counter steps
`fin × mx / 4096` times and likewise for y, which is what gives a line its
slope. MAME runs that loop. We compute the endpoint directly — and then charge
the CPU the `fin` cycles the DVG would have spent, because the game polls the
VG_HALT line and needs to see the generator busy.

### Framing

The beam window is the part that decides where everything sits. The game draws
the full 1024 across but only rows 96–927, centred on 512. Mapping *that window*
onto the 4:3 picture — rather than a full square — is what puts the score at the
top of the screen and the copyright line at the bottom, where they belong.

Lines are anti-aliased. At 240×180 a hard one-pixel Bresenham stroke that falls
between two rows lands entirely on one of them, and small text breaks up into
something unreadable. Spreading each step across two pixels costs one extra
write and is closer to what a phosphor does anyway. Writes are additive, so
where the beam crosses itself the trace is brighter.

### The 6502

`core/m6502fast.h` is an instruction-stepped NMOS 6502 written for this family
of projects. It passes **Klaus Dormann's 6502 functional test** in full,
including decimal mode. It exists because the cycle-stepped core it replaced
carries its pin state in a `uint64_t`, which costs about 200 CPU clocks per
emulated 6502 cycle on a 32-bit RISC-V — roughly four times the budget.

### Audio

The cabinet's sound is entirely discrete analogue: a 555 thump VCO whose pitch
comes off a 4-bit DAC, an LFSR noise source, and one-shots for fire, thrust,
saucer and the extra-ship ping. MAME models the whole thing as a netlist, which
is not a thing a microcontroller is going to run.

So this is a synthesiser driven from the same latches — a reconstruction, not a
port. It is the one part of this project that is *like* the hardware rather than
*from* it.

---

## 📁 Project Structure

```
core/           platform-independent emulation, shared with the host harness
  asteroids.c     memory map, the DVG, and the sound synthesiser
  m6502fast.h     instruction-stepped NMOS 6502
main/           the ESP32 application
  render.cpp      anti-aliased vector rasteriser, letterboxed 240×180
  input.cpp       tilt-to-rotation, tip-to-thrust, button
components/     display, IMU and audio HAL for the Waveshare board
host/           builds the same core on a desktop; frames to PPM, audio to WAV
tools/          ROM converter
```

## 💻 Running It on Your Computer

```sh
cd host && make
./harness /tmp/out 20 --every 1 --dsw 0x04 --wav /tmp/ast.wav \
    --script "1.0:coin=1,1.2:coin=0,2.0:start=1,2.2:start=0,4.0:thrust=1,6.0:fire=1"
```

The harness also prints the bounding box of every vector it drew, which is how
the beam window above was determined. Script keys: `coin`, `start`, `fire`,
`thrust`, `left`, `right`, `hyper`.

## ⚙️ Configuration

| What | Where |
|---|---|
| DIP switches | `ast_set_dips()` in `main/main.cpp` — `0x04` is free play, English, 3 ships |
| Picture size and shape | `PIC_W`, `PIC_H`, `TOP_BAR` in `main/render.cpp` |
| Tilt sensitivity | `ROT_DEADBAND_DEG`, `THRUST_DEG` in `main/input.cpp` |
| Tilt direction | `ROT_SIGN`, `THRUST_SIGN` in `main/input.cpp` |
| Hyperspace hold time | `HOLD_HYPER_US` in `main/input.cpp` |

## 📌 Status and Known Gaps

Runs at full speed with sound, every frame drawn, nothing skipped or dropped.

- Sound is a reconstruction. It is recognisably Asteroids; it is not the netlist.
- The DVG is functional rather than gate-level. Vector endpoints are computed
  instead of stepped, so a display list that relied on mid-vector clipping
  behaviour would differ. Nothing in this game does.
- Cocktail flip is not implemented.
- The 50-pixel letterbox bars are empty. They could hold a marquee.

## 📄 Legal Notice

### ROM files

No ROMs here. Asteroids is © 1979 Atari. This project ships a converter, not a
game.

### Third-party code

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The machine model and the
DVG are written from MAME (BSD-3-Clause).

### Disclaimer

Not affiliated with, endorsed by, or connected to Atari, its successors, or the
Fiesta San Antonio Commission.

## 🙏 Credits

The MAME team, and in particular whoever documented the DVG's rate multipliers
well enough that they could be turned back into arithmetic. Klaus Dormann, for
the test that made the new CPU core trustworthy.

## 📜 License

[0BSD](LICENSE) — no attribution required, no conditions.
