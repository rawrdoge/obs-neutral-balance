# References for Colorimetry

This file documents the primary references for the colorimetry functions implemented in `src/colorimetry.c` and `src/matrix.c`.

## RGB to XYZ Conversion

The transformation from linear RGB to CIE XYZ uses the sRGB/BT.709 primaries with D65 white point, as defined in:
- [ITU-R BT.709](https://www.itu.int/rec/R-REC-BT.709)
- [IEC 61966-2-1:1999 - sRGB](https://www.iec.ch/)

The matrix coefficients are taken directly from these standards:

```
[ 0.4124564  0.3575761  0.1804375 ]
[ 0.2126729  0.7151522  0.0721750 ]
[ 0.0193339  0.1191920  0.9503041 ]
```

## Chromatic Adaptation Transforms (CAT)

The CAT matrices used in this project are sourced from:
1. **Bradford**: Original paper by R. S. Hunt, "Measuring Colour" (3rd Ed.), Appendix B.
2. **CAT02**: From Fairchild's "Color Appearance Models" (2nd Ed.), Section 4.2.
3. **CAT16**: From Li et al., "CAM16 color appearance model", Color Research & Application, 2016.
4. **Von Kries (HPE)**: From Hunt's Appendix B, using the HPE cone fundamentals.

## CIELAB and CIELCh Conversions

The CIELAB and CIELCh conversions follow the CIE 1976 standard:
- [CIE 1976](https://cie.co.at/publications/iso-cie-1976-lab-cielch)
- Implemented according to the formulas in Wyszecki & Stiles, "Color Science: Concepts and Methods"

## Delta E 2000

The Delta E 2000 implementation follows:
- Sharma, G., Wu, W., & Dalal, E. N. (2005). "The CIEDE2000 color-difference formula." Color Research & Application, 30(1), 21-30.

## Reference Test Cases

### Test 1: RGB to XYZ conversion of pure red
- Input: linear RGB = (1, 0, 0)
- Expected XYZ: (0.4124564, 0.2126729, 0.0193339)

### Test 2: RGB to XYZ conversion of pure green
- Input: linear RGB = (0, 1, 0)
- Expected XYZ: (0.3575761, 0.7151522, 0.1191920)

### Test 3: RGB to XYZ conversion of pure blue
- Input: linear RGB = (0, 0, 1)
- Expected XYZ: (0.1804375, 0.0721750, 0.9503041)

### Test 4: Round-trip test (RGB -> XYZ -> RGB)
- Input: linear RGB = (0.5, 0.25, 0.75)
- Expected: Output RGB should match input within epsilon (1e-6)

### Test 5: CIE white point (D65)
- Input: linear RGB = (1, 1, 1)
- Expected XYZ: (0.95047, 1.0, 1.08883)

### Test 6: Bradford CAT from D65 to D50
- Source white point (D65): (0.95047, 1.0, 1.08883)
- Destination white point (D50): (0.96422, 1.0, 0.82521)
- Expected CAT matrix can be verified against ICC v4 specification.

### Test 7: LAB to LCH round-trip
- Input LAB = (50, -10, 20)
- Convert to LCH, then back to LAB
- Expected output LAB should match input within epsilon

### Test 8: Delta E 2000 identity
- Input: Two identical LAB colors (e.g., (50, 0, 0))
- Expected Delta E 2000 = 0

### Test 9: Maximum Delta E 2000
- Input: Black (0, 0, 0) vs White (100, 0, 0)
- Expected Delta E 2000 = 100

### Test 10: CAT Diagonal method (identity)
- Input: Any source and destination white points that are equal
- Expected: Output CAT matrix is the identity matrix
