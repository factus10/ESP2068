# Execution Plan

This is the phased build order for the port described in [TS2068-ESPECTRUM-PORT-PLAN.md](TS2068-ESPECTRUM-PORT-PLAN.md). That document is the technical contract (SCLD registers, memory model, display, timing, cartridge format); this document is the sequencing — what happens in what order, who can start when, and what's still an open call.

## Phase 0 — Bootstrap (blocks everything)

1. ~~**Vendor ESPectrum into this repo.**~~ **Done (2026-07-10).** Added `upstream` remote (`https://github.com/EremusOne/ESPectrum.git`), fetched full history, merged `upstream/master` into local `main` with `--allow-unrelated-histories` (no conflicts — no filename overlap between the planning docs and ESPectrum's tree). Upstream's default branch is `master`, not `main` as originally guessed — to pull fixes later: `git fetch upstream && git merge upstream/master`. No GitHub-side fork was created (that's still a separate call tied to your GitHub identity, not needed to start coding).
2. ~~**Baseline build check.**~~ **Compile verified (2026-07-10).** `pio run -e nopsram` (PlatformIO 6.1.19, freshly installed) succeeds on the stock, unmodified tree: `espressif32@5.4` + the pinned Xtensa toolchain/ESP-IDF (`framework-espidf@3.40405.230623`) installed and built cleanly, `SUCCESS` in ~2 minutes, RAM 7.1% (23384/327680 B), Flash 70.8% (1392785/1966080 B). No errors. **Flash-and-boot on real hardware is still open** — no ESP32 board is attached in this environment, so that half of the check needs to happen on your machine with a board connected: `pio run -e nopsram -t upload`, then confirm a stock 48K/128K boot over serial/VGA before slice 1 changes land.
3. **License/attribution check — still open.** Confirm `LICENSE` (GPL-3.0) and upstream attribution headers stay intact. The TS2068 build is a derivative work and inherits GPL-3.0.

Exit criteria: repo has ESPectrum's source under version control here (done), stock build compiles (done — see above) and boots on hardware (open, needs a board), `CLAUDE.md`'s file-path claims are re-verified against the now-local checkout (done — one correction found: the config file is `src/ESPConfig.cpp`/`include/ESPConfig.h`, not `Config.cpp`/`Config.h`; `CLAUDE.md` updated).

## Phase 1 — Freeze the interface (small, but unblocks the team)

~~Turn the plan doc's "Interface to agree before anyone writes code" section into an actual compiling header.~~ **Done (2026-07-10).** [`include/SCLD.h`](include/SCLD.h) + [`src/SCLD.cpp`](src/SCLD.cpp) added:

- `mmuSelect`/`displayControl` (raw port 0xF4/0xFF values) plus `displayControl`'s decoded fields (`videoMode`, `hiresInkPaper`, `intInhibit`, `exromSelect`).
- `memChunk[8]` / `memChunkReadOnly[8]`, `resolveMemChunks()` implementing the plan doc's resolve loop verbatim, and `homePage()`/`dockPage()`/`exromPage()` stubs named to match the plan doc's pseudocode 1:1.
- `OUT_F4`/`IN_F4`/`OUT_FF`/`IN_FF` port handler stubs (not yet wired into `Ports::input`/`output` — that's slice 1's job in Phase 2).
- `DFILE1_BASE`/`DFILE2_BASE` constants, with a comment explicitly declining to pick a single "use secondary DFILE" toggle, since mode 010 needs both DFILEs at once — that decision is left to slice 2, not baked in here.
- Two extra write-protect call sites found while grounding the header against the real (now-vendored) source, beyond the one `CPU.cpp` call site already known: `MemESP::writebyte`/`writeword` (in `include/MemESP.h`) is a *second*, independent poke path — used by `Snapshot.cpp` and `Tape.cpp` for direct memory pokes — with its own different hardcoded check (`page == pagingmode2A3`, not `page == 0`). Slice 1 needs to repoint both paths at `memChunkReadOnly[]`, not just the CPU hot path. Noted in `SCLD.h`'s comments so it doesn't get missed.

Verified compiling for real: `pio run -e nopsram` shows `Compiling .pio/build/nopsram/src/SCLD.o` and the env still builds `SUCCESS` (firmware size unchanged from the Phase 0 baseline — expected, since nothing references the new symbols yet and the linker garbage-collects the unused section). `src/CMakeLists.txt` globs all of `src/*.*`, so any new `.cpp` here is picked up automatically without touching the build config.

Not yet done: explicit sign-off from whoever ends up owning slice 1 — this header is a proposal until a human reviews it, not a rubber stamp because it compiles.

Exit criteria: `SCLD.h` exists (done), compiles (done, verified above), and is reviewed/agreed by whoever owns slice 1 (open — needs a human, not just a green build).

## Phase 2 — Four slices

Same split as the plan doc, with sequencing notes:

| Slice | Depends on | Can start |
|---|---|---|
| 4. DOCK image corpus + test harness | nothing | **Immediately, parallel with Phase 1.** No code dependency; unblocks everyone else's testing, so starting late is the most common way this kind of plan slips. |
| 1. MMU + memory core | Phase 1 interface | Right after Phase 1 lands. Critical path — this is `src/CPU.cpp`'s six `Z80Ops::*_std` functions, the `memChunk[]` resolve logic, the new per-chunk read-only flag, and the 0xF4/0xFF port handlers. |
| 2. Hi-res renderer + VGA mode table | Phase 1 interface (build against the stub) | Right after Phase 1 lands, in parallel with slice 1. Needs real monitor bench time for the new 512-column `vidmodes[]` timing row — budget that explicitly, it's not pure arithmetic. |
| 3. Cartridge loader | Phase 1 interface | Right after Phase 1 lands, in parallel with slices 1–2. Uses the `check_trdos` PC-watching pattern for LROS/AROS/DOCK autostart. |

Also in this phase, folded into slice 1's scope (it's small and touches the same `Reset()` architecture-select switch slice 1 is already editing, not worth a fifth slice): set `tStatesPerLine`, `tStatesScreen`, `tStatesBorder`, `VsyncTarget` for 60Hz TS2068 timing, and collapse the contention table to the flat constant confirmed in the plan doc's verified findings.

### Slice 1 — done (2026-07-10), first increment

Real memory core landed, not just the Phase 1 stub:

- **`src/SCLD.cpp` backing store:** HOME ROM (16K, chunks 0-1) + HOME RAM (48K, chunks 2-7) are real allocated memory (`heap_caps_calloc`/`MALLOC_CAP_8BIT` on-device, matching `MemESP.cpp`'s existing pattern — nothing here needs PSRAM). DOCK (up to 8×8K) and EXROM (a single 8K image, chip-select-mirrored across every chunk that selects it — there's no larger EXROM address space for a second chunk bit to index into) resolve to a shared, zeroed, read-only "empty socket" page until `loadDockChunk()`/`loadExromImage()` populate them (slice 3's job) — `memChunk[]` is never null, even with nothing loaded. `resolveMemChunks()` now does the real HOME/DOCK/EXROM resolution, not a stub.
- **HOME ROM content is a placeholder (zeroed), not a real TS2068 ROM image.** No TS2068 ROM is embedded in this repo — loading a real one (from SD card, matching how the existing Spectrum ROMs' custom-flash procedure works, rather than baking copyrighted bytes into a git-tracked C header) is a separate, not-yet-scoped piece of work. The memory *mechanism* is complete and tested; booting real BASIC is not possible until that lands.
- **`src/CPU.cpp`:** added `Z80Ops::is2068` (alongside `is48`/`is128`/`isPentagon`/`is2a3`, explicitly set in every branch of `CPU::reset()`'s per-arch switch so the flag invariant holds across machine switches) and six `_2068` functions (`fetchOpcode_2068`, `peek8_2068`, `poke8_2068`, `peek16_2068`, `poke16_2068`, `addressOnBus_2068`) mirroring the `_std` versions with `>>13`/`&0x1fff`/`SCLD::memChunk[]`/`memChunkReadOnly[]`, contention always false. **Correction to the plan doc's original framing:** these are new sibling functions selected via the existing `_std`/`_2A3` function-pointer dispatch pattern, not in-place edits to `_std` — editing `_std` directly would have broken 48K/128K/Pentagon/TK, which share it. Also added a `Config::arch == "2068"` branch with real 60Hz timing constants (see below) — this is slice 1 making the trigger string exist in `CPU.cpp`/`Ports.cpp`, not the OSD menu wiring, which stays Phase 3's job as originally scoped.
- **TS2068 timing constants (`include/cpuESP.h`):** `TSTATES_PER_FRAME_2068`=58688, `MICROS_PER_FRAME_2068`=16639 (+125/150-speed variants), `INT_START2068`=0, `INT_END2068`=32. Sourced from [libspectrum](https://github.com/simonowen/libspectrum)'s `timings.c` (the Fuse emulator project ships a `timings_frame_timex_scld_60hz` table for this exact machine: 3,528,000Hz clock, 224 T-states/line, 262 lines/frame = 24 top border + 192 active + 25 bottom border + 21 vsync) — not derived or guessed. Not yet cross-checked against the TS2068 technical/service manual or real hardware; verify at bring-up.
- **`src/Ports.cpp`:** `Z80Ops::is2068` guard added at the top of `input()`/`output()`, fully decoding `address == 0x00F4`/`0x00FF` before any of the Spectrum's partial-decode branches run. This isn't cosmetic: 0xF4 is an *even* address, so without the guard it would fall into the ULA-port branch (`(address & 0x0001) == 0`) and never reach the SCLD at all. Other TS2068 ports (keyboard, AY, etc.) return a stub `0xff`/no-op — out of scope for "memory core."
- **`src/ESPectrum.cpp`:** `SCLD::allocateMemory()` added next to `MemESP::Init()` (called once at boot, not per-reset), `SCLD::reset()` added next to both `MemESP::Reset()` call sites (boot and user-triggered reset).
- **Test: `test/host/scld_test.cpp`** — standalone, host-g++-compilable (no ESP-IDF dependency; `SCLD.cpp` takes a `calloc()` path instead of `heap_caps_calloc()` when `ESP2068_HOST_TEST` is defined, so the test exercises the real firmware code, not a reimplementation). 30 checks, all passing: reset state, all-HOME resolution with correct read-only flags, HOME RAM read/write survives a DOCK round-trip, populated DOCK RAM vs DOCK ROM chunks, EXROM mirroring across multiple chunks, port read-back, and a write-protect check against the same flag `CPU.cpp`'s `poke8_2068`/`poke16_2068` gate on. Build/run: `g++ -std=c++17 -Wall -I include -DESP2068_HOST_TEST test/host/scld_test.cpp src/SCLD.cpp -o /tmp/scld_test && /tmp/scld_test`.
- **Firmware still builds:** `pio run -e nopsram` → `SUCCESS`, RAM 9.7% (31704/327680 B, up from the Phase 0 baseline's 7.1% — the increase is the new static 8K "empty socket" page plus small bookkeeping arrays, not the heap-allocated HOME RAM/ROM which doesn't show in this report), Flash 70.9% (1393953/1966080 B). No errors.

**Explicitly not done in this increment** (tracked so it isn't mistaken for finished): a real TS2068 ROM image and a loader for it; the second write-protect call site flagged in Phase 1 (`MemESP::writebyte`/`writeword`) — turned out not to matter yet, since TS2068 uses its own backing store entirely separate from `MemESP::ram[]`/`rom[]` and doesn't go through `Snapshot.cpp`/`Tape.cpp`'s `.SNA`/`.TAP` paths; it only becomes relevant if something later decides to reuse those paths for TS2068 snapshots. The OSD menu entry (Phase 3). Flash-and-boot on real hardware (no board in this environment — same open item as Phase 0).

Exit criteria per slice:
- **Slice 1:** ~~peek/poke/fetchOpcode round-trip correctly through `memChunk[]` for HOME/DOCK/EXROM in a unit-level test~~ **done**, see above. ~~0xF4/0xFF port writes visibly repoint the right chunks~~ **done** (host test + `Ports.cpp` wiring). ~~ROM chunks reject writes~~ **done** (host test exercises the flag; `CPU.cpp`'s actual gate is code-reviewed, not test-executed, since that needs the full Z80 core). Remaining before calling slice 1 fully closed: real ROM image + loader, and hardware bring-up.
- **Slice 2:** renders a static test card (from slice 4's corpus) correctly in standard, extended-colour, and hi-res modes on real hardware/monitor.
- **Slice 3:** loads a known-good `.DCK` (from slice 4) and autostarts it.
- **Slice 4:** a documented set of `.DCK` dumps + per-mode test cards checked in (or referenced, if licensing requires external sourcing) with a README noting provenance — don't check in cartridge dumps without confirming you have the right to redistribute them; homebrew/public-domain test cards are the safe default for anything committed to the repo.

## Phase 3 — Integration

Bring the four slices together against a real `Config::arch` entry (`"2068"` or similar), wire the machine into the OSD machine-select menu (`OSDMain.cpp`), and do a full boot-to-BASIC test on hardware. This is where slice-1/slice-2/slice-3 interface mismatches would surface if Phase 1's contract had a gap — treat any mismatch found here as a Phase 1 spec bug, fix the doc and the code together.

## Open decisions

These are called out in the plan doc too; repeating here because they affect sequencing, not just architecture:

- **TS2068 (60Hz/NTSC) vs TC2068 (50Hz/PAL):** build 60Hz first (it's the target). PAL is a timing-profile variant on the same `Reset()` switch slice 1 already owns — cheap to add after, not worth blocking v1 on.
- **Extended colour mode (010):** recommend deferring past v1. Standard + hi-res cover most native software; this shrinks slice 2's v1 scope without losing much real-world compatibility.
- **Board:** nopsram (TTGo VGA32-class) is the primary target and what the memory budget in the plan doc assumes. Keep one PSRAM board on the bench for double-buffering experiments only — don't let it become a dependency for v1.
- **Repo hosting (new, not in v0.1):** local git history with an `upstream` remote is enough to start (Phase 0). Whether/when to push this to a GitHub fork under your account is a separate call — flag it when you're ready to collaborate with the rest of the team or want CI, since that's the point a fork or a fresh repo with `EremusOne/ESPectrum` credited becomes worth setting up.

## Risk register

- **VGA timing for the new 512-column mode is hand-derived, not copy-paste.** Existing `vidmodes[]` rows top out at 360 active columns and each hand-tunes six APLL clock-divider constants plus porch/sync widths. Budget real monitor time for slice 2, not just a spreadsheet calculation.
- ~~**No existing per-chunk read-only mechanism.**~~ **Resolved by slice 1.** v0.1 of the plan doc assumed ESPectrum's ROM write-protect "generalizes"; verification found it's a single hardcoded `page == 0` check, not a flag array. `SCLD::memChunkReadOnly[8]` is the new one, built fresh as expected, and is what `CPU.cpp`'s `poke8_2068`/`poke16_2068` gate on.
- **DOCK/ROM image provenance.** Real `.DCK` dumps and ROM images may be copyrighted. Fine to use privately for bring-up/testing; be deliberate about what actually gets committed to a repo, especially if it's ever made public.
- **GPL-3.0 inheritance.** Any dependency or tool pulled in during this port needs to stay license-compatible with the GPL-3.0 base.
