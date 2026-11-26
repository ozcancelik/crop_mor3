<h1 align="center">
  <img src="docs/assets/img/crop_mor3_logo.png" alt="Crop Mor3 (crop_mor3)" width="440">
</h1>

**Crop Mor3** is Crop Mood for the 5D Mark III, by Özcan Çelik: a port of Bilal Fakhouri's `crop_rec` for the EOS M. 13 presets, three families: `1:1 crop` up to 3.5K, `1x3` up to 5.7K, `3x3` up to 60p. What you see on the LCD is what you record.

**Website and interactive simulator:** https://ozcancelik.github.io/crop_mor3/

Magic Lantern modules for the **Canon EOS 5D Mark III** (plus an `mlv_play` drop-in for **EOS-M**):

- **`crop_rec.mo`** (**Crop Mor3**) — Drop-in replacement for `crop_rec.mo` on 5D Mark III with EOS M-style `1:1 crop` (`1280p` / `1080p` / `816p` / `2.3K` / `3.1K` / `3.5K`), `1x3` tall anamorphic (`3.4K` / `4.5K` / `5.4K 2040p` / `5.7K`), and `3x3` High-FPS (`50/60p` / `45/48p Tall` / `2.40:1 Scope`) LiveView preview and RAW readout modes.
- **`mlv_lite.mo`** — Matching `1x3` tall anamorphic (up to `2040p`), `1:1 crop`, & `3x3` High-FPS RAW recording (`10/12/14-bit` lossless & uncompressed) with clean `RAWC` binning metadata and symmetric framing guides.
- **`mlv_play.mo`** — Rewritten MLV player: gallery with thumbnails and clip details, real-time `.PRX` proxies with sound, LUTs and `.cube` files, 10/12/14-bit and LJ92. See [mlv_play](#mlv_play) below.

> [!IMPORTANT]
> **Download ready-to-use packages from the [Releases](../../releases) page.**
> These packages **never** include `ML-SETUP.FIR` and do not touch Canon ROM, NVRAM, or bootflags. Your card must already be bootable with official Magic Lantern.

> [!WARNING]
> **AI-assisted, not independently verified.** The register values, firmware addresses and hooks in these modules were found with the help of an AI model (LLM) during reverse engineering. Every mode was tested on my own cameras, but none of it has been reviewed or confirmed by Magic Lantern developers. Some values may be wrong or cause crashes in situations I haven't tested. Use at your own risk.

---

## Supported Cameras & Modes

### Canon EOS 5D Mark III (Firmware 1.1.3 & 1.2.3)
Both official Canon firmwares **1.1.3** and **1.2.3** are fully supported and verified on real hardware (same `crop_rec.mo` binary auto-detects firmware at runtime).

In the **Movie → Crop Mor3** menu, press **SET** to cycle the readout family (`OFF` → `1:1 crop` → `1x3` → `3x3`) and press **Q** to choose the **Preset**:

| Crop Mor3 Family | Preset | Raw Capture | Final Output | H FOV | V FOV | LiveView Preview |
|---|---|---|---|---|---|---|
| **1:1 crop** | `1280p (3:2)` | `1920×1280` | `1920×1280` | 33% | 33% | WYSIWYG (`x5`, pan with joystick) |
| **1:1 crop** | `1080p (16:9)` | `1920×1080` | `1920×1080` | 33% | 28% | WYSIWYG (`x5`, pan with joystick) |
| **1:1 crop** | `816p (2.35:1)` | `1920×816` | `1920×816` | 33% | 21% | WYSIWYG (`x5`, pan with joystick) |
| **1:1 crop** | `2.3K (16:9)` | `2336×1312` | `2336×1312` | 41% | 34% | Centered 1920 px (`x5`) |
| **1:1 crop** | `3.1K (2.35:1)` | `3104×1320` | `3104×1320` | 54% | 34% | Centered 1920 px (`x5`) |
| **1:1 crop** | `3.5K (2.71:1)` | `3584×1320` | `3584×1320` | 62% | 34% | Centered 1920 px (`x5`) |
| **1x3** | `3.4K (16:9)` | `1136×1920` (24p) | `3408×1920` | 59% | 50% | WYSIWYG |
| **1x3** | `4.5K (2.35:1)` | `1504×1920` (24p) | `4512×1920` | 78% | 50% | WYSIWYG |
| **1x3** | `5.4K (2040p)` | `1808×2040` (24p) | `5424×2040` | 94% | 53% | WYSIWYG |
| **1x3** | `5.7K (3:1 Max)` | `1920×1920` (24p) | `5760×1920` | 100% | 50% | WYSIWYG |
| **3x3** | `50/60p (HFR)` | `1920×800` (60p) / `1920×960` (50p) | `1920×800` / `1920×960` | 100% | 63% / 75% | WYSIWYG (Canon 720p mode) |
| **3x3** | `45/48p Tall` | `1920×1040` (48p) / `1920×1080` (45p) | `1920×1040` / `1920×1080` | 100% | 81% / 84% | WYSIWYG (Canon 720p mode) |
| **3x3** | `2.40:1 Scope` | `1920×800` (50/60p) | `1920×800` | 100% | 63% | WYSIWYG (Canon 720p mode) |

> [!NOTE]
> **`x5` frame rate (`29.776 fps`):** Canon's `x5` focus-zoom LiveView natively runs at `29.776 fps` regardless of the Canon menu's 24p/25p/30p setting (standard Canon 5D3 hardware behavior on both 1.1.3 and 1.2.3). To shoot `23.976 fps` in `1:1 crop` modes, enable Magic Lantern's **Movie → FPS override** (`23.976`). The `1x3` tall anamorphic modes follow Canon's selected FPS automatically (`24p`, `25p`, `30p`), and `3x3` High-FPS modes require Canon's video mode set to `720p 50/60fps`.

### Canon EOS-M (Firmware 2.0.2 — `mlv_play.mo` drop-in)
- Drop-in `ML/MODULES/mlv_play.mo` for existing Crop-Mood EOS-M installations. Same features as on the 5D3 (see below).
- No ERASE key: delete from the gallery with `INFO → SET`. Touch events are ignored, and it still runs when DryOS is out of task slots.

---

## mlv_play

**Browse**
- 3×2 gallery with mode badges (`5.7K`, `1080p`…) and an SD/CF tag.
- Thumbnails saved as `.THM`: instant on the next visit.
- **INFO**: clip details. Size, date, raw → desqueezed size, bit depth, LJ92, black level, fps, frames, length, MB/s, ISO, shutter, WB, lens, audio.
- Safe delete: always starts on **Cancel**.
- Hides macOS `._` files. Plays old `.RAW` clips too.

**Play**
- `.PRX` proxy: built on the first play (`PRX+`), then real time at the clip's fps.
- Sound from the MLV, in sync, during proxy playback. Mute, 1–5, +6 dB, +12 dB.
- Pause, previous / next clip, back to the gallery, progress bar.
- 4-step zoom, pan with the arrows (zoom reads the raw).
- `exact`: real-time pacing. `all`: every frame.

**Look**
- Superpixel renderer (default): 10, 12 and 14-bit, uncompressed and LJ92.
- 1x3 clips desqueezed automatically (RAWC). Bayer phase detected per clip.
- Right colours: BT.709 on the 5D3, BT.601 on EOS-M.
- LUTs: Orig, Rec709, Kodak, Warm, Teal, B&W, plus your own `.cube` files in `ML/LUTS/` (up to 24).

**Controls**

| Key | Gallery | Playback |
|---|---|---|
| SET | Play clip | Show the playback bar |
| Arrows / wheel | Select clip | Wheel ◀ ▶: previous / next clip |
| PLAY | Exit | Pause / play |
| INFO | Clip details | Info text on / off |
| TRASH | Delete (starts on Cancel) | Delete |
| RATE | – | Next LUT |
| ZOOM | – | Zoom, arrows to pan |
| Q | – | Show / hide the playback bar |
| MENU | Exit | Back to the gallery |

Playback bar: Grid · ◀◀ · pause · ▶▶ · quality (color / fast / super) · LUT · volume · exact / all · Delete · Quit. Quality, LUT, volume and exact / all are remembered.

**vs. stock mlv_play**

| | Stock | Crop Mor3 |
|---|---|---|
| Bayer phase | Fixed | Detected per clip |
| Real time from SD | No | Yes, with `.PRX` |
| Sound | No | Yes (proxy playback) |
| Browsing | Next / previous | Gallery, thumbnails, clip details |
| Looks | – | 6 LUTs + your `.cube` |
| Delete | Press again within 5 s | Confirm screen, starts on Cancel |

---

## Known limitations

- **Pink or broken LiveView:** sometimes, when returning to LiveView, the picture is pink or broken. Press **MENU** to open Canon's menu, then press **MENU** again to close it; LiveView comes back clean.
- **Tested on SD only:** all modes were tested on an SD card with an overclocked bus (`sd_uhs`). CF cards and long takes are not tested yet. How long you can record depends on your card.
- **2.3K–3.5K 1:1 crop** need a fast card. **5.4K (2040p)** is 24p only.
- **1:1 crop frame rate:** these modes run in Canon's x5 LiveView at `29.776 fps`. Set **Movie → FPS override** to `23.976` for 24p.
- **3x3 High-FPS** modes only work with Canon's movie mode set to **720p 50/60fps**.
- **Panning** the 1:1 window works in 1280p, 1080p and 816p. 2.3K–3.5K record a wider window but LiveView shows the center 1920 px.
- **Panning and recording:** while panning the 1:1 window with the joystick, LiveView can sometimes turn pink or broken. Don't start recording in that state: recording stops with a `Compression error` (an `ASSERT` log on the card). Press **MENU** twice (open and close Canon's menu) to get a clean picture, then record.
- **Tall height:** 1x3 modes read at most 1920 lines (2040 in 5.4K). Taller readouts were unstable (black level drops), so they are not offered.
- **Firmware:** 5D Mark III 1.1.3 and 1.2.3 only. EOS-M gets `mlv_play` only.
- **mlv_play:**
  - Real-time playback needs a `.PRX` proxy, built on the first play (that first play is slower).
  - Sound plays only from the proxy.
  - A proxy takes 170–345 KB per frame depending on the aspect ratio (≈ 250–500 MB per minute at 24p). Delete `.PRX` files to free space; they are rebuilt when needed.

---

## Installation

### Canon 5D Mark III (1.1.3 or 1.2.3)
1. Ensure your SD/CF card is already bootable with Magic Lantern (`1.1.3` or `1.2.3`).
2. Download `crop_mor3-vX.Y.Z-5D3.113.zip` or `crop_mor3-vX.Y.Z-5D3.123.zip` from **Releases**.
3. Extract `autoexec.bin` and the `ML/` folder directly to the root of your card (required modules are pre-enabled via `ML/SETTINGS/*.EN`).
4. Boot the camera, open ML Menu → **Modules**, and confirm `crop_rec.mo` (**Crop Mor3**), `mlv_lite.mo`, `mlv_play.mo`, and `silent.mo` are loaded.

### Canon EOS-M (2.0.2)
1. Download `crop_mor3-vX.Y.Z-EOSM.202-mlv_play.zip` from **Releases**.
2. Copy `ML/MODULES/mlv_play.mo` onto your existing Crop-Mood SD card, replacing the old `mlv_play.mo`.
3. Make sure `mlv_play.mo` and `silent.mo` are enabled in ML Menu → **Modules**.

> [!TIP]
> **macOS users:** When copying files to FAT32/exFAT cards from macOS, use `cp -X` (or clean `._*` files) so DryOS does not see AppleDouble metadata files as ghost modules or clips. The release ZIPs are already packaged without `._*` files.

---

## Source Code & License

Modified module sources are included under [`src/`](src/) (`crop_rec`, `mlv_lite`, `mlv_play`) for full transparency and GPLv2 compliance.

© 2026 Özcan Çelik. Licensed under the **GNU General Public License v2.0** (same as Magic Lantern). See [LICENSE](LICENSE).
