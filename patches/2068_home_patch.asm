; ESP2068 -- TS2068 port of ESPectrum
;
; patches/2068_home_patch.asm -- Phase 4, Piece C: the two HOME ROM
; patch points for the virtual disk feature (CAT, CLOSE #4). See
; TS2068-VIRTUAL-DISK-PLAN.md and PLAN.md's "Phase 4" section for the
; full design; this file is the concrete implementation.
;
; Both patch points and the one free-space pocket used here were
; hex-verified against the real, genuine Timex ROM dump in
; patches/base/2068Home.BIN before this source was written, not
; guessed at. Assemble AFTER 2068_exrom_patch.asm (CAT's trampoline
; calls into that file's CAT_listing routine at a fixed address, so
; that file's layout must be known first):
;
;   sjasmplus --raw=patches/out/2068Home.BIN patches/2068_home_patch.asm

    DEVICE NOSLOT64K

; ---- Wire protocol constants (must match include/VirtualDisk.h exactly) ----
PORT_CMD            EQU $0E
CMD_UNMOUNT          EQU $03

; ---- Real system variables/entry points (hex-verified, not guessed) ----
STRMN               EQU $5CCB   ; current stream number, written by the real $140F
BANK_ENABLE         EQU $6499   ; real, RAM-resident TS2068 dispatcher: BANK_ENABLE(B=bank, C=horizontal select)
                                ; confirmed from "docs/Timex Sinclair 2068 EXROM.txt"'s labeled
                                ; disassembly, and already used by real HOME ROM code
                                ; immediately before CAT's own dead stub (the routine ending
                                ; at $25B9-$25C7, right before this patch point).

; This intercept pages EXROM into chunk 0 to reach the real EXROM
; patch's CAT_listing routine. Its address is fixed by
; 2068_exrom_patch.asm's own layout -- confirmed via that file's
; assembled symbol table (patches/out/2068Exrom.sym), not guessed.
CAT_listing         EQU $1691

; ---- Original ROM content, unpatched, up to the first patch point ----
    ORG 0
    INCBIN "base/2068Home.BIN", 0, $139F

; ---- Patch 1: CLOSE #4 ($139F), 3-byte CALL redirect over the real "CALL $140F" ----
    CALL CLOSE4_intercept

    INCBIN "base/2068Home.BIN", $139F+3, $25C8-($139F+3)

; ---- Patch 2: CAT ($25C8), 4-byte redirect over the real dead-end stub
; ("LD B,$CF" / "JR $25D6") -- CAT is now fully handled by the
; trampoline below and never falls through to the shared FORMAT/MOVE/
; ERASE error tail at $25D6, which stays completely untouched for
; those three (still-dead) keywords. ----
    CALL CAT_trampoline
    RET

    INCBIN "base/2068Home.BIN", $25C8+4, $3CDC-($25C8+4)

; ---- New code, placed in the confirmed-dead $3CDC-$3CFF pocket (36
; bytes, immediately before CHRSET at $3D00 -- independently
; corroborated by gus-rom-analysis.md, which documents TS-Pico's own
; real, shipped ROM using this exact same pocket for its own
; trampoline) ----
    ORG $3CDC

; CLOSE #4 intercept. Real $139F's own tail ("LD A,B"/"OR C"/"RET Z"/
; "CALL $13BE", untouched, resumes right after this CALL returns) --
; confirmed from the design doc's hex trace of $139F/$140F. $140F
; itself writes the requested stream number to STRMN before resolving
; it into a channel lookup in BC, so this intercept only needs to
; check STRMN, not BC's resolved value. Nothing between CALL $140F and
; the STRMN check touches BC, so no save/restore of it is needed for
; the "not stream 4" path -- it survives exactly as real $140F left it.
CLOSE4_intercept:
    CALL $140F
    LD A,(STRMN)
    CP 4
    RET NZ                       ; not stream #4: BC left exactly as $140F resolved it --
                                  ; the real, untouched tail at $13A2 (LD A,B/OR C/RET Z/
                                  ; CALL $13BE) then behaves exactly as stock
    LD A,CMD_UNMOUNT
    OUT (PORT_CMD),A
    LD BC,0                      ; force "unused stream" so the untouched tail's own
    RET                          ; RET Z fires, silently skipping the real CALL $13BE

; CAT trampoline. Pages EXROM into chunk 0 via the real BANK_ENABLE
; service (B=$FE/C=$FE matches D_EXT's own real parameter values,
; confirmed from the disassembly), calls the real listing loop that
; lives in the patched EXROM, then restores HOME (B=$FF/C=$00, matches
; D_HOME's own values) before returning -- CAT's own dispatch-table
; entry expects a normal RET when done, same as every other keyword
; handler.
CAT_trampoline:
    LD BC,$FEFE
    CALL BANK_ENABLE
    CALL CAT_listing
    LD BC,$FF00
    CALL BANK_ENABLE
    RET

; ---- Real ROM content resumes, unpatched, from here to the end of the file ----
    INCBIN "base/2068Home.BIN", $, 16384-$
