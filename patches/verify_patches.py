#!/usr/bin/env python3
"""
ESP2068 -- TS2068 port of ESPectrum

patches/verify_patches.py -- hex-level verification for the Phase 4
virtual disk ROM patches (Pieces B/C). This project has no host-testable
way to run real Z80 machine code against patched ROM content (CPU.cpp's
Z80 core is ESP-IDF-entangled, unlike SCLD/DockLoader/RomLoader) -- this
script is the interim check PLAN.md's Phase 4 Piece D writeup already
anticipated: confirm the patched ROM bytes at each of the five confirmed
offsets match the intended new opcodes, and that every byte outside the
patch points and the two confirmed-dead code pockets is byte-identical
to the real, genuine Timex ROM. It does NOT confirm the patches behave
correctly when actually executed by a Z80 -- that still needs either a
host-side Z80 core (which doesn't exist in this project yet) or real
hardware.

Run after assembling both patches (see patches/README.md):

    python3 patches/verify_patches.py

Exits 0 and prints "ALL PASSED" if every check passes; otherwise prints
each failure and exits with the failure count.
"""

import os

HERE = os.path.dirname(os.path.abspath(__file__))
BASE_EXROM = os.path.join(HERE, "base", "2068Exrom.BIN")
BASE_HOME = os.path.join(HERE, "base", "2068Home.BIN")
OUT_EXROM = os.path.join(HERE, "out", "2068Exrom.BIN")
OUT_HOME = os.path.join(HERE, "out", "2068Home.BIN")

failures = 0


def check(cond, what):
    global failures
    if cond:
        print(f"  PASS  {what}")
    else:
        print(f"  FAIL  {what}")
        failures += 1


def read(path):
    with open(path, "rb") as f:
        return f.read()


def main():
    print("ROM patch verification")
    print("=======================")

    for path in (BASE_EXROM, BASE_HOME, OUT_EXROM, OUT_HOME):
        check(os.path.isfile(path), f"{os.path.relpath(path, HERE)} exists")

    real_exrom = read(BASE_EXROM)
    patched_exrom = read(OUT_EXROM)
    real_home = read(BASE_HOME)
    patched_home = read(OUT_HOME)

    print("\n-- file sizes --")
    check(len(real_exrom) == 8192, "base EXROM is exactly 8192 bytes")
    check(len(patched_exrom) == 8192, "patched EXROM is exactly 8192 bytes (no size extension)")
    check(len(real_home) == 16384, "base HOME ROM is exactly 16384 bytes")
    check(len(patched_home) == 16384, "patched HOME ROM is exactly 16384 bytes")

    print("\n-- EXROM patch points (3-byte redirects) --")
    # W_TAPE ($0068): JP W_TAPE_intercept ($1624)
    check(patched_exrom[0x0068:0x006B] == bytes([0xC3, 0x24, 0x16]),
          "W_TAPE ($0068) redirects to JP $1624 (W_TAPE_intercept)")
    # R_TAPE ($00FC): JP R_TAPE_intercept ($163D)
    check(patched_exrom[0x00FC:0x00FF] == bytes([0xC3, 0x3D, 0x16]),
          "R_TAPE ($00FC) redirects to JP $163D (R_TAPE_intercept)")
    # PGPSTR call site ($0215): CALL PGPSTR_intercept ($165C)
    check(patched_exrom[0x0215:0x0218] == bytes([0xCD, 0x5C, 0x16]),
          "PGPSTR call site ($0215) redirects to CALL $165C (PGPSTR_intercept)")

    print("\n-- EXROM: everything outside the three patch points and the new-code region is untouched --")
    for start, end, label in [
        (0x0000, 0x0068, "before W_TAPE patch"),
        (0x006B, 0x00FC, "between W_TAPE and R_TAPE patches"),
        (0x00FF, 0x0215, "between R_TAPE and PGPSTR patches"),
        (0x0218, 0x1624, "between PGPSTR patch and new-code region"),
        (0x16C3, 0x2000, "after the new-code region to end of file"),
    ]:
        check(real_exrom[start:end] == patched_exrom[start:end], f"EXROM {label} ({start:#06x}-{end - 1:#06x}) byte-identical to real ROM")

    def at(data, addr, hexstr, what):
        want = bytes.fromhex(hexstr)
        check(data[addr:addr + len(want)] == want, what)

    print("\n-- EXROM: new code decodes as intended (spot checks, not a full disassembly) --")
    at(patched_exrom, 0x1624, "f53add5db728",
       "W_TAPE_intercept opens PUSH AF / LD A,($5DDD) / OR A / JR Z,+n")
    at(patched_exrom, 0x165C, "cd990ff578b1",
       "PGPSTR_intercept opens CALL $0F99 / PUSH AF / LD A,B / OR C")
    at(patched_exrom, 0x1691, "0e0a06c8dd21de5d",
       "CAT_listing opens LD C,$0A (CMD_CAT_CONTAINER_LINE) / LD B,$C8 (CAT_MAX_LINES safety bound) / LD IX,$5DDE (CAT_LINE_BUF)")

    print("\n-- HOME ROM patch points --")
    # CLOSE #4 ($139F): CALL CLOSE4_intercept ($3CDC)
    check(patched_home[0x139F:0x13A2] == bytes([0xCD, 0xDC, 0x3C]),
          "CLOSE #4 ($139F) redirects to CALL $3CDC (CLOSE4_intercept)")
    # CAT ($25C8): CALL CAT_trampoline ($3CED) / RET
    check(patched_home[0x25C8:0x25CC] == bytes([0xCD, 0xED, 0x3C, 0xC9]),
          "CAT ($25C8) redirects to CALL $3CED (CAT_trampoline) / RET")

    print("\n-- HOME ROM: everything outside the two patch points and the new-code pocket is untouched --")
    for start, end, label in [
        (0x0000, 0x139F, "before CLOSE #4 patch"),
        (0x13A2, 0x25C8, "between CLOSE #4 and CAT patches"),
        (0x25CC, 0x3CDC, "between CAT patch and the new-code pocket (includes untouched FORMAT/MOVE/ERASE)"),
        (0x3D00, 0x4000, "CHRSET and everything after -- must be byte-identical"),
    ]:
        check(real_home[start:end] == patched_home[start:end], f"HOME ROM {label} ({start:#06x}-{end - 1:#06x}) byte-identical to real ROM")

    print("\n-- HOME ROM: new code decodes as intended --")
    at(patched_home, 0x3CDC, "cd0f143acb5cfe04c0",
       "CLOSE4_intercept: CALL $140F / LD A,($5CCB) / CP 4 / RET NZ")
    at(patched_home, 0x3CED, "01fefecd9964cd9116",
       "CAT_trampoline opens LD BC,$FEFE / CALL $6499 / CALL $1691 (CAT_listing)")

    print("\n=======================")
    if failures == 0:
        print("ALL PASSED")
    else:
        print(f"{failures} CHECK(S) FAILED")
    return failures


if __name__ == "__main__":
    raise SystemExit(main())
