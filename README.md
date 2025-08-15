*** Add File: README.md
# DPIOverlay – C++ Win32 DPI-aware overlay (prototype)

Minimal Win32 C++ overlay that stays sharp across DPI/scales and multi-monitor setups. Uses PerMonitorV2 DPI, layered windows, and GDI+ rendering. Includes NSIS installer script.

## Features
- PerMonitorV2 DPI awareness (manifest + runtime)
- One overlay window per monitor, always-on-top, transparent
- Crisp text/shapes at 100/125/150/175% scaling
- Optional click-through mode
- Global hotkeys: Ctrl+Alt+T (toggle click-through), Ctrl+Alt+Q (quit)
- Basic NSIS installer script

## Build (Windows)
1. Install dependencies:
   - Visual Studio 2019/2022 with Desktop development with C++
   - CMake 3.20+
   - NSIS (for installer)
2. Configure/build:
```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```
3. Run the app:
```bat
build\Release\DPIOverlay.exe
```

## Installer (Windows)
After building the Release executable:
```bat
"C:\\Program Files (x86)\\NSIS\\makensis.exe" installer\\installer.nsi
```
Outputs `DPIOverlay-Setup-0.1.0.exe` in the repo root.

## Usage
- The overlay shows on all monitors.
- Toggle click-through: Ctrl+Alt+T
- Quit: Ctrl+Alt+Q
- Move between monitors/change scaling: overlay reflows without blur.

## QA checklist (prototype)
- Single monitor at 100/125/150/175%: crisp border and badge text
- Dual monitors mixed DPI (e.g., 100% + 150%): no blur on either
- Change scaling while running: windows resize to suggested rect, remain crisp
- Plug/unplug monitor: overlay appears/disappears correctly
- Click-through toggle works; mouse passes through when enabled
- Installer installs/uninstalls shortcuts and app

## Notes
- Rendering uses UpdateLayeredWindow with a 32-bit ARGB DIB for per-pixel alpha.
- For heavier animations or GPU scaling, consider Direct2D/Direct3D11.
- Code signing: sign the built EXE and installer in CI.


