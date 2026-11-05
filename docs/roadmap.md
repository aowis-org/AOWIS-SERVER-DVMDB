# DVMDB — remaining product integration and release work

This is a **forward-looking** roadmap, not a log of earlier patch stages.
Current behavior is in [architecture](architecture.md),
[working copies](working-copies.md), [History](history-contract.md),
[History transport](history-transport.md) and [HTTP workflows](http-workflows.md).

## 1. Whole-project checkout into a personal working DVMDB

Use verified revision bundles and repository tracking primitives to transfer
all required immutable revision ancestry, branches and HEAD relationships
from a browsable source repository into a chosen **personal working** target.
Preserve UUIDv7 identities, expected target state, provenance and MODEL
fingerprint; never overwrite dirty local drafts. The GUI currently designates
one personal server; design transport/session identities so per-project
selection among multiple personal servers is possible later. A personal
server can be embedded or a remote REST server; no code may silently assume
local storage or change target on disconnection.

## 2. Fetch and recoverable Publish

Fetch immutable revisions and update remote-tracking observations without
moving a local personal branch HEAD. Publish must verify complete object
closure, then update the exact expected remote branch HEAD via CAS. Support
first provisioning, divergence detection, durable operation ID, retry/status
lookup and interrupted transport recovery. Never force-overwrite a shared
branch when expected refs disagree.

## 3. Explicit reconciliation and merge UI

Expose pinned base/local/remote versions, native three-way object/field
conflicts, deliberate conflict resolution and correct ordered two-parent
immutable revisions. Preserve unrelated personal working drafts and refuse
unsupported structural merges rather than losing data.

## 4. Complete editor/import and working-copy qualification

End-to-end verify `NetworkHydraulic` canonical bytes for all 18 collections
and root metadata under live GUI HTTP, embedded Standalone, process restart
and lost Save/Commit acknowledgements. Test leases, generation/HEAD fencing,
branch and revision deletion, first untouched-unborn import, large atomic
logical changes and general replacement semantics (not yet implemented).
Hydraulic solver convergence is not a Save/Commit prerequisite; canonical
codec and reference integrity are.

## 5. History and platform acceptance

The History engine, cross-Commit action selection, sparse deltas, idempotent
replay and SHA-256 reconciliation are implemented. GUI-process restart is
addressed by a **client-side durable intent**, not a second DVMDB journal.
Qualify real termination, power loss, ENOSPC, independent server restart,
WASM browser storage and slow/unreliable network scenarios. See the
[History acceptance plan](history-roadmap.md).

## 6. Operations and release evidence

Run real powercut/crash/restore/compaction/backup, concurrent-client, large
history and HTTP-soak acceptance with exact revision IDs, commands and
recorded observations. A source review or mocked Qt regression is not a
substitute for release validation. Security (authentication, TLS,
authorization) is a separate design and deployment workstream; repository
roles and leases are not access control. Pre-release development formats do
not require migration machinery.
