#pragma once
// Gyro aiming in courses (gyro.c).
#include <3ds.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void gyro_init(void);
void gyro_exit(void);
bool gyro_enabled(void);
void gyro_set_enabled(bool on);
// Every UI frame: samples the gyroscope (inside a course with gyro aiming on), adding up the turn.
void gyro_apply(circlePosition* circle);
// At the game's controller read: hands the turn since the last read over as stick deflection,
// added to x and y (-1..1).
void gyro_take(float* x, float* y);

#ifdef __cplusplus
}
#endif
