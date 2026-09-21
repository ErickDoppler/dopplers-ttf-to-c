#pragma once

#include "font_engine.h"

#include <string>

// Full C source for the rendered char set (UTF-8, LF line endings).
std::string GenerateCFile(const RenderResult& r, const FontFile& font, const CharSet& cs);

// Array initializer for a single glyph, used by the preview panel (UTF-8, LF line endings).
std::string GenerateGlyphSnippet(const RenderResult& r, size_t index);

std::string ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);
