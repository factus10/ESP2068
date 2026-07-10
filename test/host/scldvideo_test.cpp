/*

ESP2068 — TS2068 port of ESPectrum

test/host/scldvideo_test.cpp — standalone, host-compilable test for
SCLDVideo's byte-to-pixel expansion (include/SCLDVideo.h,
src/SCLDVideo.cpp). No test framework, no ESP-IDF dependency at all (this
module doesn't need the ESP2068_HOST_TEST allocator-shim convention
SCLD.cpp/DockLoader.cpp use, since it does no allocation).

Build and run:

    g++ -std=c++17 -Wall -I include \
        test/host/scldvideo_test.cpp src/SCLDVideo.cpp \
        -o /tmp/scldvideo_test \
    && /tmp/scldvideo_test

*/

#include "SCLDVideo.h"
#include <cstdio>
#include <cstring>

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

    printf("SCLDVideo host test\n");
    printf("====================\n");

    // ---- Standard mode: bit order and pixel-doubling ----
    printf("\n-- expandStandardByte --\n");
    {
        // bmp = 0b10110000: ink shows at source pixels 0,2,3 (bit7,bit5,bit4)
        // att = ink 2 (red), paper 5 (cyan)
        uint8_t out[16];
        SCLDVideo::expandStandardByte(0b10110000, (5 << 3) | 2, out);
        uint8_t expected[16] = { 2,2, 5,5, 2,2, 2,2, 5,5, 5,5, 5,5, 5,5 };
        check(memcmp(out, expected, 16) == 0, "0b10110000/ink2paper5 expands to the expected 16-pixel doubled line");

        // All-paper byte: every output pixel should be the paper color
        SCLDVideo::expandStandardByte(0x00, (7 << 3) | 0, out);
        bool allPaper = true;
        for (int i = 0; i < 16; i++) if (out[i] != 7) allPaper = false;
        check(allPaper, "an all-zero bitmap byte expands to 16 paper-color pixels");

        // All-ink byte
        SCLDVideo::expandStandardByte(0xFF, (7 << 3) | 3, out);
        bool allInk = true;
        for (int i = 0; i < 16; i++) if (out[i] != 3) allInk = false;
        check(allInk, "an all-one bitmap byte expands to 16 ink-color pixels");

        // Bit 7 is confirmed leftmost (per the reference library's pixel
        // address formula comment) -- so a single high bit at bit 7 must
        // land at output pixels 0 and 1, not 14 and 15.
        SCLDVideo::expandStandardByte(0b10000000, (0 << 3) | 1, out);
        check(out[0] == 1 && out[1] == 1 && out[14] == 0 && out[15] == 0,
              "bit 7 (leftmost source pixel) maps to output pixels 0-1, not 14-15");
    }

    // ---- Hi-res mode: interleave order and pixel-doubling ----
    printf("\n-- expandHiresBytePair --\n");
    {
        // DFILE1 all-ink, DFILE2 all-paper: even columns should be 1, odd 0
        uint8_t out[16];
        SCLDVideo::expandHiresBytePair(0xFF, 0x00, out);
        bool interleaved = true;
        for (int i = 0; i < 16; i++) {
            uint8_t expected = (i % 2 == 0) ? 1 : 0;
            if (out[i] != expected) interleaved = false;
        }
        check(interleaved, "DFILE1=0xFF/DFILE2=0x00 gives alternating 1,0,1,0... (DFILE1 on even columns)");

        // Swap the source bytes: odd columns should now be 1
        SCLDVideo::expandHiresBytePair(0x00, 0xFF, out);
        bool swapped = true;
        for (int i = 0; i < 16; i++) {
            uint8_t expected = (i % 2 == 0) ? 0 : 1;
            if (out[i] != expected) swapped = false;
        }
        check(swapped, "DFILE1=0x00/DFILE2=0xFF gives the mirror-image pattern (DFILE2 on odd columns)");

        // Bit 7 (leftmost) of each source byte maps to output columns 0/1
        SCLDVideo::expandHiresBytePair(0b10000000, 0b10000000, out);
        check(out[0] == 1 && out[1] == 1, "bit 7 of both source bytes maps to output columns 0 and 1");
    }

    // ---- hiresInkPaperColors: all 8 entries of the plan doc's table ----
    printf("\n-- hiresInkPaperColors (all 8 pairs) --\n");
    {
        // {field value, expected ink, expected paper} per
        // TS2068-ESPECTRUM-PORT-PLAN.md's port 0xFF table.
        struct { uint8_t field, ink, paper; const char* name; } table[8] = {
            {0b000, 0, 7, "black/white"},
            {0b001, 1, 6, "blue/yellow"},
            {0b010, 2, 5, "red/cyan"},
            {0b011, 3, 4, "magenta/green"},
            {0b100, 4, 3, "green/magenta"},
            {0b101, 5, 2, "cyan/red"},
            {0b110, 6, 1, "yellow/blue"},
            {0b111, 7, 0, "white/black"},
        };
        for (auto& row : table) {
            uint8_t ink, paper;
            SCLDVideo::hiresInkPaperColors(row.field, ink, paper);
            char label[64];
            snprintf(label, sizeof(label), "0b%d%d%d (%s) -> ink=%d paper=%d",
                     (row.field >> 2) & 1, (row.field >> 1) & 1, row.field & 1,
                     row.name, row.ink, row.paper);
            check(ink == row.ink && paper == row.paper, label);
        }
    }

    printf("\n====================\n");
    if (failures == 0) {
        printf("ALL PASSED\n");
    } else {
        printf("%d CHECK(S) FAILED\n", failures);
    }

    return failures;
}
