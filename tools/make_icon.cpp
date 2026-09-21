// Build-time helper: writes res/app.ico - an amber 5x7 dot-matrix "A" on a dark rounded tile.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

static const char* kGlyph[7] = {
    ".###.",
    "#...#",
    "#...#",
    "#####",
    "#...#",
    "#...#",
    "#...#",
};

static std::vector<uint32_t> Render(int n) {
    std::vector<uint32_t> px(n * n, 0);
    const double r = n * 0.18, pitch = n / 7.0, dot = pitch * 0.36;
    const double ox = (n - 4 * pitch) / 2.0, oy = (n - 6 * pitch) / 2.0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            double cx = x + 0.5, cy = y + 0.5;
            // rounded tile coverage
            double dx = std::fmax(std::fmax(r - cx, cx - (n - r)), 0.0);
            double dy = std::fmax(std::fmax(r - cy, cy - (n - r)), 0.0);
            double a = std::fmin(std::fmax(r - std::sqrt(dx * dx + dy * dy) + 0.5, 0.0), 1.0);
            if (a <= 0) continue;
            double R = 28, G = 32, B = 40;
            for (int gy = 0; gy < 7; ++gy)
                for (int gx = 0; gx < 5; ++gx) {
                    double d = std::hypot(cx - (ox + gx * pitch), cy - (oy + gy * pitch));
                    double c = std::fmin(std::fmax(dot - d + 0.5, 0.0), 1.0);
                    if (c <= 0) continue;
                    bool on = kGlyph[gy][gx] == '#';
                    double tr = on ? 255 : 60, tg = on ? 176 : 64, tb = on ? 32 : 74;
                    R += (tr - R) * c; G += (tg - G) * c; B += (tb - B) * c;
                }
            px[y * n + x] = ((uint32_t)(a * 255) << 24) | ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
        }
    return px;
}

static void Put16(std::vector<uint8_t>& o, uint32_t v) { o.push_back(v & 0xFF); o.push_back((v >> 8) & 0xFF); }
static void Put32(std::vector<uint8_t>& o, uint32_t v) { Put16(o, v & 0xFFFF); Put16(o, v >> 16); }

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "app.ico";
    const int sizes[] = {16, 24, 32, 48, 64};
    const int count = sizeof sizes / sizeof sizes[0];

    std::vector<std::vector<uint8_t>> images;
    for (int n : sizes) {
        std::vector<uint32_t> px = Render(n);
        std::vector<uint8_t> img;
        Put32(img, 40); Put32(img, n); Put32(img, n * 2); Put16(img, 1); Put16(img, 32);
        Put32(img, 0); Put32(img, 0); Put32(img, 0); Put32(img, 0); Put32(img, 0); Put32(img, 0);
        for (int y = n - 1; y >= 0; --y)
            for (int x = 0; x < n; ++x) Put32(img, px[y * n + x]);
        int maskRow = ((n + 31) / 32) * 4;
        img.insert(img.end(), (size_t)maskRow * n, 0);   // alpha channel is used instead
        images.push_back(img);
    }

    std::vector<uint8_t> ico;
    Put16(ico, 0); Put16(ico, 1); Put16(ico, count);
    uint32_t offset = 6 + 16 * count;
    for (int i = 0; i < count; ++i) {
        ico.push_back((uint8_t)sizes[i]); ico.push_back((uint8_t)sizes[i]);
        ico.push_back(0); ico.push_back(0);
        Put16(ico, 1); Put16(ico, 32);
        Put32(ico, (uint32_t)images[i].size()); Put32(ico, offset);
        offset += (uint32_t)images[i].size();
    }
    for (auto& img : images) ico.insert(ico.end(), img.begin(), img.end());

    FILE* f = fopen(out, "wb");
    if (!f) return 1;
    fwrite(ico.data(), 1, ico.size(), f);
    fclose(f);
    return 0;
}
