/*

ESP2068 — TS2068 port of ESPectrum

test/host/scld_test.cpp — standalone, host-compilable unit test for the
SCLD memory model (include/SCLD.h, src/SCLD.cpp). This is the "simple
host-side harness" PLAN.md's Phase 2 slice 1 exit criteria calls for: it
exercises the real SCLD.cpp (built with ESP2068_HOST_TEST so it uses
calloc() instead of heap_caps_calloc(), see SCLD.cpp), not a reimplementation
of it, so a pass here means the actual firmware code resolves memChunk[]
correctly — outside of src/CPU.cpp's poke8_2068/poke16_2068, which are the
functions that actually enforce memChunkReadOnly[] on real writes, and which
this test does not re-test (see the note by TEST("write-protect...") below).

No test framework dependency, no ESP-IDF dependency. Build and run:

    g++ -std=c++17 -Wall -I include -DESP2068_HOST_TEST \
        test/host/scld_test.cpp src/SCLD.cpp -o /tmp/scld_test \
    && /tmp/scld_test

Exits 0 and prints "ALL PASSED" if every check passes; otherwise prints
each failure and exits with the failure count.

*/

#include "SCLD.h"
#include <cstdio>

static int failures = 0;

static void check(bool cond, const char* what) {
    if (cond) {
        printf("  PASS  %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        failures++;
    }
}

int main() {

    printf("SCLD host test\n");
    printf("==============\n");

    SCLD::allocateMemory();
    SCLD::reset();

    // ---- Reset state ----
    printf("\n-- reset state --\n");
    check(SCLD::mmuSelect == 0x00, "mmuSelect is 0x00 after reset");
    check(SCLD::displayControl == 0x00, "displayControl is 0x00 after reset");
    check(SCLD::exromSelect == false, "exromSelect is false after reset");
    check(SCLD::intInhibit == false, "intInhibit is false after reset");
    check(SCLD::videoMode == 0, "videoMode is 0 after reset");
    check(SCLD::hiresInkPaper == 0, "hiresInkPaper is 0 after reset");

    // ---- All chunks resolve to HOME on reset (0xF4 = 0x00, all HOME) ----
    printf("\n-- all-HOME resolution --\n");
    bool allNonNull = true;
    for (int c = 0; c < 8; c++) if (!SCLD::memChunk[c]) allNonNull = false;
    check(allNonNull, "memChunk[c] is never null for any of the 8 chunks");
    check(SCLD::memChunkReadOnly[0] && SCLD::memChunkReadOnly[1],
          "HOME ROM chunks (0,1) are read-only");
    bool ramWritable = true;
    for (int c = 2; c < 8; c++) if (SCLD::memChunkReadOnly[c]) ramWritable = false;
    check(ramWritable, "HOME RAM chunks (2-7) are writable");

    // ---- HOME RAM is real, addressable memory ----
    printf("\n-- HOME RAM read/write --\n");
    uint8_t* homeChunk2 = SCLD::memChunk[2];
    homeChunk2[0x100] = 0xAB;
    check(SCLD::memChunk[2][0x100] == 0xAB, "byte written through memChunk[2] reads back");

    // ---- Switching every chunk to DOCK (unpopulated) and back ----
    printf("\n-- 0xF4 = 0xFF, DOCK selected, nothing loaded --\n");
    SCLD::OUT_F4(0xFF); // exromSelect still false from reset -> DOCK
    check(SCLD::IN_F4() == 0xFF, "IN_F4() reflects the value just written");
    check(SCLD::memChunk[2] != homeChunk2,
          "chunk 2 no longer points at HOME RAM once its F4 bit is set");
    bool allReadOnlyEmpty = true;
    for (int c = 0; c < 8; c++) if (!SCLD::memChunkReadOnly[c]) allReadOnlyEmpty = false;
    check(allReadOnlyEmpty, "unpopulated DOCK chunks default to read-only");

    printf("\n-- 0xF4 = 0x00, back to HOME --\n");
    SCLD::OUT_F4(0x00);
    check(SCLD::memChunk[2] == homeChunk2, "chunk 2 points at the same HOME RAM page again");
    check(SCLD::memChunk[2][0x100] == 0xAB, "the earlier marker byte survived the round trip");
    check(!SCLD::memChunkReadOnly[2], "chunk 2 is writable again");

    // ---- DOCK: a writable (RAM cart) chunk and a read-only (ROM cart) chunk ----
    printf("\n-- DOCK: populated chunks --\n");
    uint8_t dockRam[0x2000] = { 0 };
    uint8_t dockRom[0x2000] = { 0 };
    SCLD::loadDockChunk(3, dockRam, /*writable=*/true);
    SCLD::loadDockChunk(4, dockRom, /*writable=*/false);
    SCLD::OUT_F4(0b00011000); // bits 3 and 4
    check(SCLD::memChunk[3] == dockRam, "chunk 3 resolves to the loaded DOCK RAM image");
    check(!SCLD::memChunkReadOnly[3], "DOCK RAM chunk (writable=true) is not read-only");
    check(SCLD::memChunk[4] == dockRom, "chunk 4 resolves to the loaded DOCK ROM image");
    check(SCLD::memChunkReadOnly[4], "DOCK ROM chunk (writable=false) is read-only");

    // ---- EXROM: chip-select mirrored across every chunk that selects it ----
    printf("\n-- EXROM: unpopulated, then loaded, mirrored across two chunks --\n");
    SCLD::OUT_FF(0x80); // exromSelect = true; mmuSelect still 0b00011000 from above
    check(SCLD::memChunkReadOnly[4], "unpopulated EXROM chunk defaults to read-only");
    check(SCLD::memChunk[4] != dockRom, "chunk 4 no longer shows the DOCK image once EXROM is selected");
    uint8_t exromImg[0x2000] = { 0 };
    SCLD::loadExromImage(exromImg);
    SCLD::OUT_F4(0b00110000); // bits 4 and 5 both select EXROM now
    check(SCLD::memChunk[4] == exromImg, "chunk 4 resolves to the loaded EXROM image");
    check(SCLD::memChunk[5] == exromImg, "chunk 5 mirrors the same EXROM image (chip-select, not per-chunk data)");
    check(SCLD::memChunkReadOnly[4] && SCLD::memChunkReadOnly[5], "EXROM chunks are always read-only");

    // ---- IN_F4/IN_FF round-trip ----
    printf("\n-- port read-back --\n");
    check(SCLD::IN_F4() == 0b00110000, "IN_F4() reflects the last OUT_F4() value");
    check(SCLD::IN_FF() == 0x80, "IN_FF() reflects the last OUT_FF() value");

    // ---- Reset again: back to all-HOME, but HOME RAM content survives ----
    printf("\n-- reset again --\n");
    SCLD::reset();
    check(SCLD::mmuSelect == 0x00 && SCLD::displayControl == 0x00, "ports read back 0x00 after reset");
    check(SCLD::memChunk[2] == homeChunk2, "chunk 2 is HOME RAM again");
    check(SCLD::memChunk[2][0x100] == 0xAB, "HOME RAM content survives reset (real hardware doesn't zero RAM on reset)");

    // ---- Write-protect: exercises the same memChunkReadOnly[] gate
    // src/CPU.cpp's poke8_2068/poke16_2068 check on every real Z80 write.
    // This does not call into CPU.cpp (that needs the full Z80 core), so a
    // pass here means the flag is correct, not that CPU.cpp definitely
    // reads it — CPU.cpp's gate is a one-line `if (memChunkReadOnly[page])
    // return;`, reviewed by hand when it was written. ----
    printf("\n-- write-protect flag gates writes correctly --\n");
    uint8_t before = SCLD::memChunk[0][0];
    if (!SCLD::memChunkReadOnly[0]) SCLD::memChunk[0][0] = 0x42; // should not execute
    check(SCLD::memChunk[0][0] == before, "HOME ROM chunk 0 rejects a write gated on memChunkReadOnly[]");

    printf("\n==============\n");
    if (failures == 0) {
        printf("ALL PASSED\n");
    } else {
        printf("%d CHECK(S) FAILED\n", failures);
    }

    return failures;
}
