# Android host adapter contract

The existing project already separates part of the host frontend from the guest runtime. The iOS frontend explicitly describes itself as the counterpart of `runtime/frontend.m` and exports the same VI/window-facing entry points. Android should follow that pattern rather than forking game/runtime logic.

## Existing host adapters to replace

| Existing source | Apple dependency / responsibility | Android replacement |
|---|---|---|
| `runtime/frontend.m` | macOS window, CAMetalLayer, display link, VI presentation, focus and text input | Android Activity/Surface lifecycle, ANativeWindow or SurfaceView, Choreographer/vsync, soft keyboard |
| `runtime/frontend_ios.m` | UIKit view, CAMetalLayer, CADisplayLink, VI presentation | Reference for frontend contract only; Metal layer must not be reused |
| `runtime/gamepad.m` | GameController discovery and controller input | Android input events / game controller APIs plus touch controls |
| `runtime/audio_out.c` | Current audio device output adapter | AAudio (preferred where available), with a compatible fallback |
| `third_party/d2r_gfx/gfx/nvn/nvn_mtl.m` | Metal device, buffers, command queue, events, textures and presentation | New Vulkan backend; this is the largest blocking subsystem |

## Frontend contract

The two Apple frontends currently provide these shared entry points (the Android adapter must be checked against every external reference before implementation):

- Boot/status reporting: `runtime_boot_progress`, `runtime_boot_count`, `runtime_boot_status`, `runtime_warm_progress`, `runtime_warm_done`
- Presentation and display: `front_frame_presented`, `front_game_res`, `vi_initialize`, `vi_open`, `vi_layer`, `vi_layer_settings`, `vi_layer_size`, `vi_close_display`, `vi_destroy_layer`, `vi_finalize`, `vi_window`, `vi_vsync`
- Window/app state: `runtime_front_focus`, `runtime_front_request_close`, `runtime_front_run`
- Text input: `runtime_front_text_input`, `runtime_front_text_test`

This list is a starting inventory, not a complete ABI guarantee. Before writing the Android frontend, compare all definitions and call sites in the runtime and HLE implementation.

## Vulkan backend compatibility requirements

The current Metal header exposes an NVN execution-layer contract, not a generic renderer. A Vulkan implementation must preserve its ordering and lifetime guarantees, including:

- GPU-address-to-buffer lookup for guest memory pools.
- Ordered command submission, copy operations, completion events and counters.
- Fence wait/finish/drain semantics.
- Resource lifetime across in-flight submissions.
- Texture and render-target views, compressed formats, depth/stencil, storage views and shader pipelines.
- Swapchain/window textures and presentation synchronization.

A superficial wrapper around Vulkan calls will not be sufficient. In particular, completion events must not be reported before the associated GPU work has actually completed.

## Next code milestone

Implement the Android frontend only after defining the renderer boundary and Android surface ownership. A frontend that creates a window but still calls Metal is not a usable Android port.
