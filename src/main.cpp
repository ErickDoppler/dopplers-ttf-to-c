// Doppler's TTF to C - converts TTF/OTF fonts into fixed-size C bitmap arrays for LED/LCD displays.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "codegen.h"
#include "font_engine.h"

namespace {

const wchar_t* const APP_TITLE = L"Doppler's TTF to C";

enum : int {
    IDC_DROP = 100, IDC_GRID, IDC_VIEW,
    IDC_HDR_SET, IDC_LBL_TYPE, IDC_MODE_MONO, IDC_MODE_AA,
    IDC_LBL_CHARSET, IDC_CHARSET,
    IDC_LBL_H, IDC_H_EDIT, IDC_H_SPIN,
    IDC_LBL_W, IDC_W_EDIT, IDC_W_SPIN,
    IDC_LBL_SIZE, IDC_SIZE_EDIT, IDC_SIZE_SPIN,
    IDC_GENERATE, IDC_HDR_PREV, IDC_INFO, IDC_CODE, IDC_STATUS, IDC_BULK,
};

const UINT_PTR TIMER_REGEN = 1;
const int MAX_CELL = 256;
const int MAX_FONT_PX = 512;

struct App {
    HINSTANCE hInst = nullptr;
    HWND hMain = nullptr, hDrop = nullptr, hGrid = nullptr, hView = nullptr;
    HWND hInfo = nullptr, hCode = nullptr, hStatus = nullptr;
    HFONT uiFont = nullptr, boldFont = nullptr, monoFont = nullptr;
    HBRUSH whiteBrush = nullptr;
    int dpi = 96;

    FontFile font;
    std::vector<uint32_t> cps;
    RenderResult result;
    std::vector<std::vector<uint32_t>> dib;   // per-glyph 32bpp preview pixels
    int selected = 0;

    int gridScroll = 0;
    int gridHover = -1;
    bool dropHover = false;
    std::wstring lastSaved;
} g;

int S(int v) { return MulDiv(v, g.dpi, 96); }
HWND Ctl(int id) { return GetDlgItem(g.hMain, id); }

std::wstring WFmt(const wchar_t* f, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, f);
    _vsnwprintf(buf, 1023, f, ap);
    va_end(ap);
    buf[1023] = 0;
    return buf;
}

std::wstring Crlf(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size() + s.size() / 16);
    for (wchar_t c : s) {
        if (c == L'\n') o += L'\r';
        o += c;
    }
    return o;
}

int ReadInt(int id, int lo, int hi, int fallback) {
    BOOL ok = FALSE;
    int v = (int)GetDlgItemInt(g.hMain, id, &ok, FALSE);
    if (!ok) return fallback;
    return std::clamp(v, lo, hi);
}

// ---------------------------------------------------------------------------------------------
// Rendering state

void BuildDibCache() {
    const RenderResult& r = g.result;
    g.dib.assign(r.glyphs.size(), {});
    for (size_t i = 0; i < r.glyphs.size(); ++i) {
        const Glyph& gl = r.glyphs[i];
        const int bg = gl.present ? 0xFF : 0xE0;   // missing glyphs get a pink cell
        auto& d = g.dib[i];
        d.resize(gl.px.size());
        for (size_t k = 0; k < gl.px.size(); ++k) {
            int v = gl.px[k];
            int rr = 255 - (255 - 16) * v / 255;
            int gg = bg - (bg - 16) * v / 255;
            int bb = bg - (bg - 16) * v / 255;
            d[k] = (uint32_t)((rr << 16) | (gg << 8) | bb);
        }
    }
}

void BlitGlyph(HDC dc, size_t i, int x, int y, int zoom) {
    const RenderResult& r = g.result;
    if (i >= g.dib.size() || r.cellW <= 0 || r.cellH <= 0) return;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = r.cellW;
    bmi.bmiHeader.biHeight = -r.cellH;           // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, x, y, r.cellW * zoom, r.cellH * zoom, 0, 0, r.cellW, r.cellH,
                  g.dib[i].data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
}

void UpdatePreview();
void UpdateStatus();
void GridUpdateScroll();

struct Settings {
    const CharSet* charset;
    int cellW, cellH, fontPx;
    RenderMode mode;
};

Settings ReadSettings() {
    const auto& sets = BuiltinCharSets();
    int csIdx = (int)SendMessageW(Ctl(IDC_CHARSET), CB_GETCURSEL, 0, 0);
    if (csIdx < 0 || csIdx >= (int)sets.size()) csIdx = 1;
    Settings s;
    s.charset = &sets[csIdx];
    s.cellH = ReadInt(IDC_H_EDIT, 1, MAX_CELL, 16);
    s.cellW = ReadInt(IDC_W_EDIT, 1, MAX_CELL, 12);
    s.fontPx = ReadInt(IDC_SIZE_EDIT, 0, MAX_FONT_PX, 0);
    s.mode = IsDlgButtonChecked(g.hMain, IDC_MODE_AA) == BST_CHECKED ? RenderMode::Gray8 : RenderMode::Mono1;
    return s;
}

void Regenerate() {
    KillTimer(g.hMain, TIMER_REGEN);
    const Settings s = ReadSettings();
    g.cps = ExpandCharSet(*s.charset);
    if (g.font.IsLoaded()) {
        HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
        g.result = g.font.Render(g.cps, s.cellW, s.cellH, s.mode, s.fontPx);
        SetCursor(old);
    } else {
        g.result = RenderResult{};
    }
    BuildDibCache();
    if (g.selected < 0 || g.selected >= (int)g.result.glyphs.size()) {
        auto it = std::find(g.cps.begin(), g.cps.end(), (uint32_t)'A');   // default to a representative char
        g.selected = (it != g.cps.end() && !g.result.glyphs.empty()) ? (int)(it - g.cps.begin()) : 0;
    }

    EnableWindow(Ctl(IDC_GENERATE), g.font.IsLoaded());
    GridUpdateScroll();
    InvalidateRect(g.hGrid, nullptr, FALSE);
    UpdatePreview();
    UpdateStatus();
}

void UpdateStatus() {
    std::wstring s;
    if (!g.font.IsLoaded()) {
        s = L"No font loaded. Drop a .ttf or .otf file into the window.";
    } else {
        const RenderResult& r = g.result;
        s = WFmt(L"%s   |   %d chars   |   cell %d x %d px   |   font %d px%s   |   %d missing   |   %d clipped",
                 g.font.DisplayName().c_str(), (int)r.glyphs.size(), r.cellW, r.cellH, r.fontPx,
                 r.autoFit ? L" (auto)" : L"", r.missing, r.clipped);
        if (!g.lastSaved.empty()) s += L"   |   saved: " + g.lastSaved;
    }
    SendMessageW(g.hStatus, SB_SETTEXTW, 0, (LPARAM)s.c_str());
}

void UpdatePreview() {
    const RenderResult& r = g.result;
    if (g.selected < 0 || g.selected >= (int)r.glyphs.size()) {
        SetWindowTextW(g.hInfo, L"");
        SetWindowTextW(g.hCode, L"");
    } else {
        const Glyph& gl = r.glyphs[g.selected];
        std::wstring ch = gl.cp == 0x20 ? L"SP" : L"'" + std::wstring(1, (wchar_t)gl.cp) + L"'";
        std::wstring info = WFmt(L"U+%04X  %s    #%d of %d    advance %d px", (unsigned)gl.cp,
                                 ch.c_str(), g.selected, (int)r.glyphs.size(), gl.advance);
        if (!gl.present) info += L"    MISSING";
        else if (gl.clipped) info += L"    CLIPPED";
        SetWindowTextW(g.hInfo, info.c_str());
        SetWindowTextW(g.hCode, Crlf(FromUtf8(GenerateGlyphSnippet(r, g.selected))).c_str());
    }
    InvalidateRect(g.hView, nullptr, FALSE);
}

// ---------------------------------------------------------------------------------------------
// File handling

void LoadFontFile(const std::wstring& path) {
    std::wstring err;
    if (!g.font.Load(path, err)) {
        MessageBoxW(g.hMain, (path + L"\n\n" + err).c_str(), APP_TITLE, MB_ICONWARNING);
        SetWindowTextW(g.hMain, APP_TITLE);
    } else {
        SetWindowTextW(g.hMain, (std::wstring(APP_TITLE) + L" - " + g.font.FileName()).c_str());
    }
    g.selected = -1;
    g.gridScroll = 0;
    g.lastSaved.clear();
    InvalidateRect(g.hDrop, nullptr, FALSE);
    Regenerate();
}

void OpenFontDialog() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g.hMain;
    ofn.lpstrFilter = L"Fonts (*.ttf, *.otf)\0*.ttf;*.otf;*.ttc\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Open font";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) LoadFontFile(file);
}

std::wstring SafeFileName(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) o += (c < 32 || wcschr(L"<>:\"/\\|?*", c)) ? L'_' : c;
    return o.empty() ? L"font" : o;
}

bool WriteFileBytes(const std::wstring& path, const std::string& bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr) &&
              written == bytes.size();
    CloseHandle(h);
    return ok;
}

std::wstring WithSlash(std::wstring dir) {
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    return dir;
}

// Writes {fontStem}-Code-{YYYYMMDD-HHMMSS}.txt into dir. Adds -2, -3... if that name is taken
// (e.g. Foo.ttf and Foo.otf in the same bulk run). Returns the written path, or empty on failure.
std::wstring SaveCFile(const FontFile& font, const RenderResult& r, const CharSet& cs,
                       const std::wstring& dir) {
    std::string text = GenerateCFile(r, font, cs);
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::wstring base = WithSlash(dir) + SafeFileName(font.FileStem()) +
                        WFmt(L"-Code-%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay,
                             st.wHour, st.wMinute, st.wSecond);
    std::wstring path = base + L".txt";
    for (int n = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++n)
        path = base + WFmt(L"-%d.txt", n);
    return WriteFileBytes(path, text) ? path : std::wstring();
}

void GenerateAndSave() {
    if (!g.font.IsLoaded()) return;
    Regenerate();   // make sure pending edits are applied
    const CharSet& cs = *ReadSettings().charset;

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);

    std::wstring path = SaveCFile(g.font, g.result, cs, dir);
    if (path.empty()) {
        // The app folder may be read-only (e.g. Program Files); fall back to Documents.
        wchar_t docs[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs)))
            path = SaveCFile(g.font, g.result, cs, docs);
        if (path.empty()) {
            MessageBoxW(g.hMain, L"Could not write the output file to the app folder or Documents.",
                        APP_TITLE, MB_ICONERROR);
            return;
        }
    }
    g.lastSaved = path;
    UpdateStatus();

    if ((INT_PTR)ShellExecuteW(g.hMain, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL) <= 32)
        ShellExecuteW(g.hMain, nullptr, L"notepad.exe", (L"\"" + path + L"\"").c_str(), nullptr,
                      SW_SHOWNORMAL);
}

std::wstring PickFolder(const std::wstring& initial) {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return result;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Bulk processing - choose a folder with fonts");
    if (!initial.empty()) {
        IShellItem* start = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&start)))) {
            dlg->SetFolder(start);
            start->Release();
        }
    }
    if (SUCCEEDED(dlg->Show(g.hMain))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

std::vector<std::wstring> ListFontFiles(const std::wstring& dir) {
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((WithSlash(dir) + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return files;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const wchar_t* ext = wcsrchr(fd.cFileName, L'.');
        if (ext && (!_wcsicmp(ext, L".ttf") || !_wcsicmp(ext, L".otf") || !_wcsicmp(ext, L".ttc")))
            files.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(files.begin(), files.end(),
              [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return files;
}

void SetStatusNow(const std::wstring& s) {
    SendMessageW(g.hStatus, SB_SETTEXTW, 0, (LPARAM)s.c_str());
    UpdateWindow(g.hStatus);
}

// Converts every font in a folder with the current UI settings; the .txt files go to that folder.
void BulkProcess() {
    std::wstring startDir;
    if (g.font.IsLoaded()) startDir = g.font.Path().substr(0, g.font.Path().find_last_of(L"\\/"));
    std::wstring dir = PickFolder(startDir);
    if (dir.empty()) return;

    std::vector<std::wstring> files = ListFontFiles(dir);
    if (files.empty()) {
        MessageBoxW(g.hMain, (L"No .ttf / .otf fonts found in:\n" + dir).c_str(), APP_TITLE, MB_ICONINFORMATION);
        return;
    }

    Regenerate();   // apply pending edits
    const Settings s = ReadSettings();
    const std::vector<uint32_t> cps = ExpandCharSet(*s.charset);

    // Release the previewed font so a bulk font with the same family name cannot be confused with it.
    const std::wstring previewPath = g.font.Path();
    g.font.Unload();

    HCURSOR oldCursor = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    int ok = 0, withMissing = 0;
    std::wstring failures;
    for (size_t i = 0; i < files.size(); ++i) {
        SetStatusNow(WFmt(L"Bulk processing %d / %d:  %s", (int)i + 1, (int)files.size(), files[i].c_str()));
        FontFile font;
        std::wstring err;
        if (!font.Load(WithSlash(dir) + files[i], err)) {
            failures += L"\n" + files[i] + L" - " + err;
            continue;
        }
        RenderResult r = font.Render(cps, s.cellW, s.cellH, s.mode, s.fontPx);
        if (SaveCFile(font, r, *s.charset, dir).empty()) {
            failures += L"\n" + files[i] + L" - could not write the output file";
            continue;
        }
        ++ok;
        if (r.missing) ++withMissing;
    }
    SetCursor(oldCursor);

    if (!previewPath.empty()) {
        std::wstring err;
        if (!g.font.Load(previewPath, err)) SetWindowTextW(g.hMain, APP_TITLE);
    }
    g.lastSaved.clear();
    InvalidateRect(g.hDrop, nullptr, FALSE);
    Regenerate();

    std::wstring msg = WFmt(L"Converted %d of %d font(s) into:\n%s", ok, (int)files.size(), dir.c_str());
    if (withMissing) msg += WFmt(L"\n\n%d font(s) lacked some chars - those were replaced with spaces.", withMissing);
    if (!failures.empty()) msg += L"\n\nFailed:" + failures;
    msg += L"\n\nOpen the folder?";
    if (MessageBoxW(g.hMain, msg.c_str(), APP_TITLE,
                    MB_YESNO | (failures.empty() ? MB_ICONINFORMATION : MB_ICONWARNING)) == IDYES)
        ShellExecuteW(g.hMain, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------------------------
// Drop zone

void TrackLeave(HWND h) {
    TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
    TrackMouseEvent(&t);
}

LRESULT CALLBACK DropProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);

        FillRect(mem, &rc, GetSysColorBrush(COLOR_BTNFACE));
        HBRUSH fill = CreateSolidBrush(g.dropHover ? RGB(232, 241, 255) : RGB(250, 251, 253));
        HPEN pen = CreatePen(PS_DASH, 1, g.dropHover ? RGB(40, 110, 220) : RGB(130, 145, 170));
        HGDIOBJ oldPen = SelectObject(mem, pen);
        HGDIOBJ oldBrush = SelectObject(mem, fill);
        SetBkColor(mem, g.dropHover ? RGB(232, 241, 255) : RGB(250, 251, 253));
        RoundRect(mem, 0, 0, rc.right, rc.bottom, S(10), S(10));
        SelectObject(mem, oldPen);
        SelectObject(mem, oldBrush);
        DeleteObject(pen);
        DeleteObject(fill);

        SetBkMode(mem, TRANSPARENT);
        RECT top = rc, bottom = rc;
        top.bottom = rc.bottom / 2 + S(2);
        bottom.top = rc.bottom / 2 + S(4);
        std::wstring line1, line2;
        if (g.font.IsLoaded()) {
            line1 = g.font.DisplayName();
            line2 = g.font.FileName() + L"   -   drop another font here or click to browse";
        } else {
            line1 = L"Drop a .TTF / .OTF font here";
            line2 = L"or click to browse";
        }
        SelectObject(mem, g.boldFont);
        SetTextColor(mem, RGB(30, 40, 60));
        DrawTextW(mem, line1.c_str(), -1, &top, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(mem, g.uiFont);
        SetTextColor(mem, RGB(100, 110, 130));
        DrawTextW(mem, line2.c_str(), -1, &bottom, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;
    case WM_MOUSEMOVE:
        if (!g.dropHover) {
            g.dropHover = true;
            TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        g.dropHover = false;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        OpenFontDialog();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------
// Glyph grid

struct GridGeom {
    int zoom, pad, label, boxW, boxH, cols, rows, originX, contentH;
};

GridGeom CalcGrid() {
    RECT rc;
    GetClientRect(g.hGrid, &rc);
    GridGeom m{};
    int cw = std::max(1, g.result.cellW), ch = std::max(1, g.result.cellH);
    int target = S(44);
    m.zoom = std::max(1, std::min(target / cw, target / ch));
    m.pad = S(6);
    m.label = S(16);
    m.boxW = std::max(cw * m.zoom + 2 * m.pad, S(40));
    m.boxH = ch * m.zoom + 2 * m.pad + m.label;
    m.cols = std::max(1, (int)(rc.right - S(8)) / m.boxW);
    int n = (int)g.result.glyphs.size();
    m.rows = (n + m.cols - 1) / m.cols;
    m.originX = std::max(S(4), (int)(rc.right - m.cols * m.boxW) / 2);
    m.contentH = m.rows * m.boxH + S(8);
    return m;
}

RECT GridBox(const GridGeom& m, int i) {
    int x = m.originX + (i % m.cols) * m.boxW;
    int y = S(4) + (i / m.cols) * m.boxH - g.gridScroll;
    return RECT{x, y, x + m.boxW, y + m.boxH};
}

void GridUpdateScroll() {
    RECT rc;
    GetClientRect(g.hGrid, &rc);
    GridGeom m = CalcGrid();
    int maxScroll = std::max(0, m.contentH - (int)rc.bottom);
    g.gridScroll = std::clamp(g.gridScroll, 0, maxScroll);
    SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL};
    si.nMin = 0;
    si.nMax = std::max(0, m.contentH - 1);
    si.nPage = (UINT)rc.bottom;
    si.nPos = g.gridScroll;
    SetScrollInfo(g.hGrid, SB_VERT, &si, TRUE);
}

void GridScrollTo(int pos) {
    g.gridScroll = pos;
    GridUpdateScroll();
    InvalidateRect(g.hGrid, nullptr, FALSE);
}

void GridSelect(int i) {
    int n = (int)g.result.glyphs.size();
    if (n == 0) return;
    g.selected = std::clamp(i, 0, n - 1);
    GridGeom m = CalcGrid();
    RECT rc;
    GetClientRect(g.hGrid, &rc);
    RECT b = GridBox(m, g.selected);
    if (b.top < 0) GridScrollTo(g.gridScroll + b.top - S(4));
    else if (b.bottom > rc.bottom) GridScrollTo(g.gridScroll + (b.bottom - rc.bottom) + S(4));
    InvalidateRect(g.hGrid, nullptr, FALSE);
    UpdatePreview();
}

int GridHitTest(int x, int y) {
    GridGeom m = CalcGrid();
    for (int i = 0; i < (int)g.result.glyphs.size(); ++i) {
        RECT b = GridBox(m, i);
        POINT p{x, y};
        if (PtInRect(&b, p)) return i;
    }
    return -1;
}

void GridPaint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    FillRect(mem, &rc, g.whiteBrush);
    SetBkMode(mem, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(mem, g.uiFont);

    const RenderResult& r = g.result;
    if (r.glyphs.empty()) {
        SetTextColor(mem, RGB(150, 150, 150));
        DrawTextW(mem, L"Font preview will appear here", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        GridGeom m = CalcGrid();
        HBRUSH selBrush = CreateSolidBrush(RGB(222, 234, 252));
        HBRUSH hovBrush = CreateSolidBrush(RGB(243, 246, 251));
        HBRUSH frameBrush = CreateSolidBrush(RGB(205, 208, 214));
        HBRUSH selFrame = CreateSolidBrush(RGB(40, 110, 220));
        for (int i = 0; i < (int)r.glyphs.size(); ++i) {
            RECT b = GridBox(m, i);
            if (b.bottom < 0 || b.top > rc.bottom) continue;
            RECT inner = {b.left + 1, b.top + 1, b.right - 1, b.bottom - 1};
            if (i == g.selected) {
                FillRect(mem, &inner, selBrush);
                FrameRect(mem, &inner, selFrame);
            } else if (i == g.gridHover) {
                FillRect(mem, &inner, hovBrush);
            }
            int gw = r.cellW * m.zoom, gh = r.cellH * m.zoom;
            int gx = b.left + (m.boxW - gw) / 2, gy = b.top + m.pad;
            BlitGlyph(mem, i, gx, gy, m.zoom);
            RECT fr = {gx - 1, gy - 1, gx + gw + 1, gy + gh + 1};
            FrameRect(mem, &fr, frameBrush);

            const Glyph& gl = r.glyphs[i];
            wchar_t lbl[8] = {};
            if (gl.cp == 0x20) wcscpy(lbl, L"SP");
            else lbl[0] = (wchar_t)gl.cp;
            RECT lr = {b.left, gy + gh + S(1), b.right, b.bottom - S(1)};
            SetTextColor(mem, !gl.present ? RGB(200, 40, 40) : gl.clipped ? RGB(200, 120, 0) : RGB(90, 95, 105));
            DrawTextW(mem, lbl, -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        DeleteObject(selBrush);
        DeleteObject(hovBrush);
        DeleteObject(frameBrush);
        DeleteObject(selFrame);
    }

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

LRESULT CALLBACK GridProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        GridPaint(h);
        return 0;
    case WM_SIZE:
        GridUpdateScroll();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO si{sizeof si, SIF_ALL};
        GetScrollInfo(h, SB_VERT, &si);
        int row = CalcGrid().boxH;
        int pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= row; break;
        case SB_LINEDOWN: pos += row; break;
        case SB_PAGEUP: pos -= (int)si.nPage; break;
        case SB_PAGEDOWN: pos += (int)si.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = si.nMax; break;
        }
        GridScrollTo(pos);
        return 0;
    }
    case WM_MOUSEWHEEL:
        GridScrollTo(g.gridScroll - GET_WHEEL_DELTA_WPARAM(wp) * CalcGrid().boxH / WHEEL_DELTA);
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(h);
        int i = GridHitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (i >= 0) GridSelect(i);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int i = GridHitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (i != g.gridHover) {
            g.gridHover = i;
            TrackLeave(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g.gridHover = -1;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_KEYDOWN: {
        int cols = CalcGrid().cols;
        switch (wp) {
        case VK_LEFT: GridSelect(g.selected - 1); break;
        case VK_RIGHT: GridSelect(g.selected + 1); break;
        case VK_UP: GridSelect(g.selected - cols); break;
        case VK_DOWN: GridSelect(g.selected + cols); break;
        case VK_HOME: GridSelect(0); break;
        case VK_END: GridSelect((int)g.result.glyphs.size() - 1); break;
        }
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------
// Enlarged single-glyph view

LRESULT CALLBACK ViewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        HBRUSH bg = CreateSolidBrush(RGB(238, 240, 244));
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
        SetBkMode(mem, TRANSPARENT);
        HGDIOBJ oldFont = SelectObject(mem, g.uiFont);

        const RenderResult& r = g.result;
        if (g.selected < 0 || g.selected >= (int)r.glyphs.size() || r.cellW <= 0) {
            SetTextColor(mem, RGB(150, 150, 150));
            DrawTextW(mem, L"Click a char to preview it", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            int pad = S(10);
            int zoom = std::max(1, std::min(((int)rc.right - 2 * pad) / r.cellW,
                                            ((int)rc.bottom - 2 * pad) / r.cellH));
            int gw = r.cellW * zoom, gh = r.cellH * zoom;
            int ox = (rc.right - gw) / 2, oy = (rc.bottom - gh) / 2;
            BlitGlyph(mem, g.selected, ox, oy, zoom);

            if (zoom >= 4) {
                HPEN grid = CreatePen(PS_SOLID, 1, RGB(210, 213, 220));
                HGDIOBJ op = SelectObject(mem, grid);
                for (int x = 1; x < r.cellW; ++x) { MoveToEx(mem, ox + x * zoom, oy, nullptr); LineTo(mem, ox + x * zoom, oy + gh); }
                for (int y = 1; y < r.cellH; ++y) { MoveToEx(mem, ox, oy + y * zoom, nullptr); LineTo(mem, ox + gw, oy + y * zoom); }
                SelectObject(mem, op);
                DeleteObject(grid);
            }
            if (r.baseline > 0 && r.baseline < r.cellH) {
                HPEN base = CreatePen(PS_SOLID, 1, RGB(230, 60, 60));
                HGDIOBJ op = SelectObject(mem, base);
                int by = oy + r.baseline * zoom;
                MoveToEx(mem, ox - S(6), by, nullptr);
                LineTo(mem, ox + gw + S(6), by);
                SelectObject(mem, op);
                DeleteObject(base);
            }
            HBRUSH frame = CreateSolidBrush(RGB(120, 128, 140));
            RECT fr = {ox - 1, oy - 1, ox + gw + 1, oy + gh + 1};
            FrameRect(mem, &fr, frame);
            DeleteObject(frame);
        }

        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldFont);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------
// Main window

HWND MakeChild(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0) {
    return CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g.hMain,
                           (HMENU)(INT_PTR)id, g.hInst, nullptr);
}

void MakeSpin(int editId, int spinId, int lo, int hi, int value) {
    MakeChild(L"EDIT", L"", WS_TABSTOP | ES_NUMBER | ES_RIGHT, editId, WS_EX_CLIENTEDGE);
    HWND spin = MakeChild(UPDOWN_CLASSW, L"",
                          UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, spinId);
    SendMessageW(spin, UDM_SETBUDDY, (WPARAM)Ctl(editId), 0);
    SendMessageW(spin, UDM_SETRANGE32, lo, hi);
    SendMessageW(spin, UDM_SETPOS32, 0, value);
}

void CreateControls() {
    g.hDrop = MakeChild(L"DopplerDrop", L"", 0, IDC_DROP);
    g.hGrid = MakeChild(L"DopplerGrid", L"", WS_VSCROLL | WS_TABSTOP, IDC_GRID, WS_EX_CLIENTEDGE);
    g.hView = MakeChild(L"DopplerView", L"", 0, IDC_VIEW, WS_EX_CLIENTEDGE);

    MakeChild(L"STATIC", L"SETTINGS", 0, IDC_HDR_SET);
    MakeChild(L"STATIC", L"Array type", 0, IDC_LBL_TYPE);
    MakeChild(L"BUTTON", L"1-bit (mono)", WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, IDC_MODE_MONO);
    MakeChild(L"BUTTON", L"Anti-aliased (8-bit)", BS_AUTORADIOBUTTON, IDC_MODE_AA);
    CheckRadioButton(g.hMain, IDC_MODE_MONO, IDC_MODE_AA, IDC_MODE_MONO);

    MakeChild(L"STATIC", L"Char set", 0, IDC_LBL_CHARSET);
    HWND cs = MakeChild(L"COMBOBOX", L"", WS_TABSTOP | WS_GROUP | CBS_DROPDOWNLIST | WS_VSCROLL, IDC_CHARSET);
    for (const auto& set : BuiltinCharSets()) SendMessageW(cs, CB_ADDSTRING, 0, (LPARAM)set.uiName);
    SendMessageW(cs, CB_SETCURSEL, 1, 0);

    MakeChild(L"STATIC", L"Char height, px", 0, IDC_LBL_H);
    MakeSpin(IDC_H_EDIT, IDC_H_SPIN, 1, MAX_CELL, 16);
    MakeChild(L"STATIC", L"Char width, px", 0, IDC_LBL_W);
    MakeSpin(IDC_W_EDIT, IDC_W_SPIN, 1, MAX_CELL, 12);
    MakeChild(L"STATIC", L"Font size, px (0 = auto fit)", 0, IDC_LBL_SIZE);
    MakeSpin(IDC_SIZE_EDIT, IDC_SIZE_SPIN, 0, MAX_FONT_PX, 0);

    MakeChild(L"BUTTON", L"Generate", WS_TABSTOP | BS_PUSHBUTTON, IDC_GENERATE);
    MakeChild(L"BUTTON", L"BULK PROCESSING", WS_TABSTOP | BS_PUSHBUTTON, IDC_BULK);
    MakeChild(L"STATIC", L"CHAR PREVIEW", 0, IDC_HDR_PREV);
    g.hInfo = MakeChild(L"STATIC", L"", SS_NOPREFIX | SS_ENDELLIPSIS, IDC_INFO);
    g.hCode = MakeChild(L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                        ES_AUTOVSCROLL | ES_AUTOHSCROLL, IDC_CODE, WS_EX_CLIENTEDGE);
    g.hStatus = MakeChild(STATUSCLASSNAMEW, L"", SBARS_SIZEGRIP, IDC_STATUS);

    EnumChildWindows(g.hMain, [](HWND c, LPARAM) -> BOOL {
        SendMessageW(c, WM_SETFONT, (WPARAM)g.uiFont, FALSE);
        return TRUE;
    }, 0);
    SendMessageW(Ctl(IDC_HDR_SET), WM_SETFONT, (WPARAM)g.boldFont, FALSE);
    SendMessageW(Ctl(IDC_HDR_PREV), WM_SETFONT, (WPARAM)g.boldFont, FALSE);
    SendMessageW(Ctl(IDC_GENERATE), WM_SETFONT, (WPARAM)g.boldFont, FALSE);
    SendMessageW(g.hCode, WM_SETFONT, (WPARAM)g.monoFont, FALSE);
}

void Layout() {
    RECT rc;
    GetClientRect(g.hMain, &rc);
    SendMessageW(g.hStatus, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(g.hStatus, &sr);
    const int statusH = sr.bottom - sr.top;
    const int cw = rc.right, ch = rc.bottom - statusH;

    const int m = S(10), rightW = S(330), dropH = S(64);
    const int leftW = std::max(S(100), cw - rightW - 3 * m);
    MoveWindow(g.hDrop, m, m, leftW, dropH, TRUE);
    MoveWindow(g.hGrid, m, m + dropH + m, leftW, std::max(S(50), ch - dropH - 3 * m), TRUE);

    const int x = m + leftW + m;
    const int labelW = S(170), fieldX = x + labelW, fieldW = rightW - labelW;
    const int rowH = S(24), gap = S(6);
    int y = m;
    auto put = [](int id, int X, int Y, int W, int H) { MoveWindow(Ctl(id), X, Y, W, H, TRUE); };

    put(IDC_HDR_SET, x, y, rightW, S(20)); y += S(26);
    put(IDC_LBL_TYPE, x, y + S(4), labelW, S(18));
    put(IDC_MODE_MONO, fieldX, y, fieldW, rowH); y += rowH;
    put(IDC_MODE_AA, fieldX, y, fieldW, rowH); y += rowH + gap;
    put(IDC_LBL_CHARSET, x, y + S(4), labelW, S(18));
    put(IDC_CHARSET, fieldX, y, fieldW, S(200)); y += rowH + gap;

    const int spins[3][3] = {{IDC_LBL_H, IDC_H_EDIT, IDC_H_SPIN},
                             {IDC_LBL_W, IDC_W_EDIT, IDC_W_SPIN},
                             {IDC_LBL_SIZE, IDC_SIZE_EDIT, IDC_SIZE_SPIN}};
    for (const auto& s : spins) {
        put(s[0], x, y + S(4), labelW, S(18));
        put(s[1], fieldX, y, fieldW, rowH);
        SendMessageW(Ctl(s[2]), UDM_SETBUDDY, (WPARAM)Ctl(s[1]), 0);   // re-attach to the right edge
        y += rowH + gap;
    }
    y += S(4);
    put(IDC_GENERATE, x, y, rightW, S(38)); y += S(38) + S(6);
    put(IDC_BULK, x, y, rightW, S(30)); y += S(30) + S(16);
    put(IDC_HDR_PREV, x, y, rightW, S(20)); y += S(26);

    const int bottom = ch - m;
    const int infoH = S(20);
    const int codeH = std::max(S(110), (bottom - y) * 42 / 100);
    const int viewH = std::max(S(60), bottom - y - codeH - infoH - 2 * gap);
    MoveWindow(g.hView, x, y, rightW, viewH, TRUE); y += viewH + gap;
    MoveWindow(g.hInfo, x, y, rightW, infoH, TRUE); y += infoH + gap;
    MoveWindow(g.hCode, x, y, rightW, std::max(S(40), bottom - y), TRUE);
}

void CreateFonts() {
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g.uiFont = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bold = ncm.lfMessageFont;
    bold.lfWeight = FW_BOLD;
    g.boldFont = CreateFontIndirectW(&bold);
    LOGFONTW mono{};
    mono.lfHeight = -MulDiv(9, g.dpi, 72);
    mono.lfCharSet = DEFAULT_CHARSET;
    mono.lfQuality = CLEARTYPE_QUALITY;
    wcscpy(mono.lfFaceName, L"Consolas");
    g.monoFont = CreateFontIndirectW(&mono);
}

LRESULT CALLBACK MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g.hMain = h;
        CreateControls();
        DragAcceptFiles(h, TRUE);
        return 0;
    case WM_SIZE:
        Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize = {S(760), S(620)};
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        wchar_t file[MAX_PATH] = {};
        if (DragQueryFileW(drop, 0, file, MAX_PATH)) {
            SetForegroundWindow(h);
            LoadFontFile(file);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if ((id == IDC_MODE_MONO || id == IDC_MODE_AA) && code == BN_CLICKED) Regenerate();
        else if (id == IDC_CHARSET && code == CBN_SELCHANGE) { g.selected = -1; g.gridScroll = 0; Regenerate(); }
        else if ((id == IDC_H_EDIT || id == IDC_W_EDIT || id == IDC_SIZE_EDIT) && code == EN_CHANGE)
            SetTimer(h, TIMER_REGEN, 250, nullptr);   // debounce typing
        else if (id == IDC_GENERATE && code == BN_CLICKED) GenerateAndSave();
        else if (id == IDC_BULK && code == BN_CLICKED) BulkProcess();
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_REGEN) Regenerate();
        return 0;
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT gr;
        GetWindowRect(g.hGrid, &gr);
        if (PtInRect(&gr, p)) return SendMessageW(g.hGrid, msg, wp, lp);
        break;
    }
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g.hCode) {
            SetBkColor((HDC)wp, RGB(255, 255, 255));
            SetTextColor((HDC)wp, RGB(20, 30, 50));
            return (LRESULT)g.whiteBrush;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void RegisterWndClass(const wchar_t* name, WNDPROC proc, HBRUSH bg, HCURSOR cursor) {
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = proc;
    wc.hInstance = g.hInst;
    wc.hCursor = cursor;
    wc.hbrBackground = bg;
    wc.lpszClassName = name;
    wc.hIcon = LoadIconW(g.hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow) {
    g.hInst = hInst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);   // folder picker
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_UPDOWN_CLASS};
    InitCommonControlsEx(&icc);

    HDC screen = GetDC(nullptr);
    g.dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    CreateFonts();
    g.whiteBrush = CreateSolidBrush(RGB(255, 255, 255));

    HCURSOR arrow = LoadCursor(nullptr, IDC_ARROW);
    RegisterWndClass(L"DopplerMain", MainProc, GetSysColorBrush(COLOR_BTNFACE), arrow);
    RegisterWndClass(L"DopplerDrop", DropProc, nullptr, arrow);
    RegisterWndClass(L"DopplerGrid", GridProc, nullptr, arrow);
    RegisterWndClass(L"DopplerView", ViewProc, nullptr, arrow);

    HWND hwnd = CreateWindowExW(0, L"DopplerMain", APP_TITLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, S(1120), S(760), nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) LoadFontFile(argv[1]);   // font dropped onto the exe
    else Regenerate();
    if (argv) LocalFree(argv);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
