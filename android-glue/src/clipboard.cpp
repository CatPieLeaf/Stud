#include "stud/clipboard.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "wayland_overlay_deps.h"

#include "stud/android_glue.h"
#include "x11_backend.h"

// The real system clipboard, for the text box Stud draws itself.
//
// Stud owns text editing while a Lua TextBox is focused (see
// stud/text_overlay.h), so copy and paste are Stud's job too, exactly
// as they are the Android EditText's job on a device. This is ordinary
// wl_data_device work: the process that owns the seat is the only one
// that can hold a selection, so it lives here.
namespace stud::android_glue {
namespace {

constexpr const char* kMimeUtf8 = "text/plain;charset=utf-8";
constexpr const char* kMimePlain = "text/plain";

struct ClipboardState {
    wl_data_device* device = nullptr;
    wl_data_source* source = nullptr;
    // What Stud last put on the clipboard, kept because the compositor
    // asks for it again on every paste by anyone.
    std::string offered;
    // The most recent offer someone else published, and whether it can
    // give us text at all.
    wl_data_offer* offer = nullptr;
    bool offer_has_text = false;
    std::mutex mutex;
};

ClipboardState& clipboard() {
    static ClipboardState c;
    return c;
}

void source_target(void*, wl_data_source*, const char*) {}

void source_send(void* /*data*/, wl_data_source* source, const char* /*mime*/, int32_t fd) {
    std::string payload;
    {
        auto& c = clipboard();
        std::lock_guard<std::mutex> lock(c.mutex);
        if (c.source != source) {
            ::close(fd);
            return;
        }
        payload = c.offered;
    }
    // The reader may go away mid-write (a paste that is cancelled), which
    // arrives as SIGPIPE on a plain write, MSG_NOSIGNAL is not available
    // on a pipe, so the write is simply allowed to fail.
    size_t written = 0;
    while (written < payload.size()) {
        const ssize_t n = ::write(fd, payload.data() + written, payload.size() - written);
        if (n <= 0) break;
        written += static_cast<size_t>(n);
    }
    ::close(fd);
}

void source_cancelled(void*, wl_data_source* source) {
    auto& c = clipboard();
    std::lock_guard<std::mutex> lock(c.mutex);
    if (c.source == source) {
        wl_data_source_destroy(source);
        c.source = nullptr;
    }
}

const wl_data_source_listener kSourceListener = {
    .target = source_target,
    .send = source_send,
    .cancelled = source_cancelled,
    .dnd_drop_performed = [](void*, wl_data_source*) {},
    .dnd_finished = [](void*, wl_data_source*) {},
    .action = [](void*, wl_data_source*, uint32_t) {},
};

void offer_offer(void* /*data*/, wl_data_offer* offer, const char* mime) {
    auto& c = clipboard();
    std::lock_guard<std::mutex> lock(c.mutex);
    if (c.offer != offer) return;
    if (std::strcmp(mime, kMimeUtf8) == 0 || std::strcmp(mime, kMimePlain) == 0) {
        c.offer_has_text = true;
    }
}

const wl_data_offer_listener kOfferListener = {
    .offer = offer_offer,
    .source_actions = [](void*, wl_data_offer*, uint32_t) {},
    .action = [](void*, wl_data_offer*, uint32_t) {},
};

void device_data_offer(void*, wl_data_device*, wl_data_offer* offer) {
    auto& c = clipboard();
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        c.offer = offer;
        c.offer_has_text = false;
    }
    wl_data_offer_add_listener(offer, &kOfferListener, nullptr);
}

void device_selection(void*, wl_data_device*, wl_data_offer* offer) {
    auto& c = clipboard();
    std::lock_guard<std::mutex> lock(c.mutex);
    if (offer == nullptr) {
        c.offer = nullptr;
        c.offer_has_text = false;
        return;
    }
    // The offer announced by data_offer is the one being selected; if the
    // compositor selects a different one, anything it can provide was
    // already reported through that offer's own listener.
    if (c.offer != offer) {
        c.offer = offer;
        c.offer_has_text = false;
    }
}

const wl_data_device_listener kDeviceListener = {
    .data_offer = device_data_offer,
    .enter = [](void*, wl_data_device*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t,
                wl_data_offer*) {},
    .leave = [](void*, wl_data_device*) {},
    .motion = [](void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) {},
    .drop = [](void*, wl_data_device*) {},
    .selection = device_selection,
};

bool ensure_device(const WaylandOverlayDeps& deps) {
    auto& c = clipboard();
    if (c.device != nullptr) return true;
    if (deps.data_device_manager == nullptr || deps.seat == nullptr) return false;
    c.device = wl_data_device_manager_get_data_device(deps.data_device_manager, deps.seat);
    if (c.device == nullptr) return false;
    wl_data_device_add_listener(c.device, &kDeviceListener, nullptr);
    return true;
}

}  // namespace

void clipboard_set_text(const std::string& text) {
    // X11 owns its selections through a window rather than a seat, so it
    // has its own implementation entirely (x11_backend.cpp).
    if (display_backend() == DisplayBackend::X11) {
        x11::clipboard_set(text);
        return;
    }
    const WaylandOverlayDeps deps = overlay_deps();
    if (deps.display == nullptr || !ensure_device(deps)) return;
    auto& c = clipboard();
    wl_data_source* source =
        wl_data_device_manager_create_data_source(deps.data_device_manager);
    if (source == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        c.offered = text;
        if (c.source != nullptr) wl_data_source_destroy(c.source);
        c.source = source;
    }
    wl_data_source_add_listener(source, &kSourceListener, nullptr);
    wl_data_source_offer(source, kMimeUtf8);
    wl_data_source_offer(source, kMimePlain);
    // A selection must be justified by a real input event, which is why
    // the window layer tracks the serial of the last one.
    wl_data_device_set_selection(c.device, source, last_input_serial());
    wl_display_flush(deps.display);
}

std::string clipboard_get_text() {
    if (display_backend() == DisplayBackend::X11) return x11::clipboard_get();
    const WaylandOverlayDeps deps = overlay_deps();
    if (deps.display == nullptr || !ensure_device(deps)) return {};
    auto& c = clipboard();
    wl_data_offer* offer = nullptr;
    bool has_text = false;
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        offer = c.offer;
        has_text = c.offer_has_text;
        // Stud's own selection never round-trips through the compositor:
        // asking for it back would deadlock, since this process would be
        // both the reader and the writer.
        if (c.source != nullptr) return c.offered;
    }
    if (offer == nullptr || !has_text) return {};

    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC) != 0) return {};
    wl_data_offer_receive(offer, kMimeUtf8, fds[1]);
    wl_display_flush(deps.display);
    ::close(fds[1]);

    std::string out;
    char buf[4096];
    // Bounded: a compositor that never writes must not hang the caller.
    for (;;) {
        pollfd p{fds[0], POLLIN, 0};
        const int ready = ::poll(&p, 1, 200);
        if (ready <= 0) break;
        const ssize_t n = ::read(fds[0], buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
        if (out.size() > (1u << 20)) break;  // a text box is not a file drop
    }
    ::close(fds[0]);
    return out;
}

}  // namespace stud::android_glue
