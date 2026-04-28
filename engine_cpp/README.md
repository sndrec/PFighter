# PFighter C++ Raylib Prototype

This is the new C++ engine scaffold for the custom 2.5D platform fighter. The existing Godot project is reference material.

## Build

Headless sim verification does not need raylib:

```powershell
cmake -S engine_cpp -B engine_cpp/build -G "MinGW Makefiles" -DPFIGHTER_BUILD_RAYLIB_APP=OFF
cmake --build engine_cpp/build
engine_cpp/build/pfighter_headless.exe
```

The raylib app auto-detects a sibling extracted raylib package such as `../raylib-6.0_win64_mingw-w64`. You can also pass `-DPFIGHTER_RAYLIB_DIR=C:/path/to/raylib`.

```powershell
cmake -S engine_cpp -B engine_cpp/build-raylib -G "MinGW Makefiles"
cmake --build engine_cpp/build-raylib
engine_cpp/build-raylib/pfighter_raylib.exe
```

If no local raylib package is available, use `-DPFIGHTER_FETCH_RAYLIB=ON` to let CMake download raylib.

## Current Controls

- Player 1: `A/D` move, `W` jump, `F` attack
- Player 2: arrow keys move, right shift jump, enter attack
- Editor/debug: `F1` boxes, `[` and `]` selected state, space pause, `R` reset

## Design Notes

The sim is deterministic-friendly: gameplay values use fixed-point storage, rendering stays outside the core, and runtime bone poses are stored in fighter state because hitboxes and hurtboxes depend on animation.

Movement states run named callbacks from `src/core/state_functions.cpp`. Those callbacks are meant to become Melee-common behavior ports, while fighter data decides which states call them.
