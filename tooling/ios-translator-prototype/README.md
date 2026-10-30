# In-process translator source

These are the actual C# NativeAOT adapter and native C++ bridge sources used by the device integration. `Program.Generated.cs` and `Service.Generated.cs` are mechanically adapted **translator tooling source**, not generated guest game code. They retain the existing v4 shader transformation/reflection pipeline while replacing CLI/file/process operations with a serialized in-memory ABI. The underlying Ryujinx source is included once at `tooling/ryujinx/`; the project references that copy.

Existing NativeAOT build route, with a compatible .NET10 SDK and iOS runtime pack available:

```sh
dotnet publish tooling/ios-translator-prototype/translator/GtaShaderAot.csproj \
  -c Release -r ios-arm64 -o build/ios-translator/native
```

The target is a shared native ARM64 iOS library. The host expects an embedded `GtaShaderAot.framework` with that library's install name set to `@rpath/GtaShaderAot.framework/GtaShaderAot`, plus the exported C header, Info.plist, and module map. Framework packaging and signing outputs are not in this source backup. The NativeAOT library remains loaded for the life of the process.

The SPIRV-Cross bridge recipe is `spirv-cross/build.py`. Set `GTA_SPIRV_CROSS_SOURCE` to your own clean checkout at commit `aa217aeb6c9f0ace7a0ab233b28807edf45eb165`; its default is the omitted `GTA_build/src/SPIRV-Cross` work directory. The script checks the pin and builds device/macOS artifacts. It is retained as the existing recipe and was not executed by this export.

The existing iOS wrapper expects `ios/ShaderTranslator/GtaShaderAot.framework`, `libgta_spirv_cross.a`, `gta_spirv_cross.h`, and a freshly generated `artifacts.json` containing their SHA256 values. Copy only your own newly built artifacts there before invoking that wrapper. This snapshot does not claim a verified clean-machine build or supply cached binaries.

The adaptation generator is included for provenance; generated C# files are already present. Source changes to the translator and its patched Ryujinx inputs must be kept in sync. This source collection does not enable the separate experimental geometry/tessellation prototypes.
