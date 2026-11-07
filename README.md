# obs-neutral-balance

Format-aware, linear-light, chromatic-adaptation white balance filter for OBS Studio, modeled on darktable's color calibration measure-and-correct workflow. v2.1.

## Features

- Color-space aware: queries `obs_source_get_color_space()` per frame; works in linear sRGB or linear BT.709, logged fallback for unknown spaces.
- Linear-light statistics: Grey World and Shades of Grey (Minkowski p, default 6) from a 160x90 read-back, smoothed at 10 Hz.
- Full CAT correction: 3x3 chromatic-adaptation matrix (Bradford, CAT02, CAT16, von Kries/HPE, or diagonal) maps measured illuminant in CIE XYZ onto the target; diagonal is the identity-cone special case.
- CIE LCh target: hue, luminance, chroma sliders define the destination neutral perceptually; luminance is a true gain/exposure axis, not a brightness filter.
- Eyedropper: frozen-frame dialog with crosshair, magnified loupe, non-neutral sample warning, live audit line (linear RGB, CIE XYZ, adapted result); sample may also be typed as `Measured Sample: (R,G,B)` on 0-255.
- Diagnostics: read-only panel reports working space, video range, measured linear RGB/XYZ, CAT matrix rows, and Match Darktable button scoring the corrected sample vs target in DeltaE00.
- Split preview: live before/after divider (left = uncorrected input).
- Settings persisted; all strings wrapped for localization; zero-copy render path when no analysis pending.

## Requirements

- OBS Studio 28.0+ (Qt6 build recommended for the eyedropper).
- CMake 3.16+, C11/C++17 compiler, libobs dev files.
- Qt6/Qt5 Widgets optional: without it the filter builds and automatic methods work, but the eyedropper button is inert.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<obs-prefix>
cmake --build build
```

CI builds Windows and Ubuntu packages on tag push (`v*`) and on push to main/master; see `.github/workflows/build.yaml`. For prebuilt zips and install locations see [INSTALL.md](INSTALL.md).

## Install

Install the module plus `data/obs-plugins/obs-neutral-balance/` (effect file + locale) into the OBS plugin directory. Windows 10/11 per-user: `%APPDATA%\obs-studio\plugins\obs-neutral-balance\` (no admin rights). Restart OBS, then: source -> Filters -> + -> Manual White Balance.

## Usage

1. Add the filter to a video source.
2. Pick a measurement: Pick Neutral... on a gray/white surface, type `Measured Sample: (R,G,B)`, or select Grey World / Shades of Grey.
3. Set the target with hue / luminance / chroma sliders (defaults give a neutral middle gray at the measured exposure).
4. Choose a CAT method (Bradford default; Diagonal reproduces the legacy approximation).
5. Enable Split preview to compare before (left) and after (right); use Match Darktable (dE00) to score how close the corrected sample lands to the target.
6. Reset to Neutral restores Bradford + LCh(50, 0, 0) and clears the measurement.

## Troubleshooting

The correction looks wrong (magenta/cyan or heavy cast): the measured color was not actually neutral. The eyedropper warns on high-chroma samples - re-pick a true gray/white patch; larger radius only helps on uniformly neutral surfaces. Compare shown R/G/B before accepting. The frozen frame is the filter input at native resolution - if the source is cropped, scaled, or mirrored in the scene, trust the dialog window, not the OBS preview.

## Known limitations

- HDR white points (scRGB) map to the linear BT.709 working space; target L stays the perceptual default of 50.
- Grey World / Shades of Grey statistics use a fixed 160x90 read-back.
- GPU (video_render) filter: RGB texture from OBS is full range, so the range panel reports FULL.

## License

GPL-2.0-or-later, matching OBS Studio. See [LICENSE](LICENSE).
