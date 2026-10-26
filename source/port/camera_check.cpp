// Camera Check on the touch screen. The game's Camera Check (camera_check overlay, 87D1A0.c) is a menu
// (Oak's Mark / Album Mark / To Prof. Oak) leading to a 3x2 photo grid and a detail view per mark. Here
// the game stays in that menu (func_camera_check_801E1BD4) and a hook in its loop (snap_cc_menu,
// pokemonsnap.us.3ds.toml) does what the touch screen asks with the game's own functions: the marks
// are set by the same calls its detail views make (sounds and rules included: one Oak's Mark per
// Pokemon, album room), and "To Prof. Oak" picks that menu button, so the game's own question and
// everything after it run as usual (snap_cc_confirm answers the question from the touch screen).
// The hook also has the game render the photos: every one at the grid's size for the bottom screen
// and the chosen one at its big view's size (280x210) for the top screen (photo_cache.cpp keeps them).
// While the menu is in charge the game sees no buttons; bottom_ui.c handles them.

#include <3ds.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

#include "recomp.h"
#include "funcs.h"

#include "camera_check.h"
#include "photo_cache.h"
#include "snap_hooks.h"

namespace {

// Camera Check's data (camera_check/87FB20.c, 87D1A0.c, 87A330.c).
constexpr uint32_t LIST = 0x80249B30;    // UnkIndigoHalibut[60], 0x1C each: PhotoData* +0, species +8, flags +0x18
constexpr uint32_t OFFSET = 0x8024A1C0;  // the shown part of the list (its detail views narrow it)
constexpr uint32_t COUNT = 0x8024A1C4;
constexpr uint32_t CURSOR = 0x80208960;  // the photo the mark functions act on
constexpr uint32_t BUTTONS = 0x80249AA8; // UIButton* (id +0, text +4), the menu's buttons
constexpr uint32_t GRID = 0x802499C0;    // UnkTealFlounder[7] (0x20 each; +0xC the photo's SObj)
constexpr uint32_t BIG = 0x802498F0;     // the detail view's UnkTealFlounder
constexpr uint32_t FLAG_NEW = 0x80000000u, FLAG_OAK = 0x20000000u, FLAG_SIGN = 0x10000000u, FLAG_ALBUM = 0x08000000u;
constexpr uint32_t A_BUTTON = 0x8000, B_BUTTON = 0x4000;
constexpr int BUTTON_TO_OAK = 22;
constexpr int SOUND_CURSOR = 69, SOUND_BACK = 0x48;

enum class Act { Select, Oak, Album, Send };
struct Action { Act act; int index; };

std::mutex mutex;
CcView view;                  // as the hook last saw it
std::deque<Action> actions;   // from the touch screen
int selected;
int answer = -1;              // the question's answer: 1 yes, 0 no
std::atomic<uint64_t> menu_tick, confirm_tick;

// Game thread only.
uint32_t photo_ptr[CC_MAX_PHOTOS];
uint64_t photo_hash[CC_MAX_PHOTOS];
bool eligible[CC_MAX_PHOTOS];
bool tried[CC_MAX_PHOTOS];    // asked the game to render it (a photo it can't render stays missing)
int big_tried = -1;

gpr addr(uint32_t a) { return (gpr)(int32_t)a; }
uint32_t rd(uint8_t* rdram, uint32_t a) { return (uint32_t)MEM_W(0, addr(a)); }
void wr(uint8_t* rdram, uint32_t a, uint32_t v) { MEM_W(0, addr(a)) = (int32_t)v; }

uint64_t ms(uint64_t ticks) { return ticks / (SYSCLOCK_ARM11 / 1000); }
bool recent(uint64_t tick) { return tick && ms(svcGetSystemTick() - tick) < 600; }

// Calls into the game (inside its own thread, from the hook): arguments in a0/a1, result in v0.
uint32_t call(uint8_t* rdram, recomp_context* ctx, void (*f)(uint8_t*, recomp_context*), uint32_t a0 = 0, uint32_t a1 = 0) {
    ctx->r4 = addr(a0);
    ctx->r5 = addr(a1);
    f(rdram, ctx);
    return (uint32_t)ctx->r2;
}

uint32_t sprite_of(uint8_t* rdram, uint32_t flounder) { return rd(rdram, flounder + 0x0C) + 0x10; }

void render(uint8_t* rdram, recomp_context* ctx, int i, uint32_t sprite) {
    call(rdram, ctx, func_80374714_847EC4, photo_ptr[i], sprite);
}

// The photos as they are now; a new Camera Check starts over.
void read_list(uint8_t* rdram, recomp_context* ctx, bool fresh) {
    int offset = (int)rd(rdram, OFFSET), count = (int)rd(rdram, COUNT);
    if (offset != 0 || count < 0 || count > CC_MAX_PHOTOS) count = 0; // only the whole list (the menu's)
    bool tutorial_done = call(rdram, ctx, checkPlayerFlag, 8) != 0;  // PFID_HAS_FINISHED_TUTORIAL
    int space = (int)call(rdram, ctx, func_camera_check_801E2E04);   // album room
    int marked = (int)call(rdram, ctx, func_camera_check_801E2CC0);  // Album Marks so far
    CcPhoto photos[CC_MAX_PHOTOS];
    bool any = false;
    for (int i = 0; i < count; i++) {
        uint32_t entry = LIST + (uint32_t)i * 0x1C;
        uint32_t photo = rd(rdram, entry);
        int16_t species = MEM_H(8, addr(entry));
        uint32_t flags = rd(rdram, entry + 0x18);
        if (fresh || photo != photo_ptr[i]) {
            photo_ptr[i] = photo;
            photo_hash[i] = photo >= 0x80000000u && photo < 0x80800000u ? photo_cache_hash(rdram, photo) : 0;
            eligible[i] = call(rdram, ctx, func_camera_check_801E2EF4, (uint32_t)(int32_t)species) != 0;
            tried[i] = false;
        }
        uint8_t marks = 0;
        if (flags & FLAG_NEW) marks |= CC_NEW;
        if (flags & (FLAG_OAK | FLAG_SIGN)) marks |= CC_OAK;
        if (flags & FLAG_ALBUM) marks |= CC_ALBUM;
        if (eligible[i]) marks |= CC_OAK_OK;
        if (species == 9999) marks |= CC_NO_POKEMON;
        any |= (marks & (CC_OAK | CC_ALBUM)) != 0;
        photos[i] = { photo_hash[i], species, marks };
    }
    std::lock_guard lock{ mutex };
    view.count = count;
    memcpy(view.photos, photos, sizeof(CcPhoto) * (size_t)count);
    view.album_open = tutorial_done && space > 0;
    view.album_left = space - marked;
    view.marked = any;
    if (selected >= count) selected = count > 0 ? count - 1 : 0;
}

// The menu button "To Prof. Oak": its index in the menu (the menu's cursor is that number).
int send_button(uint8_t* rdram) {
    uint32_t buttons = rd(rdram, BUTTONS);
    if (buttons < 0x80000000u || buttons >= 0x80800000u) return -1;
    for (int i = 0; i < 8; i++) {
        if ((int)rd(rdram, buttons + (uint32_t)i * 8) == BUTTON_TO_OAK) return i;
    }
    return -1;
}

} // namespace

// In func_camera_check_801E1BD4's loop, right after it read the controller (v0: its state, +0x14
// held, +0x18 pressed; the menu's cursor is the word at sp + 0x38).
extern "C" void snap_cc_menu(uint8_t* rdram, recomp_context* ctx) {
    uint64_t now = svcGetSystemTick();
    bool fresh = !recent(menu_tick.load()) && !recent(confirm_tick.load());
    menu_tick = now;
    uint32_t pad = (uint32_t)ctx->r2;
    for (uint32_t off = 0x14; off <= 0x20; off += 4) wr(rdram, pad + off, 0); // the touch screen's turn
    recomp_context saved = *ctx;

    if (fresh) {
        std::lock_guard lock{ mutex };
        actions.clear();
        selected = 0;
        answer = -1;
        big_tried = -1;
    }
    read_list(rdram, ctx, fresh);

    Action a{ Act::Select, -1 };
    int sel;
    {
        std::lock_guard lock{ mutex };
        if (!actions.empty()) {
            a = actions.front();
            actions.pop_front();
        }
        sel = selected;
    }
    int count = view.count; // only this thread writes it
    bool send = false;
    if (a.index >= 0 && a.index < count) {
        uint32_t entry = LIST + (uint32_t)a.index * 0x1C;
        switch (a.act) {
            case Act::Select:
                call(rdram, ctx, auPlaySound, SOUND_CURSOR);
                break;
            case Act::Oak: // as the detail view's A (func_camera_check_801E04F4)
                if (!eligible[a.index]) {
                    call(rdram, ctx, auPlaySound, SOUND_BACK);
                    break;
                }
                wr(rdram, CURSOR, (uint32_t)a.index);
                if (call(rdram, ctx, func_camera_check_801E04B0, entry)) call(rdram, ctx, func_camera_check_801E0118, 1);
                else call(rdram, ctx, func_camera_check_801E01C0, 1);
                break;
            case Act::Album: // as the album view's A (func_camera_check_801E15B4)
                wr(rdram, CURSOR, (uint32_t)a.index);
                if (!view.album_open) call(rdram, ctx, auPlaySound, SOUND_BACK);
                else call(rdram, ctx, func_camera_check_801E0034, 1);
                break;
            case Act::Send:
                break;
        }
        if (a.act != Act::Select) read_list(rdram, ctx, false);
    }
    if (a.act == Act::Send) send = true;

    // One photo rendered per pass: the chosen one at the big view's size, else the next grid size one.
    if (!send && count > 0) {
        if (sel >= 0 && sel < count && big_tried != sel && photo_hash[sel] && photo_cache_width(photo_hash[sel]) < 280) {
            big_tried = sel;
            render(rdram, ctx, sel, sprite_of(rdram, BIG));
        } else {
            int page = sel / 12 * 12;
            for (int k = 0; k < count; k++) {
                int i = (page + k) % count;
                if (tried[i] || !photo_hash[i]) continue;
                tried[i] = true;
                if (photo_cache_width(photo_hash[i]) > 0) continue;
                render(rdram, ctx, i, sprite_of(rdram, GRID + 6 * 0x20)); // the menu's hidden preview
                break;
            }
        }
    }

    *ctx = saved;
    if (send) {
        int button = send_button(rdram);
        if (button >= 0) {
            wr(rdram, (uint32_t)ctx->r29 + 0x38, (uint32_t)button);
            wr(rdram, pad + 0x18, A_BUTTON);
            std::lock_guard lock{ mutex };
            answer = -1;
        }
    }
    menu_tick = svcGetSystemTick();
}

// In func_camera_check_801E1EA4's loop ("Show this picture to Prof. Oak?"), after its controller read.
extern "C" void snap_cc_confirm(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    confirm_tick = svcGetSystemTick();
    uint32_t pad = (uint32_t)ctx->r2;
    std::lock_guard lock{ mutex };
    if (answer < 0) return;
    wr(rdram, pad + 0x18, answer ? A_BUTTON : B_BUTTON);
    answer = -1;
}

extern "C" CcMode cc_view(CcView* out) {
    uint64_t m = menu_tick.load(), c = confirm_tick.load();
    CcMode mode = CC_OFF;
    if (recent(m) && m >= c) mode = CC_MENU;
    else if (recent(c)) mode = CC_CONFIRM;
    if (out) {
        std::lock_guard lock{ mutex };
        *out = view;
    }
    return mode;
}

extern "C" int cc_selected(void) {
    std::lock_guard lock{ mutex };
    return selected;
}

static void push(Act act, int index) {
    std::lock_guard lock{ mutex };
    if (actions.size() < 8) actions.push_back({ act, index });
}

extern "C" void cc_select(int index) {
    {
        std::lock_guard lock{ mutex };
        if (index < 0 || index >= view.count || index == selected) return;
        selected = index;
    }
    push(Act::Select, index);
}

extern "C" void cc_toggle_oak(int index) { push(Act::Oak, index); }
extern "C" void cc_toggle_album(int index) { push(Act::Album, index); }
extern "C" void cc_send(void) { push(Act::Send, 0); }

extern "C" void cc_answer(bool yes) {
    std::lock_guard lock{ mutex };
    answer = yes ? 1 : 0;
}

extern "C" bool cc_top(uint64_t* hash, uint8_t* marks) {
    if (cc_view(nullptr) != CC_MENU) return false;
    std::lock_guard lock{ mutex };
    if (selected < 0 || selected >= view.count) return false;
    *hash = view.photos[selected].hash;
    *marks = view.photos[selected].marks;
    return true;
}
