# Pokémon Snap → 3DS: phase log

Same route as the Mega Man 64 port (`G:\!VibeCodeMashups\Megaman643DS`): N64Recomp static
recompilation + ported N64ModernRuntime + 3DS glue + our own F3DEX2→Citro3D renderer.

# Phase 1 — Evaluation ✅ (2026-10-20)

## ROM
- `Pokemon Snap.n64` is byte-swapped (.v64 order). `rom/pokemonsnap.us.z64` = `dd conv=swab` of it.
- Header: "POKEMON SNAP", game code NPFE (USA), 16 MB, CIC-6103 (entry 0x80000400).
- SHA-1 (z64) `edc7c49cc568c045fe48be0d18011c30f393cbaf` = the USA ROM the decomp and Snap64Recomp target.

## References (reference/, shallow clones, read only)
- `Snap64Recomp` (JackandBeans, GPL-3.0): the PC recompilation (RT64 renderer, Windows/Linux/Mac).
  Its N64Recomp input is normally the decomp's ELF relinked with `--emit-relocs`, built under WSL
  with IDO 7.1. This machine has no WSL.
- `pokemonsnap` (ethteck): the decompilation.

## Recompilation without the decomp build
- Snap64Recomp tracks `patches/pokemonsnap.syms.toml` (names + addresses of every section and
  function, generated from the decomp). Copied to `recomp/`; used as `symbols_file_path` the way
  MM64 used MegaMan64RecompSyms.
- 32 sections: `.main`, `.app_render`, `.more_funcs`, `.world`, 7 level `*_code` + 7 `*_assets`,
  `.app_level`, `.window`, menus (`.main_menu`, `.menu_new_game`, `.oaks_lab`, `.pokemon_album`,
  `.pokemon_report`, `.gallery`, `.photo_check`, `.camera_check`, `.credits`, …). Most overlays share
  VMAs (e.g. 0x800E18A0, 0x801DC8C0), so the runtime must register the loaded section.
- Coverage: every code section's functions are contiguous except 27 gaps in `.main`, filled by the
  28 `manual_funcs` copied from Snap64Recomp's `pokemonsnap.us.toml` (static callbacks).
- Ignored (RSP microcode, not CPU code): rspbootTextEnd, aspMainTextStart/End,
  gspF3DEX2_NoN_fifoTextStart/End.
- Result: `tools/n64recomp/N64Recomp.exe recomp/pokemonsnap.us.3ds.toml` → 4268 functions, 86 files,
  24 MB of C in 0.6 s, exit 0. 115 "ambiguous jal target" notes (calls into addresses shared by
  overlays) fall back to runtime function lookup.

## Differences from MM64 to expect
- Graphics microcode: F3DEX2_NoN fifo (+ L3DEX2 for lines) — our renderer is F3DEX2 already.
- Audio: aspMain; check whether it matches MM64's native audio HLE (source/port/audio_hle.cpp).
- `auThreadMain` reads AI_LEN (0xA4500004) directly twice (vram 0x800219DC, 0x80022278; osAiGetLength
  inlined): needs a hook/instruction patch so it asks the audio queue (Snap64Recomp does this in
  tools/hook_funcs.py).
- Snap64Recomp's game-side patches (patches/src, IDO-compiled) can't be built here; port any we need
  as N64Recomp hooks or native function overrides instead.
- Overlay loading: find Snap's overlay/segment loader and hook it (MM64: Overlay_Loader →
  recomp_load_overlays). Snap64Recomp's src/overlay_hook.cpp and src/recomp_overlays.inl show how.
- ROM is 16 MB (MM64 32 MB): fits in memory easily.

# Phase 2 — 3DS skeleton (2026-10-20)

Copied from Megaman643DS: runtime/ (N64ModernRuntime port), source/render (renderer), the generic
source/port files (audio_3ds, input_3ds, crash_log, trace.h), Makefile, tools (run_emu.ps1,
test_run.ps1, snap.py, cia/), assets/cia.rsf, azahar/ (binaries + config; own user/sdmc). Names
mm64_* → snap_*; data folder sdmc:/3ds/psnap; app psnap-3ds.3dsx.

Snap-specific:
- `source/port/snap_game.cpp/.h`: boot (game id "pokemonsnap", save type AllowAll), callbacks,
  overlay bookkeeping, dev helpers. `snap_hooks.h` is N64Recomp's `recomp_include`.
- Overlays: hook at dmaLoadOverlay's entry (0x80002B64) reads the Overlay struct (romStart, romEnd,
  vramStart), evicts every resident section it overlaps (each by its own extent) and registers the
  sections in that ROM range. Runtime changes: `recomp3ds_boot_code_size` (boot registers only
  .main, not 1 MB of sections at wrong addresses) and `unload_overlays` no longer counts a section
  that only touches the region as overlapping (that hit the fatal partial-unload check).
- AI_LEN: the two `lw` from 0xA4500004 in auThreadMain (0x800219DC, 0x80022278) are patched to stack
  reads ([[patches.instruction]]) and hooks after them set the register to
  ultramodern::get_remaining_audio_bytes().
- Audio: Snap's aspMain differs from MM64's (MM64's native audio HLE doesn't apply). RSPRecomp
  output recomp/rsp/aspMain.cpp (config recomp/aspMain.us.3ds.toml from Snap64Recomp: text
  ROM 0x3E580 size 0xE20, IMEM 0x1080, 16 dispatch targets).
- Missing libultra: __osSetWatchLo (no-op), __osContRamRead/Write (PFS_ERR_NOPACK).
- manual_funcs: N64Recomp ignores them with a symbols file (first boot: "Failed to find function at
  0x8002E2F8"). tools/add_manual_funcs.py merges them into recomp/pokemonsnap.syms.toml. Two are
  libultra internals and are ignored: 0x8003CF20 (viMgrMain) and 0x80034398 (Controller Pak CRC).
- Removed MM64-only: bottom UI, savestates, patches, title logo/credit, narration skip, HUD diversion
  (renderer hooks left with zero counters).
- main.c: placeholder bottom screen (title, version, ROM/game state, counters); START+SELECT exits.

## v1 (2026-10-20): first boot ✅
Azahar, 40 s: boots, loads .app_render/.more_funcs/.main_menu overlays, draws the intro (Todd in the
forest) with textures and lighting; ~20 display lists/s, VI ~49/s. Audio plays but the recompiled
aspMain takes ~65% of the game core (underruns): a native audio path for Snap's microcode is the
main performance item. Bottom screen: placeholder status page. dist/psnap-3ds-v1.3dsx (ROM on the
SD card: sdmc:/3ds/psnap/pokemonsnap.us.z64, .z64 byte order).

## v2 (2026-10-20): leaves and logos
User report on v1: tree textures in squares, opening logos distorted.
- Leaves (CI4 64x64 at 0x35F968, om C811307D = TEX_EDGE + G_AC_THRESHOLD with blend alpha 0): the
  threshold's ref 0 replaced the coverage cut-out ref and a ref of 0 disables the test, so every
  leaf card was a solid square. CVG_X_ALPHA now keeps at least the 0x4D cut-out.
- HAL logo (32x32 RGBA32 tiles via LOADBLOCK): RGBA32 sits in TMEM as two 16-bit halves, so the
  tile's line (and TMEM offset) count half of an RDRAM row; stride was line*8, half a row, so
  alternate rows came from the wrong place. Doubled for 32-bit LOADBLOCK textures.
- Also added: LOADBLOCK dxt 0 → odd rows read with byte offsets XOR 4 (XOR 8 for 32-bit), as the
  RDP's odd-row un-swap does; not what broke the logo, but correct for such loads.
Checked: N64 logo, Nintendo, HAL, Jack and Beans and the forest intro all render correctly.

## v3 (2026-10-20): native audio, smooth dialogue
- Native ABI1 audio (source/port/audio_hle.cpp): SEGMENT/SETBUFF/SETVOL/... per mupen64plus
  semantics; ENVMIXER translated from the microcode (IMEM 0x1B38: 32-bit volume lanes multiplied by
  the 16.16 rate per 8-sample group, VCL/VGE clamp, init interpolation with 0x2000*(k+1); state
  0x50 bytes = L hi/lo, R hi/lo, params); RESAMPLE state is 0x20 bytes (+0xA extra, +0x10 look-ahead,
  used with flag 2). DMEM layout: buffers 0x5C0+operand, state block 0x360, resample table 0xC0.
  Checked with audio_compare.txt (DATA_PATH; N tasks run both ways, or "lle"): mixed output
  identical in every task; only RESAMPLE's look-ahead bytes beyond the input differ (microcode's
  own DMEM use). Audio ~20% of the game core in the lab (was ~65% recompiled); 30 fps in menus.
- Dialogue report ("not smooth when it appears"): text reveals one character per game frame
  (UIText_PrintString, ohWait(1)). Causes: low/uneven game frame rate (fixed by the above) and a
  top-screen hitch every 2 s: the watchdog flushed the log to the SD card holding stderr's lock, so
  any thread that logged (UI, renderer, audio) waited. Now stderr goes to an in-memory device
  (source/port/log_ring.c, "log:"), drained to runtime.log by the watchdog; full reports only on
  development runs (autoinput.txt or devlog.txt). [pace] log line: UI and game frame gaps in
  vblanks. Lab: 30 fps, no 4+ vblank UI gaps outside the dev screenshot (every 180 frames).
- Note for testing: snapshots every few VIs starve the UI thread (BMP writes) and several get the
  same frame; don't judge animation smoothness from dense snapshots.
- Found for later: pokemon_detect.c renders into an off-screen buffer (PokemonDetector_Regions) and
  reads it back to see which Pokémon are in a photo; the renderer must support render-to-RDRAM
  for photo scoring.

## v4 (2026-10-20): into the Beach
Scripted: title → name card → Oak's lab (A through the talk, B at "repeat?", A on Go to Course) → Beach.
Renderer fixes found there:
- **Z-buffer clears were lost**: Snap clears depth by pointing the color image at the z-buffer and
  filling (render.c), then selects the framebuffer. The renderer keyed frames by color image, so the
  clear went into a never-shown "frame" at the z-buffer address; every frame then depth-tested
  against stale depth and was composited onto the previous frame (2x draws). The clear is now held
  until the next color buffer is selected; a frame starting with a z clear isn't composited. This
  was the blue patches (the sea showing through the sand).
- Depth: perspective vertices get z = -w/2 with a w-buffer (C3D_DepthMap(false, -1/32768, 0)):
  depth = distance, clipping only at w >= 0 (F3DEX2_NoN doesn't clip near/far), uniform precision.
  Depth clears write w = 32768. Orthographic vertices keep the N64 z.
- Guard-band clipping in software (x, y within 2 screens, w >= 0.01), Sutherland-Hodgman with
  interpolated attributes: huge off-screen/near-camera triangles were dropped by the PICA.
- G_BRANCH_Z implemented (vertex screen z < w1 16.16): terrain chunks pick their detailed version
  when near (was always taken).
- gSPMvpRecalc (opcode 0xD5) and G_MW_MATRIX writes into the combined matrix (billboards).
Dev tools added (pokes.txt): magenta 1 (uncovered pixels), drawid 1 (flat color = draw index),
nocull 1, dltrace N (every command + triangles of display list N), texnocache 1; audio_compare.txt
"off" skips audio tasks. Watchdog: thread_switches, switch_us.
Performance (Azahar, Beach): courses target 30 fps (gtlSetIntervals(1, 2): logic every VI, draw
every 2nd); we get ~18-20 (game frame gaps of 3 VIs). Game core (core 2: game threads + display
list interpretation) is saturated; audio (core 0) isn't the limit ("off" changes nothing). Snap's
object processes are N64 threads (omRunProcess/ohWait: osStartThread/osStopThread): ~2100 thread
switches/s, average signal-to-run latency ~280 us (includes waiting behind display-list work on the
same core; set_native_thread_priority is a no-op on 3DS). Candidates: fibers for game threads,
moving display-list interpretation off core 2. Needs real-hardware numbers.
Still to check: sand texture looks faint vs the reference (texture LOD / 2-cycle detail?).

## v5 (2026-10-20): photos
Taking photos: hold Z (ZL/ZR) to zoom, A takes the picture (player.c: shutter only while zoomed).
Snap stores a photo as scene data and re-renders it (into a 320-wide RGBA16 buffer, a 64-wide
thumbnail sheet, and 64x24 strips copied out of the sheet) whenever it's shown.
- **Focus detection** (pokemon_detect.c): after each Pokemon the game COPY-rects the framebuffer's
  center 8x8 tile (x 156-163, y 116-123) into an 8-wide buffer and counts changed pixels. The renderer
  follows that tile in software (center_raster: every triangle rasterized over the 64 pixels with a
  1/w depth test) and center_copy writes per-pixel ids. Pokemon get identified ("pictures of PIDGEY").
  (Width-8 color images were clamped to 320: cimg_real_width keeps the real width.)
- **Render-to-RAM** (finish_offscreen_frames / c3d_backend offscreen_*): color images the VI never
  showed (>= 64 wide, not z-buffers) are off-screen images: drawn with a 1:1 mapping
  (DrawCmd::offscreen_map, 512x256 area), sent to the backend after each display list (or before a
  texture load reads them, which then waits), drawn there into ONE render target made at start-up
  (256x512 RGBA8) on top of a seed of the image's current RDRAM contents, copied to linear memory
  with GX_DisplayTransfer behind the draws (C3D_FrameSplit first), and written back to RDRAM over
  the draws' bounding boxes in the next UI frame. Linear read-back index: x * 256 + (255 - y).
- Things that froze or broke on the way (don't retry): synchronous transfers inside a frame
  (C3D_SyncDisplayTransfer hangs: citro3d's queue isn't running mid-frame); CPU access to VRAM
  (crawls); render targets made from textures (C3D_RenderTargetCreateFromTex needs VRAM and froze
  Azahar); creating/deleting render targets during play (froze Azahar); GX_TextureCopy round trips
  (froze); writing back a whole scissor area (Snap draws 64x24 strips with a full scissor: neighbors
  got flattened); per-seed textures (exhausted linear memory).
- Remaining: in the thumbnail sheet (0x2B8600, 64 wide) the first thumbnail is right, later ones
  come out with alternating rows of content/black, and the strips copied from them are black.
  Pokemon Snap's camera check in Azahar is slow (each off-screen image costs a UI frame).

## v6 (2026-10-20): thumbnail copies on the CPU
- COPY-mode 1:1 16-bit texture rectangles into off-screen images are done on the CPU in RDRAM
  (cpu_copy_rect), after waiting for pending write-backs of source and destination. Images are
  sized by their draws' scissors (image_bytes), not 3/4 of the width.
- Camera check layout found so far: each photo is rendered into a 64x48 buffer at 0x2B8600 in
  several display lists; scissor 0,0,64,624 is set with cimg 0x2B9E00; 64x24 strips at
  0x2CD600..0x2D4200 are copied from 0x2B9E00/0x2BAA00. Read as 104 wide, 0x2B9E00.. holds coherent
  104x78 photo renders with every other row black: the remaining black thumbnails (top row) come
  from that. Next: find who writes 0x2B9E00 (dltrace the camera-check DLs) and why rows alternate.

## v7 (2026-10-20): all Camera Check thumbnails, controls
- Cause of the black thumbnails (decomp window/847B60.c, func_80374714): the game re-renders each
  photo at 2x into a buffer (gtlDrawOne) and then averages it 2x2 into the thumbnail bitmap on the
  CPU, right after the task completes. A GPU fill clears the bitmaps first. Our write-backs landed a
  UI frame later, so the CPU read stale renders and the late fill wiped its thumbnails.
- Fix: run_display_list doesn't return (so the task doesn't complete) until every off-screen image
  the display list drew is back in RDRAM (finish_offscreen_frames -> sync_offscreen_reads over all
  of RDRAM). All six thumbnails render in Azahar.
- Controls (docs/CONTROLS.md): Z also on L (Old 3DS has no ZL/ZR), R dash, Y Poke Flute (C-Down).
- Camera Check DL interpretation is ~40 ms per DL (draw-state 47%); performance is next.

## v8 (2026-10-20): courses at 30 fps (Azahar)
- Game-thread switches cost ~280 us each (~2000/s in a course, over half the game core). Cause:
  moodycamel's LightweightSemaphore spins up to 10000 times before blocking. Snap's object
  processes are N64 threads handing off to each other on core 2, and the 3DS kernel doesn't
  time-slice, so the spinning waiter only delayed the thread it had just woken. Spinning is off on
  3DS (lightweightsemaphore.h): switches ~45 us.
- Beach: ~19 -> ~30 fps (the game's own cap), mostly 2-vblank frame gaps; audio underruns in the
  test run 1514 -> 103.
- Next heaviest: audio tasks ~45% of core 0 (~7.5 ms each), DL interpretation ~11 ms per frame.

## v9 (2026-10-20): CIA
- `make release` then `make cia` -> dist/psnap-3ds-v9.cia (title ID 00040000000F5A9C, product
  code CTR-H-PSNP). The ROM is packed into RomFS (romfs/rom/pokemonsnap.z64, personal use), so the
  .3dsx carries it too (22 MB); an SD ROM at sdmc:/3ds/psnap/ is no longer needed.
- HOME menu art: tools/make_banner.py builds the banner (256x128) and icon (48x48) from a title
  screen capture (assets/banner/title.png) and synthesizes a shutter-and-chime banner sound.
- Tested: the installed CIA boots in Azahar with the SD ROM removed ("ROM: OK (packed in the app)").
  Azahar installs a .cia when it's opened; the installed .app is under
  azahar/user/sdmc/Nintendo 3DS/.../title/00040000/0f5a9c00/content/.

## v10 (2026-10-20): Beach textures (user: "the beach level does not render correctly")
- Ground textures (sand, terraces, rail ties) were smeared along one axis. Snap's terrain tiles set
  clamp AND mask (e.g. cms=2 masks=6) with tile sizes up to 1024 texels: the RDP clamps at the tile
  size and wraps by the mask inside it, so the texture repeats. bind_tile treated any clamp flag as
  clamping at the texture edge. Now a tile clamps only when it fits in one mask period.
- Draw dumps (snapshots_full.txt) also print wrap modes, UVs and w of each draw's first triangle.

## v11 (2026-10-20): Oak's text (user: "the dialogue still does not appear smoothly")
- The N64 VI ran on its own 60 Hz timer; the 3DS top screen refreshes at ~59.83 Hz. As the two
  drifted, a 30 fps game frame was shown for 1 or 3 vblanks instead of 2 every so often, so the
  text reveal stuttered. The VI thread now ticks on the top screen's vblank (gspWaitForVBlank in
  events.cpp; nothing else waits on GSP's VBlank0, citro3d uses its own callback). Lab dialogue:
  every game frame exactly 2 vblanks apart. Game speed is 0.3% slower (59.83 Hz); audio underruns
  in a 150 s run 103 -> 26.
- Not the cause: SD writes by the watchdog (tested with 10 s spacing). The 4+ vblank stall in test
  runs every 180 frames is the harness's screenshot (DEV_SCREENSHOT_INTERVAL, scripted runs only).
- tools/test_run.ps1 now copies the input script (tools/autoinput_beach.txt) to the SD card only
  for its run and removes it after; a leftover autoinput.txt replayed presses in the user's runs.

## v12 (2026-10-20): Camera Check photo scoring and speed (user: "the framerate drops
significantly and you are unable to select any photos")
- Photos were never identified, so Album Mark / Oak's Mark stayed disabled (the cursor sat on a
  disabled item). Snap scores each photo (score.c, func_800A007C) by re-rendering it at 64x48 with
  a z-buffer per Pokemon (13 buffers from 0x2B9E00, cleared to 0xFFFC by a FILL), snapshotting the
  z-buffer with COPY rects before each Pokemon, then counting depth pixels on the CPU. Our depth
  lived only on the GPU. Now, for 64-wide (or smaller) off-screen targets, triangles are also
  rasterized on the CPU into the z image in RDRAM (depth_raster: screen z interpolated across the
  screen, stored as z*63 below 0xFFFC, less-than test) and FILLs into them are done in RDRAM
  (cpu_fill_rect; z-buffer fills get no GPU draw). The COPY snapshots already went through
  cpu_copy_rect. Result: "4 pictures of PIDGEY have been taken!", NEW ribbons, cursor on photos.
- Speed: every texture bind called sync_offscreen_reads, which walked all ~24 frames with set
  lookups (36 ms of ~60 ms per Camera Check frame for 1285 binds). Off-screen candidates are now
  cached as RDRAM ranges and rebuilt only when the current image / frame set changes. Camera
  Check: game frames every 4+ vblanks -> mostly 3.
- Correction to the v6/v7 notes: 0x2B8600 is the score image and 0x2B9E00.. the score z-buffers;
  the "strips" at 0x2CD600.. are the per-Pokemon z snapshots. The 104x78 thumbnail renders reuse
  the same memory afterwards.

## v13 (2026-10-20): dialogue text appears a character at a time (user, in Azahar: "The text is
supposed to smoothly appear as it does in the original N64 version")
- Snap prints dialogue into text bitmaps (224x9 strips at ~0x1C6C50) with the CPU, a character per
  frame. The texture cache trusted a 16-word fingerprint and re-hashed in full only every 8th
  display list, so new characters showed up in bursts every ~8 frames. A texture source whose
  content has changed once is now "volatile" and hashed in full every display list (SourceInfo /
  BindEntry volatile_src). The text strips now update on every display list in which the game
  prints. Course cost unchanged (~10 KB hashed per frame).
- v11's vblank-locked VI is kept (it fixed real frame-time jitter), but it wasn't this.

## Test save and all-course playtest (2026-10-21)
- tools/devsave/pokemonsnap_100.bin: every course unlocked (save data +0x50 = 6, +0x64 flags =
  D324E4A0: apple, Pester Ball, flute, Zero-One, tutorial done), written by the game itself (pokes
  tools/devsave/pokes_unlock_all.txt on a new game, then Save in the lab, tools/autoinput_makesave.txt),
  so its checksum is valid. Pokedex/album aren't filled. Save struct: pointer at 0x800C21B0 -> 0x800C21C0.
- tools/test_run.ps1 sets the user's saves folder aside for every run (saves.user) and restores it;
  -Save <file> starts from a given save, -KeepSave captures the result, -Pokes copies a pokes.txt.
  Release builds and the user's SD card never get the dev save.
- tools/test_levels.sh plays each course from that save (snapshots + log in tools/tmp/levels/).
  All 7 courses load, play to the end and render correctly (black openings in the Cave are part of
  the level, per the user). Game frames slower than 30 fps (3+ vblank gaps), share of all frames:
  Beach 11%, Tunnel 12%, Volcano 4%, River 5%, Cave 14%, Valley 11%, Rainbow Cloud 6%.
  Audio underruns per 300 s run 50-420, mostly around Camera Check (still ~20 fps there).

## Roadmap
1. Evaluation ✅
2. 3DS project skeleton: copy Megaman643DS's Makefile, runtime/, source/port, source/render; swap
   MM64-specific code for Snap (overlay table, hooks, entry); build a .3dsx.
3. OS layer: threads, message queues, PI DMA, timers, EEPROM/flash saves — boot to first display list.
4. Renderer: F3DEX2_NoN + L3DEX2 lines; fog, framebuffer effects (the camera/photo capture reads
   the framebuffer — photos are a core feature).
5. Input (camera aim on Circle Pad, zoom, Pester Ball/apple/flute), audio, pause, performance.
6. Bottom screen (design with the user: e.g. live film counter, Pokédex/report, photo album).
7. CIA packaging (banner, icon, rsf from MM64's lessons), real-hardware testing.

## v14 (2026-10-21): red in-focus dot, Camera Check at 30 fps, audio off the critical path
- Red dot: the game paints it into the finished framebuffer with the CPU
  (PokemonDetector_PostProcessImage, 157-161 x 117-121, RGBA5551 0xF801). publish() reads two of its
  pixels from RDRAM, clears the 5x5 area (stale pixels would light it again) and flags the frame;
  main.c draws it over the top screen with citro2d (n64_top_red_dot).
- Camera Check 20 -> 30 fps: ~1300 sprite rectangles per frame each got their own draw because every
  sprite re-issues identical SETTILE/LOADBLOCK/SETTILESIZE. State commands now only count as a change
  when a value changes (set_tmem_load, SETTILE/SIZE, prim/env/combine, other modes), and a rectangle
  with no change since the previous one extends its draw (rect_state_changed / last_rect). 1288
  rects -> 51 draws, rectangle time 23 -> 9 ms.
- Audio tasks are reported done when they start (at most one ahead; events.cpp task_thread_func), so
  display lists no longer wait ~7 ms behind each audio task; queue_audio_buffer waits for pending
  tasks (wait_rsp_tasks). Cave underruns 355 -> ~140.
- Tried and rejected: display-list interpretation on core 0 (UI fell to 30 fps with stalls).
- The first-run Beach tutorial stops the cart at Meowth ("Press Z to aim") until the player zooms:
  tools/autoinput_camera.txt now keeps zoom/shutter going longer.

## v15 (2026-10-21): gyro aiming, bottom screens, HOME menu art
- Scenes from the overlay descriptors dmaLoadOverlay gets (table at 0x800ABBD0, 0x24 each): 3 Oak's
  Lab, 16-29 courses (assets/code pairs: Beach, Tunnel, Cave, River, Volcano, Valley, Rainbow), 10/11/13
  title (main menu, new game, intro); 1/9/14/15 shared (snap_game.cpp track_scene, snap_scene()).
- Gyro aiming (source/port/gyro.c): angular rate (libctru dps coefficient) added to the Circle Pad,
  full deflection at 60 deg/s, 2 deg/s deadzone, only in courses; on/off on the Oak's Lab bottom
  screen, saved in DATA_PATH/settings.txt. Axis signs untested on hardware.
- Bottom screens (source/ui/bottom_ui.c): title/intro: the island (gfx/island.png); Oak's Lab: gyro
  switch; courses: faded island, course name, best report photo of the course (save +0x6C scores,
  +0x180 photos, species via s8 table 0x800AE4E4) or a blank photo with "No Score", and round item
  buttons (apple A, Pester Ball B, Poke Flute C-Down, dash R) that throb with the game's own icon
  sprite scale. The top HUD's item icons are hidden (hook on renDrawSprite, snap_on_draw_sprite).
- Photo thumbnails (photo_cache.cpp): hooks in func_80374714_847EC4 capture the CPU-averaged photo
  bitmaps (RGBA5551, odd rows XOR 4), keyed by a hash of PhotoData[8..0x3A0), saved to
  DATA_PATH/photos/ by the watchdog. Not yet verified end to end (needs a scored report photo).
- Item icons: tools/make_item_icons.py from a course RAM snapshot (28x28 RGBA32 sprites, odd rows
  XOR 8); button badges replaced by the ring band sampled 90 degrees either side (the flute: its
  reflection), ring gaps filled, cut to a circle.
- Beach tutorial: items are disabled until 3 photos are taken (game rule, IsInputDisabled).
- HOME menu: icon = Todd's photographer-card portrait; banner = the title logo rendered in 3D
  (extruded body per word, bevel, gloss, perspective) over the island (tools/make_banner.py).
- tools/test_run.ps1 sets saves/, photos/ and settings.txt aside per run and restores them, also on
  the next run if one was interrupted.

## v16 (2026-10-21): bottom screen look
- Items greyed (gfx/icon_*_off.png) until the game unlocks them: read from the course HUD itself
  (Icons_MainObject 0x80388230 set, Icons_NumItemsAvailable 0x80388238 >= 1/2/3, dash icon object at
  0x80388194), not the save flags; untappable while grey.
- Best photo top left; course name top right on a roll of film: the film counter's canister
  (gfx/film_head.png) and a seamless strip (tools/make_film.py: interior tiled forwards/mirrored,
  smoothed, sprocket holes redrawn every 4 pixels).
- All bottom-screen text in the game's 12x12 UI font: tools/make_font.py reads the glyphs from a lab
  RAM snapshot (pointers at 0x8037EA68, UIText_CharTable's full-width letters) into gfx/font.png and
  glyph widths into source/ui/game_font.h; drawn proportionally with the game's 1-pixel shadow.

## v17 (2026-10-21): Oak's Lab bottom screen background
- The lab's own interior (gfx/lab.png), dimmed, behind the settings. Assembled from the lab
  background's texture strips in a full snapshot (30 rows of 256x8 + 64x8 RGBA16 from RDRAM
  0x0E67F8). The isolate dev mode now leaves full-screen content unzoomed.

## v18-19 (2026-10-21): lab settings, widescreen, particles behind walls
- Lab: title on the film roll, see-through settings panels, ON/OFF as the lab menu's button without
  its flag tab (tools/make_ui_parts.py) with the game's orange focus brackets.
- Settings module (source/port/settings.c): gyro, widescreen in DATA_PATH/settings.txt.
- Widescreen: on VI framebuffers, full-width scissors map to all 400 pixels and full-width fills /
  depth clears reach the sides; the game's 4:3 projection already places what lies past its view's
  edges there (no projection change). 2D art stays 4:3.
- Particles (smoke, sparks, Haunter's clouds) showed through walls: they are texture rectangles with
  Z source = primitive depth (G_SETPRIMDEPTH, (z/w * vscaleZ + vtransZ) * 32, app_render/effect.c);
  rectangles were never depth tested. Now the primitive depth is turned back into the distance w
  (clip z = k * w + c from the projection slot) and the rectangle is drawn at that w with the depth
  test/update its render mode asks for (user confirmed: Tunnel, Haunter at the large door).
- An interrupted test left the 100% dev save in the user's emulator saves; restored (fresh).

## v20 (2026-10-21): widescreen film counter in the corner
- In widescreen inside a course (n64_hud_wide, set by main.c), HUD rectangles wholly in the
  framebuffer's top-right corner (x >= 62.5% of the width, bottom above 18.75%) move right by the
  side area, so the film counter sits in the screen's top-right corner.

## v21 (2026-10-21): intro audio and frame rate
- Choppy intro audio (user): the display-list interpreter ran on the game's core above the game
  threads, so during heavy display lists (~17-30 ms in the intro / attract scenes) the game's audio
  thread couldn't build its next frame in time: ~0.6 audio tasks per VI, 150 underruns in 90 s. The
  gfx thread now runs just below the game threads (ThreadPriority::Low, events.cpp): 1 audio task per
  VI, 0 underruns in the intro; Cave underruns 355 -> 0, its frame rate about the same.
- The development timers added in v12 (svcGetSystemTick around every draw, bind and rectangle: a
  system call, slow in Azahar) are gone.
- Intro/attract scenes: ~30 ms of display-list interpretation per frame, now mostly 30 fps in Azahar
  (a few slow frames where it starts); the triangle path is the next thing to speed up.

## v23 (2026-10-21): first console run (New 3DS): black top screen, no sound
- Console log (FTP 10.0.0.181:5000): boots to the game entrypoint, then crash.txt: data abort on core
  2 in check_sp_imem (sys/main.c), far 0x2C03C000. thread5_main checks SP IMEM/DMEM by reading them
  through KSEG1 (lw 0x1000(0xA4000000)): outside RDRAM. Azahar lets the bad read through; the console
  faults. Both reads are replaced by the values a booted N64 has there (6103, -1) with
  [[patches.instruction]] (0x80000484, 0x800004B4). No other direct hardware reads are left (the
  0xBF80.. lui matches are float constants; AI_LEN was already patched).
- v22: the title bottom screen shows ROM/game status and counters when the game hasn't drawn
  anything after 6 s. Stereoscopic 3D (work in progress, paused) is off unless DATA_PATH/stereotest.txt;
  its per-vertex depth works in courses (disparity checked), the Album/Camera Check photo depth doesn't
  take effect yet.

## User's next steps (2026-10-21, in order)
1. In courses the crosshair doesn't turn red when a Pokemon would register in the photo (fix).
2. Camera Check / photo selection must hold 30 fps.
3. Gyro aiming, toggled on/off from the bottom screen in Oak's Lab; applies only inside a course.
4. In-course bottom screen: apple, Pester Ball, Poke Flute and dash (Zero-One boost) as touch
   buttons that use them (removed from the top-screen HUD); top right shows the course's best photo
   with its Pokemon and score ("No Score" on a blank photo the first time); background an opaque
   image of Pokemon Island.

## v24 / v25 — real-hardware fixes (2026-10-21)
- v23: check_sp_imem/check_sp_dmem read SP memory through KSEG1 (crashed on the console); instruction patches load the expected values.
- v24: thread_queue_remove guards NULL/garbage queues (crash on the console).
- v25: freeze on taking control in the Beach. The console limits an application to 32 threads; Snap runs up to ~57 N64 threads (log: "[threads] pool thread #23 ... FAILED"). Azahar doesn't enforce the limit. Game threads are now fibers on one host thread (runtime/ultramodern/fiber_switch.s, threads.cpp, fibers.hpp); dead fibers are freed on creation and in the host loop (~22 alive in the Volcano, 865 created). Pacing is unchanged at 30 fps.
- Banner: the logo is keyed from the title screen (no longer cut off), shown over an island photo tilted to the right; the audio is the first 3 s of the title music.

## Stereoscopic 3D (resumed after v25)
- On in the intro (overlay 13), courses, Camera Check (2) and the Album (4), as deep as the 3D slider (0.05 of the half width at most). DATA_PATH/stereotest.txt still forces full depth and puts the right eye in screenshots.
- Fix: Camera Check and the Album load the course's even overlay (16, 18...) for the photos' data; that switched the scene back to COURSE (photo depth off, course bottom screen and gyro on). An even course overlay no longer ends those scenes; courses start with their odd one.
- Photos (sprite bitmaps printed by func_80374714) sit at 0.2 of the separation behind the screen, with their grey mat (an untextured rect up to 6 px wider, single or a batch of up to 8) moving with them.
- The attract intro runs in main_menu (overlay 10; 13 is intro_code, the boot), so 10 is a 3D scene; its title and menus are 2D and stay flat.
- 2D drawn before a frame's first 3D draw (a course's sky) is put at infinity, stretched by the shift so no edge is uncovered (stereo.z).
- Pop-out is capped in the shader (min(C, 2w)): nothing comes out further than the far distance goes in (the intro's boot was ~40 px apart).
- Photo bitmap ranges are cleared when a new scene overlay loads.
- v26: 3D follows the slider (16 steps). Azahar pacing at full depth was unchanged; console GPU cost untested.

## v27 — gyro aiming stabilized
- Bug: HIDUSER_GetGyroscopeRawToDpsCoefficient gives raw units per dps (14.375); the code multiplied by it, so readings were ~200x too large (shaky, twitchy aiming on the console; Azahar reads 0 at rest so it never showed).
- The rate is now bias-corrected (learned while still), smoothed, soft-deadzoned, summed between the game's stick reads (gyro_take in get_input) and handed over with the part under the game's thresholds (pitch 6, reticle 16 per pixel) carried to the next read. Full stick = 25 dps (about 1:1 with the camera).
- tools/test_run.ps1 -SettingsFile runs with a given settings.txt.

## v28 — ROM check; shared and personal builds
- `make cia`: no ROM inside (dist/psnap-3ds-vN.cia, ~3.3 MB), safe to share. `make cia-personal`: packs rom/pokemonsnap.us.z64 into the RomFS (dist/psnap-3ds-vN-personal.cia), own console only. romfs/ holds no ROM any more; cia.rsf's RootPath is $(ROMFS_ROOT).
- source/port/rom_check.c: the app's own ROM if packed; else the one checked on an earlier boot (sdmc:/3ds/psnap/rom_verified.txt: path, then SHA-1 size mtime); else any 16 MB file in /3ds/psnap/ or /roms/n64/, .z64/.v64/.n64 (non-.z64 converted into pokemonsnap.us.z64), checked against the USA SHA-1 edc7c49cc568c045fe48be0d18011c30f393cbaf. Header first: other regions / games named on the help page.
- Bottom screen: a progress bar for the first check; a help page (what was found, where to put the dump, the SHA-1) when there's no usable ROM.
- Tested in Azahar: first boot hashes, second skips; a .v64 under another name converts byte-identical; a European header shows the help page.

## v29 — "Pokemon Snap: Double Take!!"
- tools/make_subtitle.py: a film strip with "DOUBLE TAKE!!" exposed twice (yellow/blue face, red offset ghost). On the banner under the logo (make_banner.py) and on the title screen: n64_renderer.cpp add_subtitle() appends a draw after the logo's last tile ((227,99)-(286,109), overlay 10), so the game's fades and menus cover it. SMDH title "Pokemon Snap: Double Take!!".

## v30 — best photo on the bottom screen
- It never showed (a blank print): Tex3DS subtextures with top < bottom read as rotated, and the rows were stored the other way up. Rows now go in top first with top 1.0 / bottom 1 - h/256.
- Thumbnails come from the game's 2x render (hook after func_80374608, 0x80374744): 104x78 from Camera Check's grid, 280x210 from the big view, instead of the averaged 52x39 / 140x105.
- Name in red and score in blue on the print. Dev: DATA_PATH/besttest.txt (a thumbnail hash) forces the panel; test_run.ps1 -PhotosFrom / -PhotosTo.

## v31 — audio margin, 2-cycle combiner buffer
- Hardware crackling (as in the Mega Man 64 port before its v49): ultramodern buffer_offset_frames 2 -> 3 VIs (~50 ms) and limiter max gain 2.0 -> 1.7, the same fix.
- Mew's bubble in the intro (VI ~2450, draw with the I4 glow 350B38, 2-cycle) drew as an opaque teal box, in Azahar too. The PICA combiner buffer: stage k's update bit stores stage k's output and stage k+2 is the first to read it (Azahar: next_combiner_buffer). The backend set the bit of the stage before cycle 1's last, so cycle 2 read cycle 1's next-to-last result (alpha 1 - shade, before x TEX0_A). Now cycle 1's last stage writes and a pass-through stage goes before cycle 2. Only 2-cycle draws whose first cycle needs 2+ stages were affected. Beach unchanged.
- Debugging aids used: pokes "drawid 1" (flat colour = draw index; all draws opaque, so it shows the topmost, not the visible one) and "isolate lo hi" (only draws textured from that RDRAM range).

## v32 — clamped tiles larger than their mask
- Pikachu (Beach, right of the track after Doduo, VI ~5920 with tools/tmp/pika/r4/ai.txt) had a brown streak on his chest: his back-stripe texture 1090B8 (CI4 32x16) is a tile with cms = mirror|clamp, mask 5 (32) and 64 texels wide - mirrored within 32, clamped at 64. bind_tile gave the GPU mirror only (clamp was used only when the tile fits one mask period), so past the edge the whole stripe repeated.
- get_texture now decodes such tiles (clamp, mask smaller than the tile, tile <= 256 texels) at the tile's size with the wrap/mirror done on the CPU, and bind_tile clamps them. Larger ones (terrain, 64-texel masks on tiles up to 1024) keep the GPU wrap. Beach and intro unchanged otherwise.

## v33 — DANKEY KONG credits
- Intro: JACK and BEANS (tiles from (71,112), overlay 10, ~120 display lists: alpha 08..F6 in 15 steps, hold, back down) is followed by the DANKEY KONG logo (art/dankey_kong.png -> source/render/dankey_logo_rgba.h, 512x64, drawn 240 wide centred at (160,120)). The renderer records JACK and BEANS' prim alpha per display list and plays it back for the logo from the first display list without it (intro_logo_step at G_ENDDL). Cancelled when the title's copyright line is drawn (START during the intro).
- Title screen: the second copyright line (tiles at y 208-218 from x 74, text 97-221) moves 32 left and "Dankey Kong 2026" follows it in the game's own font (tools/make_dankey_assets.py: glyphs copied from the two lines or drawn to match), so the line is centred on 160 again (65-254).
- Renderer helpers: port_image / add_image (an image draw appended to the game's frame, texture alpha x a fade alpha).

## v34 — Camera Check on the touch screen
- Camera Check's photo choice (Oak's Mark / Album Mark / To Prof. Oak) moved to the bottom screen: a 4x3 grid of the photos per page (game's grey frames, NEW ribbons, small marks, orange page knobs, the menu's own three buttons, all cut from the game by tools/make_cc_parts.py), a status line in the game's font with its own messages. Tapping a photo shows it on the top screen at the game's 2x render (280x210) on Camera Check's background tile, with the big view's Oak / album marks and NEW badge (n64_renderer.cpp camera_check_step).
- How: the game stays in its menu (func_camera_check_801E1BD4). A hook after its controller read (0x801E1C84, snap_cc_menu in source/port/camera_check.cpp) hides the buttons from it and does what the touch screen asked with the game's own functions: func_camera_check_801E01C0 / 0118 (Oak's Mark, one per Pokemon, Signs with the dash engine), 0034 (Album Mark, album room), with the cursor 0x80208960 set first. It also has the game render the photos (func_80374714_847EC4): each one into the menu's hidden preview sprite (grid size, 104x78 kept by photo_cache), the chosen one into the hidden big-view sprite (280x210). "To Prof. Oak" sets the menu's cursor (sp+0x38) to that button and presses A; the game's question then shows on its own top screen and a hook in its loop (0x801E1FD4, snap_cc_confirm) presses A or B for the bottom screen's answer buttons.
- Buttons in the touch menu: D-Pad/Circle Pad choose, L/R page, A Oak's Mark, Y Album Mark, START To Prof. Oak. Not carried over: the detail view's Z comparison with the picture Oak already has.
- Scenes: a course's even (data) overlay never starts a scene (courses load 17 then 16), so Prof. Oak's Check no longer showed the course page on the bottom screen.

## v35 — "ROM needed" on the top screen

With no usable ROM the top screen was black; it now shows Pokemon Island with "Pokemon Snap / ROM
needed", the reason (same line as the bottom screen) and a pointer to the bottom screen's
instructions, in the game's font (`bottom_ui_draw_rom_top`, drawn from main.c). The top target has to
be cleared with C2D_TargetClear first: the N64 renderer clears depth to the far end, which hides 2D.
Checked in Azahar with the ROM moved off the SD (tools/tmp/v35_no_rom.png).
