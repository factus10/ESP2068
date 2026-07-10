/*

ESP2068 — TS2068 port of ESPectrum

SCLDVideo.h — pixel-expansion core for the TS2068's SCLD display modes.
New for this port; there is no upstream ESPectrum equivalent.

This is deliberately narrow: it answers "given these source bytes, which
of the 16 output pixel positions are ink vs paper" and nothing else — not
color/RGB conversion, not framebuffer writes, not VGA/DMA timing. That
keeps it pure and host-testable (see test/host/scldvideo_test.cpp), which
matters because the *spatial* mapping (bit order, interleave order) is
exactly the kind of off-by-one that's easy to get subtly wrong and hard to
notice by eye on real hardware — a wrong ink/paper swap or reversed column
order still "looks like a picture," just a mirrored or inverted one.

Two source bit → output pixel mappings, both confirmed against a TS2068
reference library (see PLAN.md's slice 2 writeup for provenance):

  - Standard mode: one 8-pixel source byte + one attribute byte -> 16
    output pixels (doubled, since the physical VGA mode this port always
    boots into is fixed at 512 active columns — see
    TS2068-ESPECTRUM-PORT-PLAN.md's "Display" section). Bit 7 of the
    source byte is the leftmost pixel (confirmed: TS2068 reference
    library's pixel-address formula comment, "bit 7 = leftmost pixel" —
    same convention as the Spectrum this hardware descends from).

  - Hi-res (64-column) mode: two 8-pixel source bytes, one from each
    display file, interleaved column-by-column into 16 output pixels —
    DFILE1's bit i lands at output position 2i, DFILE2's bit i at 2i+1.
    This interleave *order* (which file supplies even vs odd columns) is
    the one piece of this file NOT independently confirmed by the
    reference library research for this port (see DockLoader.cpp-style
    honesty: the reference library confirms 64-column mode exists and is
    DECR bit 2, but not the exact SCLD-hardware interleave order) — it
    matches this project's original plan doc's description and is
    internally consistent, but treat it as needing hardware verification
    before trusting a real hi-res image's left/right column order.

Based on ESPectrum, a Sinclair ZX Spectrum emulator for Espressif ESP32 SoC
Copyright (c) 2023-2025 Víctor Iborra [Eremus] and 2023 David Crespo [dcrespo3d]
https://github.com/EremusOne/ESPectrum

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

*/

#ifndef SCLDVideo_h
#define SCLDVideo_h

#include <inttypes.h>

class SCLDVideo {
public:

    // Expands one standard-mode source byte (bitmap) + its attribute byte
    // into 16 output pixels (physical, pixel-doubled). out[i] is a color
    // index 0..7 (the standard Spectrum ink/paper palette — see
    // TS2068-ESPECTRUM-PORT-PLAN.md/the reference library's "Attribute
    // Byte Format": bits 2..0 = ink, 5..3 = paper). BRIGHT/FLASH are
    // attribute-level modifiers, not per-pixel, so this function leaves
    // them out of `out[]` — callers read att's bits 6/7 directly.
    static void expandStandardByte(uint8_t bmp, uint8_t att, uint8_t out[16]);

    // Expands one hi-res-mode source byte pair (one bit-plane byte from
    // each display file) into 16 output pixels, interleaved column by
    // column. out[i] is 0 (paper) or 1 (ink) — hi-res mode has no
    // per-pixel color, just the two colors SCLD::hiresInkPaper selects
    // globally for the whole screen (see SCLD.h).
    static void expandHiresBytePair(uint8_t dfile1Byte, uint8_t dfile2Byte, uint8_t out[16]);

    // Decodes SCLD::hiresInkPaper (0..7) into the two Spectrum-palette
    // color indices it selects, per TS2068-ESPECTRUM-PORT-PLAN.md's
    // port 0xFF table. inkOut/paperOut are 0..7, same palette as
    // expandStandardByte()'s out[] values.
    static void hiresInkPaperColors(uint8_t hiresInkPaper, uint8_t& inkOut, uint8_t& paperOut);

};

#endif // SCLDVideo_h
