/*

ESP2068 — TS2068 port of ESPectrum

RomLoader.cpp — see include/RomLoader.h.

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

#include "RomLoader.h"
#include "SCLD.h"
#include <cstdio>
#include <cstring>

#ifdef ESP2068_HOST_TEST
#include <cstdlib>
static inline uint8_t* romloader_alloc8k() { return (uint8_t*) calloc(0x2000, 1); }
#else
#include "esp_heap_caps.h"
static inline uint8_t* romloader_alloc8k() { return (uint8_t*) heap_caps_calloc(0x2000, 1, MALLOC_CAP_8BIT); }
#endif

const char* RomLoader::DEFAULT_HOME_ROM_PATH = "/sd/2068/home.rom";
const char* RomLoader::DEFAULT_EXROM_PATH = "/sd/2068/exrom.rom";

namespace {

    // Reads fn fully into a newly malloc'd buffer. *outSize is the file
    // size read. Returns nullptr on any failure; caller must free() a
    // non-null result.
    uint8_t* readWholeFile(const std::string& fn, size_t* outSize) {

        FILE* file = fopen(fn.c_str(), "rb");
        if (file == NULL) {
            printf("RomLoader: error opening %s\n", fn.c_str());
            return nullptr;
        }

        fseek(file, 0, SEEK_END);
        long size = ftell(file);
        rewind(file);

        if (size <= 0) {
            printf("RomLoader: %s is empty or unreadable\n", fn.c_str());
            fclose(file);
            return nullptr;
        }

        uint8_t* buf = (uint8_t*) malloc((size_t) size);
        if (!buf) {
            printf("RomLoader: out of memory reading %s (%ld bytes)\n", fn.c_str(), size);
            fclose(file);
            return nullptr;
        }

        size_t got = fread(buf, 1, (size_t) size, file);
        fclose(file);

        if (got != (size_t) size) {
            printf("RomLoader: short read on %s (%zu of %ld bytes)\n", fn.c_str(), got, size);
            free(buf);
            return nullptr;
        }

        *outSize = got;
        return buf;
    }

}

bool RomLoader::loadHomeRomFromBuffer(const uint8_t* data, size_t size) {
    if (size != 0x4000) {
        printf("RomLoader: HOME ROM must be exactly 16384 bytes, got %zu\n", size);
        return false;
    }
    return SCLD::loadHomeRom(data, size);
}

bool RomLoader::loadExromFromBuffer(const uint8_t* data, size_t size) {
    if (size != 0x2000) {
        printf("RomLoader: EXROM must be exactly 8192 bytes, got %zu\n", size);
        return false;
    }
    uint8_t* buf = romloader_alloc8k();
    if (!buf) {
        printf("RomLoader: out of memory allocating EXROM buffer\n");
        return false;
    }
    memcpy(buf, data, 0x2000);
    SCLD::loadExromImage(buf);
    return true;
}

bool RomLoader::loadHomeRomFromFile(const std::string& fn) {
    size_t size = 0;
    uint8_t* buf = readWholeFile(fn, &size);
    if (!buf) return false;
    bool ok = loadHomeRomFromBuffer(buf, size);
    free(buf); // loadHomeRomFromBuffer() copies; this was scratch
    if (!ok) printf("RomLoader: %s failed to load as a HOME ROM\n", fn.c_str());
    return ok;
}

bool RomLoader::loadExromFromFile(const std::string& fn) {
    size_t size = 0;
    uint8_t* buf = readWholeFile(fn, &size);
    if (!buf) return false;
    bool ok = loadExromFromBuffer(buf, size);
    free(buf); // loadExromFromBuffer() makes its own owned copy; this was scratch
    if (!ok) printf("RomLoader: %s failed to load as an EXROM\n", fn.c_str());
    return ok;
}
