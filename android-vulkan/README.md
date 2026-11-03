# GTA V Native Android — independent Android bootstrap

This branch contains a separate Android prototype module under `android-vulkan/`.
It does not include GTA V game code, executables, assets, generated game code, or shaders.

## Completed prototype milestones

- Kotlin Android activity and ARM64-only NDK module.
- JNI bridge into native C++.
- Vulkan loader and physical-device enumeration.
- Android `SurfaceView` integration.
- Native checks for `VK_KHR_surface`, `VK_KHR_android_surface`,
  graphics-queue presentation support, and `VK_KHR_swapchain`.

The surface check creates and destroys a Vulkan Android surface and reports device
capabilities. It does **not** create a swapchain, submit draw commands, or render frames.
This is not a playable GTA V port.

## Build

Open this directory as a Gradle project in Android Studio, or run:

```sh
gradle assembleDebug
```

The APK is written to `app/build/outputs/apk/debug/app-debug.apk`.

## Next engineering milestones

1. Create a Vulkan logical device and Android swapchain.
2. Add a minimal clear-colour render pass and frame presentation loop.
3. Add lifecycle-safe swapchain recreation on rotation/surface resize.
4. Design an independent guest-runtime boundary and file-system abstraction.
5. Research a legally distributable Xbox 360 guest-code recompilation pipeline.
6. Add touch/controller input, audio, save paths, and performance diagnostics.

Bring your own legally obtained game files. No proprietary game data is stored here.
Any third-party code or libraries added later must retain their original licenses and notices.
