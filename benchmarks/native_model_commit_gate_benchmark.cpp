#include "aowis/dvmdb/native_store.h"
#include "aowis/dvmdb/object_id.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__linux__)
#include <sys/resource.h>
#endif

namespace {
using namespace aowis::dvmdb;
using Clock = std::chrono::steady_clock;

struct TemporaryRepository final {
    std::filesystem::path path;
    explicit TemporaryRepository(const std::string& prefix)
        : path(std::filesystem::temp_directory_path() /
               (prefix + "-" + UuidV7::generate().toString())) {}
    ~TemporaryRepository() {
        std::error_code error;
        std::filesystem::remove_all(this->path, error);
    }
};

Key keyFor(std::uint64_t number) {
    Key result{};
    for (std::size_t index = 0; index < 8; ++index) {
        result[15 - index] = static_cast<std::uint8_t>(number & 0xffU);
        number >>= 8U;
    }
    return result;
}

ModelRecord genericRecord(std::uint64_t serial) {
    ModelRecord result;
    result.kind = 31;
    result.payload.resize(16);
    for (std::size_t index = 0; index < result.payload.size(); ++index)
        result.payload[index] = static_cast<std::uint8_t>((serial + index) & 0xffU);
    return result;
}

CommitMetadata metadata() {
    CommitMetadata result;
    result.authorId = *UuidV7::parse("018f84c0-0000-7000-8000-00000000a002");
    result.authorName = "DVMDB benchmark";
    result.message = "Generic model commit gate benchmark";
    return result;
}

struct Measurement final {
    std::size_t records = 0;
    double medianMs = 0.0;
    double maximumMs = 0.0;
};

Measurement run(std::size_t records, std::size_t updates) {
    TemporaryRepository temporary("aowis-model-gate");
    Repository repository(temporary.path);
    const ProjectId project = ProjectId::generate();
    repository.createUnbornBranch(project, "main");
    const CommitMetadata commitMetadata = metadata();
    for (std::size_t start = 0; start < records; start += 128) {
        std::vector<ModelChange> changes;
        const std::size_t end = std::min(records, start + 128);
        changes.reserve(end - start);
        for (std::size_t index = start; index < end; ++index)
            changes.push_back(ModelChange{keyFor(index + 1), genericRecord(index)});
        (void)repository.commitModel(project, "main", std::move(changes), commitMetadata);
    }
    std::vector<double> elapsed;
    elapsed.reserve(updates);
    for (std::size_t iteration = 0; iteration < updates; ++iteration) {
        const Clock::time_point start = Clock::now();
        (void)repository.commitModel(project, "main",
                {ModelChange{keyFor(1), genericRecord(iteration + 1000000)}}, commitMetadata);
        const Clock::time_point finish = Clock::now();
        elapsed.push_back(std::chrono::duration<double, std::milli>(finish - start).count());
    }
    const std::optional<RevisionId> head = repository.optionalHead(project, "main");
    if (!head || !repository.getModelAtRevision(project, *head, keyFor(records)))
        throw std::runtime_error("benchmark lost generic MODEL data");
    std::sort(elapsed.begin(), elapsed.end());
    Measurement measurement;
    measurement.records = records;
    measurement.medianMs = elapsed[elapsed.size() / 2];
    measurement.maximumMs = elapsed.back();
    return measurement;
}
}

int main(int argc, char** argv) {
    try {
        const std::size_t small = argc > 1 ? std::stoull(argv[1]) : 1024;
        const std::size_t large = argc > 2 ? std::stoull(argv[2]) : 16384;
        const std::size_t updates = argc > 3 ? std::stoull(argv[3]) : 12;
        if (small == 0 || large <= small || updates == 0 || large > 1000000)
            throw std::invalid_argument("expected: small > 0, large > small, 1 <= updates, large <= 1000000");
        const Measurement first = run(small, updates);
        const Measurement second = run(large, updates);
        std::cout << std::fixed << std::setprecision(3)
                  << "records,median_single_record_commit_ms,max_single_record_commit_ms\n"
                  << first.records << ',' << first.medianMs << ',' << first.maximumMs << '\n'
                  << second.records << ',' << second.medianMs << ',' << second.maximumMs << '\n';
        if (first.medianMs > 0.0)
            std::cout << "median_large_over_small_ratio," <<
                         second.medianMs / first.medianMs << '\n';
#if defined(__linux__)
        struct rusage usage{};
        if (::getrusage(RUSAGE_SELF, &usage) == 0)
            std::cout << "peak_process_rss_kib," << usage.ru_maxrss << '\n';
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "model commit gate benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
