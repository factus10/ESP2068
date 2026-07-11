/*

ESP2068 — TS2068 port of ESPectrum

VirtualDisk.cpp — see include/VirtualDisk.h.

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

#include "VirtualDisk.h"
#include <cstring>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>

const char* VirtualDisk::DEFAULT_MOUNT_DIR = "/sd/2068/disk/";
std::string VirtualDisk::mountDir = VirtualDisk::DEFAULT_MOUNT_DIR;

FILE* VirtualDisk::file = nullptr;
bool VirtualDisk::isContainer = false;
bool VirtualDisk::writeMode = false;
std::vector<VirtualDisk::BlockInfo> VirtualDisk::blocks;
int VirtualDisk::currentBlock = -1;
int VirtualDisk::catSdIndex = 0;
int VirtualDisk::catContainerIndex = 0;
uint8_t VirtualDisk::status_ = VirtualDisk::STATUS_OK;

void VirtualDisk::reset() {
    unmount();
}

bool VirtualDisk::hasExtension(const std::string& filename, const std::string& ext) {
    if (filename.length() < ext.length()) return false;
    std::string tail = filename.substr(filename.length() - ext.length());
    for (auto& c : tail) c = (char) tolower((unsigned char) c);
    std::string wanted = ext;
    for (auto& c : wanted) c = (char) tolower((unsigned char) c);
    return tail == wanted;
}

void VirtualDisk::unmount() {
    if (file) { fclose(file); file = nullptr; }
    blocks.clear();
    currentBlock = -1;
    isContainer = false;
    writeMode = false;
    catSdIndex = 0;
    catContainerIndex = 0;
    status_ = STATUS_OK;
}

bool VirtualDisk::scanBlocks() {
    blocks.clear();
    fseek(file, 0, SEEK_SET);
    while (true) {
        long offset = ftell(file);
        uint8_t lenBuf[2];
        if (fread(lenBuf, 1, 2, file) != 2) break; // clean EOF between blocks
        int blockLen = lenBuf[0] | (lenBuf[1] << 8);
        if (blockLen < 2) return false; // malformed: must hold at least flag+checksum

        uint8_t flag;
        if (fread(&flag, 1, 1, file) != 1) return false;
        int dataLen = blockLen - 2;

        BlockInfo b;
        b.offset = offset;
        b.length = dataLen;
        b.flag = flag;
        b.type = -1;
        b.declaredLen = 0;

        long dataStart = ftell(file);
        if (flag == 0x00 && dataLen >= 17) {
            uint8_t hdr[17];
            if (fread(hdr, 1, 17, file) != 17) return false;
            b.type = hdr[0];
            std::string name((char*)&hdr[1], 10);
            while (!name.empty() && name.back() == ' ') name.pop_back();
            b.name = name;
            b.declaredLen = hdr[11] | (hdr[12] << 8);
        }
        // Skip to the end of this block's data (whether or not the
        // header branch above already consumed the first 17 bytes),
        // then the trailing checksum byte.
        fseek(file, dataStart + dataLen, SEEK_SET);
        fseek(file, 1, SEEK_CUR); // checksum

        blocks.push_back(b);
    }
    return true;
}

bool VirtualDisk::mountRead(const std::string& filename) {
    unmount();
    std::string path = mountDir + filename;
    file = fopen(path.c_str(), "rb");
    if (!file) { status_ = STATUS_ERROR; return false; }

    if (!hasExtension(filename, ".tap")) {
        // Non-.tap "direct load" and .tzx containers are both explicitly
        // out of scope for this increment -- see VirtualDisk.h's
        // file-level comment and PLAN.md's Phase 4 Piece A writeup.
        // Fail cleanly rather than guess at an unconfirmed shape.
        fclose(file); file = nullptr;
        status_ = STATUS_ERROR;
        return false;
    }

    isContainer = true;
    writeMode = false;
    if (!scanBlocks()) {
        fclose(file); file = nullptr;
        isContainer = false;
        status_ = STATUS_ERROR;
        return false;
    }
    currentBlock = 0;
    status_ = STATUS_OK;
    return true;
}

bool VirtualDisk::mountWrite(const std::string& filename) {
    unmount();
    if (hasExtension(filename, ".tzx")) {
        // .tzx write support doesn't exist anywhere in this codebase
        // (Tape_TZX.cpp is read-only) -- error cleanly rather than
        // silently doing something else, per the design doc's decision.
        status_ = STATUS_ERROR;
        return false;
    }
    std::string path = mountDir + filename;
    file = fopen(path.c_str(), "wb"); // fresh: create or truncate
    if (!file) { status_ = STATUS_ERROR; return false; }
    writeMode = true;
    isContainer = false;
    status_ = STATUS_OK;
    return true;
}

bool VirtualDisk::findBlock(const std::string& name) {
    if (!isContainer) { status_ = STATUS_ERROR; return false; }
    for (size_t i = 0; i < blocks.size(); i++) {
        if (blocks[i].flag == 0x00 && blocks[i].name == name) {
            currentBlock = (int) i;
            status_ = STATUS_OK;
            return true;
        }
    }
    status_ = STATUS_EOF;
    return false;
}

int VirtualDisk::readCurrentBlock(uint8_t* dest, int maxLen) {
    if (!file || !isContainer || currentBlock < 0 || currentBlock >= (int) blocks.size()) {
        status_ = STATUS_EOF;
        return -1;
    }
    BlockInfo& b = blocks[currentBlock];
    fseek(file, b.offset + 2 /* length prefix */ + 1 /* flag */, SEEK_SET);
    int n = b.length < maxLen ? b.length : maxLen;
    size_t got = (n > 0) ? fread(dest, 1, n, file) : 0;
    currentBlock++;
    status_ = (got == (size_t) n) ? STATUS_OK : STATUS_ERROR;
    return (int) got;
}

int VirtualDisk::readHeader(uint8_t* dest, int maxLen) { return readCurrentBlock(dest, maxLen); }
int VirtualDisk::readData(uint8_t* dest, int maxLen)   { return readCurrentBlock(dest, maxLen); }

bool VirtualDisk::writeBlock(const uint8_t* src, int len, uint8_t flag) {
    if (!file || !writeMode || len < 0) { status_ = STATUS_ERROR; return false; }
    uint16_t blockLen = (uint16_t)(len + 2);
    uint8_t lenLo = (uint8_t)(blockLen & 0xFF);
    uint8_t lenHi = (uint8_t)((blockLen >> 8) & 0xFF);
    fwrite(&lenLo, 1, 1, file);
    fwrite(&lenHi, 1, 1, file);
    fwrite(&flag, 1, 1, file);
    uint8_t xorSum = flag;
    for (int i = 0; i < len; i++) {
        fwrite(&src[i], 1, 1, file);
        xorSum ^= src[i];
    }
    fwrite(&xorSum, 1, 1, file);
    status_ = STATUS_OK;
    return true;
}

bool VirtualDisk::writeHeader(const uint8_t* src, int len) {
    return writeBlock(src, len, 0x00);
}

bool VirtualDisk::writeData(const uint8_t* src, int len) {
    bool ok = writeBlock(src, len, 0xFF);
    // One-shot semantics: the data block completes the header+data
    // pair, so the file closes here regardless of success/failure --
    // matching the design doc's "open, write one block, close" contract.
    if (file) { fclose(file); file = nullptr; }
    writeMode = false;
    return ok;
}

int VirtualDisk::catSdLine(uint8_t* dest, int maxLen) {
    DIR* d = opendir(mountDir.c_str());
    if (!d) { status_ = STATUS_ERROR; return 0; }

    struct dirent* entry = nullptr;
    int idx = 0;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (idx == catSdIndex) break;
        idx++;
    }

    if (!entry) {
        closedir(d);
        catSdIndex = 0;
        status_ = STATUS_EOF;
        return 0;
    }

    std::string path = mountDir + entry->d_name;
    struct stat st;
    long size = (stat(path.c_str(), &st) == 0) ? (long) st.st_size : 0;
    std::string name = entry->d_name;
    closedir(d);

    char line[80];
    int n = snprintf(line, sizeof(line), "%-20s %6ld\n", name.c_str(), size);
    if (n < 0) n = 0;
    if (n > maxLen) n = maxLen;
    if (n > 0) memcpy(dest, line, n);
    catSdIndex++;
    status_ = STATUS_OK;
    return n;
}

int VirtualDisk::catContainerLine(uint8_t* dest, int maxLen) {
    if (!isContainer) { status_ = STATUS_ERROR; return 0; }

    while (catContainerIndex < (int) blocks.size() && blocks[catContainerIndex].flag != 0x00)
        catContainerIndex++;

    if (catContainerIndex >= (int) blocks.size()) {
        catContainerIndex = 0;
        status_ = STATUS_EOF;
        return 0;
    }

    static const char* typeNames[] = { "Program", "Number array", "Character array", "Code" };
    BlockInfo& b = blocks[catContainerIndex];
    const char* tn = (b.type >= 0 && b.type <= 3) ? typeNames[b.type] : "Unknown";

    char line[80];
    int n = snprintf(line, sizeof(line), "%-10s %-16s %5d\n", b.name.c_str(), tn, b.declaredLen);
    if (n < 0) n = 0;
    if (n > maxLen) n = maxLen;
    if (n > 0) memcpy(dest, line, n);
    catContainerIndex++;
    status_ = STATUS_OK;
    return n;
}

uint8_t VirtualDisk::lastStatus() {
    return status_;
}
