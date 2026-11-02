# GUI integration requirements

What the AOWIS GUI may rely on from DVMDB and how it must present history and
drafts. The rules come from the [version-control contract](version-control-semantics.md);
the routes are in the [REST guide](rest-api-design.md) and the
[OpenAPI contract](dvmdb-http-api.yaml).

## Required domain model

Native and REST expose project, branch, revision and private working-copy
UUIDv7 identities. SHA-256 record and snapshot hashes remain **separate
integrity fields**. A branch record has a nullable committed HEAD and
immutable `originBranchId` / `forkRevisionId`; it is not a commit vertex.
A new project creates `main` with no committed HEAD or fabricated initial
revision. It can also create multiple independent unborn branches. Working
copies are independent mutable session state; the GUI HEAD **card** is the
writable workflow tip, not an extra immutable revision.

## Personal working repository and shared source roles

Standalone uses its embedded DVMDB as the personal repo; basic and web GUI
require a separately configured private HTTP DVMDB for user drafts and local
commits. Shared sources remain separate. See
[private working repositories](private-working-repositories.md).

## Required operations

1. **Catalogue:** list sources/projects/branches, including unborn branches,
   stable UUIDv7s, names, current committed HEADs and immutable provenance.
2. **History:** fetch a pinned, paginated, complete project DAG with all
   branch HEADs, zero/one/two committed parents and SHA-256 bindings; reject
   invalid graph closure or changing pinned HEADs.
3. **Checkout:** make an owned private working copy based on an exact
   committed revision UUIDv7, or null for an unborn first checkout.
4. **Edit:** change only that working copy, retaining dirty state and
   revision base across UI navigation or reconnection.
5. **Commit locally:** validate and create a committed revision in the
   user's personal DVMDB with an expected **local** HEAD/null and an
   idempotency key, without advancing the shared branch.
6. **Publish:** transfer missing immutable objects, then conditionally
   advance the shared branch from exact expected **remote** HEAD/null.
   Conflict retains local commits/drafts; fetch/reconcile explicitly.
7. **Branch from revision:** accept the exact immutable source revision UUID,
   optional source branch UUID, store both creation-origin identities; do not
   create a revision just to name the branch. Uncommitted edits require an
   explicit commit-first or exclude-edits choice.
8. **Merge:** use expected source/target committed HEADs, both ordered parent
   IDs for a divergent merge and hydraulic-aware validation; no silent reset
   of private working state.

### What exists over HTTP today

| Operation | Status |
|---|---|
| Catalogue, branches, nullable HEADs, provenance | available |
| Pinned paged revision DAG, revision records with author, message and commit time | available |
| Every write that creates a revision sends `author` (UUIDv7 + name) and `message` | required |
| Checkout: draft at a HEAD or null; unborn-branch and project provisioning | available |
| Edit: leased, fenced event Saves with server-side membership; draft network hydration | available |
| Saved draft and pinned committed network summaries | available |
| First import into untouched private unborn draft | available; replacement/merge not supported |
| Revision display-message annotation without rewriting hashed commit | available |
| Branch from revision or create independent unborn branch | available, with identity-based name/tags/description updates |
| Generic and hydraulic merges with explicit choices | available |
| Local Commit and its status | native and HTTP routes available |
| Fetch, Publish | not built |

HTTP must distinguish invalid IDs, missing objects, forbidden operations,
validation failure and **409 stale HEAD**; a missing expected HEAD is not an
explicit `null` expected HEAD. Uncertain write results require a safe
idempotency query rather than blind automatic retry. The native service must
provide equivalent behavior.

## Graph and inspector invariants

- The GUI graph displays server/project cards, immutable revision cards and
  **separate writable HEAD cards** labeled with their branches. HEAD cards
  represent working states, not extra immutable revision records. Revision
  ancestry edges join only real committed revisions; HEAD-to-base and
  project-to-unborn-HEAD links are organizational.
- Shared immutable revisions remain a **single logical UUIDv7 object**.
- The branch inspector shows `originBranchId` and `forkRevisionId` without
  a second provenance edge in the revision history graph.
- A private working copy is represented by a writable HEAD card, not a
  committed revision vertex. Its pinned base may differ from current branch HEAD.
- A project with no commits displays `main` and `No commits yet`.
- UI selection/filtering/camera/inspector changes must not mutate refs,
  create revisions or discard uncommitted work.

## Validation matrix

- Empty project, first commit and independent branch creation.
- Separate personal working repos, local commits, stale remote Publish
  rejection without losing committed or draft content, and reconciliation.
- Two divergent commits and two-parent merge.
- Shared and non-leaf branch HEADs.
- Branch rename and provenance surviving restart/replication.
- Invalid hashes, unbounded/partial graph results, offline read-only states.
- Native versus REST agreement on UUIDs, nullable HEADs, integrity hashes
  and 409 semantics.

The acceptance target is this contract, not a particular version of a
prototype GUI or backend route.
