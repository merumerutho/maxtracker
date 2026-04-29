# Effects reference

maxtracker uses IT-style effect codes: one letter (A–Z) plus a two-hex parameter `xy`. The `fx` byte stored in `MT_Cell` is the 1-based numeric code (A=1, B=2, … Z=26). A `fx` of 0 means "no effect".

The table below is generated from `arm9/source/core/effects.def` — keep both in sync. Effects flagged **TODO** are parsed and displayed but are not yet dispatched by the maxmod engine, so they are audibly a no-op.

## How to read the parameter

Most parameters split into high nibble `x` and low nibble `y`. Volume-slide–style effects (**D**, **K**, **L**, **N**, **W**, **P**) share a common encoding:

| Parameter | Meaning                          |
|-----------|----------------------------------|
| `Dx0`     | slide up by `x` per tick         |
| `D0y`     | slide down by `y` per tick       |
| `DxF`     | fine slide up (1 tick)           |
| `DFy`     | fine slide down (1 tick)         |
| `D00`     | continue last slide              |

Pitch slides (**E**, **F**) use the same `Fx`/`Ex` sub-encoding for "fine" / "extra-fine".

Tempo (**T**) is split by value range: `00`–`0F` slides tempo down, `10`–`1F` slides up, `>= 20h` sets an absolute BPM.

## Effect table

| Code | Name                     | Param | Status | Description |
|------|--------------------------|-------|--------|-------------|
| A    | Set Speed                | `xx`  |        | Set ticks per row (01–1F). Lower = faster. |
| B    | Position Jump            | `xx`  |        | Jump to order position `xx`, resume at row 0. |
| C    | Pattern Break            | `xx`  |        | End current row, advance one order, resume at row `xx`. |
| D    | Volume Slide             | `xy`  |        | See shared slide encoding above. |
| E    | Pitch Slide Down         | `xx`  |        | Slide pitch down `xx` per tick. `EFy` = fine, `EEy` = extra-fine. |
| F    | Pitch Slide Up           | `xx`  |        | Slide pitch up `xx` per tick. `FFy` = fine, `FEy` = extra-fine. |
| G    | Portamento To Note       | `xx`  |        | Glide toward the new note at speed `xx`. Note is not retriggered. |
| H    | Vibrato                  | `xy`  |        | Pitch LFO. `x` = speed, `y` = depth. |
| I    | Tremor                   | `xy`  | TODO   | Square-wave volume gate. Not implemented in the engine. |
| J    | Arpeggio                 | `xy`  |        | Rotate note / note+`x` semitones / note+`y` semitones each tick. |
| K    | Vibrato + Volume Slide   | `xy`  |        | Continue last Hxy; slide volume per Dxy semantics. |
| L    | Portamento + Volume Slide| `xy`  |        | Continue last Gxx; slide volume per Dxy semantics. |
| M    | Set Channel Volume       | `xx`  |        | Set channel volume (00–40). |
| N    | Channel Volume Slide     | `xy`  |        | Channel-volume slide with Dxy encoding. |
| O    | Sample Offset            | `xx`  |        | Start the note from byte offset `xx × 100h` into the sample. |
| P    | Panning Slide            | `xy`  | TODO   | Pan slide with Dxy encoding. Not implemented. |
| Q    | Retrigger Note           | `xy`  |        | Retrigger every `y` ticks with volume function `x` (0 = copy, 1–5 slide down, 6–F slide up). |
| R    | Tremolo                  | `xy`  |        | Volume LFO. `x` = speed, `y` = depth. |
| S    | Extended                 | `xy`  |        | See sub-commands below. |
| T    | Set Tempo                | `xx`  |        | `Txx >= 20h` sets BPM `xx`. 00–0F slides tempo down, 10–1F slides up. |
| U    | Fine Vibrato             | `xy`  |        | Vibrato with 1/4 depth resolution. |
| V    | Global Volume            | `xx`  |        | Set global mix volume (00–80). |
| W    | Global Volume Slide      | `xy`  |        | Global-volume slide with Dxy encoding. |
| X    | Set Panning              | `xx`  |        | Hard panning (00 = left, 80 = center, FF = right). |
| Y    | Panbrello                | `xy`  | TODO   | Panning LFO. Not implemented. |
| Z    | MIDI / Filter            | `xx`  | TODO   | MIDI macro / resonant filter. Not implemented. |

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

While editing a pattern, moving the cursor over the effect or parameter column shows `FX <letter> <name>` on the bottom screen's hint row. The letter column itself is displayed as hex for now — nibble editing (A+arrows) operates on the raw byte.

## Adding an effect

1. Pick the next free numeric code (> 26) or claim an existing letter.
2. Add an `FX(...)` line to `arm9/source/core/effects.def`.
3. Regenerate this document from the X-list (by hand for now — the table above mirrors the def columns).
4. If the engine needs new dispatch, edit `lib/maxmod/source/core/mas.c` — that's vendor-forked territory; tread carefully.
