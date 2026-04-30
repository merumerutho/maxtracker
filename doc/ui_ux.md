# maxtracker -- UI/UX Specification

Parent: [DESIGN.md](../DESIGN.md)

---

## 1. Philosophy

The UI follows the LSDJ/M8/Piggy school of tracker design:

- **The cursor is the interface.** You move to a field, you edit it. No mode dialogs.
- **Buttons for speed, touch for space.** All pattern editing, navigation, and transport is buttons-only. Touchscreen is reserved for tasks that are genuinely spatial: drawing waveforms, shaping envelope curves, dragging mixer faders.
- **Hierarchical zoom.** Song -> Pattern overview -> Channel inside -> Field. You drill down with A / SELECT+RIGHT and back up with SELECT+LEFT. The current zoom level determines what d-pad does.
- **No menus for common operations.** Copy, paste, delete, transpose -- all are button combos. Menus exist only for infrequent operations (file save, channel count change, song settings).
- **Every pixel is data.** Minimal chrome. No decorative borders. Status information is dense and always visible.

---

## 2. Button Mapping (LSDJ / M8 Faithful)

maxtracker maps NDS buttons to match LSDJ and M8 Tracker conventions as closely
as possible. The NDS has more buttons than Game Boy (L, R, X, Y), so we use the
extras as ergonomic modifiers without breaking the LSDJ muscle-memory core.

### 2.1 Button Roles

| NDS Button | LSDJ Equivalent | Role |
|------------|----------------|------|
| **SELECT** | **SELECT** | **SHIFT** -- primary modifier. Screen switch, copy, paste. |
| **A** | **A** | Place / confirm / edit value (A+d-pad) |
| **B** | **B** | Modifier for warp (B+d-pad) and delete (B+A). B alone does nothing. |
| **START** | **START** | Play / stop transport |
| **D-pad** | **D-pad** | Navigate (context-sensitive per screen & column) |
| **L** | *(no equiv)* | Channel/group scroll left, or prev item |
| **R** | *(no equiv)* | Channel/group scroll right, or next item |
| **X** | *(no equiv)* | **ALT modifier** -- page jump, step size |
| **Y** | *(no equiv)* | **OPT modifier** -- octave, instrument select |

The SHIFT button is configurable at compile time via `MT_SHIFT_KEY` (default:
`KEY_SELECT`). It can be reassigned to X, Y, or R if preferred.

### 2.2 SHIFT Combos (LSDJ "rooms-in-a-house" model)

Navigation follows LSDJ's "rooms in a house" model. SELECT+LEFT/RIGHT move
through a connected horizontal chain, each step going deeper with context:

```
Song ← → Pattern Overview ← → Inside Column ← → Instrument ← → Sample
```

Each RIGHT step carries context forward:
- Song -> Pattern: enters the pattern at the cursor's order position
- Overview -> Inside: enters the highlighted channel
- Inside -> Instrument: opens the instrument at the cursor row (scans up for last set instrument)
- Instrument -> Sample: opens the sample assigned to that instrument

Each LEFT step reverses exactly.

UP/DOWN are reversible vertical jumps (UP goes to Mixer, DOWN returns to
wherever UP came from). The Disk screen is **no longer** opened by a
SHIFT+START chord; see §11 for the v1 entry path (on-screen LOAD/SAVE
buttons in PROJECT and SAMPLE views).

| Combo | Action | Notes |
|-------|--------|-------|
| SHIFT + LEFT | **Navigate back** one step | Sample->Inst->Inside->Overview->Song |
| SHIFT + RIGHT | **Navigate deeper** with context | Song->Overview->Inside->Inst->Sample |
| SHIFT + UP | Jump to **Mixer** screen | Remembers return screen for DOWN |
| SHIFT + DOWN | **Return** from Mixer/Disk | Goes back to opener (Disk uses `disk_return_screen`) |
| SHIFT + START | Start **song playback** from current order position | All screens except Disk (which has no playback shortcut) |
| SHIFT + B | **Enter selection mode** | Like M8 SHIFT+OPTION (Pattern view only) |
| SHIFT + A | **Paste** clipboard at cursor | Like M8 SHIFT+EDIT |

### 2.3 Pattern Screen -- Inside Mode (LSDJ A-held editing)

A is always "do something" (place, adjust). B is always "clear/delete" (never
navigation). There is no "edit mode" toggle; holding A while pressing d-pad
adjusts values live. The action happens on A **release**: if d-pad was used
while A was held, it was an edit (no stamp). If A was tapped without d-pad,
it stamps.

Note placement does **not** auto-advance the cursor (LSDJ style). Use d-pad
to move to the next row manually.

**Note column (column 0):**

| Combo | Action |
|-------|--------|
| **A** (tap) | Place note (octave*12 + semitone) + instrument at cursor (no advance) |
| **B+A** | Delete entire cell (clear note, inst, vol, fx, param) |
| **START** | Note-off (^^^) at cursor |

**Hex columns (columns 1-4: inst, vol, fx, param):**

| Combo | Action |
|-------|--------|
| **A held + RIGHT** | Increment low nibble |
| **A held + LEFT** | Decrement low nibble |
| **A held + UP** | Increment high nibble |
| **A held + DOWN** | Decrement high nibble |
| **A** (tap, col 1) | Stamp current instrument (no advance) |
| **A** (tap, cols 2-4) | No action (use A+d-pad to edit values) |
| **B+A** | Delete entire cell |

**Navigation (no A held):**

| Combo | Action |
|-------|--------|
| **D-pad UP/DOWN** | Move cursor row (by step size) |
| **D-pad LEFT/RIGHT** | Move between sub-columns (0-4) |
| **L / R** | Prev / next channel (stay in inside mode) |
| **X + UP/DOWN** | Page up/down (16 rows) |
| **X + LEFT/RIGHT** | Change edit step size (1/2/4/8/16) |
| **Y + UP/DOWN** | Change octave |
| **Y + LEFT/RIGHT** | Change current instrument |

### 2.4 Pattern Screen -- Overview Mode (like LSDJ Song/Chain navigation)

| Combo | Action |
|-------|--------|
| **SELECT+RIGHT** | Enter inside mode on selected channel |
| **D-pad UP/DOWN** | Move cursor row (by step size) |
| **D-pad LEFT/RIGHT** | Move cursor between channels |
| **L / R** | Switch channel group (1-8 -> 9-16 -> ...) |
| **X + UP/DOWN** | Page up/down |
| **X + LEFT/RIGHT** | Change step size |
| **Y + UP/DOWN** | Change octave |
| **Y + LEFT/RIGHT** | Change instrument |
| **START** | Play / stop |

### 2.5 Song Screen (Order Table -- like LSDJ Song screen)

| Combo | Action |
|-------|--------|
| **D-pad UP/DOWN** | Navigate order positions |
| **D-pad LEFT/RIGHT** | Change pattern number at cursor |
| **A** | Jump to pattern editor at selected position (same as SELECT+RIGHT) |
| **Y + A** | Insert order entry (duplicate current) |
| **Y + B** | Delete order entry |
| **L / R** | Quick order skip (+/-8 positions) |

---

## 3. Screen Assignment

**Top screen (256x192, non-touch): Primary workspace.**
Always shows the main editing view for the current screen mode. This is where your eyes stay 90% of the time. Rendered as a tile-based text console using a small fixed-width font (4x6 pixels allows 64 columns x 32 rows, or 6x8 for 42 columns x 24 rows).

**Bottom screen (256x192, touchscreen): Context panel.**
Changes based on current screen mode. Shows supplementary information and touch-interactive tools. Can be a bitmap (for waveform drawing), tile-based text (for lists), or a hybrid.

---

## 4. Screen Modes

Seven screen modes. SHIFT+LEFT/RIGHT navigate the depth hierarchy
(Song <-> Pattern Overview <-> Inside Column <-> Instrument <-> Sample).
SHIFT+UP/DOWN navigate a vertical axis. B is never used for navigation
between screens -- only SELECT+direction.

```
  Depth navigation (SHIFT+LEFT / SHIFT+RIGHT):

    Song ←→ Pattern Overview ←→ Inside Column ←→ Instrument ←→ Sample

  Vertical navigation (SHIFT+UP / SHIFT+DOWN):

    From Song:     UP → Project,  DOWN returns from Project
    From Pattern:  UP → Mixer,    DOWN returns from Mixer
    From Project:  >> LOAD / >> SAVE / >> SAVE AS rows open the Disk
    From Sample:   >> Load .wav / >> Save .wav rows open the Disk
    SHIFT+DOWN from Disk returns to whichever view opened it
    (tracked in the global `disk_return_screen`).

  Navigation map:

                    Project ── (LOAD / SAVE / SAVE AS) ──┐
                       ↕ SEL+UP/DOWN                     │
    Song ←→ Pattern Overview ←→ Inside ←→ Instrument ←→ Sample
         SEL+L/R            SEL+L/R     SEL+L/R       SEL+L/R   │
                       ↕ SEL+UP/DOWN                            │
                     Mixer                                      │
                                                                ↓
                    Disk (returns to opener with SEL+DOWN or B-at-root)
```

| Mode | Top Screen | Bottom Screen |
|------|-----------|---------------|
| **Pattern** | Pattern grid editor | Quick info: current instrument params, channel mute toggles, mini song position |
| **Instrument** | Instrument parameter list (all numeric fields) | Envelope editor (touchscreen: tap/drag nodes to shape curves) |
| **Sample** | Sample waveform display (zoomable) | Waveform drawing canvas (touchscreen: draw with stylus) + loop point adjustment |
| **Song** | Order table (arrangement view) | Pattern operations: clone, insert, delete, swap |
| **Mixer** | Channel activity display (levels, notes playing) | Fader strips: touch-drag per-channel volume and panning |
| **Project** | Song-level settings (tempo, speed, volume, channels) | Song statistics summary |
| **Disk** | File browser (d-pad navigation, A to select) | File info / confirmation prompts |

---

## 5. Pattern Screen (Main Editing View)

The pattern screen uses a **hybrid classic-tracker / LSDJ** approach with two
distinct viewing modes:

- **Overview mode** -- classic tracker look. Shows 8 channels at once with a
  compact note+instrument preview per channel. You can **navigate** (move the
  cursor to any row/channel) but **cannot directly edit** cell data. This gives
  you the big-picture view of what is happening across all channels.
- **Inside mode** -- LSDJ-style focused editing. Zooms into a single channel,
  showing all 5 fields (Note, Inst, Vol, Fx, Param) with room for comfortable
  editing. All data entry happens here. Press **A** to enter, **B** to exit.

This hybrid lets you keep the spatial awareness of a classic tracker while
maintaining the precise, uncluttered editing of an LSDJ-style workflow.

### 4.1 Overview Mode (navigation / preview -- default)

Shows 8 channels at a time. Each channel shows note + instrument. **Read-only**:
cursor movement only, no data entry. Use this to find where you want to edit,
then press A to zoom into the selected channel.

```
                maxtracker v0.1     Pat:03 Spd:06 BPM:125
   ┌────┬────────┬────────┬────────┬────────┬────────┬────────┬────────┬────────┐
   │ RW │  CH 01 │  CH 02 │  CH 03 │  CH 04 │  CH 05 │  CH 06 │  CH 07 │  CH 08 │
   ├────┼────────┼────────┼────────┼────────┼────────┼────────┼────────┼────────┤
   │ 00 │ C-4 01 │ --- -- │ --- -- │ --- -- │ --- -- │ --- -- │ --- -- │ --- -- │
   │ 01 │ --- -- │ D-5 03 │ --- -- │ --- -- │ --- -- │ --- -- │ --- -- │ --- -- │
   │ 02 │ E-4 01 │ --- -- │ G-3 02 │ --- -- │ --- -- │ --- -- │ --- -- │ --- -- │
   │ 03 │ --- -- │ --- -- │ --- -- │ A-2 04 │ --- -- │ --- -- │ --- -- │ --- -- │
   │>04 │ F-4 01 │ C-5 03 │ --- -- │ --- -- │ --- -- │ E-3 01 │ --- -- │ --- -- │
   │ 05 │ --- -- │ --- -- │ --- -- │ --- -- │ --- -- │ --- -- │ --- -- │ --- -- │
   │ 06 │ G-4 01 │ --- -- │ B-3 02 │ --- -- │ --- -- │ --- -- │ --- -- │ --- -- │
   │ .. │        │        │        │        │        │        │        │        │
   └────┴────────┴────────┴────────┴────────┴────────┴────────┴────────┴────────┘
    Step:1  Note:C-4  Ins:01  Oct:4                                      [1-8]
```

- **Row numbers** in hex (00-FF). Every 4th row has a brighter background (beat marker).
- **Cursor** highlights the selected row + channel. The cursor row is marked with `>`.
- **Active group** shown in footer: `[1-8]`, `[9-16]`, etc.
- **Footer** shows step size, current note/octave/instrument for quick reference.

Navigation:
- D-pad up/down: move cursor row (by step size)
- D-pad left/right: move cursor between channels in the group
- L/R: switch channel group (1-8 -> 9-16 -> ... -> 25-32 -> 1-8)
- X + d-pad up/down: jump by 16 rows (page)
- X + d-pad left/right: change step size (1, 2, 4, 8, 16)
- Y + d-pad up/down: change octave
- Y + d-pad left/right: change instrument
- **A: enter Inside mode** on the selected channel

### 4.2 Inside Mode (editing -- LSDJ-style)

Press A on a channel to zoom in. Shows all fields of one channel. **This is
where all data entry happens.** The cursor moves between sub-columns (Note, Ins,
Vol, Eff, Prm) and the d-pad behavior changes based on which column is active.

```
                maxtracker v0.1     Pat:03 CH:02  BPM:125
   ┌────┬──────┬─────┬─────┬─────┬──────┐
   │ RW │ Note │ Ins │ Vol │ Eff │ Prm  │
   ├────┼──────┼─────┼─────┼─────┼──────┤
   │ 00 │ C-4  │  01 │  -- │  -- │  --  │
   │ 01 │ ---  │  -- │  -- │  -- │  --  │
   │ 02 │ E-4  │  01 │  40 │  -- │  --  │
   │ 03 │ ---  │  -- │  -- │  0F │  20  │
   │>04 │ F-4  │  01 │  -- │  -- │  --  │  <-- cursor on Note column
   │ 05 │ ---  │  -- │  -- │  -- │  --  │
   │ .. │      │     │     │     │      │
   └────┴──────┴─────┴─────┴─────┴──────┘
    Oct:4  Ins:01  Col:Note  Step:1   CH:02
```

Navigation:
- D-pad up/down: move cursor row (by step size)
- D-pad left/right: move between sub-columns (Note, Ins, Vol, Eff, Prm)
- **SELECT+LEFT: exit to Overview mode** (keeping current row/channel position)
- L/R: switch to adjacent channel's inside view (stay zoomed in)
- X + d-pad: page movement / step change (same as overview)
- Y + d-pad: octave / instrument change (same as overview)

### 4.3 Data Entry (Inside Mode only -- A-held editing)

There is no "enter edit mode" step. A is always an action key.

**Note column (column 0):**
- **A** (tap): Stamp the current note (cursor.octave * 12 + cursor.semitone)
  into the cell and advance cursor down by step size. The current instrument
  is also written to the cell's inst field.
- **START**: Write a note-off (^^^) into the cell and advance.
- **B on a filled cell**: Delete (set cell to empty).
- **B on an empty cell**: No action.
- **B+A**: Delete entire cell (all fields).
- The current note preview is shown in the footer. Change it with Y+d-pad
  (up/down = octave, left/right = instrument).

**Instrument column (column 1):**
- **A** (tap): Stamp the current instrument number. Advance cursor.
- **A held + UP/DOWN**: Increment/decrement high nibble of inst value.
- **A held + LEFT/RIGHT**: Increment/decrement low nibble of inst value.
- **B**: Clear instrument field.

**Volume / Effect / Param columns (columns 2-4):**
- **A held + UP/DOWN**: Increment/decrement high nibble.
- **A held + LEFT/RIGHT**: Increment/decrement low nibble.
  Releasing A finalizes the edit; no confirm step needed.
- **A** (tap, no d-pad): Advance cursor by step size.
- **B**: Clear the field (set to 0).

### 4.5 Editing Operations

| Combo | Action | Context |
|-------|--------|---------|
| A | Place note/value | On any column |
| B | Delete cell (set to empty) | On any column |
| SELECT + B | Enter selection mode | Anchor at cursor, extend with d-pad |
| B (in selection) | Copy | Copies selection, exits selection mode |
| SELECT + A | Paste | Stamps clipboard at cursor |
| A + B (in selection) | Cut | Copies selection, clears it, exits |
| Y + A | Insert row (shift down) | Pattern screen |
| Y + B | Delete row (shift up) | Pattern screen |
| X + A | Preview note (play without writing) | Pattern screen |
| Y + d-pad up/down | Change current instrument | Anywhere |
| X + d-pad up/down | Page up/down (16 rows) | Pattern screen |
| X + d-pad left/right | Change edit step | Pattern screen |
| L + R | Toggle follow mode (cursor follows playback) | Pattern screen |

### 4.6 Block Selection

Hold SELECT and move d-pad to define a rectangular selection (like LSDJ's mark mode). The selected block is highlighted. Then:
- SELECT + A: copy block
- SELECT + B: paste block
- B: clear block (delete all cells in selection)
- Y + d-pad up/down: transpose block (shift all notes by semitone)
- X + Y: transpose block by octave

---

## 6. Instrument Screen

### 6.1 Top Screen: Parameter List

```
   Instrument 01                    [01/24]
   ────────────────────────────────────────
   Global Volume .............. 128
   Fadeout ..................... 004
   Panning .................... 128
   New Note Action ............ Cut
   Envelope: Volume ........... ON
   Envelope: Panning .......... OFF
   Envelope: Pitch ............ OFF
   Note Map ................... Single (Smp 01)
   Sample ..................... 01 "kick"
   ────────────────────────────────────────
   A:Edit  B+A:Reset  L/R:Prev/Next  SEL+L:back
```

D-pad up/down to highlight a parameter. Hold A + LEFT/RIGHT to adjust the
parameter value (no edit mode toggle). D-pad LEFT/RIGHT without A does quick
+/-1 adjustment. L/R to switch instruments. SELECT+LEFT to return to pattern.

### 6.2 Bottom Screen: Envelope Editor (Touch)

When an envelope parameter is selected:

```
   Volume Envelope (6 nodes)    Loop: 2-4  Sus: --
   ┌──────────────────────────────────────────────┐
   │    *                                          │  64
   │   / \     *─────*                             │
   │  /   \   /       \                            │
   │ *     \ /         *                           │
   │        *                              *       │  0
   └──────────────────────────────────────────────┘
   Tick: 0          32          64          96
   [Tap to add node] [Drag to move] [Double-tap to delete]
```

- Touch and drag nodes to reshape the envelope
- Tap empty space to add a node
- Double-tap a node to delete it
- Loop/sustain points shown as vertical markers (drag-able)
- The top screen shows numeric node values updating in real-time

---

## 7. Sample Screen

### 7.1 Top Screen: Waveform + Action Rows

```
   Sample 01 "kick"  8-bit  Len:4096  Loop:1024-3072
   ┌──────────────────────────────────────────────┐
   │         ╱╲                                    │
   │       ╱    ╲         ╱╲                       │
   │─────╱───────╲──────╱──╲──────────────────────│
   │                ╲  ╱    ╲                      │
   │                 ╲╱      ╲____                 │
   └──────────────────────────────────────────────┘
   Parameters
     Volume:   64
     Panning: 128
     Rate:   22050 Hz

     >> Load .wav
     >> Save .wav
     >> Rename
   L/R:prev/next  UP/DN:action  A:trigger  X:zoom
```

The action rows below the parameter list are navigated with **UP/DOWN** and
triggered with **A**. They are anchored to the footer so they don't collide
with the help bar in BIG font mode (see `feedback_scaled_offset_drift.md`).

| Row | Behavior | Confirm? |
|-----|----------|----------|
| `>> Load .wav` | Opens disk browser scoped to this sample slot; selecting a `.wav` populates `song.samples[selected]` and returns to SAMPLE | Two-tap if slot is non-empty |
| `>> Save .wav` | Writes `./data/sample_XX.wav` (XX = 1-based slot). 8-bit samples are upconverted to 16-bit on the fly via `wav_save_mono16` | Two-tap if slot non-empty (overwrite warning) |
| `>> Rename` | Opens the on-screen QWERTY keyboard (§13) on `s->name` (32 chars max). Cancel restores prior name. | n/a |

**B+A** (two-tap) deletes the selected sample slot: frees PCM, resets all
fields, and (if playing) calls `playback_rebuild_mas()` to refresh the
audio engine. Without the two-tap, a stray B+A could free PCM that
playback or the LFE draft is still dereferencing.

### 7.2 Bottom Screen: Waveform Drawing (Touch)

For drawn samples (single-cycle or short waveforms):

```
   Draw Sample  Length: 256  [Sine] [Saw] [Square] [Noise]
   ┌──────────────────────────────────────────────┐
   │                                               │
   │           ████                                │
   │         ██    ██                              │
   │       ██        ██                     ██████ │
   │─────██────────────██───────────────████───────│
   │   ██                ██           ██           │
   │ ██                    ██       ██             │
   │█                        ██████                │
   └──────────────────────────────────────────────┘
   Draw with stylus. Preset buttons at top. A:Confirm B:Cancel
```

- Stylus draws directly onto the waveform
- Preset buttons seed basic shapes (sine, saw, square, noise)
- The drawn waveform becomes a sample in the sample pool
- Can be saved to SD card as a raw file for reuse

For loaded samples (WAV from SD):
- Bottom screen shows loop point adjustment: drag L/R markers on a zoomed waveform region
- Touch to set the loop start/end visually

---

## 8. Song Screen

### 8.1 Top Screen: Order Table

```
   Song Arrangement           Length: 12  Repeat: 00
   ────────────────────────────────────────────────
   Pos │ Pattern │ Info
   ────┼─────────┼─────────────────────────────────
    00 │   00    │ 64 rows
    01 │   01    │ 64 rows
    02 │   02    │ 64 rows
   >03 │   01    │ 64 rows  ◄ cursor
    04 │   03    │ 128 rows
    05 │   00    │ 64 rows
    .. │   ..    │
   ────┴─────────┴─────────────────────────────────
   A:goto pat  B+A:del  Y+A:Insert  Y+B:Delete  SEL+L:back
```

- D-pad up/down: move in order list
- D-pad left/right: change pattern number at current position
- A: jump to pattern editor for the selected pattern
- Y+A: insert order entry
- Y+B: delete order entry

### 8.2 Bottom Screen: Pattern Operations

```
   Pattern Operations
   ────────────────────
   [Clone Pattern]   Create copy of current pattern
   [New Empty]       Insert new empty pattern
   [Delete Pattern]  Remove pattern (if unused in orders)
   [Swap]            Swap two pattern numbers
   ────────────────────
   Patterns: 12/256 used    Memory: 120KB/640KB
```

Touch buttons for pattern management. Shows memory usage.

---

## 9. Project Screen (SCREEN_PROJECT)

The Project screen provides song-level settings. It is reached from the Song
screen via SELECT+UP and returns via SELECT+DOWN or SELECT+LEFT.

### 9.1 Purpose

Edit global song parameters: song name, tempo (BPM), speed (ticks/row), master
volume, channel count, repeat position, frequency mode, and XM compatibility
mode. Also shows read-only statistics (instrument/sample/pattern counts) and
provides actions (New Song, Compact Song).

### 9.2 Navigation

- **SELECT+UP** from Song screen enters Project
- **SELECT+DOWN** or **SELECT+LEFT** returns to Song screen

### 9.3 Controls

| Combo | Action |
|-------|--------|
| **D-pad UP/DOWN** | Navigate parameter rows |
| **A held + LEFT/RIGHT** | Adjust selected value (+-1) |
| **A held + UP/DOWN** | Adjust by large step (+-10 for tempo/vol, +-5 for speed) |
| **A** (on action row) | First press = request confirm, second press = execute |
| **B** | Cancel pending confirmation |

### 9.4 Actions

The action rows live below the song-stat separator and behave as on-screen
buttons. UP/DOWN navigate to them; **A** triggers the highlighted row.

| Row | Behavior | Confirm? |
|-----|----------|----------|
| `>> New` | Clears all song data and re-initializes | Two-tap (A=yes, B=no) |
| `>> Load` | Opens the disk browser; selecting a `.mas` loads it | Two-tap if the current song is modified |
| `>> Save` | Writes the current song to `./data/song.mas` | None (overwrites silently) |
| `>> Save As` | Writes to the next free `./data/song_NN.mas` slot (01-99) | None |
| `>> Compact Song` | Frees patterns not referenced in the order list | Two-tap |

The `>> Load` action sets `disk_return_screen = SCREEN_PROJECT` so cancelling
the browser (B-at-root or SHIFT+DOWN) returns here. The **Song Name** row is
special: pressing **A** on it opens the on-screen QWERTY keyboard
(see §13). The keyboard owns both screens until OK / CANCEL.

### 9.5 B/A semantics across screens

The B+A chord is reserved for **destructive actions on persistent state**.
All such actions require a two-tap confirm; transient editing operations
(cell cut/clear in the pattern editor) intentionally remain single-tap to
preserve editing flow.

| Screen | B+A | Confirm? | Notes |
|--------|-----|----------|-------|
| Pattern (inside) | Cut / clear cell | None | High-frequency editing op |
| Song | Delete order entry at cursor | None | Recoverable via undo |
| Instrument | Reset instrument to defaults | **Two-tap** | Frees envelope nodes |
| Sample | Delete sample slot (frees PCM) | **Two-tap** | PCM may be in use by playback / LFE |

B alone clears only the current field (pattern inside mode) or cancels any
pending confirm. B never navigates between screens.

---

## 10. Mixer Screen

### 10.1 Top Screen: Channel Activity

```
   Mixer                                  BPM:125
   CH│Vol│Pan│ Note │ Activity
   ──┼───┼───┼──────┼──────────────────────────────
   01│ 64│ C │ C-4  │ ████████████░░░░░░░░
   02│ 64│ C │ D-5  │ ██████░░░░░░░░░░░░░░
   03│ 48│ L │ ---  │ ░░░░░░░░░░░░░░░░░░░░
   04│ 64│ R │ A-2  │ ████████████████████
   05│ 00│ C │ ---  │ [MUTE]
   ..│   │   │      │
```

Real-time display of what each channel is doing during playback.

### 10.2 Bottom Screen: Fader Strips (Touch)

```
   01  02  03  04  05  06  07  08
   ┌──┐┌──┐┌──┐┌──┐┌──┐┌──┐┌──┐┌──┐
   │  ││  ││  ││  ││  ││  ││  ││  │
   │██││██││  ││██││  ││  ││  ││  │
   │██││██││██││██││  ││  ││  ││  │
   │██││██││██││██││  ││  ││  ││  │
   │██││██││██││██││  ││██││  ││  │
   └──┘└──┘└──┘└──┘└──┘└──┘└──┘└──┘
    64   64  48   64  00   32  00  00
   [S] [S] [ ] [S] [M] [ ] [ ] [ ]
   S=Solo  M=Mute    Drag faders to adjust
```

Touch-drag faders for volume. Tap S/M buttons for solo/mute. L/R to show next 8 channels.

---

## 11. Disk Screen

The disk screen is **never** opened directly by the user; there is no
SHIFT+START shortcut. It is opened by on-screen action rows in other views,
each of which sets the global `disk_return_screen` (and, for SAMPLE,
`sample_load_target`) before transitioning. Exits return to whatever the
opener set.

### 11.1 Entry points

| Opener | Row triggered | `disk_return_screen` | `sample_load_target` |
|--------|---------------|----------------------|----------------------|
| PROJECT view `>> Load` | A on Load row | `SCREEN_PROJECT` | 0 (legacy path) |
| PROJECT view `>> Save` / `>> Save As` | A on Save / Save As | n/a (writes directly) | n/a |
| SAMPLE view `>> Load .wav` | A on Load row | `SCREEN_SAMPLE` | sv.selected + 1 |
| SAMPLE view `>> Save .wav` | A on Save row | n/a (writes directly) | n/a |

The `sample_load_target` flag is the routing hint that tells the disk
screen's `.wav` branch which sample slot to populate. When non-zero, the
load goes to `song.samples[sample_load_target - 1]` and the screen returns
to SCREEN_SAMPLE on success. When zero, the legacy path uses
`cursor.instrument` instead.

### 11.2 Top Screen: File Browser

```
   FILE BROWSER                    ./data/
   ────────────────────────────────────────────────
    [songs/]
    [samples/]
   > mysong.mas           12.4 KB
     another.mas           8.2 KB
     test.mas             45.1 KB
   ────────────────────────────────────────────────
```

D-pad navigation. **A** enters a directory or selects a file (load).
**B** at the root exits to `disk_return_screen`. **B** elsewhere walks up
one directory level. **SHIFT+DOWN** also exits.

### 11.3 Bottom Screen

Shows status messages from the most recent load/save operation. Save and
Save-As are now performed from the PROJECT view (see §9.4), so the disk
screen is effectively read-only for the user; file *writes* never happen
from inside it in v1.

### 11.4 Edge cases

- If the configured root directory is empty, B is clamped to keep the
  user inside it (an earlier bug let B walk above the root and lock the
  user out of the SHIFT+DOWN exit).
- The browser can show up to `FB_MAX_ENTRIES` entries per directory.
  Excess entries are silently dropped (known limitation, not documented
  to the user yet).

---

## 12. Font Requirements

The pattern grid needs to be information-dense. Font size options:

| Font | Columns x Rows | Pro | Con |
|------|---------------|-----|-----|
| 4x6 | 64 x 32 | Maximum density | Hard to read on real hardware |
| 5x8 | 51 x 24 | Good balance | Slightly tight for 4-channel overview |
| 6x8 | 42 x 24 | Comfortable reading | Fewer columns, may need scrolling |

**Recommendation: 6x8 for text, 4x6 for the pattern grid numbers only.** The pattern grid uses a custom narrow font; status bars and menus use a standard readable font.

The pattern grid with 4 channels in overview needs: 4 chars (row) + 4 * (7 chars per channel) + separators = ~36 characters. At 6x8, that's 216 pixels -- fits in 256px width.

---

## 13. Text Input Modal (`text_input` widget)

A reusable on-screen QWERTY keyboard for any string entry. Implemented at
`arm9/source/ui/text_input.{h,c}`. Currently wired to:

- PROJECT view -> Song Name row (A opens it on `song.name`)
- SAMPLE view -> `>> Rename` action (A opens it on `s->name`)

### 13.1 Behavior

While the keyboard is active (`text_input_is_active()` returns true) it
**owns both screens**. The opening view must forward all input frames to
`text_input_input(down, held)` and all draws to `text_input_draw(top, bot)`,
returning early from its own handlers, until the keyboard closes itself.

### 13.2 Layout

```
   Top screen
   ────────────────────────────────────────
    Rename Sample
   > kick_drum_01_                       ← caret
     14 / 32 chars

   Bottom screen
   ────────────────────────────────────────
    1  2  3  4  5  6  7  8  9  0
    Q  W  E  R  T  Y  U  I  O  P
    A  S  D  F  G  H  J  K  L  -
    Z  X  C  V  B  N  M  .  _  /
   [DEL ][SPACE ][CANCEL][ OK ]
   A:press  B:del  START:ok  SELECT:cancel
```

Uppercase only in v1; lowercase / shift is on the post-v1 list.

### 13.3 Controls

| Key | Action |
|-----|--------|
| D-pad | Move cursor between keys (wraps within row, no vertical wrap) |
| A | Press the highlighted key (append char, or run special action) |
| B | Quick backspace (shortcut for the DEL key) |
| START | OK -- close, keep edits |
| SELECT | CANCEL -- close, restore the snapshot taken at open time |

Single instance only: `text_input_open()` is a no-op if a keyboard is
already active. The caller's buffer must be writable for `max_len + 1`
bytes; the widget snapshots up to 64 bytes internally for CANCEL restore.

---

## 14. Color Palette

Minimal, high-contrast palette suitable for DS LCD:

| Index | Color | Usage |
|-------|-------|-------|
| 0 | Black (#000000) | Background |
| 1 | Dark gray (#404040) | Inactive text, separators |
| 2 | Light gray (#A0A0A0) | Normal text |
| 3 | White (#FFFFFF) | Highlighted/cursor text |
| 4 | Green (#00FF00) | Active/playing row |
| 5 | Cyan (#00FFFF) | Note values |
| 6 | Yellow (#FFFF00) | Instrument numbers |
| 7 | Blue (#4080FF) | Effect commands |
| 8 | Magenta (#FF40FF) | Effect parameters |
| 9 | Red (#FF0000) | Warnings, muted channels |
| 10 | Dark green (#004000) | Every-4th-row highlight |
| 11 | Orange (#FF8000) | Selection/block highlight |
