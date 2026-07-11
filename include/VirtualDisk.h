/*

ESP2068 — TS2068 port of ESPectrum

VirtualDisk.h — Phase 4, Piece A: the ESP32-side half of the virtual
disk drive feature (see TS2068-VIRTUAL-DISK-PLAN.md and PLAN.md's
"Phase 4" section for the full design). New for this port, no upstream
ESPectrum equivalent.

This class is the pure, host-testable core: every method takes a plain
filename or a caller-supplied `uint8_t*`/length buffer, never a Z80
register or a Z80 memory address. It has no dependency on Z80.h,
Z80operations.h, or VIDEO — same convention as SCLDVideo.h/SCLDPrinter.h.
The thin adapter that translates real Z80 registers/memory (IX, DE, BC,
Z80Ops::peek8_2068/poke8_2068) into calls on this class lives in
src/Ports.cpp's is2068 case 0x000E (command, write)/0x000F (status,
read) — matching this project's existing convention that Ports.cpp
itself isn't part of the ESP2068_HOST_TEST set, so the register-touching
glue has nowhere better to live.

Wire protocol (ports $0E/$0F, full low-byte decode like every other
TS2068 port added by this port): $0E is write-only, one command byte
selects the operation; $0F is read-only, one status byte reports the
result of the last command. The Command enum values below are this
project's own choice, not derived from any real hardware protocol (see
the design doc's "Wire protocol" section) — matching TS-Pico's port
*numbers* only, not its framing.

Scope of this first increment, and what's deliberately deferred: only
.tap-shaped containers are supported (mount, per-block read/write, a
name-based lookup this project's block engine didn't previously have,
and a two-mode CAT). .tzx containers, and LOAD "D:file.ext" for
extensions other than .tap (the design doc's "loads directly, no
container semantics" case), are NOT implemented yet — mountRead()
fails cleanly for both rather than guessing at an unconfirmed shape.
See PLAN.md's Phase 4 Piece A writeup for the reasoning.

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

#ifndef VirtualDisk_h
#define VirtualDisk_h

#include <inttypes.h>
#include <cstdio>
#include <string>
#include <vector>

class VirtualDisk {
public:

    // Port $0E command bytes (write-only from the Z80 side). Piece B's
    // EXROM/HOME ROM patches (not yet written) are the ones that decide
    // when to issue which -- this class only needs to know the byte
    // values so Ports.cpp's adapter can dispatch on them.
    enum Command : uint8_t {
        CMD_MOUNT_READ         = 0x01, // DE=name ptr, BC=name length -- open for LOAD/VERIFY/MERGE
        CMD_MOUNT_WRITE        = 0x02, // DE=name ptr, BC=name length -- open for one-shot SAVE
        CMD_UNMOUNT             = 0x03,
        CMD_FIND_BLOCK          = 0x04, // DE=name ptr, BC=name length -- position cursor to the named block
        CMD_READ_HEADER         = 0x05, // IX=dest addr, DE=max length
        CMD_READ_DATA           = 0x06, // IX=dest addr, DE=max length
        CMD_WRITE_HEADER        = 0x07, // IX=src addr,  DE=length
        CMD_WRITE_DATA          = 0x08, // IX=src addr,  DE=length
        CMD_CAT_SD_LINE         = 0x09, // IX=dest addr, DE=max length
        CMD_CAT_CONTAINER_LINE  = 0x0A, // IX=dest addr, DE=max length
    };

    // Port $0F status byte (read-only from the Z80 side).
    enum Status : uint8_t {
        STATUS_OK    = 0x00,
        STATUS_ERROR = 0x01, // file not found, wrong mode, malformed container, etc.
        STATUS_EOF   = 0x02, // no more blocks/lines
    };

    // Default SD-card directory virtual disk files live in and CAT
    // lists. Same "bootstrapping convention, not a final OSD-integrated
    // interface" caveat as RomLoader::DEFAULT_HOME_ROM_PATH -- Phase 3
    // is where this gets replaced by real path handling from the OSD.
    static const char* DEFAULT_MOUNT_DIR;

    // The directory actually in effect (initialized to
    // DEFAULT_MOUNT_DIR). Mutable, same shape as the real codebase's
    // FileUtils::MountPoint, so host tests can point it at a scratch
    // directory instead of the real (nonexistent, off-device) /sd path.
    static std::string mountDir;

    // Clears all mount state. Called from CPU::reset()'s "2068" branch,
    // alongside SCLD::reset()/SCLDPrinter::reset().
    static void reset();

    // ---- Mount/unmount ----

    // Opens filename (relative to MOUNT_DIR) for reading. If it has a
    // .tap extension, scans it as a multi-block container (see
    // scanBlocks()) and readHeader()/readData()/findBlock() become
    // usable. Any other extension currently fails (see the file-level
    // comment) -- returns false, status() reports STATUS_ERROR.
    static bool mountRead(const std::string& filename);

    // Opens filename (relative to MOUNT_DIR) fresh (create/truncate)
    // for a one-shot SAVE: exactly one writeHeader() + one writeData()
    // call, the second of which closes the file. .tzx extensions are
    // rejected cleanly (write support doesn't exist -- see the design
    // doc's "Reused vs new").
    static bool mountWrite(const std::string& filename);

    // Closes whatever's open and clears state. Safe to call when
    // nothing is mounted (a no-op, matching stock CLOSE #'s existing
    // "unused stream" no-op behavior the real CLOSE #4 patch will rely
    // on).
    static void unmount();

    // ---- Container navigation (mountRead() with a .tap container only) ----

    // Searches the mounted container for a header block (flag 0x00)
    // whose name matches exactly (trailing spaces already trimmed on
    // both sides -- callers should trim before calling). On a match,
    // positions the read cursor at that header block so the next
    // readHeader()/readData() pair serves it. Returns false (status()
    // STATUS_EOF) if not found, STATUS_ERROR if no container is
    // mounted.
    static bool findBlock(const std::string& name);

    // ---- Block I/O ----

    // Reads the block at the current cursor into dest (up to maxLen
    // bytes) and advances the cursor by one block. Both just read
    // "the current block" -- real .tap files already store a header
    // block immediately followed by its data block, so two calls in a
    // row naturally serve the header then the data, matching the real
    // W_TAPE/R_TAPE two-phase (flag 0x00, then 0xff) calling
    // convention. Returns the number of bytes written (may be less
    // than the block's real length if maxLen is smaller), or -1 on
    // EOF/error (check status()).
    static int readHeader(uint8_t* dest, int maxLen);
    static int readData(uint8_t* dest, int maxLen);

    // Writes one new .tap-format block (2-byte little-endian length,
    // flag byte, data, XOR checksum) to the mounted write file, using
    // the same on-disk shape Tape::Save() already writes for the
    // Spectrum machines -- this is a fresh implementation of that same
    // well-understood algorithm (not a call into Tape::Save() itself,
    // which would touch Tape's own separate global mount state), with
    // Tape::Save()'s MemESP::readbyte() call site replaced by this
    // class taking the source bytes as a plain parameter instead --
    // Ports.cpp's adapter is what actually resolves them via
    // Z80Ops::peek8_2068(). writeData() closes the file afterward,
    // completing the one-shot "open, write one block pair, close"
    // contract.
    static bool writeHeader(const uint8_t* src, int len);
    static bool writeData(const uint8_t* src, int len);

    // ---- CAT ----

    // Formats one line of the SD-directory listing (filename + size)
    // into dest (up to maxLen bytes) and advances an internal line
    // cursor. The Ports.cpp adapter only sees the return value inside
    // this same C++ process, but the Z80 side reading dest through
    // emulated memory has no way to learn a byte count out-of-band --
    // so the last byte actually written has its high bit set as a
    // terminator, matching the real TS2068 ROM's own message-table
    // convention (confirmed against the HOME ROM disassembly's SEPRMT
    // table), rather than relying on a trailing NUL or a separate
    // length channel. Returns 0 (status() STATUS_EOF) once every entry
    // has been listed, at which point the cursor resets so a fresh CAT
    // starts over. Does not reuse FileUtils's cached directory index
    // (that machinery isn't host-testable and is tied to the OSD
    // file-picker's own index-rebuild lifecycle) -- plain
    // opendir()/readdir()/stat() instead, portable and simple. Fine
    // for the modest directory sizes a virtual disk's own subdirectory
    // realistically holds; revisit if this becomes a bottleneck.
    static int catSdLine(uint8_t* dest, int maxLen);

    // Formats one line of the mounted container's block listing (name
    // + type + declared length) into dest, same calling convention as
    // catSdLine(). STATUS_ERROR if no container is mounted.
    static int catContainerLine(uint8_t* dest, int maxLen);

    // ---- Status ----

    static uint8_t lastStatus();

private:

    struct BlockInfo {
        long offset;        // file offset of this block's 2-byte length prefix
        int length;          // this block's own data length (what readHeader/readData serves)
        uint8_t flag;         // 0x00 = header block, 0xff = data block (real .tap convention)
        std::string name;     // populated only for header blocks
        int type;             // populated only for header blocks (0=Program,1=Number array,2=Character array,3=Code), else -1
        int declaredLen;      // header's own embedded "following data block length" field, for CAT display only
    };

    static bool scanBlocks();   // walks the open (read) file, builds `blocks`
    static bool hasExtension(const std::string& filename, const std::string& ext);
    static int readCurrentBlock(uint8_t* dest, int maxLen);
    static bool writeBlock(const uint8_t* src, int len, uint8_t flag);

    static FILE* file;
    static bool isContainer;
    static bool writeMode;
    static std::vector<BlockInfo> blocks;
    static int currentBlock;
    static int catSdIndex;
    static int catContainerIndex;
    static uint8_t status_;

};

#endif // VirtualDisk_h
