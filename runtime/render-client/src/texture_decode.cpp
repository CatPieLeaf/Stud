#include "texture_decode.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// detex decodes every ETC/EAC format Stud emulates. It replaced
// etcdec.h, which is a reference decoder written for clarity: measured on
// this machine the two agree byte for byte on every format here, and
// detex is 2.3x faster on the two colour formats a texture burst is
// actually made of. See runtime/CMakeLists.txt for how it is vendored.
extern "C" {
#include <detex.h>
}

#include <PVRTDecompress.h>

#include "texture_cache.h"

namespace stud::texture_decode {
namespace {

// A few worker threads for the block loop.
//
// Decoding a compressed block is per-block work with no dependency
// between blocks, and there are a lot of them: a
// 1024x1024 texture is 65,536. Done one after another on the calling
// thread that is hundreds of milliseconds, which is exactly the stall
// the engine reports as a 500ms frame when a new place loads and every
// texture in it arrives at once.
//
// Threads are created once and parked. The pool is deliberately small.
// This runs on the engine's own thread while the engine has work of its
// own to do, and taking every core would win the texture and lose the
// frame.
class BlockPool {
public:
    // Never destroyed. The workers sleep on this pool's condition variable
    // for the life of the process, so a static destructor at exit destroyed
    // it under them, which is undefined and was caught hanging a process
    // in exit() with every worker parked.
    static BlockPool& instance() {
        static BlockPool* pool = new BlockPool;
        return *pool;
    }

    unsigned workers() const { return static_cast<unsigned>(threads_.size()); }

    // Runs fn(begin, end) over [0, count) split across the pool, and
    // returns once every part is done. The caller's own thread takes a
    // share too rather than waiting idle.
    void run(uint32_t count, const std::function<void(uint32_t, uint32_t)>& fn) {
        const unsigned parts = workers() + 1;
        if (parts <= 1 || count < 2) {
            fn(0, count);
            return;
        }
        // One parallel decode at a time. The engine decodes textures on
        // several threads at once (it runs eight texture-loading
        // threads), and they were all writing the same work state,
        // each one clobbering the others' slice bounds, so the counter
        // this waits on never reached zero and everything stopped.
        //
        // A second caller does its own work inline rather than queueing
        // behind the first: it already has a thread of its own, so the
        // machine stays just as busy and nothing waits on anything.
        std::unique_lock<std::mutex> busy(run_mutex_, std::try_to_lock);
        if (!busy.owns_lock()) {
            fn(0, count);
            return;
        }
        const uint32_t per = (count + parts - 1) / parts;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            work_ = &fn;
            next_ = per;          // the caller takes the first slice
            end_ = count;
            stride_ = per;
            remaining_ = 0;
            for (uint32_t at = per; at < count; at += per) ++remaining_;
            generation_++;
        }
        wake_.notify_all();
        fn(0, per < count ? per : count);
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [this] { return remaining_ == 0; });
        work_ = nullptr;
    }

private:
    BlockPool() {
        unsigned hw = std::thread::hardware_concurrency();
        if (hw < 2) hw = 2;
        // A quarter of the machine, not half, and not a flat three.
        //
        // Encoding a block is the whole cost of this path and blocks are
        // independent, so one texture decodes faster the more threads it
        // gets. Measured on a 20-thread machine, one 1024x1024 level:
        //
        //    1 thread   82.7 ms
        //    4 threads  20.4 ms   <- what a flat three workers gave
        //    8 threads  15.1 ms
        //   12 threads  10.3 ms
        //   16 threads   9.0 ms
        //
        // But that is the wrong question during play. The engine runs
        // EIGHT texture-loading threads of its own, and only one parallel
        // decode happens at a time here -- the rest decode inline on the
        // thread that asked (see run()). So streaming is already spread
        // across the machine without this pool, and sizing the pool to
        // half the cores just takes them away from the frame being drawn.
        // Half measured visibly worse in a game than the flat three it
        // replaced.
        //
        // A quarter keeps most of the win on a big texture -- five
        // workers plus the caller is still around five times one thread --
        // while leaving the engine and the render host the rest.
        unsigned want = hw / 4;
        if (want < 2) want = 2;
        if (want > 6) want = 6;
        for (unsigned i = 0; i < want; ++i) {
            threads_.emplace_back([this] { worker(); });
        }
    }

    void worker() {
        uint64_t seen = 0;
        for (;;) {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return generation_ != seen; });
            seen = generation_;
            for (;;) {
                if (work_ == nullptr || next_ >= end_) break;
                const uint32_t begin = next_;
                uint32_t stop = begin + stride_;
                if (stop > end_) stop = end_;
                next_ = stop;
                const auto* fn = work_;
                lock.unlock();
                (*fn)(begin, stop);
                lock.lock();
                if (remaining_ > 0) --remaining_;
                if (remaining_ == 0) done_.notify_all();
            }
        }
    }

    std::vector<std::thread> threads_;
    // Held for the length of one parallel run, so the shared slice state
    // below belongs to exactly one caller at a time.
    std::mutex run_mutex_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    const std::function<void(uint32_t, uint32_t)>* work_ = nullptr;
    uint32_t next_ = 0;
    uint32_t end_ = 0;
    uint32_t stride_ = 1;
    uint32_t remaining_ = 0;
    uint64_t generation_ = 0;
};



// How a format is laid out and what it decodes into. Everything the
// decoders need is here, so the switch below stays a table rather than a
// pile of special cases.
struct Layout {
    VkFormat substitute = VK_FORMAT_UNDEFINED;
    uint32_t block_w = 4;
    uint32_t block_h = 4;
    uint32_t block_bytes = 8;
    uint32_t texel_bytes = 4;
    enum Codec { kEtcRgb, kEtcRgbA1, kEacRgba, kEacR11, kEacRg11, kPvrtc4, kPvrtc2 } codec = kEtcRgb;
};

bool layout_for(VkFormat f, Layout& out) {
    switch (f) {
        // ETC1 has no Vulkan format of its own: it is a strict subset of
        // ETC2 RGB, and a decoder for the latter decodes the former.
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
            out = {VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 8, 4, Layout::kEtcRgb};
            return true;
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
            out = {VK_FORMAT_R8G8B8A8_SRGB, 4, 4, 8, 4, Layout::kEtcRgb};
            return true;
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
            out = {VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 8, 4, Layout::kEtcRgbA1};
            return true;
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
            out = {VK_FORMAT_R8G8B8A8_SRGB, 4, 4, 8, 4, Layout::kEtcRgbA1};
            return true;
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
            out = {VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 16, 4, Layout::kEacRgba};
            return true;
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
            out = {VK_FORMAT_R8G8B8A8_SRGB, 4, 4, 16, 4, Layout::kEacRgba};
            return true;
        // EAC carries 11 bits per channel, so it decodes to 16-bit rather
        // than 8, rounding it to a byte would throw away precision the
        // source actually has. The signed variants are deliberately absent:
        // this decoder produces unsigned data, and claiming support for a
        // format whose sign it would silently lose is worse than saying no.
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
            out = {VK_FORMAT_R16_UNORM, 4, 4, 8, 2, Layout::kEacR11};
            return true;
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
            out = {VK_FORMAT_R16G16_UNORM, 4, 4, 16, 4, Layout::kEacRg11};
            return true;
        // PVRTC. The 4bpp modes cover 4x4 texels per 8-byte block and the
        // 2bpp modes 8x4, but neither decodes block-at-a-time: a PVRTC
        // texel is interpolated between neighbouring blocks, so the whole
        // level goes through the decoder at once.
        case VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG:
            out = {VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 8, 4, Layout::kPvrtc4};
            return true;
        case VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG:
            out = {VK_FORMAT_R8G8B8A8_SRGB, 4, 4, 8, 4, Layout::kPvrtc4};
            return true;
        case VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG:
            out = {VK_FORMAT_R8G8B8A8_UNORM, 8, 4, 8, 4, Layout::kPvrtc2};
            return true;
        case VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG:
            out = {VK_FORMAT_R8G8B8A8_SRGB, 8, 4, 8, 4, Layout::kPvrtc2};
            return true;
        default:
            return false;
    }
}

uint32_t blocks(uint32_t extent, uint32_t block) { return (extent + block - 1) / block; }

}  // namespace

bool is_emulated(VkFormat format) {
    Layout l;
    return layout_for(format, l);
}

bool is_pvrtc(VkFormat format) {
    Layout l;
    if (!layout_for(format, l)) return false;
    return l.codec == Layout::kPvrtc4 || l.codec == Layout::kPvrtc2;
}

VkFormat substitute(VkFormat format) {
    Layout l;
    if (!layout_for(format, l)) return format;
    return l.substitute;
}

uint64_t decoded_size(VkFormat format, uint32_t width, uint32_t height) {
    Layout l;
    if (!layout_for(format, l)) return 0;
    return static_cast<uint64_t>(width) * height * l.texel_bytes;
}

uint64_t encoded_size(VkFormat format, uint32_t width, uint32_t height) {
    Layout l;
    if (!layout_for(format, l)) return 0;
    return static_cast<uint64_t>(blocks(width, l.block_w)) * blocks(height, l.block_h) *
           l.block_bytes;
}

uint64_t decoded_row_offset(VkFormat format, uint32_t width, uint32_t y) {
    Layout l;
    if (!layout_for(format, l)) return 0;
    return static_cast<uint64_t>(y) * width * l.texel_bytes;
}

uint64_t row_pitch_for_texels(VkFormat format, uint32_t row_texels) {
    Layout l;
    if (!layout_for(format, l)) return 0;
    return static_cast<uint64_t>(blocks(row_texels, l.block_w)) * l.block_bytes;
}

bool decode(VkFormat format, const void* src, uint32_t width, uint32_t height, void* dst,
            uint64_t src_row_pitch) {
    Layout l;
    if (!layout_for(format, l) || src == nullptr || dst == nullptr || width == 0 || height == 0) {
        return false;
    }
    auto* out = static_cast<uint8_t*>(dst);
    const auto* in = static_cast<const uint8_t*>(src);

    if (l.codec == Layout::kPvrtc4 || l.codec == Layout::kPvrtc2) {
        // PVRTC interpolates across block boundaries, so the decoder takes
        // the whole level at once and always writes RGBA8. It has no way
        // to skip padding between rows, so a padded source is refused.
        if (src_row_pitch != 0 &&
            src_row_pitch != static_cast<uint64_t>(blocks(width, l.block_w)) * l.block_bytes) {
            return false;
        }
        pvr::PVRTDecompressPVRTC(in, l.codec == Layout::kPvrtc2 ? 1u : 0u, width, height, out);
        return true;
    }

    const uint32_t bw = blocks(width, l.block_w);
    const uint32_t bh = blocks(height, l.block_h);
    const uint32_t row_bytes = width * l.texel_bytes;
    // Has this exact decode been done before?
    //
    // The work below is deterministic: the same source bytes always
    // produce the same output, and Roblox's content never changes. So the
    // result is kept between runs.
    const uint64_t source_bytes =
        static_cast<uint64_t>(bh) * (src_row_pitch != 0 ? src_row_pitch
                                                        : static_cast<uint64_t>(bw) * l.block_bytes);
    const uint64_t output_bytes = static_cast<uint64_t>(height) * row_bytes;
    uint64_t key_high = 0;
    uint64_t key_low = 0;
    // Only levels of 64KB and up, a 256x256 level: every decode that
    // consults the cache pays a hash of the source, and every miss a file
    // write, which is pure loss for Home's thumbnails, each unique and
    // below that size.
    //
    // What a hit saves, in CPU, measured on real dumped ETC2 levels: a
    // 1024x1024 decode is 3.1-3.2 ms across the pool, a hit read back from
    // disk 1.4-1.5 ms and from the page cache 0.26-0.30 ms, and a miss adds
    // 0.56-0.58 ms (hash and write), about 18%, repaid the first time the
    // level is seen again in a later session. 256x256: 0.22 ms against
    // 0.06-0.09 cold. In wall time a cold hit is slower than decoding (1.9
    // against 0.66 ms at 1024x1024), because the read waits and the decode
    // runs on several threads: the cache saves CPU, not latency.
    constexpr uint64_t kMinCacheableOutput = 64u * 1024u;
    const bool cacheable = stud::texture_cache::enabled() && output_bytes >= kMinCacheableOutput;
    if (cacheable) {
        stud::texture_cache::key_for(in, source_bytes, static_cast<uint32_t>(format),
                                     0, width,
                                     height, &key_high, &key_low);
        if (stud::texture_cache::load(key_high, key_low, dst, output_bytes)) return true;
    }
    // Rows of blocks are tightly packed unless the copy said otherwise.
    const uint64_t block_row_pitch =
        src_row_pitch != 0 ? src_row_pitch : static_cast<uint64_t>(bw) * l.block_bytes;
    // Every block decodes into scratch and is copied out, so a block on
    // the right or bottom edge of a non-multiple-of-four level writes only
    // the part that exists. 4x4 texels at 4 bytes is the largest case any
    // of these codecs produces.
    // Rows of blocks are split across the pool. Each row writes only its
    // own output, so nothing is shared and no locking is needed inside.
    // Small levels stay on one thread: waking workers costs more than the
    // work itself, and a texture's small mips are most of its levels.
    // A codec layout_for() already validated cannot turn up here, but if
    // it did a worker cannot return from decode(); it records it.
    std::atomic<bool> failed{false};
    const auto do_rows = [&](uint32_t row_begin, uint32_t row_end) {
    uint8_t scratch[4 * 4 * 4];

    for (uint32_t by = row_begin; by < row_end; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* block =
                in + static_cast<uint64_t>(by) * block_row_pitch + static_cast<uint64_t>(bx) * l.block_bytes;
            const uint32_t x0 = bx * l.block_w;
            const uint32_t y0 = by * l.block_h;
            const uint32_t w = (x0 + l.block_w <= width) ? l.block_w : width - x0;
            const uint32_t h = (y0 + l.block_h <= height) ? l.block_h : height - y0;
            // detex always writes a tight 4x4 block, so a block that
            // lands whole inside the level is decoded into the scratch
            // and copied out in four rows, cheap next to the decode,
            // and it keeps one code path for both cases.
            bool ok = false;
            switch (l.codec) {
                case Layout::kEtcRgb:
                    ok = detexDecompressBlockETC2(block, DETEX_MODE_MASK_ALL, 0, scratch);
                    break;
                case Layout::kEtcRgbA1:
                    ok = detexDecompressBlockETC2_PUNCHTHROUGH(block, DETEX_MODE_MASK_ALL, 0,
                                                               scratch);
                    break;
                case Layout::kEacRgba:
                    ok = detexDecompressBlockETC2_EAC(block, DETEX_MODE_MASK_ALL, 0, scratch);
                    break;
                case Layout::kEacR11:
                    ok = detexDecompressBlockEAC_R11(block, DETEX_MODE_MASK_ALL, 0, scratch);
                    break;
                case Layout::kEacRg11:
                    ok = detexDecompressBlockEAC_RG11(block, DETEX_MODE_MASK_ALL, 0, scratch);
                    break;
                default: failed.store(true); continue;
            }
            // A block detex refuses is a block the engine should not have
            // produced. Zero is visibly wrong rather than random, which is
            // the honest answer for data this decoder cannot read.
            if (!ok) std::memset(scratch, 0, sizeof(scratch));

            for (uint32_t y = 0; y < h; ++y) {
                std::memcpy(out + static_cast<uint64_t>(y0 + y) * row_bytes +
                                static_cast<uint64_t>(x0) * l.texel_bytes,
                            scratch + static_cast<uint64_t>(y) * l.block_w * l.texel_bytes,
                            static_cast<size_t>(w) * l.texel_bytes);
            }
        }
    }
    };

    // Worth spreading only when there is enough work to pay for waking
    // the pool. Below this a level is a handful of blocks and the
    // calling thread does it faster alone.
    constexpr uint32_t kParallelBlockThreshold = 4096;
    if (static_cast<uint64_t>(bw) * bh >= kParallelBlockThreshold) {
        BlockPool::instance().run(bh, do_rows);
    } else {
        do_rows(0, bh);
    }
    const bool ok = !failed.load();
    if (ok && cacheable) {
        stud::texture_cache::store(key_high, key_low, dst, output_bytes);
    }
    return ok;
}

}  // namespace stud::texture_decode
