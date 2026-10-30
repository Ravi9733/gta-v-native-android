# GTA iOS async completion ready ordering

`patch_gta_async_ready.py` prepares the pinned September GTA Switch executable
for the iOS embedded image. It changes the generated iOS `main.x0.bin` and the
main-x0 SHA256 field in both generated `ios_embed.h` files.

The original producer submits an I/O request at `main+0x276d6b0`, then clears
`job+0x184c` at `0x276d6dc`. Its completion callback at `0x276da70` sets that
byte to one. The request is published and its semaphore signalled before submit
returns, so completion can precede the original clear.

The patch branches from the existing NOP at `0x276d6a0` into verified RX padding
at `0x29efe50`, resets the single ready byte, and resumes at `0x276d6a4`.
The old late reset becomes two NOPs. The submit arguments, callback/context,
neighbouring bytes, NZCV, LR, and image size are preserved.

Check without writing:

```sh
python3 /Users/your-user/Games/gta-port/ios/patch_gta_async_ready.py --check --self-test
```

When application of the offline patch is authorized, after backing up the generated files:

```sh
python3 /Users/your-user/Games/gta-port/ios/patch_gta_async_ready.py --apply
```

The script accepts only the pinned original SHA256 or its exact patched result;
it rejects unknown images, unexpected instruction bytes, changed padding, and
unrecognized main digest rows before writing. A repeated application is safe.
Known mixed original/patched header states can be completed after an interrupted run.

`ios/Gen/embed.S` dependency metadata does not track `.incbin` file contents.
The script refreshes the mtimes of both `GTA_build/ios-embed/gen` and
`ios/Gen/embed.S` after a change. The source `gen` mtime matters because
`build_ipa_gta.py` copies it to `embed.S` with `copy2`.

The script verifies that the Mac rewrite and Mac executable remain unchanged.
It performs no build, signing, installation, launch, or device operation.
Rebuild and re-sign the app through the existing iOS process, then independently
verify the signed Mach-O `__mainx0` section SHA256 before installation.
Actual Story Mode progress remains a separate runtime validation.
