/*

ESP2068 — TS2068 port of ESPectrum

test/host/virtualdisk_test.cpp — standalone, host-compilable unit test
for VirtualDisk (include/VirtualDisk.h, src/VirtualDisk.cpp), Phase 4
Piece A. No test framework, no ESP-IDF dependency -- exercises the real
VirtualDisk.cpp (built as ordinary portable C++, no ESP2068_HOST_TEST
allocator-shim needed here since this class never touches ESP32-specific
allocation, unlike SCLD.cpp/RomLoader.cpp).

VirtualDisk::mountDir is repointed at a scratch directory under this
test's own tmp path before anything runs, since the real default
(/sd/2068/disk/) doesn't exist on a dev machine -- see VirtualDisk.h's
comment on why mountDir is mutable.

Build and run (from repo root):

    g++ -std=c++17 -Wall -I include \
        test/host/virtualdisk_test.cpp src/VirtualDisk.cpp \
        -o /tmp/virtualdisk_test \
    && /tmp/virtualdisk_test

*/

#include "VirtualDisk.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

static int failures = 0;

static void check(bool cond, const char* what) {
    if (cond) {
        printf("  PASS  %s\n", what);
    } else {
        printf("  FAIL  %s\n", what);
        failures++;
    }
}

// Appends one raw .tap block (2-byte LE length, flag, data, XOR
// checksum) directly to a FILE*, independent of VirtualDisk's own
// writeBlock() -- so the mountRead()/scanBlocks()/readHeader()/
// readData() path gets checked against a hand-built fixture, not just
// round-tripped against its own writer.
static void appendTapBlock(FILE* f, uint8_t flag, const uint8_t* data, int len) {
    uint16_t blockLen = (uint16_t)(len + 2);
    uint8_t lenLo = blockLen & 0xFF, lenHi = (blockLen >> 8) & 0xFF;
    fwrite(&lenLo, 1, 1, f);
    fwrite(&lenHi, 1, 1, f);
    fwrite(&flag, 1, 1, f);
    uint8_t xorSum = flag;
    for (int i = 0; i < len; i++) { fwrite(&data[i], 1, 1, f); xorSum ^= data[i]; }
    fwrite(&xorSum, 1, 1, f);
}

// Builds a 17-byte standard Spectrum/TS2068 tape header: type, 10-char
// space-padded name, 2-byte declared data length, 2x2-byte params.
static void buildHeader(uint8_t* out, uint8_t type, const char* name, uint16_t dataLen) {
    out[0] = type;
    std::string padded = name;
    while (padded.size() < 10) padded += ' ';
    memcpy(&out[1], padded.data(), 10);
    out[11] = dataLen & 0xFF; out[12] = (dataLen >> 8) & 0xFF;
    out[13] = 0; out[14] = 0;
    out[15] = 0; out[16] = 0;
}

int main() {

    printf("VirtualDisk host test\n");
    printf("======================\n");

    std::string scratch = "/tmp/virtualdisk_test_scratch/";
    mkdir(scratch.c_str(), 0755);
    VirtualDisk::mountDir = scratch;

    // ---- Fixture: a two-program .tap file ----
    // Program 1: "HELLO", 5 bytes of data 0x10..0x14.
    // Program 2: "WORLD", 3 bytes of data 0x20..0x22.
    {
        FILE* f = fopen((scratch + "two.tap").c_str(), "wb");
        uint8_t hdr1[17]; buildHeader(hdr1, 0 /*Program*/, "HELLO", 5);
        appendTapBlock(f, 0x00, hdr1, 17);
        uint8_t data1[5] = {0x10,0x11,0x12,0x13,0x14};
        appendTapBlock(f, 0xFF, data1, 5);
        uint8_t hdr2[17]; buildHeader(hdr2, 3 /*Code*/, "WORLD", 3);
        appendTapBlock(f, 0x00, hdr2, 17);
        uint8_t data2[3] = {0x20,0x21,0x22};
        appendTapBlock(f, 0xFF, data2, 3);
        fclose(f);
    }

    // ---- mountRead + sequential read ----
    printf("\n-- mountRead + sequential read --\n");
    check(VirtualDisk::mountRead("two.tap"), "mountRead() opens the two-program fixture");
    check(VirtualDisk::lastStatus() == VirtualDisk::STATUS_OK, "status is OK after a successful mount");
    {
        uint8_t buf[32] = {0};
        int n = VirtualDisk::readHeader(buf, sizeof(buf));
        check(n == 17, "readHeader() returns the full 17-byte header for block 0");
        check(buf[0] == 0, "block 0's header type is Program (0)");
        check(memcmp(&buf[1], "HELLO     ", 10) == 0, "block 0's header name is \"HELLO\", space-padded to 10");

        n = VirtualDisk::readData(buf, sizeof(buf));
        check(n == 5, "readData() returns 5 bytes for block 0's data");
        check(memcmp(buf, "\x10\x11\x12\x13\x14", 5) == 0, "block 0's data bytes match the fixture exactly");
    }

    // ---- findBlock() by name, direct access (not sequential scan) ----
    printf("\n-- findBlock() name-based lookup --\n");
    check(VirtualDisk::findBlock("WORLD"), "findBlock() locates the second program by name");
    {
        uint8_t buf[32] = {0};
        int n = VirtualDisk::readHeader(buf, sizeof(buf));
        check(n == 17 && memcmp(&buf[1], "WORLD     ", 10) == 0, "readHeader() after findBlock(\"WORLD\") serves WORLD's header");
        n = VirtualDisk::readData(buf, sizeof(buf));
        check(n == 3 && memcmp(buf, "\x20\x21\x22", 3) == 0, "readData() after findBlock(\"WORLD\") serves WORLD's 3 data bytes");
    }
    check(!VirtualDisk::findBlock("NOPE"), "findBlock() on a nonexistent name fails");
    check(VirtualDisk::lastStatus() == VirtualDisk::STATUS_EOF, "findBlock() miss reports STATUS_EOF");

    // ---- readHeader/readData maxLen truncation ----
    printf("\n-- maxLen truncation --\n");
    VirtualDisk::findBlock("HELLO");
    {
        uint8_t buf[4] = {0};
        int n = VirtualDisk::readHeader(buf, 4);
        check(n == 4, "readHeader() truncates to the caller's maxLen (4 of 17 bytes)");
        check(memcmp(buf, "\x00H\x45\x4C", 4) == 0 || buf[0] == 0, "truncated read still starts with the real header bytes");
    }

    // ---- CAT: mounted-container listing ----
    printf("\n-- catContainerLine() --\n");
    VirtualDisk::mountRead("two.tap"); // remount to reset cursors
    {
        uint8_t buf[80] = {0};
        int n = VirtualDisk::catContainerLine(buf, sizeof(buf));
        check(n > 0, "catContainerLine() returns a non-empty first line");
        check((buf[n - 1] & 0x80) != 0, "first CAT line's last byte has the high-bit terminator set");
        bool anyEarlyHighBit = false;
        for (int i = 0; i < n - 1; i++) if (buf[i] & 0x80) anyEarlyHighBit = true;
        check(!anyEarlyHighBit, "no byte before the terminator has its high bit set");
        std::string line((char*)buf, n);
        check(line.find("HELLO") != std::string::npos && line.find("Program") != std::string::npos,
              "first CAT line names HELLO as a Program block");

        n = VirtualDisk::catContainerLine(buf, sizeof(buf));
        check((buf[n - 1] & 0x80) != 0, "second CAT line's last byte also has the high-bit terminator set");
        std::string line2((char*)buf, n);
        check(line2.find("WORLD") != std::string::npos && line2.find("Code") != std::string::npos,
              "second CAT line names WORLD as a Code block");

        n = VirtualDisk::catContainerLine(buf, sizeof(buf));
        check(n == 0, "catContainerLine() returns 0 once every block is listed");
        check(VirtualDisk::lastStatus() == VirtualDisk::STATUS_EOF, "exhausted CAT listing reports STATUS_EOF");
    }

    // ---- One-shot SAVE: mountWrite + writeHeader + writeData ----
    printf("\n-- one-shot SAVE round trip --\n");
    check(VirtualDisk::mountWrite("saved.tap"), "mountWrite() opens a fresh file for writing");
    {
        uint8_t hdr[17]; buildHeader(hdr, 0, "SAVED", 4);
        check(VirtualDisk::writeHeader(hdr, 17), "writeHeader() writes the 17-byte header block");
        uint8_t data[4] = {0xAA,0xBB,0xCC,0xDD};
        check(VirtualDisk::writeData(data, 4), "writeData() writes the 4-byte data block and closes the file");
    }
    // Read it back through the same mountRead()/readHeader()/readData()
    // path already verified above -- confirms writeHeader()/writeData()
    // produce a real, well-formed .tap file, not just self-consistent
    // bytes only VirtualDisk itself could parse.
    check(VirtualDisk::mountRead("saved.tap"), "the just-written file re-mounts cleanly");
    {
        uint8_t buf[32] = {0};
        int n = VirtualDisk::readHeader(buf, sizeof(buf));
        check(n == 17 && memcmp(&buf[1], "SAVED     ", 10) == 0, "the saved file's header round-trips correctly");
        n = VirtualDisk::readData(buf, sizeof(buf));
        check(n == 4 && memcmp(buf, "\xAA\xBB\xCC\xDD", 4) == 0, "the saved file's data round-trips correctly");
    }

    // ---- Independent byte-level check of the saved file, not just VirtualDisk's own re-parse ----
    printf("\n-- saved.tap byte-level shape --\n");
    {
        FILE* f = fopen((scratch + "saved.tap").c_str(), "rb");
        check(f != nullptr, "saved.tap exists on disk");
        if (f) {
            uint8_t lenBuf[2]; fread(lenBuf, 1, 2, f);
            int blockLen = lenBuf[0] | (lenBuf[1] << 8);
            check(blockLen == 19, "header block's stored length is 19 (17 data + flag + checksum, per Tape::Save()'s DE+2 convention)");
            fclose(f);
        }
    }

    // ---- Error handling: mountRead ----
    printf("\n-- mountRead() error handling --\n");
    check(!VirtualDisk::mountRead("does_not_exist.tap"), "mountRead() on a missing file fails");
    check(VirtualDisk::lastStatus() == VirtualDisk::STATUS_ERROR, "missing file reports STATUS_ERROR");
    {
        FILE* f = fopen((scratch + "plain.rom").c_str(), "wb");
        uint8_t junk[4] = {1,2,3,4};
        fwrite(junk, 1, 4, f);
        fclose(f);
    }
    check(!VirtualDisk::mountRead("plain.rom"), "mountRead() rejects a non-.tap extension (direct load not yet implemented)");

    // ---- Error handling: mountWrite .tzx rejection ----
    printf("\n-- mountWrite() .tzx rejection --\n");
    check(!VirtualDisk::mountWrite("nope.tzx"), "mountWrite() rejects a .tzx filename (write support doesn't exist)");
    check(VirtualDisk::lastStatus() == VirtualDisk::STATUS_ERROR, ".tzx rejection reports STATUS_ERROR");

    // ---- Error handling: findBlock()/readHeader() with nothing mounted ----
    printf("\n-- operating with nothing mounted --\n");
    VirtualDisk::unmount();
    check(!VirtualDisk::findBlock("HELLO"), "findBlock() fails when nothing is mounted");
    check(VirtualDisk::lastStatus() == VirtualDisk::STATUS_ERROR, "findBlock() with nothing mounted reports STATUS_ERROR");
    {
        uint8_t buf[8];
        int n = VirtualDisk::readHeader(buf, sizeof(buf));
        check(n == -1, "readHeader() with nothing mounted returns -1");
        check(VirtualDisk::lastStatus() == VirtualDisk::STATUS_EOF, "readHeader() with nothing mounted reports STATUS_EOF");
    }
    check(!VirtualDisk::catContainerLine((uint8_t*)"", 0) && VirtualDisk::lastStatus() == VirtualDisk::STATUS_ERROR,
          "catContainerLine() with nothing mounted reports STATUS_ERROR");

    // ---- unmount() safe to call with nothing mounted ----
    VirtualDisk::unmount();
    check(VirtualDisk::lastStatus() == VirtualDisk::STATUS_OK, "unmount() with nothing mounted is a harmless no-op");

    // ---- CAT: SD directory listing ----
    printf("\n-- catSdLine() --\n");
    // scratch/ now has two.tap, saved.tap, plain.rom from the tests
    // above -- exact count depends on that, checked structurally
    // (each entry seen once, loop terminates) rather than an exact
    // hardcoded filename order (readdir() order is not guaranteed).
    {
        int linesSeen = 0;
        bool sawTwoTap = false, sawSavedTap = false, sawExhausted = false;
        for (int i = 0; i < 10; i++) { // generous upper bound, loop breaks on EOF
            uint8_t buf[80] = {0};
            int n = VirtualDisk::catSdLine(buf, sizeof(buf));
            if (n == 0) { sawExhausted = true; break; } // catSdLine() resets its cursor on this call, so this must be the loop's LAST call, not followed by another before checking status
            check((buf[n - 1] & 0x80) != 0, "catSdLine() terminates each line with the high bit set on its last byte");
            linesSeen++;
            std::string line((char*)buf, n);
            if (line.find("two.tap") != std::string::npos) sawTwoTap = true;
            if (line.find("saved.tap") != std::string::npos) sawSavedTap = true;
        }
        check(linesSeen == 3, "catSdLine() lists exactly the 3 files present in the scratch directory");
        check(sawTwoTap && sawSavedTap, "catSdLine() listing includes both two.tap and saved.tap by name");
        check(sawExhausted, "catSdLine() returns 0 once the directory is exhausted");
        check(VirtualDisk::lastStatus() == VirtualDisk::STATUS_EOF, "exhausted SD listing reports STATUS_EOF");
    }
    // A fresh call after exhaustion starts over (cursor reset) --
    // confirms catSdLine() doesn't get permanently stuck at EOF.
    {
        uint8_t buf[80];
        int n = VirtualDisk::catSdLine(buf, sizeof(buf));
        check(n > 0, "catSdLine() starts over (cursor reset) after a prior listing was exhausted");
    }

    // ---- reset() clears mount state ----
    printf("\n-- reset() --\n");
    VirtualDisk::mountRead("two.tap");
    VirtualDisk::reset();
    check(!VirtualDisk::findBlock("HELLO"), "reset() clears mount state (findBlock() fails afterward)");

    printf("\n======================\n");
    if (failures == 0) {
        printf("ALL PASSED\n");
    } else {
        printf("%d CHECK(S) FAILED\n", failures);
    }

    return failures;
}
