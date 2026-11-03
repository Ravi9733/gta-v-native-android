# GTA V Native Android — independent Android bootstrap

This directory is a separate Android application experiment. It does **not** copy the
reference project's Android app, game runtime, renderer, generated game code, or assets.

## Current milestone

- Native Android app shell (Kotlin + JNI).
- ARM64-only NDK build.
- Vulkan loader/device enumeration screen.
- GitHub Actions debug-APK build.

This is only a platform/bootstrap milestone. It is **not a playable GTA V port** and contains
no GTA V executable, game data, shaders, or copyrighted assets.

## Build

Open this directory as a Gradle project in Android Studio, or run:

```sh
gradle assembleDebug
```

The APK is written to `app/build/outputs/apk/debug/app-debug.apk`.

## Roadmap

1. Validate Android lifecycle, Vulkan loader, and Adreno device enumeration.
2. Add a Vulkan surface and render-pass/device capability tests.
3. Design an independent guest-runtime boundary and file-system abstraction.
4. Research a legally distributable Xbox 360 guest-code recompilation pipeline.
5. Add touch/controller input, audio, save paths, and performance diagnostics.

Bring your own legally obtained game files. This repository intentionally contains no game data.
Third-party components, if added later, must retain their original licenses and notices.
