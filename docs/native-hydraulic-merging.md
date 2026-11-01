# Hydraulic-aware three-way branch merges

`aowis::dvmdb::hydraulic::mergeHydraulicBranches(repository, project, target, source, selections)`
(HTTP: `POST /dvmdb/v1/projects/{id}/hydraulic-merges`) builds
on the native two-parent revision/merge API while validating the combined
hydraulic model **before any bytes are appended or the target branch ref moves**.
The operation returns `MergeStatus::Conflicts` with diagnostics on rejected
merges. It never changes the source branch.

For independently modified fields of the **same** canonical hydraulic entity,
all three historical model records are decoded and compared field by field. A
field changed on only one side is taken from that side. Identical changes on
both sides are accepted. Different changes to the same field are reported with
field names. Nested structs/lists remain atomic merge fields, except the
canonical seven-field metadata merge already defined by `ModelCodec`.

## Explicit conflict resolution

In the examples, `namespace hydraulic = aowis::dvmdb::hydraulic;`,
`project` is the selected project UUID, and `targetHead` / `sourceHead`
are pinned during conflict preview, not refreshed after the user chooses.

The mandatory fifth argument, `HydraulicMergeSelections`, allows the AOWIS UI to
resolve individual fields after previewing a conflict:

```cpp
hydraulic::HydraulicMergeSelections selections{targetHead, sourceHead, {}, {}};
selections.fields.push_back({pipeUuid, QStringLiteral("diameter_mm"),
    hydraulic::HydraulicMergeSide::Source});
const aowis::dvmdb::MergeResult result =
    hydraulic::mergeHydraulicBranches(repository, project, "main", "feature", selections);
```

Each selection identifies a canonical entity UUID, an **exact conflicting field
name** and whether to take the target or source branch's value. Unspecified
non-conflicting edits still merge automatically; unspecified conflicting fields
are reported, not guessed. A conflict on `metadata.description` can be selected
independently from other metadata fields.

**Both previewed branch head revision UUIDs are always required**, with or without choices.
The native repository checks them inside the same lock as publication. If either
branch has advanced, the operation throws without modifying the repository;
re-preview and obtain new conflict diagnostics before choosing again. Duplicate,
unknown, non-conflicting, or unconsumed selections also throw before any
pack objects are written, including attempts to replay selections after a
successful merge or to pass selections to a fast-forward merge.

### Explicit whole-object / structural choices

For a genuine concurrent change to the same UUID, including deletion versus
modification and incompatible topology rewires, AOWIS may instead choose one
**complete object** from either branch:

```cpp
hydraulic::HydraulicMergeSelections choices{targetHead, sourceHead, {}, {}};
choices.objects.push_back({pipeUuid, hydraulic::HydraulicMergeSide::Source});
const aowis::dvmdb::MergeResult merged =
    hydraulic::mergeHydraulicBranches(repository, project, "main", "feature", choices);
```

An object choice deliberately discards the other side's changes to *that one
UUID*, but does not select a whole branch: changes to other UUIDs are still
merged. It never applies automatically. Whole-object and field choices for the
same UUID are mutually exclusive. Duplicate, stale, irrelevant and unused
choices are rejected without writing objects.

Choosing a deleted side means deletion. When a user explicitly selects a
surviving entity that the other branch removed from membership, the merge now
finds the **selected side's historical network/collection ownership** and
restores *only that chosen entity* to a bounded membership chunk. Other objects
removed on either branch are not resurrected. This also works with multiple
chosen entities in the same merge. Ambiguous historical ownership, invalid
references or an invalid hydraulic network cause a conflict **before pack
writes**; a stale branch head cannot accept an old selection.

Concurrent independent appends/deletions within the same membership chunk are
merged using three-way UUID membership. When both branches create the same
initial chunk UUID from a deterministic serial, their distinct member UUIDs
are combined rather than dropping either branch. Network-root membership
changes are reconciled similarly, retaining stable original order and
appending new entries in target-then-source order. **Reordering existing
members and inserting in the middle are not automatic merges**: such changes
remain conflicts. A resulting chunk exceeding 256 entries is also a conflict,
not silently truncated. Complex overlapping chunk deletions may still require
manual structural resolution.

Field selections still only resolve true same-field conflicts of existing
entities with unchanged kinds and outgoing UUID-reference sets. Structural
selections are whole-record decisions; they do not merge different fields.

## Publication safety and limitations

Before publishing a divergent merge, the callback traverses the proposed
copy-on-write model snapshot without invoking repository APIs, reconstructs
every hydraulic network from its chunks, checks identities and canonical model
bytes, detects cross-network ownership collisions and orphaned records, revalidates network rules
(including pipe-assigned demand endpoints), and compares stored reference edges
against canonically extracted model dependencies. Any failure blocks publication.

The plain `Repository::mergeBranches()` remains a Qt-free generic object merge
with no **implicit** hydraulic validation. AOWIS should use the hydraulic-specific
API for branches containing hydraulic networks. **Fast-forwards validate the
complete immutable source snapshot before publishing the target reference**;
an invalid snapshot returns `Conflicts` and leaves both heads and the pack
unchanged. A source snapshot that is not model-indexed is rejected when a
validator is supplied. **Up-to-date** results (equal heads or a source already
incorporated in target history) are no-ops and deliberately do not revalidate
the existing target. Explicit pinned heads and unused-selection checks still
run before either result. This API does not implement recursive merge-base
synthesis, unconstrained multi-object topology reconciliation, or solver
feasibility validation.
Candidate-wide semantic validation still takes O(network size) and is not an
indexed edit operation.
