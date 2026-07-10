/*

ESP2068 — TS2068 port of ESPectrum

DockLoader.h — .DCK cartridge container loader (slice 3, PLAN.md Phase 2).
New for this port; there is no upstream ESPectrum equivalent. Parses the
.DCK format documented in test/dock/README.md and populates SCLD's
DOCK/EXROM backing store (include/SCLD.h) accordingly.

See src/DockLoader.cpp's top comment for the autostart design and its
honest limits — this does not parse a cartridge's own in-ROM LROS/AROS
header (start address, magic bytes); that byte layout was not pinned
down with enough confidence during research to implement it correctly.
What's here is a simpler, deliberately-scoped mechanism that gets a
plain LROS-style cartridge running from a cold boot.

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

#ifndef DockLoader_h
#define DockLoader_h

#include <inttypes.h>
#include <cstddef>
#include <string>

class DockLoader {
public:

    // Parses .DCK bytes already in memory and, only if the whole file is
    // structurally valid, applies it to SCLD (all-or-nothing — a bad file
    // never leaves SCLD half-updated). `data`/`size` are the caller's;
    // DockLoader copies whatever it needs and does not hold onto the
    // pointer afterwards. Returns false and leaves SCLD's cartridge state
    // untouched on any structural error (truncated header, truncated
    // chunk data — mirrors test/dock/dck_format.py's parse() exactly).
    //
    // Host-testable: no ESP-IDF dependency, works identically under
    // ESP2068_HOST_TEST (see SCLD.cpp for the same convention).
    static bool loadFromBuffer(const uint8_t* data, size_t size);

    // Reads fn fully into memory (plain stdio — fopen/fread/fclose, no
    // ESP-IDF-specific headers, so this is host-testable too) and calls
    // loadFromBuffer(). Returns false if the file can't be opened/read or
    // fails loadFromBuffer()'s validation.
    static bool loadFromFile(const std::string& fn);

    // Frees whatever the last successful load allocated and clears SCLD's
    // DOCK/EXROM chunks back to unpopulated (cartridge eject). Safe to
    // call with nothing loaded.
    static void unload();

    // True if the last successful load's DOCK bank had chunk 0 present —
    // the LROS convention (see DockLoader.cpp) for "this cartridge takes
    // over at reset."
    static bool hasAutostart();

    // The port 0xF4 value to apply on reset for the autostart candidate:
    // one bit per DOCK chunk the cartridge actually populated. Only
    // meaningful when hasAutostart() is true.
    static uint8_t autostartMmuSelect();

};

#endif // DockLoader_h
