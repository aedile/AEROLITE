# Third-party code

## MAME (BSD-3-Clause)

The machine model in `core/asteroids.c` — memory map, the bit-per-address input
ports, the 74LS253 DIP multiplexer, the periodic NMI, and the Digital Vector
Generator's instruction set and rate-multiplier vector arithmetic — is written
from MAME's `src/mame/atari/asteroid.cpp`, `asteroid_m.cpp` and
`src/devices/video/avgdvg.cpp`. Copyright Brad Oliver, Bernd Wiebelt, Allard van
der Bas, Mathis Rosenhauer and the MAME team; used under the BSD-3-Clause
license:

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED. IN NO EVENT SHALL THE
COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

## The 6502

`core/m6502fast.h` was written for this project and is covered by the project
license. It passes Klaus Dormann's 6502 functional test in full, including
decimal mode.

## Sound

The cabinet's audio is entirely discrete analogue and MAME models it with a
netlist. That is not runnable on a microcontroller, so `ast_render_audio()` is
an original synthesiser driven from the same latches. It is a reconstruction,
not a port.

## ROMs

No game ROMs are included in this repository, and none ever will be. Supplying
them is up to whoever builds the thing.
