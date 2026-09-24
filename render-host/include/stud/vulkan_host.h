// Host-side handlers for the forwarded Vulkan commands. See
// vulkan_host.cpp and vulkan_forward.h for the rationale.
#ifndef STUD_VULKAN_HOST_H
#define STUD_VULKAN_HOST_H

#include <cstdint>
#include <string>
#include <vector>

namespace stud::render_host {

// Each returns the real VkResult (sign-extended into the protocol's
// uint64 result field) and fills `out`/`out_len` with the reply body.
// Tells the Vulkan layer the size of the one real window this process
// owns. Must be called before any surface query: Wayland reports an
// undefined currentExtent and this is the honest answer to substitute.
void vk_set_window_size(uint32_t width, uint32_t height);

// How much bigger the PRESENTED image is than the one the engine renders,
// in 120ths. 120 is 1:1 and disables the whole path.
//
// This is Stud's own upscaler, and it exists because the engine's own
// scale cannot be touched: below 1.0 it switches to a simplified UI with
// square corners, and above 1.0 it drops SSAO, both measured. So the
// engine is left believing its window is exactly the size it asked for,
// at scale 1.0, and renders into an offscreen image of that size; Stud
// presents a full-resolution image built from it. The engine learns
// nothing, so its UI size, its rounded corners, its SSAO and the cursor
// are all untouched.
//
// The ratio is the display's own scale in the non-HiDPI configuration,
// where the engine already renders at the window's logical size and the
// compositor stretches the result. This replaces that stretch with a
// real pass, at exactly the performance non-HiDPI already has.
// The size Stud's upscaler writes, in the display's own pixels. Always
// the window's real size, so the presented image matches the screen
// exactly and is shown 1:1, and it follows a resize. 0x0 disables the
// whole path.
void vk_set_upscale_output_size(uint32_t width, uint32_t height);

// How hard the upscaler's sharpening pass pulls, 0-100. Read when the
// pass is recorded, so it applies from the next swapchain onwards.

// Whether the pass may use its compute shaders (EASU/RCAS) or must stay a
// plain scaled blit.
//
// Both are the same pass; only the filter differs. X11 needs the blit
// even with upscaling switched off, because there is no compositor there
// to scale the engine's buffer up to the window, which is a job Wayland
// does for free. Defaults to true, which is every existing caller.
void vk_set_upscale_use_compute(bool use_compute);

// Whether the window is an X11 one, which decides whether instance
// creation asks for VK_KHR_xlib_surface or VK_KHR_wayland_surface.
void vk_set_on_x11(bool on_x11);

// Whether this process offers Vulkan at all. The user's own graphics-mode
// setting decides it: choosing OpenGL means Stud does not provide Vulkan,
// and vkCreateInstance then answers VK_ERROR_INCOMPATIBLE_DRIVER, the
// same answer a device with no Vulkan driver gives. That is an honest
// report of what Stud is offering, not a spoof, and it needs no FFlag
// (see flag_overrides.h: overrides come only from the hand-edited file)
// and no knowledge of any Roblox build's internals.
void vk_set_vulkan_enabled(bool enabled);

// The GPU chosen in Settings, as an index into vkEnumeratePhysicalDevices'
// own order. On the Vulkan path it moves that device to the front of the
// list the engine enumerates; nothing is hidden, the preference is simply
// expressed as order. Setting it does nothing if the index is out of
// range, which is what a saved choice for a GPU that is no longer present
// should do.
void vk_set_preferred_device_index(uint32_t index);

// Translates that same index into the "vendorId:deviceId" token Mesa's
// MESA_VK_DEVICE_SELECT expects, by asking the real loader what is at
// that position. Empty when it cannot be determined. Used on the OpenGL
// path, where Zink, not the engine, is the one choosing a GPU.
std::string vk_device_select_token_for_index(uint32_t index);

uint64_t vk_enumerate_instance_version(std::vector<uint8_t>& out, uint32_t* out_len);
uint64_t vk_enumerate_instance_extension_properties(const std::vector<uint8_t>& in,
                                                     uint32_t capacity,
                                                     std::vector<uint8_t>& out, uint32_t* out_len);
uint64_t vk_enumerate_instance_layer_properties(uint32_t capacity, std::vector<uint8_t>& out,
                                                 uint32_t* out_len);
uint64_t vk_create_instance(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                             uint32_t* out_len);

// Physical-device level. `device` is the host's own real VkPhysicalDevice,
// travelling as an opaque token. `capacity` follows the real two-call
// idiom: 0 means "just report the count".
uint64_t vk_enumerate_physical_devices(uint32_t capacity, std::vector<uint8_t>& out,
                                        uint32_t* out_len);
uint64_t vk_get_physical_device_properties(uint64_t device, std::vector<uint8_t>& out,
                                            uint32_t* out_len);
uint64_t vk_get_physical_device_features(uint64_t device, std::vector<uint8_t>& out,
                                          uint32_t* out_len);
uint64_t vk_get_physical_device_memory_properties(uint64_t device, std::vector<uint8_t>& out,
                                                   uint32_t* out_len);
uint64_t vk_get_physical_device_queue_family_properties(uint64_t device, uint32_t capacity,
                                                         std::vector<uint8_t>& out,
                                                         uint32_t* out_len);
uint64_t vk_enumerate_device_extension_properties(uint64_t device, const std::vector<uint8_t>& in,
                                                   uint32_t capacity, std::vector<uint8_t>& out,
                                                   uint32_t* out_len);

// pNext-carrying commands. `node_count` is how many flattened chain
// nodes the in-buffer holds.
uint64_t vk_get_physical_device_features2(uint64_t device, const std::vector<uint8_t>& in,
                                           uint32_t node_count, std::vector<uint8_t>& out,
                                           uint32_t* out_len);
uint64_t vk_create_device(uint64_t physical_device, const std::vector<uint8_t>& in,
                           std::vector<uint8_t>& out, uint32_t* out_len);

uint64_t vk_get_physical_device_format_properties(uint64_t device, uint32_t format,
                                                   std::vector<uint8_t>& out, uint32_t* out_len);
uint64_t vk_get_physical_device_image_format_properties(uint64_t device, uint32_t format,
                                                         uint32_t type, uint32_t tiling,
                                                         uint32_t usage, uint32_t flags,
                                                         std::vector<uint8_t>& out,
                                                         uint32_t* out_len);

// Device level. The VkDevice is the one this process created, held by
// the loader, so it does not travel on every call.
uint64_t vk_get_device_queue(uint32_t family, uint32_t index, std::vector<uint8_t>& out,
                              uint32_t* out_len);
uint64_t vk_create_command_pool(uint32_t flags, uint32_t family, std::vector<uint8_t>& out,
                                 uint32_t* out_len);
uint64_t vk_create_semaphore(uint32_t flags, std::vector<uint8_t>& out, uint32_t* out_len);
uint64_t vk_create_fence(uint32_t flags, std::vector<uint8_t>& out, uint32_t* out_len);
uint64_t vk_create_query_pool(uint32_t flags, uint32_t query_type, uint32_t query_count,
                               uint32_t pipeline_statistics, std::vector<uint8_t>& out,
                               uint32_t* out_len);
uint64_t vk_create_pipeline_cache(uint32_t flags, const std::vector<uint8_t>& in,
                                   std::vector<uint8_t>& out, uint32_t* out_len);
uint64_t vk_get_pipeline_cache_data(uint64_t cache, uint32_t capacity, std::vector<uint8_t>& out,
                                     uint32_t* out_len);
uint64_t vk_destroy_pipeline_cache(uint64_t cache);
uint64_t vk_create_image(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                          uint32_t* out_len);
uint64_t vk_get_image_memory_requirements(uint64_t image, std::vector<uint8_t>& out,
                                           uint32_t* out_len);
uint64_t vk_get_physical_device_surface_capabilities(uint64_t physical_device, uint64_t surface,
                                                      std::vector<uint8_t>& out,
                                                      uint32_t* out_len);

uint64_t vk_device_wait_idle();

// Whether the real driver has this command (name in the buffer).
uint64_t vk_host_has_proc(const std::vector<uint8_t>& in);
// `shared_id`, when non-zero, names a memfd the client has already mapped
// and handed over the shared-memory fd channel (see register_shared_fd):
// the same pages are imported here as device memory so nothing has to be
// copied between the two processes.
uint64_t vk_allocate_memory(uint64_t size, uint32_t type_index, const std::vector<uint8_t>& in,
                             uint32_t node_count, std::vector<uint8_t>& out, uint32_t* out_len,
                             uint64_t shared_id = 0);
// The shared-memory fd channel's two ends. register_shared_fd() keeps an
// fd the client sent under the id it chose; take_shared_fd() hands it to
// whichever call names that id, exactly once, and -1 if there is none.
// forget_shared_fds() closes whatever was sent and never claimed.
void register_shared_fd(uint64_t id, int fd);
int take_shared_fd(uint64_t id);
void forget_shared_fds();
uint64_t vk_bind_image_memory(uint64_t image, uint64_t memory, uint64_t offset);
uint64_t vk_free_memory(uint64_t memory);
uint64_t vk_map_memory(uint64_t memory, uint64_t offset, uint64_t size, uint32_t flags);
uint64_t vk_write_mapped_memory(uint64_t memory, uint64_t offset, const std::vector<uint8_t>& in);
// What the host's own copy of a mapped allocation holds now. The GPU
// writes a readback into it, and where the host could not import the
// engine's pages that is the only copy there is.
uint64_t vk_read_mapped_memory(uint64_t memory, uint64_t offset, uint64_t size,
                               std::vector<uint8_t>& out, uint32_t* out_len);
// A mapped allocation both processes map, and a write whose bytes are
// therefore already here; see the definitions.
uint64_t vk_share_mapped_memory(uint64_t memory, uint64_t shared_id, uint64_t size);
uint64_t vk_write_shared_mapped_memory(uint64_t memory, uint64_t offset, uint64_t n);
uint64_t vk_unmap_memory(uint64_t memory);
uint64_t vk_flush_mapped_memory_ranges(uint64_t memory, uint64_t offset, uint64_t size);
uint64_t vk_get_surface_formats(uint64_t physical_device, uint64_t surface, uint32_t capacity,
                                 std::vector<uint8_t>& out, uint32_t* out_len);
uint64_t vk_get_surface_present_modes(uint64_t physical_device, uint64_t surface, uint32_t capacity,
                                       std::vector<uint8_t>& out, uint32_t* out_len);
uint64_t vk_get_surface_support(uint64_t physical_device, uint32_t queue_family, uint64_t surface,
                                 std::vector<uint8_t>& out, uint32_t* out_len);
uint64_t vk_create_swapchain(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                              uint32_t* out_len);
uint64_t vk_get_swapchain_images(uint64_t swapchain, uint32_t capacity, std::vector<uint8_t>& out,
                                  uint32_t* out_len);
uint64_t vk_get_image_format_properties2(uint64_t physical_device, const std::vector<uint8_t>& in,
                                          uint32_t node_count, std::vector<uint8_t>& out,
                                          uint32_t* out_len);

uint64_t vk_create_buffer(uint32_t flags, uint64_t size, uint32_t usage, uint32_t sharing_mode,
                           std::vector<uint8_t>& out, uint32_t* out_len);
uint64_t vk_get_buffer_memory_requirements(uint64_t buffer, std::vector<uint8_t>& out,
                                            uint32_t* out_len);
uint64_t vk_bind_buffer_memory(uint64_t buffer, uint64_t memory, uint64_t offset);
uint64_t vk_create_image_view(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                               uint32_t* out_len);
uint64_t vk_create_shader_module(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                                  uint32_t* out_len);
uint64_t vk_destroy_handle(uint32_t kind, uint64_t handle);

// Draw pipeline. Payloads are read with the shared Reader in
// vulkan_forward.h; see the implementations for each layout.
uint64_t vk_create_render_pass(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                                uint32_t* out_len);
uint64_t vk_create_framebuffer(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                                uint32_t* out_len);
uint64_t vk_create_sampler(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                            uint32_t* out_len);
uint64_t vk_create_descriptor_set_layout(const std::vector<uint8_t>& in,
                                          std::vector<uint8_t>& out, uint32_t* out_len);
uint64_t vk_create_pipeline_layout(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                                    uint32_t* out_len);
uint64_t vk_create_descriptor_pool(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                                    uint32_t* out_len);
uint64_t vk_allocate_descriptor_sets(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                                      uint32_t* out_len);
uint64_t vk_reset_descriptor_pool(uint64_t pool, uint32_t flags);
uint64_t vk_create_descriptor_update_template(const std::vector<uint8_t>& in,
                                               std::vector<uint8_t>& out, uint32_t* out_len);
uint64_t vk_update_descriptor_set_with_template(uint64_t set, uint64_t tmpl,
                                                 const std::vector<uint8_t>& in);
uint64_t vk_create_graphics_pipelines(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                                       uint32_t* out_len);
uint64_t vk_create_compute_pipelines(const std::vector<uint8_t>& in, std::vector<uint8_t>& out,
                                      uint32_t* out_len);

uint64_t vk_allocate_command_buffers(uint64_t pool, uint32_t level, uint32_t count,
                                      std::vector<uint8_t>& out, uint32_t* out_len);
uint64_t vk_begin_command_buffer(uint64_t cb, uint32_t flags);
uint64_t vk_end_command_buffer(uint64_t cb);
uint64_t vk_reset_command_pool(uint64_t pool, uint32_t flags);
uint64_t vk_queue_submit(uint64_t queue, uint64_t fence, const std::vector<uint8_t>& in);
uint64_t vk_wait_for_fences(const std::vector<uint8_t>& in, uint32_t wait_all, uint64_t timeout);
uint64_t vk_reset_fences(const std::vector<uint8_t>& in);
uint64_t vk_acquire_next_image(uint64_t swapchain, uint64_t timeout, uint64_t semaphore,
                                uint64_t fence, std::vector<uint8_t>& out, uint32_t* out_len);
uint64_t vk_queue_present(uint64_t queue, const std::vector<uint8_t>& in);
uint64_t vk_get_query_pool_results(uint64_t pool, uint32_t first, uint32_t count, uint32_t stride,
                                    uint32_t flags, std::vector<uint8_t>& out, uint32_t* out_len);

// The whole vkCmd* family, keyed by vk_wire::CmdKind.
// Records one command. The batch form takes a pointer into the batch
// rather than a vector, so a frame's worth of commands is decoded where
// it already sits instead of each one being copied out first.
uint64_t vk_cmd_record(uint64_t cb_handle, uint32_t kind, const uint8_t* data, size_t size);
inline uint64_t vk_cmd_record(uint64_t cb_handle, uint32_t kind, const std::vector<uint8_t>& in) {
    return vk_cmd_record(cb_handle, kind, in.data(), in.size());
}

}  // namespace stud::render_host

#endif  // STUD_VULKAN_HOST_H
