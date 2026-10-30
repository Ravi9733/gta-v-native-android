# Component notices

Original copyright, permission, and embedded notices are preserved. This is a modified experimental source snapshot, not an unmodified release of any upstream project. There is no blanket replacement license for the collection.

| Component | Notice retained in this snapshot |
|---|---|
| nxnative-derived host/integration source, sampled-view-cache and triangle-fan adaptations, copyright 2026 seedds | `LICENSES/nxnative-MIT.txt` and exact component copy `third_party/d2r_gfx/gfx/nvn/LICENSE.nxnative` (MIT) |
| D2R Mobile rewriter and renderer, copyright 2026 jhaizhou | `third_party/d2r/LICENSE`, `third_party/d2r_gfx/LICENSE`, `tooling/totk_xlate/LICENSE` (MIT) |
| MacNativeGames-derived runtime/frontend/translator adaptations, copyright 2026 MacNativeGames contributors | `LICENSES/MacNativeGames-MIT.txt` and `third_party/d2r_gfx/gfx/nvn/LICENSE.MacNativeGames.txt` (MIT) |
| Ryujinx shader/common source subset, Ryujinx Team and Contributors | `tooling/ryujinx/LICENSE.txt` (MIT) |
| Ryujinx-derived audio algorithms | `third_party/licenses/Ryujinx-Audio.txt` (the Ryujinx Team and Contributors MIT notice) |
| Spv.Generator | Its separate `tooling/ryujinx/src/Spv.Generator/LICENSE` |
| Reconstructed NVN API header, copyright 2020 h1k421 | Embedded MIT notice in `third_party/d2r_gfx/gfx/nvn/include/nvn.h` |
| deko3d-derived texture layout, copyright 2018–2020 fincs | `third_party/d2r_gfx/LICENSE.deko3d` (zlib) and source comments |
| bcdec, copyright 2022 Sergii Kudlai | `third_party/d2r_gfx/third_party/bcdec/LICENSE` and embedded header text (MIT or Unlicense alternatives) |
| SPIRV-Cross, separately fetched dependency | `LICENSES/SPIRV-Cross-Apache-2.0.txt` (Apache-2.0); no SPIRV-Cross source or library is bundled |

The original rewriter/renderer provenance identifies D2R Mobile commit `9f0c25768ac4b231c6f4cb0a9f182ded68d0e7cf`. The sampled-view-cache adaptation is based on nxnative commit `beea17cfdbd8fe6112de9d1c191c0488da7970bf` and preserves its seedds MIT notice. The local shader tool identifies a Ryujinx 1.3.2 subset with local translator patches. The native bridge build recipe checks SPIRV-Cross commit `aa217aeb6c9f0ace7a0ab233b28807edf45eb165`.

External Opus, libvpx, .NET runtime components, Apple SDKs, and other build dependencies are not bundled. Their own source/runtime license and patent notices must be considered when constructing or distributing binaries. No game assets or rights to game content are included.

The argument-resolution and pipeline-key adaptations were reviewed against MacNativeGames commit `51cd535d1b6d33433c730fc8f976250d96c9a71f`. Local changes are not represented as an upstream release.
