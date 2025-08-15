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
static const int HOTKEY_SETTINGS = 3;

enum class BadgeCorner {
    TopLeft = 0,
    TopRight = 1,
    BottomLeft = 2,
    BottomRight = 3
};

struct GlobalSettings {
    BadgeCorner badgeCorner = BadgeCorner::TopLeft;
    bool clickThrough = false;
    int opacityPercent = 100; // 40..100
    COLORREF accentColor = RGB(0, 122, 255);
};

static GlobalSettings g_settings{};
static HWND g_settingsWnd = nullptr;

// Forward declarations
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void CreateWindowsForAllMonitors();
void DestroyAllWindows();
void RenderOverlay(OverlayWindow* window);
void UpdateClickThrough(OverlayWindow* window, bool enable);
void RenderAll();
void ApplyClickThroughAll(bool enable);
void ShowSettingsWindow();
LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

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
    g_settings.clickThrough = g_windows.empty() ? g_settings.clickThrough : g_windows.front()->clickThrough;
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

void RenderAll() {
    for (auto& w : g_windows) {
        RenderOverlay(w.get());
    }
}

void ApplyClickThroughAll(bool enable) {
    for (auto& w : g_windows) {
        w->clickThrough = enable;
        UpdateClickThrough(w.get(), enable);
    }
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

    // Edge outline (crisp, 2px aligned) with opacity/accent
    BYTE a = static_cast<BYTE>(g_settings.opacityPercent * 255 / 100);
    Gdiplus::Color accent(a, GetRValue(g_settings.accentColor), GetGValue(g_settings.accentColor), GetBValue(g_settings.accentColor));
    Gdiplus::Pen outline(accent, 2.0f);
    outline.SetAlignment(Gdiplus::PenAlignmentInset);
    g.DrawRectangle(&outline, 1, 1, pixelWidth - 2, pixelHeight - 2);

    // A DPI-scaled badge in the corner
    const float scaleX = dpiX / 96.0f;
    const float scaleY = dpiY / 96.0f;
    const int badgeW = static_cast<int>(160 * scaleX);
    const int badgeH = static_cast<int>(48 * scaleY);

    Gdiplus::SolidBrush badgeBg(Gdiplus::Color(static_cast<BYTE>(a * 0.8), 30, 30, 30));
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(a, 255, 255, 255));
    int marginX = 20;
    int marginY = 20;
    int x = marginX;
    int y = marginY;
    switch (g_settings.badgeCorner) {
        case BadgeCorner::TopLeft:
            x = marginX; y = marginY; break;
        case BadgeCorner::TopRight:
            x = pixelWidth - badgeW - marginX; y = marginY; break;
        case BadgeCorner::BottomLeft:
            x = marginX; y = pixelHeight - badgeH - marginY; break;
        case BadgeCorner::BottomRight:
            x = pixelWidth - badgeW - marginX; y = pixelHeight - badgeH - marginY; break;
    }
    g.FillRectangle(&badgeBg, x, y, badgeW, badgeH);

    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font font(&ff, 14.0f * scaleY, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    wstring label = L"Overlay DPI: " + std::to_wstring(static_cast<int>(dpiX * 100 / 96)) + L"%";
    Gdiplus::RectF layout(static_cast<Gdiplus::REAL>(x + 4), static_cast<Gdiplus::REAL>(y + 4), static_cast<Gdiplus::REAL>(badgeW - 8), static_cast<Gdiplus::REAL>(badgeH - 8));
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

// IDs for settings controls
static const int IDC_RAD_TL = 1001;
static const int IDC_RAD_TR = 1002;
static const int IDC_RAD_BL = 1003;
static const int IDC_RAD_BR = 1004;
static const int IDC_CHK_CLICKTHRU = 1005;
static const int IDC_SLD_OPACITY = 1006;
static const int IDC_CMB_COLOR = 1007;

void ShowSettingsWindow() {
    if (g_settingsWnd) {
        ShowWindow(g_settingsWnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_settingsWnd);
        return;
    }
    const int width = 320;
    const int height = 230;
    g_settingsWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Overlay Settings",
                                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                    CW_USEDEFAULT, CW_USEDEFAULT, width, height,
                                    nullptr, nullptr, g_hInstance, nullptr);
    SetWindowLongPtrW(g_settingsWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SettingsWndProc));

    CreateWindowExW(0, L"BUTTON", L"Top-Left", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                    10, 10, 100, 24, g_settingsWnd, reinterpret_cast<HMENU>(IDC_RAD_TL), g_hInstance, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Top-Right", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                    130, 10, 100, 24, g_settingsWnd, reinterpret_cast<HMENU>(IDC_RAD_TR), g_hInstance, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Bottom-Left", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                    10, 40, 100, 24, g_settingsWnd, reinterpret_cast<HMENU>(IDC_RAD_BL), g_hInstance, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Bottom-Right", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                    130, 40, 110, 24, g_settingsWnd, reinterpret_cast<HMENU>(IDC_RAD_BR), g_hInstance, nullptr);

    CreateWindowExW(0, L"BUTTON", L"Click-through", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                    10, 80, 120, 24, g_settingsWnd, reinterpret_cast<HMENU>(IDC_CHK_CLICKTHRU), g_hInstance, nullptr);

    // Opacity slider
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);
    CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                    10, 110, 200, 32, g_settingsWnd, reinterpret_cast<HMENU>(IDC_SLD_OPACITY), g_hInstance, nullptr);
    SendMessageW(GetDlgItem(g_settingsWnd, IDC_SLD_OPACITY), TBM_SETRANGE, TRUE, MAKELONG(40, 100));
    SendMessageW(GetDlgItem(g_settingsWnd, IDC_SLD_OPACITY), TBM_SETPOS, TRUE, g_settings.opacityPercent);

    // Color combo
    CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                    10, 150, 160, 100, g_settingsWnd, reinterpret_cast<HMENU>(IDC_CMB_COLOR), g_hInstance, nullptr);
    HWND hCmb = GetDlgItem(g_settingsWnd, IDC_CMB_COLOR);
    SendMessageW(hCmb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Blue"));
    SendMessageW(hCmb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Green"));
    SendMessageW(hCmb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Red"));
    SendMessageW(hCmb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"White"));
    int sel = 0; // default Blue
    if (g_settings.accentColor == RGB(0, 200, 0)) sel = 1;
    else if (g_settings.accentColor == RGB(220, 0, 0)) sel = 2;
    else if (g_settings.accentColor == RGB(255, 255, 255)) sel = 3;
    SendMessageW(hCmb, CB_SETCURSEL, sel, 0);

    // Initialize states
    CheckRadioButton(g_settingsWnd, IDC_RAD_TL, IDC_RAD_BR, IDC_RAD_TL + static_cast<int>(g_settings.badgeCorner));
    SendMessageW(GetDlgItem(g_settingsWnd, IDC_CHK_CLICKTHRU), BM_SETCHECK, g_settings.clickThrough ? BST_CHECKED : BST_UNCHECKED, 0);

    ShowWindow(g_settingsWnd, SW_SHOWNORMAL);
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_RAD_TL || id == IDC_RAD_TR || id == IDC_RAD_BL || id == IDC_RAD_BR) {
                int selected = id - IDC_RAD_TL;
                g_settings.badgeCorner = static_cast<BadgeCorner>(selected);
                RenderAll();
                return 0;
            }
            if (id == IDC_CHK_CLICKTHRU) {
                BOOL checked = (SendMessageW(reinterpret_cast<HWND>(lParam), BM_GETCHECK, 0, 0) == BST_CHECKED);
                g_settings.clickThrough = checked;
                ApplyClickThroughAll(checked);
                return 0;
            }
            break;
        }
        case WM_HSCROLL: {
            if (reinterpret_cast<HWND>(lParam) == GetDlgItem(hwnd, IDC_SLD_OPACITY)) {
                int pos = static_cast<int>(SendMessageW(reinterpret_cast<HWND>(lParam), TBM_GETPOS, 0, 0));
                g_settings.opacityPercent = max(40, min(100, pos));
                RenderAll();
                return 0;
            }
            break;
        }
        case WM_COMMAND: {
            if (HIWORD(wParam) == CBN_SELCHANGE && LOWORD(wParam) == IDC_CMB_COLOR) {
                HWND cmb = reinterpret_cast<HWND>(lParam);
                int sel = static_cast<int>(SendMessageW(cmb, CB_GETCURSEL, 0, 0));
                switch (sel) {
                    case 0: g_settings.accentColor = RGB(0, 122, 255); break;
                    case 1: g_settings.accentColor = RGB(0, 200, 0); break;
                    case 2: g_settings.accentColor = RGB(220, 0, 0); break;
                    case 3: g_settings.accentColor = RGB(255, 255, 255); break;
                }
                RenderAll();
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (hwnd == g_settingsWnd) g_settingsWnd = nullptr;
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
    // Register global hotkeys: Ctrl+Alt+T toggle click-through, Ctrl+Alt+Q quit, Ctrl+Alt+S settings
    RegisterHotKey(nullptr, HOTKEY_TOGGLE, MOD_CONTROL | MOD_ALT, 'T');
    RegisterHotKey(nullptr, HOTKEY_QUIT, MOD_CONTROL | MOD_ALT, 'Q');
    RegisterHotKey(nullptr, HOTKEY_SETTINGS, MOD_CONTROL | MOD_ALT, 'S');

    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_HOTKEY) {
            if (msg.wParam == HOTKEY_TOGGLE) {
                ToggleAllClickThrough();
                continue;
            } else if (msg.wParam == HOTKEY_QUIT) {
                PostQuitMessage(0);
                continue;
            } else if (msg.wParam == HOTKEY_SETTINGS) {
                ShowSettingsWindow();
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyAllWindows();
    return 0;
}


