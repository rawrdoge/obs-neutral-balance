# obs-neutral-balance

Format-aware, linear-light, chromatic-adaptation white balance for OBS Studio,
modeled on darktable's color calibration measure-and-correct workflow.

## Features

- **Color-space aware (FR-01..04)** - queries `obs_source_get_color_space()` per
  frame; works in linear sRGB or linear BT.709 depending on the source, with a
  logged fallback for unknown spaces.
- **Linear-light statistics (FR-09..13)** - Grey World and Shades of Grey
  (Minkowski p, default 6) estimates are gathered in linear light from a
  160x90 read-back, temporally smoothed at 10 Hz.
- **Full CAT correction (FR-14..18)** - a 3x3 chromatic-adaptation matrix
  (Bradford, CAT02, CAT16, von Kries/HPE, or diagonal) maps the measured
  illuminant in CIE XYZ onto the target; the diagonal mode is the
  identity-cone special case of the same pipeline.
- **CIE LCh target (FR-19..24)** - hue, luminance, and chroma sliders define
  the destination neutral perceptually; luminance is a true gain/exposure axis
  (the measured color maps exactly onto it), not a brightness filter.
- **Eyedropper** - frozen-frame dialog with crosshair, magnified loupe,
  non-neutral sample warning, and a live audit line (linear RGB, CIE XYZ,
  adapted result). The sample may also be typed numerically as
  **Measured Sample: (R,G,B)** on the 0-255 scale.
- **Diagnostics (FR-27/28)** - a read-only panel reports the working space,
  video range, measured linear RGB/XYZ, the CAT matrix rows, and a Match
  Darktable button that scores the corrected sample against the target in
  DeltaE00.
- **Split preview** - live before/after divider (left = uncorrected input).
- Settings fully persisted; all strings wrapped for localization; zero-copy
  render path when no analysis is pending.

## Requirements

- OBS Studio 28.0+ (Qt6 build recommended for the eyedropper)
- CMake 3.16+, C11/C++17 compiler, libobs dev files
- Qt6/Qt5 Widgets (optional; without it the filter builds and the automatic
  methods work, but the eyedropper button is inert)

## Building and installing

CI builds Windows and Ubuntu packages automatically on tag push (`v*`); see
`.github/workflows/build.yaml`. For a local build against an OBS install:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<obs-prefix>
cmake --build build
```

Install the module plus `data/obs-plugins/obs-neutral-balance/` (effect file +
locale) into your OBS plugin directory; on Windows 10/11 the per-user
location `%APPDATA%\obs-studio\plugins\obs-neutral-balance\` works without
admin rights. Restart OBS, then: source -> **Filters** -> **+** -> **Manual
White Balance**.

## Usage

1. Add the filter to a video source.
2. Pick a measurement: **Pick Neutral...** on a gray/white surface, type a
   **Measured Sample: (R,G,B)**, or select Grey World / Shades of Grey.
3. Set the **target** with the hue / luminance / chroma sliders (defaults give
   a neutral middle gray at the measured exposure).
4. Choose a CAT method (Bradford is the default; Diagonal reproduces the
   legacy approximation for comparison).
5. Enable **Split preview** to compare before (left) and after (right); use
   **Match Darktable (dE00)** to score how close the corrected sample lands to
   the target.
6. **Reset to Neutral** restores Bradford + LCh(50, 0, 0) and clears the
   measurement.

## Troubleshooting

**The correction looks wrong (magenta/cyan or a heavy cast).** The measured
color was not actually neutral. The eyedropper warns when the sampled area has
high chroma - re-pick a true gray/white patch; a larger radius only helps on a
uniformly neutral surface. Compare the shown R/G/B against what you expect
before accepting. The frozen frame is the filter input at native resolution -
if the source is cropped, scaled, or mirrored in the scene, trust what you see
in the dialog window, not the OBS preview.

## Known limitations

- HDR white points (scRGB) map to the linear BT.709 working space; the target
  L stays the perceptual default of 50.
- Grey World / Shades of Grey statistics use a fixed 160x90 read-back.
- The plugin is a GPU (video_render) filter: the RGB texture from OBS is
  full range, so the range panel reports FULL.

## License

GPL-2.0-or-later, matching OBS Studio. See [LICENSE](LICENSE).
