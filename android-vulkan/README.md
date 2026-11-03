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
- Surface-format, color-attachment usage, and non-zero extent validation.

The frame test creates the Vulkan objects, presents one clear-colour frame, and releases
the resources. It is deliberately a one-shot proof of presentation, not a continuous
frame loop. The next renderer milestone is a persistent device/swapchain context with
per-frame synchronization and safe recreation after surface changes. This is not a
playable GTA V port.

## Build

Open this directory as a Gradle project in Android Studio, or run:

```sh
gradle assembleDebug
```

The APK is written to `app/build/outputs/apk/debug/app-debug.apk`.

## Next engineering milestones

1. Persistent Vulkan device/swapchain context and frame-in-flight synchronization.
2. Lifecycle-safe swapchain recreation and resize handling.
3. Add a basic shader pipeline and draw a test triangle.
4. Design an independent guest-runtime boundary and file-system abstraction.
5. Research a legally distributable Xbox 360 guest-code recompilation pipeline.
6. Add touch/controller input, audio, save paths, and performance diagnostics.

Bring your own legally obtained game files. No proprietary game data is stored here.
Any third-party code or libraries added later must retain their original licenses and notices.


## Frame synchronization groundwork

The one-shot swapchain test now attaches a Vulkan fence to graphics submission and waits for
that fence before presenting. This makes completion of the submitted command buffer explicit
instead of relying only on queue-wide idle synchronization. The test still creates and destroys
the Vulkan instance, device, swapchain, and related objects for each invocation; it is **not**
the persistent renderer yet.

Next renderer milestone: move instance/device/queue ownership into a persistent native renderer
context, then retain per-frame fences/semaphores and recreate only swapchain-dependent resources
when Android reports a surface resize or replacement. A device run is still required to validate
actual presentation on target hardware.
