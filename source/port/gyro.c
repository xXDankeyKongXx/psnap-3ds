// Gyro aiming: inside a course, turning the system turns the camera. Toggled on the bottom screen in
// Oak's Lab (settings.c).
//
// Pokemon Snap's stick sets how fast the camera turns, per game frame, and it has thresholds: the
// pitch ignores stick values under 6 (of 80), and the reticle moves in whole pixels, trunc(5 * stick
// / 80) a frame, so under 16 it doesn't move at all (app_level/player.c, handleAnalogStick). Fed the
// gyroscope's rate as it was at the moment the game read the stick, slow turns did nothing and then
// lurched, and sensor noise and the sensor's resting bias came straight through.
// So: the rate is sampled every UI frame (60 Hz), its resting bias removed (learned whenever the
// system is held still), smoothed and given a soft deadzone; the turn is summed between the game's
// stick reads (gyro_take, from the controller read) and handed over as one stick value, the part a
// threshold would drop carried into the next read. Slow turns then move the camera at the right
// average speed instead of not at all.

#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gyro.h"
#include "settings.h"
#include "snap_game.h"

static bool started;
static float raw_to_dps = 1.0f / 14.375f; // from the system's own coefficient (gyro_init)

void gyro_init(void) {
    if (R_SUCCEEDED(HIDUSER_EnableGyroscope())) {
        started = true;
        float coeff;
        // The coefficient is raw units per degree a second (14.375 for the sensor): divide by it.
        // (Multiplying by it made every reading ~200 times too large: shaky aiming.)
        if (R_SUCCEEDED(HIDUSER_GetGyroscopeRawToDpsCoefficient(&coeff)) && coeff > 0.0f) raw_to_dps = 1.0f / coeff;
    }
    fprintf(stderr, "[gyro] %s, aiming %s, %.4f dps per unit\n", started ? "started" : "unavailable", gyro_enabled() ? "on" : "off",
        (double)raw_to_dps);
}

bool gyro_enabled(void) { return setting(SETTING_GYRO); }

void gyro_set_enabled(bool on) { setting_set(SETTING_GYRO, on); }

void gyro_exit(void) {
    if (started) HIDUSER_DisableGyroscope();
}

// Degrees per second that equal a full stick deflection (80) at 30 game frames a second. A full
// stick pitches the camera 0.015 rad a frame (~26 degrees a second) and moves the reticle 150 pixels
// a second (~20 degrees of the view), so the camera turns about as far as the system does.
#define FULL_TURN_DPS 25.0f
// Soft deadzone: this much of the (unbiased, smoothed) rate is taken off, so holding the system
// steady by hand doesn't wobble the camera.
#define DEADZONE_DPS 1.2f
// Smoothing of the rate (share of each new sample).
#define SMOOTHING 0.35f
// Bias learning: the system counts as still while the rate stays within STILL_DPS of the bias for
// STILL_FRAMES samples; the bias then follows the raw rate by BIAS_RATE a sample.
#define STILL_DPS 3.0f
#define STILL_FRAMES 20
#define BIAS_RATE 0.04f
// The game's thresholds (handleAnalogStick): pitch moves from 6, the reticle a pixel per 16.
#define PITCH_MIN 6.0f
#define YAW_STEP 16.0f
#define STICK_MAX 80.0f

static float bias[2], smooth[2];
static int still_frames;
static bool calibrated;
static u64 last_tick;
static LightLock lock = 1; // LightLock_Init: unlocked
static float pending[2], carry[2]; // stick units x game frames; under `lock`
static bool active;                // in a course with gyro aiming on

static void reset(void) {
    LightLock_Lock(&lock);
    pending[0] = pending[1] = carry[0] = carry[1] = 0.0f;
    LightLock_Unlock(&lock);
    smooth[0] = smooth[1] = 0.0f;
    still_frames = 0;
    last_tick = 0;
}

void gyro_apply(circlePosition* circle) {
    (void)circle;
    bool on = gyro_enabled() && started && snap_scene() == SNAP_SCENE_COURSE;
    if (on != active) {
        reset();
        active = on;
    }
    if (!on) return;

    angularRate rate;
    hidGyroRead(&rate);
    // Held like a camera: turning left/right is a rotation around the system's vertical (y) axis,
    // tilting the top toward or away from you one around its horizontal (x) axis.
    float raw[2] = { -rate.y * raw_to_dps, rate.x * raw_to_dps };

    u64 now = svcGetSystemTick();
    float dt = last_tick ? (float)(now - last_tick) / (float)SYSCLOCK_ARM11 : 0.0f;
    last_tick = now;
    if (dt > 0.1f) dt = 0.1f; // a stall: don't turn by all of it

    // Resting bias: learned while the system is still (and taken whole the first time).
    bool still = fabsf(raw[0] - bias[0]) < STILL_DPS && fabsf(raw[1] - bias[1]) < STILL_DPS;
    if (!calibrated) still = fabsf(raw[0]) < 10.0f && fabsf(raw[1]) < 10.0f;
    still_frames = still ? still_frames + 1 : 0;
    if (still_frames >= STILL_FRAMES) {
        for (int i = 0; i < 2; i++) bias[i] = calibrated ? bias[i] + BIAS_RATE * (raw[i] - bias[i]) : raw[i];
        if (!calibrated) fprintf(stderr, "[gyro] resting bias %.2f, %.2f dps\n", (double)bias[0], (double)bias[1]);
        calibrated = true;
    }

    float add[2];
    for (int i = 0; i < 2; i++) {
        smooth[i] += SMOOTHING * ((raw[i] - bias[i]) - smooth[i]);
        float a = fabsf(smooth[i]) - DEADZONE_DPS;
        float dps = a > 0.0f ? (smooth[i] < 0.0f ? -a : a) : 0.0f;
        add[i] = dps / FULL_TURN_DPS * STICK_MAX * dt * 30.0f; // stick units x 30 fps game frames
    }
    LightLock_Lock(&lock);
    for (int i = 0; i < 2; i++) {
        pending[i] += add[i];
        // The game not reading (a pause, a cutscene): don't save up a turn for later.
        if (pending[i] > 1.5f * STICK_MAX) pending[i] = 1.5f * STICK_MAX;
        if (pending[i] < -1.5f * STICK_MAX) pending[i] = -1.5f * STICK_MAX;
    }
    LightLock_Unlock(&lock);
}

// One axis: the turn due plus what earlier reads couldn't hand over, as a stick value the game acts
// on (`step`: values it acts on are multiples of it; `min`: smaller ones it ignores).
static float take_axis(int i, float step, float min) {
    float total = pending[i] + carry[i];
    pending[i] = 0.0f;
    float out;
    if (fabsf(total) >= STICK_MAX) {
        out = total > 0.0f ? STICK_MAX : -STICK_MAX;
        carry[i] = 0.0f; // turning faster than the camera can: no catching up afterwards
        return out;
    }
    out = step > 1.0f ? truncf(total / step) * step : (fabsf(total) >= min ? truncf(total) : 0.0f);
    carry[i] = (total - out) * 0.98f; // what's left of a turn fades if no more comes
    // A whole step plus one: the game's float maths must not land a hair under the pixel.
    if (step > 1.0f && out != 0.0f) out += out > 0.0f ? 1.0f : -1.0f;
    return out;
}

void gyro_take(float* x, float* y) {
    if (!active) return;
    LightLock_Lock(&lock);
    float sx = take_axis(0, YAW_STEP, 0.0f);
    float sy = take_axis(1, 1.0f, PITCH_MIN);
    LightLock_Unlock(&lock);
    // Into the Circle Pad's -1..1 (82 N64 units on an axis, input.cpp), a little over so the
    // conversion's truncation lands on the value meant.
    if (sx != 0.0f) *x += (sx + (sx > 0 ? 0.5f : -0.5f)) / 82.0f;
    if (sy != 0.0f) *y += (sy + (sy > 0 ? 0.5f : -0.5f)) / 82.0f;
    if (*x > 1.0f) *x = 1.0f;
    if (*x < -1.0f) *x = -1.0f;
    if (*y > 1.0f) *y = 1.0f;
    if (*y < -1.0f) *y = -1.0f;
}
