#include "stud/input_bridge.h"

#include "stud/haptics_bridge.h"

#include <functional>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <cstring>
#include <thread>
#include <vector>

#include "render_client_common.h"
#include "stud/android_glue.h"
#include "stud/android_framework_stubs.h"
#include "stud/game_activity_stubs.h"
#include "stud/key_map.h"
#include "stud/text_editor.h"
#include "stud/bionic_jvm.h"
#include "stud/trap_recovery.h"
#include "mouse_behavior.h"

using stud::render_host::CallId;

namespace stud::jni_bridge {

namespace {

std::atomic<bool> g_running{false};
FakeJni::Jvm* g_jvm = nullptr;

using MouseMoveFn = void (*)(JNIEnv*, jclass, jfloat, jfloat, jfloat, jfloat);
// Whether the engine currently wants the mouse held at the centre -- what
// it does while a camera is being turned. The real app polls this on every
// mouse event and takes or drops Android's pointer capture accordingly
// (the app's own input handler's own OnGenericMotionListener/OnCapturedPointerListener pair).
using IsMouseLockedFn = jboolean (*)(JNIEnv*, jclass);
using MouseButtonFn = void (*)(JNIEnv*, jclass, jfloat, jfloat, jboolean, jint);
using MouseWheelFn = void (*)(JNIEnv*, jclass, jfloat, jfloat, jfloat);
using KeyEventFn = void (*)(JNIEnv*, jclass, jboolean, jint, jint, jboolean);
// nativePassInput(pointerId, x, y, state, viewWidth, viewHeight) -- the real
// touch path, state 0 = down, 1 = move, 2 = up (confirmed against the app's own code from the real
// handler in the app's own input handler: `hVar2.d(0)` on ACTION_DOWN, `.d(1)` on move).
using PassInputFn = void (*)(JNIEnv*, jclass, jint, jfloat, jfloat, jint, jint, jint);
// nativePassText(textBoxHandle, fullText, done, cursorPos) and
// nativeReturnPressedFromOnScreenKeyboard(textBoxHandle) -- the real path
// a focused Lua TextBox takes its text through (RbxKeyboard.java:55,67).
//
// STATE OF TEXT ENTRY (measured against THIS APK, 2.736.1408, not assumed):
//   * Key events reach the engine. Enter works. Focus works: the engine calls
//     showKeyboard with the real TextBox handle, and its own log confirms
//     handleTextBoxFocused_AndroidLayer_ for that exact handle.
//   * nativePassText's compiled body opens with `test %r8d,%r8d; je <epilogue>`
//     -- with done=false (what a real device sends on every keystroke) it
//     returns immediately having done nothing. Its done=true branch is the
//     COMMIT: live-confirmed, the search box submits after every letter.
//   * So live typing rides on syncTextboxTextAndCursorPosition2(text, cursor),
//     which RbxKeyboard.onTextChanged calls FIRST -- confirmed against
//     this APK's own RbxKeyboard, not an older copy.
//   * Stud calls it, and it has no effect, because its body bails out at
//     `mov 0x9c8(%rdi),%rax; test %rax,%rax; je <exit>` -- the engine's own
//     "currently focused text box" pointer is null in Stud even though the
//     focus callback demonstrably ran.
// The open question is precisely: what populates that pointer on a real
// device. Disproven already, do not repeat: the IME handshake
// (nativeGetTextBoxInfo + updateKeyboardSize(true, ...)) does not arm it.
using PassTextFn = void (*)(JNIEnv*, jclass, jlong, jstring, jboolean, jint);
using ReturnPressedFn = void (*)(JNIEnv*, jclass, jlong);
// NativeGLInterface.syncTextboxTextAndCursorPosition2(String text, int cursor).
// This -- not nativePassText -- is what carries live typing in this build.
// RbxKeyboard.onTextChanged calls k() (which is exactly this) FIRST and only
// then nativePassText(..., done=false, ...), and nativePassText's own compiled
// body opens with `test %r8d,%r8d; je <epilogue>`: with done == false it
// returns immediately having done nothing at all. Stud delivered only the
// call that is a no-op, so every keystroke reached the engine and was
// discarded. Takes no TextBox handle -- the engine applies it to whatever it
// currently considers focused.
using SyncTextFn = void (*)(JNIEnv*, jclass, jstring, jint);

// Game controllers. Signatures read off the real exported symbols; the
// engine takes Android's own keycodes and axis ids, which is exactly what
// render-host already produces from evdev.
using MousePinchFn = void (*)(JNIEnv*, jclass, jfloat, jfloat, jfloat);
using GamepadConnectFn = void (*)(JNIEnv*, jclass, jint /*deviceId*/, jint /*type*/);
using GamepadDisconnectFn = void (*)(JNIEnv*, jclass, jint /*deviceId*/);
using GamepadButtonFn = void (*)(JNIEnv*, jclass, jint /*deviceId*/, jint /*keyCode*/,
                                  jint /*action*/);
using GamepadAxisFn = void (*)(JNIEnv*, jclass, jint /*deviceId*/, jint /*axis*/, jfloat,
                                jfloat, jfloat);
using GamepadSupportedKeyFn = void (*)(JNIEnv*, jclass, jint /*deviceId*/, jint /*keyCode*/,
                                        jboolean /*supported*/, jint /*type*/);
using GamepadSupportedMotionFn = void (*)(JNIEnv*, jclass, jint /*deviceId*/, jint /*axis*/,
                                           jint /*direction*/, jboolean /*supported*/,
                                           jint /*type*/);
// NativeGLInterface.updateKeyboardSize(boolean visible, int x, int y, int w, int h)
// and nativeGetTextBoxInfo(): the rest of the handshake a real device performs
// when its IME actually opens over the GL view. Under test because
// syncTextboxTextAndCursorPosition2's compiled body bails out immediately when
// the engine's own "currently focused text box" pointer is null
// (`mov 0x9c8(%rdi),%rax; test %rax,%rax; je <exit>`), and in Stud it is --
// even though the engine logs handleTextBoxFocused_AndroidLayer_.
using UpdateKeyboardSizeFn = void (*)(JNIEnv*, jclass, jboolean, jint, jint, jint, jint);
using GetTextBoxInfoFn = jobject (*)(JNIEnv*, jclass);
//
// Live-tested and DISPROVEN, twice, do not re-try: completing the IME
// handshake on focus -- nativeGetTextBoxInfo() plus updateKeyboardSize(true,
// ...) -- does not arm the pointer sync needs. Tried once with zero geometry
// and again with a real open-keyboard rectangle (the real caller only reports
// visible=true when the measured height exceeds 10, so the zero case was not
// a fair test); neither changed anything. Both are still resolved here
// because they are real entry points a real device calls.

struct InputFns {
    MouseMoveFn mouse_move = nullptr;
    MouseButtonFn mouse_button = nullptr;
    MouseWheelFn mouse_wheel = nullptr;
    KeyEventFn key_event = nullptr;
    PassInputFn pass_input = nullptr;
    PassTextFn pass_text = nullptr;
    ReturnPressedFn return_pressed = nullptr;
    SyncTextFn sync_text = nullptr;
    UpdateKeyboardSizeFn update_keyboard_size = nullptr;
    GetTextBoxInfoFn get_text_box_info = nullptr;
    IsMouseLockedFn is_mouse_locked = nullptr;
    MousePinchFn mouse_pinch = nullptr;
    GamepadConnectFn gamepad_connect = nullptr;
    GamepadDisconnectFn gamepad_disconnect = nullptr;
    GamepadButtonFn gamepad_button = nullptr;
    GamepadAxisFn gamepad_axis = nullptr;
    GamepadSupportedKeyFn gamepad_supported_key = nullptr;
    GamepadSupportedMotionFn gamepad_supported_motion = nullptr;
};

// Mouse look. Roblox's own camera puts the mouse into
// Enum.MouseBehavior.LockCurrentPosition for the whole of a rotation drag:
// it stops moving its own cursor and steers the camera from the deltas
// alone. A desktop client answers that by holding the OS pointer still, so
// nothing drifts and the cursor is exactly where it was when the button
// comes back up. Stud had no such thing -- the real pointer walked across
// the desktop while the camera turned, and the engine adopted wherever it
// had got to the moment the drag ended. That is the teleport.
//
// The engine never says which mode it is in. Its one exported predicate,
// nativeGetMainWindowIsMouseLockedCenter, answers only for LockCenter (the
// shift-lock/first-person case) -- live-confirmed returning false through a
// whole rotation drag. So a real Android device with a mouse attached
// drifts in exactly the same way; it simply never comes up on a phone.
// Holding the pointer for the duration of any button hold is what the real
// desktop client does, and it needs no signal the engine does not give.
// STUD_NO_MOUSE_LOCK=1 turns it off.
// Real MotionEvent button bits. The camera is dragged with either the
// right button or the middle one -- the middle is what a mouse with no
// usable right button (or a player who binds it that way) turns the view
// with, and every desktop client treats the two the same here.
constexpr jint kButtonSecondary = 2;
constexpr jint kButtonTertiary = 4;
constexpr jint kCameraButtons = kButtonSecondary | kButtonTertiary;
std::atomic<bool> g_drag_locked{false};
// Taking the physical pointer away for a camera drag, off by default.
//
// It was how the cursor was kept still through an orbit, and it is no
// longer needed: the cursor is simply left where it is for the whole
// gesture and the pointer is put back on it at the end (see
// g_drag_confined). A lock also takes the pointer's position away
// entirely, which is what made a right-drag on an in-game UI overlay
// unusable. STUD_DRAG_LOCK=1 brings it back.
//
// The engine's OWN request (LockCenter: first person, shift lock) is a
// different thing entirely and is still honoured.
bool drag_lock_enabled() {
    static const bool enabled = std::getenv("STUD_DRAG_LOCK") != nullptr;
    return enabled;
}
bool mouse_lock_enabled() {
    static const bool enabled = std::getenv("STUD_NO_MOUSE_LOCK") == nullptr;
    return enabled;
}
// Mouse look: hold the physical pointer still while the engine has its own
// cursor pinned, and do nothing else.
//
// Nothing is warped on release. Under a lock the compositor does not move
// the pointer at all, so when the lock ends it is still exactly where the
// drag began -- which is where the engine's pinned cursor is. Asking the
// compositor to place it somewhere is both unnecessary and a trick: an
// earlier version set a cursor-position hint from the accumulated
// position, which papered over the real question of where the engine
// thinks its cursor is, and (once positions moved to density-independent
// units) warped to the wrong place anyway.
// Set when a lock ends, so the next ordinary motion event adopts the real
// pointer position without turning the difference into a delta.
//
// While locked the reported position is accumulated from raw deltas and
// the physical pointer has not moved, so the two are far apart by the time
// the drag ends. The first unlocked motion event would otherwise report
// `true_position - accumulated_position` as its delta -- a single huge
// step that the engine applies as camera movement. That is the rotation
// jumping after a few spins.
std::atomic<bool> g_resync_after_unlock{false};
// The last physical pointer position seen, in the same units as last_x/
// last_y. Movement is measured against this; the reported position is
// integrated from it rather than set to it.
float g_prev_raw_x = 0.0f;
float g_prev_raw_y = 0.0f;
bool g_have_prev_raw = false;

// Two things can ask for the pointer lock and they must not fight over
// it: the engine's own LockCenter (first person, polled every ~8ms) and a
// right-button drag (a camera orbit). They were both calling
// set_pointer_locked() directly, so a lock taken on right-button-down was
// destroyed by the very next poll -- measured in Stud's own Wayland
// traffic, 34 MICROSECONDS after the request, long before the compositor
// could activate it. That is why it never once sent locked() back, why
// the pointer kept moving through an orbit, and why every variation of
// the surrounding logic behaved identically: the lock was never in
// effect at all.
bool g_lock_from_engine = false;
bool g_lock_from_drag = false;
// Escape lets go of the lock without letting go of the button.
//
// A camera drag holds the pointer for as long as the button is down, so a
// drag that starts over something it should not have -- or simply a hand
// that wants the desktop back -- has no way out but releasing. Escape is
// what a player already presses (it opens Roblox's own menu), so it ends
// the lock too, and the latch keeps it ended: re-locking would be
// immediate otherwise, since the button is still held. Cleared the first
// moment nothing is asking for the lock, so the next drag re-arms it.
bool g_lock_suppressed = false;

// The cursor stays where the engine left it when a camera drag ends.
//
// The engine pins its OWN cursor for the whole of a camera rotation
// (MouseBehavior.LockCurrentPosition) and steers from the deltas -- with
// or without any pointer lock, live-observed with locking off entirely.
// It ignores the position it is given for the duration and picks it up
// again on release, so a slow orbit that walked the hand 224 units left
// ends with the cursor jumping exactly that far:
//
//   press    pos=(525.5,559.2)          <- where the engine pins its cursor
//   ...1365 moves, hand travels left...
//   release  pos=(301.8,616.0)          <- adopted, and the cursor jumps
//
// So the gesture's starting point is remembered, and from the release
// onwards every position is reported shifted by the distance between it
// and where the hand ended. The cursor carries on from where it visibly
// is, which is the whole of what was missing -- no lock, no warp, nothing
// put back.
//
// This is NOT integration: each position is still the pointer's own plus
// one constant, fixed once per gesture, so nothing accumulates while the
// mouse moves. The position is also NOT frozen during the drag -- tried,
// and it made the rotation itself wrong.
bool g_drag_anchored = false;
float g_drag_anchor_x = 0.0f;
float g_drag_anchor_y = 0.0f;
// The same point in surface pixels, which is what a warp takes. At the
// press the cursor and the pointer are together, so this is simply where
// the press happened.
float g_drag_anchor_px = 0.0f;
float g_drag_anchor_py = 0.0f;

// The pointer is kept inside the window for a camera drag -- confined,
// not taken.
//
// A lock replaces the pointer's position with deltas, and every attempt
// to rebuild a position from those deltas changed how the engine's cursor
// behaves on UI and 2D-camera drags. A confinement changes nothing about
// the pointer: it keeps its real position and its ordinary motion events,
// it simply cannot cross the window's edge. That edge would otherwise be
// a wall for the camera (a pointer that can go no further reports no more
// motion), so the movement comes from the compositor's RELATIVE stream,
// which keeps reporting what the device did however far the pointer got.
bool g_drag_confined = false;

// What the engine says it is doing with its own cursor, polled beside the
// LockCenter poll. This is the fact every earlier attempt at this had to
// guess at -- see mouse_behavior.h.
std::atomic<int> g_engine_mouse_behavior{
    static_cast<int>(stud::runtime::MouseBehavior::kUnknown)};

// How long the host may wait for an event, and equally how often input is
// asked for when it has to share the render connection.
//
// STUD_INPUT_POLL_MS overrides it. 4ms rather than 8: the engine paints
// its cursor inside the frame, so every millisecond between the hand
// moving and the engine hearing about it is a millisecond the cursor is
// visibly behind -- there is no hardware cursor here to hide it, the way
// there is on Windows.
int poll_interval_ms() {
    static const int ms = [] {
        const char* v = std::getenv("STUD_INPUT_POLL_MS");
        const int n = v != nullptr ? std::atoi(v) : 4;
        return n > 0 ? n : 4;
    }();
    return ms;
}

bool engine_pins_cursor() {
    // Either locked mode: the engine holds its own cursor and ignores
    // every position it is given. LockCurrentPosition is a camera
    // rotation, LockCenter is first person -- for the cursor they are the
    // same thing, and only Default means "follow the pointer".
    const int behavior = g_engine_mouse_behavior.load();
    return behavior == static_cast<int>(stud::runtime::MouseBehavior::kLockCurrentPosition) ||
           behavior == static_cast<int>(stud::runtime::MouseBehavior::kLockCenter);
}

bool engine_locks_center() {
    return g_engine_mouse_behavior.load() ==
           static_cast<int>(stud::runtime::MouseBehavior::kLockCenter);
}

// Set the moment the engine starts pinning during a gesture, and holds
// where its cursor stopped. Until then a drag is an ordinary drag and the
// cursor follows the hand, which is what an in-game UI needs.
bool g_pin_seen = false;
float g_pin_x = 0.0f;
float g_pin_y = 0.0f;
float g_pin_px = 0.0f;
float g_pin_py = 0.0f;

// The last raw pointer position in surface pixels, which is what a warp
// takes -- kept because the pin can begin on a poll, away from any event.
float g_last_raw_px = 0.0f;
float g_last_raw_py = 0.0f;

// A warp in flight, and everything it will stir up.
//
// Moving the pointer produces motion events like any other movement, and
// the compositor may still have one or two of the HAND's own events in
// flight when the warp is asked for. A single "take the next event as-is"
// flag is not enough: the stale event eats it, and then the warp's own
// arrival is read as real movement -- live-caught as the cursor jumping
// back to where the hand had been and a -390 unit delta reaching the
// camera in one step, which is exactly the spin teleporting.
//
// So a warp is pending until the pointer is actually SEEN at the place it
// was sent to. Everything up to that point is the warp's business and
// none of the engine's: the position stays where the engine's cursor is
// and no movement is reported at all.
bool g_warp_pending = false;
float g_warp_target_px = 0.0f;
float g_warp_target_py = 0.0f;
// Where the pointer was when the warp was asked for. The echo is
// recognised by being nearer the place it was sent to than the place it
// came from, which ends the window on the FIRST event after the warp
// lands however fast the hand is still moving -- waiting for the pointer
// to be exactly on the target instead held the cursor still for a couple
// of hundred milliseconds whenever the hand had not stopped.
float g_warp_from_px = 0.0f;
float g_warp_from_py = 0.0f;
std::chrono::steady_clock::time_point g_warp_started{};


// Whether the compositor can move the pointer at all (wp_pointer_warp_v1,
// or X11's own warp). Asked once. Without it the pointer cannot be put
// back on the cursor when a drag ends, and the cursor goes to the pointer
// instead -- the old jump, on old compositors only.
bool pointer_warp_available() {
    static const bool available = []() {
        uint64_t a[8] = {};
        return stud::render_client::connection().call(CallId::CanWarpPointer, a, nullptr, 0,
                                                       nullptr, 0, nullptr) != 0;
    }();
    return available;
}

void warp_pointer_to(float surface_x, float surface_y) {
    uint64_t a[8] = {};
    a[0] = static_cast<uint64_t>(static_cast<int64_t>(surface_x * 256.0f));
    a[1] = static_cast<uint64_t>(static_cast<int64_t>(surface_y * 256.0f));
    stud::render_client::connection().call(CallId::WarpPointer, a, nullptr, 0, nullptr, 0, nullptr);
}

void set_pointer_confined(bool confined) {
    if (confined == g_drag_confined) return;
    g_drag_confined = confined;
    uint64_t a[8] = {};
    a[0] = confined ? 1 : 0;
    stud::render_client::connection().call(CallId::SetPointerConfined, a, nullptr, 0, nullptr, 0,
                                           nullptr);
    if (std::getenv("STUD_INPUT_TRACE") != nullptr) {
        std::printf("stud: pointer %s the window for the drag\n",
                    confined ? "confined to" : "released from");
        std::fflush(stdout);
    }
}

void begin_warp(float target_px, float target_py) {
    if (!pointer_warp_available()) return;
    warp_pointer_to(target_px, target_py);
    g_warp_pending = true;
    g_warp_target_px = target_px;
    g_warp_target_py = target_py;
    g_warp_from_px = g_last_raw_px;
    g_warp_from_py = g_last_raw_py;
    // A bound in TIME, so a warp the compositor quietly drops cannot
    // swallow the pointer -- and short, because every millisecond of it
    // is a millisecond the cursor does not move.
    g_warp_started = std::chrono::steady_clock::now();
}

void set_pointer_locked(bool locked) {
    if (locked == g_drag_locked.load()) return;
    g_drag_locked.store(locked);
    if (!locked) g_resync_after_unlock.store(true);
    if (std::getenv("STUD_INPUT_TRACE") != nullptr) {
        std::printf("stud: pointer lock %s\n", locked ? "ON" : "off");
        std::fflush(stdout);
    }
    uint64_t a[8] = {};
    a[0] = locked ? 1 : 0;
    stud::render_client::connection().call(CallId::SetPointerLocked, a, nullptr, 0, nullptr, 0,
                                           nullptr);
}

// The lock is held while EITHER reason holds it -- unless the player is
// typing, or Escape has just let go of it.
//
// Typing: a focused Lua TextBox means the mouse is not steering a camera,
// and pinning the pointer while someone types into chat is only ever in
// the way. The engine's own LockCenter still wins -- Roblox can keep
// shift-lock on with chat focused, and that is its decision to make, not
// a drag Stud inferred.
void apply_pointer_lock() {
    if (!mouse_lock_enabled()) return;
    const bool typing = NativeGLJavaInterfaceStub::active_text_box() != 0;
    const bool dragging = g_lock_from_drag && !typing;
    const bool asked = g_lock_from_engine || dragging;
    if (!asked) g_lock_suppressed = false;
    set_pointer_locked(asked && !g_lock_suppressed);
}




// Real Android behaviour with a real mouse attached, read directly out of
// the app's own handler (`the app's own input handler`) rather than assumed:
//
//   boolean isMouse  = (event.getSource() & 8194) == 8194;   // SOURCE_MOUSE
//   boolean isFinger = isMouse && event.getToolType(0) == 1; // TOOL_TYPE_FINGER
//   if (isMouse && !isFinger) {
//       if (event.getButtonState() != 0) return y(event);    // mouse path only
//       ...                                                   // scroll
//       return true;                                          // early return
//   }
//   ... only non-mouse events reach nativePassInput
//
// So a real mouse produces ONLY nativePassMouse{Move,Button,Wheel} and
// NEVER nativePassInput -- the touch path is for actual fingers. Stud used
// to synthesize a touch pointer from the primary button on the theory that
// the app shell's buttons were touch-driven; that is a real deviation from
// the device, and touch input is also what makes Roblox suppress its own
// in-frame mouse cursor. Kept behind an opt-in env var purely so the two
// behaviours can still be compared live, default OFF.
bool touch_synthesis_enabled() {
    static const bool enabled = std::getenv("STUD_SYNTHESIZE_TOUCH") != nullptr;
    return enabled;
}
bool g_touch_down = false;

void send_touch(const InputFns& fns, JNIEnv* jni_env,
                const stud::android_glue::HostInputEvent& ev, int state) {
    if (!touch_synthesis_enabled() || fns.pass_input == nullptr) return;
    call_trapping_abort(fns.pass_input, jni_env, nullptr, static_cast<jint>(0), ev.x, ev.y,
                        static_cast<jint>(state), static_cast<jint>(ev.surface_width),
                        static_cast<jint>(ev.surface_height));
}

// Real AGDK GameActivity input delivery. AGDK's own Java side calls:
//   onTouchEventNative(handle, event, pointerCount, historySize, deviceId,
//       source, action, eventTime, downTime, flags, metaState, actionButton,
//       buttonState, classification, edgeFlags, precisionX, precisionY)
//   onKeyDownNative(handle, keyEvent) / onKeyUpNative(handle, keyEvent)
// (GameActivity.java:88-95, 215-222). These are registered by
// initializeNativeCode via RegisterNatives -- they are NOT exported symbols,
// so they must be invoked as ordinary JNI methods on the activity object,
// exactly like drive_game_activity_lifecycle() already does.
struct AgdkInput {
    std::shared_ptr<MainGameActivityStub> activity;
    jlong handle = 0;
    bool resolved = false;
    jmethodID on_key_down = nullptr;
    jmethodID on_key_up = nullptr;
    jmethodID on_text_input = nullptr;
    jmethodID on_window_focus_changed = nullptr;
};
AgdkInput g_agdk;
// Set for the duration of one drained batch, so dispatch_event() can reach
// the frame's own env/activity reference without threading them through
// every call.
FakeJni::Env* g_agdk_env = nullptr;
jobject g_agdk_activity_ref = nullptr;
jint g_button_state = 0;
// Every key this process currently believes is held, by evdev code.
// Touched only from the input thread, which is one thread.
std::set<uint32_t>& held_keys() {
    static std::set<uint32_t> k;
    return k;
}

// Keys that were held when a text box took focus, and whose release has
// already been sent.
//
// Releasing them once is not enough on its own: the compositor reports a
// held key ~25 times a second (android-glue synthesises the repeats
// Wayland leaves to the client), so the very next repeat presses the key
// straight back down and the character carries on walking. A key stays
// here until it is genuinely let go, or genuinely pressed again.
std::set<uint32_t>& keys_released_into_text_entry() {
    static std::set<uint32_t> k;
    return k;
}

// Set when a text box has just taken focus with keys held, so the window
// focus the engine was told it lost can be given back on the next round.
bool g_restore_window_focus_next_round = false;

// Real Android meta-state bits, tracked from the physical modifier keys so
// every KeyEvent Stud synthesizes reports them the way a real OTG keyboard
// would: META_SHIFT_ON=0x1, META_ALT_ON=0x02, META_CTRL_ON=0x1000.
jint g_meta_state = 0;
jlong g_down_time = 0;

// Real android.view.InputDevice / MotionEvent constants.
constexpr jint kSourceMouse = 0x2002;      // SOURCE_MOUSE
constexpr jint kSourceKeyboard = 0x101;    // SOURCE_KEYBOARD
constexpr jint kKeyCodeBack = 4;           // KEYCODE_BACK
constexpr jint kMetaAlt = 0x02;            // META_ALT_ON
constexpr jint kToolTypeMouse = 3;         // TOOL_TYPE_MOUSE
constexpr jint kActionHoverEnter = 9;
constexpr jint kActionHoverExit = 10;
constexpr jint kActionScroll = 8;
constexpr jint kKeyActionDown = 0;
constexpr jint kKeyActionUp = 1;

// Hands the focused TextBox its whole updated contents, in the real order
// RbxKeyboard uses: syncTextboxTextAndCursorPosition2 first (the call that
// actually carries live typing in this build), then nativePassText.
void deliver_text(const InputFns& fns, const std::string& text, long text_box, bool done) {
    if (g_jvm == nullptr) return;
    FakeJni::LocalFrame text_frame(*g_jvm);
    auto& tenv = text_frame.getJniEnv();
    auto* env = static_cast<JNIEnv*>(&tenv);
    // STUD_NO_TEXT_SYNC=1 skips the Android text path entirely, leaving
    // only raw key events.
    //
    // Real-device logcat (Waydroid) shows a phone drives text through
    // Android's IME -- RemoteInputConnectionImpl, requestCursorUpdates,
    // an EditText -- which is why selecting text there shows ANDROID's
    // highlight. Sober on this machine shows ROBLOX's own font and
    // highlight instead, so it is not on that path at all: the engine is
    // editing and drawing the text itself, the way the desktop client
    // does. Stud reports a desktop client too, so feeding it the Android
    // path may be the reason the engine's Android-side focused-text-box
    // object is never constructed.
    static const bool no_text_sync = std::getenv("STUD_NO_TEXT_SYNC") != nullptr;
    if (no_text_sync) {
        std::printf("stud: text path: skipped (STUD_NO_TEXT_SYNC), %zu chars pending\n",
                    text.size());
        std::fflush(stdout);
        return;
    }
    const auto cursor = static_cast<jint>(text.size());
    bool sync_ok = false;
    if (fns.sync_text != nullptr) {
        jstring jsync = tenv.NewStringUTF(text.c_str());
        sync_ok = call_trapping_abort(fns.sync_text, env, nullptr, jsync, cursor);
        clear_pending_jni_exception(env, "syncTextboxTextAndCursorPosition2");
    }
    // Length and outcome only -- a TextBox can be a password field, so the
    // contents are never logged.
    std::printf("stud: text delivered: %zu chars, cursor=%d, box=%ld, sync=%s, done=%d\n",
                text.size(), static_cast<int>(cursor), text_box,
                fns.sync_text == nullptr ? "unavailable" : (sync_ok ? "ok" : "trapped"),
                static_cast<int>(done));
    std::fflush(stdout);
    if (fns.pass_text != nullptr) {
        // STUD_TEXT_DONE=1 is a documented, deliberately-off workaround, not a
        // fix. It makes every keystroke land -- and every keystroke also
        // COMMIT, because done=true is exactly "the user finished editing":
        // live-confirmed, the search box submits after each letter and the
        // engine re-issues showKeyboard with the committed text. Useful only
        // to prove text delivery works at all. See this file's header comment
        // for why the correct path is still blocked.
        static const bool force_done = std::getenv("STUD_TEXT_DONE") != nullptr;
        const bool done_flag = done || force_done;
        jstring jtext = tenv.NewStringUTF(text.c_str());
        call_trapping_abort(fns.pass_text, env, nullptr, static_cast<jlong>(text_box), jtext,
                            static_cast<jboolean>(done_flag ? JNI_TRUE : JNI_FALSE), cursor);
    }
}

jlong now_ms() {
    return static_cast<jlong>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

bool agdk_input_enabled() {
    // On by default. This is the only input path that carries a real
    // InputDevice source and tool type (SOURCE_MOUSE / TOOL_TYPE_MOUSE):
    // NativeInputInterface.nativePassMouse* delivers coordinates but says
    // nothing about what produced them, so an engine fed only that cannot
    // know a mouse exists at all -- which is exactly what the app's own
    // handler (the app's own input handler) branches on. It was switched off during the
    // black-screen hunt as a suspect and left that way; rendering has since
    // been root-caused to render-host's own window-size and surface handling
    // and is stable, so the gate is back to what a real device does.
    // STUD_AGDK_INPUT=0 turns it off.
    static const char* env = std::getenv("STUD_AGDK_INPUT");
    static const bool on = env == nullptr || std::string_view(env) != "0";
    return on;
}

void resolve_agdk(FakeJni::Env& env, jobject activity_ref) {
    if (g_agdk.resolved) return;
    g_agdk.resolved = true;
    // Window focus is lifecycle, not input, so it is resolved before the
    // AGDK *input* switch below -- which is off by default. Telling the
    // engine it lost focus is what makes it let go of a key whose release
    // happened somewhere this window could not hear, and that has to work
    // whether or not the AGDK input path is in use.
    //
    // Resolved once: focus changes are rare, but looking it up on each one
    // would repeat the not-found diagnostic on any build lacking it.
    {
        jclass focus_cls = env.GetObjectClass(activity_ref);
        if (focus_cls != nullptr) {
            g_agdk.on_window_focus_changed =
                env.GetMethodID(focus_cls, "onWindowFocusChangedNative", "(JZ)V");
        }
    }
    if (!agdk_input_enabled()) {
        std::printf("stud: input bridge: AGDK input path disabled (STUD_AGDK_INPUT=1 enables)\n");
        std::fflush(stdout);
        return;
    }
    jclass cls = env.GetObjectClass(activity_ref);
    if (cls == nullptr) return;
    // No onTouchEventNative here. A real mouse never reaches it: the
    // app's own handler returns from its mouse branch before the touch
    // path, so mouse input goes through nativePassMouse* alone -- and
    // the dispatcher that used to build a MotionEvent for it had no
    // callers left at all.
    g_agdk.on_key_down = env.GetMethodID(cls, "onKeyDownNative", "(JLandroid/view/KeyEvent;)Z");
    g_agdk.on_key_up = env.GetMethodID(cls, "onKeyUpNative", "(JLandroid/view/KeyEvent;)Z");
    // Real AGDK text input. This is a genuinely separate path from the
    // Android IME one Stud has been using: AGDK's own InputConnection
    // reports the edited text straight to native code as a
    // gametextinput.State, with no Java EditText and no IME involved --
    // which is exactly the shape Stud can actually satisfy, since it has
    // no DEX to run a real EditText in. libroblox carries the whole
    // GameTextInput surface (its own `gametextinput.State` /
    // `InputConnection` class names and the real
    // `(JLcom/google/androidgamesdk/gametextinput/InputConnection;)V`
    // signature are all in the binary), so this is a real path the engine
    // implements, not a speculative one.
    g_agdk.on_text_input = env.GetMethodID(
        cls, "onTextInputEventNative",
        "(JLcom/google/androidgamesdk/gametextinput/State;)V");
    std::printf("stud: input bridge: AGDK onKeyDown=%s onKeyUp=%s onTextInputEvent=%s\n",
                g_agdk.on_key_down ? "ok" : "MISSING", g_agdk.on_key_up ? "ok" : "MISSING",
                g_agdk.on_text_input ? "ok" : "MISSING");
    std::fflush(stdout);
}

// Delivers the focused text box's whole current contents through AGDK's
// own text-input callback -- the same thing a real InputConnection sends
// on every edit, with the caret at the end and no composing region (Stud
// has no IME, so there is never a composing region to report).
void send_agdk_text(FakeJni::Env& env, jobject activity_ref, const std::string& text) {
    if (g_agdk.on_text_input == nullptr) return;
    // No composing region: Stud has no IME, so nothing is ever mid-
    // composition, and -1/-1 is the honest answer. Reporting the whole
    // string as a composing region was tried live, on the theory that a
    // client renders composing text while an edit is in progress -- no
    // effect on the invisible-while-typing symptom, so it is not kept as
    // a guess.
    const auto len = static_cast<FakeJni::JInt>(text.size());
    auto state = std::make_shared<stud::jni_bridge::GameTextInputStateStub>(
        std::make_shared<FakeJni::JString>(text), len, len, static_cast<FakeJni::JInt>(-1),
        static_cast<FakeJni::JInt>(-1));
    jobject state_ref = env.createLocalReference(state);
    env.CallVoidMethod(activity_ref, g_agdk.on_text_input, g_agdk.handle, state_ref);
}

void send_agdk_key(FakeJni::Env& env, jobject activity_ref, bool down, jint scan, jint key_code,
                   jint unicode_char) {
    jmethodID m = down ? g_agdk.on_key_down : g_agdk.on_key_up;
    if (m == nullptr) return;
    auto ev = std::make_shared<stud::jni_bridge::KeyEventStub>();
    ev->action = down ? kKeyActionDown : kKeyActionUp;
    ev->scan_code = scan;
    ev->key_code = key_code;
    // A real physical keyboard reports the character the key produces;
    // without it nothing downstream can turn a key into text.
    ev->unicode_char = unicode_char;
    ev->meta_state = g_meta_state;
    ev->source = kSourceKeyboard;
    ev->event_time = now_ms();
    ev->down_time = ev->event_time;
    jobject ev_ref = env.createLocalReference(ev);
    env.CallBooleanMethod(activity_ref, m, g_agdk.handle, ev_ref);
}

// The engine's own input entry points take density-independent units, not
// pixels: the real handler passes `MotionEvent.getX() / DisplayMetrics.
// density` (the app's own input handler, whose density divisor is that density). AGDK's own
// MotionEvent path is the opposite -- getX() really is pixels -- so the
// two need different numbers from the same event.
//
// Stud passed pixels to both, so on a 1.25x display the engine placed its
// cursor 1.25 times too far right and down. That is the offset the user
// saw on entering the window.
// Smooth zoom.
//
// Roblox on desktop eases the camera toward a new zoom distance; the
// Android build steps straight to it, so a wheel notch is a jump. The
// difference is not the engine's camera code -- it is what the platform
// hands it. Android's AXIS_VSCROLL is a float, and a high-resolution
// wheel legitimately reports fractions of a notch, so a notch delivered
// as a short run of fractional deltas is ordinary input the engine
// already knows how to take, not a trick played on it.
//
// One notch becomes an exponential ease-out: each poll round emits a
// fraction of what is left. Notches scrolled in quick succession add to
// the same remainder rather than queueing, which is what makes a fast
// flick feel like one continuous movement. The total always sums to what
// the wheel actually reported, so the camera lands exactly where an
// abrupt step would have put it.
std::mutex& wheel_mutex() {
    static std::mutex m;
    return m;
}
float& wheel_remaining() {
    static float remaining = 0.0f;
    return remaining;
}

std::atomic<bool> g_smooth_zoom{true};

bool smooth_zoom_enabled() {
    // STUD_NO_SMOOTH_ZOOM=1 still forces it off, for comparing the two
    // without going through Settings.
    static const bool env_off = std::getenv("STUD_NO_SMOOTH_ZOOM") != nullptr;
    return !env_off && g_smooth_zoom.load(std::memory_order_relaxed);
}

// How much zoom one wheel detent is worth. The window layer already
// reports exact detents (wl_pointer.axis_value120/axis_discrete), so 1.0
// is one notch -- the same amount the desktop client sends -- and this
// exists to dial the step finer without a rebuild.
// Reads a live NativeTextBoxInfo back out of the engine. The fields are
// the real ones the engine itself populates; going through plain JNI
// rather than casting the object keeps this honest about what it is --
// an ordinary read of a Java object Stud registered.
bool read_text_box_info(JNIEnv* env, jobject info,
                        NativeGLJavaInterfaceStub::TextBoxStyle& out) {
    jclass cls = env->GetObjectClass(info);
    if (cls == nullptr) return false;
    auto get_float = [&](const char* name, float& dst) {
        jfieldID id = env->GetFieldID(cls, name, "F");
        if (id != nullptr) dst = env->GetFloatField(info, id);
    };
    auto get_int = [&](const char* name, int& dst) {
        jfieldID id = env->GetFieldID(cls, name, "I");
        if (id != nullptr) dst = env->GetIntField(info, id);
    };
    get_float("x", out.x);
    get_float("y", out.y);
    get_float("width", out.width);
    get_float("height", out.height);
    get_float("fontSize", out.font_size);
    get_int("font", out.font);
    int color = 0;
    get_int("textColor", color);
    out.color = static_cast<unsigned>(color);
    get_int("xAlignment", out.x_alignment);
    get_int("yAlignment", out.y_alignment);
    int input_type = 0;
    get_int("textInputType", input_type);
    out.password = input_type == 5 || input_type == 9;
    if (text_input_trace_enabled()) {
        // What the engine says about the box, which is all Stud has to go
        // on: it reports a TextSize but not whether the box scales its
        // text to fit (Roblox's TextScaled), and those two cases need
        // different ems. Never the contents -- a TextBox can be a
        // password field.
        std::printf("stud: text box: font=%d fontSize=%.2f box=%.1fx%.1f at (%.1f,%.1f) "
                    "align=%d/%d\n",
                    out.font, static_cast<double>(out.font_size),
                    static_cast<double>(out.width), static_cast<double>(out.height),
                    static_cast<double>(out.x), static_cast<double>(out.y), out.x_alignment,
                    out.y_alignment);
        std::fflush(stdout);
    }
    // A box the engine reports with no area is one of the ghost focuses
    // showKeyboard also produces; refuse it rather than blanking a real
    // box's geometry.
    return out.width > 0.0f && out.height > 0.0f;
}

// The editing model for the box Stud draws. A focused TextBox's caret,
// selection and clipboard all belong to the platform's text widget, which
// on a device is an Android EditText and here is Stud.
stud::jni_bridge::TextEditor& editor() {
    static stud::jni_bridge::TextEditor e;
    return e;
}

void set_clipboard(const std::string& text) {
    if (text.empty()) return;
    uint64_t a[8] = {};
    stud::render_client::connection().call(CallId::SetClipboardText, a, text.data(),
                                           static_cast<uint32_t>(text.size()), nullptr, 0, nullptr);
}

std::string get_clipboard() {
    uint64_t a[8] = {};
    std::vector<char> buf(64 * 1024);
    uint32_t out_len = 0;
    stud::render_client::connection().call(CallId::GetClipboardText, a, nullptr, 0, buf.data(),
                                           static_cast<uint32_t>(buf.size()), &out_len);
    if (out_len > buf.size()) out_len = static_cast<uint32_t>(buf.size());
    return std::string(buf.data(), out_len);
}

// Text selection with the mouse, for the box Stud draws. A drag inside a
// focused TextBox belongs to the text widget, not to the camera -- the
// same division a device makes between an EditText and the GL view under
// it.
std::atomic<bool> g_text_drag{false};

// Is this point inside the focused box, in the engine's own
// density-independent units (what every pointer coordinate here is in)?
bool point_in_focused_box(float x, float y) {
    if (NativeGLJavaInterfaceStub::active_text_box() == 0) return false;
    const auto style = NativeGLJavaInterfaceStub::active_text_box_style();
    if (style.width <= 0.0f || style.height <= 0.0f) return false;
    return x >= style.x && x <= style.x + style.width && y >= style.y &&
           y <= style.y + style.height;
}

// Byte offset in the focused box's text under this x. The layout lives
// where the font is, so the process that draws it answers.
int text_offset_at(float density_independent_x) {
    const float density = stud::jni_bridge::engine_layout_density();
    uint64_t a[8] = {};
    a[0] = static_cast<uint64_t>(static_cast<uint32_t>(
        static_cast<int32_t>(std::lround(density_independent_x * density))));
    return static_cast<int>(stud::render_client::connection().call(
        CallId::TextOverlayOffsetAtX, a, nullptr, 0, nullptr, 0, nullptr));
}

// Hands the focused TextBox's text, caret and styling to the process
// that owns the window, which draws it over the box. The engine stops
// drawing a focused TextBox's own text (see the text-input entry in
// the engineering notes), so without this the letters land but never appear.
void push_text_overlay(bool visible, const std::string& text, int caret, int sel_begin,
                       int sel_end) {
    static const bool disabled = std::getenv("STUD_NO_TEXT_OVERLAY") != nullptr;
    if (disabled) return;
    auto style = NativeGLJavaInterfaceStub::active_text_box_style();
    // The engine works in density-independent units -- the same space
    // nativePassMouseMove takes, which is why to_density_independent()
    // exists for the opposite direction -- while the overlay is drawn in
    // real buffer pixels. Everything geometric therefore scales by the
    // density the engine was told about, the box and the font size alike.
    //
    // That is DisplayMetrics' density, not the display's. They are equal
    // only while the buffer is scaled by the display's own scale; with
    // HiDPI off the buffer is the window's logical size and this was
    // multiplying by the monitor's 1.25 anyway, which put the overlay a
    // quarter too large and a quarter too far down and right.
    const float density = stud::jni_bridge::engine_layout_density();
    if (density > 0.0f) {
        style.x *= density;
        style.y *= density;
        style.width *= density;
        style.height *= density;
        style.font_size *= density;
    }
    uint64_t a[8] = {};
    a[0] = (visible ? 1u : 0u) | (style.password ? 2u : 0u);
    // The rectangle travels as floats: it is genuinely fractional once
    // the density is applied, and rounding it here costs half a pixel of
    // vertical alignment against the engine's own text.
    auto pack_floats = [](float lo, float hi) {
        uint32_t a_bits = 0, b_bits = 0;
        std::memcpy(&a_bits, &lo, sizeof(a_bits));
        std::memcpy(&b_bits, &hi, sizeof(b_bits));
        return static_cast<uint64_t>(a_bits) | (static_cast<uint64_t>(b_bits) << 32);
    };
    a[1] = pack_floats(style.x, style.y);
    a[2] = pack_floats(style.width, style.height);
    uint32_t font_bits = 0;
    std::memcpy(&font_bits, &style.font_size, sizeof(font_bits));
    a[3] = static_cast<uint64_t>(font_bits) |
           (static_cast<uint64_t>(static_cast<uint32_t>(style.font)) << 32);
    a[4] = static_cast<uint64_t>(style.color) |
           (static_cast<uint64_t>(static_cast<uint32_t>(caret)) << 32);
    a[6] = static_cast<uint64_t>(static_cast<uint32_t>(sel_begin)) |
           (static_cast<uint64_t>(static_cast<uint32_t>(sel_end)) << 32);
    a[5] = static_cast<uint64_t>(static_cast<uint32_t>(style.x_alignment)) |
           (static_cast<uint64_t>(static_cast<uint32_t>(style.y_alignment)) << 32);
    stud::render_client::connection().call(CallId::SetTextOverlay, a, text.data(),
                                           static_cast<uint32_t>(text.size()), nullptr, 0, nullptr);
}

float wheel_scale() {
    static const float scale = [] {
        const char* raw = std::getenv("STUD_WHEEL_SCALE");
        if (raw == nullptr) return 1.0f;
        float v = std::strtof(raw, nullptr);
        return v > 0.0f ? v : 1.0f;
    }();
    return scale;
}

bool input_trace_enabled() {
    static const bool on = std::getenv("STUD_INPUT_TRACE") != nullptr;
    return on;
}

float to_density_independent(float pixels) {
    // DisplayMetrics' own density, which is exactly what a real device's
    // view layer divides by (the app's own input handler, whose density divisor is that density).
    // NOT the display's measured density: those are equal only while the
    // buffer is scaled by the display's own scale. With HiDPI off the
    // buffer IS the window's logical size, so dividing by the monitor's
    // 1.25 put every pointer coordinate at 80% of where the pointer
    // really was -- the engine's own cursor visibly lagging behind the
    // system one on the way into the window.
    const float density = stud::jni_bridge::engine_layout_density();
    return density > 0.0f ? pixels / density : pixels;
}

// Told when a pad appears or goes away, so the haptics bridge can
// answer the engine truthfully. Set by start_input_bridge(), which is
// the only place that has the Jvm and the library to hand.
std::function<void(int device_id, bool can_rumble)>& gamepad_presence_hook() {
    static std::function<void(int, bool)> hook;
    return hook;
}

void dispatch_event(stud::android_glue::HostInputEvent ev, const InputFns& fns, JNIEnv* jni_env,
                    float& last_x, float& last_y) {
    using Ev = stud::android_glue::HostInputEvent;
    switch (ev.type) {
        case Ev::kPointerMotion: {
            if (g_touch_down) send_touch(fns, jni_env, ev, 1);
            if (fns.mouse_move == nullptr) return;
            // Real caller (the app's own input handler) passes absolute position plus the
            // delta since the previous move, both already divided by the
            // display density -- so `last_x`/`last_y` are kept in those
            // same units, not pixels.
            const float x = to_density_independent(ev.x);
            const float y = to_density_independent(ev.y);
            // First move after a lock: take the position, report no
            // movement. The pointer did not travel from the accumulated
            // position to here -- that difference is an artefact of the
            // lock, and reporting it as a delta moves the camera.
            // The position reported to the engine is INTEGRATED from the
            // pointer's movement; it is never assigned from the pointer's
            // absolute position.
            //
            // That one distinction is what removes the teleport, in both
            // directions. While the pointer is locked it cannot move, so
            // the reported position advances by relative deltas instead --
            // and the moment the lock ends, assigning the physical
            // position back would snap the engine's cursor across the
            // whole distance the hand travelled. Integrating means there
            // is nothing to snap: the reported position simply carries on
            // from where it was, moved by however far the mouse moves.
            //
            // An earlier attempt kept an offset between the two and added
            // it to each absolute position. That compounds -- the offset
            // was computed from a position that already contained the
            // previous one -- so every drag displaced the cursor further,
            // and a third-person camera ended up somewhere random. There
            // is no offset here to compound.
            // The real client's own unlocked mouse path, followed
            // exactly (the app's own input handler's y(), the branch taken whenever
            // getButtonState() != 0, and the ordinary hover path beside
            // it):
            //
            //     float x = event.getX() / density;
            //     float y = event.getY() / density;
            //     float dx = x - lastX, dy = y - lastY;
            //     lastX = x; lastY = y;
            //     nativePassMouseMove(x, y, dx, dy);
            //
            // The position it passes is the POINTER'S OWN, every time.
            // Stud used to integrate the deltas into a position of its
            // own instead, and clamp that to the window. Both were
            // mistakes and both were visible: an integrated position
            // drifts away from the pointer, and once it has drifted past
            // an edge the clamp holds it there -- a cursor stuck against
            // the left of the screen, which the real client has no notion
            // of. It also cannot drift back, because nothing ever
            // reconciles the two.
            //
            // Integration is only needed while the pointer is locked and
            // has no position of its own; that is the branch above, and
            // the resync flag is what carries the two back into agreement
            // when the lock ends.
            const bool resync = g_resync_after_unlock.exchange(false);
            if (resync || !g_have_prev_raw) {
                g_prev_raw_x = x;
                g_prev_raw_y = y;
                g_have_prev_raw = true;
            }
            const float dx = x - g_prev_raw_x;
            const float dy = y - g_prev_raw_y;
            g_prev_raw_x = x;
            g_prev_raw_y = y;
            if (input_trace_enabled()) {
                // Motions during a drag, and the first few after it ends --
                // the window where a jump would happen.
                static int after_release = 0;
                if (g_button_state != 0) {
                    after_release = 4;
                } else if (after_release > 0) {
                    --after_release;
                }
                if (g_button_state != 0 || after_release > 0) {
                    std::printf("stud: move pos=(%.1f,%.1f) d=(%.1f,%.1f) raw=(%.1f,%.1f) "
                                "state=0x%x locked=%d%s\n",
                                last_x, last_y, dx, dy, ev.x, ev.y,
                                static_cast<unsigned>(g_button_state),
                                g_drag_locked.load() ? 1 : 0, resync ? " RESYNC" : "");
                    std::fflush(stdout);
                }
            }
            if (g_text_drag.load() && NativeGLJavaInterfaceStub::active_text_box() != 0) {
                auto& ed = editor();
                ed.set_caret(text_offset_at(last_x), /*select=*/true);
                push_text_overlay(true, ed.text(), ed.caret(), ed.selection_begin(),
                                  ed.selection_end());
            }
            // The real client's mouse path, in full (the app's own input handler's y(),
            // reached from onTouch() at line 864 for any event whose
            // source is SOURCE_MOUSE and whose tool type is not FINGER):
            //
            //     x = event.getX() / density;  y = event.getY() / density;
            //     dx = x - lastX;  dy = y - lastY;
            //     lastX = x; lastY = y;
            //     nativePassMouseMove(x, y, dx, dy);
            //
            // That is the whole of it. The position passed is the
            // pointer's own, every time, and the camera rotates from the
            // deltas beside it.
            //
            // NO PAN. onTouch() returns y(motionEvent) for a real mouse
            // before it ever reaches the GestureDetector, so
            // nativePassMousePan is never called for one -- the pan at
            // the app's own input handler is guarded by SOURCE_MOUSE but is only
            // reachable for an event whose tool type IS a finger, which a
            // mouse's never is. Stud sent it anyway, and a pan carries an
            // anchor: it tells the engine a gesture is under way about a
            // fixed point, so the engine pins its cursor there for the
            // duration and takes the position back at the end. That is
            // the cursor jumping to where the hand stopped, and
            // everything built to hide it -- snapping the position back
            // to the anchor on release, locking the real pointer for a
            // drag, clamping the position inside the window -- was
            // working around a gesture the platform should never have
            // reported.
            //
            // No clamp either. The client bounds nothing here; only its
            // ACTION_SCROLL branch floors the position it reports at
            // zero, which is the wheel's business and not the pointer's.
            if (g_warp_pending) {
                // Nothing here is the hand: either an event that was
                // already on its way when the warp was asked for, or the
                // warp itself arriving. Keep the engine's cursor where it
                // is, report no movement, and measure the next real
                // movement from wherever the pointer actually is now.
                const float to_target = std::fabs(ev.x - g_warp_target_px) +
                                        std::fabs(ev.y - g_warp_target_py);
                const float to_origin = std::fabs(ev.x - g_warp_from_px) +
                                        std::fabs(ev.y - g_warp_from_py);
                const bool landed = to_target <= to_origin;
                const bool timed_out =
                    std::chrono::steady_clock::now() - g_warp_started >
                    std::chrono::milliseconds(80);
                g_prev_raw_x = x;
                g_prev_raw_y = y;
                g_have_prev_raw = true;
                g_last_raw_px = ev.x;
                g_last_raw_py = ev.y;
                if (landed || timed_out) {
                    g_warp_pending = false;
                    if (input_trace_enabled()) {
                        std::printf("stud: warp %s at (%.1f,%.1f)\n",
                                    landed ? "landed" : "gave up (not seen)",
                                    static_cast<double>(ev.x), static_cast<double>(ev.y));
                        std::fflush(stdout);
                    }
                }
                return;
            }
            g_last_raw_px = ev.x;
            g_last_raw_py = ev.y;
            if (g_drag_confined) {
                // Per EVENT, not per poll. The camera sets its pin a frame
                // or so after the press, and an 8ms poll can miss the whole
                // of a short flick -- which is exactly when the cursor was
                // still jumping: the gesture ended before Stud ever saw the
                // pin, so it treated a rotation as a UI drag.
                g_engine_mouse_behavior.store(static_cast<int>(stud::runtime::read_mouse_behavior()));
            }
            if (g_drag_confined && engine_pins_cursor()) {
                // The engine is pinning its own cursor RIGHT NOW -- it
                // says so (MouseBehavior == LockCurrentPosition), which
                // is the one thing this used to have to guess at.
                //
                // While that lasts the engine ignores every position it
                // is given, so the position is left exactly where its
                // cursor is and only the movement is sent -- from the
                // relative stream, which keeps reporting what the device
                // did even once the pointer has reached the confinement
                // boundary and stopped. That is what makes a spin
                // unlimited, and what makes the end of one a non-event:
                // nothing was moved, so nothing has to be put back.
                if (!g_pin_seen) {
                    g_pin_seen = true;
                    // The press point, not wherever the pointer had got to
                    // by the time this was noticed. The camera pins on the
                    // button-down it receives, so that is where the
                    // engine's own cursor is standing -- adopting the
                    // current position instead puts the cursor (and the
                    // warp at the end) a flick's worth away from it.
                    g_pin_x = g_drag_anchor_x;
                    g_pin_y = g_drag_anchor_y;
                    g_pin_px = g_drag_anchor_px;
                    g_pin_py = g_drag_anchor_py;
                    if (input_trace_enabled()) {
                        std::printf("stud: the engine pinned its cursor at (%.1f,%.1f)\n",
                                    static_cast<double>(g_pin_x), static_cast<double>(g_pin_y));
                        std::fflush(stdout);
                    }
                }
                last_x = g_pin_x;
                last_y = g_pin_y;
                return;
            }
            last_x = x;
            last_y = y;
            if (g_drag_confined) {
                // Confined but NOT pinned: an ordinary drag, including a
                // right-drag on an in-game UI, where the engine really is
                // following the positions it is given. The cursor follows
                // the hand; only the movement is withheld, because the
                // relative event for this same motion carries it and
                // sending it twice would turn the camera twice as far.
                return;
            }
            call_trapping_abort(fns.mouse_move, jni_env, nullptr, last_x, last_y, dx, dy);
            return;
        }
        case Ev::kPointerRelative: {
            // Mouse look. `ev.x`/`ev.y` are the raw delta, not a position:
            // the compositor is holding the cursor still, so this is the
            // only motion there is. The real captured-pointer path
            // (the app's own input handler's z()) reads exactly this -- Android's
            // AXIS_RELATIVE_X/Y -- and adds it to the position it tracks
            // itself before passing both on, which is what this mirrors.
            // (It only skips the accumulation under FFlag
            // AndroidMouseLockButtonFix, whose compiled-in default is
            // false.) Deliberately not routed through AGDK's
            // onTouchEventNative: a captured pointer does not go through
            // the view's ordinary touch path on a real device either.
            if (fns.mouse_move == nullptr) return;
            // The captured-pointer path, as the client itself handles it
            // (reached from onCapturedPointer while the view holds
            // pointer capture): it divides the relative axis values by
            // the display density, adds them to the position it is
            // tracking, and passes that position alongside the deltas.
            //
            // So the position is INTEGRATED by the deltas rather than
            // frozen: it is the client's idea of where the cursor now is,
            // and capture does not suspend that -- it only changes where
            // the movement comes from.
            //
            // Stud used to freeze the position here and pass the stale one
            // with each delta. That is why a camera rotation ended
            // somewhere else: the engine spent the whole gesture being
            // told the cursor had not moved, and the first ordinary move
            // afterwards -- which passes the pointer's real position --
            // moved it the entire distance at once. The jump was never at
            // the release; it was on the next motion after it, which is
            // exactly how it presents.
            // The position does NOT advance while the pointer is
            // captured; only the deltas do.
            //
            // The client has both behaviours and a flag picks between
            // them: it integrates the deltas into its position only when
            // that flag is off, and passes the position either way.
            //
            // The flag is FFlagAndroidMouseLockButtonFix, and
            // production serves it True -- so the branch is NOT taken and
            // the client passes its position unchanged with each delta.
            // Integrating instead is what left the orbit teleporting: the
            // engine holds its cursor still for the gesture, so the
            // position Stud had accumulated meant nothing to it, and the
            // first ordinary move after the button came up handed it that
            // accumulated position all at once.
            //
            // Frozen, it comes out right by construction: the pointer is
            // locked and therefore physically still at the anchor, the
            // position the engine was given never left the anchor either,
            // and the first absolute position after the unlock IS the
            // anchor. Nothing moves that should not.
            if (input_trace_enabled() && (g_button_state != 0 || g_drag_locked.load())) {
                // The camera's whole input while the engine has its cursor
                // pinned. A rotation that jumps means one of these deltas
                // is wrong, and nothing has ever looked at them.
                static float run_x = 0.0f;
                static float run_y = 0.0f;
                run_x += ev.x;
                run_y += ev.y;
                std::printf("stud: rel d=(%.1f,%.1f) run=(%.1f,%.1f) pos=(%.1f,%.1f)%s\n",
                            static_cast<double>(ev.x), static_cast<double>(ev.y),
                            static_cast<double>(run_x), static_cast<double>(run_y),
                            last_x, last_y,
                            (std::fabs(ev.x) > 40.0f || std::fabs(ev.y) > 40.0f) ? "  <-- JUMP"
                                                                                 : "");
                std::fflush(stdout);
            }
            call_trapping_abort(fns.mouse_move, jni_env, nullptr, last_x, last_y, ev.x, ev.y);
            return;
        }
        case Ev::kPointerButton: {
            // While the pointer is locked the compositor is not moving it,
            // so a button event still carries the position from before the
            // lock. Adopting that would throw away everything accumulated
            // during the drag -- including on the release that ends it,
            // which is the moment the accumulated position is needed.
            if (!g_drag_locked.load()) {
                // Move by however far the pointer has travelled since the
                // last event, for the same reason as the motion path: the
                // reported position is integrated, never assigned.
                const float bx = to_density_independent(ev.x);
                const float by = to_density_independent(ev.y);
                if (g_have_prev_raw) {
                    last_x += bx - g_prev_raw_x;
                    last_y += by - g_prev_raw_y;
                }
                g_prev_raw_x = bx;
                g_prev_raw_y = by;
                g_have_prev_raw = true;
            }

            {
                // Real MotionEvent button bits: PRIMARY=1, SECONDARY=2,
                // TERTIARY=4. `ev.code` is already the Android button index
                // the app's own handler works in (getActionButton() - 1).
                jint bit = ev.code == 0 ? 1 : (ev.code == 1 ? 2 : 4);
                bool down = ev.a != 0.0f;
                const jint before = g_button_state;
                if (down) {
                    g_button_state |= bit;
                    g_down_time = now_ms();
                } else {
                    g_button_state &= ~bit;
                }
                // The anchor is the point the gesture began at, and it
                // lasts until every button is up -- the same lifetime the
                // real client's gesture has.
                // A right-button drag inside an experience locks the
                // pointer, and nothing else.
                //
                // Read straight off Sober's own Wayland traffic, which is
                // the same engine behaving correctly on this machine.
                // Every in-experience right press is followed immediately
                // by zwp_pointer_constraints_v1.lock_pointer(..., nil,
                // PERSISTENT) and every release by
                // zwp_locked_pointer_v1.destroy(), one for one, five times
                // running. Left presses get none. Right drags earlier in
                // the same session, before the join, get none.
                //
                // Note what is NOT there: no set_cursor_position_hint
                // anywhere in the log. The compositor does not move a
                // locked pointer, so destroying the lock leaves it where
                // the drag began -- the cursor "stays where it was
                // locked" for free, and moving afterwards carries on from
                // there. Sending a hint moves the pointer a second time,
                // and putting the reported position back on the anchor
                // moves the engine's cursor a third: those two together
                // are the "teleports to origin then back" this used to
                // do. Neither is wanted; the lock alone is the whole
                // mechanism.
                // OPEN: this locks for EVERY right press inside an
                // experience, and a right-drag over an in-game UI should
                // not lock at all.
                //
                // Sober gets this right and the difference is engine
                // state, not input handling. Measured from its own Wayland
                // traffic, one session, one game: a right press that
                // orbits the camera is followed immediately by
                // lock_pointer + locked(), and a right press over a UI --
                // a real 2.5-second drag with 583 motion events -- takes
                // no lock at all. The lock arrives on the PRESS, before
                // any motion, so whatever decides it is already known at
                // press time.
                //
                // In the client the only thing that requests capture is
                // nativeGetMainWindowIsMouseLockedCenter() (the app's own input handler)
                // -- and in Stud that call is live (never trapped) and
                // reads 0 through an entire session, orbit included, in
                // 9,480 polls. It does go to 1 for first person. So the
                // engine simply is not entering that state for an orbit
                // here, where it must be doing so for Sober, and the cause
                // is something Stud tells the engine about itself rather
                // than anything on this path.
                if ((bit & kCameraButtons) != 0 &&
                    stud::jni_bridge::NativeHelperStub::experience_is_loaded()) {
                    // From the live button state, not from this one edge:
                    // releasing the right button while the middle is still
                    // held is still a drag.
                    const bool held = (g_button_state & kCameraButtons) != 0;
                    if (held && !g_drag_anchored) {
                        g_drag_anchored = true;
                        // Where the cursor is, which at a press is also
                        // where the pointer is -- the two only come apart
                        // during the gesture, and are put back together at
                        // the end of it.
                        g_drag_anchor_x = to_density_independent(ev.x);
                        g_drag_anchor_y = to_density_independent(ev.y);
                        g_drag_anchor_px = ev.x;
                        g_drag_anchor_py = ev.y;
                        g_pin_seen = false;
                        last_x = g_drag_anchor_x;
                        last_y = g_drag_anchor_y;
                        set_pointer_confined(true);
                    } else if (!held && g_drag_anchored) {
                        g_drag_anchored = false;
                        // One last read before the engine lets go. A
                        // gesture short enough to finish between two polls
                        // would otherwise look like it was never pinned at
                        // all -- the engine drops the pin on its own frame
                        // after this button-up, so right now it still says
                        // what it was doing.
                        if (!g_pin_seen) {
                            g_engine_mouse_behavior.store(
                                static_cast<int>(stud::runtime::read_mouse_behavior()));
                            if (engine_pins_cursor()) {
                                g_pin_seen = true;
                                g_pin_x = g_drag_anchor_x;
                                g_pin_y = g_drag_anchor_y;
                                g_pin_px = g_drag_anchor_px;
                                g_pin_py = g_drag_anchor_py;
                                if (input_trace_enabled()) {
                                    std::printf("stud: the engine was pinning after all -- "
                                                "caught at the release\n");
                                    std::fflush(stdout);
                                }
                            }
                        }
                        set_pointer_confined(false);
                        // The pointer goes back to the cursor, not the
                        // cursor to the pointer. The cursor did not move
                        // for the whole gesture, so returning the pointer
                        // to it leaves nothing to reconcile: the next
                        // ordinary motion arrives at the anchor and moves
                        // the cursor by the hand's own movement from
                        // there.
                        //
                        // The resync makes that first motion adopt
                        // whatever position it really carries with a zero
                        // delta, so a compositor that cannot warp loses
                        // the cursor's place rather than turning the
                        // difference into camera movement.
                        if (g_pin_seen) {
                            // The engine held its cursor still for part of
                            // this gesture, so the pointer is somewhere
                            // else by now. Put the pointer back ON the
                            // cursor rather than moving the cursor to the
                            // pointer -- that direction is the teleport.
                            begin_warp(g_pin_px, g_pin_py);
                            last_x = g_pin_x;
                            last_y = g_pin_y;
                        }
                        if (input_trace_enabled()) {
                            std::printf("stud: drag ended at (%.1f,%.1f) -- %s\n",
                                        static_cast<double>(last_x), static_cast<double>(last_y),
                                        g_pin_seen
                                            ? (pointer_warp_available()
                                                   ? "the engine had pinned its cursor, pointer "
                                                     "put back on it"
                                                   : "the engine had pinned its cursor, and this "
                                                     "compositor cannot warp")
                                            : "the cursor followed the hand, nothing to put back");
                            std::fflush(stdout);
                        }
                        g_pin_seen = false;
                    }
                    g_lock_from_drag = held && drag_lock_enabled();
                    apply_pointer_lock();
                }
                if (down && before == 0) {
                } else if (!down && g_button_state == 0) {
                    // A drag ends wherever the hand ends. Nothing is
                    // put back.
                    //
                    // This used to snap the reported position to the point
                    // the drag began, on the reasoning that the engine
                    // pins its own cursor for a camera rotation
                    // (MouseBehavior.LockCurrentPosition) and takes the
                    // position back from the platform on release. That
                    // moved only Stud's idea of the cursor: the desktop
                    // pointer stayed where the hand ended, so the two
                    // disagreed -- visible the moment the pointer left the
                    // window, and as a cursor teleporting back in a view
                    // whose camera does not rotate at all.
                    //
                    // Locking the real pointer for the drag to make them
                    // agree was tried and rejected: a drag is meant to
                    // move the cursor freely and leave it where it is
                    // released. So the reported position simply follows
                    // the pointer, the whole way through, and there is
                    // nothing for either cursor to jump to.
                    //
                    // Inside an experience the lock taken above ends here.
                    // The compositor does not move a locked pointer, so it
                    // is still where the drag began -- which is where the
                    // engine's pinned cursor is -- and the next motion
                    // event re-bases rather than applying a delta across
                    // the gap.
                }
                // A primary press inside the focused box places the caret
                // and starts a selection drag; the release ends it. The
                // press still reaches the engine, so the box keeps its own
                // focus behaviour.
                if (bit == 1) {
                    if (down && point_in_focused_box(last_x, last_y)) {
                        auto& ed = editor();
                        if (ed.text() != NativeGLJavaInterfaceStub::active_text_box_text()) {
                            ed.set_text(NativeGLJavaInterfaceStub::active_text_box_text());
                        }
                        ed.set_caret(text_offset_at(last_x), /*select=*/false);
                        g_text_drag.store(true);
                        push_text_overlay(true, ed.text(), ed.caret(), ed.selection_begin(),
                                          ed.selection_end());
                    } else if (!down) {
                        g_text_drag.store(false);
                    }
                }
                // No lock is taken for a held button: the cursor travels
                // with the hand, which is what the real client does and
                // what an in-game UI needs. The engine's own LockCenter
                // answer still locks, and it is polled above.
                // Deliberately NOT locking the pointer merely because a
                // button is held.
                //
                // The real device does not: the app's own input handler takes Android's
                // pointer capture only when
                // nativeGetMainWindowIsMouseLockedCenter() is true, and a
                // trace of that predicate shows it stays false through an
                // entire session including in-game camera rotation. So on
                // real hardware a drag-to-rotate leaves the pointer free
                // and the engine follows ordinary absolute positions.
                //
                // Locking on a button was tried at length and every
                // version traded one bug for another, because the engine
                // pins its own cursor for some drags and not others and
                // reports neither: holding the pointer still is right for
                // in-game rotation and wrong for the avatar editor, which
                // wants the cursor to travel with the hand. Faithful
                // absolute positions need no such distinction. The lock
                // remains for the one case the engine does report --
                // shift-lock and first person, below.
            }
            // Only the primary button maps onto a real touch pointer.
            if (ev.code == 0) {
                bool down = ev.a != 0.0f;
                if (down != g_touch_down) {
                    g_touch_down = down;
                    send_touch(fns, jni_env, ev, down ? 0 : 2);
                }
            }
            if (input_trace_enabled()) {
                std::printf("stud: button %s code=%u -> engine pos=(%.1f,%.1f) raw=(%.1f,%.1f) "
                            "state=0x%x locked=%d\n",
                            ev.a != 0.0f ? "DOWN" : "up  ", ev.code, last_x, last_y, ev.x, ev.y,
                            static_cast<unsigned>(g_button_state), g_drag_locked.load() ? 1 : 0);
                std::fflush(stdout);
            }
            if (fns.mouse_button == nullptr) return;
            // A RELEASE outside the view is reported at the view's edge,
            // and nothing else is touched.
            //
            // While a button is held the compositor keeps delivering
            // motion after the pointer has left the window -- that is what
            // an implicit grab is for -- so the release can arrive at a
            // coordinate the view does not contain. Live-captured: a
            // right-button release at x=-95.0 on a 1728-wide surface, with
            // the engine simply not acting on it and the button left held
            // for ever. A real Android view never receives such an event
            // at all, so the nearest point it CAN act on is the honest
            // translation.
            //
            // Only the release, and only when it is genuinely outside:
            // motion is left exactly as it was, because the position
            // stream during a drag is what the engine's own cursor
            // behaviour is built on and clamping that breaks it (tried,
            // and it put a wall under every UI and 2D-camera drag).
            float button_x = last_x;
            float button_y = last_y;
            if (ev.a == 0.0f && ev.surface_width > 0 && ev.surface_height > 0) {
                const float w = to_density_independent(static_cast<float>(ev.surface_width));
                const float h = to_density_independent(static_cast<float>(ev.surface_height));
                button_x = button_x < 0.0f ? 0.0f : (button_x > w - 1.0f ? w - 1.0f : button_x);
                button_y = button_y < 0.0f ? 0.0f : (button_y > h - 1.0f ? h - 1.0f : button_y);
                if (input_trace_enabled() && (button_x != last_x || button_y != last_y)) {
                    std::printf("stud: release was outside the view (%.1f,%.1f) -- reported at "
                                "(%.1f,%.1f)\n", static_cast<double>(last_x),
                                static_cast<double>(last_y), static_cast<double>(button_x),
                                static_cast<double>(button_y));
                    std::fflush(stdout);
                }
            }
            call_trapping_abort(fns.mouse_button, jni_env, nullptr, button_x, button_y,
                                static_cast<jboolean>(ev.a != 0.0f ? JNI_TRUE : JNI_FALSE),
                                static_cast<jint>(ev.code));
            return;
        }
        case Ev::kPointerAxis: {
            if (fns.mouse_wheel == nullptr) return;
            // The real caller clamps both coordinates at zero here.
            // Same units as every other nativePass* coordinate.
            const float x = last_x > 0.0f ? last_x : 0.0f;
            const float y = last_y > 0.0f ? last_y : 0.0f;
            const float notches = ev.a * wheel_scale();
            if (input_trace_enabled()) {
                std::printf("stud: wheel raw=%.4f scaled=%.4f\n", ev.a, notches);
                std::fflush(stdout);
            }
            if (!smooth_zoom_enabled()) {
                call_trapping_abort(fns.mouse_wheel, jni_env, nullptr, x, y, notches);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(wheel_mutex());
                // A notch in the opposite direction cancels what is left
                // rather than fighting it, so reversing mid-zoom responds
                // immediately.
                if ((wheel_remaining() > 0.0f) != (notches > 0.0f)) wheel_remaining() = 0.0f;
                wheel_remaining() += notches;
            }
            return;
        }
        case Ev::kKey: {
            bool down = ev.a != 0.0f;
            // What this process believes is held. android-glue keeps its
            // own set for focus changes, but a Lua TextBox taking focus is
            // something only this side ever hears about.
            if (down) {
                held_keys().insert(ev.code);
            } else {
                held_keys().erase(ev.code);
            }

            // A key already released into text entry stays released until
            // it is really let go. Without this the next auto-repeat --
            // 25 a second -- presses it straight back down, which is why
            // the character kept walking while its owner typed.
            {
                auto& released_into_text = keys_released_into_text_entry();
                const auto it = released_into_text.find(ev.code);
                if (it != released_into_text.end()) {
                    const bool is_a_repeat = ev.b != 0.0f;
                    if (down && is_a_repeat) return;  // the repeat of a key already let go
                    // A real release, or a deliberate fresh press: either
                    // ends the suppression, and both are forwarded. The
                    // release especially -- the one sent at focus time was
                    // discarded by the engine, so this is not a duplicate
                    // of anything that landed.
                    released_into_text.erase(it);
                }
            }
            // Track modifiers locally: Wayland reports them in a separate
            // event Stud does not forward, and every synthesized KeyEvent
            // has to carry them the way a real keyboard would.
            static bool shift_down = false;
            // A repeat is a press that never had a matching release, so it
            // must not disturb the latched modifier state.
            const bool is_repeat = ev.b != 0.0f;
            if (is_repeat) {
                // Modifiers themselves do not repeat usefully.
            } else if (ev.code == 42 || ev.code == 54) {  // LEFTSHIFT / RIGHTSHIFT
                shift_down = down;
                g_meta_state = down ? (g_meta_state | 0x1) : (g_meta_state & ~0x1);
            } else if (ev.code == 56 || ev.code == 100) {  // LEFTALT / RIGHTALT
                g_meta_state = down ? (g_meta_state | 0x02) : (g_meta_state & ~0x02);
            } else if (ev.code == 29 || ev.code == 97) {  // LEFTCTRL / RIGHTCTRL
                g_meta_state = down ? (g_meta_state | 0x1000) : (g_meta_state & ~0x1000);
            }
            // Escape gives the pointer back, whatever is holding it -- see
            // g_lock_suppressed. The key still reaches the engine as
            // normal; this only lets go of the lock alongside it.
            if (down && !is_repeat && ev.code == 1) {  // KEY_ESC
                if (g_drag_locked.load()) g_lock_suppressed = true;
                apply_pointer_lock();
            }
            // What this key really types, according to the compositor's own
            // keymap. Stud's own table is positional and describes a US
            // layout, so it is only the fallback now -- for the X11 backend,
            // and for the moment before the keymap has arrived.
            std::string typed_text;
            if (!text_from_keymap(ev.keysym, ev.codepoint, ev.composed_utf8,
                                  sizeof(ev.composed_utf8), &typed_text)) {
                // Only reached with no keymap at all -- the X11 backend, or
                // the moment before wl_keyboard.keymap arrives.
                char from_table = 0;
                if (char_for_scan_code(ev.code, shift_down, &from_table)) {
                    typed_text.assign(1, from_table);
                }
            }
            // KeyEvent.getUnicodeChar() is a code point, not a byte, so the
            // real one goes through whole rather than being truncated.
            // KeyEvent.getUnicodeChar() is a single code point, so a
            // multi-character composed result has none to report -- the
            // text still reaches the engine through typed_text -- and
            // neither does a dead key, which types nothing on its own.
            const jint unicode_char =
                ev.codepoint != 0
                    ? static_cast<jint>(ev.codepoint)
                    : (ev.composed_utf8[0] != '\0' || typed_text.empty()
                           ? 0
                           : static_cast<jint>(static_cast<unsigned char>(typed_text[0])));
            const jint key_code = android_key_code_for_event(ev.code, ev.keysym);
            // The scan code is what the engine actually keys off -- it
            // indexes a table straight to a USB HID usage code, and never
            // reads the Android key code at all. So it has to carry what
            // the key types rather than where it sits. See
            // engine_scan_code_for_event().
            const jint scan_code = static_cast<jint>(
                engine_scan_code_for_event(ev.code, typed_text));

            // Real text entry. When the engine has told us a Lua TextBox
            // is focused (NativeGLJavaInterface.showKeyboard), keystrokes
            // must go back as the TextBox's whole updated contents via
            // nativePassText -- a real device does exactly this from its
            // IME, never as key events.
            long text_box = NativeGLJavaInterfaceStub::active_text_box();

            // TEMPORARY, env-gated (STUD_DIAG_TEXTBOX=1), never armed by
            // default: read the engine's own focused-text-box fields on the
            // id-4 service singleton directly.
            //
            // RESULT, live: 0x9c8 is null while 0xa78/0xa80 both hold real
            // pointers. An earlier reading of this took the null 0x9c8 for
            // the reason typing does not echo -- that is RETRACTED. Its
            // consumer only takes 0x9c8 as a fast path and falls through to
            // a second path built on 0xa80, so
            // syncTextboxTextAndCursorPosition2 really does reach the
            // focused box. See this file's header for what actually
            // suppresses the drawing.
            //
            // Version-specific offsets, so this is an investigation tool
            // only and must never become load-bearing: see this project's
            // own non-negotiable constraints.
            if (std::getenv("STUD_DIAG_TEXTBOX") != nullptr && down && text_box != 0) {
                static int reported = 0;
                if (reported < 3) {
                    ++reported;
                    auto* anchor = reinterpret_cast<const unsigned char*>(fns.key_event);
                    if (anchor != nullptr) {
                        const unsigned char* base = anchor - 0x2e4fcff;
                        auto read_field = [base](unsigned long long packed) -> unsigned long long {
                            const unsigned long long obj_off = packed >> 16;
                            const unsigned long long field = packed & 0xffff;
                            return *reinterpret_cast<const unsigned long long*>(base + obj_off +
                                                                                 field);
                        };
                        // The three fields the engine itself reads off this
                        // one service object: 0x9c8 is what the live sync
                        // path bails on, 0xa78/0xa80 are what
                        // nativeGetTextBoxInfo reads for the same box.
                        for (unsigned long long obj : {0x72f2c80ULL, 0x72f3d50ULL}) {
                            unsigned long long v9c8 = 0, va78 = 0, va80 = 0;
                            const bool a = call_trapping_abort_with_result(read_field, v9c8,
                                                                          (obj << 16) | 0x9c8);
                            const bool b = call_trapping_abort_with_result(read_field, va78,
                                                                          (obj << 16) | 0xa78);
                            const bool c = call_trapping_abort_with_result(read_field, va80,
                                                                          (obj << 16) | 0xa80);
                            std::printf("stud: DIAG textbox obj=0x%llx 0x9c8=%s0x%llx "
                                        "0xa78=%s0x%llx 0xa80=%s0x%llx\n",
                                        obj, a ? "" : "(trapped) ", v9c8, b ? "" : "(trapped) ",
                                        va78, c ? "" : "(trapped) ", va80);
                        }
                        std::fflush(stdout);
                    }
                }
            }
            // Opt-in (STUD_IME_HANDSHAKE=1) retest of the IME handshake a
            // real device performs when its keyboard opens over the GL view.
            // This was tried twice before and disproven -- but both attempts
            // predate `java.lang.String.getBytes` existing at all, and
            // nativeGetTextBoxInfo builds a real NativeTextBoxInfo out of the
            // focused box's own text. Off by default; the DIAG probe above
            // reports whether it arms the pointer sync needs.
            if (down && text_box != 0 && std::getenv("STUD_IME_HANDSHAKE") != nullptr) {
                static bool handshaken = false;
                if (!handshaken) {
                    handshaken = true;
                    if (fns.get_text_box_info != nullptr) {
                        jobject info = nullptr;
                        call_trapping_abort_with_result(fns.get_text_box_info, info, jni_env,
                                                        nullptr);
                        std::printf("stud: IME handshake: nativeGetTextBoxInfo -> %s\n",
                                    info != nullptr ? "object" : "null");
                    }
                    if (fns.update_keyboard_size != nullptr) {
                        // A real open keyboard covers the bottom of the view.
                        // The real caller only reports visible=true when the
                        // measured height exceeds 10, so this must be a real
                        // rectangle, not zeroes.
                        // The event carries the live surface size, which is
                        // the same thing the window owner reports.
                        const jint w = static_cast<jint>(ev.surface_width);
                        const jint h = static_cast<jint>(ev.surface_height);
                        const jint kb_h = h / 3 > 10 ? h / 3 : 300;
                        call_trapping_abort(fns.update_keyboard_size, jni_env, nullptr,
                                            static_cast<jboolean>(JNI_TRUE), 0, h - kb_h, w, kb_h);
                        std::printf("stud: IME handshake: updateKeyboardSize(true, 0,%d,%d,%d)\n",
                                    h - kb_h, w, kb_h);
                    }
                    std::fflush(stdout);
                }
            }

            if (down && text_box != 0 && fns.pass_text != nullptr) {
                auto& ed = editor();
                if (ed.text() != NativeGLJavaInterfaceStub::active_text_box_text()) {
                    // The engine replaced the contents (a fresh focus, or
                    // Lua setting the text) -- adopt it.
                    ed.set_text(NativeGLJavaInterfaceStub::active_text_box_text());
                }
                const bool ctrl = (g_meta_state & 0x1000) != 0;
                bool changed = false;
                bool moved = false;
                if (ctrl && (ev.code == 30)) {  // Ctrl+A
                    ed.select_all();
                    moved = true;
                } else if (ctrl && (ev.code == 46 || ev.code == 45)) {  // Ctrl+C / Ctrl+X
                    set_clipboard(ed.selected_text());
                    if (ev.code == 45) changed = ed.delete_selection();  // cut
                    moved = true;
                } else if (ctrl && ev.code == 47) {  // Ctrl+V
                    std::string pasted = get_clipboard();
                    // A text box takes one line: a pasted newline ends the
                    // paste rather than smuggling a control character in.
                    const size_t newline = pasted.find_first_of("\r\n");
                    if (newline != std::string::npos) pasted.resize(newline);
                    changed = ed.insert(pasted);
                } else if (ev.code == 14) {  // BACKSPACE
                    changed = ed.backspace();
                } else if (ev.code == 111) {  // DELETE
                    changed = ed.del();
                } else if (ev.code == 105) {  // LEFT
                    ed.move_left(shift_down);
                    moved = true;
                } else if (ev.code == 106) {  // RIGHT
                    ed.move_right(shift_down);
                    moved = true;
                } else if (ev.code == 102) {  // HOME
                    ed.move_home(shift_down);
                    moved = true;
                } else if (ev.code == 107) {  // END
                    ed.move_end(shift_down);
                    moved = true;
                } else if (ev.code == 28) {  // ENTER
                    // Real order from RbxKeyboard.onEditorAction: sync the
                    // text first, then report the return key, then the final
                    // done=true nativePassText.
                    deliver_text(fns, ed.text(), text_box, /*done=*/true);
                    if (fns.return_pressed != nullptr) {
                        call_trapping_abort(fns.return_pressed, jni_env, nullptr,
                                            static_cast<jlong>(text_box));
                    }
                } else if (!typed_text.empty() && !ctrl) {
                    changed = ed.insert(typed_text);
                }
                const std::string text = ed.text();
                if (moved && !changed) {
                    // Caret and selection moved without the text changing:
                    // the engine has nothing new to receive, but what is
                    // drawn has to follow.
                    push_text_overlay(true, text, ed.caret(), ed.selection_begin(),
                                      ed.selection_end());
                }
                if (changed) {
                    if (std::getenv("STUD_INPUT_TRACE") != nullptr) {
                        // Length only -- never the content, which can be a password.
                        std::printf("stud: input bridge: nativePassText -> %zu chars\n",
                                    text.size());
                        std::fflush(stdout);
                    }
                    if (std::getenv("STUD_INPUT_TRACE") != nullptr) {
                        static bool told_handle = false;
                        if (!told_handle) {
                            told_handle = true;
                            std::printf("stud: input bridge: TextBox handle=0x%llx\n",
                                        static_cast<unsigned long long>(text_box));
                            std::fflush(stdout);
                        }
                    }
                    NativeGLJavaInterfaceStub::set_active_text_box_text(text);
                    deliver_text(fns, text, text_box, /*done=*/false);
                    push_text_overlay(true, text, ed.caret(), ed.selection_begin(),
                                      ed.selection_end());
                    // ...and through AGDK's own text-input callback, which
                    // needs no Java EditText and no IME. See resolve_agdk().
                    if (g_agdk_env != nullptr) {
                        send_agdk_text(*g_agdk_env, g_agdk_activity_ref, text);
                    }
                }
            }

            // Real Android Back. The app shell leaves a page (a WebView
            // page, a settings screen) on KEYCODE_BACK, which every real
            // device has as a button or a gesture and which Stud had no
            // way to send at all -- leaving a user stuck on any screen
            // whose only exit is Back, live-reported exactly that way.
            // Escape stays KEYCODE_ESCAPE, because that is what a real
            // hardware keyboard sends and what Roblox uses in-game for
            // its own menu; Alt+Left is the desktop convention for
            // "back" and is what is bound here instead.
            const bool alt_left_back = ev.code == 105 && (g_meta_state & kMetaAlt) != 0;
            if (alt_left_back) {
                if (g_agdk_env != nullptr) {
                    send_agdk_key(*g_agdk_env, g_agdk_activity_ref, down,
                                  static_cast<jint>(ev.code), kKeyCodeBack, unicode_char);
                }
                if (fns.key_event != nullptr) {
                    call_trapping_abort(fns.key_event, jni_env, nullptr,
                                        static_cast<jboolean>(down ? JNI_TRUE : JNI_FALSE),
                                        static_cast<jint>(ev.code), kKeyCodeBack,
                                        static_cast<jboolean>(JNI_FALSE));
                }
                if (down) {
                    std::printf("stud: input bridge: Alt+Left -> KEYCODE_BACK\n");
                    std::fflush(stdout);
                }
                return;
            }

            if (g_agdk_env != nullptr) {
                send_agdk_key(*g_agdk_env, g_agdk_activity_ref, down, scan_code, key_code,
                              unicode_char);
            }
            // One-shot per path, so a live run says exactly which of the
            // three real key paths actually reached the engine.
            {
                static bool told_text = false, told_key = false;
                if (!told_text && text_box != 0) {
                    told_text = true;
                    std::printf("stud: input bridge: text path ACTIVE (TextBox focused, "
                                "pass_text=%d)\n", fns.pass_text != nullptr);
                    std::fflush(stdout);
                }
                if (!told_key) {
                    told_key = true;
                    std::printf("stud: input bridge: nativePassKeyEvent path active "
                                "(scan=%u->%d keycode=%d unicode=%d)\n", ev.code,
                                scan_code, key_code, unicode_char);
                    std::fflush(stdout);
                }
            }
            // The first keys of a session, one line each, so a run can say
            // exactly what the engine was handed rather than leaving it to
            // be inferred. Bounded so it cannot fill a log, and off unless
            // asked for -- see STUD_INPUT_TRACE.
            {
                static const bool trace_keys = std::getenv("STUD_INPUT_TRACE") != nullptr;
                static int traced = 0;
                if (trace_keys && traced < 40) {
                    ++traced;
                    std::printf("stud: key: %s scan=%u->%d keycode=%d repeat=%d textbox=%d "
                                "held=%zu\n",
                                down ? "down" : "up  ", ev.code, scan_code, key_code,
                                ev.b != 0.0f ? 1 : 0, text_box != 0 ? 1 : 0, held_keys().size());
                    std::fflush(stdout);
                }
            }
            if (fns.key_event == nullptr) return;
            // A held key repeats, and says so: real Android reports the
            // same through KeyEvent.getRepeatCount(), which is exactly
            // what this argument stands for.
            call_trapping_abort(fns.key_event, jni_env, nullptr,
                                static_cast<jboolean>(ev.a != 0.0f ? JNI_TRUE : JNI_FALSE),
                                scan_code,
                                key_code,
                                static_cast<jboolean>(ev.b != 0.0f ? JNI_TRUE : JNI_FALSE));
            return;
        }
        case Ev::kPointerEnter:
        case Ev::kWindowFocus: {
            const bool focused = ev.a != 0.0f;
            // Stud has already sent a release for everything it knew was
            // held (android-glue does that before this event), but the
            // engine keeps its own input state and will not drop it just
            // because Stud did. Real Android delivers exactly this, and
            // it is what makes the engine let go.
            //
            // This is the opposite of listening in the background: no
            // input is read while unfocused -- the compositor stops
            // sending any -- this only says "stop", and says it once.
            if (!focused) {
                // Local latched state goes too, or a modifier held at the
                // moment focus left stays latched for the next keystroke
                // after coming back.
                g_meta_state = 0;
                g_button_state = 0;
                if (g_lock_from_drag) {
                    g_lock_from_drag = false;
                    apply_pointer_lock();
                }
            }
            // Releasing the held keys above is the fix on its own; telling
            // the engine is the belt-and-braces for input it believes is
            // held that Stud never knew about. If that ever misbehaves,
            // this turns it off without losing the fix.
            static const bool tell_engine = std::getenv("STUD_NO_FOCUS_EVENTS") == nullptr;
            if (tell_engine && g_agdk_env != nullptr && g_agdk_activity_ref != nullptr &&
                g_agdk.on_window_focus_changed != nullptr) {
                g_agdk_env->CallVoidMethod(
                    g_agdk_activity_ref, g_agdk.on_window_focus_changed, g_agdk.handle,
                    static_cast<jboolean>(focused ? JNI_TRUE : JNI_FALSE));
            }
            if (std::getenv("STUD_INPUT_TRACE") != nullptr) {
                std::printf("stud: input bridge: window focus %s\n", focused ? "gained" : "lost");
                std::fflush(stdout);
            }
            return;
        }
        case Ev::kPointerLeave: {
            // A drag lock cannot survive the pointer crossing the surface
            // boundary in either direction. Leaving means the release that
            // would have ended it is never coming; entering means it was
            // somehow outstanding while the pointer was free, which it
            // never should be. Either way, let go.
            if (g_lock_from_drag) {
                g_lock_from_drag = false;
                apply_pointer_lock();
            }
            // Real hover enter/exit. Without these the engine has no way to
            // know the pointer is inside the window at all -- and Roblox
            // only draws its own cursor while it believes it is.
            if (ev.type == Ev::kPointerEnter && fns.mouse_move != nullptr) {
                // Tell the engine where the pointer came in, straight
                // away. A hover-enter carries no position the engine acts
                // on, so without this its cursor stays wherever it was
                // when the pointer last left and only catches up on the
                // next movement -- which is why it took a moment to
                // appear, in the wrong place, instead of being there.
                const float x = to_density_independent(ev.x);
                const float y = to_density_independent(ev.y);
                // Take the entry point for the reported position AND for
                // the baseline movement is measured against.
                //
                // Only the first was being set. The reported position is
                // integrated from the pointer's movement, so the next
                // motion measured itself against wherever the pointer was
                // when it last left -- a delta across everything that
                // happened outside the window -- and the cursor jumped off
                // the moment it came back in. Re-basing here is what makes
                // entering seamless: the pointer really is at this point,
                // so this is the one place adopting it is correct.
                // Carrying whatever shift a camera drag left behind, so
                // Nothing to carry across: a camera drag leaves the
                // pointer on the cursor, so entering is just entering.
                last_x = x;
                last_y = y;
                g_prev_raw_x = x;
                g_prev_raw_y = y;
                g_have_prev_raw = true;
                call_trapping_abort(fns.mouse_move, jni_env, nullptr, last_x, last_y, 0.0f, 0.0f);
            }
            if (ev.type == Ev::kPointerLeave && g_button_state != 0) {
                // Release whatever is still held.
                //
                // Wayland stops sending button events the moment the
                // pointer leaves the surface, so a button released outside
                // -- after dragging out of the window, or when another
                // surface takes the pointer, or on alt-tab -- is a release
                // Stud never hears about. The engine is then left holding
                // a button forever: a left-drag that never ends (the climb
                // fling that will not let go), and a right button that
                // stays "down" so mouse look never releases the cursor.
                //
                // A real device cannot reach this state, because its
                // window system does not take the pointer away mid-gesture
                // without ending the gesture. Ending it here is what makes
                // Stud behave the same.
                for (jint bit : {1, 2, 4}) {
                    if ((g_button_state & bit) == 0) continue;
                    const jint code = bit == 1 ? 0 : (bit == 2 ? 1 : 3);
                    g_button_state &= ~bit;
                    if (fns.mouse_button != nullptr) {
                        call_trapping_abort(fns.mouse_button, jni_env, nullptr, last_x, last_y,
                                            static_cast<jboolean>(JNI_FALSE), code);
                    }
                }
                if (g_touch_down) {
                    g_touch_down = false;
                    send_touch(fns, jni_env, ev, 2);
                }
                g_text_drag.store(false);
                // No button is held any more, so nothing justifies keeping
                // the pointer locked -- or fenced in.
                g_lock_from_drag = false;
                apply_pointer_lock();
                g_drag_anchored = false;
                g_warp_pending = false;
                set_pointer_confined(false);
                if (input_trace_enabled()) {
                    std::printf("stud: pointer left the surface -- released every held button\n");
                    std::fflush(stdout);
                }
            }
            // Explicit, and load-bearing: this case used to fall through
            // to `default: return`, which was harmless only while nothing
            // sat between the two. The gamepad cases below now do, and
            // without this every pointer enter and leave was handled as a
            // controller being plugged in.
            return;
        }
        case Ev::kPointerPinch: {
            if (fns.mouse_pinch == nullptr) return;
            // The real caller scales the change in pinch factor by 3.5
            // before handing it over (the app's own mouse branch), and
            // passes the position already divided by the display
            // density -- same units every other pointer call uses here.
            call_trapping_abort(fns.mouse_pinch, jni_env, nullptr, to_density_independent(ev.x),
                                to_density_independent(ev.y), ev.a * 3.5f);
            return;
        }
        // Game controllers.
        //
        // render-host has already done the hard part: these arrive as
        // Android keycodes and axis ids, so there is no mapping left to
        // get wrong here. `b` carries the device id, which the engine
        // uses to keep several pads apart.
        case Ev::kGamepadConnect: {
            if (fns.gamepad_connect == nullptr) return;
            const jint device_id = static_cast<jint>(ev.b);
            const jint type = static_cast<jint>(ev.a);
            call_trapping_abort(fns.gamepad_connect, jni_env, nullptr, device_id, type);
            std::printf("stud: gamepad %d connected to the engine (type %d)\n",
                        static_cast<int>(device_id), static_cast<int>(type));
            std::fflush(stdout);
            // v1 of the connect event says whether this pad has force
            // feedback -- see render-host/src/gamepad.cpp.
            if (gamepad_presence_hook()) {
                gamepad_presence_hook()(static_cast<int>(device_id), ev.y != 0.0f);
            }
            return;
        }
        // What the pad can do, announced BEFORE it is said to have
        // arrived -- the real app does the same (`E(deviceId, type)` runs
        // immediately before the connect call). A pad that reports no
        // keys is a pad with no bindings, which looks exactly like a
        // controller that does nothing.
        //
        // These arrive before the connect event, so they carry the
        // gamepad type themselves rather than waiting for it.
        case Ev::kGamepadSupportedKey: {
            if (fns.gamepad_supported_key == nullptr) return;
            if (input_trace_enabled()) {
                std::printf("stud: pad %d supports key %u = %d\n", static_cast<int>(ev.b),
                            ev.code, ev.a != 0.0f ? 1 : 0);
            }
            call_trapping_abort(fns.gamepad_supported_key, jni_env, nullptr,
                                static_cast<jint>(ev.b), static_cast<jint>(ev.code),
                                static_cast<jboolean>(ev.a != 0.0f ? JNI_TRUE : JNI_FALSE),
                                static_cast<jint>(ev.y));
            return;
        }
        case Ev::kGamepadSupportedAxis: {
            if (fns.gamepad_supported_motion == nullptr) return;
            const jint device_id = static_cast<jint>(ev.b);
            const jint axis = static_cast<jint>(ev.code);
            const jboolean present =
                static_cast<jboolean>(ev.a != 0.0f ? JNI_TRUE : JNI_FALSE);
            const jint type = static_cast<jint>(ev.y);
            // Direction -1 for every axis, and additionally +1 for the
            // two hat axes -- exactly what the real caller registers
            // (jadx, `kl/e.java`'s own `E()`), not both directions for
            // everything.
            if (input_trace_enabled()) {
                std::printf("stud: pad %d supports axis %d = %d\n", static_cast<int>(device_id),
                            static_cast<int>(axis), present != 0 ? 1 : 0);
            }
            call_trapping_abort(fns.gamepad_supported_motion, jni_env, nullptr, device_id, axis,
                                -1, present, type);
            if (axis == 15 || axis == 16) {
                call_trapping_abort(fns.gamepad_supported_motion, jni_env, nullptr, device_id,
                                    axis, 1, present, type);
            }
            return;
        }
        case Ev::kGamepadDisconnect: {
            if (gamepad_presence_hook()) {
                gamepad_presence_hook()(static_cast<int>(ev.b), false);
            }
            if (fns.gamepad_disconnect == nullptr) return;
            call_trapping_abort(fns.gamepad_disconnect, jni_env, nullptr,
                                static_cast<jint>(ev.b));
            return;
        }
        case Ev::kGamepadButton: {
            if (fns.gamepad_button == nullptr) return;
            // 1 is pressed, 0 is released -- NOT Android's ACTION_DOWN/UP
            // constants, which are the other way round. The real caller
            // converts explicitly (jadx, the app's own key listener:
            // `i11 = keyEvent.getAction() == 0 ? 1 : 0`), so sending the
            // raw action inverts every edge: a press arrives as a release
            // and the release that follows arrives as a press, leaving the
            // button latched down forever. Live symptom: one tap and the
            // character jumps continuously.
            if (input_trace_enabled()) {
                std::printf("stud: pad %d key %u %s\n", static_cast<int>(ev.b), ev.code,
                            ev.a != 0.0f ? "down" : "up");
                std::fflush(stdout);
            }
            call_trapping_abort(fns.gamepad_button, jni_env, nullptr,
                                static_cast<jint>(ev.b), static_cast<jint>(ev.code),
                                ev.a != 0.0f ? 1 : 0);
            return;
        }
        case Ev::kGamepadAxis: {
            if (fns.gamepad_axis == nullptr) return;
            // The entry point takes a VECTOR, not a scalar: render-host
            // has already built the exact triple the real caller sends
            // (a stick's two components on both of its axis ids, a
            // trigger or hat value in the third float).
            if (input_trace_enabled()) {
                std::printf("stud: pad %d axis %u = (%.3f, %.3f, %.3f)\n",
                            static_cast<int>(ev.b), ev.code, ev.x, ev.y, ev.a);
                std::fflush(stdout);
            }
            call_trapping_abort(fns.gamepad_axis, jni_env, nullptr,
                                static_cast<jint>(ev.b), static_cast<jint>(ev.code),
                                ev.x, ev.y, ev.a);
            return;
        }
        default:
            return;
    }
}

}  // namespace

void set_smooth_zoom_enabled(bool enabled) {
    g_smooth_zoom.store(enabled, std::memory_order_relaxed);
}

bool start_input_bridge(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                        std::shared_ptr<MainGameActivityStub> activity, long activity_handle) {
    if (g_running.exchange(true)) {
        return true;
    }
    InputFns fns;
    fns.mouse_move = reinterpret_cast<MouseMoveFn>(
        lib.find_symbol("Java_com_roblox_engine_jni_NativeInputInterface_nativePassMouseMove"));
    fns.mouse_button = reinterpret_cast<MouseButtonFn>(
        lib.find_symbol("Java_com_roblox_engine_jni_NativeInputInterface_nativePassMouseButton"));
    fns.mouse_wheel = reinterpret_cast<MouseWheelFn>(
        lib.find_symbol("Java_com_roblox_engine_jni_NativeInputInterface_nativePassMouseWheel"));
    fns.key_event = reinterpret_cast<KeyEventFn>(
        lib.find_symbol("Java_com_roblox_engine_jni_NativeGLInterface_nativePassKeyEvent"));
    fns.pass_input = reinterpret_cast<PassInputFn>(
        lib.find_symbol("Java_com_roblox_engine_jni_NativeInputInterface_nativePassInput"));
    fns.pass_text = reinterpret_cast<PassTextFn>(
        lib.find_symbol("Java_com_roblox_engine_jni_NativeGLInterface_nativePassText"));
    fns.return_pressed = reinterpret_cast<ReturnPressedFn>(lib.find_symbol(
        "Java_com_roblox_engine_jni_NativeGLInterface_nativeReturnPressedFromOnScreenKeyboard"));
    fns.sync_text = reinterpret_cast<SyncTextFn>(lib.find_symbol(
        "Java_com_roblox_engine_jni_NativeGLInterface_syncTextboxTextAndCursorPosition2"));
    fns.update_keyboard_size = reinterpret_cast<UpdateKeyboardSizeFn>(
        lib.find_symbol("Java_com_roblox_engine_jni_NativeGLInterface_updateKeyboardSize"));
    fns.get_text_box_info = reinterpret_cast<GetTextBoxInfoFn>(
        lib.find_symbol("Java_com_roblox_engine_jni_NativeGLInterface_nativeGetTextBoxInfo"));
    fns.is_mouse_locked = reinterpret_cast<IsMouseLockedFn>(lib.find_symbol(
        "Java_com_roblox_engine_jni_NativeInputInterface_nativeGetMainWindowIsMouseLockedCenter"));
    fns.mouse_pinch = reinterpret_cast<MousePinchFn>(
        lib.find_symbol("Java_com_roblox_engine_jni_NativeInputInterface_nativePassMousePinch"));
    fns.gamepad_connect = reinterpret_cast<GamepadConnectFn>(lib.find_symbol(
        "Java_com_roblox_engine_jni_NativeInputInterface_nativeGamepadConnectEventWithGamepadType"));
    fns.gamepad_disconnect = reinterpret_cast<GamepadDisconnectFn>(lib.find_symbol(
        "Java_com_roblox_engine_jni_NativeInputInterface_nativeGamepadDisconnectEvent"));
    fns.gamepad_button = reinterpret_cast<GamepadButtonFn>(lib.find_symbol(
        "Java_com_roblox_engine_jni_NativeInputInterface_nativeGamepadButtonEvent"));
    fns.gamepad_axis = reinterpret_cast<GamepadAxisFn>(lib.find_symbol(
        "Java_com_roblox_engine_jni_NativeInputInterface_nativeGamepadAxisEvent"));
    fns.gamepad_supported_key = reinterpret_cast<GamepadSupportedKeyFn>(lib.find_symbol(
        "Java_com_roblox_engine_jni_NativeInputInterface_nativeSetGamepadSupportedKeyWithGamepadType"));
    fns.gamepad_supported_motion = reinterpret_cast<GamepadSupportedMotionFn>(lib.find_symbol(
        "Java_com_roblox_engine_jni_NativeInputInterface_nativeSetGamepadSupportedMotionWithGamepadType"));
    // The engine's own MouseBehavior, decoded out of the engine itself.
    // Everything the cursor does during a drag depends on it -- see
    // mouse_behavior.h -- and it degrades to "unknown" safely.
    stud::runtime::init_mouse_behavior_probe(lib);
    g_jvm = &jvm;
    g_agdk.activity = std::move(activity);
    g_agdk.handle = static_cast<jlong>(activity_handle);

    // Controller rumble. Registered here rather than at bring-up because
    // this is where a pad's arrival and departure are already known, and
    // the engine has to be told the truth at both moments -- a pad
    // plugged in while Stud is running is the ordinary case, and one
    // that was never there must not be advertised.
    {
        auto* jvm_ptr = &jvm;
        const auto* lib_ptr = &lib;
        stud::jni_bridge::run_haptics_bridge(
            jvm, lib, [](int device_id, float strong, float weak, int duration_ms) {
                if (!stud::render_client::connection().connected()) return false;
                const uint64_t a[8] = {static_cast<uint64_t>(device_id),
                                       static_cast<uint64_t>(strong * 1000.0f),
                                       static_cast<uint64_t>(weak * 1000.0f),
                                       static_cast<uint64_t>(duration_ms),
                                       0, 0, 0, 0};
                return stud::render_client::connection().call(
                           stud::render_host::CallId::SetGamepadRumble, a, nullptr, 0, nullptr, 0,
                           nullptr) != 0;
            });
        gamepad_presence_hook() = [jvm_ptr, lib_ptr](int device_id, bool can_rumble) {
            stud::jni_bridge::set_haptics_device(*jvm_ptr, *lib_ptr, device_id, can_rumble);
        };
    }

    if (fns.mouse_move == nullptr && fns.mouse_button == nullptr && fns.key_event == nullptr) {
        std::fprintf(stderr,
                     "stud: input bridge: this build exports none of the real input entry "
                     "points -- not starting\n");
        g_running.store(false);
        return false;
    }
    std::printf("stud: input bridge starting: move=%d button=%d wheel=%d key=%d touch=%d\n",
                fns.mouse_move != nullptr, fns.mouse_button != nullptr,
                fns.mouse_wheel != nullptr, fns.key_event != nullptr,
                fns.pass_input != nullptr);
    std::printf("stud: input bridge: text=%d return=%d sync=%d keyboardSize=%d textBoxInfo=%d\n",
                fns.pass_text != nullptr, fns.return_pressed != nullptr, fns.sync_text != nullptr,
                fns.update_keyboard_size != nullptr, fns.get_text_box_info != nullptr);

    std::thread([&jvm, fns]() {
        // Every thread that makes a real JNI call has to be attached
        // first -- the same precondition every other background caller in
        // this codebase observes.
        stud::jni_bridge::ensure_current_thread_attached_to_jvm();
        float last_x = 0.0f;
        float last_y = 0.0f;
        bool g_pointer_locked = false;
        std::vector<stud::android_glue::HostInputEvent> batch(64);
        while (g_running.load(std::memory_order_relaxed)) {
            // Input's own connection, so asking for events never queues
            // behind a frame's worth of GL calls and never makes one wait
            // -- and so the host can simply hold the request until an
            // event arrives. Falls back to the shared connection if the
            // second socket could not be opened.
            auto& input_conn = stud::render_client::input_connection().connected()
                                   ? stud::render_client::input_connection()
                                   : stud::render_client::connection();
            const bool own_connection = stud::render_client::input_connection().connected();
            if (!input_conn.connected()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            uint64_t a[8] = {};
            // How long the host may wait for an event before answering
            // empty. Only on input's own connection: waiting on the
            // shared one would park every GL call behind it, which is
            // what once cost a measured frame 642ms.
            a[0] = own_connection ? static_cast<uint64_t>(poll_interval_ms()) : 0;
            uint32_t written = 0;
            uint64_t n = 0;
            if (own_connection) {
                n = input_conn.call(CallId::PollInputEvents, a, nullptr, 0, batch.data(),
                                    static_cast<uint32_t>(batch.size() * sizeof(batch[0])),
                                    &written);
            } else if (!input_conn.try_call(
                           CallId::PollInputEvents, a, batch.data(),
                           static_cast<uint32_t>(batch.size() * sizeof(batch[0])), &written, &n)) {
                // Sharing: best-effort, skip the round rather than block
                // the render thread behind us.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            size_t count = written / sizeof(batch[0]);
            if (count > batch.size()) count = batch.size();

            // A Lua TextBox taking focus ends every held key, the same way
            // losing the window does.
            //
            // The engine stops treating keys as gameplay input the moment
            // one of its own text boxes has focus, so the release that
            // would have stopped a walk is simply never acted on -- click
            // into chat while holding W and the character walks forever.
            // A real device has the same handover and ends the gesture.
            //
            // Polled rather than driven by the key event, because focus
            // usually arrives from a MOUSE click and the engine reports it
            // asynchronously, with no key event anywhere near it.
            std::vector<android_glue::HostInputEvent> released;
            bool pulse_on_focus = false;
            {
                static jlong previous_text_box = 0;
                const jlong text_box = NativeGLJavaInterfaceStub::active_text_box();
                if (text_box != 0 && previous_text_box == 0) {
                    // Unconditional: whether this fires, and how many keys
                    // it found held, is the first thing to check when a
                    // walk does not stop.
                    std::printf("stud: input bridge: a text box took focus with %zu key(s) held\n",
                                held_keys().size());
                    std::fflush(stdout);
                    for (uint32_t code : held_keys()) {
                        android_glue::HostInputEvent up{};
                        up.type = android_glue::HostInputEvent::kKey;
                        up.code = code;
                        up.a = 0.0f;
                        released.push_back(up);
                    }
                    // Held until really let go, so the repeats that
                    // follow cannot press them back down.
                    keys_released_into_text_entry() = held_keys();
                    pulse_on_focus = !held_keys().empty();
                    held_keys().clear();

                }
                if (text_box == 0 && previous_text_box != 0) {
                    // The text box let go, so the engine is listening to
                    // keys again -- and this is the only moment a release
                    // for them can actually land.
                    //
                    // Measured: while a text box has focus the engine
                    // discards key events entirely, so both the release
                    // Stud sends at focus time and the user's own physical
                    // release are ignored, and the key stays held forever
                    // -- "it only stops if i press the same key once
                    // again", which is exactly a release finally being
                    // heard. Sending them here ends that for good.
                    //
                    // Harmless if the key really is still held: the
                    // compositor's own repeats press it straight back
                    // down, which is the truth of the matter.
                    auto& still_suppressed = keys_released_into_text_entry();
                    for (uint32_t code : still_suppressed) {
                        android_glue::HostInputEvent up{};
                        up.type = android_glue::HostInputEvent::kKey;
                        up.code = code;
                        up.a = 0.0f;
                        released.push_back(up);
                    }
                    if (!still_suppressed.empty()) {
                        std::printf("stud: input bridge: the text box let go -- releasing %zu "
                                    "key(s) the engine would not let go of\n",
                                    still_suppressed.size());
                        std::fflush(stdout);
                    }
                    still_suppressed.clear();
                }
                previous_text_box = text_box;
            }

            if (count > 0 || !released.empty() || g_restore_window_focus_next_round) {
                // One-shot confirmation that the real seat is actually
                // reaching this process -- silent afterwards.
                // Temporary, low-rate diagnostic: which real event types
                // actually reach the engine, once per second at most.
                static unsigned counts[14] = {};
                static auto last_report = std::chrono::steady_clock::now();
                for (size_t i = 0; i < count; ++i) {
                    if (batch[i].type < 14) ++counts[batch[i].type];
                }
                static const bool input_trace = std::getenv("STUD_INPUT_TRACE") != nullptr;
                auto now = std::chrono::steady_clock::now();
                if (input_trace && now - last_report > std::chrono::seconds(2)) {
                    last_report = now;
                    std::printf("stud: input bridge: motion=%u button=%u axis=%u key=%u "
                                "pad(connect=%u button=%u axis=%u) last=(%.1f,%.1f)\n",
                                counts[1], counts[2], counts[3], counts[4], counts[8],
                                counts[10], counts[11], batch[0].x, batch[0].y);
                    std::fflush(stdout);
                }
                FakeJni::LocalFrame frame(jvm);
                auto& env = frame.getJniEnv();
                auto* jni_env = static_cast<JNIEnv*>(&env);
                // jnivm local references are only valid on the thread and
                // frame that made them, so the activity reference and the
                // AGDK method IDs are re-established per batch.
                if (g_agdk.activity) {
                    g_agdk_activity_ref = env.createLocalReference(g_agdk.activity);
                    resolve_agdk(env, g_agdk_activity_ref);
                    g_agdk_env = &env;
                }
                // Ahead of the batch: whatever else arrived this round, the
                // keys that were being held are no longer held.
                for (const android_glue::HostInputEvent& up : released) {
                    dispatch_event(up, fns, jni_env, last_x, last_y);
                }

                // ...and then tell the engine the window lost focus, and
                // give it back on the next round.
                //
                // Those releases above are not enough on their own, and
                // this was measured rather than assumed: a live run showed
                // "a text box took focus with 1 key(s) held", so the
                // release really was sent -- and the character carried on
                // walking. Once one of its own text boxes has focus the
                // engine stops treating key events as gameplay, so neither
                // Stud's release nor the user's own physical one is acted
                // on. It does not type them either (chat shows no doubled
                // characters), so they are discarded outright and no key
                // event can clear what is already held.
                //
                // Window focus is the one lever the engine demonstrably
                // honours -- it is what produces APP_CMD_LOST_FOCUS in
                // every log -- and it is what a real device does here too:
                // the IME takes window focus when it opens, and an Android
                // app drops held input when it loses focus. Restoring it a
                // round later keeps the window genuinely focused for the
                // typing that follows, rather than leaving the engine
                // believing it is in the background while someone types.
                static const bool pulse_focus =
                    std::getenv("STUD_NO_TEXTBOX_FOCUS_PULSE") == nullptr;
                const bool can_pulse = pulse_focus && g_agdk_env != nullptr &&
                                       g_agdk_activity_ref != nullptr &&
                                       g_agdk.on_window_focus_changed != nullptr;
                if (g_restore_window_focus_next_round && can_pulse) {
                    g_restore_window_focus_next_round = false;
                    g_agdk_env->CallVoidMethod(g_agdk_activity_ref,
                                               g_agdk.on_window_focus_changed, g_agdk.handle,
                                               static_cast<jboolean>(JNI_TRUE));
                }
                if (pulse_on_focus && can_pulse) {
                    g_agdk_env->CallVoidMethod(g_agdk_activity_ref,
                                               g_agdk.on_window_focus_changed, g_agdk.handle,
                                               static_cast<jboolean>(JNI_FALSE));
                    g_restore_window_focus_next_round = true;
                    std::printf("stud: input bridge: told the engine the window lost focus, to "
                                "make it let go of the held key(s)\n");
                    std::fflush(stdout);
                }
                for (size_t i = 0; i < count; ++i) {
                    dispatch_event(batch[i], fns, jni_env, last_x, last_y);
                }
                g_agdk_env = nullptr;
                g_agdk_activity_ref = nullptr;
            }

            // Mouse look. The engine is asked, every round, whether it wants
            // the pointer held -- exactly what the real app does on every
            // mouse event (the app's own input handler takes Android's pointer capture when
            // this is true and drops it when it is false). Without it the
            // real cursor keeps travelling across the desktop while the
            // camera turns, and the engine's own cursor lands wherever the
            // pointer ended up the moment the button is released, which is
            // the teleport. Polled rather than event-driven because the
            // engine offers no callback, and polled even on an empty round
            // because it can release the lock with no input at all.
            // Pay out the pending zoom over real time, not per poll round:
            // an exponential ease with a time constant, so the feel does
            // not change with STUD_INPUT_POLL_MS. About 4 time constants
            // covers the move, so the default settles in roughly a third
            // of a second -- close to the desktop client's own camera
            // spring, and long enough to read as motion rather than a cut.
            if (fns.mouse_wheel != nullptr && smooth_zoom_enabled()) {
                float step = 0.0f;
                {
                    static std::chrono::steady_clock::time_point last_pay =
                        std::chrono::steady_clock::now();
                    const auto now = std::chrono::steady_clock::now();
                    const float dt_ms = std::chrono::duration<float, std::milli>(now - last_pay).count();
                    last_pay = now;
                    std::lock_guard<std::mutex> lock(wheel_mutex());
                    float& left = wheel_remaining();
                    if (left != 0.0f) {
                        // STUD_ZOOM_TAU_MS tunes it; 0 disables the ease.
                        static const float tau = [] {
                            const char* raw = std::getenv("STUD_ZOOM_TAU_MS");
                            if (raw == nullptr) return 80.0f;
                            float v = std::strtof(raw, nullptr);
                            return v >= 0.0f ? v : 80.0f;
                        }();
                        const float frac =
                            tau > 0.0f ? 1.0f - std::exp(-std::min(dt_ms, 100.0f) / tau) : 1.0f;
                        step = left * frac;
                        left -= step;
                        // Terminate on what is left, never on how small a
                        // step is: this loop can come round in well under a
                        // millisecond, which makes an honest step tiny, and
                        // treating a tiny step as "close enough, pay it all"
                        // collapsed the whole ease into two payments.
                        //
                        // This threshold is what actually ends the glide --
                        // an exponential never reaches zero -- so it sets
                        // the tail length: tau * ln(1/eps), which is about
                        // 370ms at the defaults. The earlier 0.002 (0.2% of
                        // a notch) ran ~500ms, and that last stretch moves
                        // the camera by an amount too small to see while
                        // still reading as "it is still gliding". One
                        // percent of a notch is past the point of being
                        // visible, so ending there trims the tail without
                        // touching the part of the motion anyone can see.
                        static const float end_eps = [] {
                            const char* raw = std::getenv("STUD_ZOOM_END_EPS");
                            if (raw == nullptr) return 0.01f;
                            float v = std::strtof(raw, nullptr);
                            return v > 0.0f ? v : 0.01f;
                        }();
                        if (std::fabs(left) < end_eps) {
                            step += left;
                            left = 0.0f;
                        }
                    }
                }
                if (step != 0.0f) {
                    if (input_trace_enabled()) {
                        std::printf("stud: wheel pay step=%.4f left=%.4f\n", step,
                                    wheel_remaining());
                        std::fflush(stdout);
                    }
                    FakeJni::LocalFrame wheel_frame(jvm);
                    auto* wheel_env = static_cast<JNIEnv*>(&wheel_frame.getJniEnv());
                    const float wx = last_x > 0.0f ? last_x : 0.0f;
                    const float wy = last_y > 0.0f ? last_y : 0.0f;
                    call_trapping_abort(fns.mouse_wheel, wheel_env, nullptr, wx, wy, step);
                }
            }
            // Focus changes arrive on the engine's own thread, which
            // cannot reach the render connection, so they are picked up
            // here: a box appearing shows the overlay with whatever text
            // the box already had, and a box going away hides it.
            //
            // The geometry is re-read every round rather than taken once
            // from showKeyboard, because a real TextBox MOVES: the Home
            // search box animates wider when it is clicked, and a widget
            // pinned to where the box started is visibly wrong for the
            // whole animation. nativeGetTextBoxInfo is the engine's own
            // answer to "where is the focused box right now", and it is
            // what a real device re-reads on every
            // onLuaTextBoxPropertyChanged callback.
            {
                static long last_box = 0;
                static std::string last_text;
                static NativeGLJavaInterfaceStub::TextBoxStyle last_style;
                const long box = NativeGLJavaInterfaceStub::active_text_box();
                std::string text =
                    box != 0 ? NativeGLJavaInterfaceStub::active_text_box_text() : std::string();
                if (box != 0 && fns.get_text_box_info != nullptr) {
                    FakeJni::LocalFrame info_frame(jvm);
                    auto* info_env = static_cast<JNIEnv*>(&info_frame.getJniEnv());
                    jobject info = nullptr;
                    if (call_trapping_abort_with_result(fns.get_text_box_info, info, info_env,
                                                        nullptr) &&
                        info != nullptr) {
                        NativeGLJavaInterfaceStub::TextBoxStyle live;
                        if (read_text_box_info(info_env, info, live)) {
                            NativeGLJavaInterfaceStub::set_active_text_box_style(live);
                        }
                    }
                    clear_pending_jni_exception(info_env, "nativeGetTextBoxInfo");
                }
                const auto style = NativeGLJavaInterfaceStub::active_text_box_style();
                const bool moved = style.x != last_style.x || style.y != last_style.y ||
                                   style.width != last_style.width ||
                                   style.height != last_style.height ||
                                   style.font_size != last_style.font_size ||
                                   style.font != last_style.font ||
                                   style.color != last_style.color;
                if (box != last_box || text != last_text || moved) {
                    last_box = box;
                    last_text = text;
                    last_style = style;
                    if (editor().text() != text) editor().set_text(text);
                    push_text_overlay(box != 0, text, editor().caret(),
                                      editor().selection_begin(), editor().selection_end());
                }
            }
            {
                // What the engine is doing with its own cursor, asked at
                // the same cadence as the button state it goes with. A
                // rotation begins and ends inside a gesture, not on its
                // edges, so this has to be polled rather than read once at
                // the press.
                const auto behavior = stud::runtime::read_mouse_behavior();
                const int before = g_engine_mouse_behavior.exchange(static_cast<int>(behavior));
                if (input_trace_enabled() && before != static_cast<int>(behavior)) {
                    const char* name = behavior == stud::runtime::MouseBehavior::kDefault
                                           ? "Default"
                                           : behavior == stud::runtime::MouseBehavior::kLockCenter
                                                 ? "LockCenter"
                                                 : behavior ==
                                                           stud::runtime::MouseBehavior::
                                                               kLockCurrentPosition
                                                       ? "LockCurrentPosition"
                                                       : "unknown";
                    std::printf("stud: engine MouseBehavior = %s\n", name);
                    std::fflush(stdout);
                }
            }
            if (fns.is_mouse_locked != nullptr && mouse_lock_enabled()) {
                FakeJni::LocalFrame lock_frame(jvm);
                auto* lock_env = static_cast<JNIEnv*>(&lock_frame.getJniEnv());
                jboolean locked = 0;
                // Shift-lock and first-person: the one mode the engine will
                // actually admit to. Or-ed with the button hold above, which
                // covers the rotation drag it says nothing about.
                bool asked = call_trapping_abort_with_result(fns.is_mouse_locked, locked,
                                                             lock_env, nullptr);
                if (!asked) {
                    // A trapped call leaves `locked` untouched, so a
                    // failing query is indistinguishable from a steady
                    // "false" -- which is exactly what this looked like.
                    static bool said = false;
                    if (!said) {
                        said = true;
                        std::printf("stud: nativeGetMainWindowIsMouseLockedCenter TRAPPED -- the "
                                    "engine's own mouse-lock state is not being read at all\n");
                        std::fflush(stdout);
                    }
                }
                if (input_trace_enabled()) {
                    // Every poll, not just the changes: a value that never
                    // changes and a query that never runs look identical
                    // in a change-only trace.
                    static int n = 0;
                    if ((n++ % 120) == 0) {
                        std::printf("stud: LockCenter poll #%d ok=%d value=%d\n", n, asked ? 1 : 0,
                                    static_cast<int>(locked));
                        std::fflush(stdout);
                    }
                }
                const bool probe_valid = g_engine_mouse_behavior.load() !=
                                         static_cast<int>(stud::runtime::MouseBehavior::kUnknown);
                if (probe_valid) {
                    // The field itself, not the predicate over it. Both
                    // read the same MouseBehavior -- the predicate just
                    // answers for one of its three values -- and a live
                    // capture caught them disagreeing, which is worth
                    // knowing about rather than silently picking one.
                    const bool field_says_center = engine_locks_center();
                    if (asked && (locked != 0) != field_says_center) {
                        static bool said = false;
                        if (!said) {
                            said = true;
                            std::printf("stud: the engine's LockCenter predicate (%d) and its own "
                                        "MouseBehavior (%d) disagree -- going with the field\n",
                                        static_cast<int>(locked), g_engine_mouse_behavior.load());
                            std::fflush(stdout);
                        }
                    }
                    locked = field_says_center ? 1 : 0;
                    asked = true;
                }
                if (asked) {
                    static int last_reported = -1;
                    if (std::getenv("STUD_INPUT_TRACE") != nullptr && locked != last_reported) {
                        last_reported = locked;
                        std::printf("stud: engine LockCenter = %d\n", static_cast<int>(locked));
                        std::fflush(stdout);
                    }
                    // Only what the engine actually asks for.
                    //
                    // Holding a button no longer takes the lock. The
                    // cursor is meant to travel with the hand while a
                    // button is held -- confirmed directly by the user
                    // against the real client -- and pinning it is what
                    // made a right-drag over an in-game UI end somewhere
                    // else: the pointer could not move, so the position
                    // the engine was given came from accumulated deltas
                    // and had nothing to do with where the mouse was.
                    //
                    // This is also what the real device does. the app's own input handler
                    // takes Android's pointer capture only when
                    // nativeGetMainWindowIsMouseLockedCenter() is true --
                    // shift-lock and first person -- and leaves an
                    // ordinary drag uncaptured.
                    g_lock_from_engine = locked != 0;
                    apply_pointer_lock();
                }
            }
            // ~8ms: fast enough that a real click/drag feels immediate,
            // slow enough that an idle window costs almost nothing.
            // No sleep when input has its own connection: the host has
            // already waited for exactly this long, on the compositor's
            // own fd, and answered the moment something arrived. Sleeping
            // here as well would put the interval back.
            if (n == 0 && !own_connection) {
                std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms()));
            }
        }
    }).detach();
    return true;
}

void stop_input_bridge() { g_running.store(false); }

}  // namespace stud::jni_bridge
