// Pokemon Snap 3DS front end.
// Top screen: the game, drawn by the N64 renderer (source/render).
// Bottom screen: a status page for now (Phase 6 designs the real one).
// Controls: see source/port/input_3ds.cpp; Start+Select = exit.

#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <malloc.h>
#include <unistd.h>

#include "bottom_ui.h"
#include "gyro.h"
#include "settings.h"
#include "photo_cache.h"
#include "c3d_backend.h"
#include "snap_game.h"
#include "crash_log.h"
#include "rom_check.h"
#include "log_ring.h"
#include "updater.h"

#define DATA_PATH "sdmc:/3ds/psnap"
// The ROM packed into the app itself (romfs/rom/, by `make cia-personal`): a personal build needs
// no other files. Shared builds have none; the player's own dump is found on the SD card
// (rom_check.c).
#define ROMFS_ROM_PATH "romfs:/rom/pokemonsnap.z64"

// Development aid: on scripted test runs (DATA_PATH/autoinput.txt present), write both screens to
// DATA_PATH/screenshot.bmp every few seconds.
#define DEV_SCREENSHOT_INTERVAL 180

// Bottom screen while the ROM is read through the first time (rom_check).
static C3D_RenderTarget* bottom_target;
static void rom_progress(const char* message, int percent) {
    static int shown = -1;
    if (percent == shown) return;
    shown = percent;
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(bottom_target, C2D_Color32(0x18, 0x20, 0x30, 0xFF));
    C2D_SceneBegin(bottom_target);
    bottom_ui_draw_rom_progress(message, percent);
    C3D_FrameEnd(0);
}

#ifdef DEV_SCREENSHOT_INTERVAL
// Writes the top (400x240) and bottom (320x240, centered) screens as one 400x480 24-bit BMP.
// Framebuffers are BGR8, stored rotated: pixel (x, y) is at ((x * 240) + (239 - y)) * 3.
// Development: with DATA_PATH/stereotest.txt, stereoscopic 3D runs at full depth whatever the slider
// says, and screenshots show the right eye's image where the bottom screen would be.
static int stereo_test = -1;

static void write_screenshot(const char* path) {
    const int w = 400, h = 480, row = w * 3;
    static u8 image[400 * 480 * 3];
    memset(image, 0, sizeof(image));
    u8* top = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    u8* bot = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    for (int y = 0; y < 240; y++) {
        for (int x = 0; x < 400; x++) {
            memcpy(&image[y * row + x * 3], &top[(x * 240 + (239 - y)) * 3], 3);
        }
        if (stereo_test == 1 && n64_top_target_right()) {
            u8* right = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL);
            for (int x = 0; x < 400; x++) memcpy(&image[(240 + y) * row + x * 3], &right[(x * 240 + (239 - y)) * 3], 3);
            continue;
        }
        for (int x = 0; x < 320; x++) {
            memcpy(&image[(240 + y) * row + (x + 40) * 3], &bot[(x * 240 + (239 - y)) * 3], 3);
        }
    }
    FILE* f = fopen(path, "wb");
    if (!f) return;
    u32 data_size = row * h;
    u8 header[54] = { 'B', 'M' };
    u32 file_size = 54 + data_size;
    memcpy(&header[2], &file_size, 4);
    header[10] = 54;
    header[14] = 40;
    memcpy(&header[18], &w, 4);
    memcpy(&header[22], &h, 4);
    header[26] = 1;
    header[28] = 24;
    memcpy(&header[34], &data_size, 4);
    fwrite(header, 1, sizeof(header), f);
    for (int y = h - 1; y >= 0; y--) fwrite(&image[y * row], 1, row, f); // BMP rows are bottom-up
    fclose(f);
}
#endif

// Scripted input (development): if DATA_PATH/autoinput.txt exists, each line
//   <vi> <duration> <key> [<key>...]
// holds the named keys for <duration> game VIs (60 per second of game time) starting at VI <vi>.
// Game time rather than UI frames, so scripts keep working when performance changes. Keys: A B X Y L R ZL ZR START
// SELECT UP DOWN LEFT RIGHT, STICK:<x>,<y> for the Circle Pad (-156..156) and TOUCH:<x>,<y> for the
// touch screen (0..319, 0..239), and TAP:<n> to press the line's buttons repeatedly, every n VIs
// (held for the first half of each period). DRAG:<x>,<y> moves the stylus of a touch held by an
// earlier line (without a new press), for drag tests. Lets automated test
// runs get past menus without anyone at the controls.
typedef struct {
    u32 start, duration, keys;
    s16 stick_x, stick_y;
    bool stick;
    u16 touch_x, touch_y;
    bool touch;
    bool pressed; // its keys have been reported as newly pressed
    u16 tap_period;   // TAP:<n>, 0 = hold for the whole duration
    u32 last_tap;     // index of the last tap reported as newly pressed (+1)
} AutoInputStep;

typedef struct {
    AutoInputStep steps[256];
    int count;
} AutoInput;

static void autoinput_load(AutoInput* ai, const char* path) {
    static const struct { const char* name; u32 key; } names[] = {
        { "A", KEY_A }, { "B", KEY_B }, { "X", KEY_X }, { "Y", KEY_Y }, { "L", KEY_L }, { "R", KEY_R },
        { "ZL", KEY_ZL }, { "ZR", KEY_ZR }, { "START", KEY_START }, { "SELECT", KEY_SELECT },
        { "UP", KEY_DUP }, { "DOWN", KEY_DDOWN }, { "LEFT", KEY_DLEFT }, { "RIGHT", KEY_DRIGHT },
    };
    ai->count = 0;
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[160];
    while (ai->count < 256 && fgets(line, sizeof(line), f)) {
        AutoInputStep s = { 0 };
        int used = 0;
        if (sscanf(line, "%lu %lu%n", &s.start, &s.duration, &used) < 2) continue;
        char* tok = strtok(line + used, " \t\r\n");
        for (; tok; tok = strtok(NULL, " \t\r\n")) {
            int x, y;
            if (sscanf(tok, "STICK:%d,%d", &x, &y) == 2) {
                s.stick = true; s.stick_x = x; s.stick_y = y;
                continue;
            }
            int period;
            if (sscanf(tok, "TAP:%d", &period) == 1) {
                s.tap_period = (u16)(period > 1 ? period : 2);
                continue;
            }
            if (sscanf(tok, "DRAG:%d,%d", &x, &y) == 2) {
                s.touch = true; s.touch_x = x; s.touch_y = y; // moves a touch another line holds
                continue;
            }
            if (sscanf(tok, "TOUCH:%d,%d", &x, &y) == 2) {
                s.touch = true; s.touch_x = x; s.touch_y = y;
                s.keys |= KEY_TOUCH;
                continue;
            }
            for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
                if (!strcmp(tok, names[i].name)) s.keys |= names[i].key;
            }
        }
        ai->steps[ai->count++] = s;
    }
    fclose(f);
    fprintf(stderr, "[autoinput] %d steps from %s\n", ai->count, path);
}

static void autoinput_apply(AutoInput* ai, u32 vi, u32* held, u32* down, circlePosition* circle, touchPosition* touch) {
    for (int i = 0; i < ai->count; i++) {
        AutoInputStep* s = &ai->steps[i];
        if (vi < s->start || vi >= s->start + s->duration) continue;
        if (s->tap_period) {
            u32 t = vi - s->start;
            if (t % s->tap_period < s->tap_period / 2u) {
                *held |= s->keys;
                u32 tap = t / s->tap_period + 1;
                if (s->last_tap != tap) {
                    *down |= s->keys;
                    s->last_tap = tap;
                }
            }
        } else {
            *held |= s->keys;
            if (!s->pressed) {
                *down |= s->keys;
                s->pressed = true;
            }
        }
        if (s->stick) { circle->dx = s->stick_x; circle->dy = s->stick_y; }
        if (s->touch) { touch->px = s->touch_x; touch->py = s->touch_y; }
    }
}

// Development: memory pokes for reverse engineering (DATA_PATH/pokes.txt). Each line
//   <vi> <address> <size 1|2|4> <value>      (address and value in hex)
// writes the value into game RAM once the game's VI count reaches <vi>. Commands instead of an
// address: "<vi> texlog <n>" logs the next n texture decodes, "<vi> isolate <lo> <hi>" keeps only
// draws textured from RDRAM [lo, hi), "<vi> audiorec <frames>",
// "<vi> tracedump <calls>", "<vi> overlays 1".
typedef struct {
    u32 vi, address, size, value;
    bool done;
} Poke;
static Poke pokes[512];
static int poke_count;

enum { POKE_ISOLATE = 4, POKE_OVERLAYS = 6, POKE_TRACEDUMP = 7, POKE_AUDIOREC = 9, POKE_TEXLOG = 11, POKE_TEXNOCACHE = 12, POKE_MAGENTA = 13, POKE_DRAWID = 14, POKE_NOCULL = 15, POKE_DLTRACE = 16 };

static void pokes_load(const char* path) {
    static const struct { const char* name; u32 code; } commands[] = {
        { "texlog", POKE_TEXLOG }, { "texnocache", POKE_TEXNOCACHE }, { "magenta", POKE_MAGENTA }, { "drawid", POKE_DRAWID }, { "nocull", POKE_NOCULL }, { "dltrace", POKE_DLTRACE }, { "audiorec", POKE_AUDIOREC },
        { "tracedump", POKE_TRACEDUMP }, { "overlays", POKE_OVERLAYS },
    };
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[96];
    while (poke_count < 512 && fgets(line, sizeof(line), f)) {
        Poke p = { 0 };
        char op[16];
        if (sscanf(line, "%lu %lx %lu %lx", &p.vi, &p.address, &p.size, &p.value) == 4) {
            pokes[poke_count++] = p;
        } else if (sscanf(line, "%lu isolate %lx %lx", &p.vi, &p.value, &p.size) == 3) {
            p.address = POKE_ISOLATE; // value..size = texture source range
            pokes[poke_count++] = p;
        } else if (sscanf(line, "%lu %15s %lu", &p.vi, op, &p.value) == 3) {
            for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
                if (strcmp(op, commands[i].name)) continue;
                p.address = commands[i].code;
                p.size = 200; // marks a command
                pokes[poke_count++] = p;
            }
        }
    }
    fclose(f);
    fprintf(stderr, "[pokes] %d from %s\n", poke_count, path);
}

u8* snap_debug_rdram(void);

static void pokes_apply(u32 vi) {
    u8* rdram = snap_debug_rdram();
    if (!rdram) return;
    for (int i = 0; i < poke_count; i++) {
        Poke* p = &pokes[i];
        if (p->done || vi < p->vi) continue;
        p->done = true;
        if (p->address == POKE_ISOLATE) {
            extern volatile uint32_t n64_isolate_lo, n64_isolate_hi;
            n64_isolate_lo = p->value;
            n64_isolate_hi = p->size;
            continue;
        }
        if (p->size == 200) {
            switch (p->address) {
                case POKE_TEXLOG: { extern volatile int n64_texlog_remaining; n64_texlog_remaining = (int)p->value; break; }
                case POKE_TEXNOCACHE: { extern volatile bool n64_tex_nocache; n64_tex_nocache = p->value != 0; break; }
                case POKE_MAGENTA: { extern volatile bool n64_debug_magenta; n64_debug_magenta = p->value != 0; break; }
                case POKE_DRAWID: { extern volatile bool n64_debug_drawid; n64_debug_drawid = p->value != 0; break; }
                case POKE_NOCULL: { extern volatile bool n64_debug_nocull; n64_debug_nocull = p->value != 0; break; }
                case POKE_DLTRACE: { extern volatile int n64_dltrace_dl; n64_dltrace_dl = (int)p->value; break; }
                case POKE_AUDIOREC: { extern void snap_debug_record_audio(unsigned frames); snap_debug_record_audio((unsigned)p->value); break; }
                case POKE_TRACEDUMP: { extern void snap_trace_unique(unsigned count); snap_trace_unique((unsigned)p->value); break; }
                case POKE_OVERLAYS: snap_debug_dump_overlays(); break;
            }
            continue;
        }
        for (u32 k = 0; k < p->size; k++) { // big-endian value, RDRAM bytes stored as addr ^ 3
            u8 byte = (u8)(p->value >> (8 * (p->size - 1 - k)));
            rdram[((p->address & 0x00FFFFFF) + k) ^ 3] = byte;
        }
        fprintf(stderr, "[pokes] vi %lu: %08lX <- %lX (%lu bytes)\n", vi, p->address, p->value, p->size);
    }
}

void ultramodern_debug_dump_waits(void); // runtime/ultramodern/mesgqueue.cpp
int32_t osViGetCurrentFramebuffer(void); // runtime/ultramodern/events.cpp
int32_t osViGetNextFramebuffer(void);
void ultramodern_debug_dump_events(void); // runtime/ultramodern/events.cpp
void ultramodern_debug_dump_run_queue(u8* rdram); // runtime/ultramodern/threadqueue.cpp
void ultramodern_debug_dump_native(void); // runtime/ultramodern/events.cpp

// Watchdog (development): logs where the UI loop is and whether the game is progressing, from a
// thread above every other app thread, so a stalled UI or game shows up in runtime.log.
static volatile int ui_stage;   // 0 input, 1 frame begin, 2 top render, 3 bottom, 4 frame end
static volatile u32 ui_frame;
// The UI thread's last stats line, written to DATA_PATH/status.txt by the watchdog thread: the UI
// thread does no file I/O during play.
static char status_line[160];
// Development runs (DATA_PATH/autoinput.txt or devlog.txt present) get the full report every 2 s;
// otherwise the watchdog logs one line.
static bool dev_logging;

static void watchdog(void* arg) {
    (void)arg;
    for (;;) {
        svcSleepThread(2000000000LL);
        photo_cache_flush(); // new photo thumbnails to the SD card (bottom screen)
        FILE* status = fopen(DATA_PATH "/status.txt", "w");
        if (status) {
            fputs(status_line, status);
            fclose(status);
        }
        SnapStats s;
        snap_get_stats(&s);
        extern unsigned snap_thread_resumes(void); // snap_game.cpp: game thread switches so far
        extern unsigned snap_switch_latency_us(void);
        fprintf(stderr, "[watchdog] ui_frame=%lu ui_stage=%d display_lists=%u done=%u vi_frames=%u audio_tasks=%u audio_rsp_ms=%u thread_switches=%u switch_us=%u\n",
            ui_frame, ui_stage, s.display_lists, s.display_lists_done, s.frames, s.audio_tasks, s.audio_rsp_ms, snap_thread_resumes(),
            snap_switch_latency_us());
        if (!dev_logging) {
            log_ring_drain();
            continue;
        }
        extern void n64_renderer_mem_report(void);
        n64_renderer_mem_report();
        struct mallinfo mi = mallinfo();
        extern char* fake_heap_end;
        char* brk = (char*)sbrk(0);
        fprintf(stderr, "[mem] heap free %lu KB (unused %lu KB + freed %lu KB), linear free %lu KB\n",
            (unsigned long)(((fake_heap_end - brk) + mi.fordblks) / 1024), (unsigned long)((fake_heap_end - brk) / 1024),
            (unsigned long)(mi.fordblks / 1024), (unsigned long)(linearSpaceFree() / 1024));
        ultramodern_debug_dump_native();
        fprintf(stderr, "[audio] queued=%u dropped=%u peak=%d playing=%d\n",
            s.audio_queued_frames, s.audio_dropped_frames, s.audio_peak, (int)s.audio_playing);
        snap_debug_dump_overlays();
        snap_trace_dump();
        ultramodern_debug_dump_waits();
        ultramodern_debug_dump_run_queue(snap_debug_rdram());
        snap_profile_dump();
        fprintf(stderr, "[vi] current=%08lX next=%08lX\n", (unsigned long)osViGetCurrentFramebuffer(), (unsigned long)osViGetNextFramebuffer());
        log_ring_drain();
    }
}

int main(int argc, char** argv) {
    osSetSpeedupEnable(true); // New 3DS: 804 MHz + L2 cache (no-op on Old 3DS)
    mkdir("sdmc:/3ds", 0777);
    mkdir(DATA_PATH, 0777);

    // Runtime diagnostics go to stderr; keep them on the SD card. Unbuffered while starting up, so
    // the log shows how far it got if it stops early; after 600 frames it's buffered in memory and
    // written out by the watchdog thread every 2 s, so logging never makes the UI thread wait.
    freopen(DATA_PATH "/runtime.log", "w", stderr);
    setvbuf(stderr, NULL, _IONBF, 0);

    crash_log_init();
    crash_log_attach();
    gfxInitDefault();
    romfsInit();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4); // N64 frames can have hundreds of draw calls
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    bottom_ui_init();
    settings_init(DATA_PATH "/settings.txt");
    gyro_init();
    photo_cache_init(DATA_PATH);
    n64_top_init();

    bool is_new3ds = false;
    APT_CheckNew3DS(&is_new3ds);
    // The app's own ROM (a personal build), else the player's from the SD card.
    bottom_target = bottom;
    // Pokemon Island until the ROM check and the game's start-up are done (a black screen otherwise).
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(bottom, C2D_Color32(0x18, 0x20, 0x30, 0xFF));
    C2D_SceneBegin(bottom);
    bottom_ui_draw_idle();
    C3D_FrameEnd(0);
    static RomCheck rom;
    rom_check(&rom, DATA_PATH, ROMFS_ROM_PATH, rom_progress);
    const char* rom_state = rom.state;
    fprintf(stderr, "[front] Pokemon Snap 3DS v" SNAP_VERSION ", hardware: %s, ROM: %s\n", is_new3ds ? "New 3DS" : "Old 3DS", rom_state);

    static const char* boot_msgs[] = { "running", "ROM not found", "ROM not .z64", "out of memory" };
    SnapBootResult boot = SNAP_BOOT_ROM_NOT_FOUND;
    if (rom.ok) boot = snap_boot(rom.path, DATA_PATH);
    fprintf(stderr, "[front] game runtime: %s\n", boot_msgs[boot]);
    threadCreate(watchdog, NULL, 16 * 1024, 0x18, -2, true);
    snap_profile_start();

    extern volatile bool ultramodern_ui_busy; // runtime/ultramodern/events.cpp
    u32 frame = 0;

    static AutoInput autoinput; // ~7 KB: keep it off the main thread's stack
    autoinput_load(&autoinput, DATA_PATH "/autoinput.txt");
    pokes_load(DATA_PATH "/pokes.txt");
    FILE* devlog = fopen(DATA_PATH "/devlog.txt", "r");
    dev_logging = autoinput.count > 0 || devlog;
    if (devlog) fclose(devlog);
    // Test runs never ask GitHub by themselves (its API allows 60 requests an hour).
    updater_init(argc > 0 ? argv[0] : NULL, !dev_logging);

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        u32 held = hidKeysHeld();
        circlePosition circle;
        hidCircleRead(&circle);
        touchPosition touch;
        hidTouchRead(&touch);
        SnapStats input_stats;
        snap_get_stats(&input_stats);
        autoinput_apply(&autoinput, input_stats.frames, &held, &down, &circle, &touch);
        pokes_apply(input_stats.frames);

        if ((held & KEY_START) && (held & KEY_SELECT)) break;
        gyro_apply(&circle);
        bottom_ui_input(down, &touch);
        held |= bottom_ui_keys();
        // Stereoscopic 3D in the scenes that have it, as deep as the 3D slider says (0.05 of the
        // screen's half width at most, ~10 pixels).
        if (stereo_test < 0) stereo_test = access(DATA_PATH "/stereotest.txt", F_OK) == 0;
        float slider = stereo_test ? 1.0f : roundf(osGet3DSliderState() * 16.0f) / 16.0f; // steady steps
        n64_top_set_stereo(snap_stereo_scene() && slider > 0.01f ? 0.05f * slider : 0.0f);
        extern volatile bool n64_photo_depth; // n64_renderer.cpp
        n64_photo_depth = snap_stereo_scene() && snap_scene() != SNAP_SCENE_COURSE;
        extern volatile bool n64_title_screen; // n64_renderer.cpp: the "Double Take!!" subtitle
        n64_title_screen = snap_scene_overlay() == 10;
        extern volatile bool n64_hud_wide; // n64_renderer.cpp
        n64_hud_wide = setting(SETTING_WIDESCREEN) && snap_scene() == SNAP_SCENE_COURSE;
        snap_input_update(held, circle.dx, circle.dy, false);
        frame++;
        ui_frame = frame;
        // From here on the log is kept in memory and written out by the watchdog (log_ring.c).
        // (Development: with DATA_PATH/logdirect.txt the log keeps going straight to the SD card,
        // so the last lines before a freeze are kept.)
        if (frame == 600 && access(DATA_PATH "/logdirect.txt", F_OK) != 0) log_ring_start(DATA_PATH "/runtime.log");
        ui_stage = 0;

        SnapStats stats;
        snap_get_stats(&stats);
        unsigned draws, vertices;
        n64_top_stats(&draws, &vertices);

        // Development: when the game stops sending display lists for 2 s, log the frame on screen
        // once per stall.
        static unsigned last_dl_count;
        static u32 dl_still_frames;
        static bool stall_dumped;
        if (stats.display_lists != last_dl_count) {
            last_dl_count = stats.display_lists;
            dl_still_frames = 0;
            stall_dumped = false;
        } else if (++dl_still_frames == 120 && !stall_dumped) {
            stall_dumped = true;
            n64_top_dump_current();
            ultramodern_debug_dump_events();
        }

        snprintf(status_line, sizeof(status_line), "ui_frame=%lu display_lists=%u vi_frames=%u audio_tasks=%u draws=%u vertices=%u\n",
            frame, stats.display_lists, stats.frames, stats.audio_tasks, draws, vertices);

        ui_stage = 1;
        n64_offscreen_before_frame();
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        ultramodern_ui_busy = true; // audio tasks wait for the end of this frame (events.cpp)
        ui_stage = 2;
        bool fresh = n64_top_render(false);

        // Frame pacing (development): gaps between UI frames and between new game frames, in vblanks,
        // logged every 120 UI frames. A steady game frame rate shows as one dominant gap.
        {
            static u64 last_ui, last_fresh;
            static u32 ui_gaps[5], fresh_gaps[5], n;
            u64 now = svcGetSystemTick(), vb = SYSCLOCK_ARM11 / 60;
            if (last_ui) {
                u32 g = (u32)((now - last_ui + vb / 2) / vb);
                ui_gaps[g > 4 ? 4 : g]++;
            }
            last_ui = now;
            if (fresh) {
                if (last_fresh) {
                    u32 g = (u32)((now - last_fresh + vb / 2) / vb);
                    fresh_gaps[g > 4 ? 4 : g]++;
                }
                last_fresh = now;
            }
            if (++n == 120) {
                fprintf(stderr, "[pace] ui gaps 1:%lu 2:%lu 3:%lu 4+:%lu | game frame gaps 1:%lu 2:%lu 3:%lu 4+:%lu\n",
                    ui_gaps[1], ui_gaps[2], ui_gaps[3], ui_gaps[4], fresh_gaps[1], fresh_gaps[2], fresh_gaps[3], fresh_gaps[4]);
                memset(ui_gaps, 0, sizeof(ui_gaps));
                memset(fresh_gaps, 0, sizeof(fresh_gaps));
                n = 0;
            }
        }
        ui_stage = 3;
        C2D_Prepare(); // the N64 renderer changed shader/attribute/TexEnv state
        if (n64_top_red_dot()) {
            // The game's in-focus dot (N64 157-161 x 117-121, corners cut), at 1:1 in the 400-wide screen.
            u32 red = C2D_Color32(0xFF, 0x00, 0x00, 0xFF);
            C2D_SceneBegin(n64_top_target());
            C2D_DrawRectSolid(198, 117, 0.9f, 3, 5, red);
            C2D_DrawRectSolid(197, 118, 0.9f, 5, 3, red);
            if (n64_top_target_right()) {
                C2D_SceneBegin(n64_top_target_right());
                C2D_DrawRectSolid(198, 117, 0.9f, 3, 5, red);
                C2D_DrawRectSolid(197, 118, 0.9f, 5, 3, red);
            }
        }
        if (!rom.ok) {
            // Cleared the 2D way: the N64 renderer clears depth to the far end, which hides 2D.
            C2D_TargetClear(n64_top_target(), C2D_Color32(0, 0, 0, 0xFF));
            C2D_SceneBegin(n64_top_target());
            bottom_ui_draw_rom_top(rom.state);
            if (n64_top_target_right()) {
                C2D_TargetClear(n64_top_target_right(), C2D_Color32(0, 0, 0, 0xFF));
                C2D_SceneBegin(n64_top_target_right());
                bottom_ui_draw_rom_top(rom.state);
            }
        }
        C2D_TargetClear(bottom, C2D_Color32(0x18, 0x20, 0x30, 0xFF));
        C2D_SceneBegin(bottom);
        if (rom.ok) bottom_ui_draw(rom_state, boot_msgs[boot], &stats);
        else bottom_ui_draw_rom_help(rom.state, rom.found);
        ui_stage = 4;
        C3D_FrameEnd(0);
        ultramodern_ui_busy = false;

#ifdef DEV_SCREENSHOT_INTERVAL
        if (autoinput.count > 0 && frame % DEV_SCREENSHOT_INTERVAL == 0) write_screenshot(DATA_PATH "/screenshot.bmp");
        // Development: RAM snapshots + screenshots at listed game VI counts (DATA_PATH/snapshots.txt,
        // one VI count per line); with DATA_PATH/snapshots_full.txt also RAM, textures and draws.
        static u32 snapshot_frames[256];
        static int snapshot_count = -1;
        static bool snapshot_full;
        if (snapshot_count < 0) {
            snapshot_count = 0;
            FILE* full = fopen(DATA_PATH "/snapshots_full.txt", "r");
            if (full) {
                snapshot_full = true;
                fclose(full);
            }
            FILE* list = fopen(DATA_PATH "/snapshots.txt", "r");
            if (list) {
                while (snapshot_count < 256 && fscanf(list, "%lu", &snapshot_frames[snapshot_count]) == 1) snapshot_count++;
                fclose(list);
            }
        }
        for (int i = 0; i < snapshot_count; i++) {
            if (snapshot_frames[i] == 0 || stats.frames < snapshot_frames[i]) continue;
            u32 vi = snapshot_frames[i];
            snapshot_frames[i] = 0; // taken
            char path[96];
            snprintf(path, sizeof(path), DATA_PATH "/snap_%lu.bmp", vi);
            write_screenshot(path);
            if (!snapshot_full) continue;
            snprintf(path, sizeof(path), DATA_PATH "/snap_%lu", vi);
            mkdir(path, 0777);
            n64_top_dump_textures(path);
            n64_top_dump_current();
            snprintf(path, sizeof(path), DATA_PATH "/snap_%lu.bin", vi);
            FILE* f = fopen(path, "wb");
            if (f) {
                fwrite(snap_debug_rdram(), 1, 0x400000, f); // the game's 4 MB
                fclose(f);
            }
        }
#endif
    }

    // Leaving: pause the game first (its threads would run through the teardown and crash), then
    // tell APT the app is closing and end the process with every thread (as in the MM64 port, v33).
    fprintf(stderr, "[front] exiting\n");
    snap_set_paused(true);
    for (int i = 0; i < 6; i++) svcSleepThread(16666667LL); // ~100 ms
    fflush(stderr);
    log_ring_drain();
    snap_shutdown();
    n64_top_exit();
    updater_exit();
    bottom_ui_exit();
    gyro_exit();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    aptExit();
    svcExitProcess();
}
