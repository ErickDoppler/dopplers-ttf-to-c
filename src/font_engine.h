#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

enum class RenderMode { Mono1, Gray8 };

struct CharRange { uint32_t first, last; };

struct CharSet {
    const wchar_t* uiName;
    const char* codeName;
    std::vector<CharRange> ranges;
};

const std::vector<CharSet>& BuiltinCharSets();
std::vector<uint32_t> ExpandCharSet(const CharSet& cs);

struct Glyph {
    uint32_t cp = 0;
    bool present = false;       // font has a glyph for this code point
    bool clipped = false;       // some ink fell outside the cell
    int advance = 0;            // advance width in px at the rendered size
    std::vector<uint8_t> px;    // cellW * cellH, 0 = background .. 255 = full ink
};

struct RenderResult {
    int cellW = 0, cellH = 0;
    int fontPx = 0;             // em size used for rasterizing
    bool autoFit = true;
    int baseline = 0;           // row of the baseline, from the top of the cell
    RenderMode mode = RenderMode::Mono1;
    std::vector<Glyph> glyphs;
    int missing = 0, clipped = 0;
};

class FontFile {
public:
    FontFile() = default;
    FontFile(const FontFile&) = delete;
    FontFile& operator=(const FontFile&) = delete;
    ~FontFile() { Unload(); }

    bool Load(const std::wstring& path, std::wstring& err);
    void Unload();
    bool IsLoaded() const { return hRes_ != nullptr; }

    const std::wstring& Path() const { return path_; }
    const std::wstring& FileName() const { return fileName_; }
    const std::wstring& FileStem() const { return stem_; }
    const std::wstring& Family() const { return family_; }
    const std::wstring& SubFamily() const { return subFamily_; }
    std::wstring DisplayName() const;

    // fontPx <= 0 picks the largest size whose ink fits the cell for every char in cps.
    RenderResult Render(const std::vector<uint32_t>& cps, int cellW, int cellH,
                        RenderMode mode, int fontPx) const;

private:
    HFONT MakeFont(int px) const;

    HANDLE hRes_ = nullptr;
    std::vector<uint8_t> data_;
    std::wstring path_, fileName_, stem_, family_, subFamily_, fullName_;
    int weight_ = FW_NORMAL;
    bool italic_ = false;
};
