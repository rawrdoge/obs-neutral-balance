# CONTRIBUTING

Thank you for your interest in contributing to `obs-neutral-balance`! This document provides guidelines and instructions for contributing to the project.

## Table of Contents

- [Code of Conduct](#code-of-conduct)
- [Getting Started](#getting-started)
- [Development Workflow](#development-workflow)
- [Coding Standards](#coding-standards)
- [Testing](#testing)
- [Submitting Changes](#submitting-changes)
- [Documentation](#documentation)
- [Reporting Issues](#reporting-issues)
- [License](#license)

## Code of Conduct

This project adheres to a code of conduct that aims to foster an open and welcoming environment. Please be respectful and constructive in all interactions.

## Getting Started

### Prerequisites

- OBS Studio 28.0+ (with development headers)
- CMake 3.16+
- C17 compatible compiler (GCC, Clang, or MSVC)
- Qt6 or Qt5 (optional, for eyedropper dialog)

### Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Running Tests

```bash
cmake --build build --target test
```

## Development Workflow

1. **Fork the repository** on GitHub
2. **Create a feature branch** from `main`:
   ```bash
   git checkout -b feature/your-feature-name
   ```
3. **Make your changes** with clear, descriptive commit messages
4. **Run tests** to ensure nothing is broken
5. **Push your branch** to your fork
6. **Create a Pull Request** against the `main` branch of the original repository

## Coding Standards

### General

- Use C17 standard (`C_STANDARD 17` in CMakeLists.txt)
- Follow the existing code style (see `.clang-format`)
- Use meaningful variable and function names
- Comment complex logic, but avoid obvious comments
- Keep functions focused and modular

### Color Science Accuracy

- All color transformations must be mathematically precise
- Use double precision for colorimetry calculations (`colorimetry.c`)
- Document the mathematical basis of your changes
- Reference relevant standards (CIE, IEC, ITU)

### OBS Plugin Development

- Follow OBS plugin development best practices
- Use `libobs` APIs correctly
- Handle memory management properly
- Support localization (`obs_module_text()`)

## Testing

### Unit Tests

We use a lightweight C test framework for unit testing the colorimetry functions:

```bash
# Build tests
cmake -DBUILD_TESTING=ON -S . -B build
cmake --build build

# Run tests
./build/tests/test_colorimetry
```

### Integration Tests

Test the filter in OBS Studio:
1. Add the filter to a video source
2. Verify white balance correction works
3. Test different CAT methods
4. Check eyedropper functionality
5. Validate split preview

## Submitting Changes

### Pull Request Guidelines

- **Title**: Clear and descriptive (e.g., "Add CAT16 chromatic adaptation support")
- **Description**: Explain what and why, not just what changed
- **Linked Issues**: Reference any related issues
- **Testing**: Describe how you tested the changes
- **Screenshots**: Include visuals for UI changes

### Commit Message Format

```
type(scope): concise description

Detailed explanation of changes (if needed).
```

Types: `feat`, `fix`, `docs`, `style`, `refactor`, `test`, `chore`

Example:
```
feat(colorimetry): add CAT16 chromatic adaptation transform

Implements the CAM16-based CAT16 matrix as specified by
Li et al. (2016). Added to existing CAT selection menu.
```

## Documentation

### Code Documentation

- Document public APIs in header files
- Use Doxygen-style comments:
  ```c
  /**
   * @brief Convert linear RGB to CIE XYZ
   * @param rgb Linear RGB values (0-1 range)
   * @param xyz Output CIE XYZ values
   */
  void nb_rgb2xyz(const double rgb[3], double xyz[3]);
  ```

### User Documentation

- Update README.md for user-facing changes
- Document new features with usage examples
- Update INSTALL.md for build/install changes

### Architecture Documentation

See [ARCHITECTURE.md](ARCHITECTURE.md) for detailed technical documentation.

## Reporting Issues

When reporting issues, please include:

1. **Description**: Clear summary of the problem
2. **Steps to Reproduce**: How to trigger the issue
3. **Expected Behavior**: What should happen
4. **Actual Behavior**: What actually happens
5. **Environment**:
   - OBS Studio version
   - Operating system
   - GPU model
   - Plugin version
6. **Screenshots/Logs**: If applicable

## License

By contributing to `obs-neutral-balance`, you agree that your contributions will be licensed under the GNU General Public License v2.0 or later.

---

Thank you for helping improve obs-neutral-balance! 🎨
