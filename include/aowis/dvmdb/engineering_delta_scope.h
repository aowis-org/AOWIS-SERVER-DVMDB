#pragma once

// Incremental validation scope for engineering MODEL candidates (Qt-free).
//
// A working-copy Save changes a few records of a candidate that already
// satisfied the engineering graph contract. Instead of re-validating the whole
// project, this module checks the graph invariants that the delta can break
// (record kinds, exact single ownership, dangling references) using point
// reads and the reverse-reference index, and returns the semantic scope that
// must be re-validated: the touched networks, layers and annotation members.
//
// Precondition: `before` satisfies the engineering contract (every Save,
// import and History publication validates its candidate). The complete
// validator stays authoritative at Commit, merge, bundle import and
// repository verification, so a candidate that was already invalid before the
// delta can never become an immutable revision.
#include "aowis/dvmdb/native_store.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <vector>

namespace aowis::dvmdb {

inline constexpr std::uint16_t engineeringNetworkKind = 100;
inline constexpr std::uint16_t engineeringNetworkChunkKind = 101;
inline constexpr std::uint16_t engineeringLayerKind = 110;
inline constexpr std::uint16_t engineeringLayerChunkKind = 111;
inline constexpr std::uint16_t engineeringAnnotationKind = 112;

// UUID version 8 is reserved for server-derived records: network membership
// chunks, Distribution annotation layer roots and annotation membership
// chunks are SHA-256 derivations with version 8. Client-identified records
// (network roots, hydraulic members, annotations) must use another version,
// so no client UUID can pre-claim a future derived ID and block a network or
// layer from growing (A7).
bool isServerDerivedUuidKey(const Key& key);
bool isClientIdentifiedEngineeringKind(std::uint16_t kind);

// One complete candidate, read by point lookups. `referrers` returns the
// sorted, unique keys of records IN THIS CANDIDATE whose references contain
// the key (the exact reverse-reference index of the candidate).
struct EngineeringCandidateView final {
    std::function<std::optional<ModelRecord>(const Key&)> get;
    std::function<std::vector<Key>(const Key&)> referrers;
};

// A base snapshot with ordered overlays (later overlays win; a nullopt record
// is a deletion). The returned view owns copies of the overlays and keeps the
// base callbacks; the base must outlive the view.
EngineeringCandidateView overlayEngineeringCandidateView(
    std::function<std::optional<ModelRecord>(const Key&)> baseGet,
    std::function<std::vector<Key>(const Key&)> baseReferrers,
    const std::vector<std::vector<ModelChange>>& overlays);

// Referrers of `key` whose record in `view` has exactly `kind`.
std::vector<Key> engineeringReferrersOfKind(const EngineeringCandidateView& view,
                                            const Key& key, std::uint16_t kind);

struct EngineeringDeltaScope final {
    // A root was created or deleted, or a non-engineering record is involved:
    // cardinality rules need the complete candidate. Callers must then run the
    // complete validator; the sets below are not meaningful.
    bool fullValidationRequired = false;
    // Existing network roots whose complete network must be loaded/validated.
    std::set<Key> networks;
    // Existing layer roots whose membership changed: load the whole layer.
    std::set<Key> layers;
    // Existing annotation members to check individually (canonical payload,
    // owner layer, anchor scope). May overlap members of `layers`.
    std::set<Key> annotations;
};

// Checks the graph invariants the delta can break and computes the semantic
// scope. Throws StorageError(InvalidInput) for a violated graph invariant,
// StorageError(Internal) if `after` does not reflect `delta` over `before`.
// Errors from the views (I/O, busy) propagate unchanged.
EngineeringDeltaScope engineeringDeltaScope(const EngineeringCandidateView& before,
                                            const EngineeringCandidateView& after,
                                            const std::vector<ModelChange>& delta);

} // namespace aowis::dvmdb
