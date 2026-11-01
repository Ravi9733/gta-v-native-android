# Android port — engineering status

This directory is the Android-port work area for the native ARM64 GTA V host.

## Important: current source is not Android-buildable yet

The published host is an Apple-platform implementation, not a portable C/C++ app that can be repackaged. The current build recipes invoke `xcrun clang`, link Apple frameworks (Metal, MetalFX, UIKit/Cocoa, CoreVideo, AudioToolbox, GameController and others), and compile the NVN graphics implementation in `third_party/d2r_gfx/gfx/nvn/nvn_mtl.m`. There is no Android Gradle project, Android NDK target, Vulkan renderer, or Android frontend in the source snapshot.

Adding only an APK wrapper or changing compiler flags would therefore produce a non-working app. The Android port needs real platform and renderer work.

## Port architecture

1. **Android application shell** — Gradle application, ARM64-only ABI, Android NDK/CMake, JNI entry point, lifecycle and surface handling.
2. **Host platform layer** — Android implementations for app lifecycle, filesystem paths, timing/threads, input/controller, audio output and diagnostics. Keep game data external; never bundle copyrighted game data.
3. **Graphics backend** — replace the Apple Metal calls in the NVN host backend with a Vulkan implementation. Preserve the existing guest-facing NVN interfaces so runtime callers do not need broad rewrites. Translate shaders to SPIR-V/Vulkan and implement resource, descriptor, pipeline, synchronization and presentation paths.
4. **Dependency/build layer** — Android builds of Opus, libvpx and shader tools; pin NDK/CMake/dependency versions and add CI.
5. **Validation** — compile smoke tests first, then renderer tests, then boot using user-supplied matching game files. No claim of gameplay support until device validation passes.

## Current verified baseline

- The repository is a fork of `slavich33/gta-v-native-ios`.
- Existing iOS project targets iOS 26 and uses XcodeGen.
- Existing Makefile is Apple-host-only and links Apple frameworks.
- The source README reports an experimental iOS baseline, with rendering artifacts, low city performance and freezes still unresolved.
- Game programs, archives, generated guest code, caches and signed apps are intentionally not part of this repository.

## Immediate implementation order

- [x] Add Android Gradle/NDK application shell, JNI smoke-test app and ARM64 CI workflow (workflow has not yet been observed completing).
- [ ] Add Android lifecycle/JNI frontend.
- [ ] Introduce a renderer interface separating guest NVN calls from Metal-specific implementation.
- [ ] Implement Vulkan backend and shader translation path.
- [ ] Port audio, input and filesystem services.
- [ ] Build and test on a physical Android device.

This status file is deliberately explicit: an Android APK is not ready until the platform and Vulkan work above is implemented and tested.
