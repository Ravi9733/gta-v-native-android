# GTA iPhone shader supplements

`prepare_gta_shader_fill.py` prepares a local, verified supplement. It never connects
to a device and never updates a Mac runtime cache or the upstream shared shader list.
Its inputs are copied iPhone request JSON, a hash-manifested complete source seed,
and the currently packaged seed manifest.

The helper translates requests anew with this project's pinned translator and
`DMSH_CHAIN`, verifies that the resulting dependencies serve the original requests
under this GTA renderer's matcher, restores variants absent from the baseline, and
compiles **every selected library anew** with `compile_seed_gta.METAL_FLAGS`
(`-fmetal-math-mode=safe -fpreserve-invariance`). It does not reuse old libraries.
It checks restored MSL/reflection hashes and index dependencies, device library
targets, complete triples, normalized indexes, and a configurable size budget.
The final `payload` folder appears only after those checks; `seed-manifest.json`
hashes every runtime file. The default assertions are 176 requests and 1563 restored
variants; change the explicit `--expect-*` arguments for a different inventory.

Example (local paths deliberately supplied by the caller):

```sh
python3 ios/prepare_gta_shader_fill.py \
  --tree /path/to/GTA_build --requests /path/to/copied/requests \
  --restore-from /path/to/complete/source-seed \
  --base-manifest /path/to/current/seed-manifest.json \
  --out /path/to/fresh/private/output --workers 4
```

Before a device transfer, exercise the actual `totk_seed_import_into` function from
`totk_shader_seed.h` with a nil marker in a private local cache. A valid pack must
import, a different chain must fail identity validation, and a deliberately corrupt
file must fail integrity validation before modifying the cache. Check all non-index
file hashes after import and compare index rows semantically (the importer rewrites
JSON formatting). Do not relax the runtime validator to accommodate another game's
translation chain.

`pack_seed_gta.py` preserves the supported compiler flag attestation when packing a
seed. Legacy safe-only seeds remain accepted; malformed prefix lookalikes,
duplicated flags, and unsupported compiler flags fail validation.

A successful compile/import does not establish correct lighting. After the normal
app start, confirm the fill merge receipt, gather fresh requests while repeating
the affected camera path, compare shader miss/skipped-draw evidence, and review the
actual output. A supplement may resolve the known requests while new paths require
additional variants.
