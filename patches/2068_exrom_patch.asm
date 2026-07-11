; ESP2068 -- TS2068 port of ESPectrum
;
; patches/2068_exrom_patch.asm -- Phase 4, Piece B: the three EXROM patch
; points for the virtual disk feature (W_TAPE, R_TAPE, the PGPSTR-call
; redirect), plus CAT's listing routine (called via BANK_ENABLE from the
; HOME ROM patch, see 2068_home_patch.asm). See TS2068-VIRTUAL-DISK-PLAN.md
; and PLAN.md's "Phase 4" section for the full design; this file is the
; concrete implementation of what those documents describe.
;
; All five patch points and both free-space regions used here were
; hex-verified against the real, genuine Timex ROM dumps in
; patches/base/ (copied in under this project's now-resolved provenance
; policy -- see test/dock/README.md) before this source was written, not
; guessed at. Assemble with:
;
;   sjasmplus --raw=patches/out/2068Exrom.BIN patches/2068_exrom_patch.asm
;
; New code all lives in $1624-$1CFF, confirmed dead/padding space in the
; real 8K EXROM (disassembly-labeled BLK1/BLK2/UNUSED1 in
; "docs/Timex Sinclair 2068 EXROM.txt") -- no size extension, everything
; stays within stock EXROM's real 8K, entirely inside chunk 0 (the same
; chunk all three patch points already execute from), so no bank
; switching is needed for W_TAPE/R_TAPE/PGPSTR to reach it.

    DEVICE NOSLOT64K

; ---- Wire protocol constants (must match include/VirtualDisk.h exactly) ----
PORT_CMD            EQU $0E
PORT_STATUS         EQU $0F

CMD_MOUNT_READ          EQU $01
CMD_MOUNT_WRITE         EQU $02
CMD_UNMOUNT             EQU $03
CMD_FIND_BLOCK          EQU $04
CMD_READ_HEADER         EQU $05
CMD_READ_DATA           EQU $06
CMD_WRITE_HEADER        EQU $07
CMD_WRITE_DATA          EQU $08
CMD_CAT_SD_LINE         EQU $09
CMD_CAT_CONTAINER_LINE  EQU $0A

STATUS_OK           EQU $00
STATUS_ERROR        EQU $01
STATUS_EOF          EQU $02

; ---- Real system variables/entry points (hex-verified, not guessed) ----
TADDR               EQU $5C74   ; low byte: $00 while SAVEing, nonzero for LOAD/VERIFY/MERGE (confirmed: EXROM $04C9 "LD A,(TADDR)/AND A/JP Z,SAVE")
STRMN               EQU $5CCB   ; current stream number (not used in this file, see 2068_home_patch.asm)

; ---- New system variables, in the confirmed-reserved $5CCC-$5EE9 area,
; just past TS-Pico's own claimed $5DCD-$5DDB block (compatibility with
; TS-Pico is not a goal -- see TS2068-VIRTUAL-DISK-PLAN.md's provenance
; note -- this is just a hedge against colliding with a real product
; that also assumes this range is free). No filename pointer/length
; variable is needed: MOUNT fires directly from inside the PGPSTR
; intercept, while DE/BC still hold the real values from the just-
; replayed $0F99 call, so there is nothing to persist for that. ----
D_MODE_FLAG         EQU $5DDD   ; 1 byte: 0 = real cassette tape, nonzero = D: virtual disk active
CAT_LINE_BUF        EQU $5DDE   ; scratch buffer CAT's listing loop pokes formatted lines into
CAT_LINE_MAXLEN     EQU 64      ; buffer size; $5DDE-$5E1D, comfortably inside the reserved area
CAT_MAX_LINES       EQU 200     ; defensive iteration bound for CAT_listing -- see its own comment

; ---- Original ROM content, unpatched, up to the first patch point ----
    ORG 0
    INCBIN "base/2068Exrom.BIN", 0, $0068

; ---- Patch 1: W_TAPE ($0068), 3-byte JP over the real "LD HL,$00E5" ----
    JP W_TAPE_intercept

    INCBIN "base/2068Exrom.BIN", $0068+3, $00FC-($0068+3)

; ---- Patch 2: R_TAPE ($00FC), 3-byte JP over the real "INC D / EX AF,AF' / DEC D" ----
    JP R_TAPE_intercept

    INCBIN "base/2068Exrom.BIN", $00FC+3, $0215-($00FC+3)

; ---- Patch 3: PGPSTR's call site inside SLVM ($0215), 3-byte CALL redirect over the real "CALL $0F99" ----
    CALL PGPSTR_intercept

    INCBIN "base/2068Exrom.BIN", $0215+3, $1624-($0215+3)

; ---- New code, placed in confirmed-dead space; real ROM content resumes unpatched after it ----
    ORG $1624

; W_TAPE intercept. Real calling convention (confirmed from the real
; SAVE routine's own disassembly, "docs/Timex Sinclair 2068 EXROM.txt"
; line ~1690): A = $00 (header) or $FF (data), IX = buffer pointer, DE
; = length. The real caller explicitly PUSH IX / CALL W_TAPE / POP IX
; around every single call rather than trusting any post-call IX value
; -- confirmed directly from that disassembly -- so this intercept does
; not need to preserve or advance IX at all.
W_TAPE_intercept:
    PUSH AF                     ; save the real header/data flag + flags
    LD A,(D_MODE_FLAG)
    OR A
    JR Z,w_tape_real
    POP AF
    OR A                        ; re-test original A: Z = header (0), NZ = data ($FF)
    LD A,CMD_WRITE_HEADER
    JR Z,w_tape_send
    LD A,CMD_WRITE_DATA
w_tape_send:
    OUT (PORT_CMD),A            ; IX/DE already correct from the caller; the ESP32 host
    RET                         ; reads them directly at the moment of this OUT
w_tape_real:
    POP AF                      ; original A, completely undisturbed
    LD HL,$00E5                 ; replicate the clobbered instruction (the real, verified
                                 ; first bytes of stock W_TAPE)
    JP $006B                    ; W_TAPE+3 -- continue the real, unmodified routine

; R_TAPE intercept. Real calling convention (confirmed from the real
; LOAD routine, same file, line ~980): on entry, A = $00 (header) or
; $FF (data) request, IX = destination pointer, DE = length -- same
; "caller re-establishes IX via PUSH/POP around each call" pattern as
; W_TAPE, confirmed at the same two call sites. Real R_TAPE's
; success/failure convention (confirmed from R_TAPE1's own "CALL
; R_TAPE / RET C", same file line ~1146): carry SET = success, carry
; CLEAR = error.
;
; Known simplification, not a silent gap: this always writes the read
; bytes to memory (matching LOAD), it does not implement a true
; non-destructive VERIFY (the real R_TAPE distinguishes LOAD/VERIFY via
; the incoming carry flag, which this intercept does not inspect) --
; VirtualDisk's READ_HEADER/READ_DATA have no verify-only mode today.
R_TAPE_intercept:
    PUSH AF
    LD A,(D_MODE_FLAG)
    OR A
    JR Z,r_tape_real
    POP AF
    OR A
    LD A,CMD_READ_HEADER
    JR Z,r_tape_send
    LD A,CMD_READ_DATA
r_tape_send:
    OUT (PORT_CMD),A
    IN A,(PORT_STATUS)
    CP STATUS_OK                ; comparing against 0 -- carry is always left clear by this
    RET NZ                      ; error/EOF: return with carry clear (matches R_TAPE1's RET C)
    SCF
    RET                         ; success: carry set
r_tape_real:
    POP AF                      ; original A, completely undisturbed
    INC D                       ; replicate the three clobbered 1-byte instructions
    EX AF,AF'
    DEC D
    JP $00FF                    ; R_TAPE+3 -- continue the real, unmodified routine

; PGPSTR's call-site intercept, inside SLVM. On entry here (redirected
; from $0215), replays the real call first -- PGPSTR (the trampoline's
; target) pops the filename's string descriptor off the calculator/FP
; stack, returning with BC = length, DE = pointer (confirmed in the
; design doc's own hex trace) -- then checks for a "D:" prefix.
;
; Design choice, not a hex-verified ROM fact: the prefix check is
; case-sensitive ("D:" only, not "d:"), matching every example in the
; design doc's own command-syntax section. Easy to relax later if
; wanted; not blocking.
;
; No "D:" prefix: falls through with BC/DE/A completely undisturbed --
; original SLVM flow proceeds exactly as stock, including its own
; harmless truncated copy into the 10-char header buffer.
PGPSTR_intercept:
    CALL $0F99                  ; the real, original call this patch replaced
    PUSH AF                     ; PGPSTR's own real result in A must survive untouched
                                 ; on every "not D:" exit path
    LD A,B
    OR C
    JR Z,pgpstr_restore         ; empty string, can't be "D:..."
    LD A,B
    OR A
    JR NZ,pgpstr_check          ; BC>=256, definitely long enough
    LD A,C
    CP 2
    JR C,pgpstr_restore         ; length<2, can't be "D:..."
pgpstr_check:
    LD A,(DE)
    CP 'D'
    JR NZ,pgpstr_restore
    INC DE
    LD A,(DE)
    CP ':'
    JR NZ,pgpstr_restore_dec    ; DE was advanced by INC DE above; undo it before returning
    ; Matched "D:" -- DE currently points at the ':'.
    POP AF                      ; discard the saved A; this path overwrites it below regardless
    INC DE                      ; DE now points at the real filename, past "D:"
    DEC BC
    DEC BC                      ; BC -= 2 (the "D:" prefix's own length)
    LD A,(TADDR)
    AND A                       ; Z (low byte 0) = SAVEing; NZ = LOAD/VERIFY/MERGE
    LD A,CMD_MOUNT_WRITE
    JR Z,pgpstr_issue
    LD A,CMD_MOUNT_READ
pgpstr_issue:
    OUT (PORT_CMD),A            ; DE/BC still hold the filename pointer/length -- the ESP32
                                 ; host reads them directly at the moment of this OUT
    LD A,1
    LD (D_MODE_FLAG),A
    RET
pgpstr_restore_dec:
    DEC DE                      ; undo the INC DE from the ':' check above
pgpstr_restore:
    POP AF                      ; original A, completely undisturbed
    RET

; CAT's listing routine. Reached from the HOME ROM patch at $25C8 via
; BANK_ENABLE (see 2068_home_patch.asm) -- by the time this runs, chunk
; 0 is already showing this EXROM, so no further bank switching is
; needed here. Tries the mounted-container listing first; if
; CAT_CONTAINER_LINE reports STATUS_ERROR (VirtualDisk::catContainerLine()
; returns that when nothing is mounted), falls back to the SD-directory
; listing instead -- matching the design doc's two-mode CAT behavior
; without the Z80 side needing to track mount state itself. C holds the
; current command byte across the whole loop; each line is printed via
; RST $10 (confirmed live and correct for TS2068 HOME ROM, real target
; $11ED/SENDCH) until the high-bit-set terminator byte
; VirtualDisk::catSdLine()/catContainerLine() write.
;
; B is a defensive iteration bound (CAT_MAX_LINES), found necessary
; during Piece D testing (ZEsarUX, no real ESP32 backend to ever answer
; STATUS_EOF/STATUS_ERROR on port $0F -- confirmed live: without this,
; the loop re-issues CAT_CONTAINER_LINE forever, since an unconnected
; port always reads back a value that satisfies neither status check).
; VirtualDisk::catSdLine()/catContainerLine() are already guaranteed to
; return STATUS_EOF once real, so this should never fire in normal
; operation -- it exists purely so a communication glitch hangs a CAT
; listing instead of the whole machine.
CAT_listing:
    LD C,CMD_CAT_CONTAINER_LINE
    LD B,CAT_MAX_LINES
cat_loop:
    LD IX,CAT_LINE_BUF
    LD DE,CAT_LINE_MAXLEN
    LD A,C
    OUT (PORT_CMD),A
    IN A,(PORT_STATUS)
    CP STATUS_ERROR
    JR NZ,cat_check_eof
    LD A,C
    CP CMD_CAT_CONTAINER_LINE
    JR NZ,cat_done               ; already in SD mode and got an error -- stop
    LD C,CMD_CAT_SD_LINE         ; container mode failed (nothing mounted) -- fall back once
    JR cat_loop_dec
cat_check_eof:
    CP STATUS_EOF
    JR Z,cat_done
    LD HL,CAT_LINE_BUF
cat_print:
    LD A,(HL)
    PUSH AF
    AND $7F
    RST $10
    POP AF
    INC HL
    AND $80
    JR Z,cat_print
cat_loop_dec:
    DJNZ cat_loop
cat_done:
    RET

; ---- Real ROM content resumes, unpatched, from here to the end of the file ----
    INCBIN "base/2068Exrom.BIN", $, 8192-$
