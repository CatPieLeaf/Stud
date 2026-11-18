#include "cube.h"

#include <QImage>
#include <QPolygonF>
#include <QTransform>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <numbers>
#include <vector>

#include "mac_client.h"

namespace {

// One stud from the installed Player's DXT1 studs texture: the top-left quarter of
// the first tile. Empty when it's missing or another format.
std::vector<unsigned char> load_one_stud(int& size) {
    const auto build = stud::ui::client::installed(stud::ui::client::Product::Player);
    if (!build) {
        std::printf("stud: no Roblox Player installed yet, the cube has no stud\n");
        return {};
    }
    const QString path = build->app + "/Contents/Resources/PlatformContent/pc/textures/studs.dds";
    std::printf("stud: the cube's stud is from Roblox Player %s, %s\n", qPrintable(build->version),
                qPrintable(path));
    std::ifstream in(path.toStdString(), std::ios::binary);
    const std::vector<unsigned char> d((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
    auto u32 = [&d](size_t at) {
        uint32_t v = 0;
        std::memcpy(&v, d.data() + at, sizeof v);
        return v;
    };
    constexpr size_t kHeader = 128;
    constexpr uint32_t kFourCc = 0x4;
    constexpr int kBlock = 4;
    constexpr size_t kBlockBytes = 8;
    if (d.size() < kHeader || std::memcmp(d.data(), "DDS ", 4) != 0) return {};
    const uint32_t height = u32(12);
    const uint32_t width = u32(16);
    const size_t blocks_across = width / kBlock;
    if ((u32(80) & kFourCc) == 0 || std::memcmp(d.data() + 84, "DXT1", 4) != 0 || width < 2 * kBlock ||
        height < width || d.size() < kHeader + blocks_across * (width / kBlock) * kBlockBytes) {
        return {};
    }
    size = static_cast<int>(width / 2);
    std::vector<unsigned char> stud(static_cast<size_t>(size) * size);
    // 5:6:5 luminance at 8 bits.
    auto luminance = [](uint16_t c, int weight_of_other = 0, uint16_t other = 0, int parts = 1) {
        auto channel = [](uint16_t v, int shift, int bits) { return ((v >> shift) & ((1 << bits) - 1)) * 255 / ((1 << bits) - 1); };
        auto lum = [&](uint16_t v) { return (channel(v, 11, 5) * 77 + channel(v, 5, 6) * 150 + channel(v, 0, 5) * 29) >> 8; };
        return (lum(c) * (parts - weight_of_other) + lum(other) * weight_of_other) / parts;
    };
    for (int by = 0; by < size / kBlock; ++by) {
        for (int bx = 0; bx < size / kBlock; ++bx) {
            const unsigned char* block = d.data() + kHeader + (by * blocks_across + bx) * kBlockBytes;
            uint16_t c0 = 0, c1 = 0;
            uint32_t indices = 0;
            std::memcpy(&c0, block, 2);
            std::memcpy(&c1, block + 2, 2);
            std::memcpy(&indices, block + 4, 4);
            int palette[4];
            palette[0] = luminance(c0);
            palette[1] = luminance(c1);
            if (c0 > c1) {
                palette[2] = luminance(c0, 1, c1, 3);
                palette[3] = luminance(c0, 2, c1, 3);
            } else {
                palette[2] = luminance(c0, 1, c1, 2);
                palette[3] = 0;
            }
            for (int y = 0; y < kBlock; ++y) {
                for (int x = 0; x < kBlock; ++x) {
                    const int index = (indices >> (2 * (y * kBlock + x))) & 0x3;
                    stud[static_cast<size_t>(by * kBlock + y) * size + bx * kBlock + x] =
                        static_cast<unsigned char>(palette[index]);
                }
            }
        }
    }
    return stud;
}

struct Vec3 {
    double x, y, z;
};

constexpr std::array<Vec3, 8> kCorners{{{0.5, 0.5, 0.5}, {-0.5, 0.5, 0.5}, {-0.5, -0.5, 0.5}, {0.5, -0.5, 0.5},
                                        {0.5, 0.5, -0.5}, {-0.5, 0.5, -0.5}, {-0.5, -0.5, -0.5}, {0.5, -0.5, -0.5}}};
constexpr std::array<Vec3, 8> kColors{{{1.0, 0.4, 0.6}, {1.0, 0.9, 0.2}, {0.7, 0.3, 0.8}, {0.5, 0.3, 1.0},
                                       {0.2, 0.6, 1.0}, {0.6, 1.0, 0.4}, {0.6, 0.8, 0.8}, {0.4, 0.8, 0.8}}};
constexpr int kFaces[6][4] = {{0, 1, 2, 3}, {0, 3, 7, 4}, {2, 6, 7, 3}, {1, 5, 6, 2}, {4, 7, 6, 5}, {5, 1, 0, 4}};
// Each face corner's place on the face image; row 0 is the top of the stud.
constexpr double kUv[4][2] = {{1, 0}, {0, 0}, {0, 1}, {1, 1}};

unsigned g_client_generation = 1;

// Each face's corner colours, blended across it and overlaid with the stud.
const std::array<QImage, 6>& face_images() {
    static unsigned generation = 0;
    static std::array<QImage, 6> faces;
    if (generation == g_client_generation) return faces;
    generation = g_client_generation;
    int size = 0;
    const std::vector<unsigned char> stud = load_one_stud(size);
    const int n = stud.empty() ? 64 : size;
    for (int f = 0; f < 6; ++f) {
        QImage image(n, n, QImage::Format_RGB32);
        for (int y = 0; y < n; ++y) {
            const double v = (y + 0.5) / n;
            for (int x = 0; x < n; ++x) {
                const double u = (x + 0.5) / n;
                double rgb[3] = {0, 0, 0};
                for (int c = 0; c < 4; ++c) {
                    const double w = (kUv[c][0] ? u : 1 - u) * (kUv[c][1] ? v : 1 - v);
                    const Vec3& col = kColors[kFaces[f][c]];
                    rgb[0] += w * col.x;
                    rgb[1] += w * col.y;
                    rgb[2] += w * col.z;
                }
                if (!stud.empty()) {
                    const double t = stud[static_cast<size_t>(y) * n + x] / 255.0;
                    for (double& c : rgb) c = c < 0.5 ? 2 * c * t : 1 - 2 * (1 - c) * (1 - t);
                }
                image.setPixel(x, y, qRgb(int(rgb[0] * 255), int(rgb[1] * 255), int(rgb[2] * 255)));
            }
        }
        faces[f] = image;
    }
    return faces;
}

}  // namespace

void draw_cube(QPainter& painter, const QRectF& area, double seconds) {
    const double pi = std::numbers::pi;
    const double spin = 2 * pi * seconds / 4.0;
    const double tilt = 0.15 * pi;
    // The closest distance where no corner leaves the area over a full turn.
    const double distance = 2.1;
    const double aspect = area.height() > 0 ? area.width() / area.height() : 1.0;

    std::array<Vec3, 8> view{};
    std::array<QPointF, 8> screen{};
    for (size_t i = 0; i < kCorners.size(); ++i) {
        const Vec3& p = kCorners[i];
        const double x1 = p.x * std::cos(spin) + p.z * std::sin(spin);
        const double z1 = -p.x * std::sin(spin) + p.z * std::cos(spin);
        const double y2 = p.y * std::cos(tilt) - z1 * std::sin(tilt);
        const double z2 = p.y * std::sin(tilt) + z1 * std::cos(tilt) - distance;
        view[i] = {x1, y2, z2};
        const double nx = (x1 / (0.5 * aspect)) / -z2;
        const double ny = (y2 / 0.5) / -z2;
        screen[i] = {area.left() + (nx + 1) / 2 * area.width(), area.top() + (1 - ny) / 2 * area.height()};
    }

    // Back to front, so nearer faces cover farther ones.
    std::array<int, 6> order{0, 1, 2, 3, 4, 5};
    const auto depth = [&](int f) {
        double z = 0;
        for (int c : kFaces[f]) z += view[c].z;
        return z;
    };
    std::sort(order.begin(), order.end(), [&](int a, int b) { return depth(a) < depth(b); });

    const auto& faces = face_images();
    const QTransform base = painter.worldTransform();
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    for (int f : order) {
        const double n = faces[f].width();
        QPolygonF from, to;
        for (int c = 0; c < 4; ++c) {
            from << QPointF(kUv[c][0] * n, kUv[c][1] * n);
            to << screen[kFaces[f][c]];
        }
        QTransform map;
        if (!QTransform::quadToQuad(from, to, map)) continue;
        painter.setWorldTransform(map * base);
        painter.drawImage(QPointF(0, 0), faces[f]);
    }
    painter.restore();
}

void cube_client_changed() { ++g_client_generation; }
