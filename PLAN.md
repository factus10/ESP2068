# Execution Plan

This is the phased build order for the port described in [TS2068-ESPECTRUM-PORT-PLAN.md](TS2068-ESPECTRUM-PORT-PLAN.md). That document is the technical contract (SCLD registers, memory model, display, timing, cartridge format); this document is the sequencing — what happens in what order, who can start when, and what's still an open call.

## Phase 0 — Bootstrap (blocks everything)

1. ~~**Vendor ESPectrum into this repo.**~~ **Done (2026-07-10).** Added `upstream` remote (`https://github.com/EremusOne/ESPectrum.git`), fetched full history, merged `upstream/master` into local `main` with `--allow-unrelated-histories` (no conflicts — no filename overlap between the planning docs and ESPectrum's tree). Upstream's default branch is `master`, not `main` as originally guessed — to pull fixes later: `git fetch upstream && git merge upstream/master`. No GitHub-side fork was created (that's still a separate call tied to your GitHub identity, not needed to start coding).
2. ~~**Baseline build check.**~~ **Compile verified (2026-07-10).** `pio run -e nopsram` (PlatformIO 6.1.19, freshly installed) succeeds on the stock, unmodified tree: `espressif32@5.4` + the pinned Xtensa toolchain/ESP-IDF (`framework-espidf@3.40405.230623`) installed and built cleanly, `SUCCESS` in ~2 minutes, RAM 7.1% (23384/327680 B), Flash 70.8% (1392785/1966080 B). No errors. **Flash-and-boot on real hardware is still open** — no ESP32 board is attached in this environment, so that half of the check needs to happen on your machine with a board connected: `pio run -e nopsram -t upload`, then confirm a stock 48K/128K boot over serial/VGA before slice 1 changes land.
3. **License/attribution check — still open.** Confirm `LICENSE` (GPL-3.0) and upstream attribution headers stay intact. The TS2068 build is a derivative work and inherits GPL-3.0.

Exit criteria: repo has ESPectrum's source under version control here (done), stock build compiles (done — see above) and boots on hardware (open, needs a board), `CLAUDE.md`'s file-path claims are re-verified against the now-local checkout (done — one correction found: the config file is `src/ESPConfig.cpp`/`include/ESPConfig.h`, not `Config.cpp`/`Config.h`; `CLAUDE.md` updated).

## Phase 1 — Freeze the interface (small, but unblocks the team)

Turn the plan doc's "Interface to agree before anyone writes code" section into an actual compiling header — not just prose agreement. Concretely:

- A header (e.g. `include/SCLD.h`) declaring: the `memChunk[8]` array, `address >> 13` chunk index, 0xF4 bit→chunk mapping, 0xFF bit-7 DOCK/EXROM toggle, the new per-chunk read-only flag, and the DFILE-base/screen-select constants for `grmem`.
- Stub implementations (even if they just return HOME pointers unconditionally) so slices 2 and 3 can compile and link against the real interface immediately, rather than against a slice-owner's private mental model of it.

This is intentionally small — a day, not a week — but everything downstream assumes it, so it goes first and gets explicit sign-off from whoever owns slice 1 before slices 2–3 start building on top of it.

Exit criteria: `SCLD.h` exists, compiles, and is reviewed/agreed by whoever owns slice 1 (the 8-slot MMU is the one place the interface has to be exactly right).

## Phase 2 — Four slices

Same split as the plan doc, with sequencing notes:

| Slice | Depends on | Can start |
|---|---|---|
| 4. DOCK image corpus + test harness | nothing | **Immediately, parallel with Phase 1.** No code dependency; unblocks everyone else's testing, so starting late is the most common way this kind of plan slips. |
| 1. MMU + memory core | Phase 1 interface | Right after Phase 1 lands. Critical path — this is `src/CPU.cpp`'s six `Z80Ops::*_std` functions, the `memChunk[]` resolve logic, the new per-chunk read-only flag, and the 0xF4/0xFF port handlers. |
| 2. Hi-res renderer + VGA mode table | Phase 1 interface (build against the stub) | Right after Phase 1 lands, in parallel with slice 1. Needs real monitor bench time for the new 512-column `vidmodes[]` timing row — budget that explicitly, it's not pure arithmetic. |
| 3. Cartridge loader | Phase 1 interface | Right after Phase 1 lands, in parallel with slices 1–2. Uses the `check_trdos` PC-watching pattern for LROS/AROS/DOCK autostart. |

Also in this phase, folded into slice 1's scope (it's small and touches the same `Reset()` architecture-select switch slice 1 is already editing, not worth a fifth slice): set `tStatesPerLine`, `tStatesScreen`, `tStatesBorder`, `VsyncTarget` for 60Hz TS2068 timing, and collapse the contention table to the flat constant confirmed in the plan doc's verified findings.

Exit criteria per slice:
- **Slice 1:** peek/poke/fetchOpcode round-trip correctly through `memChunk[]` for HOME/DOCK/EXROM in a unit-level test (even a simple host-side harness poking the array), 0xF4/0xFF port writes visibly repoint the right chunks, ROM chunks reject writes.
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
- **No existing per-chunk read-only mechanism.** v0.1 of the plan doc assumed ESPectrum's ROM write-protect "generalizes"; verification found it's a single hardcoded `page == 0` check, not a flag array. Slice 1 is building this fresh, which is more scope than the original one-line assumption implied.
- **DOCK/ROM image provenance.** Real `.DCK` dumps and ROM images may be copyrighted. Fine to use privately for bring-up/testing; be deliberate about what actually gets committed to a repo, especially if it's ever made public.
- **GPL-3.0 inheritance.** Any dependency or tool pulled in during this port needs to stay license-compatible with the GPL-3.0 base.
