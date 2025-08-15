#include <windows.h>
#include <shellscalingapi.h>
#include <gdiplus.h>
#include <vector>
#include <string>
#include <memory>

#pragma comment(lib, "Gdiplus.lib")
#pragma comment(lib, "Shcore.lib")

using std::wstring;
using std::vector;

struct GdiplusInitGuard {
    ULONG_PTR gdiplusToken = 0;
    GdiplusInitGuard() {
        Gdiplus::GdiplusStartupInput gdiplusStartupInput;
        Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);
    }
    ~GdiplusInitGuard() {
        if (gdiplusToken) {
            Gdiplus::GdiplusShutdown(gdiplusToken);
        }
    }
};

struct OverlayWindow {
    HWND hwnd = nullptr;
    HMONITOR monitor = nullptr;
    RECT monitorRect = {0, 0, 0, 0};
    UINT dpiX = 96;
    UINT dpiY = 96;
    bool clickThrough = false;
};

static const wchar_t* kClassName = L"DPIOverlayWindowClass";
static vector<std::unique_ptr<OverlayWindow>> g_windows;
static HINSTANCE g_hInstance = nullptr;
static const int HOTKEY_TOGGLE = 1;
static const int HOTKEY_QUIT = 2;

// Forward declarations
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void CreateWindowsForAllMonitors();
void DestroyAllWindows();
void RenderOverlay(OverlayWindow* window);
void UpdateClickThrough(OverlayWindow* window, bool enable);

RECT GetMonitorRect(HMONITOR hMon) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);
    return mi.rcMonitor; // full monitor bounds
}

BOOL CALLBACK EnumMonitorsProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
    vector<HMONITOR>* monitors = reinterpret_cast<vector<HMONITOR>*>(lParam);
    monitors->push_back(hMon);
    return TRUE;
}

void ToggleAllClickThrough() {
    for (auto& w : g_windows) {
        w->clickThrough = !w->clickThrough;
        UpdateClickThrough(w.get(), w->clickThrough);
    }
}

void UpdateClickThrough(OverlayWindow* window, bool enable) {
    LONG_PTR exStyle = GetWindowLongPtrW(window->hwnd, GWL_EXSTYLE);
    if (enable) {
        exStyle |= WS_EX_TRANSPARENT; // mouse pass-through
    } else {
        exStyle &= ~WS_EX_TRANSPARENT;
    }
    SetWindowLongPtrW(window->hwnd, GWL_EXSTYLE, exStyle);
    SetWindowPos(window->hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

void CreateWindowsForAllMonitors() {
    vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorsProc, reinterpret_cast<LPARAM>(&monitors));

    for (HMONITOR mon : monitors) {
        auto ow = std::make_unique<OverlayWindow>();
        ow->monitor = mon;
        ow->monitorRect = GetMonitorRect(mon);

        int x = ow->monitorRect.left;
        int y = ow->monitorRect.top;
        int width = ow->monitorRect.right - ow->monitorRect.left;
        int height = ow->monitorRect.bottom - ow->monitorRect.top;

        HWND hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            kClassName,
            L"DPI Overlay",
            WS_POPUP,
            x, y, width, height,
            nullptr, nullptr, g_hInstance, nullptr);

        if (!hwnd) continue;

        ow->hwnd = hwnd;
        UINT dpi = GetDpiForWindow(hwnd);
        ow->dpiX = dpi;
        ow->dpiY = dpi;

        // Show on top
        SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW);

        // Initial render
        RenderOverlay(ow.get());

        g_windows.push_back(std::move(ow));
    }
}

void DestroyAllWindows() {
    for (auto& w : g_windows) {
        if (w->hwnd) DestroyWindow(w->hwnd);
    }
    g_windows.clear();
}

// Helper to create a 32-bit ARGB DIB section and a mem DC for UpdateLayeredWindow
struct DibSurface {
    HBITMAP hbm = nullptr;
    HDC hdc = nullptr;
    void* bits = nullptr;
    int width = 0;
    int height = 0;
    HDC screenDC = nullptr;

    ~DibSurface() { reset(); }
    void reset() {
        if (hdc) { DeleteDC(hdc); hdc = nullptr; }
        if (hbm) { DeleteObject(hbm); hbm = nullptr; }
        if (screenDC) { ReleaseDC(nullptr, screenDC); screenDC = nullptr; }
        bits = nullptr;
        width = height = 0;
    }
    bool create(int w, int h) {
        reset();
        screenDC = GetDC(nullptr);
        if (!screenDC) return false;
        hdc = CreateCompatibleDC(screenDC);
        if (!hdc) return false;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; // top-down DIB
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        hbm = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!hbm || !bits) return false;
        SelectObject(hdc, hbm);
        width = w; height = h;
        return true;
    }
};

void DrawDemo(Gdiplus::Graphics& g, int pixelWidth, int pixelHeight, float dpiX, float dpiY) {
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    // Clear fully transparent
    Gdiplus::SolidBrush clearBrush(Gdiplus::Color(0, 0, 0, 0));
    Gdiplus::Rect full(0, 0, pixelWidth, pixelHeight);
    g.FillRectangle(&clearBrush, full);

    // Edge outline (crisp, 2px aligned)
    Gdiplus::Pen outline(Gdiplus::Color(200, 0, 122, 255), 2.0f);
    outline.SetAlignment(Gdiplus::PenAlignmentInset);
    g.DrawRectangle(&outline, 1, 1, pixelWidth - 2, pixelHeight - 2);

    // A DPI-scaled badge in the corner
    const float scaleX = dpiX / 96.0f;
    const float scaleY = dpiY / 96.0f;
    const int badgeW = static_cast<int>(160 * scaleX);
    const int badgeH = static_cast<int>(48 * scaleY);

    Gdiplus::SolidBrush badgeBg(Gdiplus::Color(180, 30, 30, 30));
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(240, 255, 255, 255));
    g.FillRectangle(&badgeBg, 20, 20, badgeW, badgeH);

    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font font(&ff, 14.0f * scaleY, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    wstring label = L"Overlay DPI: " + std::to_wstring(static_cast<int>(dpiX * 100 / 96)) + L"%";
    Gdiplus::RectF layout(static_cast<Gdiplus::REAL>(24), static_cast<Gdiplus::REAL>(24), static_cast<Gdiplus::REAL>(badgeW - 8), static_cast<Gdiplus::REAL>(badgeH - 8));
    g.DrawString(label.c_str(), -1, &font, layout, nullptr, &textBrush);
}

void RenderOverlay(OverlayWindow* window) {
    if (!window || !window->hwnd) return;

    RECT rc{};
    GetClientRect(window->hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    DibSurface surf;
    if (!surf.create(w, h)) return;

    Gdiplus::Graphics g(surf.hdc);
    g.SetPageUnit(Gdiplus::UnitPixel);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    const float dpiX = static_cast<float>(GetDpiForWindow(window->hwnd));
    const float dpiY = dpiX;
    g.SetDpi(dpiX, dpiY);

    DrawDemo(g, w, h, dpiX, dpiY);

    POINT ptSrc{0, 0};
    SIZE size{w, h};
    POINT ptDst{window->monitorRect.left, window->monitorRect.top};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255; // use per-pixel alpha
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(window->hwnd, surf.screenDC, &ptDst, &size, surf.hdc, &ptSrc, 0, &blend, ULW_ALPHA);
}

void RecreateForDpiChange(HWND hwnd, const RECT* suggested) {
    for (auto& w : g_windows) {
        if (w->hwnd == hwnd) {
            w->monitorRect = *suggested;
            SetWindowPos(hwnd, nullptr,
                         suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            UINT dpi = GetDpiForWindow(hwnd);
            w->dpiX = dpi;
            w->dpiY = dpi;
            RenderOverlay(w.get());
            break;
        }
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            return 0;
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<RECT*>(lParam);
            RecreateForDpiChange(hwnd, suggested);
            return 0;
        }
        case WM_DISPLAYCHANGE: {
            DestroyAllWindows();
            CreateWindowsForAllMonitors();
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_F8) {
                ToggleAllClickThrough();
                return 0;
            }
            return 0;
        case WM_DESTROY:
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    g_hInstance = hInstance;

    // Per-Monitor-V2 DPI awareness
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    GdiplusInitGuard gdip;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    CreateWindowsForAllMonitors();

    // Message loop
    MSG msg;
    // Register global hotkeys: Ctrl+Alt+T to toggle click-through, Ctrl+Alt+Q to quit
    RegisterHotKey(nullptr, HOTKEY_TOGGLE, MOD_CONTROL | MOD_ALT, 'T');
    RegisterHotKey(nullptr, HOTKEY_QUIT, MOD_CONTROL | MOD_ALT, 'Q');

    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_HOTKEY) {
            if (msg.wParam == HOTKEY_TOGGLE) {
                ToggleAllClickThrough();
                continue;
            } else if (msg.wParam == HOTKEY_QUIT) {
                PostQuitMessage(0);
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyAllWindows();
    return 0;
}


