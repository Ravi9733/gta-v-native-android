# GTA V Native Android — independent Android bootstrap

This branch contains a separate Android prototype module under `android-vulkan/`.
It does not include GTA V game code, executables, assets, generated game code, or shaders.

## Completed prototype milestones

- Kotlin Android activity and ARM64-only NDK module.
- JNI bridge into native C++.
- Vulkan loader and physical-device enumeration.
- Android `SurfaceView` integration.
- Native checks for Android surface and swapchain support.
- First Vulkan logical-device and swapchain creation path.
- One-shot clear-colour render pass, command submission, and presentation on surface resize.

The frame test creates the Vulkan objects, presents one clear-colour frame, and releases
the resources. It is a renderer bootstrap test, not a continuous frame loop. Swapchain
recreation, synchronization across multiple frames, and device-loss recovery are not yet
implemented. This is not a playable GTA V port.

## Build

Open this directory as a Gradle project in Android Studio, or run:

```sh
gradle assembleDebug
```

The APK is written to `app/build/outputs/apk/debug/app-debug.apk`.

## Next engineering milestones

1. Persistent frame loop with frame-in-flight synchronization.
2. Lifecycle-safe swapchain recreation and resize handling.
3. Add a basic shader pipeline and draw a test triangle.
4. Design an independent guest-runtime boundary and file-system abstraction.
5. Research a legally distributable Xbox 360 guest-code recompilation pipeline.
6. Add touch/controller input, audio, save paths, and performance diagnostics.

Bring your own legally obtained game files. No proprietary game data is stored here.
Any third-party code or libraries added later must retain their original licenses and notices.
