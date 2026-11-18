#pragma once

// Candidate-wide semantic integrity verification for revisioned map annotations.
// This is a pure, read-only validator. Call it on a complete, pinned native
// model candidate BEFORE publication/import/merge; the draft/merge wiring is
// introduced separately in B3/B4. A record-level loader alone cannot prove
// that no unowned member/chunk, duplicate layer or cross-network anchor exists.
#include "aowis/dvmdb/native_store.h"
#include "aowis/dvmdb/hydraulic_distribution_network_persistence.h"

#include <optional>
#include <string>

namespace aowis::dvmdb::annotations {

// nullopt = valid. Nonempty diagnostic = reject candidate without writing.
// The candidate must enumerate its full native model record set, not only the
// incoming delta. Strict for MODEL A3 + hydraulic manifest v3: unsupported
// native record kinds and missing Distribution layers fail closed. An empty
// project and one optional pre-network Drainage layer are permitted.
std::optional<std::string> validateAnnotationSnapshot(const MergeCandidate& candidate);

// Canonical merge for projects with Distribution networks and independent
// annotation layers. Wraps the existing hydraulic selections and membership
// reconciliation, adding canonical field merges for annotation members and
// full B2 candidate verification for diverged and fast-forward merges.
// B4.2 reconciles concurrent append/remove membership using the pinned
// ancestor, target and source, rebuilding canonical 256-member chunks;
// simultaneous reordering remains an explicit conflict.
MergeResult mergeDistributionAnnotationBranches(
    Repository& repository, const ProjectId& project,
    std::string_view target, std::string_view source,
    const hydraulic::HydraulicMergeSelections& selections,
    const CommitMetadata& metadata);

} // namespace aowis::dvmdb::annotations
