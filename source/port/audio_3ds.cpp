// N64 audio -> 3DS sound hardware for ultramodern's audio callbacks.
//
// The game's audio thread produces interleaved stereo s16 chunks (one per VI, roughly) through the
// AI. "Frames remaining" must be accurate: N64 games use it to decide how much audio to synthesize
// next. Two ways out:
// - NDSP (the DSP): each chunk goes into a free wave buffer on channel 0. On a real console the DSP
//   needs its firmware dump, sdmc:/3ds/dspfirm.cdc (made by DSP1); emulators get a blank image.
// - CSND, when there's no DSP firmware (so the app needs no extra files): the sound hardware plays
//   two looping mono ring buffers (left and right) and the chunks are written ahead of it. CSND
//   doesn't report where it is playing, so the position comes from the system clock (CSND's sample
//   timer runs off the same clock, so they don't drift).

#include <3ds.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "audio_3ds.hpp"
#include "ui_cursor_bin.h"
#include "ui_select_bin.h"
#include "ui_back_bin.h"

namespace {

enum class Backend { None, Ndsp, Csnd };
Backend backend = Backend::None;
std::mutex audio_mutex;
uint32_t rate = 32000;
bool paused = false;
uint32_t queued_frames = 0, dropped_frames = 0;
int peak = 0;
FILE* rec_file;          // development: recording the output (audio3ds::record)
uint32_t rec_remaining;  // frames still to record

// --- NDSP ---
constexpr int channel = 0;
constexpr int num_buffers = 24;
constexpr size_t buffer_frames = 4096; // per wave buffer (stereo frames)
ndspWaveBuf wave_bufs[num_buffers];
int16_t* buffer_data[num_buffers];
int next_buffer = 0;
// The bottom screen's menu sounds (the game's own, recorded: tools/make_ui_sounds.py) play on a
// channel of their own, so they're heard while the game (channel 0) is paused.
constexpr int ui_channel = 1;
ndspWaveBuf ui_buf;
int16_t* ui_data[3];     // stereo (NDSP) or mono (CSND)
uint32_t ui_frames[3];
// On a real console the game's channel stayed silent until the first menu sound played (a buffer
// on the menu channel got the DSP going). So a short silent buffer goes out there at the start, and
// again whenever the game's channel stops moving with audio queued.
constexpr uint32_t kick_frames = 256;
int16_t* kick_data;
uint32_t stall_chunks, last_pos;
uint32_t kicks;

// Called (locked) for every chunk: if the game's channel has audio queued but hasn't moved for
// ~0.5 s, it's stuck (seen on a real console: all 24 buffers queued, none ever played). Its queue is
// cleared, which makes NDSP resynchronise the channel with the DSP, and the buffers start over.
void ndsp_kick();
void ndsp_check_stall() {
    if (paused) {
        stall_chunks = 0;
        return;
    }
    int in_flight = 0;
    for (int i = 0; i < num_buffers; i++) {
        in_flight += wave_bufs[i].status == NDSP_WBUF_QUEUED || wave_bufs[i].status == NDSP_WBUF_PLAYING;
    }
    u32 pos = ndspChnGetSamplePos(channel);
    bool moving = ndspChnIsPlaying(channel) && pos != last_pos;
    last_pos = pos;
    stall_chunks = (moving || !in_flight) ? 0 : stall_chunks + 1;
    if (stall_chunks < 30) return;
    stall_chunks = 0;
    fprintf(stderr, "[audio] ndsp channel stuck (%d buffers queued, DSP frame %lu): restarting it\n", in_flight,
        (unsigned long)ndspGetFrameCount());
    ndspChnWaveBufClear(channel);
    for (int i = 0; i < num_buffers; i++) wave_bufs[i].status = NDSP_WBUF_FREE;
    next_buffer = 0;
    ndspChnSetPaused(channel, false);
    ndsp_kick();
}

void ndsp_kick() {
    if (!kick_data) return;
    ndspChnWaveBufClear(ui_channel);
    memset(&ui_buf, 0, sizeof(ui_buf));
    ui_buf.data_pcm16 = kick_data;
    ui_buf.nsamples = kick_frames;
    ndspChnWaveBufAdd(ui_channel, &ui_buf);
    kicks++;
}

// --- CSND ---
constexpr uint32_t ring_frames = 16384; // per channel, about half a second
constexpr uint32_t lead_frames = 1600;  // how far ahead of the hardware writing restarts after a gap
int csnd_chn[3];                        // left, right, menu sounds
int16_t* ring[2];
uint64_t written;                       // frames written since the channels started
uint64_t start_tick;                    // when they started
double csnd_rate;                       // the rate the hardware actually plays at

// The game mixes quietly most of the time (often ~40% of full scale), which is faint on the 3DS
// speakers, but some moments reach nearly full scale - so a fixed boost clipped them (crackling). A
// limiter instead: up to 1.7x, reduced per chunk so the chunk's loudest sample stays under the ceiling.
// Gain drops are ramped over the first frames of a chunk (no step in the waveform) and come back
// slowly (about 1 s from 1x to 1.7x), so the level doesn't pump.
constexpr float max_gain = 1.7f; // 2x also doubled the mix's own hiss (static on the console)
constexpr float gain_ceiling = 30500.0f;
constexpr float gain_release = 0.01f;  // per chunk (~60 per second)
constexpr size_t gain_ramp = 64;       // frames
float gain = max_gain;
uint32_t underruns;                    // times the DSP ran out of audio (each one is an audible gap)

// Converts n stereo frames from RDRAM order (each pair's halves swapped) into out, with the limiter.
void convert(const int16_t* in, int16_t* out, size_t n) {
    int raw_peak = 1;
    for (size_t i = 0; i < n * 2; i++) raw_peak = std::max(raw_peak, std::abs((int)in[i]));
    float target = std::min(max_gain, gain_ceiling / raw_peak);
    float from = gain;
    float to = target < gain ? target : std::min(target, gain + gain_release);
    gain = to;
    for (size_t i = 0; i < n; i++) {
        float g = i < gain_ramp && from > to ? from + (to - from) * (float)(i + 1) / gain_ramp : to;
        int l = (int)(in[i * 2 + 1] * g), r = (int)(in[i * 2 + 0] * g);
        out[i * 2 + 0] = (int16_t)std::clamp(l, -32768, 32767);
        out[i * 2 + 1] = (int16_t)std::clamp(r, -32768, 32767);
        peak = std::max({ peak, std::abs((int)out[i * 2]), std::abs((int)out[i * 2 + 1]) });
    }
}

void record_frames(const int16_t* stereo, size_t n) {
    if (!rec_file) return;
    uint32_t w = (uint32_t)std::min<size_t>(n, rec_remaining);
    fwrite(stereo, sizeof(int16_t) * 2, w, rec_file);
    rec_remaining -= w;
    if (!rec_remaining) {
        fclose(rec_file);
        rec_file = nullptr;
    }
}

const u8* const ui_clips[3] = { ui_cursor_bin, ui_select_bin, ui_back_bin };
const u32 ui_sizes[3] = { ui_cursor_bin_size, ui_select_bin_size, ui_back_bin_size };

uint64_t csnd_played() {
    return (uint64_t)((double)(svcGetSystemTick() - start_tick) * csnd_rate / SYSCLOCK_ARM11);
}

// (Re)starts both stream channels at the start of the (silent) ring.
void csnd_start() {
    for (int i = 0; i < 2; i++) {
        memset(ring[i], 0, ring_frames * 2);
        GSPGPU_FlushDataCache(ring[i], ring_frames * 2);
    }
    u32 timer = CSND_TIMER(rate);
    csnd_rate = (double)0x3FEC3FC / timer;
    for (int i = 0; i < 2; i++) {
        u32 flags = SOUND_CHANNEL(csnd_chn[i]) | SOUND_FORMAT_16BIT | SOUND_REPEAT | SOUND_LINEAR_INTERP | SOUND_ENABLE | (timer << 16);
        u32 vol = CSND_VOL(1.0f, i == 0 ? -1.0f : 1.0f);
        u32 pa = osConvertVirtToPhys(ring[i]);
        CSND_SetChnRegs(flags, pa, pa, ring_frames * 2, vol, vol);
    }
    for (int i = 0; i < 2; i++) CSND_SetPlayState(csnd_chn[i], 1);
    csndExecCmds(true);
    start_tick = svcGetSystemTick();
    written = lead_frames;
}

void csnd_stop() {
    for (int i = 0; i < 2; i++) CSND_SetPlayState(csnd_chn[i], 0);
    csndExecCmds(true);
}

bool csnd_init() {
    if (R_FAILED(csndInit())) {
        fprintf(stderr, "[audio] csndInit failed - audio disabled\n");
        return false;
    }
    int n = 0;
    for (int c = 0; c < 32 && n < 3; c++) {
        if (csndChannels & BIT(c)) csnd_chn[n++] = c;
    }
    if (n < 3) {
        fprintf(stderr, "[audio] CSND: only %d channels - audio disabled\n", n);
        csndExit();
        return false;
    }
    for (int i = 0; i < 2; i++) {
        ring[i] = static_cast<int16_t*>(linearAlloc(ring_frames * 2));
        if (!ring[i]) return false;
    }
    // Menu sounds as mono (CSND channels are mono).
    for (int i = 0; i < 3; i++) {
        uint32_t frames = ui_sizes[i] / 4;
        ui_data[i] = static_cast<int16_t*>(linearAlloc(frames * 2));
        if (!ui_data[i]) continue;
        const int16_t* src = reinterpret_cast<const int16_t*>(ui_clips[i]);
        for (uint32_t f = 0; f < frames; f++) ui_data[i][f] = (int16_t)((src[f * 2] + src[f * 2 + 1]) / 2);
        GSPGPU_FlushDataCache(ui_data[i], frames * 2);
        ui_frames[i] = frames;
    }
    csnd_start();
    fprintf(stderr, "[audio] CSND (no DSP firmware): channels %d %d %d, %.1f Hz\n", csnd_chn[0], csnd_chn[1], csnd_chn[2], csnd_rate);
    return true;
}

// Writes stereo frames (already in L/R order) into the rings ahead of the hardware.
void csnd_queue(const int16_t* stereo, size_t frames) {
    uint64_t played = csnd_played();
    if (written < played + 256) written = played + lead_frames; // fell behind (or a gap): catch up
    if (written + frames > played + ring_frames - 512) {        // too far ahead: no room
        dropped_frames += frames;
        return;
    }
    for (int ch = 0; ch < 2; ch++) {
        int16_t* r = ring[ch];
        uint32_t pos = (uint32_t)(written % ring_frames);
        size_t left = frames;
        const int16_t* in = stereo + ch;
        while (left) {
            size_t n = std::min<size_t>(left, ring_frames - pos);
            for (size_t i = 0; i < n; i++) r[pos + i] = in[i * 2];
            GSPGPU_FlushDataCache(r + pos, n * 2);
            in += n * 2;
            left -= n;
            pos = (pos + n) % ring_frames;
        }
    }
    written += frames;
}

} // namespace

// A menu sound: 0 cursor, 1 select, 2 back (cuts off one still playing).
void audio3ds::play_ui(int which) {
    if (which < 0 || which > 2 || !ui_data[which]) return;
    std::lock_guard lock{ audio_mutex };
    if (backend == Backend::Ndsp) {
        ndspChnWaveBufClear(ui_channel);
        memset(&ui_buf, 0, sizeof(ui_buf));
        ui_buf.data_pcm16 = ui_data[which];
        ui_buf.nsamples = ui_frames[which];
        ndspChnWaveBufAdd(ui_channel, &ui_buf);
    } else if (backend == Backend::Csnd) {
        csndPlaySound(csnd_chn[2], SOUND_FORMAT_16BIT | SOUND_ONE_SHOT, 32000, 1.0f, 0.0f, ui_data[which], ui_data[which], ui_frames[which] * 2);
    }
}

// Development: write the next `frames` stereo frames of output to `path` (raw s16 stereo).
void audio3ds::record(const char* path, uint32_t frames) {
    std::lock_guard lock{ audio_mutex };
    if (rec_file) fclose(rec_file);
    rec_file = fopen(path, "wb");
    rec_remaining = frames;
}

// Citra and its forks (Azahar) answer system-info type 0x20000 ("Citra information"); a real console
// returns an error.
static bool running_in_emulator() {
    s64 out = 0;
    return R_SUCCEEDED(svcGetSystemInfo(&out, 0x20000, 0));
}

static bool ndsp_init() {
    // Emulators emulate the DSP at a high level and accept any firmware image, so when the dump is
    // missing there, hand NDSP a blank one (players on emulators rarely have a dspfirm.cdc).
    if (running_in_emulator()) {
        FILE* f = fopen("sdmc:/3ds/dspfirm.cdc", "rb");
        if (f) {
            fclose(f);
        } else {
            static const u8 blank_firmware[4096] = {};
            ndspUseComponent(blank_firmware, sizeof(blank_firmware), 0xFF, 0xFF);
            fprintf(stderr, "[audio] emulator without dspfirm.cdc: using a blank firmware image\n");
        }
    }
    if (R_FAILED(ndspInit())) {
        fprintf(stderr, "[audio] ndspInit failed (no sdmc:/3ds/dspfirm.cdc) - using CSND\n");
        return false;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(channel);
    ndspChnSetInterp(channel, NDSP_INTERP_POLYPHASE);
    ndspChnSetFormat(channel, NDSP_FORMAT_STEREO_PCM16);
    ndspChnSetRate(channel, (float)rate);
    for (int i = 0; i < num_buffers; i++) {
        buffer_data[i] = static_cast<int16_t*>(linearAlloc(buffer_frames * 2 * sizeof(int16_t)));
        memset(&wave_bufs[i], 0, sizeof(ndspWaveBuf));
        wave_bufs[i].data_pcm16 = buffer_data[i];
        wave_bufs[i].status = NDSP_WBUF_FREE;
    }
    ndspChnReset(ui_channel);
    ndspChnSetInterp(ui_channel, NDSP_INTERP_LINEAR);
    ndspChnSetFormat(ui_channel, NDSP_FORMAT_STEREO_PCM16);
    ndspChnSetRate(ui_channel, 32000.0f);
    for (int i = 0; i < 3; i++) {
        ui_data[i] = static_cast<int16_t*>(linearAlloc(ui_sizes[i]));
        if (!ui_data[i]) continue;
        memcpy(ui_data[i], ui_clips[i], ui_sizes[i]);
        DSP_FlushDataCache(ui_data[i], ui_sizes[i]);
        ui_frames[i] = ui_sizes[i] / 4;
    }
    memset(&ui_buf, 0, sizeof(ui_buf));
    kick_data = static_cast<int16_t*>(linearAlloc(kick_frames * 4));
    if (kick_data) {
        memset(kick_data, 0, kick_frames * 4);
        DSP_FlushDataCache(kick_data, kick_frames * 4);
    }
    ndsp_kick();
    // Let the DSP get going (a few of its frames) before the game queues anything.
    u32 first = ndspGetFrameCount();
    for (int i = 0; i < 100 && ndspGetFrameCount() < first + 4; i++) svcSleepThread(2000000);
    fprintf(stderr, "[audio] DSP frames at start: %lu -> %lu\n", (unsigned long)first, (unsigned long)ndspGetFrameCount());
    return true;
}

bool audio3ds::init() {
    // Development: DATA_PATH/force_csnd.txt tests the CSND path where the DSP would work.
    FILE* force = fopen("sdmc:/3ds/psnap/force_csnd.txt", "r");
    if (force) fclose(force);
    if (!force && ndsp_init()) {
        backend = Backend::Ndsp;
        fprintf(stderr, "[audio] NDSP (DSP firmware) ready\n");
        return true;
    }
    if (csnd_init()) {
        backend = Backend::Csnd;
        return true;
    }
    return false;
}

void audio3ds::exit() {
    std::lock_guard lock{ audio_mutex };
    if (backend == Backend::Ndsp) {
        ndspChnReset(channel);
        ndspChnReset(ui_channel);
        ndspExit();
        for (int i = 0; i < num_buffers; i++) linearFree(buffer_data[i]);
        if (kick_data) linearFree(kick_data);
        kick_data = nullptr;
    } else if (backend == Backend::Csnd) {
        csnd_stop();
        CSND_SetPlayState(csnd_chn[2], 0);
        csndExecCmds(true);
        csndExit();
        for (int i = 0; i < 2; i++) linearFree(ring[i]);
    }
    for (int i = 0; i < 3; i++) if (ui_data[i]) linearFree(ui_data[i]);
    backend = Backend::None;
}

void audio3ds::queue_samples(int16_t* samples, size_t sample_count) {
    if (backend == Backend::None) return;
    std::lock_guard lock{ audio_mutex };
    size_t frames = sample_count / 2;
    if (backend == Backend::Csnd) {
        // RDRAM is stored as 32-bit little-endian words, which swaps each stereo pair's halves.
        static int16_t swapped[8192];
        size_t done = 0;
        while (done < frames) {
            size_t n = std::min<size_t>(frames - done, 4096);
            convert(samples + done * 2, swapped, n);
            record_frames(swapped, n);
            if (!paused) csnd_queue(swapped, n);
            queued_frames += n;
            done += n;
        }
        return;
    }
    ndsp_check_stall();
    if (!paused && queued_frames) {
        bool any = false;
        for (int i = 0; i < num_buffers && !any; i++) {
            any = wave_bufs[i].status == NDSP_WBUF_QUEUED || wave_bufs[i].status == NDSP_WBUF_PLAYING;
        }
        if (!any) underruns++;
    }
    size_t done = 0;
    while (done < frames) {
        ndspWaveBuf& wb = wave_bufs[next_buffer];
        if (wb.status != NDSP_WBUF_FREE && wb.status != NDSP_WBUF_DONE) {
            dropped_frames += frames - done;
            return; // all buffers in flight: drop the rest of this chunk
        }
        size_t n = std::min(frames - done, buffer_frames);
        int16_t* out = buffer_data[next_buffer];
        convert(samples + done * 2, out, n);
        queued_frames += n;
        record_frames(out, n);
        DSP_FlushDataCache(out, n * 2 * sizeof(int16_t));
        wb.nsamples = n;
        wb.looping = false;
        ndspChnWaveBufAdd(channel, &wb);
        // Every ~4 s: whether the DSP is taking the audio (for silent-audio reports from consoles).
        static uint32_t chunks;
        if (++chunks % 240 == 1) {
            int q = 0, d = 0;
            for (int i = 0; i < num_buffers; i++) {
                q += wave_bufs[i].status == NDSP_WBUF_QUEUED || wave_bufs[i].status == NDSP_WBUF_PLAYING;
                d += wave_bufs[i].status == NDSP_WBUF_DONE;
            }
            fprintf(stderr, "[audio] ndsp: playing %d paused %d pos %lu, buffers queued %d done %d, peak %d, gain %.2f, underruns %lu, dropped %lu\n",
                (int)ndspChnIsPlaying(channel), (int)ndspChnIsPaused(channel), (unsigned long)ndspChnGetSamplePos(channel), q, d, peak,
                (double)gain, (unsigned long)underruns, (unsigned long)dropped_frames);
        }
        next_buffer = (next_buffer + 1) % num_buffers;
        done += n;
    }
}

size_t audio3ds::get_frames_remaining() {
    if (backend == Backend::None) return 0;
    std::lock_guard lock{ audio_mutex };
    if (backend == Backend::Csnd) {
        uint64_t played = csnd_played();
        return written > played ? (size_t)(written - played) : 0;
    }
    size_t remaining = 0;
    for (int i = 0; i < num_buffers; i++) {
        const ndspWaveBuf& wb = wave_bufs[i];
        if (wb.status == NDSP_WBUF_QUEUED) {
            remaining += wb.nsamples;
        }
        else if (wb.status == NDSP_WBUF_PLAYING) {
            u32 pos = ndspChnGetSamplePos(channel);
            remaining += wb.nsamples > pos ? wb.nsamples - pos : 0;
        }
    }
    return remaining;
}

void audio3ds::set_frequency(uint32_t freq) {
    std::lock_guard lock{ audio_mutex };
    if (freq == rate) return;
    rate = freq;
    fprintf(stderr, "[audio] output rate %lu Hz\n", (unsigned long)freq);
    if (backend == Backend::Ndsp) ndspChnSetRate(channel, (float)freq);
    if (backend == Backend::Csnd && !paused) {
        csnd_stop();
        csnd_start();
    }
}

void audio3ds::set_paused(bool p) {
    std::lock_guard lock{ audio_mutex };
    if (p == paused) return;
    if (backend == Backend::Ndsp) ndspChnSetPaused(channel, p);
    if (backend == Backend::Csnd) {
        // Stopped while paused; on resume the rings start over, silent, and fill again.
        if (p) csnd_stop();
        else csnd_start();
    }
    paused = p;
}

void audio3ds::status_line(char* out, size_t size) {
    std::lock_guard lock{ audio_mutex };
    if (backend == Backend::None) {
        snprintf(out, size, "Sound: off (no DSP or CSND)");
    } else if (backend == Backend::Csnd) {
        snprintf(out, size, "CSND  sent %lu  pk %d  drop %lu", (unsigned long)queued_frames, peak, (unsigned long)dropped_frames);
    } else {
        int q = 0, d = 0;
        for (int i = 0; i < num_buffers; i++) {
            q += wave_bufs[i].status == NDSP_WBUF_QUEUED || wave_bufs[i].status == NDSP_WBUF_PLAYING;
            d += wave_bufs[i].status == NDSP_WBUF_DONE;
        }
        snprintf(out, size, "DSP %s q%d d%d pk %d g%.2f under %lu drop %lu k%lu f%lu",
            ndspChnIsPlaying(channel) ? "play" : "stop", q, d, peak, (double)gain, (unsigned long)underruns,
            (unsigned long)dropped_frames, (unsigned long)kicks, (unsigned long)(ndspGetFrameCount() % 100000));
    }
}

audio3ds::DebugStats audio3ds::debug_stats() {
    std::lock_guard lock{ audio_mutex };
    bool playing = backend == Backend::Ndsp ? ndspChnIsPlaying(channel) : backend == Backend::Csnd && !paused;
    return { queued_frames, dropped_frames, peak, playing };
}
