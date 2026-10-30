# GTA V Native iOS & macOS Port — ARM64 / Metal

Experimental **Grand Theft Auto V (GTA 5) native iPhone and Apple silicon Mac** host source. The project adapts a homebrew Switch port with Horizon service replacements, an NVN-to-Metal renderer, touch controls, audio integration and shader-translation tooling.

## Current status

A local iOS baseline reached Franklin in Los Santos. This remains an experimental port: low city frame rate, magenta rendering artifacts and intermittent freezes are unresolved. The current source snapshot includes committed native-host/MetalFX development through `b81a519`; frame interpolation and clean-machine reproducibility remain research. A source publication is not a new device-runtime validation.

## Source

- `runtime/`: ARM64 host, services, audio and diagnostics.
- `ios/`: iPhone frontend, build helpers and translator integration.
- `third_party/d2r_gfx/`: modified NVN / Metal graphics backend.
- `tooling/`: shader translation and native translator prototypes.
- `scripts/`: preparation of locally supplied game inputs.

See [development build steps](docs/BUILD.md) and [component provenance/licenses](THIRD_PARTY_NOTICES.md). Dependencies and generated embedding files must match your own game version. This source snapshot has not been validated as a one-command clean-machine distribution.

## Inputs and attribution

Supply your own legally obtained game files and Apple signing configuration. Game programs/archives, extracted game shaders, generated guest code, caches, saves, device identifiers and signed apps are outside this repository.

The host builds on [MacNativeGames](https://github.com/ly05010419/MacNativeGames), [D2R Mobile](https://github.com/jhaizhou-ops/d2r-mobile), [nxnative](https://github.com/seedds/nxnative) and the retained Ryujinx tooling subset. Original component notices are preserved; this collection has component-specific licensing. This is an unofficial community project.

По-русски: экспериментальный нативный порт GTA V / GTA 5 для iPhone, iOS и Mac на ARM64 с Metal. Здесь исходники адаптации и инструменты сборки; стабильность и производительность ещё дорабатываются.
