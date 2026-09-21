#include "font_engine.h"

#include <algorithm>
#include <climits>

namespace {

uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t rd32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

constexpr uint32_t TAG_TTCF = 0x74746366; // 'ttcf'
constexpr uint32_t TAG_OTTO = 0x4F54544F; // 'OTTO'
constexpr uint32_t TAG_TRUE = 0x74727565; // 'true'
constexpr uint32_t TAG_NAME = 0x6E616D65; // 'name'
constexpr uint32_t TAG_OS2  = 0x4F532F32; // 'OS/2'

struct SfntInfo {
    std::wstring family, subFamily, fullName;
    int weight = FW_NORMAL;
    bool italic = false;
};

bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (64LL << 20);
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size();
    }
    CloseHandle(h);
    return ok;
}

// Reads family/subfamily names and style from the sfnt 'name' and 'OS/2' tables.
// GDI needs the family name to select a privately loaded font.
bool ParseSfnt(const std::vector<uint8_t>& d, SfntInfo& info) {
    const size_t n = d.size();
    if (n < 12) return false;
    size_t base = 0;
    if (rd32(&d[0]) == TAG_TTCF) {           // collection: use the first face
        if (n < 16) return false;
        base = rd32(&d[12]);
        if (base + 12 > n) return false;
    }
    uint32_t ver = rd32(&d[base]);
    if (ver != 0x00010000 && ver != TAG_OTTO && ver != TAG_TRUE) return false;

    uint16_t numTables = rd16(&d[base + 4]);
    if (base + 12 + (size_t)numTables * 16 > n) return false;

    size_t nameOff = 0, nameLen = 0, os2Off = 0, os2Len = 0;
    for (uint16_t i = 0; i < numTables; ++i) {
        const uint8_t* rec = &d[base + 12 + (size_t)i * 16];
        uint32_t tag = rd32(rec), off = rd32(rec + 8), len = rd32(rec + 12);
        if ((size_t)off + len > n) continue;
        if (tag == TAG_NAME) { nameOff = off; nameLen = len; }
        if (tag == TAG_OS2)  { os2Off = off;  os2Len = len; }
    }
    if (!nameLen || nameLen < 6) return false;

    const uint8_t* nt = &d[nameOff];
    uint16_t count = rd16(nt + 2), strOff = rd16(nt + 4);
    if (6 + (size_t)count * 12 > nameLen) return false;

    int bestScore[5] = {};   // index: 1 = family, 2 = subfamily, 4 = full name
    std::wstring* target[5] = { nullptr, &info.family, &info.subFamily, nullptr, &info.fullName };
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* r = nt + 6 + (size_t)i * 12;
        uint16_t pid = rd16(r), eid = rd16(r + 2), lid = rd16(r + 4), nid = rd16(r + 6);
        uint16_t len = rd16(r + 8), off = rd16(r + 10);
        if (nid >= 5 || !target[nid]) continue;
        size_t s = (size_t)strOff + off;
        if (s + len > nameLen) continue;

        int score = 0;
        bool utf16 = false;
        if (pid == 3 && (eid == 0 || eid == 1 || eid == 10)) { score = lid == 0x409 ? 4 : 3; utf16 = true; }
        else if (pid == 0)                                     { score = 2; utf16 = true; }
        else if (pid == 1 && eid == 0)                         { score = 1; }
        if (score <= bestScore[nid]) continue;

        std::wstring str;
        const uint8_t* p = nt + s;
        if (utf16) for (uint16_t k = 0; k + 1 < len; k += 2) str.push_back((wchar_t)rd16(p + k));
        else       for (uint16_t k = 0; k < len; ++k) str.push_back((wchar_t)p[k]);
        if (str.empty()) continue;
        *target[nid] = str;
        bestScore[nid] = score;
    }

    if (os2Len >= 64) {
        const uint8_t* os2 = &d[os2Off];
        int w = rd16(os2 + 4);
        if (w >= 1 && w <= 1000) info.weight = w;
        info.italic = (rd16(os2 + 62) & 1) != 0;
    }
    return !info.family.empty();
}

} // namespace

const std::vector<CharSet>& BuiltinCharSets() {
    static const std::vector<CharSet> sets = {
        { L"Numbers (0-9)",    "Numbers 0-9",
          { {0x30, 0x39} } },
        { L"ASCII (EN)",       "ASCII 0x20-0x7E",
          { {0x20, 0x7E} } },
        { L"ASCII + Cyrillic", "ASCII 0x20-0x7E + Cyrillic (U+0401, U+0410-U+044F, U+0451)",
          { {0x20, 0x7E}, {0x401, 0x401}, {0x410, 0x44F}, {0x451, 0x451} } },
    };
    return sets;
}

std::vector<uint32_t> ExpandCharSet(const CharSet& cs) {
    std::vector<uint32_t> out;
    for (const auto& r : cs.ranges)
        for (uint32_t c = r.first; c <= r.last; ++c) out.push_back(c);
    return out;
}

bool FontFile::Load(const std::wstring& path, std::wstring& err) {
    std::vector<uint8_t> data;
    if (!ReadAll(path, data)) { err = L"Cannot read the file."; return false; }

    SfntInfo info;
    if (!ParseSfnt(data, info)) {
        err = L"This is not a TrueType / OpenType font (unrecognized or damaged header).";
        return false;
    }

    // Unload first: the new file may share the family name with the current one.
    Unload();

    DWORD count = 0;
    HANDLE h = AddFontMemResourceEx(data.data(), (DWORD)data.size(), nullptr, &count);
    if (!h || count == 0) { err = L"Windows could not load this font."; return false; }

    hRes_ = h;
    data_ = std::move(data);
    path_ = path;
    size_t slash = path.find_last_of(L"\\/");
    fileName_ = slash == std::wstring::npos ? path : path.substr(slash + 1);
    size_t dot = fileName_.find_last_of(L'.');
    stem_ = dot == std::wstring::npos ? fileName_ : fileName_.substr(0, dot);
    family_ = info.family;
    subFamily_ = info.subFamily;
    fullName_ = info.fullName;
    weight_ = info.weight;
    italic_ = info.italic;

    // Make sure GDI really picked our font and did not substitute another one.
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT f = MakeFont(32);
    HGDIOBJ old = SelectObject(dc, f);
    wchar_t face[LF_FACESIZE] = {};
    GetTextFaceW(dc, LF_FACESIZE, face);
    SelectObject(dc, old);
    DeleteObject(f);
    DeleteDC(dc);
    if (_wcsnicmp(face, family_.c_str(), LF_FACESIZE - 1) != 0) {
        err = L"Windows substituted \"" + std::wstring(face) + L"\" for \"" + family_ +
              L"\". The font may be damaged or use an unsupported format.";
        Unload();
        return false;
    }
    return true;
}

void FontFile::Unload() {
    if (hRes_) RemoveFontMemResourceEx(hRes_);
    hRes_ = nullptr;
    data_.clear();
    path_.clear(); fileName_.clear(); stem_.clear();
    family_.clear(); subFamily_.clear(); fullName_.clear();
}

std::wstring FontFile::DisplayName() const {
    if (!fullName_.empty()) return fullName_;
    if (subFamily_.empty()) return family_;
    return family_ + L" " + subFamily_;
}

HFONT FontFile::MakeFont(int px) const {
    LOGFONTW lf{};
    lf.lfHeight = -px;                       // negative = em height in pixels
    lf.lfWeight = weight_;
    lf.lfItalic = italic_ ? TRUE : FALSE;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_ONLY_PRECIS;
    lf.lfQuality = ANTIALIASED_QUALITY;
    wcsncpy(lf.lfFaceName, family_.c_str(), LF_FACESIZE - 1);
    return CreateFontIndirectW(&lf);
}

RenderResult FontFile::Render(const std::vector<uint32_t>& cps, int W, int H,
                              RenderMode mode, int fontPx) const {
    RenderResult r;
    r.cellW = W;
    r.cellH = H;
    r.mode = mode;
    r.autoFit = fontPx <= 0;
    r.glyphs.resize(cps.size());
    for (size_t i = 0; i < cps.size(); ++i) {
        r.glyphs[i].cp = cps[i];
        r.glyphs[i].px.assign((size_t)W * H, 0);
    }
    if (!IsLoaded() || W <= 0 || H <= 0 || cps.empty()) return r;

    HDC dc = CreateCompatibleDC(nullptr);
    const MAT2 mat = { {0, 1}, {0, 0}, {0, 0}, {0, 1} };
    const bool mono = mode == RenderMode::Mono1;
    const UINT fmt = (mono ? GGO_BITMAP : GGO_GRAY8_BITMAP) | GGO_GLYPH_INDEX;

    // Which code points exist in the font (independent of size).
    std::vector<WORD> gi(cps.size(), 0xFFFF);
    {
        std::wstring s;
        for (uint32_t cp : cps) s.push_back((wchar_t)cp);
        HFONT f = MakeFont(32);
        HGDIOBJ old = SelectObject(dc, f);
        GetGlyphIndicesW(dc, s.c_str(), (int)s.size(), gi.data(), GGI_MARK_NONEXISTING_GLYPHS);
        SelectObject(dc, old);
        DeleteObject(f);
    }
    for (size_t i = 0; i < cps.size(); ++i) {
        if (gi[i] == 0) gi[i] = 0xFFFF;             // .notdef (tofu box) also means missing
        r.glyphs[i].present = gi[i] != 0xFFFF;   // missing chars stay blank, i.e. a space
        if (!r.glyphs[i].present) ++r.missing;
    }

    struct Ink { int top = INT_MIN, bottom = INT_MAX, maxW = 0; bool any = false; };
    auto measure = [&](int px) {
        Ink k;
        HFONT f = MakeFont(px);
        HGDIOBJ old = SelectObject(dc, f);
        for (size_t i = 0; i < cps.size(); ++i) {
            if (gi[i] == 0xFFFF) continue;
            GLYPHMETRICS gm{};
            DWORD sz = GetGlyphOutlineW(dc, gi[i], fmt, &gm, 0, nullptr, &mat);
            if (sz == GDI_ERROR || sz == 0) continue;
            k.any = true;
            k.top = std::max(k.top, (int)gm.gmptGlyphOrigin.y);
            k.bottom = std::min(k.bottom, (int)gm.gmptGlyphOrigin.y - (int)gm.gmBlackBoxY);
            k.maxW = std::max(k.maxW, (int)gm.gmBlackBoxX);
        }
        SelectObject(dc, old);
        DeleteObject(f);
        return k;
    };
    auto fits = [&](const Ink& k) { return k.top - k.bottom <= H && k.maxW <= W; };

    int px = fontPx;
    if (px <= 0) {
        if (!measure(H).any) {
            px = H;
        } else {
            int lo = 1, hi = 2 * H + 8, best = 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                if (fits(measure(mid))) { best = mid; lo = mid + 1; } else hi = mid - 1;
            }
            // Hinting makes the fit slightly non-monotonic; probe a few sizes above.
            for (int p = best + 1; p <= best + 3; ++p) if (fits(measure(p))) best = p;
            px = best;
        }
    }
    r.fontPx = px;

    // Center the combined ink band of the whole char set vertically; all chars share one baseline.
    Ink k = measure(px);
    int inkH = k.any ? k.top - k.bottom : H;
    int offY = (H - inkH) / 2;
    r.baseline = k.any ? offY + k.top : H * 3 / 4;

    HFONT f = MakeFont(px);
    HGDIOBJ old = SelectObject(dc, f);
    std::vector<uint8_t> buf;
    for (size_t i = 0; i < cps.size(); ++i) {
        Glyph& g = r.glyphs[i];
        if (!g.present) continue;
        GLYPHMETRICS gm{};
        DWORD sz = GetGlyphOutlineW(dc, gi[i], fmt, &gm, 0, nullptr, &mat);
        g.advance = gm.gmCellIncX;
        if (sz == GDI_ERROR || sz == 0) continue;   // blank glyph (e.g. space)
        buf.assign(sz, 0);
        if (GetGlyphOutlineW(dc, gi[i], fmt, &gm, sz, buf.data(), &mat) == GDI_ERROR) continue;

        int bw = (int)gm.gmBlackBoxX, bh = (int)gm.gmBlackBoxY;
        int pitch = mono ? ((bw + 31) / 32) * 4 : ((bw + 3) & ~3);
        if ((DWORD)(pitch * bh) > sz) pitch = (int)(sz / bh);

        // Horizontal: center the advance box, then keep the ink inside the cell if it can fit.
        int x0 = (W - g.advance) / 2 + gm.gmptGlyphOrigin.x;
        if (bw <= W) x0 = std::clamp(x0, 0, W - bw);
        else x0 = (W - bw) / 2;
        int y0 = r.baseline - gm.gmptGlyphOrigin.y;

        for (int y = 0; y < bh; ++y) {
            const uint8_t* row = buf.data() + (size_t)y * pitch;
            for (int x = 0; x < bw; ++x) {
                uint8_t v = mono ? ((row[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0)
                                 : (uint8_t)(std::min<int>(row[x], 64) * 255 / 64);
                if (!v) continue;
                int X = x0 + x, Y = y0 + y;
                if (X < 0 || Y < 0 || X >= W || Y >= H) { g.clipped = true; continue; }
                g.px[(size_t)Y * W + X] = v;
            }
        }
        if (g.clipped) ++r.clipped;
    }
    SelectObject(dc, old);
    DeleteObject(f);
    DeleteDC(dc);
    return r;
}
