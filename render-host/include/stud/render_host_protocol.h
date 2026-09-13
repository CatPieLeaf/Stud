#pragma once

#include <cstdint>
#include <cerrno>
#include <map>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <map>
#include <vector>

// Wire protocol between Process B (real bionic, running Roblox's own
// code) and stud-render-host (Process C: a real, separate, ordinary
// glibc process hosting ANGLE, the real Vulkan loader, and the Wayland
// window -- see render-host/src/main.cpp's own doc comment for why this
// is a separate process rather than hand-loading ANGLE's glibc runtime
// into Process B directly).
//
// Covers the full real GL/EGL symbol surface libroblox.so's own dynamic
// symbol table imports (confirmed via `the ELF headers --dyn-syms`, not
// guessed) -- 85 entries. Native Vulkan calls Roblox makes directly
// (not through this ANGLE/GLES path) get a deliberately narrower
// treatment -- see vulkan_wsi's own doc comment for why a full Vulkan
// struct marshaller isn't attempted here: nothing has driven Roblox far
// enough yet to know whether it actually goes deep into native Vulkan
// calls, and guessing a large, unverified marshalling surface for an
// unconfirmed need would contradict this project's own "verify against
// real evidence" discipline.
//
// EGLDisplay/EGLSurface/EGLContext/EGLConfig and Vulkan handles are
// never sent as real pointers -- Process C owns the real objects
// entirely; Process B only ever holds small opaque integer IDs.
//
// Buffer-carrying calls (shader source, vertex/texture data, glGet*
// output arrays, info logs) send a variable-length payload immediately
// after the fixed-size header, in each direction as needed -- simplest-
// correct-thing-first, not yet chunked for buffers over kMaxBufferBytes
// (large single texture uploads could exceed this; a real, flagged
// limitation for follow-on work once actual Roblox texture sizes are
// observed, not guessed at now).

namespace stud::render_host {

std::string default_socket_path();

// Where a shared host-visible allocation is backed, named by the id the
// client chose. Both processes derive the same path from the same id, so
// nothing but the id has to travel. It lives beside the socket, in the
// runtime directory, which is a tmpfs -- these pages are memory, not disk.
std::string shared_memory_path(uint64_t id);

enum class CallId : uint32_t {
    // EGL
    EglGetDisplay = 1,
    EglInitialize,
    EglBindApi,
    EglChooseConfig,
    EglCreateWindowSurface,
    EglCreatePbufferSurface,
    EglCreateContext,
    EglMakeCurrent,
    EglSwapBuffers,
    EglGetError,
    EglQueryString,
    EglDestroyContext,
    EglDestroySurface,
    EglGetConfigAttrib,
    EglGetCurrentContext,
    EglQuerySurface,
    EglSwapInterval,
    EglTerminate,
    EglGetProcAddress,

    // GLES2 -- state/scalar
    GlActiveTexture,
    GlAttachShader,
    GlBindBuffer,
    GlBindFramebuffer,
    GlBindRenderbuffer,
    GlBindTexture,
    GlBlendFunc,
    GlBlendFuncSeparate,
    GlCheckFramebufferStatus,
    GlClear,
    GlClearColor,
    GlClearDepthf,
    GlClearStencil,
    GlColorMask,
    GlCompileShader,
    GlCopyTexSubImage2D,
    GlCreateProgram,
    GlCreateShader,
    GlCullFace,
    GlDeleteProgram,
    GlDeleteShader,
    GlDepthFunc,
    GlDepthMask,
    GlDisable,
    GlDisableVertexAttribArray,
    GlDrawArrays,
    GlDrawElements,
    GlEnable,
    GlEnableVertexAttribArray,
    GlFramebufferRenderbuffer,
    GlFramebufferTexture2D,
    GlGenerateMipmap,
    GlGetError,
    GlLinkProgram,
    GlPixelStorei,
    GlPolygonOffset,
    GlReleaseShaderCompiler,
    GlRenderbufferStorage,
    GlScissor,
    GlStencilFunc,
    GlStencilMask,
    GlStencilOp,
    GlTexParameterf,
    GlTexParameteri,
    GlUniform1i,
    GlUseProgram,
    GlViewport,
    GlVertexAttribPointer,

    // GLES2 -- string in/out
    GlGetString,
    GlGetUniformLocation,
    GlBindAttribLocation,
    GlShaderSource,
    GlGetProgramInfoLog,
    GlGetShaderInfoLog,
    GlGetActiveUniform,

    // GLES2 -- fixed-count-N id arrays
    GlDeleteBuffers,
    GlDeleteFramebuffers,
    GlDeleteRenderbuffers,
    GlDeleteTextures,
    GlGenBuffers,
    GlGenVertexArrays,
    GlBindBufferRange,
    GlClearBufferfv,
    GlDrawBuffers,
    GlTexStorage2D,
    GlTexStorage3D,
    GlTexSubImage3D,
    GlProgramParameteri,
    GlGetUniformBlockIndex,
    GlUniformBlockBinding,
    GlGetActiveUniformBlockiv,
    // Real GLES3 sync objects + image copy -- libroblox calls all of
    // these during real render bring-up (live-confirmed by the
    // "CALLED unimplemented GL function" diagnostic). A GLsync is an
    // opaque pointer the engine never dereferences, so the host's own
    // real GLsync value travels the wire as a plain handle.
    GlFenceSync,
    GlClientWaitSync,
    GlWaitSync,
    GlDeleteSync,
    GlIsSync,
    GlGetSynciv,
    GlCopyImageSubData,
    GlBindVertexArray,
    GlDeleteVertexArrays,
    GlGenFramebuffers,
    GlGenRenderbuffers,
    GlGenTextures,

    // GLES2 -- small out-param arrays (count derived from pname)
    GlGetIntegerv,
    GlTexParameterfv,
    GlGetProgramiv,
    GlGetShaderiv,

    // GLES2 -- bulk buffer transfer
    GlBufferData,
    GlBufferSubData,
    GlTexImage2D,
    GlTexSubImage2D,
    GlCompressedTexImage2D,
    GlCompressedTexSubImage2D,
    GlReadPixels,

    // Vulkan (narrow surface-creation interposition only -- see this
    // header's own doc comment)
    VkCreateWaylandSurfaceForAndroidSurface,

    // ANativeWindow -- Process C owns the real window entirely (see
    // android-glue/src/native_window.cpp, excluded from Process B's own
    // bionic build for the same glibc-only-Wayland reason ANGLE itself
    // is). Confirmed, live, this session: libroblox.so directly
    // references these (an eager/data-bound import, same class as
    // AMediaFormat_delete), not just android-glue's own old resolver
    // table -- real, not speculative.
    ANativeWindowFromSurface,
    ANativeWindowGetWidth,
    ANativeWindowGetHeight,
    ANativeWindowAcquire,
    ANativeWindowRelease,

    // Real host->client input transport. Process C owns the compositor
    // connection and therefore the real wl_seat; Process B owns the JNI
    // bridge into libroblox's own `NativeInputInterface`. The protocol is
    // strictly client-initiated, so Process B polls this and the host
    // replies with however many queued `HostInputEvent`s fit.
    PollInputEvents,

    // Real window size, from the process that owns the actual window.
    // Process B has its own android-glue copy whose size is only ever
    // updated by a compositor configure it never receives, so it used to
    // guess -- three different sizes existed for one window. Returns
    // (width << 32) | height.
    // Real GLES3 indexed buffer binding. glBindBufferRange was already
    // implemented but its far more common sibling was not -- and it is
    // genuinely called (live-caught by the "CALLED unimplemented GL
    // function" diagnostic). A no-op here leaves a shader's uniform block
    // unbound, so everything it draws gets zeroed uniforms: geometry
    // collapses to nothing and simply never appears.
    GlBindBufferBase,
    GetWindowSize,
    // Reads a live range of a real GL buffer back to Process B. Needed
    // because glMapBufferRange is emulated with a client-side staging
    // allocation: a GL_MAP_WRITE_BIT map WITHOUT an invalidate bit is
    // required by the spec to preserve every byte the caller does not
    // overwrite, and a zeroed staging buffer uploaded whole on unmap
    // destroys them. libroblox really does map its index buffer that way
    // (live-caught: target=GL_ELEMENT_ARRAY_BUFFER access=GL_MAP_WRITE_BIT,
    // 18432 bytes), which zeroed indices it did not rewrite and collapsed
    // the geometry into degenerate triangles.
    GlGetBufferSubData,
    // Real geometry of the display itself (not the window), straight
    // from the compositor's own wl_output. Only Process C has a
    // compositor connection, so this is the only place the truth lives.
    // Together these give real dots-per-inch -- output pixels divided by
    // output millimetres -- which is what DisplayMetrics.xdpi/ydpi
    // actually mean and what Stud used to synthesise from its density
    // guess.
    // Returns (px_w << 48) | (px_h << 32) | (mm_w << 16) | mm_h. 16 bits
    // each is genuinely enough: no real display is 65536 pixels or 65
    // metres across. All four are 0 when no compositor reported them --
    // an honest "unknown", not a value to invent around.
    GetDisplayOutputGeometry,
    // Real HiDPI scale in effect for Stud's own window, in 120ths
    // (120 = 1x, 150 = 1.25x, 240 = 2x) -- the unit Wayland's own
    // fractional-scale protocol uses, and the only one that can express a
    // fractionally-scaled desktop. Process B divides it by 120 to get
    // Android's DisplayMetrics density, which is exactly the same
    // quantity: how many buffer pixels one UI unit occupies.
    GetWindowBufferScale,

    // Real audio output. The engine's own FMOD initialises an Android
    // audio device at game start, and with no device at all it fails --
    // live-caught in the engine's own log, immediately before the join
    // stalls:
    //   Error [FLog::FMOD] FMOD API error, FMOD_RESULT:51,
    //       functionname:System::init
    //   Error [FLog::Audio] FMOD initialization failed with error code 51!
    // (51 is FMOD_ERR_OUTPUT_INIT.) Process B is bionic and cannot talk to
    // the host's audio server, exactly as it cannot talk to the GPU, so
    // audio crosses the same narrow boundary the GL calls already do.
    //
    // Open: args are {sample_rate, channels, bytes_per_frame}; returns a
    // stream handle, or 0 if the host has no usable audio output.
    AudioOpenStream,
    // Write: args[0] is the stream handle, the in-buffer carries
    // interleaved PCM in the format the stream was opened with. The host
    // write blocks until the device has taken the data, which is what
    // paces the client's feeder thread -- there is no separate clock.
    AudioWriteFrames,
    // Close: args[0] is the stream handle.
    AudioCloseStream,

    // Vulkan, instance level. Roblox resolves every Vulkan command by
    // name through vkGetInstanceProcAddr (never a second dlsym), so the
    // client hands back a real function per name and forwards it here,
    // where the real driver lives -- Process C, never Process B, exactly
    // as the GL path already works. That isolation is the point: the
    // vendor driver never runs in a process sharing bionic or foreign
    // TLS.
    //
    // These four are the real, live-observed first commands the engine
    // asks for (captured with STUD_VULKAN_CALL_TRACE=1), not a guess.
    //
    // vkEnumerateInstanceVersion: out-buffer is one uint32 apiVersion.
    VkEnumerateInstanceVersion,
    // Enumerate{Extension,Layer}Properties: args[0] is the caller's array
    // capacity in elements (0 = "just tell me the count"); the in-buffer
    // carries the layer-name filter, empty for none. The out-buffer is a
    // uint32 count followed by that many fixed-size property structs.
    // Returns the real VkResult.
    VkEnumerateInstanceExtensionProperties,
    VkEnumerateInstanceLayerProperties,
    // vkCreateInstance: the in-buffer carries a flattened
    // VkInstanceCreateInfo (see vulkan_forward.h). The real VkInstance
    // handle comes back in the out-buffer; the return value is the real
    // VkResult.
    VkCreateInstance,
    // Physical-device level. args[0] is the instance or physical-device
    // handle (the host's own real pointer, passed through as an opaque
    // token -- Process B never dereferences one).
    //
    // The reply structs here are pure POD with no pNext and no embedded
    // pointers, so they travel as a plain byte copy, prefixed with the
    // size the host used so a header-version mismatch is caught rather
    // than silently misread.
    VkEnumeratePhysicalDevices,
    VkGetPhysicalDeviceProperties,
    VkGetPhysicalDeviceFeatures,
    VkGetPhysicalDeviceMemoryProperties,
    VkGetPhysicalDeviceQueueFamilyProperties,
    VkEnumerateDeviceExtensionProperties,
    // Both carry pNext chains, flattened as a run of ChainNodeHeader +
    // body (see vulkan_forward.h).
    VkGetPhysicalDeviceFeatures2,
    VkCreateDevice,
    // Format capability queries. args[0] is the physical device, args[1]
    // the format; the image variant adds type/tiling/usage/flags. Both
    // reply with a POD struct.
    VkGetPhysicalDeviceFormatProperties,
    VkGetPhysicalDeviceImageFormatProperties,

    // Device level. args[0] is the VkDevice handle throughout. The
    // simple creates carry their whole CreateInfo in args, since every
    // field is a scalar; the rest flatten into the in-buffer.
    VkGetDeviceQueue,
    VkCreateCommandPool,
    VkCreateSemaphore,
    VkCreateFence,
    VkCreateQueryPool,
    VkCreatePipelineCache,
    VkGetPipelineCacheData,
    VkDestroyPipelineCache,
    VkCreateImage,
    VkGetImageMemoryRequirements,
    VkGetPhysicalDeviceSurfaceCapabilitiesKHR,

    // Device memory. Mapped memory cannot cross a process boundary, so
    // vkMapMemory maps for real on the host and hands the client a size;
    // the client backs the mapping with its own staging allocation and
    // ships the bytes over on flush/unmap -- the same shape the GL path
    // already uses for glMapBufferRange.
    VkDeviceWaitIdle,
    VkAllocateMemory,
    VkBindImageMemory,
    VkFreeMemory,
    VkMapMemory,
    VkWriteMappedMemory,
    VkUnmapMemory,
    VkFlushMappedMemoryRanges,

    // Swapchain and its queries.
    VkGetPhysicalDeviceSurfaceFormatsKHR,
    VkGetPhysicalDeviceSurfacePresentModesKHR,
    VkGetPhysicalDeviceSurfaceSupportKHR,
    VkCreateSwapchainKHR,
    VkGetSwapchainImagesKHR,
    VkGetPhysicalDeviceImageFormatProperties2,

    // Buffers, views, shader modules, and the destroy family. Every
    // destroy is fire-and-forget: it returns nothing, so it rides the
    // protocol's reply-free path rather than costing a round-trip.
    VkCreateBuffer,
    VkGetBufferMemoryRequirements,
    VkBindBufferMemory,
    VkCreateImageView,
    VkCreateShaderModule,
    VkDestroyHandle,

    // Draw pipeline. Payloads use the shared Writer/Reader in
    // vulkan_forward.h rather than a struct per command.
    VkCreateRenderPass,
    VkCreateFramebuffer,
    VkCreateSampler,
    VkCreatePipelineLayout,
    VkCreateDescriptorSetLayout,
    VkCreateDescriptorPool,
    VkAllocateDescriptorSets,
    VkResetDescriptorPool,
    VkCreateDescriptorUpdateTemplate,
    VkUpdateDescriptorSetWithTemplate,
    VkCreateGraphicsPipelines,
    VkCreateComputePipelines,

    // Command buffers and submission.
    VkAllocateCommandBuffers,
    VkBeginCommandBuffer,
    VkEndCommandBuffer,
    VkResetCommandPool,
    VkQueueSubmit,
    VkWaitForFences,
    VkResetFences,
    VkAcquireNextImageKHR,
    VkQueuePresentKHR,
    VkGetQueryPoolResults,

    // Recording. All of these return nothing, so they ride the
    // reply-free path -- which is what makes Vulkan cheaper to forward
    // than GLES was, where glGetError alone forced hundreds of blocking
    // round-trips a frame.
    VkCmdRecord,

    // Opens the web-view panel the Lua app asked for (Messages and
    // several menus), and reports when the user has closed it. Appended
    // last so every existing id keeps its value.
    //
    // Process B is sandboxed and cannot spawn a desktop window; Process C
    // is the only unsandboxed process in the architecture, so it owns the
    // viewer the same way it owns the game window. The in-buffer carries
    // the URL, the title and the engine's own cookies, one per line --
    // never argv, because /proc/<pid>/cmdline is readable by anything
    // running as this user and a session cookie is a real credential.
    OpenWebView,
    // Returns 1 once for each viewer that has exited since the last ask,
    // which is what lets the app navigate away from the placeholder
    // screen it shows behind a web view.
    PollWebViewClosed,
    // The display's current refresh rate in mHz, and the rates it
    // supports. Read from the compositor rather than assumed: the engine
    // paces frames to what it believes the panel can do, and with
    // nothing to read it settles for 60. Supported rates come back in
    // the out-buffer as uint32 mHz values.
    GetDisplayRefreshRate,
    GetSupportedRefreshRates,
    // The native text input the engine asks Stud to draw over a focused
    // Lua TextBox. Appended last so every existing id keeps its value.
    //
    // A focused TextBox stops drawing its own text on Android -- the
    // engine sets an internal "a native widget is showing my text" flag
    // and substitutes an empty string until focus is lost -- because a
    // real device lays an Android EditText over the GL view. Process C
    // owns the window, so it owns that widget too. The in-buffer carries
    // the text as UTF-8; the args carry the box, the font and the caret.
    // See stud/text_overlay.h and the text-input entry in
    // the engineering notes.
    SetTextOverlay,
    // The real system clipboard, for the text box Stud draws itself.
    // Copy and paste belong to the platform's text widget -- an Android
    // EditText owns them on a device -- and only the process holding the
    // seat can hold a Wayland selection. SetClipboardText takes the text
    // in the in-buffer; GetClipboardText returns it in the out-buffer.
    SetClipboardText,
    GetClipboardText,
    // Which character of the overlay's text sits under a given x, so a
    // click or a drag can place the caret and select. The layout lives
    // where the font is, so the answer has to come from there. args[0] is
    // the x in buffer pixels; returns a byte offset into the text.
    TextOverlayOffsetAtX,
    // Does the real driver actually have this command? The name travels
    // in the in-buffer; returns 1 or 0.
    //
    // Needed because claiming a command exists when it does not is not a
    // harmless stub: an extension entry point is precisely what an
    // application null-checks to decide whether the extension is usable.
    // Answering non-null for vkGetRefreshCycleDurationGOOGLE told the
    // engine VK_GOOGLE_display_timing worked, and it paced frames to the
    // meaningless refresh duration that stub returned.
    VkHostHasProc,
    // Read and write Stud's safe storage: a named secret, encrypted at
    // rest with a key the system keyring holds.
    //
    // Process B is sandboxed and cannot reach the Secret Service, and
    // Process A has long since exited by the time a login happens, so
    // this process -- the only unsandboxed one still alive -- does the
    // handoff, by running stud-ui in a one-shot mode.
    //
    // StoreSecret's in-buffer is "<name>\n<value>"; LoadSecret's is the
    // name alone and the value comes back in the out-buffer. Values
    // travel on the helper's stdin/stdout, never argv or the
    // environment, because /proc/<pid>/cmdline and /proc/<pid>/environ
    // are readable by anything running as this user.
    StoreSecret,
    LoadSecret,
    // Many recorded commands in one request.
    //
    // A command's wire header is 88 bytes -- eight argument slots and the
    // lengths -- while a draw's payload is sixteen. At ten thousand
    // commands a frame that framing is several times the size of the data,
    // and every one costs its own append into the send queue. A batch
    // carries them as [u64 command buffer][u32 kind][u32 length][payload]
    // back to back under a single header, which is the same stream in the
    // same order, just without repeating what does not change.
    VkCmdRecordBatch,
    // Mouse look. The engine is polled for it (its own
    // nativeGetMainWindowIsMouseLockedCenter), but only this process has a
    // compositor connection, so the actual pointer lock has to happen here.
    // While locked the compositor holds the cursor still and reports raw
    // deltas, which arrive through PollInputEvents as kPointerRelative.
    // Unlocking carries the position the pointer should reappear at, as
    // 8.8 fixed point, so the desktop pointer ends up where the engine's
    // own cursor has moved to rather than back at the press point.
    SetPointerLocked,
    // Multisampled renderbuffer storage. The engine builds its main colour
    // buffer as an MSAA renderbuffer, and while this was a no-op stub the
    // renderbuffer had no storage at all, so the framebuffer it was
    // attached to came back GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT (0x8cd6)
    // and the engine gave up on its main render target. Only reachable on
    // the OpenGL path, which is why it went unnoticed while everything ran
    // on Vulkan.
    GlRenderbufferStorageMultisample,
    // A pure hint: it tells the driver the named attachments' contents are
    // no longer needed, which lets a tiler skip writing them back. Wrong
    // to leave as a no-op even so -- on a tiler (and Zink over a tiler)
    // it is the difference between discarding a render target and
    // resolving it every frame.
    GlInvalidateFramebuffer,
    // Resolves the multisampled colour buffer into the framebuffer that
    // is actually presented -- so on the OpenGL path nothing reaches the
    // screen without it. Ten arguments where the header carries eight, so
    // `mask` and `filter` ride in the in-buffer, the same shape
    // glTexSubImage3D already uses for its overflow.
    GlBlitFramebuffer,
    // Instanced drawing and its per-attribute divisor. The engine draws
    // instanced on the OpenGL path, so while these were no-ops the draws
    // silently did nothing. glVertexAttribIPointer belongs with them:
    // integer attributes are how an instance index is usually fed in.
    GlDrawArraysInstanced,
    GlDrawElementsInstanced,
    GlVertexAttribDivisor,
    GlVertexAttribIPointer,
    // The game server's public address, discovered by Process B off the
    // engine's own UDP socket. render-host runs the region lookup and the
    // notification: Process B cannot reach the session bus from inside its
    // sandbox, and neither process is a Qt application. The address
    // travels in the in-buffer as plain text.
    NotifyServerRegion,
    // The experience the engine is in, for the Discord presence. The
    // in-buffer carries the place id and job id as text; an empty buffer
    // means the app shell, where the presence shows Stud itself.
    SetGamePresence,
    // End the session now: close the window and exit, without waiting for
    // the client to finish and disconnect.
    //
    // Only "Close Stud when leaving a game" uses this. That setting had
    // Process B decide to quit and then run its ordinary teardown --
    // LeaveGame, DestroyApp -- while the engine, already back on its app
    // shell, went on rendering it. The result was the home screen visibly
    // flashing up for half a second on the way out, which is not what the
    // setting promises.
    //
    // This process never replies: it exits inside the handler, so the
    // caller sees the connection drop rather than an answer, which is
    // exactly what it wants -- the window is gone the moment the decision
    // is made and Process B tears down behind a closed window.
    EndSession,
    // One message from a web view's page to the engine, or nothing.
    //
    // A Roblox page inside a web view talks to its host through a
    // JavaScript bridge -- the real Android client exposes
    // `__globalRobloxAndroidBridge__.executeRoblox(json)` -- and that is
    // how a login challenge reports back that it is done. Without it the
    // page completes an OTP or a captcha and the app never hears, so the
    // login simply never finishes.
    //
    // Returns the number of bytes written to the out-buffer, 0 when the
    // queue is empty. One message per call, so nothing has to guess how
    // to frame several. Appended last: every existing id keeps its value.
    PollWebViewMessage,
    // Close whatever web-view panel is open, because the app asked.
    //
    // The engine publishes its own closeWindow when it is finished with a
    // panel -- a login challenge that has just been answered, for
    // instance. Without this the panel stayed on screen after a completed
    // OTP, with the app already signed in behind it. Appended last: every
    // existing id keeps its value.
    CloseWebView,
    // Rumble one pad. a[0] is the device id, a[1]/a[2] the heavy and
    // light magnitudes in 1/1000ths (the header carries ints, and a
    // float would not survive it), a[3] the duration in milliseconds
    // (0 = until told otherwise). Both magnitudes zero stops it.
    // Returns 1 when that pad actually rumbled. Appended last: every
    // existing id keeps its value.
    SetGamepadRumble,
    // Voice chat. The microphone is opened only when the engine asks for
    // an input stream, never at startup -- a[0] is the sample rate and
    // a[1] the channel count; returns 1 when it opened. Appended last:
    // every existing id keeps its value.
    AudioOpenInputStream,
    // Reads what has been captured since the last call into the
    // out-buffer, returning the number of bytes written. Never blocks.
    AudioReadFrames,
    AudioCloseInputStream,
};
// Every CallId's own name, for diagnostics -- STUD_IPC_TOP used to print
// a bare number, and reading one wrong (this enum starts at 1, so an
// off-by-one names a completely unrelated call) sent a whole measurement
// down the wrong path. Index 0 is unused because EglGetDisplay is 1.
//
// The static_assert is what keeps this honest: append a CallId without
// appending its name here and the build stops, which is the only
// mechanism that survives an append-only enum.
inline const char* call_id_name(CallId id) {
    static const char* const kNames[] = {
        "<none>",
        "EglGetDisplay",
        "EglInitialize",
        "EglBindApi",
        "EglChooseConfig",
        "EglCreateWindowSurface",
        "EglCreatePbufferSurface",
        "EglCreateContext",
        "EglMakeCurrent",
        "EglSwapBuffers",
        "EglGetError",
        "EglQueryString",
        "EglDestroyContext",
        "EglDestroySurface",
        "EglGetConfigAttrib",
        "EglGetCurrentContext",
        "EglQuerySurface",
        "EglSwapInterval",
        "EglTerminate",
        "EglGetProcAddress",
        "GlActiveTexture",
        "GlAttachShader",
        "GlBindBuffer",
        "GlBindFramebuffer",
        "GlBindRenderbuffer",
        "GlBindTexture",
        "GlBlendFunc",
        "GlBlendFuncSeparate",
        "GlCheckFramebufferStatus",
        "GlClear",
        "GlClearColor",
        "GlClearDepthf",
        "GlClearStencil",
        "GlColorMask",
        "GlCompileShader",
        "GlCopyTexSubImage2D",
        "GlCreateProgram",
        "GlCreateShader",
        "GlCullFace",
        "GlDeleteProgram",
        "GlDeleteShader",
        "GlDepthFunc",
        "GlDepthMask",
        "GlDisable",
        "GlDisableVertexAttribArray",
        "GlDrawArrays",
        "GlDrawElements",
        "GlEnable",
        "GlEnableVertexAttribArray",
        "GlFramebufferRenderbuffer",
        "GlFramebufferTexture2D",
        "GlGenerateMipmap",
        "GlGetError",
        "GlLinkProgram",
        "GlPixelStorei",
        "GlPolygonOffset",
        "GlReleaseShaderCompiler",
        "GlRenderbufferStorage",
        "GlScissor",
        "GlStencilFunc",
        "GlStencilMask",
        "GlStencilOp",
        "GlTexParameterf",
        "GlTexParameteri",
        "GlUniform1i",
        "GlUseProgram",
        "GlViewport",
        "GlVertexAttribPointer",
        "GlGetString",
        "GlGetUniformLocation",
        "GlBindAttribLocation",
        "GlShaderSource",
        "GlGetProgramInfoLog",
        "GlGetShaderInfoLog",
        "GlGetActiveUniform",
        "GlDeleteBuffers",
        "GlDeleteFramebuffers",
        "GlDeleteRenderbuffers",
        "GlDeleteTextures",
        "GlGenBuffers",
        "GlGenVertexArrays",
        "GlBindBufferRange",
        "GlClearBufferfv",
        "GlDrawBuffers",
        "GlTexStorage2D",
        "GlTexStorage3D",
        "GlTexSubImage3D",
        "GlProgramParameteri",
        "GlGetUniformBlockIndex",
        "GlUniformBlockBinding",
        "GlGetActiveUniformBlockiv",
        "GlFenceSync",
        "GlClientWaitSync",
        "GlWaitSync",
        "GlDeleteSync",
        "GlIsSync",
        "GlGetSynciv",
        "GlCopyImageSubData",
        "GlBindVertexArray",
        "GlDeleteVertexArrays",
        "GlGenFramebuffers",
        "GlGenRenderbuffers",
        "GlGenTextures",
        "GlGetIntegerv",
        "GlTexParameterfv",
        "GlGetProgramiv",
        "GlGetShaderiv",
        "GlBufferData",
        "GlBufferSubData",
        "GlTexImage2D",
        "GlTexSubImage2D",
        "GlCompressedTexImage2D",
        "GlCompressedTexSubImage2D",
        "GlReadPixels",
        "VkCreateWaylandSurfaceForAndroidSurface",
        "ANativeWindowFromSurface",
        "ANativeWindowGetWidth",
        "ANativeWindowGetHeight",
        "ANativeWindowAcquire",
        "ANativeWindowRelease",
        "PollInputEvents",
        "GlBindBufferBase",
        "GetWindowSize",
        "GlGetBufferSubData",
        "GetDisplayOutputGeometry",
        "GetWindowBufferScale",
        "AudioOpenStream",
        "AudioWriteFrames",
        "AudioCloseStream",
        "VkEnumerateInstanceVersion",
        "VkEnumerateInstanceExtensionProperties",
        "VkEnumerateInstanceLayerProperties",
        "VkCreateInstance",
        "VkEnumeratePhysicalDevices",
        "VkGetPhysicalDeviceProperties",
        "VkGetPhysicalDeviceFeatures",
        "VkGetPhysicalDeviceMemoryProperties",
        "VkGetPhysicalDeviceQueueFamilyProperties",
        "VkEnumerateDeviceExtensionProperties",
        "VkGetPhysicalDeviceFeatures2",
        "VkCreateDevice",
        "VkGetPhysicalDeviceFormatProperties",
        "VkGetPhysicalDeviceImageFormatProperties",
        "VkGetDeviceQueue",
        "VkCreateCommandPool",
        "VkCreateSemaphore",
        "VkCreateFence",
        "VkCreateQueryPool",
        "VkCreatePipelineCache",
        "VkGetPipelineCacheData",
        "VkDestroyPipelineCache",
        "VkCreateImage",
        "VkGetImageMemoryRequirements",
        "VkGetPhysicalDeviceSurfaceCapabilitiesKHR",
        "VkDeviceWaitIdle",
        "VkAllocateMemory",
        "VkBindImageMemory",
        "VkFreeMemory",
        "VkMapMemory",
        "VkWriteMappedMemory",
        "VkUnmapMemory",
        "VkFlushMappedMemoryRanges",
        "VkGetPhysicalDeviceSurfaceFormatsKHR",
        "VkGetPhysicalDeviceSurfacePresentModesKHR",
        "VkGetPhysicalDeviceSurfaceSupportKHR",
        "VkCreateSwapchainKHR",
        "VkGetSwapchainImagesKHR",
        "VkGetPhysicalDeviceImageFormatProperties2",
        "VkCreateBuffer",
        "VkGetBufferMemoryRequirements",
        "VkBindBufferMemory",
        "VkCreateImageView",
        "VkCreateShaderModule",
        "VkDestroyHandle",
        "VkCreateRenderPass",
        "VkCreateFramebuffer",
        "VkCreateSampler",
        "VkCreatePipelineLayout",
        "VkCreateDescriptorSetLayout",
        "VkCreateDescriptorPool",
        "VkAllocateDescriptorSets",
        "VkResetDescriptorPool",
        "VkCreateDescriptorUpdateTemplate",
        "VkUpdateDescriptorSetWithTemplate",
        "VkCreateGraphicsPipelines",
        "VkCreateComputePipelines",
        "VkAllocateCommandBuffers",
        "VkBeginCommandBuffer",
        "VkEndCommandBuffer",
        "VkResetCommandPool",
        "VkQueueSubmit",
        "VkWaitForFences",
        "VkResetFences",
        "VkAcquireNextImageKHR",
        "VkQueuePresentKHR",
        "VkGetQueryPoolResults",
        "VkCmdRecord",
        "OpenWebView",
        "PollWebViewClosed",
        "GetDisplayRefreshRate",
        "GetSupportedRefreshRates",
        "SetTextOverlay",
        "SetClipboardText",
        "GetClipboardText",
        "TextOverlayOffsetAtX",
        "VkHostHasProc",
        "StoreSecret",
        "LoadSecret",
        "VkCmdRecordBatch",
        "SetPointerLocked",
        "GlRenderbufferStorageMultisample",
        "GlInvalidateFramebuffer",
        "GlBlitFramebuffer",
        "GlDrawArraysInstanced",
        "GlDrawElementsInstanced",
        "GlVertexAttribDivisor",
        "GlVertexAttribIPointer",
        "NotifyServerRegion",
        "SetGamePresence",
        "EndSession",
        "PollWebViewMessage",
        "CloseWebView",
        "SetGamepadRumble",
        "AudioOpenInputStream",
        "AudioReadFrames",
        "AudioCloseInputStream",
    };
    static_assert(sizeof(kNames) / sizeof(kNames[0]) ==
                      static_cast<size_t>(CallId::AudioCloseInputStream) + 1,
                  "a CallId was added without its name -- append it to kNames");
    const int i = static_cast<int>(id);
    if (i < 0 || i >= static_cast<int>(sizeof(kNames) / sizeof(kNames[0]))) return "<unknown>";
    return kNames[i];
}


constexpr uint64_t kNullHandle = 0;
constexpr uint32_t kMaxBufferBytes = 16u * 1024u * 1024u;  // 16MiB, generous for a single call

struct Header {
    CallId call_id;
    uint64_t args[8];
    // Real GLES pixel-buffer-object support. When a real GL_PIXEL_UNPACK_BUFFER
    // (or GL_PIXEL_PACK_BUFFER, for reads) is bound, the `pixels`/`data`
    // argument of every real texture upload/download is a byte OFFSET into
    // that buffer, not a client-side pointer -- so it must NOT be dereferenced
    // or sent as an in-buffer. Live-caught: with buffer allocation finally
    // working, libroblox started streaming compressed textures through a real
    // PBO, and the client faithfully tried to read 1MB from the raw offset
    // value, killing the render connection with EFAULT.
    // 0 means "no PBO bound, the pixel data is in the in-buffer as usual";
    // otherwise this is the real offset plus one.
    uint64_t pixel_buffer_offset_plus_one;
    // Request flags. kNoReply means the server must NOT write a response for
    // this request: the caller does not use the return value, so making it
    // wait for one is pure latency. Measured before this existed: a single
    // frame costs ~1840 synchronous round-trips and 16-100ms of pure IPC wait
    // (STUD_IPC_STATS=1), which was Stud's frame-rate ceiling.
    enum Flags : uint32_t { kNoReply = 1u << 0 };
    uint32_t flags;
    uint32_t in_buffer_len;   // bytes following this header, sent by the caller
    uint32_t out_buffer_len;  // bytes the caller is prepared to receive back (its buffer capacity)
};

struct ResponseHeader {
    uint64_t result;
    uint32_t out_buffer_len;  // bytes actually written, follows this header
};

// Thin, shared (portable POSIX, works identically compiled bionic or
// glibc) client used both by Process B's real forwarding stubs and this
// module's own test client -- owns exactly one blocking, synchronous
// request/response round-trip.
//
// Real, live-caught bug fixed (the engineering notes): this doc comment used
// to say "no connection pooling/threading" as if that were a documented
// constraint on the *caller* -- but render_client_common.cpp's own
// connection() returns one process-wide, shared Client instance, and
// several real call sites (run_bounded_v2_call's own detached background
// threads for InitWithParams/StartAppWithParams/StartGameWithParam/etc.)
// can genuinely call into GL/EGL concurrently, each forwarding over this
// exact same socket. With no serialization, two threads' requests (or a
// request and a response) can interleave on the wire -- the protocol
// has no per-call ID to resync with, so a reply meant for thread A can
// get consumed by thread B's read_all(), leaving A blocked in read_all()
// forever waiting for a reply that already went to someone else. Live-
// confirmed via a live syscall trace: a real, freshly-spawned worker thread for
// `nativeAppBridgeV2StartApp` sent one real request/response pair over
// this socket then blocked on a futex indefinitely, with no
// corresponding response ever unblocking it -- consistent with exactly
// this race, not a hang inside Roblox's own code. Fixed by serializing
// the whole round-trip with a mutex, so `call()` is atomic with respect
// to other threads sharing the same Client.
class Client {
public:
    bool connect_to(const std::string& path) {
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        return true;
    }

    // `in_buffer`/`in_len`: data sent to the server (nullptr/0 if none).
    // `out_buffer`/`out_capacity`: where a returned buffer is copied
    // (nullptr/0 if the caller expects none); `out_len_written` receives
    // the real byte count the server sent back (may be less than
    // `out_capacity`).
    uint64_t call(CallId id, const uint64_t (&args)[8], const void* in_buffer, uint32_t in_len,
                  void* out_buffer, uint32_t out_capacity, uint32_t* out_len_written,
                  uint64_t pixel_buffer_offset_plus_one = 0) {
        // STUD_IPC_CONTENTION=1: how much time engine threads spend queued
        // behind each other for this one connection.
        //
        // Vulkan is designed for several threads to record commands at
        // once, and the engine does exactly that -- but every one of those
        // calls comes through here, so they serialise on a single mutex
        // that a real driver does not have. A thread waiting on a mutex
        // burns no CPU, so this cost is invisible to a profiler: it shows
        // up as one busy thread and a lot of idle ones, which is precisely
        // what Stud's in-game profile looks like.
        static const bool contention = std::getenv("STUD_IPC_CONTENTION") != nullptr;
        if (contention) {
            const auto t0 = std::chrono::steady_clock::now();
            const bool free_now = call_mutex_.try_lock();
            if (!free_now) call_mutex_.lock();
            const double waited_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count();
            std::lock_guard<std::mutex> adopt(call_mutex_, std::adopt_lock);
            report_contention(free_now, waited_ms);
            return call_locked(id, args, in_buffer, in_len, out_buffer, out_capacity,
                               out_len_written, pixel_buffer_offset_plus_one);
        }
        std::lock_guard<std::mutex> lock(call_mutex_);
        return call_locked(id, args, in_buffer, in_len, out_buffer, out_capacity, out_len_written,
                           pixel_buffer_offset_plus_one);
    }

    static void report_contention(bool uncontended, double waited_ms) {
        static std::atomic<uint64_t> calls{0};
        static std::atomic<uint64_t> blocked{0};
        static std::atomic<uint64_t> waited_us{0};
        const uint64_t n = calls.fetch_add(1) + 1;
        if (!uncontended) {
            blocked.fetch_add(1);
            waited_us.fetch_add(static_cast<uint64_t>(waited_ms * 1000.0));
        }
        if (n % 20000 == 0) {
            const uint64_t b = blocked.exchange(0);
            const uint64_t w = waited_us.exchange(0);
            std::fprintf(stderr,
                         "stud: ipc contention: %llu/20000 calls queued behind another thread, "
                         "%.1f ms waiting in total (%.3f ms each)\n",
                         static_cast<unsigned long long>(b), w / 1000.0,
                         b ? (w / 1000.0) / static_cast<double>(b) : 0.0);
        }
    }

    uint64_t call_locked(CallId id, const uint64_t (&args)[8], const void* in_buffer,
                         uint32_t in_len, void* out_buffer, uint32_t out_capacity,
                         uint32_t* out_len_written, uint64_t pixel_buffer_offset_plus_one) {
        // STUD_IPC_STATS=1: how many round-trips a frame really costs, and how
        // much wall time they take. Every call here is synchronous -- write a
        // request, block for a reply -- so this is the number that decides
        // Stud's frame rate ceiling, and it should be measured before the
        // protocol is changed to chase it.
        static const bool stats = std::getenv("STUD_IPC_STATS") != nullptr;
        std::chrono::steady_clock::time_point call_start;
        if (stats) call_start = std::chrono::steady_clock::now();
        // STUD_IPC_TOP=1: which call ids the round-trips actually go to.
        // "IPC is expensive" is not actionable; "one call id is 90% of the
        // round-trips" says exactly what to make reply-free or cache.
        emit_command_batch_locked();
        static const bool top = std::getenv("STUD_IPC_TOP") != nullptr;
        const auto top_t0 = top ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
        struct TopGuard {
            bool on;
            CallId id;
            std::chrono::steady_clock::time_point t0;
            ~TopGuard() {
                if (!on) return;
                const double ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - t0)
                                      .count();
                static std::mutex m;
                static std::map<int, std::pair<uint64_t, double>> hist;
                static uint64_t n = 0;
                std::lock_guard<std::mutex> lock(m);
                auto& e = hist[static_cast<int>(id)];
                ++e.first;
                e.second += ms;
                if (++n % 20000 != 0) return;
                std::vector<std::pair<int, std::pair<uint64_t, double>>> v(hist.begin(),
                                                                          hist.end());
                std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
                    return a.second.second > b.second.second;
                });
                std::fprintf(stderr, "stud: ipc top (per 20000 round-trips):\n");
                for (size_t i = 0; i < v.size() && i < 6; ++i) {
                    std::fprintf(stderr, "    %-44s %8llu calls  %8.1f ms\n",
                                 call_id_name(static_cast<CallId>(v[i].first)),
                                 static_cast<unsigned long long>(v[i].second.first),
                                 v[i].second.second);
                }
                hist.clear();
            }
        } top_guard{top, id, top_t0};
        last_call_id_ = static_cast<int>(id);
        last_in_len_ = in_len;
        Header hdr{};
        hdr.call_id = id;
        for (int i = 0; i < 8; ++i) hdr.args[i] = args[i];
        hdr.pixel_buffer_offset_plus_one = pixel_buffer_offset_plus_one;
        hdr.flags = 0;
        hdr.in_buffer_len = in_len;
        hdr.out_buffer_len = out_capacity;
        // Anything queued ahead of this must reach the server first: the
        // stream is ordered, and this call's own answer depends on that
        // earlier work having run.
        if (!flush_locked()) return kNullHandle;
        if (!write_all(&hdr, sizeof(hdr))) return kNullHandle;
        if (in_len > 0 && !write_all(in_buffer, in_len)) return kNullHandle;

        ResponseHeader resp{};
        if (!read_all(&resp, sizeof(resp))) return kNullHandle;
        if (resp.out_buffer_len > 0) {
            // Always drain exactly what the server sent, even if it's
            // more than the caller's own buffer can hold (a real,
            // correctness-load-bearing case: the protocol stream must
            // stay in sync regardless of caller-side truncation).
            uint32_t to_copy = resp.out_buffer_len < out_capacity ? resp.out_buffer_len : out_capacity;
            if (out_buffer != nullptr && to_copy > 0) {
                if (!read_all(out_buffer, to_copy)) return kNullHandle;
                if (resp.out_buffer_len > to_copy) {
                    drain(resp.out_buffer_len - to_copy);
                }
            } else {
                drain(resp.out_buffer_len);
            }
        }
        if (out_len_written != nullptr) *out_len_written = resp.out_buffer_len;
        if (stats) {
            ++stats_calls_;
            stats_ns_ += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - call_start)
                    .count());
            stats_by_id_[static_cast<uint32_t>(id)]++;
            if (id == CallId::EglSwapBuffers) {
                // Which ids still cost a round-trip: the ones worth making
                // reply-free next.
                uint32_t top_id[4] = {0, 0, 0, 0};
                uint64_t top_n[4] = {0, 0, 0, 0};
                for (const auto& kv : stats_by_id_) {
                    for (int slot = 0; slot < 4; ++slot) {
                        if (kv.second > top_n[slot]) {
                            for (int j = 3; j > slot; --j) {
                                top_n[j] = top_n[j - 1];
                                top_id[j] = top_id[j - 1];
                            }
                            top_n[slot] = kv.second;
                            top_id[slot] = kv.first;
                            break;
                        }
                    }
                }
                std::printf("stud: IPCSTATS frame reply=%llu %.2fms | void=%llu %.2fms | top: "
                            "%u=%llu %u=%llu %u=%llu %u=%llu\n",
                            static_cast<unsigned long long>(stats_calls_),
                            static_cast<double>(stats_ns_) / 1e6,
                            static_cast<unsigned long long>(stats_void_calls_),
                            static_cast<double>(stats_void_ns_) / 1e6, top_id[0],
                            static_cast<unsigned long long>(top_n[0]), top_id[1],
                            static_cast<unsigned long long>(top_n[1]), top_id[2],
                            static_cast<unsigned long long>(top_n[2]), top_id[3],
                            static_cast<unsigned long long>(top_n[3]));
                std::fflush(stdout);
                stats_calls_ = 0;
                stats_ns_ = 0;
                stats_void_calls_ = 0;
                stats_void_ns_ = 0;
                stats_by_id_.clear();
            }
        }
        return resp.result;
    }

    bool connected() const { return fd_ >= 0; }

private:
    // Real, user-reported bug fixed: `connected()` used to just report
    // whether connect_to() ever succeeded, never whether the connection
    // was still alive -- so a client had no way to notice stud-render-
    // host had exited (e.g. the user closing the real window) short of
    // every individual call() already having failed. A failed write/read
    // here now marks the connection dead for real, so connected()
    // reflects live state and a caller (process-b's own render loop) can
    // treat "render-host is gone" as its own real shutdown signal.
    void mark_dead() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    // Real, live-caught bug: neither of these retried on EINTR. Process B
    // is a heavily-signalled process (trap_recovery's own SIGSEGV/SIGTRAP
    // handling, plus whatever timers libroblox arms), and a single
    // interrupted read()/write() here permanently killed the render
    // connection -- silently, since mark_dead() said nothing and every
    // later GL/EGL call then just returned 0. Live-observed as the engine
    // logging "eglMakeCurrent failed with '0'" forever while the host had
    // stopped receiving any traffic at all. EINTR is not a connection
    // error; retry it.
    bool write_all(const void* data, uint32_t len) {
        const char* p = static_cast<const char*>(data);
        uint32_t remaining = len;
        while (remaining > 0) {
            ssize_t n = ::write(fd_, p, remaining);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                report_death("write");
                mark_dead();
                return false;
            }
            p += n;
            remaining -= static_cast<uint32_t>(n);
        }
        return true;
    }
    bool read_all(void* data, uint32_t len) {
        char* p = static_cast<char*>(data);
        uint32_t remaining = len;
        while (remaining > 0) {
            ssize_t n = ::read(fd_, p, remaining);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                report_death("read");
                mark_dead();
                return false;
            }
            p += n;
            remaining -= static_cast<uint32_t>(n);
        }
        return true;
    }
    void report_death(const char* which) {
        if (fd_ < 0) return;  // already reported
        std::fprintf(stderr,
                     "stud: render-client: connection lost on %s (errno=%d %s) during call_id=%d "
                     "in_len=%u\n",
                     which, errno, std::strerror(errno), last_call_id_, last_in_len_);
    }
    void drain(uint32_t len) {
        char scratch[4096];
        while (len > 0) {
            uint32_t chunk = len < sizeof(scratch) ? len : sizeof(scratch);
            if (!read_all(scratch, chunk)) return;
            len -= chunk;
        }
    }

    int fd_ = -1;
public:
    // Fire-and-forget request for a call whose return value the caller does
    // not use. Appended to a buffer and sent in bulk, so a frame's worth of
    // state and draw calls costs a handful of writes instead of ~1840 blocking
    // round-trips. Ordering is preserved -- it is one stream socket -- and
    // anything that does need an answer flushes this queue first.
    // Appends one recorded command to the current batch. Same stream, same
    // order -- the batch is emitted into the send queue before anything
    // else is written, so nothing can overtake it.
    void append_command(uint64_t cb, uint32_t kind, const void* payload, uint32_t len) {
        std::lock_guard<std::mutex> lock(call_mutex_);
        if (fd_ < 0) return;
        const size_t at = command_batch_.size();
        command_batch_.resize(at + 16 + len);
        uint8_t* p = command_batch_.data() + at;
        std::memcpy(p, &cb, sizeof(cb));
        std::memcpy(p + 8, &kind, sizeof(kind));
        std::memcpy(p + 12, &len, sizeof(len));
        if (len > 0 && payload != nullptr) std::memcpy(p + 16, payload, len);
        if (command_batch_.size() >= kCommandBatchBytes) emit_command_batch_locked();
    }

    void call_void(CallId id, const uint64_t (&args)[8], const void* in_buffer = nullptr,
                   uint32_t in_len = 0, uint64_t pixel_buffer_offset_plus_one = 0) {
        static const bool stats = std::getenv("STUD_IPC_STATS") != nullptr;
        std::chrono::steady_clock::time_point t0;
        if (stats) t0 = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(call_mutex_);
        if (fd_ < 0) return;
        emit_command_batch_locked();
        Header hdr{};
        hdr.call_id = id;
        for (int i = 0; i < 8; ++i) hdr.args[i] = args[i];
        hdr.pixel_buffer_offset_plus_one = pixel_buffer_offset_plus_one;
        hdr.flags = Header::kNoReply;
        hdr.in_buffer_len = in_len;
        hdr.out_buffer_len = 0;
        const auto* hdr_bytes = reinterpret_cast<const uint8_t*>(&hdr);
        queue_.insert(queue_.end(), hdr_bytes, hdr_bytes + sizeof(hdr));
        if (in_len > 0 && in_buffer != nullptr) {
            const auto* in_bytes = static_cast<const uint8_t*>(in_buffer);
            queue_.insert(queue_.end(), in_bytes, in_bytes + in_len);
        }
        // STUD_IPC_NO_PIPELINE=1 sends every request immediately, i.e. the
        // pre-pipelining behaviour. Kept as a one-switch A/B for exactly the
        // kind of "did batching starve something that was waiting on it"
        // question that is otherwise guesswork.
        static const bool no_pipeline = std::getenv("STUD_IPC_NO_PIPELINE") != nullptr;
        if (no_pipeline || queue_.size() >= kQueueFlushBytes) flush_locked();
        if (stats) {
            ++stats_void_calls_;
            stats_void_ns_ += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count());
        }
    }

    // Best-effort variant for a caller that would rather skip this round than
    // wait: takes the connection only if it is free. PollInputEvents uses it,
    // because it runs on its own timer and blocking there is actively harmful
    // -- it holds the connection for a whole round-trip, and every GL call the
    // render thread issues in that window queues up behind it (live-measured:
    // a single frame spent 642ms blocked this way). Skipping one poll costs
    // nothing; the next one is 8ms away.
    bool try_call(CallId id, const uint64_t (&args)[8], void* out_buffer, uint32_t out_capacity,
                  uint32_t* out_len_written, uint64_t* result_out) {
        std::unique_lock<std::mutex> lock(call_mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        lock.unlock();
        uint64_t r = call(id, args, nullptr, 0, out_buffer, out_capacity, out_len_written);
        if (result_out != nullptr) *result_out = r;
        return true;
    }

    // Public flush, for a caller that wants queued work on its way without
    // asking a question (the swap path already asks one, so it flushes there).
    bool flush() {
        std::lock_guard<std::mutex> lock(call_mutex_);
        emit_command_batch_locked();
        return flush_locked();
    }

private:
    static constexpr size_t kQueueFlushBytes = 256u * 1024u;
    static constexpr size_t kCommandBatchBytes = 32u * 1024u;

    // Moves the accumulated commands into the send queue as one request.
    void emit_command_batch_locked() {
        if (command_batch_.empty()) return;
        Header hdr{};
        hdr.call_id = CallId::VkCmdRecordBatch;
        hdr.flags = Header::kNoReply;
        hdr.in_buffer_len = static_cast<uint32_t>(command_batch_.size());
        hdr.out_buffer_len = 0;
        const auto* hdr_bytes = reinterpret_cast<const uint8_t*>(&hdr);
        queue_.insert(queue_.end(), hdr_bytes, hdr_bytes + sizeof(hdr));
        queue_.insert(queue_.end(), command_batch_.begin(), command_batch_.end());
        command_batch_.clear();
        if (queue_.size() >= kQueueFlushBytes) flush_locked();
    }

    std::vector<uint8_t> command_batch_;

    bool flush_locked() {
        if (queue_.empty()) return true;
        const bool ok = write_all(queue_.data(), static_cast<uint32_t>(queue_.size()));
        queue_.clear();
        return ok;
    }

    std::vector<uint8_t> queue_;
    std::map<uint32_t, uint64_t> stats_by_id_;
    uint64_t stats_void_calls_ = 0;
    uint64_t stats_void_ns_ = 0;
    uint64_t stats_calls_ = 0;
    uint64_t stats_ns_ = 0;
    int last_call_id_ = -1;
    uint32_t last_in_len_ = 0;
    std::mutex call_mutex_;
};

}  // namespace stud::render_host
