#ifndef AUDIO_3DS_HPP
#define AUDIO_3DS_HPP

#include <cstddef>
#include <cstdint>

namespace audio3ds {
    bool init();
    void exit();

    // ultramodern audio callbacks
    void queue_samples(int16_t* samples, size_t sample_count);
    size_t get_frames_remaining();
    void set_frequency(uint32_t freq);

    void set_paused(bool paused);
    void record(const char* path, uint32_t frames); // development
    void play_ui(int which); // menu sound: 0 cursor, 1 select, 2 back

    // Development: totals since start, to check audio is flowing (and isn't silence).
    struct DebugStats { uint32_t queued_frames, dropped_frames; int peak; bool playing; };
    DebugStats debug_stats();
    // One line on the state of the sound output, for the Data page (hardware reports).
    void status_line(char* out, size_t size);
}

#endif
