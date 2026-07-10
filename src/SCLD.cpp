/*

ESP2068 — TS2068 port of ESPectrum

SCLD.cpp — see include/SCLD.h for the interface contract this implements.
Every body here is a stub (Phase 1, PLAN.md): they compile and link, and
resolveMemChunks() already runs the real resolve loop from the plan doc,
but homePage()/dockPage()/exromPage() return nullptr until slice 1 wires
up HOME's backing store and slice 3 wires up DOCK/EXROM image loading.

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

#include "SCLD.h"

uint8_t SCLD::mmuSelect = 0x00;
uint8_t SCLD::displayControl = 0x00;
uint8_t SCLD::videoMode = 0;
uint8_t SCLD::hiresInkPaper = 0;
bool    SCLD::intInhibit = false;
bool    SCLD::exromSelect = false;

uint8_t* SCLD::memChunk[8] = { nullptr };
bool     SCLD::memChunkReadOnly[8] = { false };

void SCLD::resolveMemChunks() {
    for (int c = 0; c < 8; c++) {
        if (mmuSelect & (1 << c)) {
            memChunk[c] = exromSelect ? exromPage(c) : dockPage(c);
        } else {
            memChunk[c] = homePage(c);
        }
    }
}

uint8_t* SCLD::homePage(int chunk) {
    // Stub. Slice 1: chunks 0-1 -> HOME ROM, chunks 2-7 -> HOME RAM.
    return nullptr;
}

uint8_t* SCLD::dockPage(int chunk) {
    // Stub. Slice 3 wires this to the loaded .DCK's per-chunk 8K images.
    return nullptr;
}

uint8_t* SCLD::exromPage(int chunk) {
    // Stub. Slice 3 wires this to the loaded EXROM's per-chunk 8K images.
    return nullptr;
}

void SCLD::OUT_F4(uint8_t value) {
    mmuSelect = value;
    resolveMemChunks();
}

uint8_t SCLD::IN_F4() {
    return mmuSelect;
}

void SCLD::OUT_FF(uint8_t value) {
    displayControl = value;
    videoMode      = value & 0x07;
    hiresInkPaper  = (value >> 3) & 0x07;
    intInhibit     = value & 0x40;
    exromSelect    = value & 0x80;
    resolveMemChunks();
}

uint8_t SCLD::IN_FF() {
    return displayControl;
}

void SCLD::reset() {
    mmuSelect = 0x00;
    OUT_FF(0x00);
}
