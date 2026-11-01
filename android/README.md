# Android ARM64 development shell

This is the first buildable Android application layer for the port effort.

## Build

Requirements:
- JDK 17
- Android SDK platform 35 and build tools 35
- Android NDK 27.2.12479018
- CMake 3.22.1
- Gradle 8.9

Run:

```sh
gradle --no-daemon :app:assembleDebug
```

The app is ARM64-only and currently builds a native JNI library. It is a
platform-shell smoke test, **not a playable game**.

## What is not connected yet

- The upstream guest runtime and generated guest inputs.
- Android replacements for the macOS/iOS frontend and platform services.
- The Metal NVN execution layer's Vulkan replacement.
- Android audio, controller/touch input and game-data loading integration.

Do not install this scaffold expecting GTA V gameplay. The screen explicitly
reports that the game runtime and Vulkan renderer are not connected.
