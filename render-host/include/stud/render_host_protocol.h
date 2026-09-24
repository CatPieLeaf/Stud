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

#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <thread>
#include <vector>

// Wire protocol between Process B (real bionic, running Roblox's own
// code) and stud-render-host (Process C: a real, separate, ordinary
// glibc process hosting ANGLE, the real Vulkan loader, and the Wayland
// window; see render-host/src/main.cpp's own doc comment for why this
// is a separate process rather than hand-loading ANGLE's glibc runtime
// into Process B directly).
//
// Covers the full real GL/EGL symbol surface libroblox.so's own dynamic
// symbol table imports (confirmed via `the ELF headers --dyn-syms`, not
// guessed), 85 entries. Native Vulkan calls Roblox makes directly
// (not through this ANGLE/GLES path) get a deliberately narrower
// treatment; see vulkan_wsi's own doc comment for why a full Vulkan
// struct marshaller isn't attempted here: nothing has driven Roblox far
// enough yet to know whether it actually goes deep into native Vulkan
// calls, and guessing a large, unverified marshalling surface for an
// unconfirmed need would contradict this project's own "verify against
// real evidence" discipline.
//
// EGLDisplay/EGLSurface/EGLContext/EGLConfig and Vulkan handles are
// never sent as real pointers. Process C owns the real objects
// entirely; Process B only ever holds small opaque integer IDs.
//
// Buffer-carrying calls (shader source, vertex/texture data, glGet*
// output arrays, info logs) send a variable-length payload immediately
// after the fixed-size header, in each direction as needed, simplest-
// correct-thing-first, not yet chunked for buffers over kMaxBufferBytes
// (large single texture uploads could exceed this; a real, flagged
// limitation for follow-on work once actual Roblox texture sizes are
// observed, checked at now).

namespace stud::render_host {

std::string default_socket_path();

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

    // GLES2, state/scalar
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

    // GLES2, string in/out
    GlGetString,
    GlGetUniformLocation,
    GlBindAttribLocation,
    GlShaderSource,
    GlGetProgramInfoLog,
    GlGetShaderInfoLog,
    GlGetActiveUniform,

    // GLES2, fixed-count-N id arrays
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
    // Real GLES3 sync objects + image copy, libroblox calls all of
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

    // GLES2, small out-param arrays (count derived from pname)
    GlGetIntegerv,
    GlTexParameterfv,
    GlGetProgramiv,
    GlGetShaderiv,

    // GLES2, bulk buffer transfer
    GlBufferData,
    GlBufferSubData,
    GlTexImage2D,
    GlTexSubImage2D,
    GlCompressedTexImage2D,
    GlCompressedTexSubImage2D,
    GlReadPixels,

    // Vulkan (narrow surface-creation interposition only; see this
    // header's own doc comment)
    VkCreateWaylandSurfaceForAndroidSurface,

    // ANativeWindow. Process C owns the real window entirely (see
    // android-glue/src/native_window.cpp, excluded from Process B's own
    // bionic build for the same glibc-only-Wayland reason ANGLE itself
    // is). Confirmed, live, this session: libroblox.so directly
    // references these (an eager/data-bound import, same class as
    // AMediaFormat_delete), not just android-glue's own old resolver
    // table; real, not speculative.
    ANativeWindowFromSurface,
    ANativeWindowGetWidth,
    ANativeWindowGetHeight,
    ANativeWindowAcquire,
    ANativeWindowRelease,

    // Real host->client input transport. Process C owns the compositor
    // connection and therefore the real wl_seat; Process B owns the JNI
    // bridge into libroblox's own `NativeInputInterface`. The protocol is
    // strictly client-initiated, so Process B asks this and the host
    // replies with however many queued `HostInputEvent`s fit.
    //
    // a[0] is how long the host may WAIT for an event before answering
    // empty, in milliseconds. 0 keeps the original behaviour (answer with
    // whatever is queued right now). A wait costs nothing and removes a
    // whole polling interval from the cursor's latency: the reply leaves
    // the moment the compositor delivers the event, instead of on the
    // next tick of a timer. Only ever asked on input's own connection,
    // waiting on the shared one would park every GL call behind it.
    PollInputEvents,

    // Real window size, from the process that owns the actual window.
    // Process B has its own android-glue copy whose size is only ever
    // updated by a compositor configure it never receives, so it used to
    // guess, three different sizes existed for one window. Returns
    // (width << 32) | height.
    // Real GLES3 indexed buffer binding. glBindBufferRange was already
    // implemented but its far more common sibling was not, and it is
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
    // Together these give real dots-per-inch, output pixels divided by
    // output millimetres, which is what DisplayMetrics.xdpi/ydpi
    // actually mean and what Stud used to synthesise from its density
    // guess.
    // Returns (px_w << 48) | (px_h << 32) | (mm_w << 16) | mm_h. 16 bits
    // each is genuinely enough: no real display is 65536 pixels or 65
    // metres across. All four are 0 when no compositor reported them,
    // an honest "unknown", not a value to invent around.
    GetDisplayOutputGeometry,
    // Real HiDPI scale in effect for Stud's own window, in 120ths
    // (120 = 1x, 150 = 1.25x, 240 = 2x), the unit Wayland's own
    // fractional-scale protocol uses, and the only one that can express a
    // fractionally-scaled desktop. Process B divides it by 120 to get
    // Android's DisplayMetrics density, which is exactly the same
    // quantity: how many buffer pixels one UI unit occupies.
    GetWindowBufferScale,

    // Real audio output. The engine's own FMOD initialises an Android
    // audio device at game start, and with no device at all it fails,
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
    // paces the client's feeder thread, there is no separate clock.
    AudioWriteFrames,
    // Close: args[0] is the stream handle.
    AudioCloseStream,

    // Vulkan, instance level. Roblox resolves every Vulkan command by
    // name through vkGetInstanceProcAddr (never a second dlsym), so the
    // client hands back a real function per name and forwards it here,
    // where the real driver lives: Process C, never Process B, exactly
    // as the GL path already works. That isolation is the point: the
    // vendor driver never runs in a process sharing bionic or foreign
    // TLS.
    //
    // These four are the real, live-observed first commands the engine
    // asks for (captured with STUD_VULKAN_CALL_TRACE=1), checked.
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
    // token; Process B never dereferences one).
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
    // ships the bytes over on flush/unmap, the same shape the GL path
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
    // reply-free path, which is what makes Vulkan cheaper to forward
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
    // the URL, the title and the engine's own cookies, one per line,
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
    // A focused TextBox stops drawing its own text on Android, the
    // engine sets an internal "a native widget is showing my text" flag
    // and substitutes an empty string until focus is lost, because a
    // real device lays an Android EditText over the GL view. Process C
    // owns the window, so it owns that widget too. The in-buffer carries
    // the text as UTF-8; the args carry the box, the font and the caret.
    // See stud/text_overlay.h and the text-input entry in
    // the engineering notes.
    SetTextOverlay,
    // The real system clipboard, for the text box Stud draws itself.
    // Copy and paste belong to the platform's text widget, an Android
    // EditText owns them on a device, and only the process holding the
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
    // this process, the only unsandboxed one still alive, does the
    // handoff, by running stud-ui in a one-shot mode.
    //
    // StoreSecret's in-buffer is "<name>\n<value>"; LoadSecret's is the
    // name alone and the value comes back in the out-buffer. Values
    // travel on the helper's stdin/stdout, never argv or the
    // environment, because /proc/<pid>/cmdline and /proc/<pid>/environ
    // are readable by anything running as this user.
    StoreSecret,
    LoadSecret,
    // Forget one. The in-buffer is the name, like LoadSecret's. This is
    // what a logout needs: the engine clears the session cookie and the
    // stored copy has to go with it, or the next launch retries a
    // credential the server has already revoked.
    DeleteSecret,
    // Many recorded commands in one request.
    //
    // A command's wire header is 88 bytes, eight argument slots and the
    // lengths, while a draw's payload is sixteen. At ten thousand
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
    // to leave as a no-op even so, on a tiler (and Zink over a tiler)
    // it is the difference between discarding a render target and
    // resolving it every frame.
    GlInvalidateFramebuffer,
    // Resolves the multisampled colour buffer into the framebuffer that
    // is actually presented, so on the OpenGL path nothing reaches the
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
    // Process B decide to quit and then run its ordinary teardown:
    // LeaveGame, DestroyApp, while the engine, already back on its app
    // shell, went on rendering it. The result was the home screen visibly
    // flashing up for half a second on the way out, which is not what the
    // setting promises.
    //
    // This process never replies: it exits inside the handler, so the
    // caller sees the connection drop rather than an answer, which is
    // exactly what it wants, the window is gone the moment the decision
    // is made and Process B tears down behind a closed window.
    EndSession,
    // One message from a web view's page to the engine, or nothing.
    //
    // A Roblox page inside a web view talks to its host through a
    // JavaScript bridge, the real Android client exposes
    // `__globalRobloxAndroidBridge__.executeRoblox(json)`, and that is
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
    // panel, a login challenge that has just been answered, for
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
    // an input stream, never at startup, a[0] is the sample rate and
    // a[1] the channel count; returns 1 when it opened. Appended last:
    // every existing id keeps its value.
    AudioOpenInputStream,
    // Reads what has been captured since the last call into the
    // out-buffer, returning the number of bytes written. Never blocks.
    AudioReadFrames,
    AudioCloseInputStream,
    // Immutable buffer storage (GL_EXT_buffer_storage). a: target, size,
    // flags; the in-buffer carries the initial contents when there are
    // any.
    //
    // The engine only started calling this once Stud stopped truncating
    // the extension string at 511 bytes, GL_EXT_buffer_storage sits
    // past that cutoff, so it had never been offered before. Without a
    // real implementation the buffer is never allocated and the first
    // map of it crashes ANGLE. Appended last: every existing id keeps
    // its value.
    GlBufferStorage,
    // GL timer queries (GLES3 core + GL_EXT_disjoint_timer_query).
    //
    // Without these the engine's own MicroProfiler reports GPU 0.00ms.
    // Every query resolved to a no-op stub and answered zero, so the
    // engine has no idea how long the GPU is taking and its adaptive
    // work scheduling runs blind. Appended last: every existing id keeps
    // its value.
    GlGenQueries,
    GlDeleteQueries,
    GlBeginQuery,
    GlEndQuery,
    GlGetQueryObjectuiv,
    GlGetQueryObjectui64v,
    // Many pipelined calls in one request.
    //
    // A reply-free call still carried a full Header, call id, eight
    // 64-bit arguments, a PBO offset, flags and two lengths, about 100
    // bytes, for calls that mostly use two or three arguments. On the
    // OpenGL path the engine issues around 17,700 of them per frame, so
    // that is ~1.7MB of header per frame to build, copy and write.
    //
    // A batch writes one Header and then packs each call as: argc, a
    // flags byte, the 16-bit call id, argc arguments, and only the
    // length/PBO fields that are actually used. A typical GL call comes
    // to 12-28 bytes instead of ~100. Appended last: every existing id
    // keeps its value.
    GlCommandBatch,
    // A URL the web view was about to navigate to and did NOT, because
    // the app has first refusal on it, exactly what a real device's
    // WebView does (`shouldOverrideUrlLoading` returns true for every
    // URL, then routes it through the LINKING protocol). Returns the
    // number of bytes written to the out-buffer, 0 when the queue is
    // empty. Appended last: every existing id keeps its value.
    PollWebViewNavigation,
    // ...and the answer. The in-buffer is the URL; a[0] is 0 for "the
    // engine does not want it, load it after all" and 1 for "the engine
    // took it, drop it", the viewer needs the second because it gives
    // an unanswered question a short deadline and then loads the page
    // itself rather than leaving a dead link.
    WebViewLoadUrl,
    // Put the in-buffer's text on the system clipboard.
    //
    // "Copy link" in an experience's invite dialog publishes
    // `ExternalContentSharing.setClipboardText` and expects the platform
    // to do this; nothing in Process B can, since the clipboard belongs
    // to the display server this process is connected to. Appended last:
    // every existing id keeps its value.
    CopyToClipboard,
    // Keep the pointer inside the window for the duration of a camera
    // drag, a[0] != 0 to confine. Not a lock: the pointer keeps its real
    // position and its ordinary motion, so the engine's cursor is driven
    // exactly as it is when nothing is constrained; it simply cannot
    // leave the window, and relative motion keeps arriving at the
    // boundary so a spin does not stop there. Appended last: every
    // existing id keeps its value.
    SetPointerConfined,
    // Put the pointer at a point in the window, a[0]/a[1] in surface
    // pixels as 8.8 fixed point. Used once per camera drag, at the end:
    // the engine's cursor did not move for the gesture, so the pointer is
    // returned to it. Appended last: every existing id keeps its value.
    WarpPointer,
    // Whether WarpPointer can do anything: 1 when the compositor supports
    // it (X11 always does), 0 otherwise. Appended last: every existing id
    // keeps its value.
    CanWarpPointer,
    // A deep link that arrived while Stud was already running.
    //
    // Clicking a game in a browser starts a SECOND stud-ui. It used to
    // find the lock held and answer with "Stud is already running,
    // close the existing window", which is the wrong answer to someone
    // who just asked to play something. That process cannot reach the
    // engine itself (it is a fresh process; Process B is inside its own
    // sandbox), but it CAN reach render-host, which both of them already
    // talk to. So it hands the link over here and exits.
    //
    // The in-buffer is the URI, not NUL-terminated. Appended last: every
    // existing id keeps its value.
    DeliverDeepLink,
    // Drains one queued deep link into the out-buffer, returning its
    // length, or 0 when there is none. Same shape as PollWebViewMessage,
    // and polled by Process B beside it. Appended last: every existing id
    // keeps its value.
    PollDeepLink,
    // Appended last, so every id above keeps its value.
    //
    // A mapped allocation the host maps too, by the same file the client
    // backs it with: the engine's writes land in pages both processes
    // see, so a flush only has to name the run that changed.
    VkShareMappedMemory,
    VkWriteSharedMappedMemory,

    // The other direction: what the HOST's copy of a mapped allocation
    // holds right now.
    //
    // Only needed where the host could not import the engine's own pages.
    // When it can, the GPU writes straight into memory the engine has
    // mapped and a readback is simply there -- which is every NVIDIA
    // session, and is why this was missing. AMD refuses the import
    // outright (amdgpu's userptr is ANONONLY and Stud's mapping is
    // file-backed), so every host-visible allocation is copied there, and
    // vkCmdCopyImageToBuffer had no path back at all: the copy ran and
    // the engine read whatever was already in its own pages.
    VkReadMappedMemory,

    // Whether the user has closed the window.
    //
    // Process B has to hear about this rather than simply losing its
    // socket: leaving a running experience is a real message to a real
    // Roblox server, and the engine only sends it if something calls
    // nativeAppBridgeV2LeaveGame. render-host exiting the instant the
    // titlebar X is clicked took the connection away first, so the
    // teardown never ran and the account stayed in the server until it
    // timed out.
    PollWindowCloseRequested,

    // GLES3 entry points the engine resolves by name and used to receive a
    // do-nothing function for; see the host's handlers for the wire shapes.
    GlTexImage3D,
    GlCompressedTexImage3D,
    GlCompressedTexSubImage3D,
    GlFramebufferTextureLayer,
    GlGetInteger64v,
    GlQueryCounter,
    GlGetQueryiv,
    GlGetQueryObjectiv,
    GlProgramBinary,
    GlGetProgramBinary,
    GlClearBufferiv,
    GlClearBufferuiv,
    GlClearBufferfi,
    GlPushGroupMarker,
    GlPopGroupMarker,
    GlObjectLabel,
    GlGetBufferParameteriv,

    // The output device's underrun count since it opened, for
    // AAudioStream_getXRunCount. Returned as the call's result.
    AudioGetUnderruns,

    // Video decoding for the engine's AMediaCodec; see video_codec.h.
    // Supported: in = MIME, NUL-terminated; result 1/0.
    // Create:    in = MIME; result = decoder handle, 0 for none.
    // Configure: a[0] decoder, a[1] width, a[2] height; in = csd bytes.
    // Queue:     a[0] decoder, a[1] pts (us), a[2] end of stream; in = data.
    // Dequeue:   a[0] decoder; out = VideoFrameHeader + NV12.
    VideoDecoderSupported,
    VideoDecoderCreate,
    VideoDecoderConfigure,
    VideoDecoderQueue,
    VideoDecoderDequeue,
    VideoDecoderFlush,
    VideoDecoderDestroy,

    // Video encoding; see video_codec.h.
    // Supported: in = MIME; result bit 0 supported, bit 1 hardware.
    // Create:    in = MIME; result = encoder handle, 0 for none.
    // Configure: a[0] encoder, a[1] width, a[2] height, a[3] bit rate,
    //            a[4] frame rate x1000, a[5] key interval (ms), a[6] colour format.
    // Queue:     a[0] encoder, a[1] pts (us), a[2] end of stream; in = raw frame.
    // Dequeue:   a[0] encoder; out = EncodedPacketHeader + bytes.
    // Config:    a[0] encoder; out = the codec configuration bytes.
    VideoEncoderSupported,
    VideoEncoderCreate,
    VideoEncoderConfigure,
    VideoEncoderQueue,
    VideoEncoderDequeue,
    VideoEncoderConfig,
    VideoEncoderDestroy,

    // Turns the connection it arrives on into the shared-memory fd
    // channel, for the rest of that connection's life. After it, each
    // message from the client is an 8-byte allocation id carrying one
    // memfd as SCM_RIGHTS, and the host answers each with one byte (1 =
    // kept). The fd is then named by that id in VkAllocateMemory's a[4] and
    // VkShareMappedMemory's a[2]. Only ever sent on a connection of its own,
    // never on one that carries other calls: ancillary data cannot ride a
    // stream that other threads write into.
    SharedMemoryFdChannel,
};

// What VideoEncoderDequeue returns ahead of a packet's bytes. `flags` are
// Android's buffer flags: 1 key frame, 2 codec configuration.
struct EncodedPacketHeader {
    int64_t pts_us;
    uint32_t flags;
    uint32_t size;
};

// What VideoDecoderDequeue returns ahead of a frame's NV12 bytes: the Y
// plane, `stride` bytes a row for `slice_height` rows, then interleaved UV
// at the same stride for half as many. The visible rectangle is the crop,
// inclusive, as Android's crop-left/top/right/bottom keys describe it.
struct VideoFrameHeader {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t slice_height;
    uint32_t crop_left;
    uint32_t crop_top;
    uint32_t crop_right;
    uint32_t crop_bottom;
    int64_t pts_us;
    uint32_t data_size;
    uint32_t reserved;
};
// Every CallId's own name, for diagnostics; STUD_IPC_TOP used to print
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
        "DeleteSecret",
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
        "GlBufferStorage",
        "GlGenQueries",
        "GlDeleteQueries",
        "GlBeginQuery",
        "GlEndQuery",
        "GlGetQueryObjectuiv",
        "GlGetQueryObjectui64v",
        "GlCommandBatch",
        "PollWebViewNavigation",
        "WebViewLoadUrl",
        "CopyToClipboard",
        "SetPointerConfined",
        "WarpPointer",
        "CanWarpPointer",
        "DeliverDeepLink",
        "PollDeepLink",
        "VkShareMappedMemory",
        "VkWriteSharedMappedMemory",
        "VkReadMappedMemory",
        "PollWindowCloseRequested",
        "GlTexImage3D",
        "GlCompressedTexImage3D",
        "GlCompressedTexSubImage3D",
        "GlFramebufferTextureLayer",
        "GlGetInteger64v",
        "GlQueryCounter",
        "GlGetQueryiv",
        "GlGetQueryObjectiv",
        "GlProgramBinary",
        "GlGetProgramBinary",
        "GlClearBufferiv",
        "GlClearBufferuiv",
        "GlClearBufferfi",
        "GlPushGroupMarker",
        "GlPopGroupMarker",
        "GlObjectLabel",
        "GlGetBufferParameteriv",
        "AudioGetUnderruns",
        "VideoDecoderSupported",
        "VideoDecoderCreate",
        "VideoDecoderConfigure",
        "VideoDecoderQueue",
        "VideoDecoderDequeue",
        "VideoDecoderFlush",
        "VideoDecoderDestroy",
        "VideoEncoderSupported",
        "VideoEncoderCreate",
        "VideoEncoderConfigure",
        "VideoEncoderQueue",
        "VideoEncoderDequeue",
        "VideoEncoderConfig",
        "VideoEncoderDestroy",
        "SharedMemoryFdChannel",
    };
    static_assert(sizeof(kNames) / sizeof(kNames[0]) ==
                      static_cast<size_t>(CallId::SharedMemoryFdChannel) + 1,
                  "a CallId was added without its name, append it to kNames");
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
    // that buffer, not a client-side pointer, so it must NOT be dereferenced
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
// module's own test client, owns exactly one blocking, synchronous
// request/response round-trip.
//
// Bug found in testing (the engineering notes): this doc comment used
// to say "no connection pooling/threading" as if that were a documented
// constraint on the *caller*, but render_client_common.cpp's own
// connection() returns one process-wide, shared Client instance, and
// several real call sites (run_bounded_v2_call's own detached background
// threads for InitWithParams/StartAppWithParams/StartGameWithParam/etc.)
// can genuinely call into GL/EGL concurrently, each forwarding over this
// exact same socket. With no serialization, two threads' requests (or a
// request and a response) can interleave on the wire, the protocol
// has no per-call ID to resync with, so a reply meant for thread A can
// get consumed by thread B's read_all(), leaving A blocked in read_all()
// forever waiting for a reply that already went to someone else. Live-
// confirmed via a live syscall trace: a real, freshly-spawned worker thread for
// `nativeAppBridgeV2StartApp` sent one real request/response pair over
// this socket then blocked on a futex indefinitely, with no
// corresponding response ever unblocking it, consistent with exactly
// this race, not a hang inside Roblox's own code. Fixed by serializing
// the whole round-trip with a mutex, so `call()` is atomic with respect
// to other threads sharing the same Client.
// STUD_IPC_STATS: round-trip counts and wall time per frame. Read once
// rather than once per method that reports them.
inline bool ipc_stats_enabled() {
    static const bool on = std::getenv("STUD_IPC_STATS") != nullptr;
    return on;
}

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
        // once, and the engine does exactly that, but every one of those
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
        // much wall time they take. Every call here is synchronous, write a
        // request, block for a reply, so this is the number that decides
        // Stud's frame rate ceiling, and it should be measured before the
        // protocol is changed to chase it.
        const bool stats = ipc_stats_enabled();
        std::chrono::steady_clock::time_point call_start;
        if (stats) call_start = std::chrono::steady_clock::now();
        // STUD_IPC_TOP=1: which call ids the round-trips actually go to.
        // "IPC is expensive" is not actionable; "one call id is 90% of the
        // round-trips" says exactly what to make reply-free or cache.
        emit_command_batch_locked();
        // Everything queued reply-free so far has to reach the host
        // before this call's answer is asked for, or the answer would be
        // computed against state that has not been applied yet.
        emit_gl_batch_locked();
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
        // Single-writer: this request takes its place in the queue like
        // everything else, and the caller waits for ITS OWN reply.
        //
        // Anything queued ahead still reaches the host first -- that is
        // what the queue is -- but the caller no longer has to drive it
        // there, and nothing queued behind it makes anyone wait. This is
        // the path that replaced draining a second producer.
        if (writer_started_) {
            Completion c;
            c.out = out_buffer;
            c.cap = out_capacity;
            const size_t at = queue_.size();
            queue_.resize(at + sizeof(hdr) + in_len);
            std::memcpy(queue_.data() + at, &hdr, sizeof(hdr));
            if (in_len > 0 && in_buffer != nullptr) {
                std::memcpy(queue_.data() + at + sizeof(hdr), in_buffer, in_len);
            }
            marks_.push_back(Mark{queue_.size(), nullptr, &c});
            // Handing the queue over before waiting: the writer needs the
            // lock this call is holding.
            call_mutex_.unlock();
            writer_wake_.notify_one();
            {
                std::unique_lock<std::mutex> wait_lock(c.m);
                c.cv.wait(wait_lock, [&c] { return c.done; });
            }
            call_mutex_.lock();
            if (out_len_written != nullptr) *out_len_written = c.written;
            return c.failed ? kNullHandle : c.result;
        }

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

    // For diagnostics only: lets a client say which socket it got,
    // so a host-side connection can be matched to the library that
    // opened it.
    int fd() const { return fd_; }

private:
    // User-reported bug, fixed: `connected()` used to just report
    // whether connect_to() ever succeeded, never whether the connection
    // was still alive, so a client had no way to notice stud-render-
    // host had exited (e.g. the user closing the real window) short of
    // every individual call() already having failed. A failed write/read
    // here now marks the connection dead for real, so connected()
    // reflects live state and a caller (process-b's own render loop) can
    // treat "render-host is gone" as its own real shutdown signal.
    void mark_dead() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    // Bug found in testing: neither of these retried on EINTR. Process B
    // is a heavily-signalled process (trap_recovery's own SIGSEGV/SIGTRAP
    // handling, plus whatever timers libroblox arms), and a single
    // interrupted read()/write() here permanently killed the render
    // connection, silently, since mark_dead() said nothing and every
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
        // The render host going away IS the shutdown, and this is the
        // earliest anything in this process learns of it, earlier than
        // the main loop, which only looks every 250ms and was still
        // sleeping while the engine's own threads faulted on the same
        // dead socket.
        //
        // Resolved rather than called directly: this header is compiled
        // into five separate .so files as well as the executable that
        // defines the function, so the only way they can all reach the
        // one real copy is by name at runtime. Absent (render-host's own
        // process, which has no such symbol) it simply does nothing.
        using NoteFn = void (*)();
        static const NoteFn note =
            reinterpret_cast<NoteFn>(::dlsym(RTLD_DEFAULT, "stud_note_shutting_down"));
        if (note != nullptr) note();
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
    // round-trips. Ordering is preserved. It is one stream socket, and
    // anything that does need an answer flushes this queue first.
    // Appends one recorded command to the current batch. Same stream, same
    // order, the batch is emitted into the send queue before anything
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
        const bool stats = ipc_stats_enabled();
        std::chrono::steady_clock::time_point t0;
        if (stats) t0 = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(call_mutex_);
        if (fd_ < 0) return;
        reserve_queue_once();
        if (!command_batch_.empty()) emit_command_batch_locked();

        // Built straight into the queue rather than on the stack and
        // copied in.
        //
        // This runs about 17,700 times a frame on the OpenGL path, the
        // engine's own call rate, so what it does per call is the
        // frame budget. The old version zero-initialised a ~100-byte
        // header (`Header hdr{}`), filled every field, then copied the
        // whole thing into the queue through an iterator-range insert.
        // That is two passes over the header and one redundant clear of
        // bytes that are all about to be overwritten.
        // Only the arguments that carry anything. The host zero-fills
        // the rest, so an argument that is legitimately zero past the
        // last non-zero one still arrives as zero.
        uint8_t argc = 8;
        while (argc > 0 && args[argc - 1] == 0) --argc;

        const bool has_payload = in_len > 0 && in_buffer != nullptr;
        const bool has_pixel_offset = pixel_buffer_offset_plus_one != 0;
        const size_t record = 4 + static_cast<size_t>(argc) * sizeof(uint64_t) +
                              (has_payload ? sizeof(uint32_t) + in_len : 0) +
                              (has_pixel_offset ? sizeof(uint64_t) : 0);

        const size_t at = batch_.size();
        batch_.resize(at + record);
        uint8_t* p = batch_.data() + at;
        *p++ = argc;
        *p++ = static_cast<uint8_t>((has_payload ? 1u : 0u) | (has_pixel_offset ? 2u : 0u));
        const auto id16 = static_cast<uint16_t>(id);
        std::memcpy(p, &id16, sizeof(id16));
        p += sizeof(id16);
        if (argc > 0) {
            std::memcpy(p, args, static_cast<size_t>(argc) * sizeof(uint64_t));
            p += static_cast<size_t>(argc) * sizeof(uint64_t);
        }
        if (has_payload) {
            std::memcpy(p, &in_len, sizeof(in_len));
            p += sizeof(in_len);
            std::memcpy(p, in_buffer, in_len);
            p += in_len;
        }
        if (has_pixel_offset) {
            std::memcpy(p, &pixel_buffer_offset_plus_one, sizeof(pixel_buffer_offset_plus_one));
        }
        // STUD_IPC_NO_PIPELINE=1 sends every request immediately, i.e. the
        // pre-pipelining behaviour. Kept as a one-switch A/B for exactly the
        // kind of "did batching starve something that was waiting on it"
        // question that is otherwise guesswork.
        static const bool no_pipeline = std::getenv("STUD_IPC_NO_PIPELINE") != nullptr;
        if (no_pipeline || batch_.size() >= kQueueFlushBytes) {
            emit_gl_batch_locked();
            hand_off_locked();
        }
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
    // it holds the connection for a whole round-trip, and every GL call the
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
        emit_gl_batch_locked();
        return hand_off_locked();
    }

private:
    static constexpr size_t kQueueFlushBytes = 256u * 1024u;
    static constexpr size_t kCommandBatchBytes = 32u * 1024u;

    // Moves the accumulated commands into the send queue as one request.
    // Packs whatever reply-free calls have accumulated into one request.
    // Called before anything that must be ordered after them, a call
    // that waits for a reply, or a flush.
    void emit_gl_batch_locked() {
        if (batch_.empty()) return;
        const size_t at = queue_.size();
        queue_.resize(at + sizeof(Header) + batch_.size());
        auto* hdr = reinterpret_cast<Header*>(queue_.data() + at);
        std::memset(hdr, 0, sizeof(*hdr));
        hdr->call_id = CallId::GlCommandBatch;
        hdr->flags = Header::kNoReply;
        hdr->in_buffer_len = static_cast<uint32_t>(batch_.size());
        std::memcpy(queue_.data() + at + sizeof(Header), batch_.data(), batch_.size());
        batch_.clear();
    }

    std::vector<uint8_t> batch_;

    void emit_command_batch_locked() {
        if (command_batch_.empty()) return;
        emit_gl_batch_locked();
        Header hdr{};
        hdr.call_id = CallId::VkCmdRecordBatch;
        hdr.flags = Header::kNoReply;
        hdr.in_buffer_len = static_cast<uint32_t>(command_batch_.size());
        hdr.out_buffer_len = 0;
        const auto* hdr_bytes = reinterpret_cast<const uint8_t*>(&hdr);
        queue_.insert(queue_.end(), hdr_bytes, hdr_bytes + sizeof(hdr));
        queue_.insert(queue_.end(), command_batch_.begin(), command_batch_.end());
        command_batch_.clear();
        if (queue_.size() >= kQueueFlushBytes) hand_off_locked();
    }

    std::vector<uint8_t> command_batch_;

    // ---- Single-writer mode -------------------------------------------
    //
    // One thread owns the socket. Everything else only ever appends to
    // `queue_` under `call_mutex_`, so the order bytes reach the host is
    // exactly the order the engine's own threads produced them, and
    // nothing has to wait to find out where it stands in that order.
    //
    // WHY, measured: vkQueueSubmit and vkQueuePresentKHR used to be run
    // by a second thread (DeferredQueue), so the stream had two producers
    // and their relative order was whatever the scheduler chose. Anything
    // that had to be ordered after a submit -- resetting a fence,
    // resetting a command pool, acquiring the next image -- could only get
    // that guarantee by DRAINING that thread first. Sampled in a real
    // game, the engine's Main thread spent 13.1% of its wall clock parked
    // in exactly that drain (futex on DeferredQueue's own condvar), while
    // Process B as a whole used 1.72 of its 12 cores. It was not waiting
    // for work; it was waiting to be told the queue was empty.
    //
    // With one writer that question disappears. A caller that wants an
    // answer waits for ITS OWN reply and nothing else; a caller that does
    // not simply appends and returns.
    //
    // `marks_` is what makes that work without giving up the batching.
    // Bytes keep accumulating in one buffer, and a mark records, by byte
    // offset, a place the writer must stop: either to run an ordered
    // action (the texture decode that has to finish before the submit
    // that reads it) or to read a reply for a caller that is waiting.
    struct Completion {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        bool failed = false;
        uint64_t result = 0;
        void* out = nullptr;
        uint32_t cap = 0;
        uint32_t written = 0;
    };

    struct Mark {
        size_t at = 0;                 // write everything before this offset first
        std::function<void()> action;  // ...then run this, if set
        Completion* completion = nullptr;  // ...then read this reply, if set
    };

public:
    // Turns this connection into a single-writer one. Call once, right
    // after connect_to(), and only for a connection that actually needs
    // it: the audio, input and test clients are each used by one thread
    // and gain nothing from a thread of their own.
    void start_writer() {
        std::lock_guard<std::mutex> lock(call_mutex_);
        if (writer_started_) return;
        writer_started_ = true;
        std::thread([this] {
            writer_thread_ = std::this_thread::get_id();
            writer_loop();
        }).detach();
    }

    bool writer_active() const { return writer_started_; }

    // True on the thread that owns the socket, i.e. inside an action.
    bool on_writer_thread() const {
        return writer_started_ && std::this_thread::get_id() == writer_thread_;
    }

    // Sends a request from INSIDE an action, at the point the action runs.
    //
    // An action runs in the middle of the writer's own batch: the bytes
    // that follow it -- the submit that reads whatever the action just
    // produced -- are already snapshotted and are written the moment the
    // action returns. Anything the action queues normally therefore lands
    // in the NEXT batch, behind that submit, which is the wrong order and
    // silent: the GPU reads the buffer before its contents are named.
    // Live-caught as decoded textures arriving one submit late -- another
    // texture's content in their place -- on any device where the host
    // cannot import the engine's pages and the bytes really have to
    // travel. Where the import works nothing is sent at all, which is why
    // this never showed on the machine it was written on.
    //
    // A blocking call() is not an option here either: it waits for a
    // reply only the writer thread can read, and this IS that thread.
    // So the request goes straight down the socket, reply-free, in the
    // one place where doing that is ordered correctly by construction.
    bool write_inline(CallId id, const uint64_t args[8], const void* in_buffer, uint32_t in_len) {
        if (!on_writer_thread()) return false;
        Header hdr{};
        hdr.call_id = id;
        for (int i = 0; i < 8; ++i) hdr.args[i] = args[i];
        hdr.flags = Header::kNoReply;
        hdr.in_buffer_len = in_len;
        hdr.out_buffer_len = 0;
        if (!write_all(&hdr, sizeof(hdr))) return false;
        if (in_len > 0 && in_buffer != nullptr && !write_all(in_buffer, in_len)) return false;
        return true;
    }

    // Puts a piece of work INTO the stream, to run on the writer thread
    // when everything queued before it has been written and before
    // anything queued after it is.
    //
    // This is what keeps the texture decode off the engine's thread
    // without a second producer: the decode is ordered against the submit
    // that reads its output by being in the same queue as it, rather than
    // by the submitting thread waiting for a different thread to finish.
    void enqueue_action(std::function<void()> fn) {
        if (!fn) return;
        if (!writer_started_) {
            fn();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(call_mutex_);
            if (fd_ < 0) return;
            emit_command_batch_locked();
            emit_gl_batch_locked();
            marks_.push_back(Mark{queue_.size(), std::move(fn), nullptr});
        }
        writer_wake_.notify_one();
    }

private:
    void writer_loop() {
        for (;;) {
            std::vector<uint8_t> bytes;
            std::deque<Mark> marks;
            {
                std::unique_lock<std::mutex> lock(call_mutex_);
                writer_wake_.wait(lock, [this] {
                    return !queue_.empty() || !marks_.empty() || !batch_.empty() ||
                           !command_batch_.empty() || fd_ < 0;
                });
                if (fd_ < 0) {
                    fail_all_pending_locked();
                    return;
                }
                emit_command_batch_locked();
                emit_gl_batch_locked();
                bytes.swap(queue_);
                queue_.clear();
                reserve_queue_once();
                marks.swap(marks_);
            }

            size_t pos = 0;
            bool ok = true;
            for (Mark& m : marks) {
                if (ok && m.at > pos) {
                    ok = write_all(bytes.data() + pos, static_cast<uint32_t>(m.at - pos));
                }
                pos = m.at;
                if (m.action) {
                    // Runs even if the socket has already failed: it owns
                    // real work (a decode) whose buffers something else
                    // may be waiting on.
                    m.action();
                }
                if (m.completion != nullptr) {
                    if (ok) {
                        ok = read_reply_into(*m.completion);
                    }
                    if (!ok) {
                        signal_failed(*m.completion);
                    }
                }
            }
            if (ok && pos < bytes.size()) {
                ok = write_all(bytes.data() + pos, static_cast<uint32_t>(bytes.size() - pos));
            }
            if (!ok) {
                std::lock_guard<std::mutex> lock(call_mutex_);
                mark_dead();
                fail_all_pending_locked();
                return;
            }
        }
    }

    bool read_reply_into(Completion& c) {
        ResponseHeader resp{};
        if (!read_all(&resp, sizeof(resp))) return false;
        if (resp.out_buffer_len > 0) {
            const uint32_t to_copy =
                resp.out_buffer_len < c.cap ? resp.out_buffer_len : c.cap;
            if (c.out != nullptr && to_copy > 0) {
                if (!read_all(c.out, to_copy)) return false;
                if (resp.out_buffer_len > to_copy) drain(resp.out_buffer_len - to_copy);
            } else {
                drain(resp.out_buffer_len);
            }
        }
        {
            std::lock_guard<std::mutex> lock(c.m);
            c.result = resp.result;
            c.written = resp.out_buffer_len;
            c.done = true;
        }
        c.cv.notify_one();
        return true;
    }

    static void signal_failed(Completion& c) {
        {
            std::lock_guard<std::mutex> lock(c.m);
            c.result = kNullHandle;
            c.written = 0;
            c.failed = true;
            c.done = true;
        }
        c.cv.notify_one();
    }

    // Nobody may be left waiting on a reply that can no longer come: a
    // dead render host has to surface as a failed call, not a hang.
    void fail_all_pending_locked() {
        for (Mark& m : marks_) {
            if (m.completion != nullptr) signal_failed(*m.completion);
        }
        marks_.clear();
    }

    // Gets what is queued moving, by whichever means this connection uses:
    // waking the writer thread, or writing it here on the caller's own
    // thread when there is no writer (audio, input, the test client).
    //
    // Never waits for the bytes to land. A caller that needs them landed
    // is a caller asking a question, and call() waits for its own answer.
    bool hand_off_locked() {
        if (writer_started_) {
            writer_wake_.notify_one();
            return fd_ >= 0;
        }
        return flush_locked();
    }

    bool flush_locked() {
        if (queue_.empty()) return true;
        const bool ok = write_all(queue_.data(), static_cast<uint32_t>(queue_.size()));
        // clear(), never shrink: the capacity earned on the first frame
        // is what keeps the next one from reallocating. A frame's worth
        // of commands is a stable size, so this settles immediately.
        queue_.clear();
        return ok;
    }

    // Sized once, for a whole flush window, so appending a command never
    // grows the buffer. Without this the queue reallocates its way up to
    // the flush threshold repeatedly, copying everything already queued
    // each time, on a path that queues 17,700 commands a frame.
    void reserve_queue_once() {
        if (queue_.capacity() >= kQueueFlushBytes + 64u * 1024u) return;
        queue_.reserve(kQueueFlushBytes + 64u * 1024u);
    }

    std::vector<uint8_t> queue_;
    std::deque<Mark> marks_;
    std::condition_variable writer_wake_;
    bool writer_started_ = false;
    // Which thread runs writer_loop(), so an action can tell that it is
    // on it; see write_inline().
    std::thread::id writer_thread_{};
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
