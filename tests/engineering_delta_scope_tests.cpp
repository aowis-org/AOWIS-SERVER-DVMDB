// Qt-free tests for the incremental engineering candidate scope (A1).
//
// The fixture is a synthetic native graph with the real engineering kinds:
// two Distribution networks (root 100, chunks 101, hydraulic members 2xx),
// their annotation layers (root 110, chunks 111, members 112) and anchors
// from annotations to hydraulic nodes. Payloads are opaque here; semantic
// decoding is covered by the Qt annotation integrity suite.
#include "aowis/dvmdb/engineering_delta_scope.h"
#include "aowis/dvmdb/project_service.h"
#include "aowis/dvmdb/storage_error.h"
#include "commit_metadata_fixture.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace db = aowis::dvmdb;

using Records = std::map<db::Key, db::ModelRecord>;

constexpr std::uint16_t junction = 201;
constexpr std::uint16_t reservoir = 202;
constexpr std::uint16_t pipe = 204;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

db::Key key(std::uint32_t ordinal) {
    db::Key value{};
    value[0] = 0x51;
    value[6] = 0x70;
    value[8] = 0x80;
    for (unsigned index = 0; index < 4; ++index)
        value[15 - index] = static_cast<std::uint8_t>((ordinal >> (index * 8U)) & 0xffU);
    return value;
}

db::ModelRecord record(std::uint16_t kind, std::vector<db::Key> references,
                       std::uint8_t payload = 1) {
    std::sort(references.begin(), references.end());
    db::ModelRecord result;
    result.kind = kind;
    result.references = std::move(references);
    result.payload = {payload};
    return result;
}

// Named keys of the fixture.
const db::Key N1 = key(1);    // network root
const db::Key C1 = key(2);    // network chunk
const db::Key J1 = key(3);    // junction (anchor target of A1)
const db::Key J2 = key(4);    // junction
const db::Key P1 = key(5);    // pipe J1-J2
const db::Key L1 = key(6);    // layer root of N1
const db::Key LC1 = key(7);   // layer chunk
const db::Key A1 = key(8);    // annotation anchored to J1
const db::Key A2 = key(9);    // free annotation
const db::Key N2 = key(11);
const db::Key C2 = key(12);
const db::Key J3 = key(13);   // junction of N2 (anchor target of A4)
const db::Key L2 = key(16);
const db::Key LC2 = key(17);
const db::Key A4 = key(19);
const db::Key D1 = key(21);   // drainage layer, no owner network
const db::Key A3 = key(30);   // unused key for insertions
const db::Key R9 = key(31);   // unused key for insertions

Records fixture() {
    Records model;
    model[N1] = record(100, {C1});
    model[C1] = record(101, {J1, J2, P1});
    model[J1] = record(junction, {});
    model[J2] = record(junction, {});
    model[P1] = record(pipe, {J1, J2});
    model[L1] = record(110, {N1, LC1});
    model[LC1] = record(111, {A1, A2});
    model[A1] = record(112, {J1});
    model[A2] = record(112, {});
    model[N2] = record(100, {C2});
    model[C2] = record(101, {J3});
    model[J3] = record(junction, {});
    model[L2] = record(110, {N2, LC2});
    model[LC2] = record(111, {A4});
    model[A4] = record(112, {J3});
    model[D1] = record(110, {});
    return model;
}

std::vector<db::Key> bruteReferrers(const Records& records, const db::Key& target) {
    std::vector<db::Key> result;
    for (const std::pair<const db::Key, db::ModelRecord>& entry : records)
        if (std::find(entry.second.references.begin(), entry.second.references.end(), target) !=
            entry.second.references.end())
            result.push_back(entry.first);
    return result;
}

db::EngineeringCandidateView view(const Records& records) {
    db::EngineeringCandidateView result;
    result.get = [&records](const db::Key& id) -> std::optional<db::ModelRecord> {
        const Records::const_iterator found = records.find(id);
        return found == records.end() ? std::nullopt : std::optional<db::ModelRecord>(found->second);
    };
    result.referrers = [&records](const db::Key& id) { return bruteReferrers(records, id); };
    return result;
}

Records applied(Records records, const std::vector<db::ModelChange>& delta) {
    for (const db::ModelChange& change : delta) {
        if (change.record) records[change.key] = *change.record;
        else records.erase(change.key);
    }
    return records;
}

db::ModelChange put(const db::Key& id, const db::ModelRecord& value) { return db::ModelChange{id, value}; }
db::ModelChange erase(const db::Key& id) { return db::ModelChange{id, std::nullopt}; }

db::EngineeringDeltaScope scopeOf(const Records& before, const std::vector<db::ModelChange>& delta) {
    const Records after = applied(before, delta);
    return db::engineeringDeltaScope(view(before), view(after), delta);
}

std::optional<db::StorageErrorKind> failureOf(const Records& before,
                                              const std::vector<db::ModelChange>& delta) {
    try {
        (void)scopeOf(before, delta);
        return std::nullopt;
    } catch (const db::StorageError& error) {
        return error.kind();
    }
}

void rejected(const Records& before, const std::vector<db::ModelChange>& delta,
              const std::string& message) {
    const std::optional<db::StorageErrorKind> kind = failureOf(before, delta);
    require(kind.has_value() && *kind == db::StorageErrorKind::InvalidInput, message);
}

std::set<db::Key> keys(std::initializer_list<db::Key> values) { return std::set<db::Key>(values); }

// Brute-force graph contract, independent of the incremental implementation.
bool isOwnable(std::uint16_t kind) {
    return kind == 101 || db::isHydraulicMemberKind(kind) || kind == 111 || kind == 112;
}
std::uint16_t ownerOf(std::uint16_t kind) {
    if (kind == 101) return 100;
    if (db::isHydraulicMemberKind(kind)) return 101;
    if (kind == 111) return 110;
    return 111;
}
bool childAllowed(std::uint16_t container, std::uint16_t child) {
    if (container == 100) return child == 101;
    if (container == 101) return db::isHydraulicMemberKind(child);
    if (container == 110) return child == 111 || child == 100;
    if (container == 111) return child == 112;
    return true;
}
bool graphValid(const Records& records) {
    for (const std::pair<const db::Key, db::ModelRecord>& entry : records) {
        if (!db::isEngineeringRecordKind(entry.second.kind)) return false;
        if ((entry.first[6] & 0xf0U) == 0x80U &&
            (entry.second.kind == 100 || db::isHydraulicMemberKind(entry.second.kind) ||
             entry.second.kind == 112))
            return false;
        for (const db::Key& reference : entry.second.references) {
            const Records::const_iterator target = records.find(reference);
            if (target == records.end()) return false;
            if (!childAllowed(entry.second.kind, target->second.kind)) return false;
        }
        if (isOwnable(entry.second.kind)) {
            std::size_t owners = 0;
            for (const db::Key& referrer : bruteReferrers(records, entry.first))
                if (records.at(referrer).kind == ownerOf(entry.second.kind)) ++owners;
            if (owners != 1) return false;
        }
        if (entry.second.kind == 100) {
            std::size_t layers = 0;
            for (const db::Key& referrer : bruteReferrers(records, entry.first))
                if (records.at(referrer).kind == 110) ++layers;
            if (layers != 1) return false;
        }
    }
    return true;
}

void testOverlayView() {
    // Deterministic cases: replace, delete, insert and reference changes.
    const Records base = fixture();
    const std::vector<db::ModelChange> first{put(P1, record(pipe, {J1})), erase(A2),
                                             put(A3, record(112, {J2}))};
    const std::vector<db::ModelChange> second{put(P1, record(pipe, {J2})), put(A2, record(112, {J1}))};
    const Records expected = applied(applied(base, first), second);
    const db::EngineeringCandidateView overlay = db::overlayEngineeringCandidateView(
        view(base).get, view(base).referrers, {first, second});
    std::set<db::Key> every;
    for (const std::pair<const db::Key, db::ModelRecord>& entry : base) every.insert(entry.first);
    for (const std::pair<const db::Key, db::ModelRecord>& entry : expected) every.insert(entry.first);
    every.insert(R9);
    for (const db::Key& id : every) {
        const std::optional<db::ModelRecord> got = overlay.get(id);
        const Records::const_iterator want = expected.find(id);
        require(got.has_value() == (want != expected.end()), "overlay get presence differs");
        if (got) require(got->references == want->second.references && got->payload == want->second.payload,
                         "overlay get content differs");
        require(overlay.referrers(id) == bruteReferrers(expected, id), "overlay referrers differ");
    }
    // Unborn candidate: no base callbacks at all.
    const db::EngineeringCandidateView unborn =
        db::overlayEngineeringCandidateView({}, {}, {{put(N1, record(100, {}))}});
    require(unborn.get(N1).has_value() && !unborn.get(C1).has_value() && unborn.referrers(N1).empty(),
            "unborn overlay view must work without a base");

    // Randomized differential against a materialized map.
    std::mt19937 random(0x5eedU);
    for (int round = 0; round < 300; ++round) {
        Records randomBase;
        const std::uint32_t universe = 12;
        for (std::uint32_t ordinal = 100; ordinal < 100 + universe; ++ordinal) {
            if (random() % 2U == 0U) continue;
            std::vector<db::Key> references;
            for (std::uint32_t target = 100; target < 100 + universe; ++target)
                if (random() % 4U == 0U) references.push_back(key(target));
            randomBase[key(ordinal)] = record(112, references, static_cast<std::uint8_t>(round));
        }
        std::vector<std::vector<db::ModelChange>> overlays(1 + random() % 3U);
        for (std::vector<db::ModelChange>& overlay : overlays) {
            std::set<db::Key> used;
            for (int edit = 0; edit < 5; ++edit) {
                const db::Key id = key(100 + static_cast<std::uint32_t>(random() % universe));
                if (!used.insert(id).second) continue;
                if (random() % 3U == 0U) {
                    overlay.push_back(erase(id));
                } else {
                    std::vector<db::Key> references;
                    for (std::uint32_t target = 100; target < 100 + universe; ++target)
                        if (random() % 4U == 0U) references.push_back(key(target));
                    overlay.push_back(put(id, record(112, references)));
                }
            }
        }
        Records wanted = randomBase;
        for (const std::vector<db::ModelChange>& overlay : overlays) wanted = applied(wanted, overlay);
        const db::EngineeringCandidateView candidate = db::overlayEngineeringCandidateView(
            view(randomBase).get, view(randomBase).referrers, overlays);
        for (std::uint32_t ordinal = 100; ordinal < 100 + universe; ++ordinal) {
            const db::Key id = key(ordinal);
            require(candidate.get(id).has_value() == wanted.contains(id), "random overlay presence differs");
            require(candidate.referrers(id) == bruteReferrers(wanted, id), "random overlay referrers differ");
        }
    }
}

void testScopes() {
    const Records base = fixture();
    require(graphValid(base), "fixture must satisfy the graph contract");
    const db::EngineeringDeltaScope empty = scopeOf(base, {});
    require(!empty.fullValidationRequired && empty.networks.empty() && empty.layers.empty() &&
            empty.annotations.empty(), "empty delta has an empty scope");

    const db::EngineeringDeltaScope text = scopeOf(base, {put(A2, record(112, {}, 7))});
    require(!text.fullValidationRequired && text.networks.empty() && text.layers.empty() &&
            text.annotations == keys({A2}), "annotation-only edit checks only that member");

    const db::EngineeringDeltaScope attach = scopeOf(base, {put(A2, record(112, {J2}))});
    require(attach.annotations == keys({A2}) && attach.networks.empty(),
            "attaching an annotation checks the member, not the network");

    const db::EngineeringDeltaScope node = scopeOf(base, {put(J1, record(junction, {}, 9))});
    require(node.networks == keys({N1}) && node.layers.empty() && node.annotations == keys({A1}),
            "anchor target edit validates its network and re-checks anchored annotations");

    const db::EngineeringDeltaScope link = scopeOf(base, {put(P1, record(pipe, {J1, J2}, 4))});
    require(link.networks == keys({N1}) && link.annotations.empty(), "pipe edit validates its network");

    const db::EngineeringDeltaScope addNote = scopeOf(base, {
        put(A3, record(112, {})), put(LC1, record(111, {A1, A2, A3})), put(L1, record(110, {N1, LC1}, 2))});
    require(addNote.layers == keys({L1}) && addNote.annotations == keys({A3}) &&
            addNote.networks.empty(), "membership change validates the whole layer");

    const db::EngineeringDeltaScope dropChunk = scopeOf(base, {
        erase(A1), erase(A2), erase(LC1), put(L1, record(110, {N1}))});
    require(dropChunk.layers == keys({L1}) && dropChunk.annotations.empty(),
            "deleting the last layer chunk validates the layer");

    const db::EngineeringDeltaScope removeNode = scopeOf(base, {
        erase(J2), put(C1, record(101, {J1, P1})), put(P1, record(pipe, {J1}))});
    require(removeNode.networks == keys({N1}), "hydraulic removal validates its network");

    const db::EngineeringDeltaScope detachAndDelete = scopeOf(base, {
        erase(J1), put(C1, record(101, {J2, P1})), put(P1, record(pipe, {J2})),
        put(A1, record(112, {}))});
    require(detachAndDelete.networks == keys({N1}) && detachAndDelete.annotations == keys({A1}),
            "target deletion with detach validates network and annotation");

    const db::EngineeringDeltaScope move = scopeOf(base, {
        put(C2, record(101, {})), put(C1, record(101, {J1, J2, P1, J3}))});
    require(move.networks == keys({N1, N2}) && move.annotations == keys({A4}),
            "moving a member validates both networks and its anchored annotations");

    const db::EngineeringDeltaScope removeChunk = scopeOf(base, {
        erase(J3), erase(C2), put(N2, record(100, {})), put(A4, record(112, {}))});
    require(removeChunk.networks == keys({N2}), "removing a network chunk validates that network");

    require(scopeOf(base, {put(R9, record(100, {}))}).fullValidationRequired,
            "network root creation requires complete validation");
    require(scopeOf(base, {erase(D1)}).fullValidationRequired,
            "layer root deletion requires complete validation");
    require(scopeOf(base, {put(R9, record(5, {}))}).fullValidationRequired,
            "generic records use the conditional complete validator");
    // A no-op entry is permitted and has no scope.
    const db::EngineeringDeltaScope same = scopeOf(base, {put(J2, base.at(J2))});
    require(same.networks.empty() && same.annotations.empty(), "unchanged record has no scope");
}

void testRejections() {
    const Records base = fixture();
    rejected(base, {erase(J2)}, "deleting a member still listed by its chunk must fail");
    rejected(base, {erase(J2), put(C1, record(101, {J1, P1}))},
             "deleting a node still referenced by a pipe must fail");
    rejected(base, {erase(J1), put(C1, record(101, {J2, P1})), put(P1, record(pipe, {J2}))},
             "deleting an anchor target without detaching must fail");
    rejected(base, {put(C1, record(101, {J1, P1}))}, "dropping a member from its chunk orphans it");
    rejected(base, {put(C1, record(101, {J1, J2, P1, J3}))},
             "a member listed by two chunks has multiple owners");
    rejected(base, {put(A2, record(112, {R9}))}, "a reference to a missing record must fail");
    rejected(base, {put(LC1, record(111, {A1, A2, J2}))},
             "a layer chunk listing a hydraulic member must fail");
    rejected(base, {put(C1, record(101, {J1, J2, P1, A2}))},
             "a network chunk listing an annotation must fail");
    rejected(base, {put(J1, record(reservoir, {}))}, "a record kind cannot change in place");
    rejected(base, {put(A3, record(112, {}))}, "a new annotation without chunk is unowned");
    rejected(base, {put(R9, record(101, {}))}, "a new network chunk without root is unowned");
    rejected(base, {put(L2, record(110, {N1, LC2}))},
             "re-owning a layer gives one network two layers and the other none");
    rejected(base, {put(A2, record(112, {}, 3)), put(A2, record(112, {}, 4))},
             "duplicate delta keys must fail");
    Records twoLayers = base;
    twoLayers[R9] = record(110, {N1});
    rejected(twoLayers, {put(J1, record(junction, {}, 5))},
             "a network in scope must have exactly one layer");

    // The views must reflect the delta: a mismatch is an internal error.
    const std::vector<db::ModelChange> delta{put(A2, record(112, {}, 6))};
    bool internal = false;
    try {
        (void)db::engineeringDeltaScope(view(base), view(base), delta);
    } catch (const db::StorageError& error) {
        internal = error.kind() == db::StorageErrorKind::Internal;
    }
    require(internal, "inconsistent views must raise an internal error");
}

// A7: version-8 UUIDs belong to server derivations (chunks, layer roots).
db::Key version8(std::uint32_t ordinal) {
    db::Key value = key(ordinal);
    value[6] = 0x80;
    return value;
}

void testServerDerivedUuidReservation() {
    const Records base = fixture();
    require(db::isServerDerivedUuidKey(version8(40)) && !db::isServerDerivedUuidKey(key(40)),
            "version 8 detection");
    for (const std::uint16_t kind : {std::uint16_t{100}, junction, pipe, std::uint16_t{112}})
        require(db::isClientIdentifiedEngineeringKind(kind), "client-identified kind");
    for (const std::uint16_t kind : {std::uint16_t{101}, std::uint16_t{110}, std::uint16_t{111}})
        require(!db::isClientIdentifiedEngineeringKind(kind), "server container kind");

    const db::Key node = version8(40);
    rejected(base, {put(node, record(junction, {})), put(C1, record(101, {J1, J2, P1, node}))},
             "a hydraulic member with a version-8 UUID must be rejected");
    const db::Key note = version8(41);
    rejected(base, {put(note, record(112, {})), put(LC1, record(111, {A1, A2, note}))},
             "an annotation with a version-8 UUID must be rejected");
    // Checked before the complete-validation fallback for root creation.
    rejected(base, {put(version8(42), record(100, {}))},
             "a network root with a version-8 UUID must be rejected");
    // Server containers keep their derived version-8 identities.
    const db::Key chunk = version8(43);
    const db::EngineeringDeltaScope derived = scopeOf(base, {
        put(chunk, record(101, {J2})), put(C1, record(101, {J1, P1})),
        put(N1, record(100, {C1, chunk}))});
    require(!derived.fullValidationRequired && derived.networks == keys({N1}),
            "a version-8 network chunk is a valid server-derived container");
    const db::Key layerChunk = version8(44);
    const db::EngineeringDeltaScope layerDerived = scopeOf(base, {
        put(layerChunk, record(111, {A2})), put(LC1, record(111, {A1})),
        put(L1, record(110, {N1, LC1, layerChunk}))});
    require(layerDerived.layers == keys({L1}), "a version-8 layer chunk is a valid container");
}

void testViewErrorsPropagate() {
    const Records base = fixture();
    const Records after = applied(base, {put(J1, record(junction, {}, 8))});
    db::EngineeringCandidateView failing = view(after);
    failing.referrers = [](const db::Key&) -> std::vector<db::Key> {
        throw db::StorageError(db::StorageErrorKind::Io, "simulated read failure");
    };
    bool io = false;
    try {
        (void)db::engineeringDeltaScope(view(base), failing, {put(J1, record(junction, {}, 8))});
    } catch (const db::StorageError& error) {
        io = error.kind() == db::StorageErrorKind::Io;
    }
    require(io, "I/O errors from the candidate views must propagate unchanged");
}

// Random deltas over the valid fixture: whenever the incremental analysis
// does not ask for complete validation, it must accept exactly the deltas
// whose result satisfies the brute-force graph contract.
void testRandomDifferential() {
    const Records base = fixture();
    std::vector<db::Key> universe;
    for (const std::pair<const db::Key, db::ModelRecord>& entry : base) universe.push_back(entry.first);
    universe.push_back(A3);
    universe.push_back(R9);
    universe.push_back(key(32));
    universe.push_back(version8(45)); // valid only for server container kinds
    const std::vector<std::uint16_t> kinds{101, junction, reservoir, pipe, 111, 112};
    std::mt19937 random(0xA1A2U);
    std::size_t compared = 0;
    std::size_t accepted = 0;
    for (int round = 0; round < 20000; ++round) {
        Records target = base;
        const std::size_t operations = 1 + random() % 3U;
        for (std::size_t operation = 0; operation < operations; ++operation) {
            const db::Key id = universe[random() % universe.size()];
            const unsigned choice = static_cast<unsigned>(random() % 5U);
            if (choice == 0U) {
                target.erase(id);
            } else if (choice == 1U && target.contains(id)) {
                target[id].payload = {static_cast<std::uint8_t>(random() % 200U)};
            } else if (choice == 2U && target.contains(id) && !target[id].references.empty()) {
                std::vector<db::Key>& references = target[id].references;
                references.erase(references.begin() +
                                 static_cast<std::ptrdiff_t>(random() % references.size()));
            } else if (choice == 3U && target.contains(id)) {
                const db::Key added = universe[random() % universe.size()];
                std::vector<db::Key>& references = target[id].references;
                if (std::find(references.begin(), references.end(), added) == references.end()) {
                    references.push_back(added);
                    std::sort(references.begin(), references.end());
                }
            } else {
                db::ModelRecord& created = target[id];
                created.kind = kinds[random() % kinds.size()];
                created.references.clear();
                if (random() % 2U == 0U)
                    created.references.assign(1, universe[random() % universe.size()]);
                created.payload.assign(1, static_cast<std::uint8_t>(random() % 200U));
            }
        }
        std::vector<db::ModelChange> delta;
        std::set<db::Key> touched;
        for (const std::pair<const db::Key, db::ModelRecord>& entry : base) touched.insert(entry.first);
        for (const std::pair<const db::Key, db::ModelRecord>& entry : target) touched.insert(entry.first);
        for (const db::Key& id : touched) {
            const Records::const_iterator now = target.find(id);
            const Records::const_iterator old = base.find(id);
            if (now == target.end()) {
                if (old != base.end()) delta.push_back(erase(id));
            } else if (old == base.end() || old->second.kind != now->second.kind ||
                       old->second.references != now->second.references ||
                       old->second.payload != now->second.payload) {
                delta.push_back(put(id, now->second));
            }
        }
        bool incrementalValid = false;
        bool full = false;
        try {
            full = db::engineeringDeltaScope(view(base), view(target), delta).fullValidationRequired;
            incrementalValid = true;
        } catch (const db::StorageError& error) {
            require(error.kind() == db::StorageErrorKind::InvalidInput,
                    std::string("unexpected error kind: ") + error.what());
        }
        if (full) continue;
        // A kind change in place is rejected incrementally; the brute-force
        // snapshot check cannot see it, so exclude it from the comparison.
        bool kindChanged = false;
        for (const db::ModelChange& change : delta) {
            const Records::const_iterator old = base.find(change.key);
            if (change.record && old != base.end() && old->second.kind != change.record->kind)
                kindChanged = true;
        }
        if (kindChanged) {
            require(!incrementalValid, "in-place kind change must be rejected");
            continue;
        }
        const bool expected = graphValid(target);
        if (incrementalValid != expected) {
            std::string description;
            for (const db::ModelChange& change : delta)
                description += " " + std::to_string(change.key[15]) +
                               (change.record ? "=" + std::to_string(change.record->kind) : "-");
            throw std::runtime_error("incremental and complete graph checks disagree (" +
                                     std::string(expected ? "valid" : "invalid") + "):" + description);
        }
        ++compared;
        if (expected) ++accepted;
    }
    require(compared > 5000 && accepted > 200,
            "random differential must exercise both valid and invalid deltas");
}

std::filesystem::path temporaryRoot() {
    std::random_device device;
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("aowis-delta-scope-" + std::to_string(device()));
    std::filesystem::create_directories(root);
    return root;
}

void testPinnedReader() {
    const std::filesystem::path root = temporaryRoot();
    try {
        {
            db::ProjectService service(root / "projects");
            service.configureRepositoryRole(db::RepositoryRole::Private);
            const db::ProjectInfo project = service.createUnbornProject("pinned");
            const std::vector<db::ModelChange> first{put(key(1), record(5, {key(2)})),
                                                     put(key(2), record(5, {}))};
            const db::RevisionId revision = service.commitModelAtExpectedHead(
                project.id, "main", first, db::test::commitMetadata("first"), std::nullopt);
            const db::RevisionId second = service.commitModelAtExpectedHead(
                project.id, "main", {put(key(3), record(5, {key(2)})), erase(key(1))},
                db::test::commitMetadata("second"), revision);
            const db::PinnedModelReader pinned = service.pinModelRevision(project.id, revision);
            const db::PinnedModelReader latest = service.pinModelRevision(project.id, second);
            for (std::uint32_t ordinal = 1; ordinal <= 4; ++ordinal) {
                const db::Key id = key(ordinal);
                const std::optional<db::ModelRecord> direct =
                    service.getModelAtRevision(project.id, revision, id);
                const std::optional<db::ModelRecord> through = pinned.get(id);
                require(direct.has_value() == through.has_value(), "pinned get presence differs");
                if (direct) require(direct->references == through->references &&
                                    direct->payload == through->payload, "pinned get content differs");
            }
            // The first revision stays pinned after the branch moved on.
            require(pinned.get(key(1)).has_value() && !latest.get(key(1)).has_value(),
                    "pinned reads must stay on their revision");
            require(pinned.referrers(key(2)) == std::vector<db::Key>{key(1)},
                    "pinned referrers at the first revision");
            require(latest.referrers(key(2)) == std::vector<db::Key>{key(3)},
                    "pinned referrers at the second revision");
            bool notFound = false;
            try {
                (void)service.pinModelRevision(project.id, db::RevisionId::generate());
            } catch (const db::ProjectError& error) {
                notFound = error.kind() == db::ProjectErrorKind::NotFound;
            }
            require(notFound, "pinning an unknown revision is NotFound");
            bool empty = false;
            try {
                (void)db::PinnedModelReader().get(key(1));
            } catch (const db::ProjectError& error) {
                empty = error.kind() == db::ProjectErrorKind::InvalidInput;
            }
            require(empty, "an empty pinned reader must not be usable");
        }
        std::filesystem::remove_all(root);
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
}

} // namespace

int main() {
    try {
        testOverlayView();
        testScopes();
        testRejections();
        testServerDerivedUuidReservation();
        testViewErrorsPropagate();
        testRandomDifferential();
        testPinnedReader();
        std::cout << "DVMDB engineering delta scope tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DVMDB engineering delta scope: " << error.what() << '\n';
        return 1;
    }
}
