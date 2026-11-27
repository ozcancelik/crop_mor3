# Changelog

## v0.4.0
- **Smaller lossless files (experimental):** new `mlv_lite` option **Movie → RAW video → LJ92 Huffman: Canon / Optimized** (default Canon). With Optimized, lossless 8–12-bit clips use a Huffman table made for their bit depth instead of Canon's 14-bit table. Tested on 5D3 1.1.3 and 1.2.3: about −15 to −25% at 12-bit and −30 to −45% at 10/11-bit (larger gains at low ISO). 14-bit stays on Canon's table. Canon's table is put back after every recording, so CR2 photos are not affected. The module only switches tables when the camera's table passes a CRC32 check; Canon's table is not stored in the source.
- **1x3 line at row 1288 fixed (all tall modes):** in 8–12-bit lossless, a dark line could appear on the left half of row 1288. The module now uses a fixed copy of Canon's HIV correction table for the full frame height.
- **5.4K (2040p) 8–12-bit fixed:** prepro/HIV height now matches the 2070-line raw frame. Fixes the 12 Hz flicker, frozen frames and garbage bottom lines in 12-bit (and the `ASSERT` on stop). The LiveView preview sits ~120 raw lines lower in this mode.
- **1:1 crop joystick pan:** the pan window is now set inside Canon's LiveView path, every frame, instead of being corrected afterwards. Fixes pink/frozen LiveView and `Compression error` when recording after panning.
- **RAW video on by default:** a fresh install now starts with both Crop Mor3 (`1:1 crop 1280p`) and RAW video enabled. Existing settings are kept.
- **`mlv_play` bit depth:** clip info, the bottom line and new thumbnails show the real bit depth (10/12/14) instead of always 14-bit. 10/12-bit clips use the full tone table (less banding). Old `.THM` thumbnails may still show the old value. Also in the EOS-M drop-in.

## v0.3.1
- **`mlv_play` colours:** the superpixel renderer now uses the same YUV matrix as the camera's core (BT.709 on the 5D Mark III, BT.601 on EOS-M), so colours match the stock `color` preview. The tone table is now 11-bit (2048 levels): smoother shadows, less banding.
- **`mlv_play` speed:**
  - Info text over the video is repainted only when it changes (frame counter every 5 frames, full repaint on INFO / menu close / new clip / every 150 frames), instead of on every frame.
  - Superpixel and `.PRX` playback write each line straight into every duplicated LCD line, instead of copying the finished line back from uncached LCD memory.
- **Module descriptions** in the ML Modules menu updated for `mlv_lite`, `mlv_play` and `crop_rec` (Crop Mor3), with author and summary.
- **`crop_rec`:** removed a leftover debug log (`ML/LOGS/CMLIVE.LOG`).

## v0.3.0
- **EOS M-Style Menu Structure (`OFF` / `1:1 crop` / `1x3` / `3x3`):**
  - Top-level menu cycles between `OFF`, `1:1 crop`, `1x3`, and `3x3` (automatically matching Canon's active `1080p` vs `720p 50/60fps` video mode), with a `Q` submenu for `Preset:`, `Aspect ratio:`, and `Output:`.
- **New `5.4K (2040p)` (`1808×2040` → `5424×2040 @ 24p`) `1x3` Anamorphic Mode & Shadow Color Shift Fix:**
  - Exceeded 1920 vertical lines in `1x3` mode (24p) with real-time hardware WYSIWYG LiveView preview.
  - Clamped `prepro_yd` to `662` (`1949` total lines) for `HIV` (`EDMAC ch 0x18` darkframe reference table) and `SSLT` while keeping a 293-line end-of-frame OB clamp window (`TimerB = 2395, TimerA = 418, CMOS[1] = 0x348`), eliminating dynamic blue ↔ pink shadow color shifts when panning across scenes.
- **Hi-Res `1:1 crop` (`2.3K`, `3.1K`, `3.5K`) Horizontal Centering Fix:**
  - Locked the `x5` LiveView preview window (`S4_WIN_S` / `S4_WIN_E`) to the exact center of the 3744-pixel `x5` RAW buffer for `2.3K (2336×1312)`, `3.1K (3104×1320)`, and `3.5K (3584×1320)` while preserving joystick panning in `1280p` / `1080p` / `816p`.
- **`3x3` Full-Frame High-FPS Modes (`50/60p HFR`, `45/48p Tall`, `2.40:1 Scope`):**
  - Added `3x3` family for full-frame high-framerate recording (`1920×800 @ 60p`, `1920×960 @ 50p`, `1920×1040 @ 48p`, `1920×1080 @ 45p`) in Canon 720p mode.
- **Safe LiveView Disarm:**
  - Pauses LiveView cleanly before unpatching `L0..L4`, SSLT margins, and Eeko tables when turning off or switching families so DMA never sees mismatched tables mid-frame.

## v0.2.0
- **5D Mark III (Firmware 1.2.3) — Full Support Verified on Hardware:**
  - Single binary auto-detects Firmware `1.1.3` and `1.2.3` at runtime (`cm_fw_113` / `cm_fw_123`).
  - Fixed `Eeko` `0x402` mode-5 2nd-scaler vertical ratios (`w[20]/w[21]`) for 1272-line `x5` output, preventing `VGAIMG_VRAM1..3` overflow into `EXMEM3_AREA1` (`PackHeap`) on 1.2.3 (eliminates both horizontal scanline flicker and `MENU` / `RAW` crashes).
  - Fixed `0x302` `f4_modify()` preserving `w[11] = 0` (L0 C-plane chroma phase matching `x5` `C0F38458 = 0x8DD80000`), giving full vivid accurate colors across all ISOs.
  - Verified on real 5D Mark III running Canon 1.2.3.
- **Release Packaging:**
  - Included pre-enabled `ML/SETTINGS/*.EN` flags in release ZIPs.

## v0.1.0
- **5D Mark III (1.1.3) — `crop_rec.mo` & `mlv_lite.mo`:**
  - Full WYSIWYG LiveView preview + clean RAW recording for `1x3` tall anamorphic modes (`3.4K 16:9`, `4.5K 2.35:1`, `5.7K 3:1`) and `x5` 3x crop modes (`Full`, `1080p`, `Scope`).
  - Eliminated 12-second OB drift / pink gradient (`ADTG PowerSaveTiming = 0`).
  - Safe `L0..L4` buffer layout, proper `RAWC` binning metadata, and symmetric framing bars.
- **5D Mark III & EOS-M — `mlv_play.mo`:**
  - 3×2 thumbnail gallery with `.THM` v2 disk cache and dynamic resolution badges (`3.4K`, `4.5K`, `5.7K`, etc.).
  - Automatic Bayer phase detection (corrects magenta tint on EOS-M `1x3` tall clips).
  - Safe delete confirmation (defaults to `[ Cancel ]`, also accessible via `INFO → SET` for cameras without a `TRASH` key).
  - Touchscreen event protection on EOS-M and macOS `._*` junk file filtering.
  - Inline render + HUD + OSD fallback when DryOS task pool is full on EOS-M.
  - `.PRX` realtime proxy playback (auto-built during first sequential pass, played at exact source FPS afterwards; auto-cleans incomplete `.PRX` files; falls back to RAW when zoomed).
