# Colorimetry Unit Testing Research

## References

- **RGB to XYZ Matrix**: Confirmed against Springer article "RGB color correction and gamut limitations in smartphone-based kinetic analysis" and Adobe RGB spec. Matrix matches exactly: `[[0.4124564, 0.3575761, 0.1804375], [0.2126729, 0.7151522, 0.0721750], [0.0193339, 0.1191920, 0.9503041]]`.
- **CAT Matrices**: Bradford, CAT02, CAT16 all validated against literature:
  - Bradford: `[[0.8951, 0.2664, -0.1614], [-0.7502, 1.7135, 0.0367], [0.0389, -0.0685, 1.0296]]` (ICC v4, deepwiki.com)
  - CAT02: `[[0.7328, 0.4296, -0.1624], [-0.7036, 1.6975, 0.0061], [0.0030, 0.0136, 0.9834]]` (CIECAM02 standard)
  - CAT16: `[[0.401288, 0.650173, -0.051461], [-0.250268, 1.204414, 0.045854], [-0.002079, 0.048952, 0.953127]]` (Li et al., CAM16)
- **Lab/LCh**: Standard CIE 1976 formulas. Verified against CSS Color Module Level 4, Kornia, and OpenCV implementations.
- **DeltaE 2000**: Full implementation matches Sharma, Wu & Dalal (2005), Color Research & Application 30(1), 21-30. Verified against colour-science library reference implementation.

## Test Framework Recommendation

Use `utest.h` (single header, no dependencies) for lightweight C unit tests. It provides `utest_setup`, `utest_teardown`, `utest_assert_*` macros. Zero external dependencies, perfect for embedded testing in OBS plugin.

## Reference Test Cases

1. **RGB→XYZ**: Test identity matrix (RGB(1,1,1) → XYZ(0.95047,1.0,1.08883) when using D65 white point).
2. **XYZ→RGB**: Round-trip test (XYZ→RGB→XYZ should be identity within EPS).
3. **CAT Bradford**: Known transform (D65→D50) from ICC v4 spec.
4. **CAT Diagonal**: Should equal identity matrix.
5. **Lab→LCh→Lab**: Round-trip test for all L in [0,100], C in [0,100], h in [0,360].
6. **DeltaE 2000**: Compare against colour-science implementation for 100 random pairs.

All tests use double precision and EPS=1e-6.