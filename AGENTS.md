# Repository Guidelines

## Project Structure & Module Organization
- `rts_core/include/rts/` and `rts_core/src/`: deterministic C++ simulation and batched environments.
- `game/`: input, commands, map loading, and gameplay integration; `game/data/` holds maps and balance tables, while `game/testdata/` holds fixtures.
- `render/`: optional raylib frontend; `bindings/`: pybind11 Python interface; `train/`: PyTorch PPO, evaluation, and export scripts.
- `tests/`: C++ and Python regression tests; `tools/`: map generation, calibration, and analysis; `cmake/`: shared build configuration.
- Consult `README.md`, `CLAUDE.md`, and the relevant module README before changing behavior or interfaces.

## Build, Test, and Development Commands
Use CMake 3.20+, a C++20 compiler, and Python 3. Run from the repository root; Windows builds require an initialized compiler environment.

```sh
cmake -S . -B build-guide -DCMAKE_BUILD_TYPE=Release
cmake --build build-guide --config Release
ctest --test-dir build-guide -C Release --output-on-failure
```

These configure, build, and test the default headless project. Always rebuild before testing. Initial configuration downloads dependencies.

- Add `-DRTS_BUILD_RENDER=ON` to enable the frontend. Build into `build/` to use the Windows `play.bat` launcher.
- Enable `-DRTS_BUILD_BINDINGS=ON -DRTS_TEST_TRAINING=ON` for training integration tests; follow `train/README.md` for Python development headers and dependencies.
- Existing `build_test*.bat` helpers contain machine-specific paths; inspect them before use.

## Coding Style & Naming Conventions
Match surrounding code: four-space indentation, `snake_case` functions/files, `PascalCase` C++ types, and trailing underscores for private members. Use English identifiers and the documented Chinese display names. Simulation entity names follow the registered naming table and seven-character limit in `CLAUDE.md`.

No shared formatter/linter configuration is present; compiler warnings are errors by default. Keep changes focused.

## Testing Guidelines
Use Catch2 v3 for C++ tests (`tests/*_test.cpp`) and the existing Python `unittest` pattern (`tests/*_test.py`, methods `test_*`). Register new tests in the appropriate CMake configuration. No numeric coverage threshold is configured; add regression coverage for changed behavior. Run renderer or training tests when those components change.

Preserve fixed 20 Hz ticks, seeded `rts::Rng`, and deterministic replay. Keep rendering read-only and Python callbacks out of simulation hot paths. Measure performance in Release.

## Commit & Pull Request Guidelines
Follow history's `type(scope): summary` pattern, such as `fix(rl): preserve replay state`; common types include `feat`, `fix`, `docs`, and `exp`. English and Chinese summaries are used.

Describe the problem, behavior change, relevant issues, and validation commands/results. Include screenshots for visual changes and seeds/configuration for simulation or training results. Keep generated builds and experiment outputs out of commits.
