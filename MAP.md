# Wayfinder Map: obs-neutral-balance v2.1 Improvements

## Destination

Transform `obs-neutral-balance` into a polished, contributor-ready, high-performance OBS Studio plugin with a dual-tier (Simple/Advanced) progressive-disclosure UI, standalone C unit testing for pure colorimetry math, GPU/CPU performance optimization, and clear developer/contributor documentation.

## Notes

- **Repository:** `/workspace/obs-neutral-balance` (git: `https://github.com/rawrdoge/obs-neutral-balance.git`)
- **Architecture state:** Monolith already split in v2.0.1 (`colorimetry.c`, `stats.c`, `capture.c`, `matrix.c`).
- **Domain focus:** Color science accuracy (sRGB/BT.709, CAT02/Bradford, CIE XYZ/LCh), OBS C API (`libobs`), Qt6/Qt5 eyedropper GUI.
- **Workflow rule:** Planning first. Tickets resolve decisions and specs before code implementation starts.

## Decisions so far

- [Standalone Unit Testing Architecture for Colorimetry & Math]: Using `utest.h` header-only C test framework; 21 tests covering matrix ops, RGB↔XYZ, Lab/LCh, DeltaE2000, CAT matrices, transfer functions. All grounded in CIE standards and Sharma et al. (2005).
- [Architecture & Contributor Documentation Structure]: Three top-level docs created: `CONTRIBUTING.md` (guidelines, coding standards, PR process), `ARCHITECTURE.md` (pipeline diagram, thread model, extensibility), `DEVELOPMENT.md` (quick start, code organization, debugging tips, CI testing guide).
- [Performance Benchmarking & Statistics Readback Optimization]: Triple-buffered ring buffer for staged async readback eliminating render-thread stalls; adaptive throttling (10Hz→5Hz when static); compile-time `NB_STATS_W/H` reduction (160×90→80×45) for optional lower overhead. CMake refactored to separate test-only builds; macOS CI added.
- [UI Redesign & Progressive Disclosure]: Implemented Simple/Advanced mode toggle (default: Simple). Simple mode shows Method, Eyedropper, Split Preview, Measure DeltaE, Reset. Advanced mode adds CAT Method, Shades P, Target LCh sliders, full Diagnostics. Renamed 'Match Darktable' button to 'Measure DeltaE'. Moved Split Preview to top. Added dynamic visibility callback.
- [Eyedropper Qt Dialog Enhancements]: Query `QScreen::devicePixelRatio()` per-screen for high-DPI; multi-monitor support via `QGuiApplication::screens()`; loupe magnifier uses device pixel ratio for crisp rendering.
- [CI & Release Automated Testing Integration]: GitHub Actions matrix (Windows x64, macOS arm64, Ubuntu x86_64) with `BUILD_TESTING=ON`; `ctest` runs on every push/PR; colorimetry tests compile without libobs; release job gated on all three platform builds passing.

## Not yet specified

*(None — all in-scope work is resolved)*

## Out of scope

*(None)*
