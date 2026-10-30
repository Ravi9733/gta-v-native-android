# GTA V native development build


This snapshot has not been rebuilt from scratch on a clean machine. Recipes are development tools that may require the matching upstream layout. Reproducibility without the omitted inputs is not established.

Required tools include full Xcode/iPhoneOS SDK, XcodeGen, Python dependencies used by the rewriter scripts, .NET 10 with its NativeAOT runtime pack, and CMake/Ninja for native dependencies. The project targets iOS 26. Supply your own Apple development identity for installation.

1. Supply matching extracted and rewritten modules in a separate GTA_build/ work tree, following scripts/prepare_gta.py and its rewrite metadata layout. The iOS builder also requires generated ios-embed/ files. Generated guest inputs are deliberately absent.
2. Generate the loader/config headers with python3 scripts/prepare_gta.py --work GTA_build. Keep the matching hash-only revision checks.
3. Build translator dependencies separately. Existing CLI recipes in tooling/totk_xlate/ were written for the upstream layout. RyuSrc can select tooling/ryujinx; local patches are already reflected there and should not be applied twice.
4. Follow tooling/ios-translator-prototype/README.md for the device translator. Fetch/build pinned SPIRV-Cross separately, then package/hash fresh artifacts in ios/ShaderTranslator/artifacts.json as required by the builder. No prebuilt artifact manifest substitutes for these builds.
5. Build Opus/libvpx for the target using the matching upstream dependency recipe and retain their notices.
6. Review ios/build_ipa_gta.py, ios/project.yml and ios/install_device_gta.py before building/signing/installing. The strict completion-overflow and optional job-ledger composers require exact source pins and matching external images. Normal mode is the default; use `--job-ledger` only for an explicitly selected diagnostic build. Generated ios/Gen/completion_overflow_sites.h, any diagnostic return-site header and image/digest headers must be produced together; they are absent from this export. These operations were not executed by the export.
7. Pass device arguments explicitly. Legacy data-push scripts require GTA_GAME_DATA to identify your own data tree; the host fallback is relative GameData/gta.

