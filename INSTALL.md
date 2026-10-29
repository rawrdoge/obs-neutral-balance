# Installing obs-neutral-balance (CI artifacts)

The GitHub Actions workflow (`.github/workflows/build.yaml`) produces three
installable zips. **No building required on your machine.**

1. Push this repository to GitHub (any private repo works - Actions minutes
   are free for private repos within monthly limits).
2. Open the **Actions** tab, pick **Build obs-neutral-balance**, and let the
   run finish (~15-20 minutes; it builds libobs from the pinned OBS source).
3. Download the artifact for your platform:
   - `obs-neutral-balance-windows-x64` (Windows 10/11, 64-bit)
   - `obs-neutral-balance-macos-arm64` (Apple Silicon)
   - `obs-neutral-balance-ubuntu-x86_64` (Ubuntu 22.04+/similar distros)
4. Extract the zip. It contains the standard OBS plugin layout:

   ```
   obs-neutral-balance/
   +-- obs-plugins/64bit/obs-neutral-balance.dll   (or .so / .dylib)
   `-- data/obs-plugins/obs-neutral-balance/       (effect + locale)
   ```

## Install locations

**Windows (no admin rights needed):**
Extract so that the files land in
`%APPDATA%\obs-studio\plugins\obs-neutral-balance\`
i.e. `%APPDATA%\obs-studio\plugins\obs-neutral-balance\obs-plugins\64bit\obs-neutral-balance.dll`.
OBS 28+ loads user plugins from this folder.

Alternative (system-wide, needs admin): copy `obs-plugins\64bit\*.dll` to
`C:\Program Files\obs-studio\obs-plugins\64bit\` and merge the
`data\obs-plugins\obs-neutral-balance` folder into
`C:\Program Files\obs-studio\data\obs-plugins\`.

**macOS (no admin rights needed):**
Extract into `~/Library/Application Support/obs-studio/plugins/` so you get
`~/Library/Application Support/obs-studio/plugins/obs-neutral-balance/obs-plugins/obs-neutral-balance.dylib`.
Because the binary is ad-hoc signed, on first load you may need to right-click
OBS -> Open (or remove quarantine: `xattr -dr com.apple.quarantine` on the
extracted folder) if your browser set the quarantine flag on the zip.

Alternative (system-wide): copy into `Obs.app/Contents/PlugIns/` and the data
folder into `Obs.app/Contents/Resources/data/obs-plugins/`.

**Linux (no root needed):**
Extract into `~/.config/obs-studio/plugins/` so you get
`~/.config/obs-studio/plugins/obs-neutral-balance/obs-plugins/obs-neutral-balance.so`.
The plugin links against Qt6 Widgets; if your distro splits Qt packages, make
sure `libqt6widgets6` is installed (it ships with OBS anyway).

## First run

Restart OBS, right-click a video source -> **Filters** -> **+** ->
**Manual White Balance**. If the filter does not appear, check
**Help -> Log Files** for a `[neutral-balance]` line.
