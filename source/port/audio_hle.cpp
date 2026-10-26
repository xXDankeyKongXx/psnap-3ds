// Native (HLE) implementation of Pokemon Snap's audio microcode (libultra's aspMain, "ABI1").
//
// The recompiled microcode (recomp/rsp/aspMain.cpp) is exact but slow on the 3DS (no SIMD for the
// RSP vector unit): it took ~65% of the game core at the intro. This does the same work in plain
// integer code. Command semantics follow mupen64plus-rsp-hle (alist_audio.c, alist.c; GPL-2+), the
// per-sample arithmetic of ADPCM, RESAMPLE and MIXER follows the Mega Man 64 port's HLE (checked
// bit-exact against its microcode, a close relative of this one). audio_hle_compare (below) runs
// both and logs differences in development builds.
//
// DMEM is the runtime's RSP data memory (librecomp/rsp.hpp), loaded with the microcode's data
// segment by recomp::rsp::run_task (the resample filter table is at 0xC0). Buffer operands are
// offsets from 0x5C0, as in the microcode. Command state (segments, buffers, volumes, ADPCM book)
// lives here between commands of one task; the microcode starts each task with it cleared.

#include <3ds.h>

#include <arm_acle.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "recomp.h"
#include "librecomp/rsp.hpp"
#include "audio_hle.hpp"

namespace {

constexpr uint32_t dmem_base = 0x5C0;      // buffer operands are relative to this
constexpr uint32_t resample_table = 0x0C0; // 64 entries of 4 taps (data segment)

// DMEM and RDRAM both hold big-endian data as host-order 32-bit words (byte address ^ 3).
inline uint8_t& d8(uint32_t addr) { return dmem[(addr ^ 3) & 0xFFF]; }
inline int16_t& d16(uint32_t addr) { return *reinterpret_cast<int16_t*>(&dmem[(addr ^ 2) & 0xFFE]); }
inline int16_t& r16(uint8_t* rdram, uint32_t addr) { return *reinterpret_cast<int16_t*>(rdram + ((addr ^ 2) & 0xFFFFFE)); }

inline int16_t clamp16(int64_t x) { return x > 32767 ? 32767 : x < -32768 ? -32768 : (int16_t)x; }

// A 4-byte-aligned DMEM buffer as host halfwords. Each 32-bit word holds two samples swapped, so
// sample i is at index i ^ 1; element-wise work on equally aligned buffers can ignore that.
inline int16_t* samples(uint32_t addr) { return reinterpret_cast<int16_t*>(&dmem[addr & 0xFFC]); }

// VMULF: (2ab + 0x8000) >> 16, which is (ab + 0x4000) >> 15.
inline int16_t vmulf(int32_t a, int32_t b) { return (int16_t)__ssat((a * b + 0x4000) >> 15, 16); }

// The microcode's mix of src*gain into dst (VMULF dst by 0x7FFF, VMACF src by gain).
inline int16_t mix(int32_t dst, int32_t src, int32_t gain) {
    return (int16_t)__ssat(__qadd(__qadd(dst * 0x7FFF, src * gain), 0x4000) >> 15, 16);
}

uint32_t align(uint32_t x, uint32_t a) { return (x + a - 1) & ~(a - 1); }

struct State {
    uint32_t segments[16];
    uint32_t in, out, count;              // SETBUFF (main)
    uint32_t dry_right, wet_left, wet_right; // SETBUFF (aux); out is the dry left buffer
    int16_t dry, wet;
    int16_t vol[2], target[2];            // [0] left, [1] right
    int32_t rate[2];
    uint32_t loop;
    int16_t table[128];                   // LOADADPCM codebook (8 entries of 16)
} st;

uint32_t address(uint32_t so) {
    uint32_t segment = (so >> 24) & 0xF;
    return (st.segments[segment] + (so & 0xFFFFFF)) & 0xFFFFFF;
}

// RSP DMA as the runtime does it: RDRAM address 8-byte aligned, DMEM 4-byte aligned, length
// rounded up to 8.
void dma_read(uint8_t* rdram, uint32_t dmem_addr, uint32_t dram_addr, uint32_t len) {
    dram_addr &= 0xFFFFF8;
    dmem_addr &= ~3u;
    len = align(len, 8);
    if (dmem_addr + len <= 0x1000) {
        memcpy(&dmem[dmem_addr], rdram + dram_addr, len);
        return;
    }
    for (uint32_t i = 0; i < len; i++) d8(dmem_addr + i) = rdram[(dram_addr + i) ^ 3];
}

void dma_write(uint8_t* rdram, uint32_t dmem_addr, uint32_t dram_addr, uint32_t len) {
    dram_addr &= 0xFFFFF8;
    dmem_addr &= ~3u;
    len = align(len, 8);
    if (dmem_addr + len <= 0x1000) {
        memcpy(rdram + dram_addr, &dmem[dmem_addr], len);
        return;
    }
    for (uint32_t i = 0; i < len; i++) rdram[(dram_addr + i) ^ 3] = d8(dmem_addr + i);
}

void clearbuff(uint32_t w1, uint32_t w2) {
    uint32_t addr = (w1 & 0xFFFF) + dmem_base;
    uint32_t count = w2 & 0xFFF;
    if (count == 0) return;
    count = align(count, 16);
    for (uint32_t i = 0; i < count; i++) d8(addr + i) = 0;
}

void dmemmove(uint32_t w1, uint32_t w2) {
    uint32_t src = (w1 & 0xFFFF) + dmem_base;
    uint32_t dst = (w2 >> 16) + dmem_base;
    uint32_t count = w2 & 0xFFFF;
    if (count == 0) return;
    count = align(count, 16);
    for (uint32_t i = 0; i < count; i++) d8(dst + i) = d8(src + i);
}

void setbuff(uint32_t w1, uint32_t w2) {
    if ((w1 >> 16) & 0x08) { // A_AUX
        st.dry_right = (w1 & 0xFFFF) + dmem_base;
        st.wet_left = (w2 >> 16) + dmem_base;
        st.wet_right = (w2 & 0xFFFF) + dmem_base;
    } else {
        st.in = (w1 & 0xFFFF) + dmem_base;
        st.out = (w2 >> 16) + dmem_base;
        st.count = w2 & 0xFFFF;
    }
}

void setvol(uint32_t w1, uint32_t w2) {
    uint32_t flags = (w1 >> 16) & 0xFF;
    if (flags & 0x08) { // A_AUX
        st.dry = (int16_t)w1;
        st.wet = (int16_t)w2;
        return;
    }
    int lr = (flags & 0x02) ? 0 : 1; // A_LEFT
    if (flags & 0x04) {               // A_VOL
        st.vol[lr] = (int16_t)w1;
    } else {
        st.target[lr] = (int16_t)w1;
        st.rate[lr] = (int32_t)w2;
    }
}

void interleave(uint32_t w2) {
    if (st.count == 0) return;
    uint32_t left = (w2 >> 16) + dmem_base, right = (w2 & 0xFFFF) + dmem_base;
    uint32_t n = align(st.count, 16) / 2; // samples per channel (count = bytes per input buffer)
    for (uint32_t i = 0; i < n; i++) {
        int16_t l = d16(left + i * 2), r = d16(right + i * 2);
        d16(st.out + i * 4) = l;
        d16(st.out + i * 4 + 2) = r;
    }
}

void mixer(uint32_t w1, uint32_t w2) {
    if (st.count == 0) return;
    int32_t gain = (int16_t)w1;
    uint32_t src = (w2 >> 16) + dmem_base;
    uint32_t dst = (w2 & 0xFFFF) + dmem_base;
    uint32_t count = align(st.count, 32);
    if (((src | dst) & 3) == 0 && src + count <= 0x1000 && dst + count <= 0x1000) {
        // Equally aligned host-order buffers: element-wise, the halfword swap cancels out.
        int16_t* d = samples(dst);
        const int16_t* s = samples(src);
        for (uint32_t i = 0; i < count / 2; i++) d[i] = mix(d[i], s[i], gain);
        return;
    }
    for (uint32_t i = 0; i < count; i += 2) d16(dst + i) = mix(d16(dst + i), d16(src + i), gain);
}

void loadbuff(uint8_t* rdram, uint32_t w2) {
    if (st.count == 0) return;
    dma_read(rdram, st.in, address(w2), st.count);
}

void savebuff(uint8_t* rdram, uint32_t w2) {
    if (st.count == 0) return;
    dma_write(rdram, st.out, address(w2), st.count);
}

void loadadpcm(uint8_t* rdram, uint32_t w1, uint32_t w2) {
    uint32_t addr = address(w2);
    uint32_t n = align(w1 & 0xFFFF, 8) / 2;
    if (n > 128) n = 128;
    for (uint32_t i = 0; i < n; i++) st.table[i] = r16(rdram, addr + i * 2);
}

void adpcm(uint8_t* rdram, uint32_t w1, uint32_t w2) {
    uint32_t flags = (w1 >> 16) & 0xFF;
    uint32_t state = address(w2);
    uint32_t in = st.in, out = st.out;
    int32_t count = (int32_t)align(st.count, 32);

    int16_t last[16];
    if (flags & 0x01) { // A_INIT
        memset(last, 0, sizeof(last));
    } else {
        uint32_t from = (flags & 0x02) ? st.loop : state; // A_LOOP
        for (int i = 0; i < 16; i++) last[i] = r16(rdram, from + i * 2);
    }
    for (int i = 0; i < 16; i++) d16(out + i * 2) = last[i];

    while (count > 0) {
        out += 32;
        uint8_t header = d8(in++);
        unsigned scale = header >> 4;
        unsigned rshift = scale < 12 ? 12 - scale : 0;
        const int16_t* book = st.table + (header & 0xF) * 16;

        int16_t residual[16];
        for (int i = 0; i < 8; i++) {
            uint8_t byte = d8(in++);
            residual[i * 2] = (int16_t)((byte & 0xF0) << 8) >> rshift;
            residual[i * 2 + 1] = (int16_t)((byte & 0x0F) << 12) >> rshift;
        }
        // Two halves of 8 samples, each predicted from the previous two output samples.
        for (int half = 0; half < 2; half++) {
            int32_t l1 = last[half ? 6 : 14];
            int32_t l2 = last[half ? 7 : 15];
            const int16_t* res = residual + half * 8;
            int16_t* dst = last + half * 8;
            for (int i = 0; i < 8; i++) {
                int32_t acc = (int32_t)res[i] << 11;
                acc += book[i] * l1 + book[8 + i] * l2;
                for (int j = 0; j < i; j++) acc += book[8 + j] * res[i - 1 - j];
                dst[i] = clamp16(acc >> 11);
            }
        }
        for (int i = 0; i < 16; i++) d16(out + i * 2) = last[i];
        count -= 32;
    }
    for (int i = 0; i < 16; i++) r16(rdram, state + i * 2) = last[i];
}

void resample(uint8_t* rdram, uint32_t w1, uint32_t w2) {
    uint32_t flags = (w1 >> 16) & 0xFF;
    uint32_t step = (w1 & 0xFFFF) << 1;
    uint32_t state = address(w2);
    uint32_t in_start = st.in;
    if (flags & 0x02) {
        // Continue with the samples saved after the last read position (state +0x10, see below):
        // they go just before the input, which then starts `extra` bytes earlier.
        for (int i = 0; i < 8; i++) d16(st.in - 16 + i * 2) = r16(rdram, state + 0x10 + i * 2);
        in_start -= (uint16_t)r16(rdram, state + 0xA);
    }
    uint32_t pos = in_start - 8; // 4 history samples before the input
    uint32_t out = st.out;
    uint32_t frames = align(st.count, 16) / 2;
    uint32_t frac;

    if (flags & 0x01) { // A_INIT
        for (int i = 0; i < 4; i++) d16(pos + i * 2) = 0;
        frac = 0;
    } else {
        for (int i = 0; i < 4; i++) d16(pos + i * 2) = r16(rdram, state + i * 2);
        frac = (uint16_t)r16(rdram, state + 8);
    }

    // Copy the input samples this frame reads into a plain array (positions only move forward).
    uint32_t end = pos, f = frac;
    for (uint32_t i = 0; i < frames; i++) {
        f += step;
        end += (f >> 16) * 2;
        f &= 0xFFFF;
    }
    static int16_t in[0x800 + 4];
    uint32_t in_count = (end - pos) / 2 + 4;
    if (in_count > sizeof(in) / sizeof(in[0])) in_count = sizeof(in) / sizeof(in[0]);
    for (uint32_t i = 0; i < in_count; i++) in[i] = d16(pos + i * 2);

    uint32_t at = 0;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t t = resample_table + (frac >> 10) * 8;
        const int16_t* x = in + at;
        int32_t p0 = vmulf(x[0], d16(t));
        int32_t p1 = vmulf(x[1], d16(t + 2));
        int32_t p2 = vmulf(x[2], d16(t + 4));
        int32_t p3 = vmulf(x[3], d16(t + 6));
        d16(out + i * 2) = (int16_t)__ssat(__ssat(p0 + p1, 16) + __ssat(p2 + p3, 16), 16);
        frac += step;
        at += frac >> 16;
        frac &= 0xFFFF;
    }
    pos += at * 2;

    // State (0x20 bytes): the 4 samples at the next read position, the fraction, then the 16 bytes
    // around the position after them, aligned to the input buffer, and how far into them it is.
    for (int i = 0; i < 4; i++) r16(rdram, state + i * 2) = d16(pos + i * 2);
    r16(rdram, state + 8) = (int16_t)frac;
    uint32_t after = pos + 8;
    uint32_t misalign = (after - st.in) & 0xF;
    uint32_t base = after - misalign;
    r16(rdram, state + 0xA) = (int16_t)(misalign ? 16 - misalign : 0);
    for (int i = 0; i < 8; i++) r16(rdram, state + 0x10 + i * 2) = d16(base + i * 2);
}

// ENVMIXER, following the microcode (IMEM 0x1B38). Each channel's volume is a 32-bit value per
// lane (8 lanes = the 8 samples of a group): high half signed, low half the fraction. Every group
// it is multiplied by the 16.16 rate (an exponential ramp) and clamped to the target: the unsigned
// minimum (VCL) while rising (rate's high half > 0), the signed maximum (VGE) otherwise. Gains are
// VMULF(volume, dry/wet), mixed with mix(). On A_INIT the lanes start interpolated between vol and
// vol*rate (fractions 0x2000*(k+1), 0xFFFF for lane 7); otherwise volumes and all parameters come
// from the 0x50-byte state: L hi[8], L lo[8], R hi[8], R lo[8], then target L, rate L (hi, lo),
// target R, rate R (hi, lo), dry, wet.

// RSP accumulator results: VMADH-style high half (signed saturate of acc >> 16) and VMADN-style low
// half (acc's low 16 bits if acc fits in 32 signed bits, else 0 or 0xFFFF).
inline int16_t acc_hi(int64_t acc) { return clamp16(acc >> 16); }
inline uint16_t acc_lo(int64_t acc) {
    if (acc < INT32_MIN) return 0;
    if (acc > INT32_MAX) return 0xFFFF;
    return (uint16_t)acc;
}

struct Lanes {
    int16_t hi[8];
    uint16_t lo[8];
};

// (hi:lo) * (rate_hi:rate_lo) >> 16, as VMUDL/VMADM/VMADN/VMADH.
void lanes_mul(Lanes& v, int16_t rate_hi, uint16_t rate_lo) {
    for (int k = 0; k < 8; k++) {
        int64_t acc = ((uint32_t)v.lo[k] * rate_lo) >> 16;
        acc += (int64_t)v.hi[k] * rate_lo;
        acc += (int64_t)v.lo[k] * rate_hi;
        acc += ((int64_t)v.hi[k] * rate_hi) << 16;
        v.hi[k] = acc_hi(acc);
        v.lo[k] = acc_lo(acc);
    }
}

void lanes_clamp(Lanes& v, int16_t target, int16_t rate_hi) {
    for (int k = 0; k < 8; k++) {
        if (rate_hi > 0) v.hi[k] = (uint16_t)v.hi[k] >= (uint16_t)target ? target : v.hi[k]; // VCL
        else v.hi[k] = v.hi[k] >= target ? v.hi[k] : target;                                  // VGE
    }
}

void lanes_init(Lanes& v, int16_t vol, int16_t rate_hi, uint16_t rate_lo) {
    // vol * rate (VMUDM, VMADH, VMADN), then minus vol (VSUBC with 0, VSUB).
    int64_t p = (int64_t)vol * rate_lo + (((int64_t)vol * rate_hi) << 16);
    int16_t d_hi = clamp16((int32_t)acc_hi(p) - vol);
    uint16_t d_lo = acc_lo(p);
    for (int k = 0; k < 8; k++) {
        uint32_t frac = k == 7 ? 0xFFFF : 0x2000u * (k + 1);
        int64_t acc = ((uint64_t)frac * d_lo) >> 16; // VMUDL
        acc += (int64_t)frac * d_hi;                 // VMADN
        acc += (int64_t)vol << 16;                   // VMADH by 1
        v.hi[k] = acc_hi(acc);
        v.lo[k] = acc_lo(acc);
    }
}

void envmixer(uint8_t* rdram, uint32_t w1, uint32_t w2) {
    uint32_t flags = (w1 >> 16) & 0xFF;
    bool aux = flags & 0x08;
    uint32_t state = address(w2);

    int16_t params[8]; // target L, rate L hi, rate L lo, target R, rate R hi, rate R lo, dry, wet
    Lanes left, right;
    if (flags & 0x01) { // A_INIT
        params[0] = st.target[0];
        params[1] = (int16_t)(st.rate[0] >> 16);
        params[2] = (int16_t)st.rate[0];
        params[3] = st.target[1];
        params[4] = (int16_t)(st.rate[1] >> 16);
        params[5] = (int16_t)st.rate[1];
        params[6] = st.dry;
        params[7] = st.wet;
    } else {
        for (int k = 0; k < 8; k++) {
            left.hi[k] = r16(rdram, state + 0x00 + k * 2);
            left.lo[k] = (uint16_t)r16(rdram, state + 0x10 + k * 2);
            right.hi[k] = r16(rdram, state + 0x20 + k * 2);
            right.lo[k] = (uint16_t)r16(rdram, state + 0x30 + k * 2);
            params[k] = r16(rdram, state + 0x40 + k * 2);
        }
    }
    int16_t target_l = params[0], rate_l_hi = params[1], target_r = params[3], rate_r_hi = params[4];
    uint16_t rate_l_lo = (uint16_t)params[2], rate_r_lo = (uint16_t)params[5];
    int32_t dry = params[6], wet = params[7];

    uint32_t in = st.in, dl = st.out, dr = st.dry_right, wl = st.wet_left, wr = st.wet_right;
    int32_t count = (int16_t)st.count;

    bool aligned = ((in | dl | dr | (aux ? (wl | wr) : 0)) & 3) == 0;
    auto mix_group = [&](const Lanes& v, uint32_t d, uint32_t w) {
        if (aligned && in + 16 <= 0x1000 && d + 16 <= 0x1000 && (!aux || w + 16 <= 0x1000)) {
            // Host index j holds sample j ^ 1 of the group (see samples()).
            const int16_t* x = samples(in);
            int16_t* dp = samples(d);
            for (int j = 0; j < 8; j++) dp[j] = mix(dp[j], x[j], vmulf(v.hi[j ^ 1], dry));
            if (aux) {
                int16_t* wp = samples(w);
                for (int j = 0; j < 8; j++) wp[j] = mix(wp[j], x[j], vmulf(v.hi[j ^ 1], wet));
            }
            return;
        }
        for (int k = 0; k < 8; k++) {
            int32_t x = d16(in + k * 2);
            d16(d + k * 2) = mix(d16(d + k * 2), x, vmulf(v.hi[k], dry));
            if (aux) d16(w + k * 2) = mix(d16(w + k * 2), x, vmulf(v.hi[k], wet));
        }
    };
    auto advance = [&]() {
        count -= 16;
        in += 16;
        dl += 16;
        dr += 16;
        if (aux) {
            wl += 16;
            wr += 16;
        }
    };

    if (flags & 0x01) {
        lanes_init(left, st.vol[0], rate_l_hi, rate_l_lo);
        lanes_clamp(left, target_l, rate_l_hi);
        mix_group(left, dl, wl);
        lanes_init(right, st.vol[1], rate_r_hi, rate_r_lo);
        lanes_clamp(right, target_r, rate_r_hi);
        mix_group(right, dr, wr);
        advance();
    }
    lanes_mul(left, rate_l_hi, rate_l_lo);
    Lanes saved_left;
    do {
        lanes_clamp(left, target_l, rate_l_hi);
        lanes_mul(right, rate_r_hi, rate_r_lo);
        saved_left = left;
        mix_group(left, dl, wl);
        lanes_clamp(right, target_r, rate_r_hi);
        lanes_mul(left, rate_l_hi, rate_l_lo);
        mix_group(right, dr, wr);
        advance();
    } while (count > 0);

    for (int k = 0; k < 8; k++) {
        r16(rdram, state + 0x00 + k * 2) = saved_left.hi[k];
        r16(rdram, state + 0x10 + k * 2) = (int16_t)saved_left.lo[k];
        r16(rdram, state + 0x20 + k * 2) = right.hi[k];
        r16(rdram, state + 0x30 + k * 2) = (int16_t)right.lo[k];
        r16(rdram, state + 0x40 + k * 2) = params[k];
    }
}

// Two-pole filter (mupen64plus alist_polef). Coefficients are the ADPCM book's first entry.
void polef(uint8_t* rdram, uint32_t w1, uint32_t w2) {
    if (st.count == 0) return;
    uint32_t flags = (w1 >> 16) & 0xFF;
    int32_t gain = (uint16_t)w1;
    uint32_t state = address(w2);
    uint32_t in = st.in, out = st.out;
    int32_t count = (int32_t)align(st.count, 16);
    const int16_t* h1 = st.table;
    int16_t* h2 = st.table + 8;
    int16_t h2_before[8];
    int16_t l1 = 0, l2 = 0;
    if (!(flags & 0x01)) {
        l1 = r16(rdram, state + 4);
        l2 = r16(rdram, state + 6);
    }
    for (int i = 0; i < 8; i++) {
        h2_before[i] = h2[i];
        h2[i] = (int16_t)(((int32_t)h2[i] * gain) >> 14);
    }
    int16_t last[8] = {};
    do {
        int16_t frame[8];
        for (int i = 0; i < 8; i++) frame[i] = d16(in + i * 2);
        for (int i = 0; i < 8; i++) {
            int32_t acc = frame[i] * gain + h1[i] * l1 + h2_before[i] * l2;
            for (int j = 0; j < i; j++) acc += h2[j] * frame[i - 1 - j];
            last[i] = clamp16(acc >> 14);
            d16(out + i * 2) = last[i];
        }
        l1 = last[6];
        l2 = last[7];
        in += 16;
        out += 16;
        count -= 16;
    } while (count > 0);
    for (int i = 0; i < 4; i++) r16(rdram, state + i * 2) = last[4 + i];
}

} // namespace

// Development: time per command type, measured on every 64th task (reading the system tick is a
// system call, too slow to do around every command) and logged every 640 tasks.
static uint64_t op_ticks[16];
static uint32_t op_count[16];
static uint32_t task_count;

RspExitReason audio_hle_run(uint8_t* rdram, uint32_t alist_addr, uint32_t alist_size) {
    memset(&st, 0, sizeof(st));
    bool timed = (++task_count & 63) == 0;
    for (uint32_t off = 0; off + 8 <= alist_size; off += 8) {
        uint32_t w1 = (uint32_t)MEM_W(off, (int32_t)alist_addr);
        uint32_t w2 = (uint32_t)MEM_W(off + 4, (int32_t)alist_addr);
        uint64_t op_start = timed ? svcGetSystemTick() : 0;
        uint32_t op = (w1 >> 24) & 0xF;
        switch (op) {
            case 0x1: adpcm(rdram, w1, w2); break;
            case 0x2: clearbuff(w1, w2); break;
            case 0x3: envmixer(rdram, w1, w2); break;
            case 0x4: loadbuff(rdram, w2); break;
            case 0x5: resample(rdram, w1, w2); break;
            case 0x6: savebuff(rdram, w2); break;
            case 0x7: st.segments[(w2 >> 24) & 0xF] = w2 & 0xFFFFFF; break; // SEGMENT
            case 0x8: setbuff(w1, w2); break;
            case 0x9: setvol(w1, w2); break;
            case 0xA: dmemmove(w1, w2); break;
            case 0xB: loadadpcm(rdram, w1, w2); break;
            case 0xC: mixer(w1, w2); break;
            case 0xD: interleave(w2); break;
            case 0xE: polef(rdram, w1, w2); break;
            case 0xF: st.loop = address(w2); break; // SETLOOP
            default: break; // 0: no-op
        }
        if (timed) {
            op_ticks[op] += svcGetSystemTick() - op_start;
            op_count[op]++;
        }
    }
    if (task_count % 640 == 0) {
        fprintf(stderr, "[audiohle] us per task by op:");
        for (int i = 0; i < 16; i++) {
            if (op_count[i]) fprintf(stderr, " %X:%llu(x%lu)", i, (unsigned long long)(op_ticks[i] / 10 / (SYSCLOCK_ARM11 / 1000000)), (unsigned long)(op_count[i] / 10));
            op_ticks[i] = 0;
            op_count[i] = 0;
        }
        fputc('\n', stderr);
    }
    return RspExitReason::Broke;
}

// --- Development: compare against the recompiled microcode ----------------------------------------

namespace {

struct Region {
    uint32_t start, len;
    uint8_t op;
};

// What a task writes to RDRAM: SAVEBUFF output and the per-voice state of ADPCM (32 bytes),
// RESAMPLE (32), ENVMIXER (0x50) and POLEF (8). Follows SEGMENT/SETBUFF to resolve them.
void collect_written_regions(uint8_t* rdram, uint32_t alist_addr, uint32_t alist_size, std::vector<Region>& regions) {
    uint32_t segments[16] = {};
    uint32_t count = 0;
    for (uint32_t off = 0; off + 8 <= alist_size; off += 8) {
        uint32_t w1 = (uint32_t)MEM_W(off, (int32_t)alist_addr);
        uint32_t w2 = (uint32_t)MEM_W(off + 4, (int32_t)alist_addr);
        uint8_t op = (w1 >> 24) & 0xF;
        if (op == 0x7) { segments[(w2 >> 24) & 0xF] = w2 & 0xFFFFFF; continue; }
        if (op == 0x8) { if (!((w1 >> 16) & 0x08)) count = w2 & 0xFFFF; continue; }
        uint32_t len = 0;
        switch (op) {
            case 0x1: len = 32; break;
            case 0x3: len = 0x50; break;
            case 0x5: len = 32; break;
            case 0x6: len = count; break;
            case 0xE: len = 8; break;
            default: continue;
        }
        uint32_t addr = (segments[(w2 >> 24) & 0xF] + (w2 & 0xFFFFFF)) & 0xFFFFF8;
        len = (len + 7) & ~7u;
        if (len && addr + len <= 0x800000) regions.push_back({addr, len, op});
    }
}

} // namespace

RspExitReason audio_hle_compare(uint8_t* rdram, uint32_t alist_addr, uint32_t alist_size, RspUcodeFunc* microcode, uint32_t ucode_addr) {
    static unsigned task_number = 0;
    task_number++;

    std::vector<Region> regions;
    collect_written_regions(rdram, alist_addr, alist_size, regions);
    std::vector<uint8_t> before, after_lle;
    for (const Region& r : regions) before.insert(before.end(), rdram + r.start, rdram + r.start + r.len);
    std::vector<uint8_t> dmem_before(dmem, dmem + 0x1000);

    RspExitReason ret = microcode(rdram, ucode_addr);
    for (const Region& r : regions) after_lle.insert(after_lle.end(), rdram + r.start, rdram + r.start + r.len);

    // Restore in reverse so overlapping regions end up with their original contents.
    size_t pos = before.size();
    for (size_t i = regions.size(); i-- > 0;) {
        pos -= regions[i].len;
        memcpy(rdram + regions[i].start, before.data() + pos, regions[i].len);
    }
    memcpy(dmem, dmem_before.data(), 0x1000);
    audio_hle_run(rdram, alist_addr, alist_size);

    // Differences per command type (state regions included).
    unsigned diffs[16] = {}, max_diff[16] = {}, total = 0;
    pos = 0;
    for (const Region& r : regions) {
        for (uint32_t i = 0; i < r.len; i += 2) {
            int16_t hle = *reinterpret_cast<int16_t*>(rdram + ((r.start + i) ^ 2));
            int16_t lle = *reinterpret_cast<const int16_t*>(after_lle.data() + ((pos + i) ^ 2));
            if (hle != lle) {
                diffs[r.op]++;
                total++;
                unsigned d = (unsigned)abs(hle - lle);
                if (d > max_diff[r.op]) max_diff[r.op] = d;
            }
        }
        pos += r.len;
    }
    static unsigned clean_tasks = 0;
    if (total) {
        fprintf(stderr, "[audiohle] task %u: %u/%u halfwords differ:", task_number, total, (unsigned)(pos / 2));
        for (int op = 0; op < 16; op++) if (diffs[op]) fprintf(stderr, " op%X %u (max %u)", op, diffs[op], max_diff[op]);
        fputc('\n', stderr);
    } else if (++clean_tasks % 50 == 0) {
        fprintf(stderr, "[audiohle] %u tasks matched exactly (task %u)\n", clean_tasks, task_number);
    }
    return ret;
}
