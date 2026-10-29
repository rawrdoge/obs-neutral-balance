# obs-neutral-balance

Manual white balance correction filter for OBS Studio, modeled on darktable's
*color calibration* correction-measure workflow: sample a neutral area, get a
measurement-driven correction, then fine-tune it with **hue (degrees)** and
**luminance** sliders.

Implements PRD `OBS-NB-PRD-001` (see `docs/` in the project repo).

## Features

- **Pick Neutral eyedropper** — captures one frozen frame of the filter input
  and lets you click a neutral area with a crosshair, magnified loupe, and
  configurable sample radius. The sampled color is mapped to the target
  neutral; the result is shown live as per-channel gains.
- **Grey World** and **Shades of Grey** automatic estimation (Minkowski `p`
  adjustable, default 6 per Finlayson & Trezzi), computed from a 160x90
  downscaled read-back at 10 Hz with temporal smoothing.
- **Target hue (0-360 deg) and luminance (0-100 %) sliders** that bias the
  correction in real time, mirroring darktable's perceptual target controls
  (v1 uses a YIQ hue rotation + lightness offset; CIE LCh is planned for v1.1).
- OBS-native properties UI, full settings persistence, localization-ready
  strings, zero-copy render path when no analysis is pending.

## Requirements

- OBS Studio 28.0+ (Qt6 build recommended; Qt5 also supported for the
  eyedropper dialog)
- CMake 3.16+, a C11/C++17 compiler
- libobs development files (ships with OBS on macOS, `obs-studio` dev package
  or an OBS build tree on Linux, or the OBS installer + CMake config on
  Windows)

## Building

This project is laid out like the official
[obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).

**Windows (from an OBS build tree):**

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/obs-build"
cmake --build build --config Release
```

**Linux (Debian/Ubuntu with an OBS build or installed libobs):**

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/obs
cmake --build build
```

**macOS:**

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt@6
cmake --build build
```

`find_package(libobs)` must resolve to an OBS install that provides
`OBS::libobs`. Qt Widgets is auto-detected; without it the plugin still builds
and the automatic methods work, but the eyedropper button is inert.

## Installing

Copy the built module and the `data/obs-plugins/obs-neutral-balance` folder
(the `.effect` file and locale) into your OBS install next to other plugins:

- Windows: `C:\Program Files\obs-studio\obs-plugins\64bit\` +
  `C:\Program Files\obs-studio\data\obs-plugins\obs-neutral-balance\`
- macOS: `Obs.app/Contents/PlugIns` + `Obs.app/Contents/Resources/data/obs-plugins/...`
- Linux: `~/.config/obs-studio/plugins/` layout or system `lib/obs-plugins` +
  `share/obs/obs-plugins/obs-neutral-balance/`

Restart OBS, then: right-click a source -> **Filters** -> **+** ->
**Manual White Balance**.

## Usage

1. Add the filter to your source.
2. (Manual mode) Click **Pick Neutral...** and click a gray card or neutral
   surface in the frozen frame. The status line shows the sample and gains.
3. Adjust **Target neutral hue** to warm/cool the neutral and **Target
   luminance** to shift its lightness.
4. Or switch the method to Grey World / Shades of Grey for fully automatic
   correction; tweak `p` to taste.
5. **Reset to Neutral** restores identity gains and defaults.

## Architecture notes

- Filter registered via `obs_source_info` (`OBS_SOURCE_TYPE_FILTER`), all
  processing in `video_render` through a GPU effect — no CPU frame copies.
- The filter input is rendered into an owned texture via
  `obs_source_video_render(obs_filter_get_target(...))` only when analysis is
  actually needed (auto methods or an eyedropper request); otherwise the
  zero-copy `obs_source_process_filter_begin/end` path is used.
- The eyedropper read-back happens once, on demand, on the render thread; the
  UI thread waits on an `os_event` with a timeout so a hidden source can never
  deadlock the properties dialog.
- Only public libobs APIs are used (`obs.h`, `obs-source.h`,
  `graphics/graphics.h`, `util/*`); code is formatted with OBS's
  `.clang-format`.

## Known limitations / roadmap (v1.1)

- Hue/luminance bias uses YIQ/HSL-style math rather than full CIE LCh
  (PRD open question #1).
- Grey World / Shades of Grey statistics run in the gamma-encoded domain.
- No auto-estimation "lock" button yet (PRD open question #3).

## License

GPL-2.0-or-later, matching OBS Studio. See [LICENSE](LICENSE).
