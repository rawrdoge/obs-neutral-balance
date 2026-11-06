---
id: 2
title: Standalone Unit Testing Architecture for Colorimetry & Math
type: research
status: open
blocked_by: []
---

## Question

Which lightweight C test framework (e.g., `utest.h`, `greatest.h`, or simple standard C test harness) should be introduced to unit-test `colorimetry.c` and `matrix.c` independently without requiring OBS Studio runtime headers or libobs linking?

## Spec & Requirements to Resolve
1. Select a zero-dependency header-only C test framework.
2. Define tests for CAT matrix calculations (Bradford, CAT02, CAT16, von Kries, Diagonal).
3. Define tests for CIE LCh <-> XYZ <-> Linear RGB conversions and DeltaE 2000 calculations against reference values.
4. Update `CMakeLists.txt` with a `BUILD_TESTING` option that compiles test executables independently.
