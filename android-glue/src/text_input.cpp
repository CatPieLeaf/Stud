#include "text_input.h"

#include "stud/android_glue.h"
#include "stud/text_overlay.h"

#include <wayland-client.h>
#include <text-input-unstable-v3-client-protocol.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>

namespace stud::android_glue {
namespace {

struct TextInput {
    wl_display* display = nullptr;
    zwp_text_input_manager_v3* manager = nullptr;
    wl_seat* seat = nullptr;
    zwp_text_input_v3* input = nullptr;
    // The surface the compositor says has text-input focus. The protocol
    // only allows enable while there is one.
    wl_surface* entered = nullptr;
    bool enabled = false;

    // What the overlay last asked for.
    bool want_active = false;
    bool sensitive = false;
    int32_t x = 0, y = 0, width = 0, height = 0;
    // The rectangle last sent, so the caret's blink -- a redraw every half
    // second -- does not become a protocol commit every half second.
    int32_t sent_x = -1, sent_y = -1, sent_width = -1, sent_height = -1;

    // Collected between one `done` and the next, then applied together, as
    // the protocol requires.
    std::optional<std::string> pending_commit;
    std::string pending_preedit;
    int32_t pending_preedit_cursor = -1;
    uint32_t pending_delete_before = 0;
    uint32_t pending_delete_after = 0;
};

TextInput& ti() {
    static TextInput t;
    return t;
}

std::mutex& ti_mutex() {
    static std::mutex m;
    return m;
}

// Brings the protocol state in line with what the overlay wants. Called
// with ti_mutex held.
void apply_locked() {
    TextInput& t = ti();
    if (t.input == nullptr) return;
    const bool should_enable = t.want_active && t.entered != nullptr;
    bool changed = false;
    if (should_enable && !t.enabled) {
        zwp_text_input_v3_enable(t.input);
        // A password box is still text an input method may type into, but
        // it must not learn from it or show it; that is what these two
        // hints ask for.
        zwp_text_input_v3_set_content_type(
            t.input,
            t.sensitive ? (ZWP_TEXT_INPUT_V3_CONTENT_HINT_SENSITIVE_DATA |
                           ZWP_TEXT_INPUT_V3_CONTENT_HINT_HIDDEN_TEXT)
                        : ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
            t.sensitive ? ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD
                        : ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
        t.enabled = true;
        t.sent_x = t.sent_y = t.sent_width = t.sent_height = -1;
        changed = true;
    } else if (!should_enable && t.enabled) {
        zwp_text_input_v3_disable(t.input);
        t.enabled = false;
        zwp_text_input_v3_commit(t.input);
        if (t.display != nullptr) wl_display_flush(t.display);
        return;
    }
    if (t.enabled && (t.x != t.sent_x || t.y != t.sent_y || t.width != t.sent_width ||
                      t.height != t.sent_height)) {
        zwp_text_input_v3_set_cursor_rectangle(t.input, t.x, t.y, t.width, t.height);
        t.sent_x = t.x;
        t.sent_y = t.y;
        t.sent_width = t.width;
        t.sent_height = t.height;
        changed = true;
    }
    if (changed) {
        zwp_text_input_v3_commit(t.input);
        if (t.display != nullptr) wl_display_flush(t.display);
    }
}

void on_enter(void*, zwp_text_input_v3*, wl_surface* surface) {
    std::lock_guard<std::mutex> lock(ti_mutex());
    ti().entered = surface;
    apply_locked();
}

void on_leave(void*, zwp_text_input_v3*, wl_surface*) {
    std::lock_guard<std::mutex> lock(ti_mutex());
    ti().entered = nullptr;
    apply_locked();
}

void on_preedit_string(void*, zwp_text_input_v3*, const char* text, int32_t cursor_begin,
                       int32_t /*cursor_end*/) {
    std::lock_guard<std::mutex> lock(ti_mutex());
    ti().pending_preedit = text != nullptr ? text : "";
    ti().pending_preedit_cursor = cursor_begin;
}

void on_commit_string(void*, zwp_text_input_v3*, const char* text) {
    std::lock_guard<std::mutex> lock(ti_mutex());
    ti().pending_commit = text != nullptr ? text : "";
}

void on_delete_surrounding_text(void*, zwp_text_input_v3*, uint32_t before, uint32_t after) {
    std::lock_guard<std::mutex> lock(ti_mutex());
    ti().pending_delete_before = before;
    ti().pending_delete_after = after;
}

void on_done(void*, zwp_text_input_v3*, uint32_t /*serial*/) {
    // In the protocol's own order: delete around the caret, then insert
    // the commit, then show the new preedit.
    std::optional<std::string> commit;
    std::string preedit;
    int32_t preedit_cursor = -1;
    uint32_t before = 0;
    uint32_t after = 0;
    {
        std::lock_guard<std::mutex> lock(ti_mutex());
        TextInput& t = ti();
        commit.swap(t.pending_commit);
        preedit.swap(t.pending_preedit);
        preedit_cursor = t.pending_preedit_cursor;
        before = t.pending_delete_before;
        after = t.pending_delete_after;
        t.pending_preedit_cursor = -1;
        t.pending_delete_before = 0;
        t.pending_delete_after = 0;
    }
    if (before != 0 || after != 0) text_input_push_delete_surrounding(before, after);
    if (commit && !commit->empty()) text_input_push_commit(*commit);
    // Outside ti_mutex: the overlay takes its own lock and, redrawing,
    // comes back here through text_input_set_target().
    set_text_overlay_preedit(preedit, preedit_cursor);
}

// Built by assignment, not a designated initialiser: newer copies of the
// protocol add events (action, language, preedit_hint) that only arrive
// at version 2. Stud binds version 1, so they are never sent, and naming
// them here would not compile against the older copies distributions
// still ship.
const zwp_text_input_v3_listener kListener = [] {
    zwp_text_input_v3_listener l{};
    l.enter = on_enter;
    l.leave = on_leave;
    l.preedit_string = on_preedit_string;
    l.commit_string = on_commit_string;
    l.delete_surrounding_text = on_delete_surrounding_text;
    l.done = on_done;
    return l;
}();

void create_if_ready_locked() {
    TextInput& t = ti();
    if (t.input != nullptr || t.manager == nullptr || t.seat == nullptr) return;
    t.input = zwp_text_input_manager_v3_get_text_input(t.manager, t.seat);
    if (t.input != nullptr) zwp_text_input_v3_add_listener(t.input, &kListener, nullptr);
    std::printf("stud: android-glue: input methods available (text-input-v3)\n");
    std::fflush(stdout);
}

}  // namespace

void text_input_bind_manager(wl_display* display, wl_registry* registry, uint32_t name,
                             uint32_t /*version*/) {
    std::lock_guard<std::mutex> lock(ti_mutex());
    TextInput& t = ti();
    if (t.manager != nullptr) return;
    t.display = display;
    t.manager = static_cast<zwp_text_input_manager_v3*>(
        wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, 1));
    create_if_ready_locked();
}

void text_input_attach_seat(wl_seat* seat) {
    std::lock_guard<std::mutex> lock(ti_mutex());
    ti().seat = seat;
    create_if_ready_locked();
}

void text_input_set_target(bool active, bool sensitive, int32_t x, int32_t y, int32_t width,
                           int32_t height) {
    std::lock_guard<std::mutex> lock(ti_mutex());
    TextInput& t = ti();
    // A box that turns into a password box (or back) needs the content
    // type sent again, which only an enable does.
    if (t.enabled && sensitive != t.sensitive && t.input != nullptr) {
        zwp_text_input_v3_disable(t.input);
        zwp_text_input_v3_commit(t.input);
        t.enabled = false;
    }
    t.want_active = active;
    t.sensitive = sensitive;
    t.x = x;
    t.y = y;
    t.width = width;
    t.height = height;
    apply_locked();
}

void text_input_push_commit(const std::string& utf8) {
    // HostInputEvent carries text in a fixed field, so a long commit goes
    // over in pieces, split between characters, never inside one; Process
    // B inserts them in order.
    constexpr size_t kField = sizeof(HostInputEvent{}.composed_utf8) - 1;
    size_t i = 0;
    while (i < utf8.size()) {
        size_t n = utf8.size() - i < kField ? utf8.size() - i : kField;
        // Back off to a character boundary: a continuation byte is
        // 10xxxxxx.
        while (n > 0 && i + n < utf8.size() &&
               (static_cast<unsigned char>(utf8[i + n]) & 0xC0) == 0x80) {
            --n;
        }
        if (n == 0) break;
        HostInputEvent ev;
        ev.type = HostInputEvent::kTextCommit;
        std::memcpy(ev.composed_utf8, utf8.data() + i, n);
        ev.composed_utf8[n] = '\0';
        push_host_input_event(ev);
        i += n;
    }
}

void text_input_push_delete_surrounding(uint32_t before_bytes, uint32_t after_bytes) {
    HostInputEvent ev;
    ev.type = HostInputEvent::kTextDeleteSurrounding;
    ev.a = static_cast<float>(before_bytes);
    ev.b = static_cast<float>(after_bytes);
    push_host_input_event(ev);
}

}  // namespace stud::android_glue
