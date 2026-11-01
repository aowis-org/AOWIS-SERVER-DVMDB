#include "commit_metadata_fixture.h"
#include "aowis/dvmdb/native_store.h"

#include <httplib.h>

#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <cstdint>
#include <exception>
#include <iterator>
#include <latch>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <regex>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <fcntl.h>

using aowis::dvmdb::test::commitMetadata;

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct WorkRoot {
    WorkRoot() {
        char pattern[] = "/tmp/aowis-http-transfer-stress-XXXXXXXX";
        char* created = ::mkdtemp(pattern);
        require(created != nullptr, "mkdtemp failed");
        this->path = created;
    }
    ~WorkRoot() {
        if (!this->preserved_) {
            std::error_code error;
            fs::remove_all(this->path, error);
        }
    }
    void preserve() { this->preserved_ = true; }
    fs::path path;
private:
    bool preserved_ = false;
};

unsigned short freePort() {
    int socketFd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(socketFd >= 0, "unable to open port reservation socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    socklen_t size = sizeof(address);
    const bool success = ::bind(socketFd, reinterpret_cast<sockaddr*>(&address), size) == 0 &&
        ::getsockname(socketFd, reinterpret_cast<sockaddr*>(&address), &size) == 0;
    ::close(socketFd);
    require(success, "could not find free loopback port");
    return ntohs(address.sin_port);
}

std::string field(const std::string& json, const std::string& key) {
    const std::regex expression("\\\"" + key + "\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"");
    std::smatch match;
    require(std::regex_search(json, match, expression), "missing JSON field: " + key);
    return match[1].str();
}

httplib::Client clientFor(unsigned short port) {
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(2s);
    client.set_read_timeout(12s);
    client.set_write_timeout(12s);
    return client;
}

struct HttpResponse {
    int status;
    std::string body;
    std::string retry;
};

HttpResponse request(unsigned short port, const std::string& method, const std::string& path,
                     const std::string& body = {}, const std::string& contentType = "application/json") {
    httplib::Client client = clientFor(port);
    httplib::Result reply = method == "GET" ? client.Get(path)
        : client.Post(path, body, contentType);
    require(static_cast<bool>(reply), "request failed " + method + " " + path +
                                  ": " + httplib::to_string(reply.error()));
    return {reply->status, reply->body, reply->get_header_value("Retry-After")};
}

std::string project(unsigned short port, const std::string& name) {
    // The project's first revision records the test author (default message).
    const HttpResponse reply = request(port, "POST", "/dvmdb/v1/projects",
        "{\"name\":\"" + name + "\",\"author\":{\"id\":\"" +
        aowis::dvmdb::test::testAuthorId().toString() + "\",\"name\":\"stress test\"}}");
    require(reply.status == 201, "unable to create project: " + reply.body);
    return field(reply.body, "id");
}

std::string prefix(const std::string& projectId) {
    return "/dvmdb/v1/projects/" + projectId + "/branches/";
}

void health(unsigned short port) {
    const HttpResponse reply = request(port, "GET", "/dvmdb/v1/health");
    require(reply.status == 200, "server health failed: " + reply.body);
}

std::string startupDiagnostics(const fs::path& log) {
    std::ifstream input(log, std::ios::binary);
    if (!input) return " (server log unavailable: " + log.string() + ")";
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    if (length > 8192) input.seekg(length - 8192, std::ios::beg);
    else input.seekg(0, std::ios::beg);
    const std::string tail((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return " (server log " + log.string() + ": " + tail + ")";
}

class ServerProcess {
public:
    ServerProcess(const std::string& binary, const fs::path& root, const fs::path& log,
                  unsigned short port, unsigned inflightMiB) {
        this->pid_ = ::fork();
        require(this->pid_ >= 0, "fork failed");
        if (this->pid_ == 0) {
            const int output = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
            if (output >= 0) {
                ::dup2(output, STDOUT_FILENO);
                ::dup2(output, STDERR_FILENO);
                ::close(output);
            }
            const std::string portText = std::to_string(port);
            const std::string inflightText = std::to_string(inflightMiB);
            const std::string configPath = (root.parent_path() / "stress-server.ini").string();
            // Isolate the test from any developer-machine server configuration.
            // The object ceiling cannot exceed the 4 MiB bundle ceiling.
            ::execl(binary.c_str(), binary.c_str(), "--config", configPath.c_str(),
                    "--bind", "127.0.0.1", "--port", portText.c_str(),
                    "--data-root", root.c_str(), "--worker-threads", "8",
                    "--max-pending-per-project", "3", "--sync-max-mib", "4",
                    "--sync-max-object-mib", "4", "--sync-max-objects", "100000",
                    "--sync-max-inflight-mib", inflightText.c_str(),
                    "--max-internal-scratch-mib", "128", static_cast<char*>(nullptr));
            ::_exit(127);
        }
        try {
            const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + 12s;
            while (std::chrono::steady_clock::now() < deadline) {
                int status = 0;
                const pid_t exited = ::waitpid(this->pid_, &status, WNOHANG);
                if (exited == this->pid_) {
                    this->pid_ = -1; // waitpid has already reaped the child
                    const std::string cause = WIFEXITED(status)
                        ? "exit code " + std::to_string(WEXITSTATUS(status))
                        : WIFSIGNALED(status) ? "signal " + std::to_string(WTERMSIG(status))
                                              : "unknown status";
                    throw std::runtime_error("HTTP server exited during startup (" + cause + ")" +
                                             startupDiagnostics(log));
                }
                require(exited >= 0, "waitpid failed during HTTP server startup");
                try {
                    health(port);
                    return;
                } catch (const std::exception&) {
                    std::this_thread::sleep_for(100ms);
                }
            }
            throw std::runtime_error("HTTP server did not become ready" + startupDiagnostics(log));
        } catch (...) {
            stop();
            throw;
        }
    }
    ~ServerProcess() { stop(); }
    ServerProcess(const ServerProcess&) = delete;
    ServerProcess& operator=(const ServerProcess&) = delete;
    void stop() {
        if (this->pid_ <= 0) return;
        ::kill(this->pid_, SIGTERM);
        const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + 12s;
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            if (::waitpid(this->pid_, &status, WNOHANG) == this->pid_) {
                this->pid_ = -1;
                return;
            }
            std::this_thread::sleep_for(50ms);
        }
        ::kill(this->pid_, SIGKILL);
        ::waitpid(this->pid_, nullptr, 0);
        this->pid_ = -1;
        throwIfUncleanShutdown_ = true;
    }
    bool forcedShutdown() const { return this->throwIfUncleanShutdown_; }
private:
    pid_t pid_ = -1;
    bool throwIfUncleanShutdown_ = false;
};

class RawSocket {
public:
    explicit RawSocket(unsigned short port) {
        this->descriptor_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        require(this->descriptor_ >= 0, "socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(::connect(this->descriptor_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                "raw socket connect failed");
    }
    ~RawSocket() { if (this->descriptor_ >= 0) ::close(this->descriptor_); }
    RawSocket(const RawSocket&) = delete;
    RawSocket& operator=(const RawSocket&) = delete;
    // A server may reject and close a streaming upload before the client has
    // sent its entire declared body. Report this without throwing so the
    // admission-race fixture can try a fresh connection.
    bool trySend(const std::string& content) {
        std::size_t written = 0;
        while (written < content.size()) {
            const ssize_t count = ::send(this->descriptor_, content.data() + written,
                                         content.size() - written, MSG_NOSIGNAL);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
            written += static_cast<std::size_t>(count);
        }
        return true;
    }
    void send(const std::string& content) {
        require(trySend(content), "raw socket write failed");
    }
    void close() {
        if (this->descriptor_ >= 0) ::close(this->descriptor_);
        this->descriptor_ = -1;
    }
    std::string readFirst(std::chrono::milliseconds timeout) {
        timeval tv{};
        tv.tv_sec = static_cast<long>(timeout.count() / 1000);
        tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
        ::setsockopt(this->descriptor_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buffer[8192];
        const ssize_t bytes = ::recv(this->descriptor_, buffer, sizeof(buffer), 0);
        return bytes > 0 ? std::string(buffer, static_cast<std::size_t>(bytes)) : std::string();
    }
private:
    int descriptor_ = -1;
};

std::string makeBundle(const fs::path& root, const std::string& projectText, std::string& expected) {
    using namespace aowis::dvmdb;
    fs::create_directories(root);
    const fs::path sourcePath = root / "source";
    Repository repository(sourcePath);
    const std::optional<ProjectId> project = ProjectId::parse(projectText);
    require(project.has_value(), "invalid project UUID for stress fixture");
    const RevisionId initial = repository.createRootBranch(*project, "main", commitMetadata());
    std::vector<Change> changes;
    changes.reserve(1024);
    for (unsigned index = 0; index < 1024; ++index) {
        Key key{};
        key[0] = static_cast<std::uint8_t>((index >> 8U) & 0xffU);
        key[1] = static_cast<std::uint8_t>(index & 0xffU);
        Bytes value(2048);
        std::uint32_t generator = (index + 1U) * 2654435761U;
        for (std::size_t byte = 0; byte < value.size(); ++byte) {
            generator ^= generator << 13U;
            generator ^= generator >> 17U;
            generator ^= generator << 5U;
            value[byte] = static_cast<std::uint8_t>(generator);
        }
        changes.push_back(Change{key, std::move(value)});
    }
    expected = repository.commit(*project, "main", changes, commitMetadata(), initial).toString();
    const fs::path output = root / "fixture.bundle";
    const int fd = ::open(output.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    require(fd >= 0, "unable to create revision-v2 fixture");
    try {
        repository.exportRevisionBundle(*project, "main", fd);
    } catch (...) {
        ::close(fd);
        throw;
    }
    ::close(fd);
    std::ifstream file(output, std::ios::binary);
    require(static_cast<bool>(file), "fixture bundle missing");
    const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    require(content.size() > 1024 * 1024 && content.size() < 4 * 1024 * 1024,
            "stress fixture is not between 1 and 4 MiB");
    return content;
}

// Every imported local branch must have its own immutable BranchId. The
// project/revision/object identities remain unchanged. In revision-v2, the
// branch ID is a header field, not part of the hashed revision graph.
std::string withFreshBranchId(const std::string& bundle) {
    require(bundle.size() >= 40, "revision-v2 fixture header is truncated");
    std::string result = bundle;
    const aowis::dvmdb::BranchId id = aowis::dvmdb::BranchId::generate();
    const std::array<std::uint8_t, 16>& uuid = id.uuid().bytes;
    for (std::size_t i = 0; i < uuid.size(); ++i)
        result[24 + i] = static_cast<char>(uuid[i]);
    return result;
}

void tightBudget(unsigned short port, const std::string& a, const std::string& b,
                 const std::string& originalHead, const std::string& fixture,
                 const std::string& recipientHead, const std::string& recipientFixture) {
    const std::string aRoot = prefix(a);
    const std::string bRoot = prefix(b);
    {
        bool refused = false;
        unsigned competingExports = 0;
        // The first streaming POST and competing GET are scheduled on distinct
        // worker threads. If the GET grabs the single 4 MiB staging slot first,
        // the server correctly rejects and closes the partial POST. That is a
        // valid admission outcome, not a failed socket implementation: retry
        // the fixture until the POST is the admitted transfer, then assert
        // that the GET receives the expected structured 503 response.
        for (unsigned admissionAttempt = 0; admissionAttempt < 5 && !refused; ++admissionAttempt) {
            RawSocket stalled(port);
            stalled.send("POST " + aRoot + "stalled/bundle HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                         "Content-Type: application/vnd.aowis.revision-bundle-v2\r\nContent-Length: 3000000\r\n\r\n" +
                         fixture.substr(0, 4096));
            // Allow the server's streaming handler to reserve the slot before
            // requesting a competing export; still handle scheduler races.
            std::this_thread::sleep_for(125ms);
            std::size_t sent = 4096;
            const std::chrono::steady_clock::time_point deadline =
                std::chrono::steady_clock::now() + 4s;
            while (std::chrono::steady_clock::now() < deadline) {
                const HttpResponse result = request(port, "GET", bRoot + "main/bundle");
                ++competingExports;
                if (result.status == 503) {
                    require(result.retry == "1" && result.body.find("staging_busy") != std::string::npos,
                            "missing structured global staging rejection");
                    refused = true;
                    break;
                }
                require(result.status == 200, "unexpected status before staging contention");
                // An early close means the POST lost the admission race. The
                // next attempt starts a new partial upload; never complete
                // the declared 3,000,000-byte request body.
                const std::size_t chunk = std::min<std::size_t>(256, fixture.size() - sent);
                require(chunk > 0, "stalled upload fixture exhausted");
                if (!stalled.trySend(fixture.substr(sent, chunk))) break;
                sent += chunk;
                std::this_thread::sleep_for(35ms);
            }
            if (refused) {
                const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
                health(port);
                require(std::chrono::steady_clock::now() - begin < 2s,
                        "health blocked by stalled bundle upload");
            }
        }
        require(refused, "stalled upload did not saturate cross-project transfer budget after " +
                         std::to_string(competingExports) + " competing exports");
    }
    bool recovered = false;
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + 6s;
    while (std::chrono::steady_clock::now() < deadline) {
        const HttpResponse result = request(port, "GET", bRoot + "main/bundle");
        if (result.status == 200) { recovered = true; break; }
        require(result.status == 503, "unexpected status waiting for staging cleanup");
        std::this_thread::sleep_for(40ms);
    }
    require(recovered, "aborted upload retained transfer reservation");
    require(field(request(port, "GET", aRoot + "seed/head").body, "id") == originalHead,
            "aborted upload moved existing head");
    require(request(port, "GET", aRoot + "stalled/head").status == 404,
            "aborted transfer created an incomplete branch");
    const HttpResponse imported = request(port, "POST", bRoot + "from_a/bundle", recipientFixture,
                                           "application/vnd.aowis.revision-bundle-v2");
    require(imported.status == 201 && field(imported.body, "head") == recipientHead,
            "valid import after budget cleanup failed");
    {
        RawSocket download(port);
        download.send("GET " + bRoot + "from_a/bundle HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                      "Connection: close\r\n\r\n");
        const std::string first = download.readFirst(5s);
        require(first.find("200") != std::string::npos, "aborted download did not begin");
    }
    recovered = false;
    const std::chrono::steady_clock::time_point downloadDeadline = std::chrono::steady_clock::now() + 6s;
    while (std::chrono::steady_clock::now() < downloadDeadline) {
        const HttpResponse result = request(port, "GET", bRoot + "from_a/bundle");
        if (result.status == 200) { recovered = true; break; }
        require(result.status == 503, "unexpected status waiting for download cleanup");
        std::this_thread::sleep_for(40ms);
    }
    require(recovered, "aborted download retained transfer reservation");
}

void concurrentLoad(unsigned short port, const std::vector<std::string>& ids,
                    const std::vector<std::string>& heads, const std::vector<std::string>& fixtures) {
    std::atomic<unsigned> imports{0};
    std::atomic<unsigned> downloads{0};
    std::atomic<unsigned> busy{0};
    std::atomic<bool> failed{false};
    std::string failure;
    std::mutex failureMutex;
    std::latch startingLine(6);
    std::vector<std::thread> workers;
    for (unsigned worker = 0; worker < 6; ++worker) {
        workers.emplace_back([&, worker] {
            try {
                const std::size_t projectIndex = worker % ids.size();
                const std::string path = prefix(ids[projectIndex]);
                const std::string& fixture = fixtures[projectIndex];
                const std::string& originalHead = heads[projectIndex];
                startingLine.arrive_and_wait();
                for (unsigned iteration = 0; iteration < 4; ++iteration) {
                    const std::string branch = "stress_" + std::to_string(worker) + "_" +
                                               std::to_string(iteration);
                    const std::string transfer = withFreshBranchId(fixture);
                    bool completed = false;
                    for (unsigned attempt = 0; attempt < 25; ++attempt) {
                        const HttpResponse uploaded = request(port, "POST", path + branch + "/bundle", transfer,
                                                              "application/vnd.aowis.revision-bundle-v2");
                        if (uploaded.status == 503) {
                            require(uploaded.retry == "1", "busy import omitted Retry-After");
                            ++busy;
                            std::this_thread::sleep_for(70ms);
                            continue;
                        }
                        require(uploaded.status == 201 && field(uploaded.body, "head") == originalHead,
                                "concurrent import published wrong head or returned unexpected status");
                        ++imports;
                        completed = true;
                        break;
                    }
                    require(completed, "concurrent import exhausted retries");
                    bool downloaded = false;
                    for (unsigned attempt = 0; attempt < 25; ++attempt) {
                        const HttpResponse exported = request(port, "GET", path + branch + "/bundle");
                        if (exported.status == 503) {
                            require(exported.retry == "1", "busy export omitted Retry-After");
                            ++busy;
                            std::this_thread::sleep_for(70ms);
                            continue;
                        }
                        require(exported.status == 200 && exported.body == transfer,
                                "concurrent export differs from immutable input bundle");
                        ++downloads;
                        downloaded = true;
                        break;
                    }
                    require(downloaded, "concurrent export exhausted retries");
                    require(field(request(port, "GET", path + branch + "/head").body, "id") == originalHead,
                            "concurrent branch head mismatch");
                }
            } catch (const std::exception& error) {
                std::lock_guard<std::mutex> lock(failureMutex);
                if (!failed.exchange(true)) failure = error.what();
            }
        });
    }
    std::exception_ptr probeFailure;
    std::chrono::milliseconds slowestHealth{0};
    try {
        for (unsigned index = 0; index < 30; ++index) {
            const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
            health(port);
            slowestHealth = std::max(slowestHealth,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started));
            std::this_thread::sleep_for(35ms);
        }
    } catch (...) { probeFailure = std::current_exception(); }
    for (std::thread& worker : workers) worker.join();
    if (probeFailure) std::rethrow_exception(probeFailure);
    require(!failed, "concurrent worker failed: " + failure);
    require(slowestHealth < 3000ms, "health endpoint blocked during parallel transfers");
    require(imports == 24 && downloads == 24, "concurrent transfer count mismatch");
    std::cout << "  concurrent transfers: imports=" << imports << " exports=" << downloads
              << " busy=" << busy << " slowest_health_ms=" << slowestHealth.count() << '\n';
}


// Opt-in sustained test. Unlike CTest's short scenario, the *same server*
// remains up for the requested duration. Each new branch is fully checked.
void sustainedLoad(const std::string& executable, unsigned seconds, const fs::path& root,
                   const std::string& unusedFixture, const std::string& unusedHead) {
    (void)unusedFixture;
    (void)unusedHead;
    const fs::path serviceRoot = root / "soak-server";
    const fs::path log = root / "soak-server.log";
    const unsigned short port = freePort();
    std::vector<std::string> projectIds;
    std::array<std::uint64_t, 3> completedByProject{};
    std::vector<std::string> fixtures;
    std::vector<std::string> heads;
    std::atomic<unsigned> completedImports{0};
    std::atomic<unsigned> completedExports{0};
    std::atomic<unsigned> busyResponses{0};
    std::atomic<bool> failed{false};
    std::mutex failureMutex;
    std::string failure;
    unsigned long worstHealthMs = 0;
    {
        ServerProcess server(executable, serviceRoot, log, port, 32);
        for (const char* name : {"soak-alpha", "soak-beta", "soak-gamma"})
            projectIds.push_back(project(port, name));
        for (std::size_t index = 0; index < projectIds.size(); ++index) {
            std::string head;
            fixtures.push_back(makeBundle(root / ("soak-fixture-" + std::to_string(index)),
                                          projectIds[index], head));
            heads.push_back(head);
        }
        const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        const std::chrono::steady_clock::time_point end = started + std::chrono::seconds(seconds);
        // Short smoke runs exercise this exact reporting path without
        // imposing a one-minute delay on CTest; normal soaks log once/minute.
        const unsigned progressIntervalSeconds = seconds < 60U ? 1U : 60U;
        unsigned nextProgressSeconds = progressIntervalSeconds;
        std::latch start(6);
        std::vector<std::thread> clients;
        for (unsigned worker = 0; worker < 6U; ++worker) {
            clients.emplace_back([&, worker]() {
                unsigned iteration = 0;
                const std::size_t projectIndex = worker % projectIds.size();
                const std::string base = prefix(projectIds[projectIndex]);
                const std::string& fixture = fixtures[projectIndex];
                const std::string& expectedHead = heads[projectIndex];
                start.arrive_and_wait();
                try {
                    while (!failed && std::chrono::steady_clock::now() < end) {
                        const std::string branch = "soak_" + std::to_string(worker) + "_" +
                            std::to_string(iteration++);
                        const std::string transfer = withFreshBranchId(fixture);
                        bool inserted = false;
                        for (unsigned attempt = 0; attempt < 100U; ++attempt) {
                            const HttpResponse result = request(port, "POST", base + branch + "/bundle",
                                transfer, "application/vnd.aowis.revision-bundle-v2");
                            if (result.status == 503) {
                                require(result.retry == "1", "soak import missing Retry-After");
                                ++busyResponses;
                                std::this_thread::sleep_for(70ms);
                                continue;
                            }
                            require(result.status == 201 && field(result.body, "head") == expectedHead,
                                    "soak import published incorrect revision: HTTP " +
                                    std::to_string(result.status) + " " + result.body);
                            ++completedImports;
                            inserted = true;
                            break;
                        }
                        require(inserted, "soak import could not complete");
                        bool exported = false;
                        for (unsigned attempt = 0; attempt < 100U; ++attempt) {
                            const HttpResponse result = request(port, "GET", base + branch + "/bundle");
                            if (result.status == 503) {
                                require(result.retry == "1", "soak export missing Retry-After");
                                ++busyResponses;
                                std::this_thread::sleep_for(70ms);
                                continue;
                            }
                            require(result.status == 200 && result.body == transfer,
                                    "soak export bytes differ from immutable source");
                            ++completedExports;
                            exported = true;
                            break;
                        }
                        require(exported, "soak export could not complete");
                        require(field(request(port, "GET", base + branch + "/head").body, "id") ==
                                    expectedHead, "soak branch head changed unexpectedly");
                    }
                } catch (const std::exception& error) {
                    std::lock_guard<std::mutex> lock(failureMutex);
                    if (!failed.exchange(true)) failure = error.what();
                }
            });
        }
        while (!failed && std::chrono::steady_clock::now() < end) {
            const std::chrono::steady_clock::time_point probeStarted = std::chrono::steady_clock::now();
            try {
                health(port);
            } catch (const std::exception& error) {
                std::lock_guard<std::mutex> lock(failureMutex);
                if (!failed.exchange(true)) failure = error.what();
                break;
            }
            const unsigned long delay = static_cast<unsigned long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - probeStarted).count());
            worstHealthMs = std::max(worstHealthMs, delay);
            // Periodic counters reveal whether the growing branch catalogue
            // changes throughput. Recording them does not add HTTP operations.
            // The final completion line remains backward-compatible.
            const unsigned elapsedSeconds = static_cast<unsigned>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - started).count());
            if (elapsedSeconds >= nextProgressSeconds && elapsedSeconds < seconds) {
                // Read completed exports first: imports are published before
                // their corresponding exports, even across six clients.
                const unsigned exported = completedExports.load();
                const unsigned imported = completedImports.load();
                std::cout << "Sustained HTTP soak progress: elapsed_s=" << elapsedSeconds
                          << " imports=" << imported
                          << " exports=" << exported
                          << " admission_503=" << busyResponses.load() << std::endl;
                nextProgressSeconds = (elapsedSeconds / progressIntervalSeconds + 1U) *
                    progressIntervalSeconds;
            }
            std::this_thread::sleep_for(250ms);
        }
        for (std::thread& thread : clients) thread.join();
        require(!failed, "soak client failed: " + failure + startupDiagnostics(log));
        require(worstHealthMs < 3000UL, "health endpoint response exceeded 3s");
        require(completedImports > 0 && completedExports == completedImports,
                "soak did not complete all import/export pairs");
        server.stop();
        require(!server.forcedShutdown(), "soak server needed forced shutdown");
    }
    for (std::size_t index = 0; index < projectIds.size(); ++index) {
        aowis::dvmdb::Repository repository(serviceRoot / "projects" / projectIds[index] / "repository");
        repository.verifyRepository();
        const std::vector<std::string> branches = repository.listBranches();
        for (const std::string& branch : branches) {
            if (branch == "main") continue;
            require(repository.head(*aowis::dvmdb::ProjectId::parse(projectIds[index]), branch).toString() == heads[index],
                    "soak branch head wrong on disk after service stop");
            ++completedByProject[index];
        }
    }
    require(completedByProject[0] + completedByProject[1] + completedByProject[2] ==
                completedImports.load(), "soak branch count differs from successful imports");
    std::cout << "Sustained HTTP soak passed: duration_s=" << seconds
              << " imports=" << completedImports << " exports=" << completedExports
              << " admission_503=" << busyResponses << " worst_health_ms=" << worstHealthMs
              << " projects=3" << std::endl;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 4) {
        std::cerr << "Usage: aowis-dvmdb-http-transfer-stress-tests SERVER_EXECUTABLE|--fixture-only\n"
                     "       aowis-dvmdb-http-transfer-stress-tests --soak-seconds SECONDS SERVER_EXECUTABLE\n";
        return 2;
    }
    const bool soak = argc == 4 && std::string_view(argv[1]) == "--soak-seconds";
    unsigned soakSeconds = 0;
    if (argc == 4) {
        if (!soak) return 2;
        try { soakSeconds = static_cast<unsigned>(std::stoul(argv[2])); }
        catch (const std::exception&) { return 2; }
        if (soakSeconds < 5 || soakSeconds > 86400) return 2;
    }
    WorkRoot root;
    try {
        std::string head;
        const aowis::dvmdb::ProjectId fixtureProject = aowis::dvmdb::ProjectId::generate();
        const std::string fixture = makeBundle(root.path, fixtureProject.toString(), head);
        if (std::string(argv[1]) == "--fixture-only") {
            const fs::path path = root.path / "fixture.bundle";
            aowis::dvmdb::Repository recipient(root.path / "fixture-recipient");
            const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            require(fd >= 0, "cannot read fixture");
            const aowis::dvmdb::RevisionBundleImport result =
                recipient.importRevisionBundle(fixtureProject, fd, "received");
            ::close(fd);
            require(result.head.toString() == head, "fixture import head mismatch");
            require(recipient.verifyRepository().revisions >= 2, "fixture import verification failed");
            std::cout << "HTTP stress fixture verified: " << fixture.size() << " bytes\n";
            return 0;
        }
        if (soak) {
            sustainedLoad(argv[3], soakSeconds, root.path, fixture, head);
            return 0;
        }
        const fs::path serviceRoot = root.path / "server";
        const fs::path log = root.path / "server.log";
        std::vector<std::string> projects;
        std::vector<std::string> emptyHeads;
        std::vector<std::string> projectFixtures;
        std::vector<std::string> projectHeads;
        {
            const unsigned short port = freePort();
            ServerProcess server(argv[1], serviceRoot, log, port, 4);
            projects.push_back(project(port, "alpha"));
            projects.push_back(project(port, "beta"));
            projects.push_back(project(port, "gamma"));
            for (const std::string& id : projects)
                emptyHeads.push_back(field(request(port, "GET", prefix(id) + "main/head").body, "id"));
            for (std::size_t index = 0; index < projects.size(); ++index) {
                std::string projectHead;
                projectFixtures.push_back(makeBundle(root.path / ("project-" + std::to_string(index)),
                                                     projects[index], projectHead));
                projectHeads.push_back(projectHead);
            }
            const HttpResponse imported = request(port, "POST", prefix(projects[0]) + "seed/bundle", projectFixtures[0],
                                                   "application/vnd.aowis.revision-bundle-v2");
            require(imported.status == 201 && field(imported.body, "head") == projectHeads[0],
                    "initial import failed: HTTP " + std::to_string(imported.status) + " " + imported.body);
            tightBudget(port, projects[0], projects[1], projectHeads[0], projectFixtures[0],
                        projectHeads[1], projectFixtures[1]);
            server.stop();
            require(!server.forcedShutdown(), "staging test server did not stop cleanly");
        }
        {
            const unsigned short port = freePort();
            ServerProcess server(argv[1], serviceRoot, log, port, 32);
            concurrentLoad(port, projects, projectHeads, projectFixtures);
            server.stop();
            require(!server.forcedShutdown(), "concurrent test server did not stop cleanly");
        }
        for (std::size_t index = 0; index < projects.size(); ++index) {
            const std::string& id = projects[index];
            aowis::dvmdb::Repository repository(serviceRoot / "projects" / id / "repository");
            require(repository.verifyRepository().revisions >= 2, "post-stress repository verification failed");
            const std::vector<std::string> branches = repository.listBranches();
            require(branches.size() == (index == 2 ? 9U : 10U), "unexpected post-stress branch count");
            for (const std::string& branch : branches) {
                require(repository.head(*aowis::dvmdb::ProjectId::parse(id), branch).toString() ==
                            (branch == "main" ? emptyHeads[index] : projectHeads[index]),
                        "post-stress branch points at an unexpected revision");
            }
        }
        std::cout << "HTTP concurrent transfer stress passed; projects=3, payload="
                  << projectFixtures[0].size() << " bytes\n";
        return 0;
    } catch (const std::exception& error) {
        root.preserve();
        std::cerr << "HTTP concurrent transfer stress FAILED: " << error.what()
                  << " (kept fixtures and server log at " << root.path << ")\n";
        return 1;
    }
}
