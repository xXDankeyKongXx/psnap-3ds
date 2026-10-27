// Updates from GitHub (updater.h).
//
// The latest release of xXDankeyKongXx/psnap-3ds is asked for (api.github.com, releases/latest): its
// tag is the version ("v37"), its body the notes, and it carries the shareable builds as assets
// (psnap-3ds.cia, psnap-3ds.3dsx; never the personal CIA, which has a ROM in it). An update is
// downloaded to the SD card, checked against the size and SHA-256 GitHub lists for it, and only then
// installed: the CIA through the AM service (as FBI installs one; it replaces this title), the .3dsx
// by replacing the file the Homebrew Launcher started. A personal build's ROM lives in its RomFS,
// which the shareable CIA doesn't have, so it's copied to the SD card first (rom_check.c finds it
// there next time).
//
// HTTPS is curl + mbedTLS (devkitPro portlibs), with the few root certificates GitHub's servers
// need (romfs/cacert.pem): the 3DS's own SSL service can't always talk to them. Everything runs on
// a worker thread; sockets are set up for each job and closed after it, so the game has the memory
// back while nobody's updating.

#include <3ds.h>
#include <curl/curl.h>
#include <mbedtls/sha256.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <string>

#include "json/json.hpp"
#include "updater.h"

#define DATA_PATH "sdmc:/3ds/psnap"
#define RELEASES_URL "https://api.github.com/repos/xXDankeyKongXx/psnap-3ds/releases/latest"
#define CA_PATH "romfs:/cacert.pem"
#define ROMFS_ROM "romfs:/rom/pokemonsnap.z64"
#define SD_ROM DATA_PATH "/pokemonsnap.us.z64"
#define DOWNLOAD_PATH DATA_PATH "/update.part"
#define SOC_BYTES 0x100000

static LightLock lock;
static UpdView view;
static bool auto_allowed, auto_done, curl_ready;
static Thread worker;
static volatile bool busy;
static char self_3dsx[256];

// What the check found, for the install.
static struct {
    std::string url;
    long long size;
    std::string sha256; // hex, or empty when GitHub doesn't list one
} asset;

static void set_state(UpdState state, int percent, const char* message) {
    LightLock_Lock(&lock);
    view.state = state;
    view.percent = percent;
    if (message) snprintf(view.message, sizeof(view.message), "%s", message);
    LightLock_Unlock(&lock);
}

static void fail(const char* message) {
    fprintf(stderr, "[update] %s\n", message);
    set_state(UPD_ERROR, 0, message);
}

// --- Network ---------------------------------------------------------------------------------------

static u32* soc_buffer;

static bool net_up(void) {
    soc_buffer = (u32*)memalign(0x1000, SOC_BYTES);
    if (!soc_buffer) return false;
    if (R_FAILED(socInit(soc_buffer, SOC_BYTES))) {
        free(soc_buffer);
        soc_buffer = NULL;
        return false;
    }
    if (!curl_ready) curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    return curl_ready;
}

static void net_down(void) {
    if (!soc_buffer) return;
    socExit();
    free(soc_buffer);
    soc_buffer = NULL;
}

static CURL* new_request(const char* url) {
    CURL* c = curl_easy_init();
    if (!c) return NULL;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "psnap-3ds/" SNAP_VERSION);
    curl_easy_setopt(c, CURLOPT_CAINFO, CA_PATH);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L); // asset downloads redirect to a storage host
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 64L); // give up on a stalled connection
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 64L * 1024);
    return c;
}

static const char* curl_problem(CURLcode code) {
    switch (code) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT: return "No internet connection.";
    case CURLE_OPERATION_TIMEDOUT: return "The connection timed out.";
    case CURLE_HTTP_RETURNED_ERROR: return "No release found on GitHub.";
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CONNECT_ERROR: return "Couldn't make a secure connection to GitHub.";
    default: return "The download failed.";
    }
}

static size_t to_string(char* p, size_t size, size_t n, void* user) {
    std::string* s = (std::string*)user;
    if (s->size() + size * n > 512 * 1024) return 0; // not a release description
    s->append(p, size * n);
    return size * n;
}

// --- Check -----------------------------------------------------------------------------------------

static int version_of(const std::string& tag) {
    size_t i = 0;
    while (i < tag.size() && (tag[i] < '0' || tag[i] > '9')) i++;
    return atoi(tag.c_str() + i);
}

static bool ends_with(const std::string& s, const char* end) {
    size_t n = strlen(end);
    return s.size() >= n && s.compare(s.size() - n, n, end) == 0;
}

// The release body, for the game's font: ASCII, no Markdown marks or blank lines.
static void plain_notes(const std::string& body, char* out, size_t cap) {
    size_t o = 0;
    bool line_start = true;
    for (size_t i = 0; i < body.size() && o + 1 < cap; i++) {
        unsigned char c = body[i];
        if (c == '\r') continue;
        if (c == '\n') {
            if (!line_start) out[o++] = '\n';
            line_start = true;
            continue;
        }
        if (line_start && (c == '#' || c == '*' || c == '-' || c == ' ')) continue;
        if (c == '*' || c == '`' || c == '_') continue;
        out[o++] = c < 128 ? c : '?';
        line_start = false;
    }
    while (o > 0 && out[o - 1] == '\n') o--;
    out[o] = 0;
}

static void check_job(void) {
    std::string body;
    CURL* c = new_request(RELEASES_URL);
    if (!c) return fail("Out of memory.");
    struct curl_slist* headers = curl_slist_append(NULL, "Accept: application/vnd.github+json");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_string);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    CURLcode code = curl_easy_perform(c);
    curl_easy_cleanup(c);
    curl_slist_free_all(headers);
    if (code != CURLE_OK) {
        fprintf(stderr, "[update] check: curl %d (%s)\n", code, curl_easy_strerror(code));
        return fail(curl_problem(code));
    }

    auto release = nlohmann::json::parse(body, nullptr, false);
    if (release.is_discarded() || !release.is_object() || !release["tag_name"].is_string()) return fail("GitHub's answer didn't make sense.");
    int latest = version_of(release["tag_name"].get<std::string>());
    const char* want = envIsHomebrew() ? ".3dsx" : ".cia";
    asset.url.clear();
    if (release["assets"].is_array()) {
        for (auto& a : release["assets"]) {
            if (!a["name"].is_string() || !a["browser_download_url"].is_string()) continue;
            std::string name = a["name"].get<std::string>();
            if (!ends_with(name, want) || name.find("personal") != std::string::npos) continue;
            asset.url = a["browser_download_url"].get<std::string>();
            asset.size = a["size"].is_number() ? a["size"].get<long long>() : -1;
            asset.sha256.clear();
            if (a["digest"].is_string()) {
                std::string d = a["digest"].get<std::string>();
                if (d.rfind("sha256:", 0) == 0) asset.sha256 = d.substr(7);
            }
            break;
        }
    }
    fprintf(stderr, "[update] latest v%d (this is v%d), asset %s\n", latest, view.current, asset.url.empty() ? "none" : asset.url.c_str());

    LightLock_Lock(&lock);
    view.latest = latest;
    std::string notes = release["body"].is_string() ? release["body"].get<std::string>() : "";
    plain_notes(notes, view.notes, sizeof(view.notes));
    LightLock_Unlock(&lock);
    if (latest <= view.current) set_state(UPD_LATEST, 0, "");
    else if (asset.url.empty()) fail("The new version has no download for this.");
    else set_state(UPD_AVAILABLE, 0, "");
}

// --- Install ---------------------------------------------------------------------------------------

typedef struct {
    FILE* file;
    mbedtls_sha256_context sha;
    long long got;
} Download;

static size_t to_file(char* p, size_t size, size_t n, void* user) {
    Download* d = (Download*)user;
    size_t bytes = size * n;
    if (fwrite(p, 1, bytes, d->file) != bytes) return 0;
    mbedtls_sha256_update_ret(&d->sha, (const unsigned char*)p, bytes);
    d->got += bytes;
    return bytes;
}

static int download_progress(void*, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
    long long size = asset.size > 0 ? asset.size : (long long)total;
    if (size > 0) set_state(UPD_DOWNLOADING, (int)(now * 100 / size), NULL);
    return 0;
}

static bool download(void) {
    set_state(UPD_DOWNLOADING, 0, "");
    Download d;
    d.file = fopen(DOWNLOAD_PATH, "wb");
    if (!d.file) {
        fail("Can't write to the SD card.");
        return false;
    }
    d.got = 0;
    mbedtls_sha256_init(&d.sha);
    mbedtls_sha256_starts_ret(&d.sha, 0);
    CURL* c = new_request(asset.url.c_str());
    CURLcode code = CURLE_OUT_OF_MEMORY;
    if (c) {
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_file);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &d);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, download_progress);
        code = curl_easy_perform(c);
        curl_easy_cleanup(c);
    }
    bool wrote = fclose(d.file) == 0;
    unsigned char hash[32];
    mbedtls_sha256_finish_ret(&d.sha, hash);
    mbedtls_sha256_free(&d.sha);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", hash[i]);
    fprintf(stderr, "[update] downloaded %lld bytes, sha256 %s (curl %d)\n", d.got, hex, code);

    const char* problem = NULL;
    if (code != CURLE_OK) problem = code == CURLE_WRITE_ERROR ? "The SD card is full." : curl_problem(code);
    else if (!wrote) problem = "The SD card is full.";
    else if (asset.size > 0 && d.got != asset.size) problem = "The download was cut short.";
    else if (!asset.sha256.empty() && asset.sha256 != hex) problem = "The download was damaged.";
    if (problem) {
        remove(DOWNLOAD_PATH);
        fail(problem);
        return false;
    }
    return true;
}

static long file_size(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

// A personal build's ROM onto the SD card, unless one is there already.
static bool keep_rom(void) {
    long size = file_size(ROMFS_ROM);
    if (size <= 0 || file_size(SD_ROM) == size) return true;
    set_state(UPD_INSTALLING, 0, "Copying your ROM to the SD card...");
    FILE* in = fopen(ROMFS_ROM, "rb");
    FILE* out = fopen(SD_ROM ".tmp", "wb");
    static u8 buf[256 * 1024];
    bool ok = in && out;
    long done = 0;
    for (size_t n; ok && (n = fread(buf, 1, sizeof(buf), in)) > 0; done += n) {
        ok = fwrite(buf, 1, n, out) == n;
        set_state(UPD_INSTALLING, (int)(done * 100 / size), NULL);
    }
    if (in) fclose(in);
    if (out) ok = fclose(out) == 0 && ok;
    ok = ok && done == size && rename(SD_ROM ".tmp", SD_ROM) == 0;
    if (!ok) {
        remove(SD_ROM ".tmp");
        fail("Couldn't copy your ROM (SD card full?).");
    }
    return ok;
}

static bool install_cia(void) {
    set_state(UPD_INSTALLING, 0, "Installing...");
    if (R_FAILED(amInit())) {
        fail("Can't install (no access to the system's installer).");
        return false;
    }
    FILE* in = fopen(DOWNLOAD_PATH, "rb");
    long size = file_size(DOWNLOAD_PATH);
    Handle cia;
    Result r = in ? AM_StartCiaInstall(MEDIATYPE_SD, &cia) : -1;
    if (R_SUCCEEDED(r)) {
        static u8 buf[256 * 1024];
        u64 offset = 0;
        for (size_t n; R_SUCCEEDED(r) && (n = fread(buf, 1, sizeof(buf), in)) > 0; offset += n) {
            u32 written = 0;
            r = FSFILE_Write(cia, &written, offset, buf, n, 0);
            set_state(UPD_INSTALLING, (int)(offset * 100 / (size > 0 ? size : 1)), NULL);
        }
        r = R_SUCCEEDED(r) ? AM_FinishCiaInstall(cia) : (AM_CancelCIAInstall(cia), r);
    }
    if (in) fclose(in);
    amExit();
    fprintf(stderr, "[update] CIA install: %08lx\n", (unsigned long)r);
    if (R_FAILED(r)) {
        fail("The install didn't work.");
        return false;
    }
    remove(DOWNLOAD_PATH);
    return true;
}

static bool install_3dsx(void) {
    set_state(UPD_INSTALLING, 50, "Installing...");
    char old[272];
    snprintf(old, sizeof(old), "%s.old", self_3dsx);
    remove(old);
    rename(self_3dsx, old);
    if (rename(DOWNLOAD_PATH, self_3dsx) != 0) {
        rename(old, self_3dsx);
        fail("Couldn't replace the .3dsx file.");
        return false;
    }
    remove(old);
    return true;
}

static void install_job(void) {
    if (!download()) return;
    bool ok = envIsHomebrew() ? install_3dsx() : keep_rom() && install_cia();
    if (!ok) {
        remove(DOWNLOAD_PATH);
        return;
    }
    fprintf(stderr, "[update] installed v%d\n", view.latest);
    set_state(UPD_DONE, 100, "");
}

// --- Worker ----------------------------------------------------------------------------------------

static void run(void* arg) {
    bool install = arg != NULL;
    if (!net_up()) fail("Couldn't start the network (out of memory?).");
    else if (install) install_job();
    else check_job();
    net_down();
    busy = false;
}

static void start(bool install) {
    if (busy) return;
    if (worker) {
        threadJoin(worker, U64_MAX);
        threadFree(worker);
        worker = NULL;
    }
    busy = true;
    set_state(install ? UPD_DOWNLOADING : UPD_CHECKING, 0, "");
    // A big stack (TLS handshakes), below the UI and the game in priority, on any core.
    worker = threadCreate(run, install ? (void*)1 : NULL, 128 * 1024, 0x31, -2, false);
    if (!worker) {
        busy = false;
        fail("Couldn't start the updater.");
    }
}

void updater_init(const char* self_path, bool auto_check) {
    LightLock_Init(&lock);
    memset(&view, 0, sizeof(view));
    view.current = atoi(SNAP_VERSION);
    auto_allowed = auto_check;
    const char* path = self_path && strncmp(self_path, "sdmc:/", 6) == 0 ? self_path : DATA_PATH "/psnap-3ds.3dsx";
    snprintf(self_3dsx, sizeof(self_3dsx), "%s", path);
}

void updater_exit(void) {
    if (!worker) return;
    threadJoin(worker, 2000000000ull); // a download in progress is given up on
    if (!busy) threadFree(worker);
    worker = NULL;
}

void updater_auto_check(void) {
    if (!auto_allowed || auto_done) return;
    auto_done = true;
    if (view.state == UPD_IDLE) start(false);
}

void updater_check(void) {
    UpdState s = view.state;
    if (s == UPD_AVAILABLE || s == UPD_DONE || s == UPD_DOWNLOADING || s == UPD_INSTALLING) return;
    start(false);
}

void updater_install(void) {
    if (view.state == UPD_AVAILABLE) start(true);
}

void updater_view(UpdView* out) {
    LightLock_Lock(&lock);
    *out = view;
    LightLock_Unlock(&lock);
}

bool updater_busy(void) {
    UpdState s = view.state;
    return s == UPD_DOWNLOADING || s == UPD_INSTALLING;
}
