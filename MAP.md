# Wayfinder Map: obs-neutral-balance v2.1 Improvements

## Destination

Transform `obs-neutral-balance` into a polished, contributor-ready, high-performance OBS Studio plugin with a dual-tier (Simple/Advanced) progressive-disclosure UI, standalone C unit testing for pure colorimetry math, GPU/CPU performance optimization, and clear developer/contributor documentation.

## Notes

- **Repository:** `/workspace/obs-neutral-balance` (git: `https://github.com/rawrdoge/obs-neutral-balance.git`)
- **Architecture state:** Monolith already split in v2.0.1 (`colorimetry.c`, `stats.c`, `capture.c`, `matrix.c`).
- **Domain focus:** Color science accuracy (sRGB/BT.709, CAT02/Bradford, CIE XYZ/LCh), OBS C API (`libobs`), Qt6/Qt5 eyedropper GUI.
- **Workflow rule:** Planning first. Tickets resolve decisions and specs before code implementation starts.

## Decisions so far

*(None yet — map initialized)*

## Not yet specified

- **GPU vs CPU Statistics Path Optimization:** Benchmarking and evaluating whether the 160x90 CPU readback should be migrated to GPU compute shaders or unified texture readbacks to eliminate CPU overhead.
- **CI & Release Automated Testing Integration:** How to integrate C unit tests into `.github/workflows/build.yaml` cross-platform builds without breaking OBS dependency linking.
- **Eyedropper Qt Dialog Enhancements:** Whether to simplify or expand the frozen-frame eyedropper dialog UI for high-DPI displays and multi-monitor setups.

## Out of scope

*(None yet)*
