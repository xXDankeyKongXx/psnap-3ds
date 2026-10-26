// Player settings from the Oak's Lab bottom screen, kept in DATA_PATH/settings.txt as "name=0|1" lines:
// gyro (gyro aiming in courses, gyro.c) and widescreen (the top screen's full width for 3D, n64_renderer.cpp).

#include <stdio.h>
#include <string.h>

#include "settings.h"

// The renderer reads it every display list (n64_renderer.cpp).
volatile bool n64_widescreen;

static const char* path;
static bool values[SETTING_COUNT];
static const char* const names[SETTING_COUNT] = { "gyro", "widescreen" };

static void apply(void) { n64_widescreen = values[SETTING_WIDESCREEN]; }

void settings_init(const char* settings_path) {
    path = settings_path;
    FILE* f = fopen(path, "r");
    if (f) {
        char line[64];
        while (fgets(line, sizeof(line), f)) {
            for (int i = 0; i < SETTING_COUNT; i++) {
                size_t n = strlen(names[i]);
                if (strncmp(line, names[i], n) == 0 && line[n] == '=') values[i] = line[n + 1] == '1';
            }
        }
        fclose(f);
    }
    apply();
}

bool setting(Setting s) { return values[s]; }

void setting_set(Setting s, bool on) {
    values[s] = on;
    apply();
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < SETTING_COUNT; i++) fprintf(f, "%s=%d\n", names[i], values[i] ? 1 : 0);
    fclose(f);
}
