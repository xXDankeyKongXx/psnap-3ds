#ifndef SNAP_GAME_H
#define SNAP_GAME_H

// C interface between the 3DS front end (main.c) and the C++ game runtime (snap_game.cpp).

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SNAP_BOOT_STARTED,
    SNAP_BOOT_ROM_NOT_FOUND,
    SNAP_BOOT_ROM_WRONG_FORMAT,
    SNAP_BOOT_OUT_OF_MEMORY,
} SnapBootResult;

typedef struct {
    unsigned display_lists; // graphics tasks the game submitted
    unsigned display_lists_done; // ... and finished interpreting
    unsigned frames;        // VI frames presented
    unsigned audio_tasks;   // audio microcode tasks executed
    unsigned audio_rsp_ms; // total time in the audio microcode
    unsigned audio_queued_frames, audio_dropped_frames; // sent to / dropped before NDSP
    int audio_peak;        // loudest sample so far (0 = silence)
    bool audio_playing;    // NDSP channel running
} SnapStats;

// Starts the game on its own threads and returns immediately.
SnapBootResult snap_boot(const char* rom_path, const char* data_path);

void snap_get_stats(SnapStats* out);

// Freezes the game (no VI/AI interrupts, audio paused) while the front end keeps running.
void snap_set_paused(bool paused);

// Main thread, once per frame: current 3DS controls (hidKeysHeld, Circle Pad). While the game is
// paused the N64 controller reads as idle. Mapping in input_3ds.cpp.
void snap_input_update(unsigned keys_held, int circle_x, int circle_y, bool game_paused);

void snap_shutdown(void);

void snap_audio_status(char* out, unsigned size); // one line on the sound output

// Development: function-entry trace and sampling profiler (need trace_mode in the recomp config).
void snap_trace_dump(void);
void snap_profile_start(void);
void snap_profile_dump(void);

// Development: read a 32-bit word of N64 RDRAM (KSEG0 address).
unsigned int snap_debug_peek(unsigned int address);
// Development: the overlays currently registered (ROM start of each), logged.
void snap_debug_dump_overlays(void);

// Which part of the game is running, from the scene overlays it loads (snap_on_overlay_load).
typedef enum { SNAP_SCENE_OTHER = 0, SNAP_SCENE_LAB = 1, SNAP_SCENE_COURSE = 2, SNAP_SCENE_TITLE = 3 } SnapScene;
SnapScene snap_scene(void);
int snap_course(void); // in a course: its scene id (0 Beach .. 6 Rainbow Cloud), else -1
// Scenes shown in stereoscopic 3D: courses (and the intro's course scenes), the intro, the Album and
// the Camera Check (choosing photos for Oak).
bool snap_stereo_scene(void);
// The last scene overlay loaded (index in the game's overlay table: 10 main menu / title screen).
int snap_scene_overlay(void);

#ifdef __cplusplus
}
#endif

#endif
