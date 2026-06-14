# Effects reference

maxtracker uses IT-style effect codes internally: one letter (A-Z) plus a two-hex parameter `xy`. The `fx` byte stored in `MT_Cell` is the 1-based numeric code (A=1, B=2, ... Z=26). A `fx` of 0 means "no effect". The letter is retained for MAS/IT interchange, but the **UI shows a 3-letter mnemonic** (LSDJ/M8 style, e.g. `ARP`, `SPD`) in the Eff column.

The table below is generated from `arm9/source/core/effects.def`; keep both in sync. Effects flagged **NO ENGINE** are parsed and displayed but not yet dispatched by the maxmod engine, so they are audibly a no-op. They render red in the Eff column and the picker labels them `[NO ENGINE]`.

## How to read the parameter

Most parameters split into high nibble `x` and low nibble `y`. Volume-slide-style effects (**D**, **K**, **L**, **N**, **W**, **P**) share a common encoding:

| Parameter | Meaning                          |
|-----------|----------------------------------|
| `Dx0`     | slide up by `x` per tick         |
| `D0y`     | slide down by `y` per tick       |
| `DxF`     | fine slide up (1 tick)           |
| `DFy`     | fine slide down (1 tick)         |
| `D00`     | continue last slide              |

Pitch slides (**E**, **F**) use the same `Fx`/`Ex` sub-encoding for "fine" / "extra-fine".

Tempo (**T**) is split by value range: `00`-`0F` slides tempo down, `10`-`1F` slides up, `>= 20h` sets an absolute BPM.

## Effect table

| Code | Mnem  | Name                     | Param | Status    | Description |
|------|-------|--------------------------|-------|-----------|-------------|
| A    | `SPD` | Set Speed                | `xx`  |           | Set ticks per row (01–1F). Lower = faster. |
| B    | `JMP` | Position Jump            | `xx`  |           | Jump to order position `xx`, resume at row 0. |
| C    | `BRK` | Pattern Break            | `xx`  |           | End current row, advance one order, resume at row `xx`. |
| D    | `VSL` | Volume Slide             | `xy`  |           | See shared slide encoding above. |
| E    | `PSD` | Pitch Slide Down         | `xx`  |           | Slide pitch down `xx` per tick. `EFy` = fine, `EEy` = extra-fine. |
| F    | `PSU` | Pitch Slide Up           | `xx`  |           | Slide pitch up `xx` per tick. `FFy` = fine, `FEy` = extra-fine. |
| G    | `POR` | Portamento To Note       | `xx`  |           | Glide toward the new note at speed `xx`. Note is not retriggered. |
| H    | `VIB` | Vibrato                  | `xy`  |           | Pitch LFO. `x` = speed, `y` = depth. |
| I    | `TMR` | Tremor                   | `xy`  | NO ENGINE | Square-wave volume gate. Not implemented in the engine. |
| J    | `ARP` | Arpeggio                 | `xy`  |           | Rotate note / note+`x` semitones / note+`y` semitones each tick. |
| K    | `VBV` | Vibrato + Volume Slide   | `xy`  |           | Continue last Hxy; slide volume per Dxy semantics. |
| L    | `PRV` | Portamento + Volume Slide| `xy`  |           | Continue last Gxx; slide volume per Dxy semantics. |
| M    | `CHV` | Set Channel Volume       | `xx`  |           | Set channel volume (00–40). |
| N    | `CVS` | Channel Volume Slide     | `xy`  |           | Channel-volume slide with Dxy encoding. |
| O    | `OFS` | Sample Offset            | `xx`  |           | Start the note from byte offset `xx × 100h` into the sample. |
| P    | `PNS` | Panning Slide            | `xy`  | NO ENGINE | Pan slide with Dxy encoding. Not implemented. |
| Q    | `RTG` | Retrigger Note           | `xy`  |           | Retrigger every `y` ticks with volume function `x` (0 = copy, 1–5 slide down, 6–F slide up). |
| R    | `TRM` | Tremolo                  | `xy`  |           | Volume LFO. `x` = speed, `y` = depth. |
| S    | `EXT` | Extended                 | `xy`  | partial   | See sub-commands below (S3/S4/S5/SA are no-ops). |
| T    | `TMP` | Set Tempo                | `xx`  |           | `Txx >= 20h` sets BPM `xx`. 00–0F slides tempo down, 10–1F slides up. |
| U    | `FVB` | Fine Vibrato             | `xy`  |           | Vibrato with 1/4 depth resolution. |
| V    | `GLV` | Global Volume            | `xx`  |           | Set global mix volume (00–80). |
| W    | `GVS` | Global Volume Slide      | `xy`  |           | Global-volume slide with Dxy encoding. |
| X    | `PAN` | Set Panning              | `xx`  |           | Hard panning (00 = left, 80 = center, FF = right). |
| Y    | `PBR` | Panbrello                | `xy`  | NO ENGINE | Panning LFO. Not implemented. |
| Z    | `FLT` | MIDI / Filter            | `xx`  | NO ENGINE | MIDI macro / resonant filter. Not implemented. |

## S-extended sub-commands

`S` uses the high nibble as the sub-command selector:

| Code  | Meaning                                     |
|-------|---------------------------------------------|
| `S0x` | Glissando control (on/off)                  |
| `S1x` | Set finetune                                |
| `S2x` | Vibrato waveform (0 sine, 1 ramp, 2 square) |
| `S3x` | Tremolo waveform                            |
| `S4x` | Panbrello waveform                          |
| `S6x` | Fine pattern delay (`x` ticks)              |
| `S7x` | Sample/NNA control                          |
| `S8x` | Set panning coarse (`x × 10h`)              |
| `S9x` | Sound control (surround etc.)               |
| `SAx` | High-offset prefix for the next `Oxx`       |
| `SBx` | Loop point (`SB0` sets, `SBx` loops `x` times) |
| `SCx` | Note cut after `x` ticks                    |
| `SDx` | Note delay by `x` ticks                     |
| `SEx` | Pattern-row delay (`x` extra rows)          |
| `SFx` | MIDI macro (TODO)                           |

## In-app

The Eff column shows the 3-letter mnemonic (red when the effect is `[NO ENGINE]`). Moving the cursor onto the Eff or Prm column surfaces `<MNEM>  <name>` plus the wrapped description on the bottom screen.

Editing the Eff column is LSDJ-style:

- **A** on an empty cell inserts the first effect.
- **A + LEFT/RIGHT** (or DOWN/UP) cycles through the effect list; the bottom-screen hint updates live so you can read each effect's meaning while browsing.
- **B + A** removes the effect (clears both the Eff and Prm bytes).

The Prm column keeps hex nibble editing: **A + UP/DOWN** edits the high nibble, **A + LEFT/RIGHT** the low nibble.

## Adding an effect

1. Pick the next free numeric code (> 26) or claim an existing letter.
2. Add an `FX(...)` line to `arm9/source/core/effects.def`.
3. Regenerate this document from the X-list (by hand for now; the table above mirrors the def columns).
4. If the engine needs new dispatch, edit `lib/maxmod/source/core/mas.c` -- vendor-forked territory; tread carefully.
