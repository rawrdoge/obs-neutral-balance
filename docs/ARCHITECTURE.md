# Architecture

This document provides a detailed technical overview of the `obs-neutral-balance` plugin architecture.

## Overview

`obs-neutral-balance` is an OBS Studio filter plugin that performs manual white balance correction using color science principles. It implements a three-stage GPU-accelerated pipeline for chromatic adaptation while maintaining accuracy through CPU-side double-precision calculations.

## Core Pipeline

### Three-Stage Shader Pipeline

Each frame passes through the following stages in the HLSL shader (`data/neutral-balance.effect`):

```
[Stage 1] Inverse EOTF: Encode → Linear
[Stage 2] CAT Matrix:   Linear RGB → Adapted Linear RGB  
[Stage 3] Forward EOTF: Linear → Encode
```

#### Stage 1: Inverse EOTF

Converts encoded RGB (sRGB or BT.709) to linear RGB:
- Detects working space from OBS (`obs_source_get_color_space()`)
- Applies inverse transfer function (gamma correction)
- Uses pre-computed 256-entry LUT for performance

#### Stage 2: Chromatic Adaptation Transform (CAT)

Applies 3×3 matrix to adapt colors from measured illuminant to target illuminant:
- Matrix computed on CPU in double precision
- Uploaded to GPU as three `float3` uniforms
- Supports 5 CAT methods: Bradford, CAT02, CAT16, Von Kries (HPE), Diagonal

#### Stage 3: Forward EOTF

Converts adapted linear RGB back to encoded space:
- Applies forward transfer function
- Clips to valid range [0, 1]

## Module Structure

### Source Files

```
src/
├── plugin-main.c       # OBS plugin registration
├── neutral-balance-filter.c  # Main filter implementation (UI, timing)
├── colorimetry.c/h     # Color science functions (pure, no OBS deps)
├── matrix.c/h          # 3×3 matrix operations
├── stats.c/h           # Statistics computation (Grey World, Shades of Grey)
├── capture.c/h         # Frame capture for eyedropper
└── eyedropper.cpp/h    # Qt dialog for manual sampling
```

### Header Files

```
src/
├── neutral-balance-filter.h  # Public filter API
├── colorimetry.h             # Colorimetry functions
├── matrix.h                  # Matrix operations
├── stats.h                   # Statistics types
├── capture.h                 # Capture types
└── eyedropper.h              # Eyedropper dialog
```

## Thread Model

### Render Thread (Primary)

Handles all GPU operations:
- Texture rendering (`gs_texrender_t`)
- Shader parameter updates
- Frame composition

### CPU Thread (Statistics)

Runs periodic analysis:
- Computes 160×90 statistics readback every 100ms (10 Hz)
- Applies temporal smoothing (α = 0.2)
- Updates CAT matrix when illuminant changes

### UI Thread (Qt Dialog)

Manages eyedropper:
- Captures frozen frame on demand
- Displays magnified view with loupe
- Handles color sample selection

## Color Space Handling

### Working Space Detection

The plugin queries OBS for the source's color space:
- `OBS_COLOR_SPACE_SRGB` → Linear sRGB
- `OBS_COLOR_SPACE_BT709` → Linear BT.709
- Other → Fallback to logged sRGB with warning

### White Point

All calculations use D65 (6504K) as the reference white:
- `NB_D65 = {0.95047, 1.0, 1.08883}` (Y-normalized)
- Used for RGB↔XYZ conversion and CAT computations

## Data Flow

### Automatic Mode (Grey World / Shades of Grey)

```
Frame → GPU Readback (160×90) → CPU Stats → Measured Illuminant
                                     ↓
                              CAT Matrix Update
                                     ↓
                              Shader Parameters
```

### Manual Mode (Eyedropper)

```
User Click → Frame Capture → Sample Selection → Linear RGB
                                    ↓
                            Manual Illuminant
                                    ↓
                            CAT Matrix Update
```

## Performance Considerations

### CPU Overhead

- **Statistics**: 10 Hz readback of 160×90 texture (~14KB/frame)
- **Matrix Math**: Double precision on CPU (negligible cost)
- **Smoothing**: Exponential moving average (lightweight)

### GPU Overhead

- **Single-pass shader**: Minimal GPU cost
- **Texture uploads**: Only when CAT matrix changes
- **Split preview**: Optional, adds one texture copy

### Optimizations

1. **Conditional updates**: CAT matrix only recomputed when measurement changes
2. **Lazy shader setup**: Parameters updated only on dirty flags
3. **Temporal smoothing**: Reduces flicker without expensive filtering

## Extensibility

### Adding New CAT Methods

1. Add constant matrix to `colorimetry.c`
2. Add enum value to `nb_cat_method`
3. Add case to `nb_cat_matrix()` switch
4. Update UI combo box in `neutral-balance-filter.c`

### Adding New Measurement Methods

1. Implement statistics function in `stats.c`
2. Add enum to `nb_method`
3. Update UI in `neutral-balance-filter.c`
4. Document in README.md

## Testing Strategy

### Unit Tests (`tests/test_colorimetry.c`)

Test pure math functions without OBS dependencies:
- RGB↔XYZ conversions
- CAT matrix computations
- Lab/LCh conversions
- Delta E 2000 calculations

### Integration Tests

Test in OBS Studio:
- Filter application to sources
- Eyedropper functionality
- UI interaction
- Cross-platform compatibility

## References

See [references.md](references.md) for detailed citations of color science standards and papers.

---

For development questions, refer to [CONTRIBUTING.md](CONTRIBUTING.md).
