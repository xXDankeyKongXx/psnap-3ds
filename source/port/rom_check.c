// Finding and checking the player's Pokemon Snap ROM (rom_check.h).
//
// Shared builds carry no ROM: the player puts their own dump on the SD card. Dumps come in three
// byte orders (.z64 big-endian, .v64 16-bit swapped, .n64 32-bit little-endian) and under any
// name; the port streams the game from a .z64. So a dump is found, read through once (converted
// into DATA_PATH/pokemonsnap.us.z64 if it isn't .z64 already) and its SHA-1 compared with the USA
// release's: a European/Japanese, hacked or bad dump would otherwise crash somewhere in the game
// instead of saying what's wrong. The check is remembered (path, then size, time and hash, in
// rom_verified.txt), so later boots don't read 16 MB first.

#include <3ds.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "rom_check.h"

#define ROM_BYTES (16 * 1024 * 1024)
#define CHUNK (256 * 1024)

// --- SHA-1 ---------------------------------------------------------------------------------------

typedef struct {
    uint32_t h[5];
    uint64_t bytes;
    uint8_t block[64];
    size_t used;
} Sha1;

static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(Sha1* s, const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_init(Sha1* s) {
    static const uint32_t h0[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    memcpy(s->h, h0, sizeof(h0));
    s->bytes = 0;
    s->used = 0;
}

static void sha1_update(Sha1* s, const uint8_t* p, size_t n) {
    s->bytes += n;
    if (s->used) {
        while (n && s->used < 64) { s->block[s->used++] = *p++; n--; }
        if (s->used < 64) return;
        sha1_block(s, s->block);
        s->used = 0;
    }
    for (; n >= 64; p += 64, n -= 64) sha1_block(s, p);
    memcpy(s->block, p, n);
    s->used = n;
}

static void sha1_hex(Sha1* s, char out[41]) {
    uint64_t bits = s->bytes * 8;
    uint8_t pad = 0x80;
    sha1_update(s, &pad, 1);
    pad = 0;
    while (s->used != 56) sha1_update(s, &pad, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - i * 8));
    sha1_update(s, len, 8);
    for (int i = 0; i < 20; i++) sprintf(out + i * 2, "%02x", (unsigned)(s->h[i / 4] >> (24 - (i % 4) * 8)) & 0xFF);
}

// --- Byte orders -----------------------------------------------------------------------------------

typedef enum { ORDER_UNKNOWN, ORDER_Z64, ORDER_V64, ORDER_N64 } Order;
static const char* const order_names[] = { "?", ".z64", ".v64", ".n64" };

static Order order_of(const uint8_t* p) {
    if (p[0] == 0x80 && p[1] == 0x37 && p[2] == 0x12 && p[3] == 0x40) return ORDER_Z64;
    if (p[0] == 0x37 && p[1] == 0x80 && p[2] == 0x40 && p[3] == 0x12) return ORDER_V64;
    if (p[0] == 0x40 && p[1] == 0x12 && p[2] == 0x37 && p[3] == 0x80) return ORDER_N64;
    return ORDER_UNKNOWN;
}

static void to_z64(uint8_t* p, size_t n, Order order) {
    if (order == ORDER_V64) {
        for (size_t i = 0; i + 1 < n; i += 2) { uint8_t t = p[i]; p[i] = p[i + 1]; p[i + 1] = t; }
    } else if (order == ORDER_N64) {
        for (size_t i = 0; i + 3 < n; i += 4) {
            uint8_t a = p[i], b = p[i + 1];
            p[i] = p[i + 3]; p[i + 1] = p[i + 2]; p[i + 2] = b; p[i + 3] = a;
        }
    }
}

// --- Files -----------------------------------------------------------------------------------------

static const char* base_name(const char* path) {
    const char* s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static long file_size(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return (long)st.st_size;
}

static void strip_newline(char* s) { s[strcspn(s, "\r\n")] = 0; }

// What the marker keeps of a checked ROM: "<sha1> <size> <mtime>".
static void stamp_of(const char* path, char* out, size_t cap) {
    u64 mtime = 0;
    archive_getmtime(path, &mtime);
    snprintf(out, cap, "%s %ld %llu", ROM_SHA1_USA, file_size(path), (unsigned long long)mtime);
}

// The ROM checked on an earlier boot, if it's still there unchanged: its path into `path`.
static bool verified_before(const char* marker, char* path, size_t cap) {
    char stamp[96] = "", want[96];
    FILE* f = fopen(marker, "r");
    if (!f) return false;
    bool got = fgets(path, (int)cap, f) && fgets(stamp, sizeof(stamp), f);
    fclose(f);
    if (!got) return false;
    strip_newline(path);
    strip_newline(stamp);
    if (file_size(path) != ROM_BYTES) return false;
    stamp_of(path, want, sizeof(want));
    return strcmp(stamp, want) == 0;
}

static void remember(const char* path, const char* marker) {
    char stamp[96];
    stamp_of(path, stamp, sizeof(stamp));
    FILE* f = fopen(marker, "w");
    if (!f) return;
    fputs(path, f);
    fputc('\n', f);
    fputs(stamp, f);
    fputc('\n', f);
    fclose(f);
}

// Reads `path` through, converting it to .z64 into `convert_to` unless it is .z64 already (then
// nothing is written). True if it is the USA game; else `why` says what it is.
static bool check_file(const char* path, const char* convert_to, Order* order_out, char* why, size_t why_cap, RomProgress progress) {
    const char* name = base_name(path);
    *order_out = ORDER_UNKNOWN;
    if (file_size(path) != ROM_BYTES) {
        snprintf(why, why_cap, "%s: not 16 MB", name);
        return false;
    }
    FILE* in = fopen(path, "rb");
    if (!in) {
        snprintf(why, why_cap, "%s: can't be read", name);
        return false;
    }
    uint8_t* buf = malloc(CHUNK);
    if (!buf) {
        fclose(in);
        snprintf(why, why_cap, "out of memory");
        return false;
    }
    bool ok = false;
    FILE* out = NULL;
    char tmp[176] = "", message[96], hex[41];
    Sha1 sha;
    size_t total = 0;
    size_t got = fread(buf, 1, CHUNK, in);
    Order order = got >= 0x40 ? order_of(buf) : ORDER_UNKNOWN;
    *order_out = order;
    if (order == ORDER_UNKNOWN) {
        snprintf(why, why_cap, "%s: not an N64 ROM", name);
        goto done;
    }
    to_z64(buf, got, order);
    // Header: game code at 0x3B, "NPF" + region (E USA, J Japan, P Europe...).
    if (memcmp(&buf[0x3B], "NPF", 3) != 0) {
        snprintf(why, why_cap, "%s: not Pokemon Snap", name);
        goto done;
    }
    if (buf[0x3E] != 'E') {
        const char* region = buf[0x3E] == 'J' ? "Japanese" : buf[0x3E] == 'P' ? "European" : "another";
        snprintf(why, why_cap, "%s: the %s version (USA needed)", name, region);
        goto done;
    }
    if (order != ORDER_Z64) {
        snprintf(tmp, sizeof(tmp), "%s.tmp", convert_to);
        out = fopen(tmp, "wb");
        if (!out) {
            snprintf(why, why_cap, "can't write %s", base_name(convert_to));
            goto done;
        }
    }
    sha1_init(&sha);
    snprintf(message, sizeof(message), "Checking %s", name);
    for (;;) {
        sha1_update(&sha, buf, got);
        if (out && fwrite(buf, 1, got, out) != got) {
            snprintf(why, why_cap, "SD card full? (writing %s)", base_name(convert_to));
            goto done;
        }
        total += got;
        if (progress) progress(message, (int)(total * 100 / ROM_BYTES));
        got = fread(buf, 1, CHUNK, in);
        if (got == 0) break;
        to_z64(buf, got, order);
    }
    sha1_hex(&sha, hex);
    fprintf(stderr, "[rom] %s (%s): sha1 %s\n", name, order_names[order], hex);
    if (total != ROM_BYTES || strcmp(hex, ROM_SHA1_USA) != 0) {
        snprintf(why, why_cap, "%s: USA Pokemon Snap, but modified or a bad dump", name);
        goto done;
    }
    ok = true;
done:
    free(buf);
    fclose(in);
    if (out) {
        fclose(out);
        if (ok) {
            // The converted copy takes the .z64 name; whatever had it is kept aside, not deleted.
            if (file_size(convert_to) >= 0) {
                char aside[176];
                snprintf(aside, sizeof(aside), "%s.wrong", convert_to);
                remove(aside);
                rename(convert_to, aside);
            }
            ok = rename(tmp, convert_to) == 0;
            if (!ok) snprintf(why, why_cap, "can't write %s", base_name(convert_to));
        } else {
            remove(tmp);
        }
    }
    return ok;
}

// --- Finding it ------------------------------------------------------------------------------------

void rom_check(RomCheck* out, const char* data_path, const char* romfs_path, RomProgress progress) {
    memset(out, 0, sizeof(*out));
    // The app's own (a personal build): put there at build time, nothing to check.
    if (romfs_path && file_size(romfs_path) == ROM_BYTES) {
        snprintf(out->path, sizeof(out->path), "%s", romfs_path);
        snprintf(out->state, sizeof(out->state), "OK (packed in the app)");
        out->ok = true;
        return;
    }
    char rom[160], marker[160];
    snprintf(rom, sizeof(rom), "%s/pokemonsnap.us.z64", data_path);
    snprintf(marker, sizeof(marker), "%s/rom_verified.txt", data_path);
    if (verified_before(marker, out->path, sizeof(out->path))) {
        snprintf(out->state, sizeof(out->state), "OK");
        out->ok = true;
        return;
    }

    // Every 16 MB file where a dump would be, the usual name first.
    static char candidates[16][160];
    int count = 0;
    if (file_size(rom) >= 0) snprintf(candidates[count++], sizeof(candidates[0]), "%s", rom);
    const char* dirs[] = { data_path, "sdmc:/roms/n64" };
    for (int d = 0; d < 2 && count < 16; d++) {
        DIR* dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent* e;
        while ((e = readdir(dir)) && count < 16) {
            char path[160];
            snprintf(path, sizeof(path), "%s/%s", dirs[d], e->d_name);
            if (strcmp(path, rom) == 0 || file_size(path) != ROM_BYTES) continue;
            snprintf(candidates[count++], sizeof(candidates[0]), "%s", path);
        }
        closedir(dir);
    }

    char why[96] = "";
    for (int i = 0; i < count; i++) {
        Order order;
        char this_why[96];
        if (!check_file(candidates[i], rom, &order, this_why, sizeof(this_why), progress)) {
            fprintf(stderr, "[rom] %s\n", this_why);
            if (!why[0]) snprintf(why, sizeof(why), "%s", this_why);
            continue;
        }
        if (order == ORDER_Z64) {
            // Used where it is (no 16 MB copy), whatever its name.
            snprintf(out->path, sizeof(out->path), "%s", candidates[i]);
            snprintf(out->state, sizeof(out->state), "OK");
        } else {
            snprintf(out->path, sizeof(out->path), "%s", rom);
            snprintf(out->state, sizeof(out->state), "OK (converted from %s)", order_names[order]);
        }
        remember(out->path, marker);
        out->ok = true;
        return;
    }
    snprintf(out->state, sizeof(out->state), count ? "No usable Pokemon Snap ROM" : "No Pokemon Snap ROM found");
    snprintf(out->found, sizeof(out->found), "%s", why);
}
