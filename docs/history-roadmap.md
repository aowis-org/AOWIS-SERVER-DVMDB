# History — remaining acceptance and scope

Native DVMDB already persists original MODEL actions, navigates eligible
active/archived boundaries, preserves or abandons Redo appropriately, and
returns sparse canonical changes with SHA-256 reconciliation. The GUI has a
durable pending navigation intent for restart recovery. See
[History](history-contract.md) and [transport](history-transport.md).

## Live end-to-end qualification

Prove a complete GUI session against an **independent server process**:
create/edit a network, Commit twice, Undo/Redo and jump across the markers,
edit from an archived position, Commit the fork and verify the original
immutable revisions/action archives and correct logical ancestry. Test
structural changes, all 18 hydraulic collections, network-root settings,
selection/renderer refresh and very long paginated branch History.

## Failure injection and recovery

Inject lost navigation replies before server execution, after atomic server
publication and during GUI termination/restart. Kill/restart DVMDB independently;
include writer-lease rotation, stale generation/HEAD, corrupt MODEL frames,
invalid hash or cursor, disk-full and host power cut. Verify the **same
operation UUID**, correctly pinned source, exact canonical model, no
duplicate Undo and no full-model GET during valid sparse restart recovery.
Block on unreconciled identity/position divergence.

## Platform acceptance

Record native Linux, embedded Standalone and WebAssembly/browser separately.
WASM restart requires durable browser-backed client intent storage; a remote
personal working server is not a local offline authority. Stress large
action archives, bounded journal/delta sizes, concurrent users and poor
network latency. Capture actual results instead of treating source code
presence as deployment acceptance.

## Deferred features

Context-aware GUI shortcuts, broader unborn deletion-order provenance and
multi-network action navigation remain separate scope decisions. The
version-control model continues to support multiple stored networks and
checkpoint-only revisions. Whole-project checkout/Fetch/Publish are tracked
in the [primary roadmap](roadmap.md), not as new History stages.
