# maxtracker -- Screen Layout Specification

Parent: [DESIGN.md](../DESIGN.md)

---

## 1. Rendering Approach

Both screens use **MODE_5_2D** with an **8bpp bitmap** background layer (BG2, `BgType_Bmp8`, `BgSize_B8_256x256`). All rendering is software-drawn to the bitmap framebuffer using a custom pixel font. No tile-based console is used.

This gives complete per-pixel control, allowing:
- Custom font sizes (4x6 primary, not limited to 8x8 tile grid)
- Colored text with per-character foreground color
- Waveform/envelope drawing on the same surface as text
- Row highlighting with background color fills

### 1.1 VRAM Configuration

```
Main engine (top screen):
  VRAM_A -> MAIN_BG (128KB)
  BG2: 8bpp bitmap, 256x256 (64KB used, 256x192 visible)

Sub engine (bottom screen):
  VRAM_C -> SUB_BG (128KB)
  BG2: 8bpp bitmap, 256x256 (64KB used, 256x192 visible)
```

### 1.2 Palette (shared by both screens, 256 entries)

```
Index  Color            Usage
─────  ───────────────  ──────────────────────────────
  0    #000000 Black    Background
  1    #1a1a2e Dark     Row background (even rows)
  2    #16213e Darker   Row background (beat rows: every 4th)
  3    #0f3460 Blue-dk  Row background (cursor row)
  4    #404040 DimGray  Separator lines, inactive text
  5    #808080 Gray     Row numbers, secondary text
  6    #c0c0c0 LtGray   Normal text, parameter values
  7    #ffffff White    Cursor text, highlighted values
  8    #00e0a0 Cyan     Note values (C-4, D#5, etc.)
  9    #f0e040 Yellow   Instrument numbers
 10    #6090ff Blue     Effect commands
 11    #e080ff Magenta  Effect parameters
 12    #40ff40 Green    Playing row indicator
 13    #ff4040 Red      Muted channel, warnings
 14    #ff8000 Orange   Selection/block highlight
 15    #204020 DkGreen  Playing row background
 16    #302030 DkPurp   Header bar background
 17-31 (reserved for UI elements)
 32-63 (waveform gradient: dark to bright green)
```

---

## 2. Font Design

### 2.1 Primary Font: 4x6

A custom monospaced bitmap font, 4 pixels wide by 6 pixels tall. Each glyph fits in a 4x6 pixel cell with no inter-character spacing (spacing is built into the glyph -- rightmost column is typically blank for separation).

Character set: ASCII 0x20-0x7F (96 glyphs). Stored as a 1-bit bitmap (96 * 4 * 6 = 2304 bits = 288 bytes).

```
Grid capacity: 256/4 = 64 columns, 192/6 = 32 rows
Total: 64 x 32 = 2048 character cells
```

The 4x6 font covers digits, uppercase letters, common symbols (`-`, `#`, `.`, `/`, `|`, `=`). Lowercase letters are mapped to uppercase (tracker tradition).

### 2.2 Glyph Examples (4x6)

```
'A'     '0'     'C'     '-'     '#'     ' '
.##.    .##.    .##.    ....    .#.#    ....
#..#    #..#    #...    ....    ####    ....
####    #..#    #...    ####    .#.#    ....
#..#    #..#    #...    ....    ####    ....
#..#    .##.    .##.    ....    .#.#    ....
....    ....    ....    ....    ....    ....
```

### 2.3 Rendering Function

```c
// Draw a single character at pixel position (px, py) with palette color index
void mt_putc(u8 *framebuffer, int px, int py, char ch, u8 color);

// Draw a string starting at character grid position (col, row)
void mt_puts(u8 *framebuffer, int col, int row, const char *str, u8 color);

// Draw a string with per-character color array
void mt_puts_colored(u8 *framebuffer, int col, int row,
                     const char *str, const u8 *colors, int len);

// Fill a character-cell rectangle with a background color
void mt_fill_row(u8 *framebuffer, int row, int col_start, int col_end, u8 bg_color);
```

Each `mt_putc` call writes 4*6 = 24 pixels. A full screen redraw (2048 chars) writes 49,152 pixels. At ~1 cycle per pixel write (ARM9 cached), this takes ~0.7ms. Full screen redraws are fast enough for 60fps.

In practice, only changed rows need redrawing. The pattern grid scrolls by row, so typically 1-3 rows change per frame during playback.

---

## 3. Top Screen Layout: Pattern Editor

### 3.1 Overview Mode (8 channels, navigation / preview only)

No data entry in this mode — it provides the classic multi-channel tracker view
for spatial awareness. Press A to zoom into the selected channel for editing.

```
256 pixels wide, 192 pixels tall
64 columns x 32 rows (4x6 font)

Row 0 (y=0..5):    Status bar
Row 1 (y=6..11):   Column headers
Row 2-29 (y=12..179): Pattern data (28 visible rows)
Row 30 (y=180..185):  Footer / edit info
Row 31 (y=186..191):  Transport bar

Column layout (64 chars):
Col 0-2:   Row number (hex, 2 digits + space)
Col 3:     Separator '|'
Col 4-10:  Channel 1: NNN II (note 3 + space + inst 2 + sep)
Col 11-17: Channel 2
Col 18-24: Channel 3
Col 25-31: Channel 4
Col 32-38: Channel 5
Col 39-45: Channel 6
Col 46-52: Channel 7
Col 53-59: Channel 8
Col 60-63: Spare (scrollbar or indicators)
```

Pixel-exact:
```
  0         1         2         3         4         5         6
  0123456789012345678901234567890123456789012345678901234567890123
  ├──────────────────────────────────────────────────────────────┤
0 │maxtracker    P:03 S:06 T:125 O:4 I:01       [1-8]  ▶ 04/12 │ status
1 │RW |  CH01 |  CH02 |  CH03 |  CH04 |  CH05 |  CH06 |  CH07 |│ headers
  ├──────────────────────────────────────────────────────────────┤
2 │00 | C-4 01| --- --| --- --| --- --| --- --| --- --| --- --| │ data
3 │01 | --- --| D-5 03| --- --| --- --| --- --| --- --| --- --| │
4 │02 | E-4 01| --- --| G-3 02| --- --| --- --| --- --| --- --| │
5 │03 | --- --| --- --| --- --| A-2 04| --- --| --- --| --- --| │
6 │04>| F-4 01| C-5 03| --- --| --- --| --- --| E-3 01| --- --| │ cursor
7 │05 | --- --| --- --| --- --| --- --| --- --| --- --| --- --| │
  │...│        28 data rows visible                              │
29│1B | --- --| --- --| --- --| --- --| --- --| --- --| --- --| │
  ├──────────────────────────────────────────────────────────────┤
30│Step:01  Note:C-4  Ins:01  Vol:--  Fx:--/--   Mem:640K free  │ footer
31│STOP  Row:00  Pos:00/12  Pat:03  ----  [SELECT+dir:screens]  │ transport
  └──────────────────────────────────────────────────────────────┘
```

28 visible pattern rows in the data area. With the cursor centered, you see 14 rows above and 13 below. For a 64-row pattern, this shows nearly half the pattern at once.

### 3.2 Inside Mode (single channel, all editing)

Press A on a channel to zoom in. **All data entry happens in this mode.**
The layout shows all 5 fields with wide spacing for comfortable editing.
The cursor highlights the active sub-column; d-pad behavior is column-aware.

```
  0         1         2         3         4         5         6
  0123456789012345678901234567890123456789012345678901234567890123
  ├──────────────────────────────────────────────────────────────┤
0 │maxtracker    P:03 CH:02   T:125 O:4 I:01              ▶ 04 │ status
1 │RW | Note  | Ins | Vol | Eff | Prm |                        │ headers
  ├──────────────────────────────────────────────────────────────┤
2 │00 | C-4   |  01 |  -- |  -- |  -- |   Instrument 01        │
3 │01 | D-4   |  01 |  -- |  -- |  -- |   "Bass Drum"          │
4 │02 | E-4   |  01 |  40 |  -- |  -- |   Sample: 01           │
5 │03 | ---   |  -- |  -- |  0F |  20 |   Vol: 64  Pan: C      │
6 │04>| F-4   |  01 |  -- |  -- |  -- |   Env: Vol ON          │
7 │05 | ---   |  -- |  -- |  -- |  -- |                        │
  │...│                                                         │
  ├──────────────────────────────────────────────────────────────┤
30│Col:Note  Step:01  CH:02/08  [L/R:prev/next ch] [SEL+L:back]     │
31│STOP  Row:04  Pos:00/12  Pat:03                              │
  └──────────────────────────────────────────────────────────────┘
```

The right side (columns 35-63) shows a **context sidebar** with the current instrument's summary info. This eliminates the need to switch screens just to check what instrument you're editing.

Field columns in inside mode:
```
Col 0-2:   Row number
Col 3:     Separator
Col 4-9:   Note (C-4 + padding, or === for cut, ^^^ for off)
Col 10:    Separator
Col 11-14: Instrument (space + 2 hex digits + space)
Col 15:    Separator
Col 16-19: Volume column
Col 20:    Separator
Col 21-24: Effect
Col 25:    Separator
Col 26-29: Parameter
Col 30:    Separator
Col 31-63: Context sidebar (33 chars = instrument info)
```

### 3.3 Song Screen (Order Table)

```
  0         1         2         3         4         5         6
  0123456789012345678901234567890123456789012345678901234567890123
  ├──────────────────────────────────────────────────────────────┤
0 │maxtracker    SONG ARRANGEMENT     T:125     Length:12        │
1 │Pos | Pat | Rows |  Pos | Pat | Rows |  Pos | Pat | Rows    │
  ├──────────────────────────────────────────────────────────────┤
2 │ 00 |  00 |  64  |  08 |  03 |  64  |  16 |  -- |  --      │
3 │ 01 |  01 |  64  |  09 |  01 |  64  |  17 |  -- |  --      │
4 │ 02 |  02 |  64  |  10 |  00 |  64  |  18 |  -- |  --      │
5 │ 03>|  01 |  64  |  11 |  05 | 128  |  19 |  -- |  --      │
  │... │     │      │     │     │      │     │     │           │
  ├──────────────────────────────────────────────────────────────┤
30│A:goto pat  Y+A:insert  Y+B:delete  L/R:±pattern  Rpt:00    │
31│STOP  Pos:03/12  Pat:01                                      │
  └──────────────────────────────────────────────────────────────┘
```

Three columns of order entries, showing 28 rows * 3 = 84 visible positions.

---

## 4. Bottom Screen Layout

The bottom screen is **touchscreen** and changes based on the current mode.

### 4.1 Pattern Mode -- Quick Panel

```
  ┌──────────────────────────────────────────────────────────────┐
  │ INSTRUMENTS          │ CHANNEL MUTES                         │ row 0-1
  ├──────────────────────┼───────────────────────────────────────┤
  │ >01 Bass Drum        │  1  2  3  4  5  6  7  8              │ row 2
  │  02 Snare            │ [■][■][■][■][□][■][□][■]             │ row 3
  │  03 HiHat            │                                      │ row 4-5
  │  04 Pad              │  SONG MAP                            │ row 6
  │  05 Lead             │ ┌──────────────────────────────┐     │ row 7
  │  06 Bass             │ │██░░██░░██████░░██░░░░░░██░░░░│     │ row 8-15
  │  07 (empty)          │ │         ▲                     │     │
  │  08 (empty)          │ │     position                  │     │
  │  ...                 │ └──────────────────────────────┘     │ row 16
  │                      │                                      │
  ├──────────────────────┴───────────────────────────────────────┤
  │ Tap instrument to select | Tap channel to mute/unmute       │ row 30-31
  └──────────────────────────────────────────────────────────────┘
```

Left half: instrument list (tap to select current instrument).
Right half: channel mute toggles (tap to toggle) + mini song position map.

### 4.2 Instrument Mode -- Envelope Editor

```
  ┌──────────────────────────────────────────────────────────────┐
  │ ENVELOPE: Volume      Nodes:6  Loop:2-4  Sus:--    [V][P][F]│ row 0-1
  ├──────────────────────────────────────────────────────────────┤
  │64├                                                          │
  │  │    ●                                                     │
  │  │   ╱ ╲     ●─────────●                                   │
  │  │  ╱   ╲   ╱           ╲                                   │ bitmap area
  │  │ ╱     ╲ ╱             ╲                                  │ (rows 2-25)
  │  │●       ●               ●                                │ 144 px tall
  │  │                                 ●                        │
  │ 0├────────────────────────────────────────────────────────  │
  │  0                                                     512 │
  ├──────────────────────────────────────────────────────────────┤
  │ Tap:add node  Drag:move  DblTap:delete   [L][S]markers     │ row 26-27
  │ Node 3: X=018  Y=40   ◄───drag to adjust───►              │ row 28-29
  │ A:confirm  B:cancel  L/R:prev/next envelope type            │ row 30-31
  └──────────────────────────────────────────────────────────────┘
```

The envelope area (144 pixels tall, 240 pixels wide) is a bitmap touch zone. Nodes are drawn as 5x5 circles. Lines connect nodes. Touch interaction:
- Tap empty area: insert node at that tick/value
- Drag existing node: move it
- Double-tap node: delete it
- [V][P][F] tabs at top-right: switch between Volume/Panning/Pitch (if the font is on the screen, they're touch targets)

### 4.3 Sample Mode -- Waveform Drawing

```
  ┌──────────────────────────────────────────────────────────────┐
  │ DRAW SAMPLE  Len:256   [SINE][SAW ][SQR ][NOIS][CLR ]      │ row 0-1
  ├──────────────────────────────────────────────────────────────┤
  │+127│                                                        │
  │    │           ████                                         │
  │    │         ██    ██                                       │
  │    │       ██        ██                              ██████ │ bitmap area
  │   0│─────██────────────██───────────────────────████────────│ 144 px tall
  │    │   ██                ██                  ██             │
  │    │ ██                    ██             ██                │
  │-128│█                        ██████████                    │
  ├──────────────────────────────────────────────────────────────┤
  │ Stylus draws waveform.  Presets seed shape, then edit.      │ row 26-27
  │ Rate:8363Hz  Bits:8  Loop:full                              │ row 28-29
  │ A:save to instrument  B:cancel  X:zoom  L/R:prev/next samp │ row 30-31
  └──────────────────────────────────────────────────────────────┘
```

The drawing area maps 256 horizontal pixels to 256 samples (1:1 for single-cycle). Vertical: 144 pixels maps to -128..+127 range. The stylus draws directly -- wherever you touch becomes the sample value at that position. Dragging paints continuously.

### 4.4 Mixer Mode -- Faders

```
  ┌──────────────────────────────────────────────────────────────┐
  │ MIXER                CH 01-08                    [L/R:9-16] │ row 0-1
  ├──────────────────────────────────────────────────────────────┤
  │  01    02    03    04    05    06    07    08                │
  │ ┌──┐ ┌──┐ ┌──┐ ┌──┐ ┌──┐ ┌──┐ ┌──┐ ┌──┐                  │
  │ │  │ │  │ │  │ │  │ │  │ │  │ │  │ │  │                    │
  │ │▓▓│ │▓▓│ │  │ │▓▓│ │  │ │  │ │  │ │  │   Volume          │ touch-drag
  │ │▓▓│ │▓▓│ │▓▓│ │▓▓│ │  │ │▓▓│ │  │ │  │   faders          │ area
  │ │▓▓│ │▓▓│ │▓▓│ │▓▓│ │  │ │▓▓│ │  │ │  │                   │ 120 px tall
  │ │▓▓│ │▓▓│ │▓▓│ │▓▓│ │▓▓│ │▓▓│ │  │ │  │                   │
  │ └──┘ └──┘ └──┘ └──┘ └──┘ └──┘ └──┘ └──┘                   │
  │  64   64   48   64   20   32   00   00                      │
  │  ◄C►  ◄C►  ◄L►  ◄R►  ◄C►  ◄C►  ◄C►  ◄C►   Pan            │ touch-drag
  │ [  ] [  ] [  ] [  ] [ M] [  ] [  ] [  ]    Mute            │ tap toggle
  ├──────────────────────────────────────────────────────────────┤
  │ Drag faders for volume.  Tap M to mute.  L/R for next 8.   │ row 30-31
  └──────────────────────────────────────────────────────────────┘
```

Each fader is 24px wide with 8px gaps. 8 faders = 8*24 + 7*8 = 248px. Fits in 256.

### 4.5 Project Mode -- Settings & Stats

**Top screen (64 cols x 32 rows):**

```
  0         1         2         3         4         5         6
  0123456789012345678901234567890123456789012345678901234567890123
  ├──────────────────────────────────────────────────────────────┤
0 │ PROJECT SETTINGS                                            │ header
  ├──────────────────────────────────────────────────────────────┤
1 │                                                              │ blank
2 │------------------------------------------------------------- │ separator
3 │ Song Name........................................ mysong      │ editable
4 │ Tempo (BPM)...................................... 125         │ editable
5 │ Speed (ticks/row)................................   6         │ editable
6 │ Master Volume.................................... 128         │ editable
7 │ Channels.........................................   8         │ editable
8 │ Repeat Position..................................   0         │ editable
9 │ Linear Freq...................................... On          │ toggle
10│ XM Mode.......................................... Off         │ toggle
11│------------------------------------------------------------- │ separator
12│ Instruments...................................... 24          │ read-only
13│ Samples.......................................... 16          │ read-only
14│ Patterns......................................... 12          │ read-only
15│------------------------------------------------------------- │ separator
16│ >> New Song                                                   │ action
17│ >> Compact Song                                               │ action
  │...                                                           │
  ├──────────────────────────────────────────────────────────────┤
30│A+L/R:edit  A+U/D:x10  SEL+D:back                            │ help
31│ PROJECT                                                       │ transport
  └──────────────────────────────────────────────────────────────┘
```

The cursor row is highlighted. Separator rows are skipped during navigation.
Read-only rows (instrument/sample/pattern counts) are displayed but not editable.
Action rows use two-tap A confirm: first press shows "[A=confirm B=cancel]",
second press executes.

**Bottom screen (64 cols x 32 rows):**

```
  ┌──────────────────────────────────────────────────────────────┐
  │ SONG INFO                                                    │ row 0
  ├──────────────────────────────────────────────────────────────┤
  │ Name: mysong                                                 │ row 2
  │ Orders: 12                                                   │ row 3
  │ Patterns: 12 allocated, 10 in use                            │ row 4
  │ Instruments: 24                                              │ row 5
  │ Samples: 16                                                  │ row 6
  │ Channels: 8                                                  │ row 7
  │ Tempo: 125  Speed: 6                                         │ row 8
  │                                                              │
  │ Sample memory: 384 KB                                        │ row 10
  │                                                              │
  └──────────────────────────────────────────────────────────────┘
```

Shows song statistics summary including sample memory usage.

---

## 5. Visual Feedback

### 5.1 Playback Row Highlight

During playback, the currently-playing row gets a distinct background color (palette 15, dark green). In follow mode, this row is always centered on screen. In non-follow mode, it scrolls through the visible area and wraps.

### 5.2 Beat Row Markers

Every 4th row (configurable: 4, 8, 16) gets a slightly different background color (palette 2) to provide visual rhythm. This is how LSDJ and M8 help you keep track of musical position.

### 5.3 Cursor Highlight

The cursor cell has a bright background (palette 3, dark blue). In overview mode, the entire channel column at the cursor row is highlighted. In inside mode, the specific sub-column (Note/Ins/Vol/Fx/Prm) is highlighted.

### 5.4 Empty vs. Data Cells

Empty cells (`--- --`) are drawn in dim gray (palette 4). Cells with data use bright colored text (palette 8-11 per field type). This makes the pattern structure immediately visible -- you can see the "shape" of the music at a glance.

### 5.5 Channel Activity Indicators

In overview mode, the rightmost 4 columns (60-63) show per-channel activity bars during playback. Each channel gets a 1-character-wide column that fills from bottom to top based on current volume. This is a simple but effective VU meter.

---

## 6. Rendering Performance

### 6.1 Full Redraw Budget

Full screen: 256 * 192 = 49,152 bytes to write (8bpp).
At ARM9 speed with cached writes: ~50K cycles = ~0.75ms.
With font rendering overhead (lookup + branch per pixel): ~2-3ms for a full text screen.

Target: 16.7ms per frame (60fps). Full redraw uses <20% of the frame budget.

### 6.2 Dirty-Region Optimization

In normal editing, only a few rows change per frame:
- Cursor moved: redraw old row (restore normal bg) + new row (cursor bg) = 2 rows
- Note entered: redraw 1 row
- Playback advancing: redraw old playing row + new playing row = 2 rows

Each row: 64 chars * 24 pixels = 1,536 bytes + background fill = ~2,048 bytes. Two rows = ~4KB.
At ARM9 speed: <0.1ms. Negligible.

### 6.3 Double Buffering

Not strictly necessary given the dirty-region approach, but if visual tearing is noticeable, we can use the 256x256 bitmap's extra 64 rows (y=192..255) as a scratch area and DMA-copy completed rows to the visible area during VBlank.

---

## 7. Touch Input Zones (Bottom Screen)

Touch zones are defined as pixel rectangles. The bottom screen renderer draws the zones, and the input handler checks `touchRead()` coordinates against them.

```c
typedef struct {
    u16 x, y, w, h;
    u8  id;             // zone identifier for the input handler
} MT_TouchZone;
```

Each screen mode registers its own set of touch zones. On mode switch, the zone list is replaced. This keeps touch handling simple and mode-specific.
