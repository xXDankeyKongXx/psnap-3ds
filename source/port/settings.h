#pragma once
// Player settings (settings.c), saved in DATA_PATH/settings.txt.
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SETTING_GYRO, SETTING_WIDESCREEN, SETTING_COUNT } Setting;

void settings_init(const char* settings_path);
bool setting(Setting s);
void setting_set(Setting s, bool on);

#ifdef __cplusplus
}
#endif
