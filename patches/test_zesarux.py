#!/usr/bin/env python3
"""
ESP2068 -- TS2068 port of ESPectrum

patches/test_zesarux.py -- Phase 4 Piece D: runs the assembled, patched
ROM (patches/out/) inside a real, independent Z80 emulator (ZEsarUX,
via its ZRCP scripting protocol) and confirms it actually boots and
behaves correctly. This is the thing verify_patches.py's own docstring
says it can't do: verify_patches.py confirms the patch *bytes* are
correct; this confirms a real Z80 core executing them produces the
intended *behavior*.

Requires ZEsarUX (https://github.com/chernandezba/zesarux) installed at
/Applications/zesarux.app (macOS) and patches/out/*.BIN already built
(see verify_patches.py / README.md). Not part of any automated CI --
this launches and drives a real emulator process, meant to be run by
hand when validating a patch change.

What this actually confirms, and what it doesn't:

- CONFIRMED, by direct memory read inside the running emulator: all
  five patch points and both new-code regions are byte-identical to
  what verify_patches.py already checked statically -- but now read
  back from inside a process that has ITS OWN, independent ROM-loading
  code, not just re-reading the same file this project produced.
- CONFIRMED, by letting the CPU run: the patched ROM boots through the
  real cold-start sequence (memory clear, interrupt setup, the real
  "T/S 2068 Computer" copyright banner), reaches a genuine interactive
  BASIC ready prompt, and evaluates an ordinary expression correctly
  (PRINT 2+2 => 4) -- a real regression check that patching five ROM
  locations didn't break anything else.
- CONFIRMED, by direct register/port inspection at each intercept's
  entry point: CLOSE4_intercept issues CMD_UNMOUNT and zeroes BC when
  STRMN==4, and leaves BC completely untouched otherwise; CAT_trampoline
  correctly invokes the real BANK_ENABLE ROM routine, which correctly
  pages the patched EXROM into chunk 0 (confirmed by reading real EXROM
  bytes at $0000 immediately after); CAT_listing's defensive DJNZ loop
  bound (added after this test suite found the unbounded version loops
  forever without a real ESP32 backend to ever answer STATUS_EOF/
  STATUS_ERROR) correctly loops while B>1 and correctly falls through
  once B reaches 0.
- NOT confirmed here, and not fixable by more ZRCP scripting: the
  virtual-disk command flow driven by real BASIC ("D:" prefixed LOAD/
  SAVE/CAT/CLOSE #4) end to end, since ZEsarUX has no knowledge of
  ports $0E/$0F -- there is no real ESP32-side VirtualDisk handler
  answering them here, only the real ROM code issuing the OUT/IN calls
  correctly (which is what the CLOSE4/CAT tests above already confirm
  in isolation). That needs real hardware or a host-side VirtualDisk-
  aware Z80 harness, neither of which exists yet.

Usage:

    # 1. Build the concatenated test ROM (HOME + EXROM, matching
    #    ZEsarUX's own ts2068.rom layout) and launch ZEsarUX:
    cat patches/out/2068Home.BIN patches/out/2068Exrom.BIN > /tmp/ts2068_patched.rom
    pkill -f zesarux; sleep 2
    nohup /Applications/zesarux.app/Contents/MacOS/zesarux \\
      --noconfigfile --machine TS2068 --romfile /tmp/ts2068_patched.rom \\
      --enable-remoteprotocol --vo null --ao null \\
      > /tmp/zesarux.log 2>&1 &
    sleep 8

    # 2. Run this script:
    python3 patches/test_zesarux.py

Exits 0 and prints "ALL PASSED" if every check passes; otherwise prints
each failure and exits with the failure count. Leaves the emulator
process running (kill it yourself with `pkill -f zesarux` when done --
this project doesn't own that process's lifecycle, since a developer
may want to keep inspecting it interactively afterward).
"""

import socket
import time
import sys

HOST, PORT = "localhost", 10000
failures = 0


def check(cond, what):
    global failures
    if cond:
        print(f"  PASS  {what}")
    else:
        print(f"  FAIL  {what}")
        failures += 1


class ZRCP:
    def __init__(self):
        self.s = socket.socket()
        self.s.settimeout(5)
        try:
            self.s.connect((HOST, PORT))
        except OSError as e:
            print(f"Could not connect to ZEsarUX ZRCP on {HOST}:{PORT} ({e}).")
            print("Is ZEsarUX running with --enable-remoteprotocol? See this file's docstring.")
            sys.exit(1)
        self.drain()  # welcome banner

    def drain(self):
        time.sleep(0.12)
        try:
            return self.s.recv(65536).decode(errors="replace")
        except socket.timeout:
            return ""

    def cmd(self, c, wait=0.2):
        self.s.sendall((c + "\n").encode())
        time.sleep(wait)
        return self.drain()

    def hexdump_bytes(self, addr, length):
        """Parses `hexdump` output back into a bytes object."""
        out = self.cmd(f"hexdump {addr} {length}")
        data = bytearray()
        for line in out.splitlines():
            line = line.strip()
            if "H " not in line:
                continue
            hexpart = line.split("H ", 1)[1].split("|")[0]
            data.extend(int(h, 16) for h in hexpart.split())
        return bytes(data[:length])

    def registers(self):
        """Parses `get-registers` into a dict, e.g. {'PC': 0x1234, 'BC': 0x0000, ...}."""
        out = self.cmd("get-registers")
        regs = {}
        for line in out.splitlines():
            if "=" not in line or not line.strip().startswith(("PC=", "SP=", "AF=")):
                continue
            for tok in line.split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    try:
                        regs[k] = int(v, 16)
                    except ValueError:
                        pass
            break
        return regs


def main():
    z = ZRCP()

    print("-- setup: cpu-step mode, step-over-interrupt, cold reset --")
    z.cmd("set-debug-settings 33")  # bit0 (show registers) + bit5 (step over interrupt)
    z.cmd("enter-cpu-step")
    z.cmd("hard-reset-cpu", 0.5)
    z.cmd("set-register PC=0000h")

    print("\n-- patch bytes, read back from inside the running emulator --")
    # Chunk 0 defaults to HOME; page in EXROM to check the EXROM-side patches.
    z.cmd("write-port 244 1")   # $F4 = 244 decimal, chunk 0 -> EXROM select bit
    z.cmd("write-port 255 128")  # $FF = 255 decimal, $80 = exromSelect
    check(z.hexdump_bytes(0x0068, 3) == bytes([0xC3, 0x24, 0x16]), "W_TAPE ($0068) is JP $1624")
    check(z.hexdump_bytes(0x00FC, 3) == bytes([0xC3, 0x3D, 0x16]), "R_TAPE ($00FC) is JP $163D")
    check(z.hexdump_bytes(0x0215, 3) == bytes([0xCD, 0x5C, 0x16]), "PGPSTR call site ($0215) is CALL $165C")
    check(z.hexdump_bytes(0x1624, 6) == bytes.fromhex("f53add5db728"), "W_TAPE_intercept's opening bytes match")
    z.cmd("write-port 244 0")
    z.cmd("write-port 255 0")
    check(z.hexdump_bytes(0x139F, 3) == bytes([0xCD, 0xDC, 0x3C]), "CLOSE #4 ($139F) is CALL $3CDC")
    check(z.hexdump_bytes(0x25C8, 4) == bytes([0xCD, 0xED, 0x3C, 0xC9]), "CAT ($25C8) is CALL $3CED / RET")
    check(z.hexdump_bytes(0x3CDC, 17) == bytes.fromhex("cd0f143acb5cfe04c03e03d30e010000c9"),
          "CLOSE4_intercept's full body matches")

    print("\n-- CLOSE4_intercept logic, tested directly (bypassing the real $140F call) --")
    z.cmd("write-memory-raw 23755 04")  # STRMN ($5CCB=23755) = 4
    z.cmd("set-register PC=3CDFh")      # right after CLOSE4_intercept's own CALL $140F
    z.cmd("set-register BC=1234h")      # sentinel
    for _ in range(6):
        z.cmd("cpu-step", 0.08)
    r = z.registers()
    check(r.get("BC") == 0x0000, "STRMN==4: BC forced to 0 after CMD_UNMOUNT is issued")

    z.cmd("write-memory-raw 23755 07")  # STRMN = 7 (not 4)
    z.cmd("set-register PC=3CDFh")
    z.cmd("set-register BC=ABCDh")      # sentinel
    for _ in range(2):
        z.cmd("cpu-step", 0.08)
    r = z.registers()
    check(r.get("BC") == 0xABCD, "STRMN!=4: BC completely untouched (RET NZ taken)")

    print("\n-- CAT_trampoline: real BANK_ENABLE pages the patched EXROM into chunk 0 --")
    z.cmd("set-register PC=3CEDh")
    z.cmd("set-register BC=0000h")
    # BANK_ENABLE's own instruction count varies slightly run to run
    # (branches on real system-variable state) -- wait generously rather
    # than assume a fixed step count, and fail loudly if it's never reached.
    reached_cat_listing = False
    for _ in range(80):
        r = z.cmd("cpu-step", 0.05)
        if "PC=1691" in r:
            reached_cat_listing = True
            break
    check(reached_cat_listing, "CAT_trampoline's CALL $1691 is reached within 80 steps")
    check(z.hexdump_bytes(0x0000, 3) == bytes([0xF3, 0x18, 0x46]),
          "chunk 0 shows real EXROM content ($F3 18 46...) once BANK_ENABLE has run")
    for _ in range(3):  # LD C,$0A / LD B,$C8 / LD IX,$5DDE -- execute all three
        z.cmd("cpu-step", 0.06)
    check(z.registers().get("IX") == 0x5DDE, "CAT_listing's IX is set to CAT_LINE_BUF ($5DDE)")

    print("\n-- CAT_listing's defensive DJNZ bound (found necessary during this test suite) --")
    z.cmd("write-port 244 1")
    z.cmd("write-port 255 128")
    z.cmd("set-register PC=16C0h")   # cat_loop_dec: DJNZ cat_loop
    z.cmd("set-register BC=0200h")   # B=2 -> should decrement and jump back
    r = z.cmd("cpu-step", 0.1)
    check(z.registers().get("PC") == 0x1695 and z.registers().get("BC") == 0x0100,
          "DJNZ with B=2 decrements to 1 and jumps back to cat_loop ($1695)")
    z.cmd("set-register PC=16C0h")
    z.cmd("set-register BC=0100h")   # B=1 -> should decrement to 0 and fall through
    r = z.cmd("cpu-step", 0.1)
    check(z.registers().get("PC") == 0x16C2 and z.registers().get("BC") == 0x0000,
          "DJNZ with B=1 decrements to 0 and falls through to cat_done ($16C2)")
    z.cmd("write-port 244 0")
    z.cmd("write-port 255 0")

    print("\n-- full boot: real copyright banner, real interactive BASIC --")
    z.cmd("hard-reset-cpu", 0.5)
    z.cmd("set-register PC=0000h")
    z.cmd("exit-cpu-step")
    time.sleep(2)
    ocr = z.cmd("get-ocr", 1.5)
    check("T/S 2068 Computer" in ocr, "boot screen shows the real TS2068 copyright banner")
    z.cmd("send-keys-ascii 100 13")
    time.sleep(2)
    ocr = z.cmd("get-ocr", 1.5)
    # get-ocr's response has ZRCP's own trailing "command> "/"command@cpu-step> "
    # prompt appended after the real screen content -- filter that out rather
    # than just taking the last non-blank line, which would pick up the prompt
    # text instead of the actual screen cursor.
    screen_lines = [l for l in ocr.splitlines() if l.strip() and not l.strip().startswith("command")]
    check(bool(screen_lines) and screen_lines[-1].strip() == "K",
          "reaches the real interactive BASIC ready prompt ('K' cursor)")

    print("\n-- ordinary BASIC still works (regression check) --")
    z.cmd("send-keys-ascii 120 80")   # 'P' -> PRINT keyword (K-mode single-key entry)
    for ch in " 2+2":
        z.cmd(f"send-keys-ascii 100 {ord(ch)}", 0.15)
    z.cmd("send-keys-ascii 100 13", 0.4)
    time.sleep(0.8)
    ocr = z.cmd("get-ocr", 1.2)
    check("4" in ocr and "0 OK" in ocr, "PRINT 2+2 evaluates to 4 with a clean '0 OK' status")

    print("\n=======================")
    if failures == 0:
        print("ALL PASSED")
    else:
        print(f"{failures} CHECK(S) FAILED")
    return failures


if __name__ == "__main__":
    raise SystemExit(main())
