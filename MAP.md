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
- [Architecture & Contributor Documentation Structure]: Three top-level docs created: `CONTRIBUTING.md` (guidelines, coding standards, PR process), `ARCHITECTURE.md` (pipeline diagram, thread model, extensibility), `DEVELOPMENT.md` (quick start, code organization, debugging tips).
- [Performance Benchmarking & Statistics Readback Optimization]: Recommend double-buffered staging surfaces + adaptive throttling for 70-80% overhead reduction. GPU-compute statistics deferred to OBS 30+.
- [Eyedropper Qt Dialog Enhancements (high-DPI, multi-monitor)]: Query QScreen::devicePixelRatio() per-screen; scale crosshair thickness and loupe zoom by DPR. Center dialog on screen under cursor via screenAt(QCursor::pos()) with primary-screen fallback. Stub path (!NB_HAVE_QT) untouched.

## Not yet specified

- **UI Progressive Disclosure Design:** Simple/Advanced mode toggle layout and control grouping.
- **Split Preview Placement:** Moving from bottom-of-list to always-visible position.
- **Match Darktable Rename:** User-friendly terminology for DeltaE validation.
- **CI & Release Automated Testing Integration:** How to integrate C unit tests into `.github/workflows/build.yaml` cross-platform builds without breaking OBS dependency linking.

## Out of scope

*(None yet)*
