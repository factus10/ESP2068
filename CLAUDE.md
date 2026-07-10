# ESP2068 — TS2068 port of ESPectrum

## What this project is

A Timex Sinclair 2068 emulator, built by porting [EremusOne/ESPectrum](https://github.com/EremusOne/ESPectrum) (an ESP32 + VGA + PS/2 ZX Spectrum 48K/128K/Pentagon emulator) to run the TS2068 on the same ESP32 hardware. The 2068's SCLD chip replaces the Sinclair ULA; everything else (Z80 core, VGA DMA driver, AY sound, PS/2 keyboard, SD/file-nav, OSD menu) is reused as a chassis.

The technical design — SCLD register contract, 8-slot memory model, display modes, timing, cartridge loading, and the four-slice work breakdown — lives in [TS2068-ESPECTRUM-PORT-PLAN.md](TS2068-ESPECTRUM-PORT-PLAN.md). Treat that file as the spec every slice codes against. The phased build-order plan is in [PLAN.md](PLAN.md).

## Current repo state

ESPectrum's full source and history are vendored in (merged from the `upstream` remote, `EremusOne/ESPectrum`, `master` branch — see Phase 0 in `PLAN.md`). `src/`, `include/`, `platformio.ini` etc. now exist at repo root alongside the planning docs. To pull upstream fixes later: `git fetch upstream && git merge upstream/master`. The stock `nopsram` build **compiles clean** (`pio run -e nopsram`, verified 2026-07-10 — see `PLAN.md` Phase 0); flash-and-boot on real hardware is still unverified (no ESP32 board in this environment) — do that on a real board before slice 1 lands.

The only TS2068-specific code so far is the frozen interface header from `PLAN.md` Phase 1: [`include/SCLD.h`](include/SCLD.h) + [`src/SCLD.cpp`](src/SCLD.cpp) — the `memChunk[8]`/port 0xF4/0xFF/DFILE-base contract, stub bodies only, verified to compile and link (`pio run -e nopsram` shows `Compiling .../SCLD.o`, build still `SUCCESS`). It is not wired into `Ports::input`/`output` yet. Everything else is still stock ESPectrum; slice 1 (MMU + memory core, `src/CPU.cpp` + `include/MemESP.h`) is the next real code change, per `PLAN.md` Phase 2.

## Upstream facts worth knowing before touching code

- **License: GPL-3.0.** ESPectrum is GPL-3.0-or-later. Anything derived from it — including this port — must stay GPL-3.0 compatible. Don't suggest closed-sourcing, relicensing, or pulling in incompatible-license dependencies.
- **Build system:** PlatformIO, `framework = espidf`, `platform = espressif32@5.4`, `board = pico32`. Two build envs in `platformio.ini`: `psram` (default) and `nopsram`. This port targets `nopsram` — the plan doc's memory budget assumes ~320K usable internal SRAM and explicitly does not need PSRAM.
- **Hardware:** ESP32 dev boards with VGA output (TTGo VGA32-class is the reference board), PS/2 keyboard input, SD card for file/cartridge loading. Same board class works for both Spectrum and TS2068 modes — no new hardware, just new firmware paths.
- **Where the port touches the tree** (once vendored — see `PLAN.md` Phase 0):
  - `src/CPU.cpp` — the actual `Z80Ops::peek8_std`/`poke8_std`/`peek16_std`/`poke16_std`/`fetchOpcode_std`/`addressOnBus_std` bodies. This is the hot path the 8-slot MMU port rewrites. (Not `include/Z80_JLS/z80.h` — that file only holds the opcode dispatch table and calls through function pointers defined in `z80operations.h`, implemented in `CPU.cpp`.)
  - `include/MemESP.h` / memory model — today `ramCurrent[4]` / `ramContended[4]` at 16K (`MEM_PG_SZ 0x4000`) granularity, `ram[8]` (128K worth of 16K banks), `rom[5]`. The port replaces the 4-slot model with an 8×8K `memChunk[]` for this machine only — it does not need the full 128K RAM page set (2068 is 48K-class: HOME 64K + EXROM 8K + DOCK ≤24K).
  - `src/Video.cpp` / `ESP32Lib/VGA/VGA.h` / `VGA6Bit.h` — display renderer and the `vidmodes[][17]` VGA timing table. `vga.init()` (which sizes the framebuffer and DMA descriptors) is called exactly once per boot, selected by `Config::arch[0]`; there is no live resolution switch anywhere in ESPectrum. The TS2068 port's resolved design is to boot into one fixed 512-column physical mode always and let the SCLD's port 0xFF video-mode bits pick which renderer fills it (see plan doc's "Display" section) — this avoids needing to invent live mode-switching.
  - `src/Ports.cpp` — I/O port dispatch. Existing Spectrum ports use partial address decoding (e.g. `(address & 0x8002) == 0`); the 2068's ports 0xFF and 0xF4 are fully decoded and the OUT handlers must match the full address, not a bitmask subset.
  - `src/Z80_JLS.cpp` `Z80::check_trdos()` — the "watch PC high byte, repoint a bank pointer, flip a flag" pattern used for TR-DOS autostart. This is the template for LROS/AROS/DOCK cartridge autostart (plan doc, Cartridge loading section).
  - `src/ESPConfig.cpp` (declares `class Config` in `include/ESPConfig.h` — file is `ESPConfig`, not `Config`) / `src/OSDMain.cpp` — machine-selection menu and `Config::arch` string (`"48K"`, `"128K"`, `"+2A"`, `"Pentagon"`, `"TK90X"`, `"TK95"`). A TS2068 entry adds a new arch string and a first-character case in the switches that key off `Config::arch[0]` (e.g. in `Video.cpp`'s mode-select). No existing first character collides with a `2068`-style prefix.

## Conventions for working in this repo

- The plan doc (`TS2068-ESPECTRUM-PORT-PLAN.md`) is the source of truth for the SCLD register contract and memory model. If an implementation detail there turns out to be wrong once code exists, update the doc in the same change — don't let it drift into a stale spec.
- Prefer small, slice-scoped changes that match the four-slice breakdown (MMU/memory core, hi-res renderer + VGA timing, cartridge loader, DOCK/test corpus) over sweeping refactors — the whole point of that split is that the slices share almost no code surface.
- This is embedded C/C++ on a resource-constrained target (no PSRAM in the primary build). Be deliberate about anything that grows static footprint or adds per-access branches on the Z80 peek/poke hot path — the plan doc calls out that the MMU port should be a shift/mask constant change, not a new branch.
- Don't invent write-protect or contention mechanisms that don't exist upstream — the plan doc's verified-findings section notes ESPectrum has no generalized per-page read-only flag today (ROM protection is a single hardcoded `page == 0` check); this port adds a real one rather than assuming it can reuse something that isn't there.
