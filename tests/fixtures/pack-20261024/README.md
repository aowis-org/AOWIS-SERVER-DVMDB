# Frozen pre-release pack and bundle specimen (2026-10-24)

Real, immutable bytes emitted by an earlier pre-release build: a pack
repository with SHA-256-identified revisions and name-keyed branch references,
plus four bundles from it (`base`, `feature`, `delta`, `tip`). Do **not**
regenerate or edit them.

DVMDB keeps no compatibility with pre-release formats. The specimen is a
**rejection** fixture: `tests/native_format_compatibility_tests.cpp` checks
the SHA-256 of every file, then requires the current build to refuse the old
repository and each old bundle with an explicit error and without modifying
the destination.

Original creation, for reference (old `Repository` API):

1. `createBranch("main")`, commit keys `10` and `20` (first byte of 16-byte
   keys, remaining bytes zero), values `alpha-v1` and `beta-v1`.
2. Export `base.aowbundle` and create `feature` from `main`.
3. On `feature`, set key `20` to `beta-feature` and key `40` to `feature-new`;
   export `feature.aowbundle`.
4. On `main`, set key `10` to `alpha-v2` and key `30` to `gamma-v1`.
5. On `main`, delete key `20` and set key `30` to `gamma-v2`; export
   `delta.aowbundle` with the first commit as recipient base, then export
   `tip.aowbundle`.
6. Keep `objects.pack` and the two branch references; drop the derived index,
   locks and temporary files.

See `docs/format-compatibility.md`.
