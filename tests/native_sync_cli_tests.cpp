#include "commit_metadata_fixture.h"
#include "aowis/dvmdb/native_store.h"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

using aowis::dvmdb::test::commitMetadata;

namespace {

using aowis::dvmdb::Bytes;
using aowis::dvmdb::Change;
using aowis::dvmdb::Repository;

void expect(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

struct TemporaryDirectory {
    TemporaryDirectory() {
        std::string name = (std::filesystem::temp_directory_path() /
                            "aowis-dvmdb-sync-cli-XXXXXXXX").string();
        std::vector<char> buffer(name.begin(), name.end());
        buffer.push_back('\0');
        const char* created = ::mkdtemp(buffer.data());
        expect(created != nullptr, "cannot create test directory");
        this->path = created;
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(this->path, error);
    }
    std::filesystem::path path;
};

int run(const std::filesystem::path& cli, const std::vector<std::string>& arguments) {
    std::vector<std::string> values{cli.string()};
    values.insert(values.end(), arguments.begin(), arguments.end());
    std::vector<char*> raw;
    for (std::string& value : values) raw.push_back(value.data());
    raw.push_back(nullptr);
    const pid_t child = ::fork();
    expect(child >= 0, "cannot fork synchronization CLI");
    if (child == 0) {
        ::execv(cli.c_str(), raw.data());
        ::_exit(127);
    }
    int status = 0;
    pid_t waited = -1;
    do { waited = ::waitpid(child, &status, 0); }
    while (waited < 0 && errno == EINTR);
    expect(waited == child && WIFEXITED(status), "sync CLI exited abnormally");
    return WEXITSTATUS(status);
}

void exerciseRevision(const std::filesystem::path& cli) {
    using aowis::dvmdb::ProjectId;
    using aowis::dvmdb::RevisionId;
    TemporaryDirectory temp;
    const std::filesystem::path source = temp.path / "source-v2";
    const std::filesystem::path destination = temp.path / "replica-v2";
    const std::filesystem::path full = temp.path / "revision-full.aowbundle";
    const std::filesystem::path delta = temp.path / "revision-incremental.aowbundle";
    const ProjectId project = ProjectId::generate();
    aowis::dvmdb::Key key{};
    key[15] = 99;
    RevisionId base = RevisionId::generate();
    RevisionId tip = RevisionId::generate();
    std::optional<aowis::dvmdb::BranchId> identity;
    {
        Repository repository(source);
        (void)repository.createRootBranch(project, "main", commitMetadata());
        base = repository.head(project, "main");
        identity = repository.branchId("main");
    }
    expect(run(cli, {"export", source.string(), project.toString(), "main", full.string()}) == 0,
           "full CLI export failed");
    {
        std::ifstream file(full, std::ios::binary);
        char header[8]{};
        file.read(header, sizeof(header));
        expect(file && std::string(header, sizeof(header)) == "AOWREV21",
               "CLI full transfer used the wrong wire format");
    }
    expect(run(cli, {"export", source.string(), project.toString(), "main", full.string()}) == 1,
           "CLI export overwrote existing bundle");
    expect(run(cli, {"import", destination.string(), project.toString(), full.string(), "incoming"}) == 0,
           "full CLI import failed");
    expect(run(cli, {"import", destination.string(), project.toString(), full.string(), "incoming"}) == 0,
           "full CLI replay failed");
    expect(run(cli, {"head", destination.string(), project.toString(), "incoming"}) == 0,
           "HEAD CLI lookup failed");
    {
        Repository repository(destination);
        expect(repository.head(project, "incoming") == base &&
               repository.branchId("incoming") == identity,
               "CLI round trip lost revision or branch identity");
    }
    {
        Repository repository(source);
        tip = repository.commit(project, "main", {{key, Bytes{7, 8, 9}}}, commitMetadata(), base);
    }
    expect(run(cli, {"export-incremental", source.string(), project.toString(), "main",
                     base.toString(), delta.string()}) == 0,
           "incremental CLI export failed");
    {
        std::ifstream file(delta, std::ios::binary);
        char header[8]{};
        file.read(header, sizeof(header));
        expect(file && std::string(header, sizeof(header)) == "AOWREV22",
               "CLI delta transfer used the wrong wire format");
    }
    expect(run(cli, {"import", destination.string(), project.toString(), delta.string(), "incoming"}) == 0,
           "incremental CLI import failed");
    {
        Repository repository(destination);
        expect(repository.head(project, "incoming") == tip &&
               repository.branchId("incoming") == identity,
               "incremental CLI import lost revision or branch identity");
    }
    expect(run(cli, {"import", destination.string(), ProjectId::generate().toString(),
                     full.string(), "other"}) == 1,
           "cross-project CLI import was accepted");
    expect(run(cli, {"export-incremental", source.string(), project.toString(), "main",
                     "invalid-uuid", (temp.path / "bad.aowbundle").string()}) == 1,
           "invalid UUIDv7 base was accepted");
    expect(!std::filesystem::exists(temp.path / "bad.aowbundle"),
           "invalid input published an output bundle");
    expect(run(cli, {"export-v2", source.string(), project.toString(),
                     "main", (temp.path / "obsolete.aowbundle").string()}) == 2,
           "old -v2 CLI alias was accepted");
    expect(run(cli, {"manifest-export", source.string(), "main",
                     (temp.path / "obsolete.manifest").string()}) == 2,
           "removed SHA identity-manifest command was accepted");
    expect(run(cli, {"head", source.string(), "main"}) == 2,
           "project-less SHA head command was accepted");
    expect(run(cli, {"head", source.string(), "not-a-uuid", "main"}) == 1,
           "invalid project UUIDv7 was accepted");
    expect(run(cli, {"branch-id", source.string(), project.toString(), "main"}) == 0,
           "canonical branch identity CLI lookup failed");
}

} // namespace

int main(int argc, char** argv) {
    try {
        expect(argc == 2, "usage: native_sync_cli_tests SYNC_CLI");
        const std::filesystem::path cli = std::filesystem::absolute(argv[1]);
        exerciseRevision(cli);
        std::cout << "Offline synchronization CLI tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Offline synchronization CLI tests failed: " << error.what() << '\n';
        return 1;
    }
}
