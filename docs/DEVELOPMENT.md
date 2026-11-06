# DEVELOPMENT

This guide helps developers understand how to work with the `obs-neutral-balance` codebase.

## Quick Start

```bash
# Clone and build
git clone https://github.com/rawrdoge/obs-neutral-balance.git
cd obs-neutral-balance
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

## Code Organization

### Core Modules

#### `colorimetry.c/h` - Color Science Engine

Pure functions for color space conversions. No OBS dependencies.

```c
// Key functions:
void nb_rgb2xyz(const double rgb[3], double xyz[3]);          // Linear RGB → CIE XYZ
void nb_white_from_lch(...);                                  // LCh → XYZ
void nb_cat_matrix(int cat_method, ...);                      // Chromatic adaptation
double nb_delta_e_2000(const double lab1[3], const double lab2[3]); // Color difference
```

#### `matrix.c/h` - Linear Algebra

3×3 matrix operations for color transforms.

```c
void nb_mat3_mul(const double a[9], const double b[9], double out[9]);
bool nb_mat3_inv(const double m[9], double out[9]);
void nb_mat3_apply(const double m[9], const double v[3], double out[3]);
```

#### `stats.c/h` - Illuminant Estimation

Automatic white balance via Grey World and Shades of Grey algorithms.

```c
void nb_update_stats(struct neutral_balance *f, gs_texture_t *input);
```

#### `capture.c/h` - Frame Capture

Captures frames for the eyedropper dialog.

```c
void nb_capture_frame(struct neutral_balance *f);
```

#### `eyedropper.cpp/h` - Qt Dialog

Interactive color picker with loupe and magnification.

### Build System

#### CMakeLists.txt

Key configuration:
- C17 standard enforced
- Qt6/Qt5 optional (for eyedropper)
- Platform-specific paths (Windows/macOS/Linux)

#### CI/CD

GitHub Actions workflow builds for:
- Windows x64
- macOS arm64
- Ubuntu x86_64

## Development Workflow

### 1. Create Feature Branch

```bash
git checkout -b feature/your-feature-name
```

### 2. Make Changes

Follow the modular structure:
- Color science changes → `colorimetry.c`
- Matrix math → `matrix.c`
- Statistics → `stats.c`
- UI → `neutral-balance-filter.c`

### 3. Add Tests

Update `tests/test_colorimetry.c` for new functionality.

### 4. Run Tests

```bash
cmake --build . --target test
./tests/test_colorimetry
```

### 5. Test in OBS

1. Build the plugin
2. Copy to OBS plugin directory
3. Restart OBS
4. Add filter to source and test

### 6. Submit PR

- Write clear description
- Link related issues
- Include screenshots for UI changes

## Color Science Implementation

### Precision Guidelines

- **CPU math**: Always use `double` for accuracy
- **GPU shaders**: Use `float` for performance
- **Tolerance**: EPSILON = 1e-6 for comparisons

### Common Patterns

#### Converting Color Spaces

```c
// RGB → XYZ → Lab → LCh
double rgb[3] = {0.5, 0.5, 0.5};
double xyz[3];
double lab[3];
double lch[3];

nb_rgb2xyz(rgb, xyz);
nb_linrgb_to_lab(rgb, lab);  // Simplified
// LCh = polar form of Lab
```

#### Computing CAT Matrix

```c
double src_white[3] = {0.95047, 1.0, 1.08883};  // D65
double dst_white[3] = {0.96422, 1.0, 0.82521};  // D50
double cat_matrix[9];

nb_cat_matrix(NB_CAT_BRADFORD, src_white, dst_white, cat_matrix);
```

#### Delta E Calculation

```c
double lab1[3] = {50.0, -10.0, 20.0};
double lab2[3] = {50.5, -9.5, 20.5};

double de = nb_delta_e_2000(lab1, lab2);
// de < 1.0: imperceptible
// de 1-2:  perceptible up close
// de 2-10: perceptible at glance
// de > 10: large difference
```

## Debugging Tips

### Enable Logging

```c
blog(LOG_INFO, "[neutral-balance] Debug message: %f", value);
```

### Check Shader Compilation

```c
const char *error = gs_effect_get_error(f->effect);
if (error) {
    blog(LOG_ERROR, "Shader error: %s", error);
}
```

### Validate Color Values

```c
// Ensure values are in valid ranges
assert(rgb[0] >= 0.0 && rgb[0] <= 1.0);
assert(xyz[1] > 0.0);  // Y must be positive
```

## Testing

### Unit Tests

Located in `tests/test_colorimetry.c`. Tests cover:
- Basic color conversions
- Round-trip consistency
- Edge cases (black, white, pure colors)
- CAT matrix correctness

### Manual Testing Checklist

- [ ] Grey World mode works
- [ ] Shades of Grey mode works
- [ ] Eyedropper samples correctly
- [ ] All CAT methods produce valid results
- [ ] Split preview toggles correctly
- [ ] Reset button works
- [ ] Settings persist across reloads

## Performance Profiling

### CPU Profiling

```bash
# Use valgrind for memory profiling
valgrind --tool=callgrind ./build/obs-neutral-balance
kcachegrind callgrind.out.*
```

### GPU Profiling

Use OBS built-in GPU stats or NVIDIA/NVIDIA NSight.

## Common Pitfalls

1. **Forgetting `_GNU_SOURCE`**: Required for POSIX functions on Linux
2. **Float precision on GPU**: Don't expect double precision in shaders
3. **Thread safety**: Statistics and render threads must synchronize
4. **Memory leaks**: Always release `gs_texrender_t` and `gs_stagesurf_t`
5. **Qt thread affinity**: Dialog must be created on UI thread

## Resources

- [OBS Plugin Development](https://github.com/obsproject/obs-studio/wiki)
- [CIE Color Science](https://cie.co.at/)
- [Color Science for Programmers](https://blog.csdn.net/)
- [HLSL Reference](https://docs.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl)

---

Happy coding! 🎨
