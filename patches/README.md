# ROM patches for the virtual disk feature (Phase 4, Pieces B/C)

Z80 assembly source patching the real TS2068 HOME ROM and EXROM to add
the virtual disk feature — see [`TS2068-VIRTUAL-DISK-PLAN.md`](../TS2068-VIRTUAL-DISK-PLAN.md)
for the full design and [`PLAN.md`](../PLAN.md)'s "Phase 4" for
sequencing. This is the first place in this project that authors new
Z80 machine code rather than reading or hex-patching existing bytes —
see `PLAN.md`'s Phase 4 writeup for why that's flagged as a genuinely
different kind of work from everything before it.

## What's here

- **`base/2068Home.BIN` / `base/2068Exrom.BIN`** — the real, genuine
  stock Timex TS2068 ROMs, copied in from the local reference library.
  Committing these is deliberate, not an oversight: per the project's
  resolved provenance policy (see [`../test/dock/README.md`](../test/dock/README.md)),
  Timex's own stock TS2068 ROMs are public domain, confirmed by a
  direct, on-file statement from Timex Group USA's own Senior VP — this
  is the one case where committing real ROM bytes into this repo is
  fine. These files are the assembler's `INCBIN` source, not test
  fixtures — the patch `.asm` files splice new code around specific,
  hex-verified byte offsets in these exact files.
- **`2068_exrom_patch.asm`** — the three EXROM patch points (`W_TAPE`
  `$0068`, `R_TAPE` `$00FC`, the `PGPSTR`-call redirect `$0215`) plus
  `CAT`'s listing routine (called from the HOME ROM patch via
  `BANK_ENABLE`). All new code lives in `$1624`-`$1CFF`, confirmed
  dead/padding space in the real 8K EXROM (disassembly-labeled
  `BLK1`/`BLK2`/`UNUSED1` in the local reference library's
  `docs/Timex Sinclair 2068 EXROM.txt`) — no size extension, the
  output stays exactly 8192 bytes.
- **`2068_home_patch.asm`** — the two HOME ROM patch points (`CAT`
  `$25C8`, `CLOSE #4` `$139F`). New code lives in `$3CDC`-`$3CFF`, a
  second confirmed-dead 36-byte pocket immediately before `CHRSET`
  (independently corroborated by the reference library's
  `gus-rom-analysis.md` — TS-Pico's own real, shipped ROM uses this
  exact same pocket for its own trampoline).
- **`out/2068Home.BIN` / `out/2068Exrom.BIN`** — the assembled,
  patched derivative ROMs. Committed deliberately (regenerable, but a
  real, small, deterministic build artifact someone can put straight
  onto an SD card without needing `sjasmplus` installed — same
  precedent as `test/dock/fixtures/synthetic.dck`).
- **`verify_patches.py`** — hex-level verification, the interim check
  `PLAN.md`'s Phase 4 Piece D writeup already anticipated (this
  project has no host-testable way to run real Z80 machine code
  against patched ROM content — `CPU.cpp`'s Z80 core is
  ESP-IDF-entangled, unlike `SCLD`/`DockLoader`/`RomLoader`). Confirms
  every patch point's redirect bytes, that everything outside the
  patch points and the two new-code regions is byte-identical to the
  real ROM (including `CHRSET`, the untouched `FORMAT`/`MOVE`/`ERASE`
  dead stubs, etc.), and spot-checks that the new code's opening
  instructions decode as intended. **Does not confirm the patches
  behave correctly when actually executed by a Z80** — that still
  needs either a host-side Z80 core (doesn't exist in this project
  yet) or real hardware. 24/24 checks passing.

## Rebuilding

```
sjasmplus --raw=patches/out/2068Exrom.BIN --sym=patches/out/2068Exrom.sym patches/2068_exrom_patch.asm
sjasmplus --raw=patches/out/2068Home.BIN  --sym=patches/out/2068Home.sym  patches/2068_home_patch.asm
python3 patches/verify_patches.py
```

Assemble the EXROM patch first — the HOME ROM patch's `CAT_trampoline`
calls into a fixed address in the assembled EXROM (`CAT_listing`,
currently `$1691`), confirmed via `out/2068Exrom.sym`, not guessed. If
you edit `2068_exrom_patch.asm` in a way that moves `CAT_listing`,
update the `CAT_listing EQU` line in `2068_home_patch.asm` to match
before reassembling the HOME ROM patch.

Requires [`sjasmplus`](https://github.com/z00m128/sjasmplus) — no
Homebrew formula exists; install from the project's own GitHub
releases. See `PLAN.md`'s Phase 4 writeup for why this tool was chosen
(verified, not assumed, against exactly this INCBIN-splice-plus-ORG
workflow).

## What's confirmed vs. what's a design choice, not yet run on real hardware

Every patch point's original bytes, the two free-space regions, `TADDR`
(`$5C74`, SAVE/LOAD/VERIFY/MERGE distinction), `STRMN` (`$5CCB`),
`BANK_ENABLE` (`$6499`) and its real `B`/`C` calling convention, `RST
$10`'s real target (`$11ED`), and `R_TAPE`'s real carry-flag
success/failure convention were all hex-verified against the real ROM
files and/or a labeled disassembly before being used here — see
`TS2068-VIRTUAL-DISK-PLAN.md`'s "Where the new code lives" section for
citations. A few things are reasoned design choices, not hex-verified
ROM facts:

- The `"D:"` prefix check is case-sensitive (`"D:"` only) — matches
  every example in the design doc's command-syntax section, but
  nothing forces this; easy to relax later.
- `D_MODE_FLAG`/`CAT_LINE_BUF` (`$5DDD`/`$5DDE`) are placed in the
  reference library's documented reserved region, just past TS-Pico's
  own claimed block — corroborated by two independent real-world
  sources but not verified against a boot-time sysvar-defaults trace.
- `D:` `VERIFY` currently behaves identically to `LOAD` (bytes get
  written to memory, not just compared) — a known simplification.
  `VirtualDisk`'s `READ_HEADER`/`READ_DATA` have no true
  non-destructive verify-only mode today.
- Non-`.tap` "direct load" and `.tzx` containers are out of scope —
  `VirtualDisk::mountRead()` already fails cleanly for both (Phase 4
  Piece A).

And the big one, repeated because it matters: **nothing here has run
against a real Z80 core, emulated or physical.** Every byte is verified
correct by careful hand-tracing against real disassembled ROM code and
cross-checked mechanically by `verify_patches.py` — that is a real,
meaningful bar, but it is not the same as watching `LOAD "D:test.tap"`
actually work.
