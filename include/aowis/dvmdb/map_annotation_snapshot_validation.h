#pragma once

// Candidate-wide semantic integrity verification for revisioned map annotations.
// This is a pure, read-only validator. Call it on a complete, pinned native
// model candidate BEFORE publication/import/merge; the draft/merge wiring is
// introduced separately in B3/B4. A record-level loader alone cannot prove
// that no unowned member/chunk, duplicate layer or cross-network anchor exists.
#include "aowis/dvmdb/native_store.h"

#include <optional>
#include <string>

namespace aowis::dvmdb::annotations {

// nullopt = valid. Nonempty diagnostic = reject candidate without writing.
// The candidate must enumerate its full native model record set, not only the
// incoming delta. Strict for MODEL A3 + hydraulic manifest v3: unsupported
// native record kinds and missing Distribution layers fail closed. An empty
// project and one optional pre-network Drainage layer are permitted.
std::optional<std::string> validateAnnotationSnapshot(const MergeCandidate& candidate);

} // namespace aowis::dvmdb::annotations
