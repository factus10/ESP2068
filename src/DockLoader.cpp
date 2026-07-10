/*

ESP2068 — TS2068 port of ESPectrum

DockLoader.cpp — see include/DockLoader.h.

Container format (bank ID + 8 chunk-type bytes per bank, 8192-byte images
for present chunks, repeat to EOF) matches test/dock/dck_format.py
exactly — that Python module was written first and cross-checked against
two independent copies of the source reference; this is a direct C++
port of the same logic, not a re-derivation.

Autostart design, and why it's simpler than the real hardware:

TS2068-ESPECTRUM-PORT-PLAN.md's "Cartridge loading" section says LROS/
AROS autostart cartridges "page themselves in and vector execution on
reset" and points at check_trdos (Z80_JLS.cpp) as a working template —
that mechanism watches the Z80's PC on every jump/call/return, because
TR-DOS can be entered at any point during a running program, not just at
boot. Research into the real TS2068 (see PLAN.md's slice 3 writeup)
turned up the *shape* of real hardware's boot protocol: a cartridge has
its own short in-ROM header (a "Memory Chunk Specification" byte and a
2-byte jump address), and the real EXROM's initialization code reads
that header and hands off control via a "GOTO BANK" routine. It did not
turn up confirmed byte offsets for that header — not enough to implement
correctly rather than guessed.

Two things make a simpler mechanism both sufficient and honest for this
increment: LROS cartridges are defined to always autorun "after
initialization is finished" (i.e. at boot, not mid-session — unlike
TR-DOS), and they take total control from address 0, which is exactly
where the Z80 already fetches its first opcode after a reset. So instead
of parsing an in-ROM header we don't have confirmed byte offsets for,
DockLoader derives the same *outcome* — the cartridge's chunks paged in
before the first opcode fetch — straight from the .DCK container's own
chunk-presence bits, which are fully spec'd and already parsed here
regardless. hasAutostart()/autostartMmuSelect() expose that; the actual
reset-time hookup lives in CPU::reset() (see PLAN.md).

This does not attempt AROS autostart (depends on a working System ROM,
which doesn't exist yet — see slice 1's still-open ROM-image TODO) or a
mid-session, check_trdos-style PC watch (nothing to watch for yet without
a real EXROM image with a "launch cartridge" entry point). Revisit both
once those exist.

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

#include "DockLoader.h"
#include "SCLD.h"
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef ESP2068_HOST_TEST
#include <cstdlib>
static inline uint8_t* dockloader_alloc8k() { return (uint8_t*) calloc(0x2000, 1); }
static inline void dockloader_free(uint8_t* p) { free(p); }
#else
#include "esp_heap_caps.h"
static inline uint8_t* dockloader_alloc8k() { return (uint8_t*) heap_caps_calloc(0x2000, 1, MALLOC_CAP_8BIT); }
static inline void dockloader_free(uint8_t* p) { heap_caps_free(p); }
#endif

namespace {

    const size_t HEADER_SIZE = 9;
    const size_t CHUNK_SIZE = 0x2000;
    const uint8_t BANK_DOCK = 0;
    const uint8_t BANK_EXROM = 254;
    const uint8_t BANK_HOME = 255;

    struct PendingChunk {
        uint8_t bankId;
        int index;       // 0..7
        bool writable;
        const uint8_t* src; // points into the caller's buffer, not owned
    };

    // Tracks every chunk buffer this loader has handed to SCLD, so
    // unload() can free them and eject the cartridge cleanly.
    std::vector<uint8_t*> allocatedChunks;

    uint8_t autostartMask = 0;
    bool autostartCandidate = false;

}

void DockLoader::unload() {
    for (int c = 0; c < 8; c++) SCLD::unloadDockChunk(c);
    SCLD::loadExromImage(nullptr);
    for (uint8_t* p : allocatedChunks) dockloader_free(p);
    allocatedChunks.clear();
    autostartMask = 0;
    autostartCandidate = false;
    SCLD::resolveMemChunks();
}

bool DockLoader::loadFromBuffer(const uint8_t* data, size_t size) {

    // ---- Pass 1: validate structure and collect chunk locations. Does
    // not touch SCLD or allocate anything, so a bad file changes nothing. ----
    std::vector<PendingChunk> pending;
    size_t offset = 0;
    while (offset < size) {

        if (offset + HEADER_SIZE > size) return false; // truncated header

        uint8_t bankId = data[offset];
        const uint8_t* typeBytes = data + offset + 1;
        offset += HEADER_SIZE;

        for (int c = 0; c < 8; c++) {
            uint8_t t = typeBytes[c];
            if (!(t & 0x02)) continue; // no image for this chunk

            if (offset + CHUNK_SIZE > size) return false; // truncated chunk image

            PendingChunk pc;
            pc.bankId = bankId;
            pc.index = c;
            pc.writable = t & 0x01;
            pc.src = data + offset;
            pending.push_back(pc);

            offset += CHUNK_SIZE;
        }
    }

    // ---- Pass 2: the file is structurally valid. Apply it. ----
    unload(); // clear whatever cartridge (if any) was loaded before

    uint8_t dockMask = 0;
    for (const PendingChunk& pc : pending) {

        if (pc.bankId == BANK_HOME) {
            // Not this loader's job — see this file's top comment and
            // slice 1's still-open "real ROM image" TODO. Deliberately
            // not applied anywhere.
            continue;
        }
        if (pc.bankId != BANK_DOCK && pc.bankId != BANK_EXROM) {
            // Reserved bank IDs (1..253): nothing defined to do with them.
            continue;
        }

        uint8_t* buf = dockloader_alloc8k();
        if (!buf) { unload(); return false; } // out of memory: fail cleanly, not partially
        memcpy(buf, pc.src, CHUNK_SIZE);
        allocatedChunks.push_back(buf);

        if (pc.bankId == BANK_DOCK) {
            SCLD::loadDockChunk(pc.index, buf, pc.writable);
            dockMask |= (1 << pc.index);
        } else { // BANK_EXROM
            SCLD::loadExromImage(buf);
        }
    }

    autostartMask = dockMask;
    autostartCandidate = (dockMask & 0x01) != 0; // chunk 0 present: LROS convention
    SCLD::resolveMemChunks();

    return true;
}

bool DockLoader::loadFromFile(const std::string& fn) {

    FILE* file = fopen(fn.c_str(), "rb");
    if (file == NULL) {
        printf("DockLoader: error opening %s\n", fn.c_str());
        return false;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);

    if (size <= 0) {
        printf("DockLoader: %s is empty or unreadable\n", fn.c_str());
        fclose(file);
        return false;
    }

    uint8_t* buf = (uint8_t*) malloc((size_t) size);
    if (!buf) {
        printf("DockLoader: out of memory reading %s (%ld bytes)\n", fn.c_str(), size);
        fclose(file);
        return false;
    }

    size_t got = fread(buf, 1, (size_t) size, file);
    fclose(file);

    bool ok = (got == (size_t) size) && loadFromBuffer(buf, got);
    free(buf); // loadFromBuffer() copies whatever it keeps; this is scratch

    if (!ok) printf("DockLoader: %s failed to load (invalid .DCK)\n", fn.c_str());

    return ok;
}

bool DockLoader::hasAutostart() {
    return autostartCandidate;
}

uint8_t DockLoader::autostartMmuSelect() {
    return autostartMask;
}
