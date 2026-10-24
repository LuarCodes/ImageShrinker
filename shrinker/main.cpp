// Image Shrinker - drop images, get smaller JPG/PNG copies in a "shrunk" folder next to the originals.
// Multi-threaded, fixes phone photo rotation (EXIF), never overwrites your originals.
#include "../common/ui.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "../third/stb_image.h"
#include "../third/zlib/zlib.h"
static unsigned char* zlibCompress(unsigned char* data, int len, int* outLen, int) {  // real zlib = much smaller PNGs
    uLongf n = compressBound(len); auto* out = (unsigned char*)malloc(n);
    if (compress2(out, &n, data, len, 9) != Z_OK) { free(out); return nullptr; }
    *outLen = (int)n; return out;
}
#define STBIW_ZLIB_COMPRESS zlibCompress
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../third/stb_image_write.h"
#include <thread>
#include <atomic>
#include <deque>
#include <memory>
#include <cmath>
using namespace ui;

enum { ID_KEEP = 100, ID_JPG, ID_PNG, ID_MAX, ID_QUAL, ID_ADD, ID_OPENDIR, ID_CLEAR, ID_LIST };
enum { WM_ITEM = WM_APP + 1 };
enum Fmt { KEEP = 0, JPG = 1, PNG = 2 };

struct Item {
    std::wstring path, out; std::atomic<int> state{0};  // 0 queued, 1 working, 2 done, 3 error
    unsigned long long before = 0, after = 0; std::wstring info; int row = -1;
};
struct Settings { int fmt = KEEP, maxSide = 1920, quality = 82; };

static HWND gWnd, gList, gFmtBtn[3], gMax, gQual, gOpenDir, gClear;
static Settings gSet;
static std::deque<std::unique_ptr<Item>> gItems;
static std::atomic<int> gPending{0};
static double gBatchStart = 0, gBatchMs = 0;
static std::wstring gLastDir;
static std::wstring gStatus = L"Ready  \x00B7  originals are never changed \x2014 copies go to a \x201Cshrunk\x201D folder";
static COLORREF gStatusColor = MUTED;
static void setStatus(const std::wstring& s, COLORREF c) { gStatus = s; gStatusColor = c; InvalidateRect(gWnd, nullptr, FALSE); }

// ---------- settings persistence ----------
static std::wstring cfgPath() {
    wchar_t p[MAX_PATH]; if (!GetEnvironmentVariableW(L"APPDATA", p, MAX_PATH)) return L"";
    std::wstring d = std::wstring(p) + L"\\QRClipTools"; CreateDirectoryW(d.c_str(), nullptr); return d + L"\\shrinker.txt";
}
static void loadCfg() { std::string s; if (readFile(cfgPath(), s)) sscanf(s.c_str(), "%d %d %d", &gSet.fmt, &gSet.maxSide, &gSet.quality);
    if (gSet.fmt < 0 || gSet.fmt > 2) gSet.fmt = KEEP; if (gSet.quality < 1 || gSet.quality > 100) gSet.quality = 82; if (gSet.maxSide < 0) gSet.maxSide = 1920; }
static void saveCfg() { char b[64]; int n = snprintf(b, 64, "%d %d %d\n", gSet.fmt, gSet.maxSide, gSet.quality); writeFile(cfgPath(), b, n); }

// ---------- image ops (RGBA8) ----------
struct Pix { int w = 0, h = 0; std::vector<uint8_t> d; };
static int exifOrientation(const std::string& f) {
    const uint8_t* p = (const uint8_t*)f.data(); size_t n = f.size();
    if (n < 4 || p[0] != 0xFF || p[1] != 0xD8) return 1;
    size_t i = 2;
    while (i + 4 < n && p[i] == 0xFF) {
        int marker = p[i + 1]; size_t len = (p[i + 2] << 8) | p[i + 3];
        if (marker == 0xE1 && i + 10 < n && !memcmp(p + i + 4, "Exif\0\0", 6)) {
            size_t t = i + 10; if (t + 8 > n) return 1;
            bool le = p[t] == 'I';
            auto u16 = [&](size_t o) -> unsigned { return o + 2 > n ? 0 : le ? p[o] | (p[o + 1] << 8) : (p[o] << 8) | p[o + 1]; };
            auto u32 = [&](size_t o) -> unsigned { return o + 4 > n ? 0 : le ? p[o] | (p[o + 1] << 8) | (p[o + 2] << 16) | ((unsigned)p[o + 3] << 24)
                                                                          : ((unsigned)p[o] << 24) | (p[o + 1] << 16) | (p[o + 2] << 8) | p[o + 3]; };
            size_t ifd = t + u32(t + 4); unsigned cnt = u16(ifd);
            for (unsigned k = 0; k < cnt && k < 512; k++) {
                size_t e = ifd + 2 + k * 12;
                if (u16(e) == 0x0112) { unsigned v = u16(e + 8); return v >= 1 && v <= 8 ? v : 1; }
            }
            return 1;
        }
        if (marker == 0xDA) break;
        i += 2 + len;
    }
    return 1;
}
static Pix orient(const Pix& s, int o) {
    if (o <= 1) return s;
    bool swap = o >= 5;
    Pix r; r.w = swap ? s.h : s.w; r.h = swap ? s.w : s.h; r.d.resize((size_t)r.w * r.h * 4);
    for (int y = 0; y < s.h; y++) for (int x = 0; x < s.w; x++) {
        int nx, ny;
        switch (o) {
        case 2: nx = s.w - 1 - x; ny = y; break;
        case 3: nx = s.w - 1 - x; ny = s.h - 1 - y; break;
        case 4: nx = x; ny = s.h - 1 - y; break;
        case 5: nx = y; ny = x; break;
        case 6: nx = s.h - 1 - y; ny = x; break;
        case 7: nx = s.h - 1 - y; ny = s.w - 1 - x; break;
        default: nx = y; ny = s.w - 1 - x; break;  // 8
        }
        memcpy(&r.d[((size_t)ny * r.w + nx) * 4], &s.d[((size_t)y * s.w + x) * 4], 4);
    }
    return r;
}
// high quality area-average downscale (separable box filter with fractional coverage)
static void axisWeights(int src, int dst, std::vector<int>& start, std::vector<std::vector<float>>& w) {
    double scale = (double)src / dst; start.resize(dst); w.resize(dst);
    for (int i = 0; i < dst; i++) {
        double a = i * scale, b = (i + 1) * scale; int s0 = (int)a, s1 = std::min(src, (int)ceil(b));
        start[i] = s0; w[i].clear();
        for (int s = s0; s < s1; s++) w[i].push_back((float)((std::min(b, s + 1.0) - std::max(a, (double)s)) / scale));
    }
}
static Pix resizeArea(const Pix& s, int W, int H) {  // streaming: memory = one row of floats
    std::vector<int> xs, ys; std::vector<std::vector<float>> xw, yw;
    axisWeights(s.w, W, xs, xw); axisWeights(s.h, H, ys, yw);
    Pix r; r.w = W; r.h = H; r.d.resize((size_t)W * H * 4);
    std::vector<float> acc((size_t)s.w * 4);
    for (int y = 0; y < H; y++) {
        std::fill(acc.begin(), acc.end(), 0.f);
        for (size_t k = 0; k < yw[y].size(); k++) {
            const uint8_t* row = &s.d[(size_t)(ys[y] + k) * s.w * 4]; float wt = yw[y][k];
            for (size_t i = 0; i < acc.size(); i++) acc[i] += row[i] * wt;
        }
        uint8_t* out = &r.d[(size_t)y * W * 4];
        for (int x = 0; x < W; x++) {
            float c[4] = {0, 0, 0, 0};
            for (size_t k = 0; k < xw[x].size(); k++) { const float* p = &acc[(size_t)(xs[x] + k) * 4]; float wt = xw[x][k];
                c[0] += p[0] * wt; c[1] += p[1] * wt; c[2] += p[2] * wt; c[3] += p[3] * wt; }
            for (int ch = 0; ch < 4; ch++) out[x * 4 + ch] = (uint8_t)std::min(255.f, c[ch] + 0.5f);
        }
    }
    return r;
}
static void writeFn(void* ctx, void* data, int size) { auto* v = (std::vector<uint8_t>*)ctx; v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size); }
static std::wstring lowerExt(const std::wstring& p) {
    size_t d = p.find_last_of(L'.'), s = p.find_last_of(L"\\/");
    if (d == std::wstring::npos || (s != std::wstring::npos && d < s)) return L"";
    std::wstring e = p.substr(d + 1); for (auto& c : e) c = towlower(c); return e;
}
static bool isImageExt(const std::wstring& e) {
    return e == L"jpg" || e == L"jpeg" || e == L"png" || e == L"bmp" || e == L"gif" || e == L"tga" || e == L"psd" || e == L"jfif";
}

static void processItem(Item* it, Settings st) {
    it->state = 1;
    std::string src;
    if (!readFile(it->path, src, 1024ull << 20)) { it->info = L"Could not read file"; it->state = 3; return; }
    it->before = src.size();
    int w, h, n;
    stbi_uc* p = stbi_load_from_memory((const stbi_uc*)src.data(), (int)src.size(), &w, &h, &n, 4);
    if (!p) { it->info = L"Unsupported image"; it->state = 3; return; }
    Pix img; img.w = w; img.h = h; img.d.assign(p, p + (size_t)w * h * 4); stbi_image_free(p);
    bool alpha = false;
    if (n == 2 || n == 4) for (size_t i = 3; i < img.d.size(); i += 4) if (img.d[i] != 255) { alpha = true; break; }
    std::wstring ext = lowerExt(it->path);
    bool srcJpg = ext == L"jpg" || ext == L"jpeg" || ext == L"jfif";
    int o = srcJpg ? exifOrientation(src) : 1;
    img = orient(img, o);
    bool resized = false;
    if (st.maxSide > 0 && std::max(img.w, img.h) > st.maxSide) {
        double sc = (double)st.maxSide / std::max(img.w, img.h);
        img = resizeArea(img, std::max(1, (int)lround(img.w * sc)), std::max(1, (int)lround(img.h * sc)));
        resized = true;
    }
    int fmtOut = st.fmt == KEEP ? (srcJpg ? JPG : (ext == L"png" || alpha) ? PNG : JPG) : st.fmt;
    std::vector<uint8_t> out;
    if (fmtOut == JPG) {
        std::vector<uint8_t> rgb((size_t)img.w * img.h * 3);
        for (size_t i = 0, j = 0; i < img.d.size(); i += 4, j += 3) {
            unsigned a = img.d[i + 3];
            for (int c = 0; c < 3; c++) rgb[j + c] = (uint8_t)((img.d[i + c] * a + 255 * (255 - a)) / 255);
        }
        stbi_write_jpg_to_func(writeFn, &out, img.w, img.h, 3, rgb.data(), st.quality);
    } else {
        stbi_write_png_compression_level = 9;
        if (alpha) stbi_write_png_to_func(writeFn, &out, img.w, img.h, 4, img.d.data(), img.w * 4);
        else {
            std::vector<uint8_t> rgb((size_t)img.w * img.h * 3);
            for (size_t i = 0, j = 0; i < img.d.size(); i += 4, j += 3) memcpy(&rgb[j], &img.d[i], 3);
            stbi_write_png_to_func(writeFn, &out, img.w, img.h, 3, rgb.data(), img.w * 3);
        }
    }
    if (out.empty()) { it->info = L"Encoding failed"; it->state = 3; return; }
    bool sameFmt = (fmtOut == JPG && srcJpg) || (fmtOut == PNG && ext == L"png");
    bool keptOriginal = false;
    if (!resized && o <= 1 && sameFmt && out.size() >= src.size()) { out.assign(src.begin(), src.end()); keptOriginal = true; }

    size_t sl = it->path.find_last_of(L"\\/");
    std::wstring dir = it->path.substr(0, sl), name = it->path.substr(sl + 1);
    size_t dot = name.find_last_of(L'.'); if (dot != std::wstring::npos) name = name.substr(0, dot);
    std::wstring outDir = dir + L"\\shrunk"; CreateDirectoryW(outDir.c_str(), nullptr);
    it->out = outDir + L"\\" + name + (fmtOut == JPG ? L".jpg" : L".png");
    if (!writeFile(it->out, out.data(), out.size())) { it->info = L"Could not write output"; it->state = 3; return; }
    it->after = out.size();
    it->info = fmt(L"%d\x00D7%d  %ls", img.w, img.h, fmtOut == JPG ? L"JPG" : L"PNG");
    if (keptOriginal) it->info += L"  (already optimal)";
    it->state = 2;
}

// ---------- list ----------
static void setCell(int row, int col, const std::wstring& t) {
    LVITEMW li = {}; li.iSubItem = col; li.pszText = (LPWSTR)t.c_str();
    SendMessageW(gList, LVM_SETITEMTEXTW, row, (LPARAM)&li);
}
static void refreshRow(Item* it) {
    setCell(it->row, 1, it->before ? sizeStr(it->before) : L"");
    int s = it->state;
    if (s == 2) {
        setCell(it->row, 2, sizeStr(it->after));
        double pct = it->before ? 100.0 * ((double)it->before - (double)it->after) / it->before : 0;
        setCell(it->row, 3, fabs(pct) < 0.5 ? L"0%" : pct > 0 ? fmt(L"-%.0f%%", pct) : fmt(L"+%.0f%%", -pct));
        setCell(it->row, 4, it->info);
    } else if (s == 3) { setCell(it->row, 2, L"\x2014"); setCell(it->row, 3, L""); setCell(it->row, 4, it->info); }
    else setCell(it->row, 4, s == 1 ? L"Working\x2026" : L"Queued");
}
static void updateSummary() {
    int done = 0, err = 0; unsigned long long b = 0, a = 0;
    for (auto& it : gItems) { if (it->state == 2) { done++; b += it->before; a += it->after; } else if (it->state == 3) err++; }
    if (gPending > 0) { setStatus(fmt(L"Shrinking\x2026 %d left", (int)gPending), MUTED); return; }
    if (!done && !err) return;
    double pct = b ? 100.0 * ((double)b - (double)a) / b : 0;
    std::wstring s = fmt(L"\x2713 %d image%ls  \x00B7  %ls \x2192 %ls  (-%.0f%%)  \x00B7  %.0f ms", done, done == 1 ? L"" : L"s",
                         sizeStr(b).c_str(), sizeStr(a).c_str(), pct, gBatchMs);
    if (err) s += fmt(L"  \x00B7  %d failed", err);
    setStatus(s, err && !done ? ERR : OK);
}
static void readSettings() {
    gSet.maxSide = _wtoi(getText(gMax).c_str());
    int q = _wtoi(getText(gQual).c_str()); gSet.quality = q < 1 ? 1 : q > 100 ? 100 : q;
    saveCfg();
}
static void addFiles(const std::vector<std::wstring>& in) {
    std::vector<std::wstring> files;
    for (auto& p : in) {
        DWORD a = GetFileAttributesW(p.c_str());
        if (a == INVALID_FILE_ATTRIBUTES) continue;
        if (a & FILE_ATTRIBUTE_DIRECTORY) {
            WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW((p + L"\\*").c_str(), &fd);
            if (f != INVALID_HANDLE_VALUE) { do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && isImageExt(lowerExt(fd.cFileName))) files.push_back(p + L"\\" + fd.cFileName); } while (FindNextFileW(f, &fd)); FindClose(f); }
        } else if (isImageExt(lowerExt(p))) files.push_back(p);
    }
    if (files.empty()) { setStatus(L"No supported images (JPG, PNG, BMP, GIF, TGA, PSD)", WARN); return; }
    readSettings();
    std::vector<Item*> batch;
    for (auto& f : files) {
        auto it = std::make_unique<Item>(); it->path = f;
        LVITEMW li = {}; li.mask = LVIF_TEXT | LVIF_PARAM; li.iItem = (int)SendMessageW(gList, LVM_GETITEMCOUNT, 0, 0);
        std::wstring nm = baseName(f); li.pszText = (LPWSTR)nm.c_str(); li.lParam = (LPARAM)it.get();
        it->row = (int)SendMessageW(gList, LVM_INSERTITEMW, 0, (LPARAM)&li);
        batch.push_back(it.get()); refreshRow(it.get());
        gItems.push_back(std::move(it));
    }
    ShowWindow(gList, SW_SHOW); EnableWindow(gClear, TRUE);
    size_t sl = files[0].find_last_of(L"\\/"); gLastDir = files[0].substr(0, sl) + L"\\shrunk";
    if (gPending == 0) gBatchStart = nowMs();
    gPending += (int)batch.size();
    updateSummary();
    auto idx = std::make_shared<std::atomic<size_t>>(0);
    Settings st = gSet;
    unsigned nt = std::max(1u, std::min(std::thread::hardware_concurrency(), (unsigned)batch.size()));
    for (unsigned t = 0; t < nt; t++)
        std::thread([batch, idx, st] {
            for (size_t i; (i = (*idx)++) < batch.size();) {
                PostMessageW(gWnd, WM_ITEM, 0, (LPARAM)batch[i]);
                processItem(batch[i], st);
                PostMessageW(gWnd, WM_ITEM, 1, (LPARAM)batch[i]);
            }
        }).detach();
}

static void syncFmt() { for (int i = 0; i < 3; i++) setOn(gFmtBtn[i], gSet.fmt == i); }

static void paint(HDC dc, RECT) {
    header(dc, L"Image Shrinker", L"drop images   \x2192   smaller JPG / PNG copies", 232);
    fillRound(dc, R(24, 70, 852, 94), S(20), CARD);
    text(dc, L"Output format", R(44, 80, 220, 20), fSmall, MUTED, LEFT1);
    text(dc, L"Max size (px)", R(282, 80, 120, 20), fSmall, MUTED, LEFT1);
    text(dc, L"JPG quality", R(402, 80, 100, 20), fSmall, MUTED, LEFT1);
    fillRound(dc, R(282, 104, 104, 40), S(12), FIELD, BORDER);
    fillRound(dc, R(402, 104, 84, 40), S(12), FIELD, BORDER);
    text(dc, L"0 = keep size", R(282, 146, 120, 16), fSmall, MUTED, LEFT1);
    fillRound(dc, R(24, 178, 852, 352), S(20), CARD);
    if (gItems.empty()) {
        RECT z = R(44, 198, 812, 312);
        HPEN pen = CreatePen(PS_DOT, 1, BORDER); HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, z.left, z.top, z.right, z.bottom, S(16), S(16)); SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);
        RECT t1 = R(44, 300, 812, 40), t2 = R(44, 340, 812, 30);
        text(dc, L"Drop images or folders here", t1, fTitle, TEXT, CENTER1);
        text(dc, L"JPG \x00B7 PNG \x00B7 BMP \x00B7 GIF \x00B7 TGA \x00B7 PSD   \x2014   or click \x201C" L"Add images\x201D", t2, fBody, MUTED, CENTER1);
    }
    text(dc, gStatus, R(26, 540, 740, 24), fSmall, gStatusColor, LEFT1);
}

static LRESULT CALLBACK listSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_NOTIFY) {
        auto* nm = (NMHDR*)l;
        if (nm->code == NM_CUSTOMDRAW && nm->hwndFrom == (HWND)SendMessageW(h, LVM_GETHEADER, 0, 0)) {
            auto* cd = (NMCUSTOMDRAW*)l;
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                fillRect(cd->hdc, cd->rc, CARD);
                wchar_t t[64] = L""; HDITEMW hi = {}; hi.mask = HDI_TEXT; hi.pszText = t; hi.cchTextMax = 64;
                SendMessageW(nm->hwndFrom, HDM_GETITEMW, cd->dwItemSpec, (LPARAM)&hi);
                RECT r = cd->rc; r.left += S(10);
                text(cd->hdc, t, r, fSmall, MUTED, LEFT1);
                return CDRF_SKIPDEFAULT;
            }
        }
    }
    return DefSubclassProc(h, m, w, l);
}

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        gWnd = h; loadCfg();
        const wchar_t* fn[3] = {L"Keep", L"JPG", L"PNG"};
        for (int i = 0; i < 3; i++) gFmtBtn[i] = button(h, ID_KEEP + i, fn[i], R(44 + i * 76, 104, 70, 40), TOGGLE);
        syncFmt();
        gMax = edit(h, ID_MAX, R(294, 113, 84, 22), ES_NUMBER | ES_AUTOHSCROLL);
        gQual = edit(h, ID_QUAL, R(414, 113, 64, 22), ES_NUMBER | ES_AUTOHSCROLL);
        SetWindowTextW(gMax, std::to_wstring(gSet.maxSide).c_str()); SetWindowTextW(gQual, std::to_wstring(gSet.quality).c_str());
        gClear = button(h, ID_CLEAR, L"Clear list", R(518, 104, 104, 40)); EnableWindow(gClear, FALSE);
        gOpenDir = button(h, ID_OPENDIR, L"Open folder", R(632, 104, 104, 40)); EnableWindow(gOpenDir, FALSE);
        button(h, ID_ADD, L"Add images", R(746, 104, 112, 40), PRIMARY);

        RECT lr = R(36, 190, 828, 328);
        gList = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | LVS_NOSORTHEADER | LVS_SHOWSELALWAYS,
                                lr.left, lr.top, lr.right - lr.left, lr.bottom - lr.top, h, (HMENU)ID_LIST, inst, nullptr);
        SendMessageW(gList, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        SendMessageW(gList, WM_SETFONT, (WPARAM)fBody, TRUE);
        SendMessageW(gList, LVM_SETBKCOLOR, 0, CARD); SendMessageW(gList, LVM_SETTEXTBKCOLOR, 0, CARD); SendMessageW(gList, LVM_SETTEXTCOLOR, 0, TEXT);
        SetWindowTheme(gList, L"DarkMode_Explorer", nullptr);
        SetWindowTheme((HWND)SendMessageW(gList, LVM_GETHEADER, 0, 0), L"DarkMode_ItemsView", nullptr);
        SetWindowSubclass(gList, listSub, 1, 0);
        SendMessageW(gList, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)ImageList_Create(1, S(28), ILC_COLOR32, 1, 1));  // taller rows
        struct { const wchar_t* t; int w; int fmt; } cols[] = {{L"File", 300, LVCFMT_LEFT}, {L"Before", 92, LVCFMT_RIGHT},
            {L"After", 92, LVCFMT_RIGHT}, {L"Saved", 70, LVCFMT_RIGHT}, {L"Result", 250, LVCFMT_LEFT}};
        for (int i = 0; i < 5; i++) { LVCOLUMNW c = {}; c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT; c.pszText = (LPWSTR)cols[i].t; c.cx = S(cols[i].w); c.fmt = cols[i].fmt;
            SendMessageW(gList, LVM_INSERTCOLUMNW, i, (LPARAM)&c); }
        DragAcceptFiles(h, TRUE);
        return 0;
    }
    case WM_DROPFILES: addFiles(dropped((HDROP)w)); return 0;
    case WM_ITEM: {
        auto* it = (Item*)l; refreshRow(it);
        if (w == 1) {
            gPending--;
            if (gPending == 0) { gBatchMs = nowMs() - gBatchStart; EnableWindow(gOpenDir, TRUE); }
            updateSummary();
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id >= ID_KEEP && id <= ID_PNG) { gSet.fmt = id - ID_KEEP; syncFmt(); saveCfg(); }
        else if (id == ID_ADD) addFiles(openFiles(h, L"Images\0*.jpg;*.jpeg;*.jfif;*.png;*.bmp;*.gif;*.tga;*.psd\0All files\0*.*\0", true));
        else if (id == ID_OPENDIR && !gLastDir.empty()) ShellExecuteW(h, L"open", gLastDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else if (id == ID_CLEAR && gPending == 0) {
            SendMessageW(gList, LVM_DELETEALLITEMS, 0, 0); gItems.clear(); ShowWindow(gList, SW_HIDE); EnableWindow(gClear, FALSE);
            setStatus(L"Cleared", MUTED);
        } else if ((id == ID_MAX || id == ID_QUAL) && HIWORD(w) == EN_KILLFOCUS) readSettings();
        return 0;
    }
    case WM_NOTIFY: {
        auto* nm = (NMHDR*)l;
        if (nm->idFrom == ID_LIST && nm->code == NM_DBLCLK) {
            auto* ia = (NMITEMACTIVATE*)l;
            if (ia->iItem >= 0) { LVITEMW li = {}; li.mask = LVIF_PARAM; li.iItem = ia->iItem; SendMessageW(gList, LVM_GETITEMW, 0, (LPARAM)&li);
                auto* it = (Item*)li.lParam; if (it && it->state == 2) ShellExecuteW(h, L"open", it->out.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
        }
        if (nm->idFrom == ID_LIST && nm->code == NM_CUSTOMDRAW) {
            auto* cd = (NMLVCUSTOMDRAW*)l;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
            if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
                auto* it = (Item*)cd->nmcd.lItemlParam;
                cd->clrText = TEXT; cd->clrTextBk = CARD;
                if (it) {
                    if (it->state == 3 && cd->iSubItem >= 3) cd->clrText = ERR;
                    else if (it->state == 2 && cd->iSubItem == 3) cd->clrText = it->after <= it->before ? OK : WARN;
                    else if (cd->iSubItem == 1 || cd->iSubItem == 4) cd->clrText = MUTED;
                }
                return CDRF_NEWFONT;
            }
        }
        return 0;
    }
    case WM_CTLCOLOREDIT: return ctlColor(w);
    case WM_DRAWITEM: drawButton((DRAWITEMSTRUCT*)l); return TRUE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paintBuffered(h, paint); return 0;
    case WM_DESTROY: readSettings(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    init(inst);
    HWND h = createMain(L"ImageShrinkerWnd", L"Image Shrinker", 900, 572, wndProc);
    ShowWindow(h, show); UpdateWindow(h);
    int argc; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1) { std::vector<std::wstring> f(argv + 1, argv + argc); addFiles(f); }
    return loop([&](MSG& m) {
        if (ctrlKey(m, 'O')) { SendMessageW(h, WM_COMMAND, ID_ADD, 0); return true; }
        return false;
    });
}
