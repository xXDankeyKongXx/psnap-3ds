#pragma once
// Updates from GitHub (updater.cpp): the latest release of xXDankeyKongXx/psnap-3ds, its shareable
// CIA (or .3dsx, when running from the Homebrew Launcher) downloaded, checked and installed over
// this one. Driven from the title page's Update button (bottom_ui.c).
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UPD_IDLE,         // not checked yet
    UPD_CHECKING,
    UPD_LATEST,       // nothing newer
    UPD_AVAILABLE,    // `latest` is newer: `notes` says what's in it
    UPD_DOWNLOADING,  // `percent`
    UPD_INSTALLING,   // `percent`
    UPD_DONE,         // installed: start the game again
    UPD_ERROR,        // `message`
} UpdState;

typedef struct {
    UpdState state;
    int current, latest;
    int percent;
    char message[96];
    char notes[400];
} UpdView;

// argv[0] (the .3dsx's path from the Homebrew Launcher, or NULL); `auto_check`: may check by itself.
void updater_init(const char* self_path, bool auto_check);
void updater_exit(void);
// Once per start, on the title screen: a check in the background (for the Update button's "!").
void updater_auto_check(void);
void updater_check(void);
void updater_install(void);
void updater_view(UpdView* out);
// Downloading or installing: the update page stays up whatever the game does.
bool updater_busy(void);

#ifdef __cplusplus
}
#endif
