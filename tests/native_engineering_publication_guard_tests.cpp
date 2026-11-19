// Native publication guards must remain effective when no Qt/MODEL runtime is
// available. Generic MODEL kinds 1..17 remain legal without a domain validator;
// reserved hydraulic/annotation kinds require an explicit semantic callback.
#include "aowis/dvmdb/native_store.h"
#include "project_repository.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {
using aowis::dvmdb::CommitMetadata;
using aowis::dvmdb::Key;
using aowis::dvmdb::MergeCandidate;
using aowis::dvmdb::MergeOptions;
using aowis::dvmdb::MergeResult;
using aowis::dvmdb::MergeStatus;
using aowis::dvmdb::ModelChange;
using aowis::dvmdb::ModelRecord;
using aowis::dvmdb::ProjectId;
using aowis::dvmdb::Repository;
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::StorageError;
using aowis::dvmdb::StorageErrorKind;
using aowis::dvmdb::test::commitMetadata;

struct TemporaryDirectory {
    explicit TemporaryDirectory(std::string_view name)
        : path(std::filesystem::temp_directory_path() /
               ("aowis-engineering-guard-" + std::string(name) + "-" +
                std::to_string(::getpid()) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(this->path);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(this->path, error);
    }
    std::filesystem::path path;
};

Key key(std::uint8_t last) {
    Key result{};
    result.back() = last;
    return result;
}

ModelChange record(std::uint8_t id, std::uint16_t kind) {
    return ModelChange{key(id), ModelRecord{kind, {}, {id}}};
}

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void rejected(const std::function<void()>& callback, const char* message) {
    try {
        callback();
    } catch (const StorageError& error) {
        check(error.kind() == StorageErrorKind::InvalidInput, message);
        return;
    }
    throw std::runtime_error(message);
}

const std::function<std::optional<std::string>(const MergeCandidate&)> trustedTestValidator =
    [](const MergeCandidate& candidate) -> std::optional<std::string> {
        // Test-only callback: production callers use full semantic validation.
        // Ensure the preview is callable, not just a truthy callback flag.
        std::size_t observed = 0;
        candidate.forEach([&observed](const Key&, const ModelRecord&) { ++observed; });
        return observed ? std::nullopt : std::optional<std::string>("empty preview");
    };

void kindClassification() {
    for (std::uint16_t kind = 1; kind <= 17; ++kind)
        check(!aowis::dvmdb::isEngineeringRecordKind(kind), "generic kind mistaken for engineering");
    for (std::uint16_t kind = 201; kind <= 217; ++kind)
        check(aowis::dvmdb::isEngineeringRecordKind(kind), "hydraulic kind not reserved");
    for (std::uint16_t kind : {std::uint16_t{100}, std::uint16_t{101},
                              std::uint16_t{110}, std::uint16_t{111}, std::uint16_t{112}})
        check(aowis::dvmdb::isEngineeringRecordKind(kind), "engineering root/chunk kind not reserved");
    check(!aowis::dvmdb::isEngineeringRecordKind(250), "unrelated generic kind reserved");
}

void rootAndMemberGuard() {
    TemporaryDirectory directory("commit");
    const ProjectId project = ProjectId::generate();
    Repository repository(directory.path);
    repository.createUnbornBranch(project, "main");
    const CommitMetadata metadata = commitMetadata();
    const RevisionId generic = repository.commitModel(project, "main", {record(1, 1)}, metadata);
    check(repository.head(project, "main") == generic, "generic Commit rejected");

    for (std::uint16_t kind : {std::uint16_t{100}, std::uint16_t{101},
                              std::uint16_t{112}, std::uint16_t{201}, std::uint16_t{217}}) {
        rejected([&]() {
            (void)repository.commitModel(project, "main", {record(2, kind)}, metadata);
        }, "native Commit accepted a new engineering record without validator");
        check(repository.head(project, "main") == generic,
              "rejected Commit unexpectedly advanced HEAD");
    }

    const RevisionId engineering = repository.commitModel(project, "main", {record(2, 201)},
        metadata, generic, true, std::nullopt, trustedTestValidator);
    rejected([&]() {
        (void)repository.commitModel(project, "main", {record(2, 1)}, metadata);
    }, "changing reserved member into generic kind bypassed native guard");
    rejected([&]() {
        (void)repository.commitModel(project, "main", {{key(2), std::nullopt}}, metadata);
    }, "deleting last engineering member bypassed native guard");
    check(repository.head(project, "main") == engineering, "rejected removal moved HEAD");
}

void mergeGuard() {
    TemporaryDirectory directory("merge");
    const ProjectId project = ProjectId::generate();
    Repository repository(directory.path);
    repository.createUnbornBranch(project, "main");
    const CommitMetadata metadata = commitMetadata();
    const RevisionId base = repository.commitModel(project, "main", {record(1, 1)}, metadata);
    (void)repository.createBranch(project, "feature", "main");
    const RevisionId feature = repository.commitModel(project, "feature", {record(2, 100)},
        metadata, base, true, std::nullopt, trustedTestValidator);
    rejected([&]() {
        (void)repository.mergeBranches(project, "main", "feature", metadata, base, feature);
    }, "native fast-forward published engineering revision without validator");
    check(repository.head(project, "main") == base, "rejected fast-forward moved HEAD");

    MergeOptions invalid;
    invalid.validate = [](const MergeCandidate&) -> std::optional<std::string> {
        return std::string("semantic candidate rejected");
    };
    const MergeResult blocked = repository.mergeBranches(project, "main", "feature",
        metadata, base, feature, invalid);
    check(blocked.status == MergeStatus::Conflicts &&
          repository.head(project, "main") == base,
          "fast-forward published an engineering candidate rejected by validator");

    MergeOptions guarded;
    guarded.validate = trustedTestValidator;
    const MergeResult advanced = repository.mergeBranches(project, "main", "feature",
        metadata, base, feature, guarded);
    check(advanced.status == MergeStatus::FastForward &&
          repository.head(project, "main") == feature, "guarded fast-forward failed");

    // An unrelated generic modification on an engineering snapshot is still
    // engineering publication, including a two-parent merge.
    const RevisionId updated = repository.commitModel(project, "main", {record(3, 1)},
        metadata, feature, true, std::nullopt, trustedTestValidator);
    (void)repository.commitModel(project, "feature", {record(4, 1)}, metadata,
        feature, true, std::nullopt, trustedTestValidator);
    rejected([&]() {
        (void)repository.mergeBranches(project, "main", "feature", metadata);
    }, "native two-parent merge bypassed validator");
    check(repository.head(project, "main") == updated, "rejected two-parent merge moved HEAD");
    const MergeResult merged = repository.mergeBranches(project, "main", "feature",
        metadata, updated, std::nullopt, guarded);
    check(merged.status == MergeStatus::Merged && merged.resultHead.has_value(),
          "guarded two-parent engineering merge failed");
}
} // namespace

int main() {
    try {
        kindClassification();
        rootAndMemberGuard();
        mergeGuard();
        std::cout << "native engineering publication guards: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native engineering publication guards: " << error.what() << '\n';
        return 1;
    }
}
