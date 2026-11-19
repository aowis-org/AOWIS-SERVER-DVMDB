#include "aowis_model_codec_fingerprint.h"
#include "commit_metadata_fixture.h"
#include "model_edit_event_fixture.h"
#include "aowis/dvmdb/dvmdb_http_routes.h"
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
#include "aowis/dvmdb/hydraulic_distribution_network_persistence.h"
#include "aowis/dvmdb/map_annotation_persistence.h"
#include "aowis/dvmdb/map_annotation_action_replay.h"
#include "aowis/dvmdb/map_annotation_snapshot_validation.h"
#include "aowis/dvmdb/model_record_conversion.h"
#include <aowis/model/edit/model_edit_transaction.h>
#include <aowis/model/edit/hydraulic_distribution_root_edit.h>
#endif

#include <httplib.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStringList>
#include <QUuid>

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <set>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>

using aowis::dvmdb::test::commitMetadata;

namespace {
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

class TemporaryRoot {
public:
    TemporaryRoot() {
        char name[] = "/tmp/aowis-dvmdb-http-XXXXXX";
        char* result = ::mkdtemp(name);
        check(result != nullptr, "mkdtemp failed");
        this->root_ = result;
    }
    ~TemporaryRoot() { std::error_code error; std::filesystem::remove_all(this->root_, error); }
    const std::filesystem::path& root() const { return this->root_; }
private:
    std::filesystem::path root_;
};

struct Response {
    int status = 0;
    QJsonObject json;
    QByteArray retryAfter;
};
struct BinaryResponse {
    int status = 0;
    QByteArray body;
    QByteArray contentType;
    QByteArray retryAfter;
};

// Raw TCP connection to 127.0.0.1 for protocol-level tests (partial bodies,
// declared lengths without bodies, slow clients). Closes on destruction.
class RawConnection {
public:
    explicit RawConnection(quint16 port) {
        this->fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        check(this->fd_ >= 0, "socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        this->connected_ = ::connect(this->fd_, reinterpret_cast<sockaddr*>(&address),
                                     sizeof(address)) == 0;
    }
    ~RawConnection() { if (this->fd_ >= 0) ::close(this->fd_); }
    RawConnection(const RawConnection&) = delete;
    RawConnection& operator=(const RawConnection&) = delete;
    bool connected() const { return this->connected_; }
    void send(const QByteArray& bytes) {
        qsizetype done = 0;
        while (done < bytes.size()) {
            const ssize_t sent = ::send(this->fd_, bytes.constData() + done,
                                        static_cast<std::size_t>(bytes.size() - done), MSG_NOSIGNAL);
            check(sent > 0, "raw send failed");
            done += sent;
        }
    }
    // Everything the server sends until it closes the connection or the
    // timeout expires (whichever comes first).
    QByteArray receive(int timeoutMs) {
        QByteArray received;
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            const long remaining = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count());
            if (remaining <= 0) break;
            pollfd descriptor{this->fd_, POLLIN, 0};
            if (::poll(&descriptor, 1, static_cast<int>(remaining)) <= 0) break;
            char buffer[4096];
            const ssize_t count = ::recv(this->fd_, buffer, sizeof(buffer), 0);
            if (count <= 0) { this->closedByPeer_ = true; break; }
            received.append(buffer, count);
        }
        return received;
    }
    bool closedByPeer() const { return this->closedByPeer_; }
    void close() {
        if (this->fd_ >= 0) ::close(this->fd_);
        this->fd_ = -1;
    }
private:
    int fd_ = -1;
    bool connected_ = false;
    bool closedByPeer_ = false;
};

int statusOf(const QByteArray& response) {
    // "HTTP/1.1 413 Payload Too Large"
    if (!response.startsWith("HTTP/1.")) return 0;
    return response.mid(9, 3).toInt();
}

// An ephemeral loopback port that is free right now.
quint16 reservePort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(fd >= 0, "socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    const bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                    ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    ::close(fd);
    check(ok, "unable to reserve test port");
    return ntohs(address.sin_port);
}

class Client {
public:
    explicit Client(quint16 port) : client_("127.0.0.1", port), port_(port) {
        this->client_.set_connection_timeout(std::chrono::seconds(5));
        this->client_.set_read_timeout(std::chrono::seconds(30));
        this->client_.set_write_timeout(std::chrono::seconds(30));
    }
    quint16 port() const { return this->port_; }
    // Like a real client, every request that creates a revision names its
    // author (and, except merges and project creation, which have default
    // messages, its message). callExact() sends the body unchanged.
    Response call(const QByteArray& verb, const QString& path, const QJsonObject& body = {}) {
        return callExact(verb, path, withAuthorship(verb, path, body));
    }
    Response callExact(const QByteArray& verb, const QString& path, const QJsonObject& body = {}) {
        const BinaryResponse reply = callRaw(verb, path,
            QJsonDocument(body).toJson(QJsonDocument::Compact), "application/json");
        const QJsonDocument json = QJsonDocument::fromJson(reply.body);
        check(json.isObject(), "HTTP response is not JSON");
        return Response{reply.status, json.object(), reply.retryAfter};
    }
    static QJsonObject testAuthor() {
        return QJsonObject{
            {"id", QString::fromStdString(aowis::dvmdb::test::testAuthorId().toString())},
            {"name", QStringLiteral("HTTP test")}};
    }
    static QJsonObject withAuthorship(const QByteArray& verb, const QString& path, QJsonObject body) {
        if (verb != "POST" || body.contains("author")) return body;
        const bool project = path == QStringLiteral("/dvmdb/v1/projects") &&
                             !body.value("unborn").toBool(false);
        const bool merge = path.startsWith(QStringLiteral("/dvmdb/v1/projects/")) &&
                           path.endsWith(QStringLiteral("merges"));
        const bool revision = path.startsWith(QStringLiteral("/dvmdb/v1/projects/")) &&
            (path.endsWith(QStringLiteral("/model-commits")) ||
             path.endsWith(QStringLiteral("/networks")) || path.endsWith(QStringLiteral("/edits")));
        if (!project && !merge && !revision) return body;
        body.insert("author", testAuthor());
        if (revision && !body.contains("message"))
            body.insert("message", QStringLiteral("HTTP test revision"));
        return body;
    }
    BinaryResponse callRaw(const QByteArray& verb, const QString& path,
                           const QByteArray& bytes = QByteArray(),
                           const QByteArray& mime = "application/vnd.aowis.revision-bundle-v2") {
        const std::string target = path.toStdString();
        httplib::Result result = verb == "GET"
            ? this->client_.Get(target)
            : verb == "PATCH"
                ? this->client_.Patch(target,
                    std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())),
                    mime.toStdString())
                : this->client_.Post(target,
                    std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())),
                    mime.toStdString());
        if (!result) {
            throw std::runtime_error("HTTP request failed: " + httplib::to_string(result.error()) +
                                     " (" + verb.toStdString() + ' ' + target + ')');
        }
        return BinaryResponse{result->status,
                              QByteArray(result->body.data(), static_cast<qsizetype>(result->body.size())),
                              QByteArray::fromStdString(result->get_header_value("Content-Type")),
                              QByteArray::fromStdString(result->get_header_value("Retry-After"))};
    }
    // Abort a bundle response after its first bytes. The server must release
    // the response's scratch file even on disconnect.
    void abortDownload(const QString& path) {
        bool receivedBody = false;
        const httplib::Result result = this->client_.Get(path.toStdString(),
            [&](const char*, std::size_t length) {
                receivedBody = length > 0;
                return false; // abort
            });
        check(!result && receivedBody, "streamed download did not produce bytes before abort");
    }
private:
    httplib::Client client_;
    quint16 port_;
};

// In-process server on a loopback port, serving from worker threads.
class TestServer {
public:
    TestServer(aowis::dvmdb::ProjectService& service, aowis::dvmdb::BundleLimits limits,
               std::shared_ptr<aowis::dvmdb::http::TransferStagingBudget> stagingBudget = nullptr) {
        this->server_.new_task_queue = [] { return new httplib::ThreadPool(8, 8, 128); };
        aowis::dvmdb::http::registerRoutes(this->server_, service, limits, stagingBudget);
        const int bound = this->server_.bind_to_any_port("127.0.0.1");
        check(bound > 0, "unable to listen");
        this->port_ = static_cast<quint16>(bound);
        this->thread_ = std::thread([this] { this->server_.listen_after_bind(); });
        this->server_.wait_until_ready();
    }
    ~TestServer() { this->stop(); }
    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;
    quint16 port() const { return this->port_; }
    void stop() {
        this->server_.stop();
        if (this->thread_.joinable()) this->thread_.join();
    }
private:
    httplib::Server server_;
    std::thread thread_;
    quint16 port_ = 0;
};


// Fixtures write through the canonical UUIDv7 repository API.
aowis::dvmdb::RevisionId commitRaw(aowis::dvmdb::ProjectService& service, const QString& projectId,
                                   std::string_view branch,
                                   const std::vector<aowis::dvmdb::Change>& changes) {
    const std::string id = projectId.toStdString();
    const std::optional<aowis::dvmdb::ProjectId> project = aowis::dvmdb::ProjectId::parse(id);
    check(project.has_value(), "fixture project is not UUIDv7");
    std::optional<aowis::dvmdb::RevisionId> committed;
    service.withRepository(id, [&](aowis::dvmdb::Repository& repository) {
        committed = repository.commit(*project, branch, changes, commitMetadata());
    });
    return *committed;
}

QString branchIdOf(aowis::dvmdb::ProjectService& service, const QString& projectId,
                   std::string_view branch) {
    const std::optional<aowis::dvmdb::BranchId> id = service.branchId(projectId.toStdString(), branch);
    check(id.has_value(), "fixture branch has no UUIDv7");
    return QString::fromStdString(id->toString());
}

QString revisionText(const aowis::dvmdb::RevisionId& id) {
    return QString::fromStdString(id.toString());
}

// Full AOWREV21 export of a branch, written by the engine to a scratch file.
QByteArray exportFull(aowis::dvmdb::ProjectService& service, const std::string& projectId,
                      std::string_view branch) {
    const std::optional<aowis::dvmdb::ProjectId> project = aowis::dvmdb::ProjectId::parse(projectId);
    check(project.has_value(), "fixture project is not UUIDv7");
    QByteArray bytes;
    service.withRepository(projectId, [&](aowis::dvmdb::Repository& repository) {
        aowis::dvmdb::ScratchFile file = repository.createScratchFile();
        repository.exportRevisionBundle(*project, branch, file.fd(), {});
        std::array<char, 64 * 1024> buffer{};
        off_t offset = 0;
        for (;;) {
            const ssize_t count = ::pread(file.fd(), buffer.data(), buffer.size(), offset);
            check(count >= 0, "reading the exported bundle failed");
            if (count == 0) break;
            bytes.append(buffer.data(), static_cast<qsizetype>(count));
            offset += static_cast<off_t>(count);
        }
    });
    return bytes;
}

// Real TCP bundle transfer of a project into its clone in an independent
// catalogue: byte-stable export, corrupt/truncated/partial uploads,
// idempotent replay, incremental fast-forward, and divergent recipients.
void exerciseBundleApi(Client& client, aowis::dvmdb::ProjectService& service,
                       const QString& sourceId) {
    namespace native = aowis::dvmdb;
    const std::string source = sourceId.toStdString();
    TemporaryRoot recipientRoot;
    native::ProjectService recipient(recipientRoot.root());
    recipient.provisionCloneTarget(source, service.project(source).name);
    TestServer recipientServer(recipient,
                               native::BundleLimits{1024ULL * 1024ULL, 10000ULL, 1024ULL * 1024ULL});
    Client receiver(recipientServer.port());
    const QString from = QStringLiteral("/dvmdb/v1/projects/") + sourceId + "/branches/main";
    const QString to = QStringLiteral("/dvmdb/v1/projects/") + sourceId + "/branches/tracking";
    native::Key item{};
    item[15] = 37;
    native::Key largeItem{};
    largeItem[15] = 38;
    const native::Bytes largePayload(512 * 1024, static_cast<std::uint8_t>(0x5c));
    (void)commitRaw(service, sourceId, "main", {{item, native::Bytes{'o', 'n', 'e'}},
                                                {largeItem, largePayload}});
    const QString firstHead = client.call("GET", from + "/head").json.value("id").toString();
    check(native::RevisionId::parse(firstHead.toStdString()).has_value(),
          "branch HEAD is not a revision UUIDv7");
    const BinaryResponse bundle = client.callRaw("GET", from + "/bundle");
    check(bundle.status == 200 && bundle.body.size() > 512 * 1024 &&
          bundle.body.startsWith("AOWREV21") &&
          bundle.contentType.startsWith("application/vnd.aowis.revision-bundle-v2"),
          "large file-backed revision bundle download failed");
    check(client.callRaw("GET", from + "/bundle").body == bundle.body,
          "repeated file-backed bundle export changed bytes");
    client.abortDownload(from + "/bundle");
    check(client.callRaw("GET", from + "/bundle").body == bundle.body,
          "download after client cancellation changed bytes");
    check(client.callRaw("GET", from + "/bundle?base=invalid").status == 400,
          "malformed base revision accepted");
    check(client.callRaw("GET", from + "/bundle?base=" +
                         revisionText(native::RevisionId::generate())).status == 404,
          "unknown export base accepted");
    check(client.callRaw("GET", from + "/bundle?unexpected=1").status == 400,
          "unknown bundle query argument accepted");
    check(client.callRaw("GET", "/dvmdb/v1/projects/invalid/branches/main/bundle").status == 400,
          "invalid project bundle URL accepted");
    check(receiver.callRaw("POST", to + "/bundle", QByteArray()).status == 400,
          "empty bundle import accepted");
    check(receiver.callRaw("POST", to + "/bundle", QByteArray(1024 * 1024 + 1, 'x')).status == 413,
          "oversized bundle upload accepted");
    // Partial request body followed by disconnect must never invoke bundle publication.
    {
        RawConnection interrupted(receiver.port());
        check(interrupted.connected(), "unable to connect interrupted upload");
        const QByteArray head = "POST " + to.toUtf8() +
            "/bundle HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Content-Type: application/vnd.aowis.revision-bundle-v2\r\n"
            "Content-Length: " + QByteArray::number(bundle.body.size()) + "\r\n\r\n";
        interrupted.send(head + bundle.body.left(bundle.body.size() / 2));
        interrupted.close();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    check(receiver.call("GET", to + "/head").status == 404,
          "disconnected partial request published a branch");
    check(receiver.callRaw("POST", to + "/bundle", bundle.body.left(bundle.body.size() - 1)).status == 400,
          "truncated bundle accepted");
    QByteArray corrupted = bundle.body;
    corrupted[corrupted.size() - 1] = static_cast<char>(corrupted.back() ^ 1);
    check(receiver.callRaw("POST", to + "/bundle", corrupted).status == 400,
          "corrupted bundle accepted");
    check(receiver.call("GET", to + "/head").status == 404,
          "invalid bundle published a branch");
    check(receiver.call("POST", to + "/bundle", QJsonObject{}).status == 400,
          "JSON was accepted as a binary bundle");
    const BinaryResponse imported = receiver.callRaw("POST", to + "/bundle", bundle.body);
    const QJsonObject first = QJsonDocument::fromJson(imported.body).object();
    check(imported.status == 201 && first.value("status").toString() == QStringLiteral("created") &&
          first.value("head").toString() == firstHead &&
          first.value("objectsImported").toInteger() > 0,
          "initial bundle import failed");
    check(service.branchId(source, "main") == recipient.branchId(source, "tracking"),
          "bundle transfer did not preserve the branch UUIDv7");
    const BinaryResponse repeated = receiver.callRaw("POST", to + "/bundle", bundle.body);
    const QJsonObject repeatJson = QJsonDocument::fromJson(repeated.body).object();
    check(repeated.status == 200 && repeatJson.value("status").toString() == QStringLiteral("up_to_date") &&
          repeatJson.value("objectsImported").toInteger() == 0,
          "repeated bundle import was not idempotent");
    (void)commitRaw(service, sourceId, "main", {{item, native::Bytes{'t', 'w', 'o'}}});
    const QString nextHead = client.call("GET", from + "/head").json.value("id").toString();
    const BinaryResponse delta = client.callRaw("GET", from + "/bundle?base=" + firstHead);
    check(delta.status == 200 && delta.body.startsWith("AOWREV22"),
          "incremental binary bundle export failed");
    const BinaryResponse advanced = receiver.callRaw("POST", to + "/bundle", delta.body);
    const QJsonObject advancedJson = QJsonDocument::fromJson(advanced.body).object();
    check(advanced.status == 200 && advancedJson.value("status").toString() == QStringLiteral("fast_forward") &&
          advancedJson.value("head").toString() == nextHead,
          "incremental import failed");
    check(receiver.call("GET", to + "/head").json.value("id").toString() == nextHead,
          "destination branch did not fast-forward");
    (void)commitRaw(recipient, sourceId, "tracking", {{item, native::Bytes{'d', 'e', 's', 't'}}});
    const QString divergentHead = receiver.call("GET", to + "/head").json.value("id").toString();
    (void)commitRaw(service, sourceId, "main", {{item, native::Bytes{'n', 'e', 'x', 't'}}});
    const BinaryResponse more = client.callRaw("GET", from + "/bundle?base=" + nextHead);
    check(more.status == 200, "follow-up incremental export failed");
    check(receiver.callRaw("POST", to + "/bundle", more.body).status == 409,
          "divergent tracking history was overwritten");
    check(receiver.call("GET", to + "/head").json.value("id").toString() == divergentHead,
          "rejected bundle mutated divergent branch");
    recipient.withRepository(source, [](native::Repository& repository) {
        (void)repository.verifyRepository();
    });
}

// Review F2/F10b: classification by StorageErrorKind, never message text.
void exerciseErrorClassification(Client& client, aowis::dvmdb::ProjectService& service,
                                 const std::filesystem::path& root) {
    namespace native = aowis::dvmdb;
    const Response created = client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "Errors"}});
    check(created.status == 201, "error-classification project creation failed");
    const QString id = created.json.value("id").toString();
    const QString prefix = "/dvmdb/v1/projects/" + id;
    const native::RevisionId initial = service.head(id.toStdString(), "main");
    native::Key key{};
    key[15] = 7;
    const native::RevisionId committed =
        commitRaw(service, id, "main", {native::Change{key, native::Bytes{1, 2, 3}}});
    // A well-formed UUIDv7 that names no revision of this project: 404, not 500.
    const QString unknown = revisionText(native::RevisionId::generate());
    const Response missing = client.call("GET", prefix + "/revisions/" + unknown);
    check(missing.status == 404 &&
          missing.json.value("error").toObject().value("code").toString() == "not_found",
          "unknown revision UUIDv7 was not reported as 404 not_found");
    check(client.call("GET", prefix + "/diff?from=" + unknown + "&to=" +
                      revisionText(committed)).status == 404,
          "diff accepted an unknown revision UUIDv7");

    // Damaged local storage is a server failure: 500 with a generic message
    // and no internal details (checksums, paths, errno text) in the response.
    // Byte 50 of the pack lies in the project's root revision record.
    const std::filesystem::path pack = root / "projects" / id.toStdString() / "repository" / "objects.pack";
    {
        std::fstream stream(pack, std::ios::in | std::ios::out | std::ios::binary);
        check(stream.good(), "cannot open project pack for corruption test");
        stream.seekg(50);
        char byte = 0;
        stream.read(&byte, 1);
        byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0xffU);
        stream.seekp(50);
        stream.write(&byte, 1);
        check(stream.good(), "cannot corrupt project pack");
    }
    const BinaryResponse damaged = client.callRaw("GET", prefix + "/revisions/" + revisionText(initial));
    const QJsonObject error = QJsonDocument::fromJson(damaged.body).object().value("error").toObject();
    check(damaged.status == 500 && error.value("code").toString() == "internal_error" &&
          error.value("message").toString() == "database operation failed",
          "storage corruption was not reported as a generic 500");
    check(!damaged.body.contains("checksum") && !damaged.body.contains("pack"),
          "storage corruption details leaked to the HTTP client");
}

// Review F9/F10a: list-like responses are bounded before materialization.
void exerciseBoundsAndScoping(Client& client, aowis::dvmdb::ProjectService& service) {
    namespace native = aowis::dvmdb;
    const Response created = client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "Bounds"}});
    check(created.status == 201, "bounds project creation failed");
    const QString id = created.json.value("id").toString();
    const QString prefix = "/dvmdb/v1/projects/" + id;
    const native::RevisionId empty = service.head(id.toStdString(), "main");
    std::vector<native::Change> changes;
    for (unsigned i = 0; i < 1000; ++i) {
        native::Key key{};
        key[0] = static_cast<std::uint8_t>(i * 37U);
        key[14] = static_cast<std::uint8_t>(i >> 8U);
        key[15] = static_cast<std::uint8_t>(i);
        changes.push_back(native::Change{key, native::Bytes{1}});
    }
    (void)service.createBranch(id.toStdString(), "bulk", "main");
    const native::RevisionId exactly1000 = commitRaw(service, id, "bulk", changes);
    native::Key extra{};
    extra[0] = 0xfe;
    extra[15] = 0xfe;
    const native::RevisionId over1000 =
        commitRaw(service, id, "bulk", {native::Change{extra, native::Bytes{2}}});
    const Response fits = client.call("GET", prefix + "/diff?from=" + revisionText(empty) +
                                      "&to=" + revisionText(exactly1000));
    check(fits.status == 200 && fits.json.value("changes").toArray().size() == 1000,
          "diff with exactly 1000 records was not returned");
    const Response tooLarge = client.call("GET", prefix + "/diff?from=" + revisionText(empty) +
                                          "&to=" + revisionText(over1000));
    check(tooLarge.status == 413 &&
          tooLarge.json.value("error").toObject().value("code").toString() == "limit_exceeded",
          "diff with 1001 records was not rejected with 413 limit_exceeded");
}

// Phase 5 (cpp-httplib): limits are enforced before bodies are read, uploads
// stream without holding project locks, and requests run concurrently.
void exerciseTransport(Client& client, aowis::dvmdb::ProjectService& service,
                       const std::filesystem::path& root, std::uint64_t transferLimit) {
    namespace native = aowis::dvmdb;
    const quint16 port = client.port();
    // The served target is a clone target of an independent source
    // catalogue, so it accepts that project's revision bundle.
    TemporaryRoot originRoot;
    native::ProjectService origin(originRoot.root());
    const native::ProjectInfo sourceProject = origin.createProject("Transport source", commitMetadata());
    const QString targetId = QString::fromStdString(sourceProject.id);
    {
        std::vector<native::Change> changes;
        for (unsigned i = 0; i < 200; ++i) {
            native::Key key{};
            key[0] = static_cast<std::uint8_t>(i);
            key[15] = 1;
            changes.push_back(native::Change{key, native::Bytes(1024, static_cast<std::uint8_t>(i))});
        }
        (void)commitRaw(origin, targetId, "main", changes);
    }
    const BinaryResponse bundle{200, exportFull(origin, sourceProject.id, "main"), {}, {}};
    check(bundle.body.size() > 100 * 1024 && bundle.body.startsWith("AOWREV21"),
          "transport bundle export failed");
    (void)service.provisionCloneTarget(sourceProject.id, "Transport target");
    const QByteArray uploadPath = "/dvmdb/v1/projects/" + targetId.toUtf8() + "/branches/incoming/bundle";

    // 1. A declared length above the transfer limit is refused before any body
    //    byte is sent; the server answers and closes the connection.
    {
        RawConnection raw(port);
        check(raw.connected(), "raw connect failed");
        raw.send("POST " + uploadPath + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " +
                 QByteArray::number(static_cast<qulonglong>(transferLimit + 1)) + "\r\n\r\n");
        const QByteArray reply = raw.receive(3000);
        check(statusOf(reply) == 413 && reply.contains("transfer_too_large"),
              "oversized declared upload not rejected with 413 before its body");
        check(raw.closedByPeer(), "connection not closed after rejecting an unread body");
    }
    // 2. Per-route JSON limits, also decided before the body is read.
    const std::vector<std::pair<QByteArray, std::size_t>> jsonRoutes{
        {"/dvmdb/v1/projects", aowis::dvmdb::http::maxJsonSmallBytes},
        {"/dvmdb/v1/projects/" + targetId.toUtf8() + "/branches", aowis::dvmdb::http::maxJsonSmallBytes},
        {"/dvmdb/v1/projects/" + targetId.toUtf8() + "/merges", aowis::dvmdb::http::maxJsonMergeBytes},
        {"/dvmdb/v1/projects/" + targetId.toUtf8() + "/branches/main/networks",
         aowis::dvmdb::http::maxJsonModelBytes}};
    for (const std::pair<QByteArray, std::size_t>& route : jsonRoutes) {
        RawConnection raw(port);
        raw.send("POST " + route.first + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                 "Content-Length: " + QByteArray::number(static_cast<qulonglong>(route.second + 1)) + "\r\n\r\n");
        const QByteArray reply = raw.receive(3000);
        check(statusOf(reply) == 413 && reply.contains("limit_exceeded") && raw.closedByPeer(),
              "JSON body over its route limit not rejected before reading");
    }
    {
        RawConnection raw(port);
        raw.send("POST /dvmdb/v1/projects HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                 "Transfer-Encoding: chunked\r\n\r\n10\r\n{\"name\":\"chunk\"}\r\n0\r\n\r\n");
        const QByteArray reply = raw.receive(3000);
        check(statusOf(reply) == 411 && reply.contains("length_required"),
              "chunked JSON request without Content-Length accepted");
    }
    // 3. A chunked upload that grows past the limit is cut off with 413 and
    //    publishes nothing.
    {
        RawConnection raw(port);
        raw.send("POST " + uploadPath + " HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\n");
        const QByteArray chunk(64 * 1024, 'x');
        const QByteArray framed = QByteArray::number(chunk.size(), 16) + "\r\n" + chunk + "\r\n";
        for (std::uint64_t sent = 0; sent <= transferLimit + chunk.size(); sent += chunk.size()) {
            try { raw.send(framed); } catch (const std::exception&) { break; } // server may close early
        }
        const QByteArray reply = raw.receive(3000);
        check(statusOf(reply) == 413, "chunked upload beyond the limit not rejected with 413");
    }
    check(client.call("GET", "/dvmdb/v1/projects/" + targetId + "/branches/incoming/head").status == 404,
          "rejected upload published a branch");
    // 4. A slow upload neither blocks other requests nor holds the catalogue
    //    or project lock while its body is still arriving.
    {
        RawConnection slow(port);
        slow.send("POST " + uploadPath + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " +
                  QByteArray::number(bundle.body.size()) + "\r\n\r\n" + bundle.body.left(bundle.body.size() / 2));
        QElapsedTimer elapsed;
        elapsed.start();
        check(client.call("GET", "/dvmdb/v1/health").status == 200, "health blocked by a slow upload");
        check(client.call("GET", "/dvmdb/v1/projects").status == 200, "catalogue blocked by a slow upload");
        check(client.call("GET", "/dvmdb/v1/projects/" + targetId + "/branches").status == 200,
              "target project blocked by a slow upload");
        check(elapsed.elapsed() < 2000, "requests during a slow upload were delayed");
        slow.send(bundle.body.mid(bundle.body.size() / 2));
        const QByteArray reply = slow.receive(10000);
        check(statusOf(reply) == 201 && reply.contains("\"created\""),
              "slow upload was not imported after completion");
    }
    // 5. An upload abandoned halfway publishes nothing and leaves no file.
    {
        RawConnection abandoned(port);
        abandoned.send("POST /dvmdb/v1/projects/" + targetId.toUtf8() + "/branches/abandoned/bundle HTTP/1.1\r\n"
                       "Host: 127.0.0.1\r\nContent-Length: " + QByteArray::number(bundle.body.size()) +
                       "\r\n\r\n" + bundle.body.left(bundle.body.size() / 3));
        abandoned.close();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        check(client.call("GET", "/dvmdb/v1/projects/" + targetId + "/branches/abandoned/head").status == 404,
              "abandoned upload published a branch");
        check(std::filesystem::is_empty(root / "projects" / targetId.toStdString() / "repository" / "tmp"),
              "abandoned upload left a scratch file");
    }
    // 6. Requests are served concurrently by the worker pool.
    {
        std::atomic<int> failures{0};
        std::vector<std::thread> workers;
        for (int worker = 0; worker < 8; ++worker) {
            workers.emplace_back([&, worker]() {
                try {
                    Client parallel(port);
                    for (int request = 0; request < 20; ++request) {
                        const QString path = request % 2 == 0
                            ? "/dvmdb/v1/projects/" + targetId + "/branches/incoming/head"
                            : "/dvmdb/v1/projects/" + targetId + "/branches/incoming/history?limit=1";
                        if (parallel.call("GET", path).status != 200) failures.fetch_add(1);
                    }
                } catch (...) {
                    failures.fetch_add(1);
                }
                (void)worker;
            });
        }
        for (std::thread& thread : workers) thread.join();
        check(failures.load() == 0, "concurrent requests failed");
    }
    // 7. Protocol-level rejections by httplib also carry JSON error bodies.
    const Response unknownRoute = client.call("GET", "/dvmdb/v1/no-such-route");
    check(unknownRoute.status == 404 &&
          unknownRoute.json.value("error").toObject().value("code").toString() == "not_found",
          "unknown route did not return a JSON 404");
    // Header and URI sizes are bounded by httplib (8 KiB per header line and
    // URI, at most 100 header fields); rejections close the connection.
    QByteArray manyHeaders;
    for (int i = 0; i < 200; ++i) manyHeaders += "X-" + QByteArray::number(i) + ": a\r\n";
    const std::vector<std::tuple<QByteArray, int, QByteArray>> protocolCases{
        {"GET /dvmdb/v1/health HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Large: " + QByteArray(16 * 1024, 'a') +
             "\r\n\r\n", 400, "invalid_request"},
        {"GET /dvmdb/v1/health HTTP/1.1\r\nHost: 127.0.0.1\r\n" + manyHeaders + "\r\n", 400, "invalid_request"},
        {"GET /dvmdb/v1/" + QByteArray(9000, 'a') + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", 414, "uri_too_long"}};
    for (const std::tuple<QByteArray, int, QByteArray>& protocolCase : protocolCases) {
        RawConnection raw(port);
        raw.send(std::get<0>(protocolCase));
        const QByteArray reply = raw.receive(3000);
        check(statusOf(reply) == std::get<1>(protocolCase) && reply.contains(std::get<2>(protocolCase)) &&
              raw.closedByPeer(), "oversized header/URI not rejected with a JSON error");
    }
}

// Holds one scoped operation of a project (as a long import would) until
// release(), so tests can observe what still runs meanwhile.
class ProjectHold {
public:
    ProjectHold(aowis::dvmdb::ProjectService& service, const std::string& projectId) {
        this->thread_ = std::thread([this, &service, projectId] {
            service.withRepository(projectId, [this](aowis::dvmdb::Repository&) {
                std::unique_lock<std::mutex> lock(this->mutex_);
                this->entered_ = true;
                this->changed_.notify_all();
                this->changed_.wait(lock, [this] { return this->released_; });
            });
        });
        std::unique_lock<std::mutex> lock(this->mutex_);
        check(this->changed_.wait_for(lock, std::chrono::seconds(10), [this] { return this->entered_; }),
              "project hold did not start");
    }
    ~ProjectHold() { this->release(); }
    ProjectHold(const ProjectHold&) = delete;
    ProjectHold& operator=(const ProjectHold&) = delete;
    void release() {
        {
            std::lock_guard<std::mutex> lock(this->mutex_);
            this->released_ = true;
        }
        this->changed_.notify_all();
        if (this->thread_.joinable()) this->thread_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool released_ = false;
    std::thread thread_;
};

// Joins a helper thread on scope exit, so a failed check reports its
// message instead of terminating with a joinable thread.
struct Joiner {
    explicit Joiner(std::thread& target) : thread(target) {}
    ~Joiner() {
        if (this->thread.joinable()) this->thread.join();
    }
    Joiner(const Joiner&) = delete;
    Joiner& operator=(const Joiner&) = delete;
    std::thread& thread;
};

void waitForPending(aowis::dvmdb::ProjectService& service, const QString& id, std::size_t count) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (service.pendingOperations(id.toStdString()) != count) {
        check(std::chrono::steady_clock::now() < deadline, "pending request count not reached");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

std::chrono::milliseconds timed(const std::function<void()>& action) {
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    action();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}

// Review F4: a long operation of one project blocks neither /health nor
// other projects; a full project queue answers 503 with Retry-After before
// reading an upload; racing writes of one project serialize correctly.
void exerciseConcurrency() {
    namespace native = aowis::dvmdb;
    TemporaryRoot root;
    native::ProjectService service(root.root(), 3);
    TestServer server(service, native::BundleLimits{64ULL * 1024ULL * 1024ULL, 100000ULL,
                                                    8ULL * 1024ULL * 1024ULL});
    Client client(server.port());
    const QString a = client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "A"}}).json.value("id").toString();
    const QString b = client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "B"}}).json.value("id").toString();
    const QString aPrefix = "/dvmdb/v1/projects/" + a;
    const QString bPrefix = "/dvmdb/v1/projects/" + b;
    const QString aMain = branchIdOf(service, a, "main");
    const QString bMain = branchIdOf(service, b, "main");

    // A bundle of ~4 MiB of an independent project, imported into that
    // project's clone target on this server while A is busy.
    TemporaryRoot originRoot;
    native::ProjectService origin(originRoot.root());
    const native::ProjectInfo imported = origin.createProject("Imported", commitMetadata());
    {
        std::vector<native::Change> changes;
        for (unsigned i = 0; i < 2000; ++i) {
            native::Key item{};
            item[0] = static_cast<std::uint8_t>(i >> 8U);
            item[1] = static_cast<std::uint8_t>(i);
            item[15] = 7;
            changes.push_back(native::Change{item, native::Bytes(2048, static_cast<std::uint8_t>(i))});
        }
        (void)commitRaw(origin, QString::fromStdString(imported.id), "main", changes);
    }
    const QByteArray bundle = exportFull(origin, imported.id, "main");
    (void)service.provisionCloneTarget(imported.id, "Imported");
    const QString importPath = "/dvmdb/v1/projects/" + QString::fromStdString(imported.id) +
                               "/branches/main/bundle";

    std::thread importer;
    std::thread first;
    std::thread second;
    const Joiner joinImporter(importer);
    const Joiner joinFirst(first);
    const Joiner joinSecond(second);
    int firstStatus = 0;
    int secondStatus = 0;
    {
        ProjectHold hold(service, a.toStdString());
        // Everything else answers promptly while A's operation runs.
        check(timed([&] { check(client.call("GET", "/dvmdb/v1/health").status == 200, "health failed"); }) <
              std::chrono::milliseconds(200), "/health waited for a project operation");
        check(timed([&] { check(client.call("GET", "/dvmdb/v1/ready").status == 200, "ready failed"); }) <
              std::chrono::milliseconds(200), "/ready waited for a project operation");
        check(client.call("GET", aPrefix + "/branches/main/head").status == 200, "read of held project failed");
        check(client.call("POST", bPrefix + "/branches",
                          QJsonObject{{"name", "side"}, {"fromBranchId", bMain}}).status == 201,
              "other project's write waited");
        // A real import into another project, with /health probed throughout.
        std::atomic<bool> importing{true};
        int importStatus = 0;
        importer = std::thread([&] {
            Client own(server.port());
            importStatus = own.callRaw("POST", importPath, bundle).status;
            importing = false;
        });
        std::chrono::milliseconds slowest{0};
        unsigned probes = 0;
        while (importing.load() || probes == 0) {
            slowest = std::max(slowest, timed([&] {
                check(client.call("GET", "/dvmdb/v1/health").status == 200, "health failed during import");
            }));
            ++probes;
        }
        importer.join();
        check(importStatus == 201, "import into another project failed while A was busy");
        check(slowest < std::chrono::milliseconds(200), "/health was slow during an import");

        // Two racing creations of one branch on A queue behind the hold.
        first = std::thread([&] {
            Client own(server.port());
            firstStatus = own.call("POST", aPrefix + "/branches",
                                   QJsonObject{{"name", "race"}, {"fromBranchId", aMain}}).status;
        });
        second = std::thread([&] {
            Client own(server.port());
            secondStatus = own.call("POST", aPrefix + "/branches",
                                    QJsonObject{{"name", "race"}, {"fromBranchId", aMain}}).status;
        });
        waitForPending(service, a, 3);
        // The queue of A is full: 503 + Retry-After, also before an upload body.
        const Response busy = client.call("POST", aPrefix + "/branches",
                                          QJsonObject{{"name", "late"}, {"fromBranchId", aMain}});
        check(busy.status == 503 && busy.retryAfter == "1" &&
              busy.json.value("error").toObject().value("code").toString() == "busy",
              "full project queue not answered with 503 busy and Retry-After");
        const BinaryResponse busyUpload = client.callRaw("POST", aPrefix + "/branches/up/bundle", bundle);
        check(busyUpload.status == 503 && busyUpload.retryAfter == "1", "upload to a full project queue accepted");
        {
            // Headers only: the 503 must come without waiting for the body.
            RawConnection raw(server.port());
            raw.send("POST " + aPrefix.toUtf8() + "/branches/up/bundle HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                     "Content-Type: application/vnd.aowis.revision-bundle-v2\r\nContent-Length: 1000000\r\n\r\n");
            const QByteArray reply = raw.receive(2000);
            check(statusOf(reply) == 503 && reply.contains("Retry-After: 1") && raw.closedByPeer(),
                  "full project queue did not refuse the upload before its body");
        }
        check(client.call("POST", bPrefix + "/branches",
                          QJsonObject{{"name", "still"}, {"fromBranchId", bMain}}).status == 201,
              "other project affected by A's full queue");
        hold.release();
        first.join();
        second.join();
        check((firstStatus == 201 && secondStatus == 409) || (firstStatus == 409 && secondStatus == 201),
              "racing branch creations not exactly one 201 and one 409");
    }
    check(service.pendingOperations(a.toStdString()) == 0, "project queue not drained");
    check(client.call("POST", aPrefix + "/branches",
                      QJsonObject{{"name", "late"}, {"fromBranchId", aMain}}).status == 201,
          "project still busy after its queue drained");


    // Stress: 8 clients, mixed reads and writes over three projects. Every
    // answer is a success or a 503 for a full queue; nothing else.
    const QString c = client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "C"}}).json.value("id").toString();
    const std::vector<QString> projects{a, b, c};
    const std::vector<QString> mainIds{aMain, bMain, branchIdOf(service, c, "main")};
    std::atomic<unsigned> created{0};
    std::atomic<unsigned> busyAnswers{0};
    std::atomic<unsigned> unexpected{0};
    std::vector<std::thread> clients;
    for (unsigned worker = 0; worker < 8; ++worker) {
        clients.emplace_back([&, worker] {
            Client own(server.port());
            for (unsigned step = 0; step < 30; ++step) {
                const std::size_t index = (worker + step) % projects.size();
                const QString prefix = "/dvmdb/v1/projects/" + projects[index];
                int status = 0;
                switch (step % 4) {
                case 0:
                    status = own.call("POST", prefix + "/branches", QJsonObject{
                        {"name", QStringLiteral("s%1_%2").arg(worker).arg(step)},
                        {"fromBranchId", mainIds[index]}}).status;
                    if (status == 201) ++created;
                    break;
                case 1: status = own.call("GET", prefix + "/branches/main/head").status; break;
                case 2: status = own.callRaw("GET", prefix + "/branches/main/bundle").status; break;
                default: status = own.call("GET", "/dvmdb/v1/health").status; break;
                }
                if (status == 503) ++busyAnswers;
                else if (status != 200 && status != 201) ++unexpected;
            }
        });
    }
    for (std::thread& thread : clients) thread.join();
    check(unexpected == 0, "unexpected status under concurrent load");
    std::size_t branches = 0;
    for (const QString& project : projects) {
        branches += static_cast<std::size_t>(client.call("GET", "/dvmdb/v1/projects/" + project + "/branches")
                                                 .json.value("branches").toArray().size());
        service.withRepository(project.toStdString(), [](native::Repository& repository) {
            (void)repository.verifyRepository();
        });
    }
    // Before the stress: A main/race/late, B main/side/still, C main.
    check(branches == 7U + created.load(), "branch count does not match successful creations");
    std::cout << "  concurrent load: " << created.load() << " branches created, "
              << busyAnswers.load() << " busy answers\n";
}

// Project catalogue and branch routes, then bundle transfer, error
// classification, response bounds and transport limits on one server.
void exercise() {
    TemporaryRoot root;
    QString firstId;
    QString firstRevision;
    quint16 listenerPort = 0;
    {
        aowis::dvmdb::ProjectService service(root.root());
        const aowis::dvmdb::BundleLimits limits{1024ULL * 1024ULL, 10000ULL, 1024ULL * 1024ULL};
        TestServer server(service, limits);
        listenerPort = server.port();
        Client client(server.port());
        check(client.call("GET", "/dvmdb/v1/health").status == 200, "health failed");
        check(client.call("GET", "/dvmdb/v1/ready").status == 200, "readiness failed");
        check(client.call("GET", "/dvmdb/v1/projects").json.value("projects").toArray().isEmpty(),
              "catalogue initially nonempty");
        check(client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", ""}}).status == 400,
              "invalid create accepted");
        check(client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "X"}, {"evil", 42}}).status == 400,
              "unknown field accepted");
        const Response first = client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "Hydraulik Ü"}});
        check(first.status == 201, "create project failed");
        firstId = first.json.value("id").toString();
        check(aowis::dvmdb::ProjectId::parse(firstId.toStdString()).has_value(),
              "created project id is not UUIDv7");
        check(first.json.value("name").toString() == "Hydraulik Ü", "project name wrong");
        check(first.json.keys() == QStringList({"id", "name"}), "project JSON has unexpected fields");
        check(client.call("POST", "/dvmdb/v1/projects",
                          QJsonObject{{"name", "X"}, {"storage", "pack"}}).status == 400,
              "removed storage field accepted");
        const Response second = client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "Independent"}});
        check(second.status == 201 && second.json.value("id").toString() != firstId,
              "second project creation failed");
        check(client.call("GET", "/dvmdb/v1/projects").json.value("projects").toArray().size() == 2,
              "project listing incorrect");
        check(client.call("GET", "/dvmdb/v1/projects/invalid-id").status == 400,
              "invalid project UUID was accepted");
        check(client.call("GET", "/dvmdb/v1/projects/00000000-0000-4000-8000-000000000000").status == 400,
              "non-UUIDv7 project id was accepted");
        check(client.call("GET", "/dvmdb/v1/projects/" +
                          QString::fromStdString(aowis::dvmdb::ProjectId::generate().toString())).status == 404,
              "unknown project accepted");
        check(client.call("GET", "/dvmdb/v1/projects/" + firstId).status == 200,
              "project details failed");
        const QString prefix = "/dvmdb/v1/projects/" + firstId;
        check(client.call("GET", prefix + "/branches").json.value("branches").toArray().size() == 1,
              "initial branch missing");
        const QString mainId = branchIdOf(service, firstId, "main");
        const Response head = client.call("GET", prefix + "/branches/main/head");
        check(head.status == 200, "head request failed");
        firstRevision = head.json.value("id").toString();
        check(aowis::dvmdb::RevisionId::parse(firstRevision.toStdString()).has_value(),
              "head is not a revision UUIDv7");
        check(client.call("GET", prefix + "/branches/main/history?limit=0").status == 400,
              "invalid history limit accepted");
        check(client.call("GET", prefix + "/branches/main/history?limit=1").json.value("revisions").toArray().size() == 1,
              "history read failed");
        check(client.call("GET", prefix + "/branches/main/history?after=" + firstRevision)
                   .json.value("revisions").toArray().isEmpty(), "exclusive cursor failed");
        check(client.call("GET", prefix + "/revisions/" + firstRevision).status == 200,
              "immutable revision read failed");
        check(client.call("GET", prefix + "/revisions/garbage").status == 400,
              "invalid revision UUID accepted");
        check(client.call("GET", prefix + "/revisions/" +
                          revisionText(aowis::dvmdb::RevisionId::generate())).status == 404,
              "unknown revision should be 404");
        check(client.call("GET", prefix + "/branches/unknown/head").status == 404,
              "unknown branch accepted");
        const Response fork = client.call("POST", prefix + "/branches",
            QJsonObject{{"name", "design"}, {"fromBranchId", mainId}, {"revision", firstRevision}});
        check(fork.status == 201 && fork.json.value("id").toString() == firstRevision &&
              fork.json.value("originBranchId").toString() == mainId,
              "pinned fork failed");
        check(client.call("POST", prefix + "/branches",
                          QJsonObject{{"name", "design"}, {"fromBranchId", mainId}}).status == 409,
              "duplicate branch accepted");
        check(client.call("POST", prefix + "/branches",
                          QJsonObject{{"name", "../bad"}, {"fromBranchId", mainId}}).status == 400,
              "malformed branch name accepted");
        check(client.call("POST", prefix + "/branches", QJsonObject{
                  {"name", "missing-source"},
                  {"fromBranchId", QString::fromStdString(aowis::dvmdb::BranchId::generate().toString())}}).status == 404,
              "missing source branch should be 404");
        check(client.call("POST", prefix + "/branches",
                          QJsonObject{{"name", "bad"}, {"revision", firstRevision}}).status == 400,
              "fork without source accepted");
        check(client.call("POST", prefix + "/branches",
                          QJsonObject{{"name", "scratch"}, {"fromBranchId", mainId}}).status == 201,
              "branch creation failed");
        check(client.call("GET", prefix + "/branches").json.value("branches").toArray().size() == 3,
              "branch listing wrong");
        check(client.call("GET", "/dvmdb/v1/projects/" + second.json.value("id").toString() + "/branches")
                   .json.value("branches").toArray().size() == 1, "branch leaked to another project");
        {
            // Review F6: bundle spools and exports use repository scratch files,
            // so the whole transfer API works without a usable TMPDIR.
            const char* previous = ::getenv("TMPDIR");
            const std::string saved = previous == nullptr ? std::string() : std::string(previous);
            check(::setenv("TMPDIR", (root.root() / "no-such-tmpdir").c_str(), 1) == 0, "setenv failed");
            try {
                exerciseBundleApi(client, service, firstId);
            } catch (...) {
                if (previous == nullptr) ::unsetenv("TMPDIR"); else ::setenv("TMPDIR", saved.c_str(), 1);
                throw;
            }
            if (previous == nullptr) ::unsetenv("TMPDIR"); else ::setenv("TMPDIR", saved.c_str(), 1);
            check(std::filesystem::is_empty(root.root() / "projects" / firstId.toStdString() /
                                            "repository" / "tmp"),
                  "bundle transfers left files in the project scratch directory");
        }
        exerciseErrorClassification(client, service, root.root());
        exerciseBoundsAndScoping(client, service);
        exerciseTransport(client, service, root.root(), limits.maxBytes);
        server.stop();
        check(!RawConnection(listenerPort).connected(), "stopped HTTP server still accepts connections");
    }
    {
        aowis::dvmdb::ProjectService reopened(root.root());
        check(reopened.project(firstId.toStdString()).name == "Hydraulik Ü", "restart lost project name");
        check(revisionText(reopened.head(firstId.toStdString(), "design")) == firstRevision,
              "restart lost fork revision");
    }
}

struct ServerRun {
    bool listened = false;
    int exitCode = -1;
    QString output;
    QString diagnostics;
};

// Holding an incomplete upload on project A must reserve the global HTTP
// staging budget and reject new staging on project B, but not block /health.
// Closing A must release capacity without changing any branch.
void exerciseTransferStagingBudget() {
    namespace native = aowis::dvmdb;
    TemporaryRoot root;
    native::ProjectService service(root.root());
    constexpr std::uint64_t transferBytes = 1024ULL * 1024ULL;
    std::shared_ptr<native::http::TransferStagingBudget> staging =
        std::make_shared<native::http::TransferStagingBudget>(transferBytes);
    TestServer server(service, native::BundleLimits{transferBytes, 10000, transferBytes}, staging);
    Client client(server.port());
    const Response first = client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "Slow source"}});
    const Response second = client.call("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "Other project"}});
    check(first.status == 201 && second.status == 201, "staging test projects not created");
    const QString firstId = first.json.value("id").toString();
    const QString secondId = second.json.value("id").toString();
    const QString otherBundle = "/dvmdb/v1/projects/" + secondId + "/branches/main/bundle";
    {
        RawConnection slow(server.port());
        check(slow.connected(), "slow staging client failed to connect");
        slow.send("POST /dvmdb/v1/projects/" + firstId.toUtf8() +
                  "/branches/unpublished/bundle HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                  "Content-Type: application/vnd.aowis.revision-bundle-v2\r\nContent-Length: 100000\r\n\r\n"
                  "partially-sent");
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (staging->reservedBytes() == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(staging->reservedBytes() == transferBytes, "slow upload did not reserve staging budget");
        const BinaryResponse rejected = client.callRaw("GET", otherBundle);
        check(rejected.status == 503 && rejected.body.contains("staging_busy") &&
              rejected.retryAfter == "1", "other project staging was not rejected with retry hint");
        check(client.call("GET", "/dvmdb/v1/health").status == 200,
              "health endpoint blocked by exhausted staging budget");
        slow.close();
    }
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (staging->reservedBytes() != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(staging->reservedBytes() == 0, "disconnected upload retained staging reservation");
    check(client.callRaw("GET", otherBundle).status == 200,
          "transfer not admitted after disconnected upload released quota");
    check(staging->reservedBytes() == 0, "completed download retained staging reservation");
    check(client.call("GET", "/dvmdb/v1/projects/" + firstId + "/branches/unpublished/head").status == 404,
          "abandoned upload published an incomplete branch");
}

// A separate full-DAG protocol test keeps the ordinary CRUD fixture intact.
// Paged revision graph over HTTP: bounded pages, signed project-bound
// cursors, a pinned HEAD snapshot across pages, and complete ancestry.
void exerciseRevisionGraphHttp() {
    TemporaryRoot root;
    aowis::dvmdb::ProjectService service(root.root());
    const std::string id = service.createProject("Graph API", commitMetadata()).id;
    const std::string other = service.createProject("Other graph", commitMetadata()).id;
    const QString project = QString::fromStdString(id);
    const aowis::dvmdb::RevisionId initial = service.head(id, "main");
    (void)commitRaw(service, project, "main", {{aowis::dvmdb::Key{1}, aowis::dvmdb::Bytes{1}}});
    (void)service.createBranch(id, "feature", "main");
    (void)service.createBranch(id, "alias", "feature");
    (void)commitRaw(service, project, "main", {{aowis::dvmdb::Key{2}, aowis::dvmdb::Bytes{2}}});
    (void)commitRaw(service, project, "feature", {{aowis::dvmdb::Key{3}, aowis::dvmdb::Bytes{3}}});
    const aowis::dvmdb::MergeResult result = service.mergeBranches(id, "main", "feature", commitMetadata());
    check(result.status == aowis::dvmdb::MergeStatus::Merged && result.resultHead.has_value() &&
          service.revision(id, *result.resultHead).parents.size() == 2,
          "HTTP graph test merge fixture failed");
    const aowis::dvmdb::RevisionId merged = *result.resultHead;
    TestServer server(service, aowis::dvmdb::BundleLimits{1024ULL * 1024ULL, 10000ULL, 1024ULL * 1024ULL});
    Client client(server.port());
    const QString path = "/dvmdb/v1/projects/" + project + "/revision-graph";
    check(client.call("GET", path + "?limit=0").status == 400,
          "graph route accepted zero limit");
    check(client.call("GET", path + "?limit=1001").status == 400,
          "graph route accepted oversized page");
    check(client.call("GET", path + "?cursor=bogus").status == 400,
          "graph route accepted invalid cursor");
    const Response first = client.call("GET", path + "?limit=1");
    check(first.status == 200 && !first.json.value("complete").toBool() &&
          first.json.value("heads").toArray().size() == 3 &&
          first.json.value("revisions").toArray().size() == 1 &&
          first.json.value("nextCursor").isString(), "graph initial response malformed");
    const QString initialCursor = first.json.value("nextCursor").toString();
    check(client.call("GET", "/dvmdb/v1/projects/" + QString::fromStdString(other) +
                      "/revision-graph?cursor=" + initialCursor).status == 400,
          "signed cursor could be replayed into another project");
    QString tampered = initialCursor;
    tampered[3] = tampered.at(3) == QChar('a') ? QChar('b') : QChar('a');
    check(client.call("GET", path + "?cursor=" + tampered).status == 400,
          "tampered graph cursor accepted");
    // Commit after the snapshot was pinned; old pages must still describe
    // the original graph, while a new request shows the advanced main head.
    const aowis::dvmdb::RevisionId later =
        commitRaw(service, project, "main", {{aowis::dvmdb::Key{4}, aowis::dvmdb::Bytes{4}}});
    std::set<QString> seen;
    const std::function<void(const Response&)> record = [&](const Response& response) {
        check(response.status == 200 && response.json.value("heads").toArray().size() == 3,
              "graph paging response malformed");
        // Heads are ordered by branch name: alias, feature, main.
        const QJsonArray heads = response.json.value("heads").toArray();
        check(heads.at(2).toObject().value("revision").toString() == revisionText(merged),
              "graph HEAD snapshot changed after concurrent commit");
        const QJsonArray revisions = response.json.value("revisions").toArray();
        check(revisions.size() == 1, "graph page size incorrect");
        const QString revision = revisions.at(0).toObject().value("id").toString();
        check(seen.insert(revision).second, "duplicate graph revision across HTTP pages");
    };
    record(first);
    QString cursor = initialCursor;
    for (int i = 0; !cursor.isEmpty(); ++i) {
        check(i < 20, "HTTP revision graph cursor loop");
        const Response response = client.call("GET", path + "?limit=1&cursor=" + cursor);
        record(response);
        cursor = response.json.value("nextCursor").toString();
        check(response.json.value("complete").toBool() == cursor.isEmpty(),
              "graph completeness flag disagrees with cursor");
    }
    check(seen.size() == 5 && seen.contains(revisionText(initial)) &&
          !seen.contains(revisionText(later)),
          "HTTP graph omitted ancestry or leaked a later revision");
    const Response updated = client.call("GET", path);
    check(updated.status == 200 && updated.json.value("complete").toBool() &&
          updated.json.value("revisions").toArray().size() == 6 &&
          updated.json.value("heads").toArray().at(2).toObject().value("revision").toString() ==
              revisionText(later),
          "fresh HTTP graph missing advanced branch tip");
}

// Starts the real executable, probes /dvmdb/v1/health on 127.0.0.1, optionally
// runs `whileRunning`, then stops it with SIGTERM. A process that exits on
// its own is reported with its exit code.
ServerRun runServer(const QString& executable, QStringList arguments,
                    const std::function<void(quint16)>& whileRunning = {}) {
    TemporaryRoot root;
    const quint16 port = reservePort();
    arguments << QStringLiteral("--port") << QString::number(port)
              << QStringLiteral("--data-root") << QString::fromStdString(root.root().string());
    QProcess process;
    process.setProgram(executable);
    process.setArguments(arguments);
    process.start();
    check(process.waitForStarted(5000), "database HTTP executable did not start");
    ServerRun run;
    try {
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < 5000 && process.state() != QProcess::NotRunning) {
            RawConnection probe(port);
            if (probe.connected()) {
                run.listened = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (run.listened) {
            Client client(port);
            check(client.call("GET", "/dvmdb/v1/health").status == 200, "executable health failed");
            check(client.call("GET", "/api/v1/health").status == 404,
                  "legacy HTTP route unexpectedly accepted");
            if (whileRunning) whileRunning(port);
            process.terminate(); // SIGTERM
        }
        if (!process.waitForFinished(5000)) {
            process.kill();
            process.waitForFinished(3000);
            throw std::runtime_error("database HTTP executable did not stop on SIGTERM");
        }
    } catch (...) {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(3000);
        }
        throw;
    }
    run.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    run.output = QString::fromLocal8Bit(process.readAllStandardOutput());
    run.diagnostics = QString::fromLocal8Bit(process.readAllStandardError());
    return run;
}

// The real executable: startup, request handling and a clean SIGTERM
// shutdown (exit code 0), plus configuration validation.
void exerciseExecutable(const QString& executable) {
    const ServerRun normal = runServer(executable, {QStringLiteral("--bind"), QStringLiteral("127.0.0.1")},
        [](quint16 port) {
            Client client(port);
            const Response created = client.call("POST", "/dvmdb/v1/projects",
                                                 QJsonObject{{"name", "Shutdown test"}});
            check(created.status == 201 && created.json.value("name").toString() == "Shutdown test",
                  "executable did not create a project");
        });
    check(normal.listened && normal.exitCode == 0, "executable did not shut down cleanly on SIGTERM");
    check(normal.output.contains("AOWIS database listening on 127.0.0.1:"), "startup message missing");

    // All addresses are served without a proxy policy: limits are enforced
    // by the server itself on every platform.
    const ServerRun anyAddress = runServer(executable, {QStringLiteral("--bind"), QStringLiteral("0.0.0.0"),
                                                        QStringLiteral("--worker-threads"), QStringLiteral("2"),
                                                        QStringLiteral("--max-pending-per-project"), QStringLiteral("2")});
    check(anyAddress.listened && anyAddress.exitCode == 0 && !anyAddress.diagnostics.contains("AOWIS database"),
          "non-loopback listener with 2 workers failed");

    const std::vector<QStringList> invalid{
        {QStringLiteral("--bind"), QStringLiteral("localhost")},
        {QStringLiteral("--bind"), QStringLiteral("127.0.0.1"), QStringLiteral("--worker-threads"), QStringLiteral("0")},
        {QStringLiteral("--bind"), QStringLiteral("127.0.0.1"), QStringLiteral("--worker-threads"), QStringLiteral("257")},
        {QStringLiteral("--bind"), QStringLiteral("127.0.0.1"), QStringLiteral("--sync-max-mib"), QStringLiteral("0")},
        {QStringLiteral("--sync-max-mib"), QStringLiteral("32"), QStringLiteral("--sync-max-inflight-mib"), QStringLiteral("16")},
        {QStringLiteral("--sync-max-inflight-mib"), QStringLiteral("4097")},
        {QStringLiteral("--bind"), QStringLiteral("127.0.0.1"), QStringLiteral("--max-pending-per-project"), QStringLiteral("0")},
        {QStringLiteral("--bind"), QStringLiteral("127.0.0.1"), QStringLiteral("--worker-threads"), QStringLiteral("4"),
         QStringLiteral("--max-pending-per-project"), QStringLiteral("5")},
        {QStringLiteral("--bind"), QStringLiteral("127.0.0.1"), QStringLiteral("--max-pending-per-project"), QStringLiteral("x")}};
    for (const QStringList& arguments : invalid) {
        const ServerRun refused = runServer(executable, arguments);
        check(!refused.listened && refused.exitCode == 2, "invalid configuration was accepted");
    }
    TemporaryRoot configuration;
    const std::filesystem::path ini = configuration.root() / "server.ini";
    {
        std::ofstream output(ini);
        output << "[server]\nbind=127.0.0.1\nworker_threads=0\n";
    }
    const ServerRun fromIni = runServer(executable, {QStringLiteral("--config"),
                                                     QString::fromStdString(ini.string())});
    check(!fromIni.listened && fromIni.exitCode == 2, "invalid INI worker_threads accepted");
    {
        std::ofstream output(ini, std::ios::trunc);
        output << "[server]\nbind=127.0.0.1\nworker_threads=2\nmax_pending_per_project=3\n";
    }
    const ServerRun pendingIni = runServer(executable, {QStringLiteral("--config"),
                                                        QString::fromStdString(ini.string())});
    check(!pendingIni.listened && pendingIni.exitCode == 2, "INI max_pending_per_project above workers accepted");
    {
        std::ofstream output(ini, std::ios::trunc);
        output << "[server]\nbind=127.0.0.1\nworker_threads=4\nmax_pending_per_project=4\n";
    }
    const ServerRun validIni = runServer(executable, {QStringLiteral("--config"),
                                                      QString::fromStdString(ini.string())});
    check(validIni.listened && validIni.exitCode == 0, "valid INI max_pending_per_project refused");
}
void exerciseRevisionBundleHttp() {
    namespace native = aowis::dvmdb;
    TemporaryRoot temp;
    const std::filesystem::path sourcePath = temp.root() / "v2-source";
    const std::filesystem::path replicaPath = temp.root() / "v2-replica";
    native::ProjectService source(sourcePath);
    const native::ProjectInfo project = source.createProject("Revision transfer", commitMetadata());
    const std::optional<native::ProjectId> projectId = native::ProjectId::parse(project.id);
    check(projectId.has_value(), "created project is not UUIDv7");
    // Clone the initial, fully published project before either side commits.
    // The source and recipient therefore agree on the project and branch IDs,
    // and only the source later advances its HEAD.
    const std::filesystem::path catalogue = replicaPath / "projects";
    std::filesystem::create_directories(catalogue);
    std::filesystem::copy(sourcePath / "projects" / project.id, catalogue / project.id,
                          std::filesystem::copy_options::recursive);
    native::ProjectService replica(replicaPath);
    const std::optional<native::BranchId> identity = source.branchId(project.id, "main");
    check(identity.has_value() && replica.branchId(project.id, "main") == identity,
          "cloned project lost branch identity");
    native::RevisionId tip = source.head(project.id, "main");
    native::Key key{};
    key[15] = 41;
    source.withRepository(project.id, [&](native::Repository& repository) {
        tip = repository.commit(*projectId, "main", {{key, native::Bytes{1, 2, 3}}}, commitMetadata(), tip);
    });
    const native::BundleLimits limits{4ULL * 1024ULL * 1024ULL, 10000ULL, 1024ULL * 1024ULL};
    TestServer sourceServer(source, limits);
    TestServer replicaServer(replica, limits);
    Client sender(sourceServer.port());
    Client receiver(replicaServer.port());
    const QString prefix = QStringLiteral("/dvmdb/v1/projects/") +
                           QString::fromStdString(project.id) +
                           QStringLiteral("/branches/main/bundle");
    const BinaryResponse full = sender.callRaw("GET", prefix);
    check(full.status == 200 && full.body.startsWith("AOWREV21") &&
          full.contentType == "application/vnd.aowis.revision-bundle-v2",
          "HTTP UUIDv7 full export is not AOWREV21");
    // First-clone path: provision the exact logical project UUID in a
    // different root, then import the verified bundle without copying files.
    const std::filesystem::path freshPath = temp.root() / "v2-http-fresh-clone";
    native::ProjectService fresh(freshPath);
    TestServer freshServer(fresh, limits);
    Client freshClient(freshServer.port());
    const QString clonePath = QStringLiteral("/dvmdb/v1/projects/clone-target");
    const QJsonObject provisionBody{{"projectId", QString::fromStdString(project.id)},
                                    {"name", "Revision transfer"}};
    check(freshClient.call("POST", clonePath, provisionBody).status == 200 &&
          freshClient.call("POST", clonePath, provisionBody).status == 200 &&
          fresh.listBranches(project.id).empty(),
          "first clone failed to provision an empty project with the source UUID");
    check(freshClient.call("POST", clonePath,
          QJsonObject{{"projectId", QString::fromStdString(project.id)},
                      {"name", "Wrong project name"}}).status == 409,
          "clone target silently renamed a provisioned project");
    const BinaryResponse firstImport = freshClient.callRaw(
        "POST", prefix, full.body, "application/vnd.aowis.revision-bundle-v2");
    check(firstImport.status == 201 && fresh.head(project.id, "main") == tip &&
          fresh.branchId(project.id, "main") == identity &&
          fresh.repositoryId() != source.repositoryId(),
          "first HTTP clone failed to preserve logical UUIDs and independent repository root");
    check(sender.callRaw("GET", prefix + "?base=not-a-uuid").status == 400,
          "invalid incremental base accepted");
    check(sender.callRaw("GET", prefix + "?unused=1").status == 400,
          "unknown export query argument accepted");
    const native::ProjectInfo unrelated = replica.createProject("Unrelated project", commitMetadata());
    const QString unrelatedPath = QStringLiteral("/dvmdb/v1/projects/") +
        QString::fromStdString(unrelated.id) + "/branches/main/bundle";
    check(receiver.callRaw("POST", unrelatedPath, full.body,
                           "application/vnd.aowis.revision-bundle-v2").status == 409,
          "cross-project HTTP revision bundle accepted");
    sender.abortDownload(prefix);
    const BinaryResponse advanced = receiver.callRaw(
        "POST", prefix, full.body, "application/vnd.aowis.revision-bundle-v2");
    check(advanced.status == 200 && replica.head(project.id, "main") == tip &&
          replica.branchId(project.id, "main") == identity,
          "HTTP UUIDv7 full import did not preserve HEAD/branch identity");
    check(receiver.callRaw("POST", prefix, full.body,
                           "application/vnd.aowis.revision-bundle-v2").status == 200,
          "HTTP full revision replay failed");
    const native::RevisionId base = tip;
    source.withRepository(project.id, [&](native::Repository& repository) {
        tip = repository.commit(*projectId, "main", {{key, native::Bytes{4, 5, 6}}}, commitMetadata(), base);
    });
    const BinaryResponse delta = sender.callRaw("GET", prefix + "?base=" +
                                              QString::fromStdString(base.toString()));
    check(delta.status == 200 && delta.body.startsWith("AOWREV22"),
          "HTTP UUIDv7 incremental export is not AOWREV22");
    QByteArray damaged = delta.body;
    check(damaged.size() > 144, "incremental fixture unexpectedly empty");
    damaged[damaged.size() - 1] = static_cast<char>(damaged.back() ^ 0x01);
    const BinaryResponse invalid = receiver.callRaw(
        "POST", prefix, damaged, "application/vnd.aowis.revision-bundle-v2");
    check(invalid.status >= 400 && invalid.status < 500 &&
          replica.head(project.id, "main") == base,
          "corrupt HTTP revision delta advanced recipient HEAD");
    const BinaryResponse imported = receiver.callRaw(
        "POST", prefix, delta.body, "application/vnd.aowis.revision-bundle-v2");
    check(imported.status == 200 && replica.head(project.id, "main") == tip &&
          replica.branchId(project.id, "main") == identity,
          "HTTP UUIDv7 incremental import lost identity");
    check(receiver.callRaw("POST", prefix, delta.body,
                           "application/vnd.aowis.revision-bundle-v2").status == 200,
          "HTTP UUIDv7 delta replay failed");
    check(receiver.callRaw("POST", prefix, QByteArray(4 * 1024 * 1024 + 1, 'x'),
                           "application/vnd.aowis.revision-bundle-v2").status == 413,
          "oversized revision upload was accepted");
}


void exerciseUnbornProjectHttp() {
    namespace native = aowis::dvmdb;
    TemporaryRoot temp;
    native::ProjectService service(temp.root());
    TestServer server(service, native::BundleLimits{4ULL * 1024ULL * 1024ULL, 10000, 1024ULL * 1024ULL});
    Client client(server.port());
    const Response repository = client.call("GET", "/dvmdb/v1/repository");
    check(repository.status == 200 && repository.json.keys() == QStringList({"modelCodecFingerprint", "repositoryId"})
          && repository.json.value("repositoryId").toString() ==
              QString::fromStdString(service.repositoryId())
          && repository.json.value("modelCodecFingerprint").toString() ==
              QString::fromLatin1(AOWIS_MODEL_CODEC_FINGERPRINT),
          "HTTP repository identity differs from native root UUID");
    const Response emptyTracking = client.call("GET", "/dvmdb/v1/repository/tracking");
    check(emptyTracking.status == 200 &&
          emptyTracking.json.value("repositoryId") == repository.json.value("repositoryId") &&
          emptyTracking.json.value("modelCodecFingerprint") ==
              repository.json.value("modelCodecFingerprint") &&
          emptyTracking.json.value("role").toString() == QStringLiteral("unconfigured") &&
          emptyTracking.json.value("remotes").toArray().isEmpty() &&
          emptyTracking.json.value("tracking").toArray().isEmpty(),
          "HTTP tracking metadata did not report an unconfigured repository");
    // The same identity discovered by Projects can be configured private
    // without requiring an out-of-band native call or manual UUID entry.
    const QString roleRoute = QStringLiteral("/dvmdb/v1/repository/role");
    const QString localRepositoryId = repository.json.value("repositoryId").toString();
    const QJsonObject privateRole{{"expectedRepositoryId", localRepositoryId}, {"role", "private"}};
    check(client.call("POST", roleRoute, QJsonObject{{"role", "private"}}).status == 400,
          "HTTP role configuration accepted missing expected repository identity");
    check(client.call("POST", roleRoute,
                      QJsonObject{{"expectedRepositoryId", QString::fromStdString(native::UuidV7::generate().toString())},
                                  {"role", "private"}}).status == 409,
          "HTTP role configuration accepted a different repository identity");
    check(client.call("POST", roleRoute,
                      QJsonObject{{"expectedRepositoryId", localRepositoryId},
                                  {"role", "unconfigured"}}).status == 400,
          "HTTP role configuration accepted unconfigured as an explicit role");
    const Response configuredRole = client.call("POST", roleRoute, privateRole);
    check(configuredRole.status == 200 &&
          configuredRole.json.value("repositoryId").toString() == localRepositoryId &&
          configuredRole.json.value("role").toString() == QStringLiteral("private") &&
          service.repositoryRole() == native::RepositoryRole::Private,
          "HTTP could not configure discovered repository as private");
    check(client.call("POST", roleRoute, privateRole).status == 200,
          "HTTP private role configuration is not idempotent");
    check(client.call("POST", roleRoute,
                      QJsonObject{{"expectedRepositoryId", localRepositoryId},
                                  {"role", "shared"}}).status == 409 &&
          service.repositoryRole() == native::RepositoryRole::Private,
          "HTTP role configuration allowed an irreversible role change");
    const std::string remoteId = native::UuidV7::generate().toString();
    const std::string trackedProject = native::ProjectId::generate().toString();
    const std::string trackedBranch = native::BranchId::generate().toString();
    service.configureRepositoryRole(native::RepositoryRole::Private);
    service.registerRemoteRepository(remoteId, "https://shared.example.test/dvmdb/v1");
    service.recordRemoteTrackingRef({remoteId, trackedProject, trackedBranch, std::nullopt},
                                      std::nullopt);
    const Response tracking = client.call("GET", "/dvmdb/v1/repository/tracking");
    const QJsonArray refs = tracking.json.value("tracking").toArray();
    check(tracking.status == 200 &&
          tracking.json.value("role").toString() == QStringLiteral("private") &&
          tracking.json.value("remotes").toArray().size() == 1 && refs.size() == 1 &&
          refs.at(0).toObject().value("remoteRepositoryId").toString() ==
              QString::fromStdString(remoteId) &&
          refs.at(0).toObject().value("projectId").toString() ==
              QString::fromStdString(trackedProject) &&
          refs.at(0).toObject().value("branchId").toString() ==
              QString::fromStdString(trackedBranch) &&
          refs.at(0).toObject().contains("observedHeadRevisionId") &&
          refs.at(0).toObject().value("observedHeadRevisionId").isNull(),
          "HTTP tracking metadata lost explicit unborn HEAD or remote identity");
    const QString projects = QStringLiteral("/dvmdb/v1/projects");
    check(client.call("POST", projects,
        QJsonObject{{"name", "bad unborn"}, {"unborn", "true"}}).status == 400,
        "HTTP accepted non-boolean unborn selector");
    check(client.call("POST", projects,
        QJsonObject{{"name", "null unborn"}, {"unborn", QJsonValue(QJsonValue::Null)}}).status == 400,
        "HTTP accepted null instead of an explicit unborn boolean");
    for (const QJsonObject& request : {
             QJsonObject{{"name", "legacy default"}},
             QJsonObject{{"name", "explicit committed"}, {"unborn", false}}}) {
        const Response legacy = client.call("POST", projects, request);
        check(legacy.status == 201, "HTTP committed project compatibility creation failed");
        const QString legacyHeadPath = projects + "/" + legacy.json.value("id").toString() +
                                       "/branches/main/head";
        const Response legacyHead = client.call("GET", legacyHeadPath);
        check(legacyHead.status == 200 &&
              native::RevisionId::parse(legacyHead.json.value("id").toString().toStdString()).has_value(),
              "HTTP committed project compatibility lost initial revision HEAD");
    }
    const Response created = client.call("POST", projects,
        QJsonObject{{"name", "Empty project"}, {"unborn", true}});
    const std::string project = created.json.value("id").toString().toStdString();
    check(created.status == 201 && native::ProjectId::parse(project).has_value(),
        "HTTP unborn project creation failed");
    // Caller-chosen project id: a resent request returns the same project.
    const QString chosenId = QString::fromStdString(native::ProjectId::generate().toString());
    const QJsonObject chosen{{"name", "Retried project"}, {"unborn", true}, {"id", chosenId}};
    const Response chosenFirst = client.call("POST", projects, chosen);
    const Response chosenAgain = client.call("POST", projects, chosen);
    check(chosenFirst.status == 201 && chosenFirst.json.value("id").toString() == chosenId &&
          chosenAgain.status == 201 && chosenAgain.json == chosenFirst.json &&
          client.call("POST", projects, QJsonObject{{"name", "Other name"}, {"unborn", true},
                                                    {"id", chosenId}}).status == 409 &&
          client.call("POST", projects, QJsonObject{{"name", "Committed"},
                                                    {"id", QString::fromStdString(
                                                        native::ProjectId::generate().toString())}}).status == 400 &&
          client.call("GET", projects + "/" + chosenId + "/branches/main/head").json.value("id").isNull(),
          "caller-chosen project creation is not idempotent or not unborn");
    std::size_t retried = 0;
    for (const native::ProjectInfo& info : service.listProjects())
        retried += info.name == "Retried project" ? 1 : 0;
    check(retried == 1, "repeated project creation published a second project");
    const QString prefix = projects + "/" + QString::fromStdString(project);
    const QString unbornBranchId = QString::fromStdString(native::BranchId::generate().toString());
    const QJsonObject newUnborn{{"name", "independent"}, {"unborn", true}, {"id", unbornBranchId}};
    const Response firstUnborn = client.call("POST", prefix + "/branches", newUnborn);
    const Response retryUnborn = client.call("POST", prefix + "/branches", newUnborn);
    check(firstUnborn.status == 201 && retryUnborn.status == 201 &&
          firstUnborn.json == retryUnborn.json &&
          firstUnborn.json.value("branchId").toString() == unbornBranchId &&
          firstUnborn.json.value("id").isNull() &&
          firstUnborn.json.value("originBranchId").isNull() &&
          client.call("GET", prefix + "/branches/independent/head").json.value("id").isNull(),
          "HTTP independent unborn branch was not created or retried correctly");
    check(client.call("POST", prefix + "/branches",
          QJsonObject{{"name", "independent"}, {"unborn", true}}).status == 409 &&
          client.call("POST", prefix + "/branches",
          QJsonObject{{"name", "invalid"}, {"unborn", true}, {"fromBranchId", unbornBranchId}}).status == 400,
          "HTTP unborn branch accepted duplicate name or origin");
    const QString headPath = prefix + "/branches/main/head";
    const QString historyPath = prefix + "/branches/main/history";
    const QString graphPath = prefix + "/revision-graph";
    const Response unbornHead = client.call("GET", headPath);
    check(unbornHead.status == 200 && unbornHead.json.contains("id") &&
          unbornHead.json.value("id").isNull(), "HTTP unborn HEAD is not explicit null");
    const Response history = client.call("GET", historyPath);
    check(history.status == 200 && history.json.value("revisions").toArray().isEmpty(),
          "HTTP unborn history must be empty");
    const Response graph = client.call("GET", graphPath + "?limit=1");
    const QJsonArray heads = graph.json.value("heads").toArray();
    check(graph.status == 200 && heads.size() == 2 &&
          heads.at(1).toObject().value("branch") == "main" &&
          heads.at(1).toObject().value("revision").isNull() &&
          heads.at(1).toObject().value("recordHash").isNull() &&
          native::BranchId::parse(heads.at(1).toObject().value("branchId").toString().toStdString()) &&
          graph.json.value("revisions").toArray().isEmpty() &&
          graph.json.value("complete").toBool() && graph.json.value("nextCursor").isNull(),
          "HTTP unborn graph must include both independent null HEADs");
    const std::optional<native::ProjectId> id = native::ProjectId::parse(project);
    native::Key key{};
    key[15] = 9;
    std::optional<native::RevisionId> first;
    service.withRepository(project, [&](native::Repository& repository) {
        first = repository.commit(*id, "main", {{key, native::Bytes{1, 2, 3}}}, commitMetadata(), std::nullopt);
    });
    check(first.has_value(), "HTTP first unborn commit returned no revision");
    const QString firstId = QString::fromStdString(first->toString());
    check(client.call("GET", headPath).json.value("id").toString() == firstId,
          "HTTP HEAD failed to transition from null after first commit");
    const Response firstRevision = client.call("GET", prefix + "/revisions/" + firstId);
    check(firstRevision.status == 200 &&
          firstRevision.json.value("parents").toArray().isEmpty(),
          "HTTP first unborn commit must have no parents");
    const Response after = client.call("GET", graphPath);
    check(after.status == 200 && after.json.value("heads").toArray().size() == 2 &&
          after.json.value("heads").toArray().at(1).toObject().value("revision") == firstId &&
          after.json.value("revisions").toArray().size() == 1,
          "HTTP graph did not expose first committed revision");
}

void exerciseUnbornCheckoutHttp() {
    namespace native = aowis::dvmdb;
    TemporaryRoot sourceRoot;
    TemporaryRoot targetRoot;
    native::ProjectService source(sourceRoot.root());
    const native::ProjectInfo project = source.createUnbornProject("Unborn network");
    const std::optional<native::BranchId> sourceBranch = source.branchId(project.id, "main");
    check(sourceBranch.has_value() && !source.optionalHead(project.id, "main"),
          "unborn source has no stable branch ID or has a revision");
    native::ProjectService target(targetRoot.root());
    target.configureRepositoryRole(native::RepositoryRole::Private);
    TestServer targetServer(target, native::BundleLimits{4ULL * 1024ULL * 1024ULL, 10000, 1024ULL * 1024ULL});
    Client client(targetServer.port());
    const QString prefix = QStringLiteral("/dvmdb/v1/projects/") + QString::fromStdString(project.id);
    const QString path = prefix + QStringLiteral("/unborn-checkout");
    const QString identity = QString::fromStdString(sourceBranch->toString());
    const QJsonObject checkout{{"branchId", identity}, {"name", "main"}};
    check(client.call("POST", path, checkout).status == 404,
          "unborn checkout silently created a missing project");
    check(client.call("POST", "/dvmdb/v1/projects/clone-target",
          QJsonObject{{"projectId", QString::fromStdString(project.id)},
                      {"name", "Unborn network"}}).status == 200,
          "unborn checkout clone target provisioning failed");
    const Response first = client.call("POST", path, checkout);
    const Response retry = client.call("POST", path, checkout);
    check(first.status == 200 && retry.status == 200 && first.json == retry.json &&
          first.json.value("branchId") == identity && first.json.value("revision").isNull() &&
          target.branchId(project.id, "main") == sourceBranch &&
          !target.optionalHead(project.id, "main"),
          "unborn checkout lost source branch UUID, null HEAD, or retry safety");
    check(client.call("POST", path,
          QJsonObject{{"branchId", identity}, {"name", "renamed"}}).status == 409,
          "unborn checkout reused an identity for a different branch");
    check(client.call("POST", path,
          QJsonObject{{"branchId", QString::fromStdString(native::BranchId::generate().toString())},
                      {"name", "main"}}).status == 409,
          "unborn checkout silently replaced an existing branch identity");
    check(client.call("POST", path,
          QJsonObject{{"branchId", "not-uuid"}, {"name", "main"}}).status == 400,
          "unborn checkout accepted a malformed branch UUID");
    const QString session = QString::fromStdString(native::UuidV7::generate().toString());
    const QString draftId = QString::fromStdString(native::UuidV7::generate().toString());
    const Response draft = client.call("POST", "/dvmdb/v1/working-copies",
          QJsonObject{{"projectId", QString::fromStdString(project.id)},
                      {"branchId", identity}, {"baseRevision", QJsonValue(QJsonValue::Null)},
                      {"sessionId", session}, {"id", draftId}});
    check(draft.status == 201 && draft.json.value("id") == draftId &&
          draft.json.value("baseRevision").isNull() &&
          target.workingCopy(draftId.toStdString()).baseRevision == std::nullopt &&
          !target.optionalHead(project.id, "main"),
          "unborn checkout could not create a durable null-base working copy");
    targetServer.stop();
}

void exerciseUuidv7HistoryHttp() {
    namespace native = aowis::dvmdb;
    TemporaryRoot temp;
    native::ProjectService service(temp.root());
    TestServer server(service, native::BundleLimits{4ULL * 1024ULL * 1024ULL, 10000, 1024ULL * 1024ULL});
    Client client(server.port());
    check(client.call("POST", "/dvmdb/v1/projects",
                      QJsonObject{{"name", "invalid"}, {"revisionFormat", "sha"}}).status == 400,
          "unexpected revision format accepted");
    check(client.call("POST", "/dvmdb/v1/projects",
                      QJsonObject{{"name", "format override"}, {"revisionFormat", "uuidv7"}}).status == 400,
          "canonical UUIDv7 project creation accepted obsolete revisionFormat selector");
    const Response created = client.call("POST", "/dvmdb/v1/projects",
        QJsonObject{{"name", "API"}});
    const std::string project = created.json.value("id").toString().toStdString();
    check(created.status == 201 && native::ProjectId::parse(project).has_value(),
          "HTTP did not create UUIDv7 project");
    const QString basePath = QStringLiteral("/dvmdb/v1/projects/") +
                             QString::fromStdString(project);
    const QString graphPath = basePath + "/revision-graph";
    const QString mainHeadPath = basePath + "/branches/main/head";
    const Response head = client.call("GET", mainHeadPath);
    check(head.status == 200 && native::RevisionId::parse(head.json.value("id").toString().toStdString()),
          "HTTP branch HEAD is not UUIDv7");
    const QString rootId = head.json.value("id").toString();
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
    // Retired direct-to-branch write routes must not publish a network
    // without annotation ownership, or bypass the MODEL History journal.
    const QString obsoleteNetworkPost = basePath + "/branches/main/networks";
    const QString obsoleteEntityPost = basePath + "/branches/main/networks/" +
        QString::fromStdString(native::ProjectId::generate().toString()) + "/edits";
    for (const QString& route : {obsoleteNetworkPost, obsoleteEntityPost}) {
        const Response rejected = client.call("POST", route,
            QJsonObject{{"expectedHead", rootId}, {"payloadBase64", ""}});
        check(rejected.status == 410 &&
              rejected.json.value("error").toObject().value("code") ==
                  "legacy_hydraulic_write_retired",
              "retired direct hydraulic branch write is still reachable");
        check(client.call("GET", mainHeadPath).json.value("id").toString() == rootId,
              "retired hydraulic write changed immutable branch HEAD");
    }
#endif
    check(client.call("GET", basePath + "/branches/main/head-v2").status == 404,
          "obsolete HEAD alias still registered");
    check(client.call("GET", basePath + "/revisions/" + QString(64, '0')).status == 400,
          "canonical revision lookup accepted a SHA-256 revision identity");
    check(client.call("GET", basePath + "/revisions/garbage").status == 400,
          "HTTP revision lookup accepted non-UUID identity");
    const Response initial = client.call("GET", basePath + "/revisions/" + rootId);
    check(initial.status == 200 && initial.json.value("recordHash").toString().size() == 64 &&
          initial.json.value("parents").toArray().isEmpty(),
          "HTTP root detail is invalid");
    // Branches are addressed by UUIDv7 (DG-01): read main's identity first.
    const std::function<QString(const QString&)> mainBranchIdOf = [&client](const QString& path) {
        for (const QJsonValue& value : client.call("GET", path + "/revision-graph?limit=1")
                                           .json.value("heads").toArray()) {
            const QJsonObject item = value.toObject();
            if (item.value("branch").toString() == QStringLiteral("main"))
                return item.value("branchId").toString();
        }
        return QString();
    };
    const QString mainBranchId = mainBranchIdOf(basePath);
    check(native::BranchId::parse(mainBranchId.toStdString()).has_value(),
          "HTTP initial graph did not expose the main branch identity");
    check(client.call("POST", basePath + "/branches",
        QJsonObject{{"name", "by-name"}, {"fromBranch", "main"}}).status == 400,
          "branch creation still accepted a source branch name");
    const QString featureId = QString::fromStdString(native::BranchId::generate().toString());
    const QJsonObject featureRequest{{"name", "feature"}, {"fromBranchId", mainBranchId},
                                     {"id", featureId}};
    const Response feature = client.call("POST", basePath + "/branches", featureRequest);
    // A resent request (lost response, transport retry) gets the original
    // answer instead of "branch already exists".
    const Response featureAgain = client.call("POST", basePath + "/branches", featureRequest);
    check(featureAgain.status == 201 && featureAgain.json == feature.json,
          "repeated branch creation with the same id was not idempotent");
    QJsonObject featureMisuse = featureRequest;
    featureMisuse.insert("name", "feature-two");
    check(client.call("POST", basePath + "/branches", featureMisuse).status == 409,
          "branch id reused for a different branch was accepted");
    check(client.call("POST", basePath + "/branches",
        QJsonObject{{"name", "ghost"}, {"fromBranchId",
            QString::fromStdString(native::BranchId::generate().toString())}}).status == 404,
          "unknown source branch UUIDv7 was not reported as missing");
    const Response featureById = client.call("GET", basePath + "/branch-ids/" + featureId);
    check(featureById.status == 200 && featureById.json.value("branch") == "feature" &&
          featureById.json.value("branchId").toString() == featureId &&
          featureById.json.value("revision").toString() == rootId &&
          featureById.json.value("recordHash").toString().size() == 64 &&
          featureById.json.value("originBranchId").toString() == mainBranchId &&
          featureById.json.value("forkRevisionId").toString() == rootId &&
          client.call("GET", basePath + "/branch-ids/" +
              QString::fromStdString(native::BranchId::generate().toString())).status == 404,
          "branch lookup by UUIDv7 is incomplete");
    check(feature.status == 201 && feature.json.value("id").toString() == rootId &&
          feature.json.value("branchId").toString() == featureId &&
          native::BranchId::parse(feature.json.value("branchId").toString().toStdString()) &&
          feature.json.value("originBranchId").toString() == mainBranchId &&
          feature.json.value("forkRevisionId").toString() == rootId,
          "HTTP branch creation did not return persisted fork provenance");
    check(client.call("POST", basePath + "/branches",
        QJsonObject{{"name", "orphan"}}).status == 400,
          "source-less fork allowed for nonempty project");
    check(client.call("POST", basePath + "/branches",
        QJsonObject{{"name", "bad"}, {"fromBranchId", mainBranchId}, {"revision", "1234"}}).status == 400,
          "invalid historical UUID was accepted");
    const std::optional<native::ProjectId> id = native::ProjectId::parse(project);
    native::RevisionId mainTip = *native::RevisionId::parse(rootId.toStdString());
    native::Key first{};
    first[15] = 7;
    service.withRepository(project, [&](native::Repository& repository) {
        mainTip = repository.commit(*id, "main", {{first, native::Bytes{1, 2}}}, commitMetadata(), mainTip);
    });
    const QString newer = QString::fromStdString(mainTip.toString());
    const Response graphFirst = client.call("GET", graphPath + "?limit=1");
    check(graphFirst.status == 200 && graphFirst.json.value("heads").toArray().size() == 2 &&
          graphFirst.json.value("revisions").toArray().size() == 1 &&
          !graphFirst.json.value("complete").toBool(), "UUIDv7 graph first page malformed");
    const QString cursor = graphFirst.json.value("nextCursor").toString();
    check(!cursor.isEmpty(), "UUIDv7 graph did not return pagination cursor");
    const QString differentProject = QString::fromStdString(service.createProject("Other", commitMetadata()).id);
    check(client.call("GET", "/dvmdb/v1/projects/" + differentProject +
                      "/revision-graph?cursor=" + cursor).status == 400,
          "UUIDv7 graph cursor was accepted across projects");
    QString changedCursor = cursor;
    changedCursor[changedCursor.size() - 2] = changedCursor.at(changedCursor.size() - 2) == QChar('A')
        ? QChar('B') : QChar('A');
    check(client.call("GET", graphPath + "?cursor=" + changedCursor).status == 400,
          "forged UUIDv7 graph cursor was accepted");
    // Advance main during paging; the pinned first page must not be rewritten.
    service.withRepository(project, [&](native::Repository& repository) {
        mainTip = repository.commit(*id, "main", {{first, native::Bytes{3, 4}}}, commitMetadata(), mainTip);
    });
    const Response graphSecond = client.call("GET", graphPath + "?limit=1&cursor=" + cursor);
    check(graphSecond.status == 200 && graphSecond.json.value("heads") == graphFirst.json.value("heads") &&
          graphSecond.json.value("complete").toBool() &&
          graphSecond.json.value("revisions").toArray().size() == 1,
          "UUIDv7 graph cursor failed to preserve a pinned HEAD snapshot");
    const Response latest = client.call("GET", graphPath);
    check(latest.status == 200 && latest.json.value("revisions").toArray().size() == 3 &&
          latest.json.value("heads").toArray().size() == 2,
          "UUIDv7 graph did not enumerate updated ancestry");
    const Response history = client.call("GET", basePath + "/branches/main/history?limit=1");
    check(history.status == 200 && history.json.value("revisions").toArray().size() == 1 &&
          history.json.value("revisions").toArray().at(0).toObject().value("id").toString() ==
          QString::fromStdString(mainTip.toString()), "history did not begin at current HEAD");
    const QJsonObject historicRequest{{"name", "historic"}, {"fromBranchId", mainBranchId},
        {"revision", rootId}, {"id", QString::fromStdString(native::BranchId::generate().toString())}};
    const Response historic = client.call("POST", basePath + "/branches", historicRequest);
    check(client.call("POST", basePath + "/branches", historicRequest).json == historic.json,
          "repeated historical branch creation was not idempotent");
    check(historic.status == 201 && historic.json.value("id").toString() == rootId &&
          native::BranchId::parse(historic.json.value("branchId").toString().toStdString()) &&
          historic.json.value("originBranchId").toString() == mainBranchId &&
          historic.json.value("forkRevisionId").toString() == rootId,
          "historical branching did not return exact persisted fork provenance");
    const Response renamed = client.call("PATCH", basePath + "/branches/historic",
        QJsonObject{{"name", "archived"}, {"expectedHead", rootId}});
    check(renamed.status == 200 &&
          client.call("GET", basePath + "/branches/archived/head").json.value("id").toString() == rootId,
          "rename moved HEAD or lost branch");
    const QString historicId = historic.json.value("branchId").toString();
    check(client.call("GET", basePath + "/branch-ids/" + historicId).json.value("branch") == "archived",
          "branch lookup by UUIDv7 did not follow the rename");
    check(client.call("PATCH", basePath + "/branches/archived",
        QJsonObject{{"name", "bad-again"}, {"expectedHead", newer}}).status == 409,
          "stale rename did not conflict");

    // A separate project starts with an empty root, so the HTTP model
    // operations are exercised against actual UUIDv7 HEADs rather than
    // the SHA-only model API or a fixture-created raw revision.
    const Response modelProject = client.call("POST", "/dvmdb/v1/projects",
        QJsonObject{{"name", "REST commits"}});
    check(modelProject.status == 201, "model test project creation failed");
    const QString modelPath = QStringLiteral("/dvmdb/v1/projects/") +
                              modelProject.json.value("id").toString();
    const QString modelRoot = client.call("GET", modelPath + "/branches/main/head")
                              .json.value("id").toString();
    const QString modelMainId = mainBranchIdOf(modelPath);
    const Response modelFeature = client.call("POST", modelPath + "/branches",
        QJsonObject{{"name", "feature"}, {"fromBranchId", modelMainId}});
    check(modelFeature.status == 201 && modelFeature.json.value("id").toString() == modelRoot,
          "model feature branch creation failed");
    const QString keyA = QStringLiteral("00000000-0000-0000-0000-000000000007");
    const QString keyB = QStringLiteral("00000000-0000-0000-0000-000000000008");
    const QString mainCommitPath = modelPath + "/branches/main/model-commits";
    const QString featureCommitPath = modelPath + "/branches/feature/model-commits";
    const QJsonObject addA{{"uuid", keyA}, {"kind", 31},
                           {"references", QJsonArray{}}, {"payloadBase64", "Cw=="}};
    const QJsonObject addB{{"uuid", keyB}, {"kind", 31},
                           {"references", QJsonArray{}}, {"payloadBase64", "Fg=="}};
    const QJsonObject firstCommit{{"expectedHead", modelRoot},
                                  {"changes", QJsonArray{addA}}};
    check(client.call("POST", mainCommitPath,
        QJsonObject{{"expectedHead", modelRoot}, {"changes", QJsonArray{}}}).status == 400,
        "REST accepted empty model change batch");
    check(client.call("POST", mainCommitPath,
        QJsonObject{{"expectedHead", modelRoot}, {"changes", QJsonArray{
            QJsonObject{{"uuid", keyA}, {"kind", 31}, {"references", QJsonArray{}},
                        {"payloadBase64", "invalid***"}}}}}).status == 400,
        "REST accepted invalid model base64");
    // The generic MODEL endpoint must preserve opaque record commits, while
    // still rejecting a native annotation record with no owning layer. The
    // failed engineering candidate must leave HEAD untouched.
    const Response orphanAnnotation = client.call("POST", mainCommitPath,
        QJsonObject{{"expectedHead", modelRoot}, {"changes", QJsonArray{
            QJsonObject{{"uuid", "00000000-0000-0000-0000-000000000009"},
                        {"kind", 112}, {"references", QJsonArray{}},
                        {"payloadBase64", "Cw=="}}}}});
    check(orphanAnnotation.status == 400 &&
          client.call("GET", modelPath + "/branches/main/head")
              .json.value("id").toString() == modelRoot,
          "REST generic MODEL Commit bypassed orphan annotation validation");
    const Response modelFirst = client.call("POST", mainCommitPath, firstCommit);
    const QString modelFirstId = modelFirst.json.value("id").toString();
    check(modelFirst.status == 201 && native::RevisionId::parse(modelFirstId.toStdString()),
          "REST model commit did not create revision UUIDv7");
    check(client.call("POST", mainCommitPath, firstCommit).status == 409,
          "REST accepted stale model expectedHead");
    const Response modelDiff = client.call("GET", modelPath + "/diff?from=" + modelRoot +
                                            "&to=" + modelFirstId);
    check(modelDiff.status == 200 && modelDiff.json.value("changes").toArray().size() == 1 &&
          modelDiff.json.value("changes").toArray().at(0).toObject().value("uuid").toString() == keyA,
          "REST immutable diff did not include added model");
    check(client.call("GET", modelPath + "/diff?from=garbage&to=" + modelFirstId).status == 400,
          "REST immutable diff accepted non-UUID identity");
    const Response modelSecond = client.call("POST", featureCommitPath,
        QJsonObject{{"expectedHead", modelRoot}, {"changes", QJsonArray{addB}}});
    const QString featureCommitId = modelSecond.json.value("id").toString();
    check(modelSecond.status == 201 && native::RevisionId::parse(featureCommitId.toStdString()),
          "REST model branch commit failed");
    const QString mergePath = modelPath + "/merges";
    const QJsonObject mergeRequest{{"target", "main"}, {"source", "feature"},
        {"expectedTargetHead", modelFirstId}, {"expectedSourceHead", featureCommitId}};
    check(client.call("POST", mergePath,
        QJsonObject{{"target", "main"}, {"source", "feature"},
                    {"expectedTargetHead", modelRoot}, {"expectedSourceHead", featureCommitId}}).status == 409,
          "REST merge accepted stale target HEAD");
    const Response merged = client.call("POST", mergePath, mergeRequest);
    const QString mergedId = merged.json.value("head").toString();
    check(merged.status == 200 && merged.json.value("status").toString() == "merged" &&
          native::RevisionId::parse(mergedId.toStdString()),
          "REST disjoint model merge did not publish UUIDv7 merge revision");
    const Response mergeRecord = client.call("GET", modelPath + "/revisions/" + mergedId);
    check(mergeRecord.status == 200 && mergeRecord.json.value("parents").toArray().size() == 2,
          "REST merge revision did not retain both parents");
    check(client.call("GET", modelPath + "/branches/feature/head")
           .json.value("id").toString() == featureCommitId,
          "REST merge unexpectedly advanced source branch HEAD");
    const Response mergedDiff = client.call("GET", modelPath + "/diff?from=" +
                                              modelFirstId + "&to=" + mergedId);
    check(mergedDiff.status == 200 && mergedDiff.json.value("changes").toArray().size() == 1 &&
          mergedDiff.json.value("changes").toArray().at(0).toObject().value("uuid").toString() == keyB,
          "REST merged revision diff omitted source change");
    const Response otherBranch = client.call("POST", modelPath + "/branches",
        QJsonObject{{"name", "competing"}, {"fromBranchId", modelMainId}});
    check(otherBranch.status == 201 && otherBranch.json.value("id").toString() == mergedId,
          "REST conflict test branch creation failed");
    const QJsonObject overwriteA{{"uuid", keyA}, {"kind", 31},
        {"references", QJsonArray{}}, {"payloadBase64", "Mw=="}};
    const QJsonObject overwriteOther{{"uuid", keyA}, {"kind", 31},
        {"references", QJsonArray{}}, {"payloadBase64", "RA=="}};
    const Response mainAdvanced = client.call("POST", mainCommitPath,
        QJsonObject{{"expectedHead", mergedId}, {"changes", QJsonArray{overwriteA}}});
    const Response otherAdvanced = client.call("POST",
        modelPath + "/branches/competing/model-commits",
        QJsonObject{{"expectedHead", mergedId}, {"changes", QJsonArray{overwriteOther}}});
    const QString mainAdvancedId = mainAdvanced.json.value("id").toString();
    const QString otherAdvancedId = otherAdvanced.json.value("id").toString();
    check(mainAdvanced.status == 201 && otherAdvanced.status == 201,
          "REST conflict fixture model commits failed");
    const Response conflict = client.call("POST", mergePath,
        QJsonObject{{"target", "main"}, {"source", "competing"},
                    {"expectedTargetHead", mainAdvancedId},
                    {"expectedSourceHead", otherAdvancedId}});
    check(conflict.status == 409 && conflict.json.value("status").toString() == "conflicts" &&
          conflict.json.value("conflicts").toArray().size() == 1,
          "REST concurrent model conflict not reported");
    check(client.call("GET", modelPath + "/branches/main/head")
                .json.value("id").toString() == mainAdvancedId &&
          client.call("GET", modelPath + "/branches/competing/head")
                .json.value("id").toString() == otherAdvancedId,
          "REST conflict changed a branch HEAD");
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
    // The hydraulic merge endpoint must apply its domain validator, not
    // silently accept generic, non-hydraulic indexed models as networks.
    const QString hydraulicMergePath = modelPath + "/hydraulic-merges";
    const QJsonObject hydraulicRequest{{"target", "main"}, {"source", "competing"},
        {"expectedTargetHead", mainAdvancedId}, {"expectedSourceHead", otherAdvancedId}};
    const Response hydraulicConflict = client.call("POST", hydraulicMergePath, hydraulicRequest);
    check(hydraulicConflict.status == 409 &&
          hydraulicConflict.json.value("status").toString() == "conflicts",
          "UUIDv7 hydraulic REST merge failed to report conflicts");
    const Response invalidHydraulicUuid = client.call("POST", hydraulicMergePath,
        QJsonObject{{"target", "main"}, {"source", "competing"},
                    {"expectedTargetHead", "not-a-uuid"},
                    {"expectedSourceHead", otherAdvancedId}});
    check(invalidHydraulicUuid.status == 400,
          "UUIDv7 hydraulic REST merge accepted an invalid revision UUID");
    const Response duplicateObjects = client.call("POST", hydraulicMergePath,
        QJsonObject{{"target", "main"}, {"source", "competing"},
                    {"expectedTargetHead", mainAdvancedId},
                    {"expectedSourceHead", otherAdvancedId},
                    {"objectChoices", QJsonArray{
                        QJsonObject{{"entityUuid", keyA}, {"side", "source"}},
                        QJsonObject{{"entityUuid", keyA}, {"side", "target"}}}}});
    check(duplicateObjects.status == 400,
          "UUIDv7 hydraulic REST merge accepted contradictory object choices");
    const Response domainRejected = client.call("POST", hydraulicMergePath,
        QJsonObject{{"target", "main"}, {"source", "competing"},
                    {"expectedTargetHead", mainAdvancedId},
                    {"expectedSourceHead", otherAdvancedId},
                    {"objectChoices", QJsonArray{
                        QJsonObject{{"entityUuid", keyA}, {"side", "source"}}}}});
    check(domainRejected.status == 409 && domainRejected.json.value("status") == "conflicts",
          "UUIDv7 hydraulic merge skipped whole-network domain validation");
    check(client.call("GET", modelPath + "/branches/main/head")
              .json.value("id").toString() == mainAdvancedId &&
          client.call("GET", modelPath + "/branches/competing/head")
              .json.value("id").toString() == otherAdvancedId,
          "rejected hydraulic REST merge advanced a branch HEAD");
#endif



}


void exerciseWorkingCopyHttp() {
    namespace native = aowis::dvmdb;
    TemporaryRoot root;
    std::string projectId;
    std::string draftId;
    QString encodedEvent;
    const QString firstSession = QString::fromStdString(native::UuidV7::generate().toString());
    const QString otherSession = QString::fromStdString(native::UuidV7::generate().toString());
    // MODEL edit events require UUIDv7 entity IDs, so the edited model record uses one.
    const QString modelUuid = QString::fromStdString(native::UuidV7::generate().toString());
    {
        native::ProjectService service(root.root());
        const native::ProjectInfo project = service.createUnbornProject("private working-copy HTTP");
        projectId = project.id;
        TestServer server(service, native::BundleLimits{4ULL * 1024ULL * 1024ULL,
                                                         10000ULL, 1024ULL * 1024ULL});
        Client client(server.port());
        const QString listPath = QStringLiteral("/dvmdb/v1/working-copies");
        const QString mainId = QString::fromStdString(
            service.branchId(projectId, "main")->toString());
        const QString chosenDraft = QString::fromStdString(native::UuidV7::generate().toString());
        const QJsonObject create{{"projectId", QString::fromStdString(projectId)},
                                 {"branchId", mainId}, {"baseRevision", QJsonValue::Null},
                                 {"sessionId", firstSession}, {"id", chosenDraft}};
        check(client.call("POST", listPath, create).status == 409,
              "non-private repository accepted draft creation");
        service.configureRepositoryRole(native::RepositoryRole::Private);
        check(client.call("POST", listPath,
              QJsonObject{{"projectId", QString::fromStdString(projectId)},
                          {"branchId", mainId}, {"sessionId", firstSession}}).status == 400,
              "draft creation accepted missing explicit unborn base");
        check(client.call("POST", listPath,
              QJsonObject{{"projectId", QString::fromStdString(projectId)}, {"branch", "main"},
                          {"baseRevision", QJsonValue::Null}, {"sessionId", firstSession}}).status == 400,
              "draft creation still accepted a branch name");
        const Response created = client.call("POST", listPath, create);
        const QString id = created.json.value("id").toString();
        // A resent creation returns the same draft; a different request may
        // not reuse its id.
        const Response createdAgain = client.call("POST", listPath, create);
        QJsonObject misuse = create;
        misuse.insert("sessionId", otherSession);
        check(createdAgain.status == 201 && createdAgain.json == created.json &&
              client.call("POST", listPath, misuse).status == 409,
              "draft creation with a caller-chosen id is not idempotent");
        check(created.status == 201 && id == chosenDraft &&
              created.json.value("baseRevision").isNull() &&
              created.json.value("generation") == QStringLiteral("0") &&
              !created.json.value("dirty").toBool(),
              "HTTP unborn draft creation failed");
        draftId = id.toStdString();
        const QString detailPath = listPath + "/" + id;
        check(client.call("GET", listPath).json.value("workingCopies").toArray().size() == 1 &&
              client.call("GET", listPath).json.value("workingCopies").toArray().at(0).toObject()
                  .value("changes").isUndefined() &&
              client.call("GET", detailPath).json.value("id") == id &&
              client.call("GET", listPath + "/not-a-uuid").status == 400,
              "HTTP working-copy list/read/UUID validation failed");
        const QString leasePath = detailPath + "/lease";
        const Response leased = client.call("POST", leasePath,
            QJsonObject{{"sessionId", firstSession}, {"seconds", 30}});
        const QString token = leased.json.value("token").toString();
        check(leased.status == 200 && native::UuidV7::parse(token.toStdString()).has_value() &&
              leased.json.value("generation") == QStringLiteral("0") &&
              !leased.json.value("expiresAtMs").toString().isEmpty() &&
              client.call("POST", leasePath,
                  QJsonObject{{"sessionId", otherSession}, {"seconds", 30}}).status == 409,
              "HTTP draft writer lease was not exclusive");
        const QJsonObject entity{{"uuid", modelUuid}, {"kind", 4},
                                 {"references", QJsonArray{}}, {"payloadBase64", "AQID"}};
        const QString savePath = detailPath + "/save";
        const QJsonObject save{{"sessionId", firstSession}, {"fencingToken", token},
                               {"expectedGeneration", "0"}, {"changes", QJsonArray{entity}}};
        check(client.call("POST", savePath,
            QJsonObject{{"sessionId", firstSession}, {"fencingToken", token},
                        {"expectedGeneration", 0}, {"changes", QJsonArray{entity}}}).status == 400,
              "HTTP draft accepted unsafe numeric generation");
        check(client.call("POST", savePath,
            QJsonObject{{"sessionId", firstSession}, {"fencingToken", token},
                        {"expectedGeneration", "0"}, {"changes", QJsonArray{entity, entity}}}).status == 400,
              "HTTP draft accepted duplicate model UUIDs in one save");
        const Response persisted = client.call("POST", savePath, save);
        const QJsonArray persistedChanges = persisted.json.value("changes").toArray();
        check(persisted.status == 200 && persisted.json.value("generation") == QStringLiteral("1") &&
              persisted.json.value("dirty") == QJsonValue(true) && persistedChanges.size() == 1 &&
              persistedChanges.at(0).toObject().value("uuid") == modelUuid &&
              persistedChanges.at(0).toObject().value("payloadBase64") == QStringLiteral("AQID"),
              "HTTP draft save did not retain opaque model payload");
        check(client.call("POST", savePath, save).status == 409,
              "HTTP accepted stale draft generation");
        check(client.call("POST", savePath,
              QJsonObject{{"sessionId", firstSession}, {"fencingToken", otherSession},
                          {"expectedGeneration", "1"}, {"changes", QJsonArray{entity}}}).status == 409,
              "HTTP accepted invalid fencing token");
        check(client.call("GET", detailPath).json.value("generation") == QStringLiteral("1") &&
              client.call("GET", detailPath).json.value("changes").toArray().size() == 1 &&
              !client.call("GET", detailPath).json.contains("token") &&
              !service.optionalHead(projectId, "main").has_value() &&
              service.revisionGraph(projectId).revisions.empty(),
              "HTTP draft save changed committed HEAD or lost candidate");
        const Response released = client.call("POST", detailPath + "/release",
            QJsonObject{{"sessionId", firstSession}, {"fencingToken", token}});
        // Releasing again (a resent request) is a no-op success.
        check(released.status == 200 && released.json.value("released") == QJsonValue(true) &&
              client.call("POST", detailPath + "/release",
                  QJsonObject{{"sessionId", firstSession}, {"fencingToken", token}}).status == 200,
              "HTTP lease release is not idempotent");
        const Response taken = client.call("POST", leasePath,
            QJsonObject{{"sessionId", otherSession}, {"seconds", 30}});
        check(taken.status == 200 && taken.json.value("token") != token &&
              taken.json.value("generation") == QStringLiteral("1") &&
              client.call("POST", savePath,
                  QJsonObject{{"sessionId", firstSession}, {"fencingToken", token},
                              {"expectedGeneration", "1"}, {"changes", QJsonArray{entity}}}).status == 409 &&
              client.call("POST", detailPath + "/release",
                  QJsonObject{{"sessionId", firstSession}, {"fencingToken", token}}).status == 409,
              "HTTP lease takeover did not fence original session");
        // The event and the candidate agree: the saved entity changes from
        // {1,2,3} (generation 1) to {4,5,6}.
        native::Key edited{};
        const QByteArray editedBytes = QUuid(modelUuid).toRfc4122();
        for (std::size_t i = 0; i < edited.size(); ++i)
            edited[i] = static_cast<std::uint8_t>(editedBytes.at(static_cast<qsizetype>(i)));
        const QJsonObject changedEntity{{"uuid", modelUuid}, {"kind", 4},
                                        {"references", QJsonArray{}}, {"payloadBase64", "BAUG"}};
        const native::Bytes event = native::test::editEvent(
            native::UuidV7::generate(), edited, native::Bytes{1, 2, 3}, native::Bytes{4, 5, 6});
        const QByteArray eventBytes(reinterpret_cast<const char*>(event.data()),
                                    static_cast<qsizetype>(event.size()));
        encodedEvent = QString::fromLatin1(eventBytes.toBase64());
        const QString eventSavePath = detailPath + "/save-events";
        const QJsonObject eventSave{{"sessionId", otherSession},
                                    {"fencingToken", taken.json.value("token")},
                                    {"expectedGeneration", "1"},
                                    {"changes", QJsonArray{changedEntity}},
                                    {"events", QJsonArray{encodedEvent}}};
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
        // R8: the journal must describe exactly the saved candidate.
        const native::Bytes wrongBefore = native::test::editEvent(
            native::UuidV7::generate(), edited, native::Bytes{9}, native::Bytes{4, 5, 6});
        const native::Bytes wrongAfter = native::test::editEvent(
            native::UuidV7::generate(), edited, native::Bytes{1, 2, 3}, native::Bytes{7});
        for (const native::Bytes& inconsistent : {wrongBefore, wrongAfter}) {
            QJsonObject mismatched = eventSave;
            mismatched.insert("events", QJsonArray{QString::fromLatin1(QByteArray(
                reinterpret_cast<const char*>(inconsistent.data()),
                static_cast<qsizetype>(inconsistent.size())).toBase64())});
            check(client.call("POST", eventSavePath, mismatched).status == 400 &&
                  client.call("GET", detailPath).json.value("generation") == QStringLiteral("1"),
                  "HTTP event save accepted a journal that disagrees with the candidate");
        }
        QJsonObject unrelated = eventSave;
        unrelated.insert("changes", QJsonArray{entity});
        check(client.call("POST", eventSavePath, unrelated).status == 400,
              "HTTP event save accepted a candidate that is not the MODEL after-image");
#endif
        const Response savedEvents = client.call("POST", eventSavePath, eventSave);
        check(savedEvents.status == 200 &&
              savedEvents.json.value("generation") == QStringLiteral("2") &&
              savedEvents.json.value("eventCount") == QStringLiteral("1") &&
              savedEvents.json.value("acceptedEventIds").toArray().size() == 1 &&
              !savedEvents.json.contains("changes") && !savedEvents.json.contains("events") &&
              client.call("GET", detailPath).json.value("events").toArray().at(0)
                  .toObject().value("payloadBase64") == encodedEvent,
              "HTTP atomic event Save did not persist event and candidate together");
        const QString actionHistoryUrl = QStringLiteral("/dvmdb/v1/projects/") +
            QString::fromStdString(projectId) + QStringLiteral("/branches/main/action-history");
        const Response draftTimeline = client.call("GET", actionHistoryUrl +
            QStringLiteral("?draftId=") + QString::fromStdString(draftId));
        const QJsonArray draftActionDetails =
            draftTimeline.json.value("draft").toObject().value("actionDetails").toArray();
        check(draftTimeline.status == 200 &&
              draftTimeline.json.value("commits").toArray().isEmpty() &&
              draftTimeline.json.value("nextCursor").isNull() &&
              draftTimeline.json.value("draft").toObject().value("generation") == "2" &&
              draftTimeline.json.value("draft").toObject().value("journalCount") == "1" &&
              draftTimeline.json.value("draft").toObject().value("actions").toArray().size() == 1 &&
              draftActionDetails.size() == 1 &&
              draftActionDetails.first().toObject().value("operation") ==
                  QStringLiteral("hydraulic.junction.move") &&
              draftActionDetails.first().toObject().value("label") ==
                  QStringLiteral("hydraulic.junction.move") &&
              draftActionDetails.first().toObject().value("editCount") == 1 &&
              draftActionDetails.first().toObject().value("id") ==
                  draftTimeline.json.value("draft").toObject().value("actions").toArray().first() &&
              draftTimeline.json.value("draft").toObject().value("nextOffset").isNull() &&
              client.call("GET", actionHistoryUrl + "?draftId=" +
                  QString::fromStdString(draftId) + "&draftOffset=1").status == 400 &&
              client.call("GET", actionHistoryUrl + "?draftId=" +
                  QString::fromStdString(draftId) +
                  "&draftOffset=1&expectedDraftGeneration=2").status == 200 &&
              client.call("GET", actionHistoryUrl + "?draftId=" +
                  QString::fromStdString(draftId) +
                  "&draftOffset=1&expectedDraftGeneration=1").status == 409 &&
              client.call("GET", actionHistoryUrl + "?draftId=" +
                  QString::fromStdString(draftId) +
                  "&draftOffset=1&expectedDraftGeneration=bogus").status == 400 &&
              client.call("GET", actionHistoryUrl + "?limit=0").status == 400 &&
              client.call("GET", actionHistoryUrl + "?draftId=" +
                  QString::fromStdString(draftId) + "&draftOffset=2").status == 400,
              "HIST-02 HTTP draft timeline failed to paginate saved MODEL actions");
        const Response replay = client.call("POST", eventSavePath, eventSave);
        check(replay.status == 200 && replay.json.value("generation") == QStringLiteral("2") &&
              replay.json.value("acceptedEventIds").toArray().size() == 1,
              "HTTP lost-acknowledgement event replay was not idempotent");
        check(client.call("POST", savePath,
              QJsonObject{{"sessionId", otherSession},
                          {"fencingToken", taken.json.value("token")},
                          {"expectedGeneration", "2"},
                          {"changes", QJsonArray{entity}}}).status == 409,
              "HTTP unjournalled save bypassed existing event journal");
        check(client.call("POST", eventSavePath,
              QJsonObject{{"sessionId", otherSession},
                          {"fencingToken", taken.json.value("token")},
                          {"expectedGeneration", "2"},
                          {"changes", QJsonArray{entity}},
                          {"events", QJsonArray{"AAAA"}}}).status == 400,
              "HTTP accepted malformed MODEL event bytes");
        server.stop();
    }
    {
        native::ProjectService reopened(root.root());
        const native::WorkingCopyInfo draft = reopened.workingCopy(draftId);
        check(draft.generation == 2 && draft.dirty && draft.journal.size() == 1 &&
              draft.candidateChanges.size() == 1 &&
              draft.candidateChanges.front().record.has_value() &&
              draft.candidateChanges.front().record->payload == native::Bytes{4, 5, 6} &&
              !reopened.optionalHead(projectId, "main").has_value(),
              "HTTP draft candidate did not survive repository restart");
        TestServer server(reopened, native::BundleLimits{4ULL * 1024ULL * 1024ULL,
                                                         10000ULL, 1024ULL * 1024ULL});
        Client client(server.port());
        const Response restored = client.call("GET",
            QStringLiteral("/dvmdb/v1/working-copies/") + QString::fromStdString(draftId));
        check(restored.status == 200 && restored.json.value("generation") == QStringLiteral("2") &&
              restored.json.value("events").toArray().size() == 1 &&
              restored.json.value("changes").toArray().size() == 1 &&
              restored.json.value("baseRevision").isNull(),
              "HTTP draft GET lost saved candidate after service restart");
        const QString timelineUrl = QStringLiteral("/dvmdb/v1/projects/") +
            QString::fromStdString(projectId) + QStringLiteral("/branches/main/action-history");
        const Response resumedTimeline = client.call("GET", timelineUrl +
            "?draftId=" + QString::fromStdString(draftId));
        check(resumedTimeline.status == 200 &&
              resumedTimeline.json.value("draft").toObject().value("journalCount") == "1",
              "HIST-02 HTTP timeline lost the draft event after restart");
        check(draft.leaseSessionId.has_value(), "HIST-02 draft session lost on restart");
        const std::string token = reopened.acquireWorkingCopyLease(
            draftId, *draft.leaseSessionId, 120).token;
        const std::string operation = native::UuidV7::generate().toString();
        const native::LocalCommitInfo committed = reopened.commitWorkingCopy(
            operation, draftId, *draft.leaseSessionId, token, 2, std::nullopt,
            commitMetadata());
        const Response committedTimeline = client.call("GET", timelineUrl +
            "?limit=1&draftId=" + QString::fromStdString(draftId));
        const QJsonArray commits = committedTimeline.json.value("commits").toArray();
        check(committed.state == native::LocalCommitState::Succeeded &&
              committedTimeline.status == 200 && commits.size() == 1 &&
              commits.first().toObject().value("revision").toObject().value("id") ==
                  QString::fromStdString(committed.reservedRevisionId) &&
              commits.first().toObject().value("operationId") == QString::fromStdString(operation) &&
              commits.first().toObject().value("actionCount") == 1 &&
              commits.first().toObject().value("actions").toArray().size() == 1 &&
              !commits.first().toObject().value("checkpointOnly").toBool() &&
              committedTimeline.json.value("draft").toObject().value("journalCount") == "0",
              "HIST-02 HTTP timeline lost archived actions after Commit");
        const QString framesUrl = timelineUrl + QStringLiteral("/") +
            QString::fromStdString(committed.reservedRevisionId) + QStringLiteral("/events");
        const Response frames = client.call("GET", framesUrl + "?limit=1&offset=0");
        const QJsonArray firstFrame = frames.json.value("actions").toArray();
        const Response nextFrames = client.call("GET", framesUrl + "?limit=1&offset=1");
        check(frames.status == 200 && frames.json.value("total") == 1 &&
              frames.json.value("operationId") == QString::fromStdString(operation) &&
              frames.json.value("nextOffset").isNull() &&
              firstFrame.size() == 1 &&
              firstFrame.first().toObject().value("frameBase64") == encodedEvent &&
              firstFrame.first().toObject().value("operation") ==
                  QStringLiteral("hydraulic.junction.move") &&
              firstFrame.first().toObject().value("label") ==
                  QStringLiteral("hydraulic.junction.move") &&
              firstFrame.first().toObject().value("editCount") == 1 &&
              nextFrames.status == 200 && nextFrames.json.value("actions").toArray().isEmpty() &&
              client.call("GET", framesUrl + "?limit=0").status == 400 &&
              client.call("GET", framesUrl + "?offset=2").status == 400 &&
              client.call("GET", timelineUrl + "/" +
                  QString::fromStdString(native::UuidV7::generate().toString()) +
                  "/events").status != 200,
              "HIST-02 HTTP archived MODEL frame retrieval was not exact and bounded");
        server.stop();
    }
}

#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
void exerciseUnbornNetworkInitializationHttp() {
    namespace native = aowis::dvmdb;
    TemporaryRoot root;
    std::string draftId;
    std::string projectId;
    const QUuid networkUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    const QString networkId = networkUuid.toString(QUuid::WithoutBraces);
    const QUuid junctionUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    const QString junctionId = junctionUuid.toString(QUuid::WithoutBraces);
    {
        native::ProjectService service(root.root());
        const native::ProjectInfo project = service.createUnbornProject("first hydraulic distribution network");
        projectId = project.id;
        service.configureRepositoryRole(native::RepositoryRole::Private);
        TestServer server(service, native::BundleLimits{4ULL * 1024ULL * 1024ULL,
                                                       10000ULL, 1024ULL * 1024ULL});
        Client client(server.port());
        const QString session = QString::fromStdString(native::UuidV7::generate().toString());
        const QString branchId = QString::fromStdString(service.branchId(project.id, "main")->toString());
        const QString listPath = QStringLiteral("/dvmdb/v1/working-copies");
        const Response created = client.call("POST", listPath,
            QJsonObject{{"projectId", QString::fromStdString(project.id)},
                        {"branchId", branchId}, {"baseRevision", QJsonValue::Null},
                        {"sessionId", session}});
        check(created.status == 201 && created.json.value("baseRevision").isNull(),
              "unborn hydraulic draft could not be created");
        draftId = created.json.value("id").toString().toStdString();
        const QString path = listPath + "/" + QString::fromStdString(draftId);
        const Response emptySummary = client.call("GET", path + "/networks/summary");
        check(emptySummary.status == 200 && emptySummary.json.value("scope") == "working-copy" &&
              emptySummary.json.value("generation") == "0" &&
              emptySummary.json.value("networkCount") == 0 &&
              emptySummary.json.value("networks").toArray().isEmpty(),
              "new unborn working copy summary should be empty");
        const Response lease = client.call("POST", path + "/lease",
            QJsonObject{{"sessionId", session}, {"seconds", 60}});
        check(lease.status == 200, "unborn hydraulic draft lease failed");
        const QString token = lease.json.value("token").toString();
        const QJsonObject request{{"sessionId", session}, {"fencingToken", token},
                                  {"expectedGeneration", QStringLiteral("0")},
                                  {"networkId", networkId}, {"name", "new-network"}};
        QJsonObject invalid = request;
        invalid.insert("networkId", QUuid::createUuid().toString(QUuid::WithoutBraces));
        check(client.call("POST", path + "/initialize-network", invalid).status == 400,
              "first network accepted non-v7 UUID");
        invalid = request;
        invalid.insert("name", QStringLiteral(""));
        check(client.call("POST", path + "/initialize-network", invalid).status == 400,
              "first network accepted empty name");
        const QJsonObject forged{{"sessionId", session}, {"fencingToken", token},
            {"expectedGeneration", QStringLiteral("0")},
            {"changes", QJsonArray{QJsonObject{{"uuid", networkId}, {"kind", 100},
                {"references", QJsonArray{}}, {"payloadBase64", "AQID"}}}}};
        check(client.call("POST", path + "/save", forged).status == 400,
              "generic draft Save accepted forged hydraulic manifest bytes");
        check(client.call("GET", path).json.value("generation") == QStringLiteral("0"),
              "failed first-network initialization changed draft generation");
        const Response initialized = client.call("POST", path + "/initialize-network", request);
        const QJsonArray initialChanges = initialized.json.value("changes").toArray();
        check(initialized.status == 200 && initialized.json.value("generation") == QStringLiteral("1") &&
              initialized.json.value("dirty").toBool() &&
              initialChanges.size() == 2 &&
              std::count_if(initialChanges.cbegin(), initialChanges.cend(),
                  [](const QJsonValue& change) {
                      return change.toObject().value("kind").toInt() == 100;
                  }) == 1 &&
              std::count_if(initialChanges.cbegin(), initialChanges.cend(),
                  [](const QJsonValue& change) {
                      return change.toObject().value("kind").toInt() == 110;
                  }) == 1 &&
              initialized.json.value("events").toArray().isEmpty() &&
              !service.optionalHead(project.id, "main").has_value(),
              "first network changed committed HEAD or did not save root in unborn draft");
        const Response firstSummary = client.call("GET", path + "/networks/summary");
        check(firstSummary.status == 200 && firstSummary.json.value("generation") == "1" &&
              firstSummary.json.value("networkCount") == 1 &&
              firstSummary.json.value("networks").toArray().first().toObject()
                  .value("id") == networkId &&
              firstSummary.json.value("networks").toArray().first().toObject()
                  .value("counts").toObject().value("junctions") == 0,
              "unborn saved network was omitted from private draft summary");
        const Response firstView = client.call("GET", path + "/networks/" + networkId);
        const QByteArray firstPayload = QByteArray::fromBase64(
            firstView.json.value("payloadBase64").toString().toLatin1());
        const std::optional<NetworkHydraulicDistribution> firstNetwork =
            native::ModelCodec<NetworkHydraulicDistribution>::decode(firstPayload);
        check(firstView.status == 200 && firstView.json.value("generation") == "1" &&
              firstView.json.value("draftId") == QString::fromStdString(draftId) &&
              firstNetwork && firstNetwork->uuid == networkUuid &&
              firstNetwork->nodes_junctions.isEmpty(),
              "unborn draft first network could not be hydrated");
        check(client.call("GET", path + "/networks/" + junctionId).status == 404 &&
              client.call("GET", path + "/networks/not-a-uuid").status == 400,
              "draft hydration accepted missing or malformed network identity");
        const Response replay = client.call("POST", path + "/initialize-network", request);
        check(replay.status == 200 && replay.json.value("generation") == QStringLiteral("1"),
              "first network retry duplicated initialization");
        invalid = request;
        invalid.insert("name", "different-network");
        check(client.call("POST", path + "/initialize-network", invalid).status == 409,
              "first network conflicting replay was accepted");
        HydraulicNodeJunction junction;
        junction.uuid = junctionUuid;
        junction.id = QStringLiteral("J1");
        junction.elevation_m = 12;
        const QByteArray bytes = native::ModelCodec<HydraulicNodeJunction>::encode(junction);
        aowis::model::ModelEditTransaction event;
        event.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
        event.operation = QStringLiteral("hydraulic.junction.create");
        event.edits.append(aowis::model::ModelEntityEdit{
            junctionUuid, QStringLiteral("HydraulicNodeJunction"), std::nullopt, bytes});
        // Creating the first member also creates its membership chunk and
        // rewrites the native root. The metadata edit must be composed onto
        // that same server-owned root without generating a duplicate key.
        NetworkHydraulicDistribution initialMixedRoot = *firstNetwork;
        initialMixedRoot.title_line_3 = QStringLiteral("First member and root metadata");
        event.edits.append(aowis::model::ModelEntityEdit{
            networkUuid, QString::fromLatin1(aowis::model::hydraulicDistributionRootEditType),
            aowis::model::hydraulicDistributionRootImage(*firstNetwork),
            aowis::model::hydraulicDistributionRootImage(initialMixedRoot)});
        QString encodingError;
        const QByteArray frame = aowis::model::ModelEditTransactionCodec::encode(event, &encodingError);
        check(!frame.isEmpty(), "failed to encode first unborn junction event");
        const QJsonObject save{{"sessionId", session}, {"fencingToken", token},
            {"expectedGeneration", QStringLiteral("1")},
            {"changes", QJsonArray{QJsonObject{{"uuid", junctionId}, {"kind", 201},
                {"references", QJsonArray{}},
                {"payloadBase64", QString::fromLatin1(bytes.toBase64())}}}},
            {"membership", QJsonArray{QJsonObject{{"networkId", networkId},
                {"entityId", junctionId}, {"kind", 201}, {"collectionIndex", 9},
                {"inserted", true}}}},
            {"events", QJsonArray{QString::fromLatin1(frame.toBase64())}}};
        const Response saved = client.call("POST", path + "/save-events", save);
        check(saved.status == 200 && saved.json.value("generation") == QStringLiteral("2") &&
              !service.optionalHead(project.id, "main").has_value(),
              "first hydraulic member could not be saved into unborn draft");
        // Existing, canonically encoded hydraulic entities must not be edited
        // through plain Save without an authenticated MODEL event journal.
        const QJsonObject unjournaledHydraulic{{"sessionId", session},
            {"fencingToken", token}, {"expectedGeneration", "2"},
            {"changes", QJsonArray{QJsonObject{{"uuid", junctionId}, {"kind", 201},
                {"references", QJsonArray{}},
                {"payloadBase64", QString::fromLatin1(bytes.toBase64())}}}}};
        check(client.call("POST", path + "/save", unjournaledHydraulic).status == 400 &&
              client.call("GET", path).json.value("generation") == "2",
              "plain Save accepted an unjournaled hydraulic edit");
        const Response populatedSummary = client.call("GET", path + "/networks/summary");
        check(populatedSummary.status == 200 && populatedSummary.json.value("generation") == "2" &&
              populatedSummary.json.value("networkCount") == 1 &&
              populatedSummary.json.value("networks").toArray().first().toObject()
                  .value("counts").toObject().value("junctions") == 1,
              "private summary missed a saved uncommitted junction");
        const Response populatedView = client.call("GET", path + "/networks/" + networkId);
        const QByteArray populatedPayload = QByteArray::fromBase64(
            populatedView.json.value("payloadBase64").toString().toLatin1());
        const std::optional<NetworkHydraulicDistribution> populatedNetwork =
            native::ModelCodec<NetworkHydraulicDistribution>::decode(populatedPayload);
        check(populatedView.status == 200 && populatedView.json.value("generation") == "2" &&
              populatedNetwork && populatedNetwork->nodes_junctions.size() == 1 &&
              populatedNetwork->nodes_junctions.first().uuid == junctionUuid &&
              populatedNetwork->nodes_junctions.first().elevation_m == 12 &&
              populatedNetwork->title_line_3 == QStringLiteral("First member and root metadata") &&
              !service.optionalHead(project.id, "main").has_value(),
              "saved membership did not hydrate canonical draft network");
        // Root metadata uses the same fenced event journal, but no client
        // sends native kind-100 manifest bytes or membership chunk records.
        NetworkHydraulicDistribution updatedRoot = *populatedNetwork;
        updatedRoot.title_line_1 = QStringLiteral("Saved root metadata");
        updatedRoot.options_hydraulic.headloss_formula = HydraulicHeadlossFormula::DarcyWeisbach;
        aowis::model::ModelEditTransaction rootEvent;
        rootEvent.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
        rootEvent.operation = QStringLiteral("hydraulic.network.root.update");
        rootEvent.edits.append(aowis::model::ModelEntityEdit{
            networkUuid, QString::fromLatin1(aowis::model::hydraulicDistributionRootEditType),
            aowis::model::hydraulicDistributionRootImage(*populatedNetwork),
            aowis::model::hydraulicDistributionRootImage(updatedRoot)});
        const QByteArray rootFrame = aowis::model::ModelEditTransactionCodec::encode(rootEvent);
        check(!rootFrame.isEmpty(), "failed to encode hydraulic root transaction");
        const QJsonObject rootSave{{"sessionId", session}, {"fencingToken", token},
            {"expectedGeneration", QStringLiteral("2")}, {"changes", QJsonArray{}},
            {"events", QJsonArray{QString::fromLatin1(rootFrame.toBase64())}}};
        const Response savedRoot = client.call("POST", path + "/save-events", rootSave);
        if (savedRoot.status != 200)
            throw std::runtime_error("root event Save failed: HTTP " +
                std::to_string(savedRoot.status) + " " +
                QJsonDocument(savedRoot.json).toJson(QJsonDocument::Compact).toStdString());
        const Response replayedRoot = client.call("POST", path + "/save-events", rootSave);
        if (replayedRoot.status != 200)
            throw std::runtime_error("root event lost-response replay failed: HTTP " +
                std::to_string(replayedRoot.status) + " " +
                QJsonDocument(replayedRoot.json).toJson(QJsonDocument::Compact).toStdString());
        check(savedRoot.json.value("generation") == "3" &&
              replayedRoot.json.value("generation") == "3",
              "root event Save or lost-response replay returned the wrong generation");
        const Response rootView = client.call("GET", path + "/networks/" + networkId);
        const std::optional<NetworkHydraulicDistribution> finalRoot =
            native::ModelCodec<NetworkHydraulicDistribution>::decode(QByteArray::fromBase64(
                rootView.json.value("payloadBase64").toString().toLatin1()));
        check(rootView.status == 200 && finalRoot &&
              finalRoot->options_hydraulic.headloss_formula == HydraulicHeadlossFormula::DarcyWeisbach &&
              finalRoot->title_line_1 == updatedRoot.title_line_1 &&
              finalRoot->nodes_junctions.size() == 1 &&
              finalRoot->nodes_junctions.first().uuid == junctionUuid,
              "root event Save changed membership or failed to persist hydraulic metadata");
        aowis::model::ModelEditTransaction stale = rootEvent;
        stale.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
        const QJsonObject staleSave{{"sessionId", session}, {"fencingToken", token},
            {"expectedGeneration", QStringLiteral("3")}, {"changes", QJsonArray{}},
            {"events", QJsonArray{QString::fromLatin1(
                aowis::model::ModelEditTransactionCodec::encode(stale).toBase64())}}};
        check(client.call("POST", path + "/save-events", staleSave).status == 409 &&
              client.call("GET", path).json.value("generation") == "3",
              "stale network-root before-image changed the saved draft");
        const Response after = client.call("POST", path + "/initialize-network", request);
        check(after.status == 409, "initialization replay was accepted after later edit");
        // INT-05: one reversible MODEL transaction changes server-owned root
        // metadata and inserts an entity. Its native manifest must be updated
        // only once, regardless of whether membership creates a new chunk.
        const QUuid additionalUuid(QString::fromStdString(native::UuidV7::generate().toString()));
        const QString additionalId = additionalUuid.toString(QUuid::WithoutBraces);
        HydraulicNodeJunction additional;
        additional.uuid = additionalUuid;
        additional.id = QStringLiteral("J2");
        additional.elevation_m = 27;
        const QByteArray additionalBytes = native::ModelCodec<HydraulicNodeJunction>::encode(additional);
        NetworkHydraulicDistribution mixedExpected = *finalRoot;
        mixedExpected.title_line_2 = QStringLiteral("Mixed atomic root and entity edit");
        aowis::model::ModelEditTransaction mixedEvent;
        mixedEvent.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
        mixedEvent.operation = QStringLiteral("hydraulic.network.mixed");
        mixedEvent.description = QStringLiteral("Add J2 and update network title");
        mixedEvent.edits.append(aowis::model::ModelEntityEdit{
            networkUuid, QString::fromLatin1(aowis::model::hydraulicDistributionRootEditType),
            aowis::model::hydraulicDistributionRootImage(*finalRoot),
            aowis::model::hydraulicDistributionRootImage(mixedExpected)});
        mixedEvent.edits.append(aowis::model::ModelEntityEdit{
            additionalUuid, QStringLiteral("HydraulicNodeJunction"), std::nullopt, additionalBytes});
        const QByteArray mixedFrame = aowis::model::ModelEditTransactionCodec::encode(mixedEvent);
        check(!mixedFrame.isEmpty(), "failed to encode mixed hydraulic transaction");
        const QJsonObject mixedSave{{"sessionId", session}, {"fencingToken", token},
            {"expectedGeneration", QStringLiteral("3")},
            {"changes", QJsonArray{QJsonObject{{"uuid", additionalId}, {"kind", 201},
                {"references", QJsonArray{}},
                {"payloadBase64", QString::fromLatin1(additionalBytes.toBase64())}}}},
            {"membership", QJsonArray{QJsonObject{{"networkId", networkId},
                {"entityId", additionalId}, {"kind", 201}, {"collectionIndex", 9},
                {"inserted", true}}}},
            {"events", QJsonArray{QString::fromLatin1(mixedFrame.toBase64())}}};
        const QUuid unrelatedUuid(QString::fromStdString(native::UuidV7::generate().toString()));
        HydraulicNodeJunction unrelated = additional;
        unrelated.uuid = unrelatedUuid;
        unrelated.id = QStringLiteral("Unjournaled");
        const QByteArray unrelatedBytes = native::ModelCodec<HydraulicNodeJunction>::encode(unrelated);
        QJsonObject unjournaled = mixedSave;
        QJsonArray unjournaledChanges = mixedSave.value("changes").toArray();
        unjournaledChanges.append(QJsonObject{
            {"uuid", unrelatedUuid.toString(QUuid::WithoutBraces)}, {"kind", 201},
            {"references", QJsonArray{}},
            {"payloadBase64", QString::fromLatin1(unrelatedBytes.toBase64())}});
        unjournaled.insert("changes", unjournaledChanges);
        check(client.call("POST", path + "/save-events", unjournaled).status == 400 &&
              client.call("GET", path).json.value("generation") == "3",
              "mixed Save accepted a typed record absent from its MODEL event");
        const Response mixedSaved = client.call("POST", path + "/save-events", mixedSave);
        if (mixedSaved.status != 200)
            throw std::runtime_error("mixed root/entity Save failed: HTTP " +
                std::to_string(mixedSaved.status) + " " +
                QJsonDocument(mixedSaved.json).toJson(QJsonDocument::Compact).toStdString());
        const Response mixedReplayed = client.call("POST", path + "/save-events", mixedSave);
        const Response mixedView = client.call("GET", path + "/networks/" + networkId);
        const std::optional<NetworkHydraulicDistribution> mixedNetwork =
            native::ModelCodec<NetworkHydraulicDistribution>::decode(QByteArray::fromBase64(
                mixedView.json.value("payloadBase64").toString().toLatin1()));
        mixedExpected.nodes_junctions.append(additional);
        const QString actionHistoryUrl = QStringLiteral("/dvmdb/v1/projects/") +
            QString::fromStdString(projectId) + QStringLiteral("/branches/main/action-history");
        const Response labelledDraft = client.call("GET", actionHistoryUrl +
            "?draftId=" + QString::fromStdString(draftId));
        const QJsonArray labelledActions = labelledDraft.json.value("draft")
            .toObject().value("actionDetails").toArray();
        check(labelledDraft.status == 200 && labelledActions.size() == 3 &&
              labelledActions.last().toObject().value("description") ==
                  QStringLiteral("Add J2 and update network title") &&
              labelledActions.last().toObject().value("label") ==
                  QStringLiteral("Add J2 and update network title") &&
              labelledActions.last().toObject().value("operation") ==
                  QStringLiteral("hydraulic.network.mixed") &&
              labelledActions.last().toObject().value("editCount") == 2,
              "HIST-02 saved hydraulic draft lost user-facing action descriptions");
        check(mixedSaved.json.value("generation") == "4" &&
              mixedReplayed.status == 200 && mixedReplayed.json.value("generation") == "4" &&
              mixedView.status == 200 && mixedNetwork &&
              native::ModelCodec<NetworkHydraulicDistribution>::encode(*mixedNetwork) ==
                  native::ModelCodec<NetworkHydraulicDistribution>::encode(mixedExpected) &&
              client.call("GET", path).json.value("events").toArray().size() == 3,
              "mixed root/entity event did not persist exactly once with complete canonical equality");
        // Bad before-image must not partially save an entity or change the
        // root, even when the mixed request includes a valid membership delta.
        aowis::model::ModelEditTransaction staleMixed = mixedEvent;
        staleMixed.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
        const QUuid rejectedUuid(QString::fromStdString(native::UuidV7::generate().toString()));
        HydraulicNodeJunction rejected = additional;
        rejected.uuid = rejectedUuid;
        rejected.id = QStringLiteral("J3");
        const QByteArray rejectedBytes = native::ModelCodec<HydraulicNodeJunction>::encode(rejected);
        for (aowis::model::ModelEntityEdit& edit : staleMixed.edits) {
            if (edit.entityType == QStringLiteral("HydraulicNodeJunction")) {
                edit.entityId = rejectedUuid;
                edit.after = rejectedBytes;
            }
        }
        QJsonObject staleMixedSave = mixedSave;
        staleMixedSave.insert("expectedGeneration", QStringLiteral("4"));
        staleMixedSave.insert("changes", QJsonArray{QJsonObject{
            {"uuid", rejectedUuid.toString(QUuid::WithoutBraces)}, {"kind", 201},
            {"references", QJsonArray{}},
            {"payloadBase64", QString::fromLatin1(rejectedBytes.toBase64())}}});
        staleMixedSave.insert("membership", QJsonArray{QJsonObject{
            {"networkId", networkId}, {"entityId", rejectedUuid.toString(QUuid::WithoutBraces)},
            {"kind", 201}, {"collectionIndex", 9}, {"inserted", true}}});
        staleMixedSave.insert("events", QJsonArray{QString::fromLatin1(
            aowis::model::ModelEditTransactionCodec::encode(staleMixed).toBase64())});
        check(client.call("POST", path + "/save-events", staleMixedSave).status == 409 &&
              client.call("GET", path).json.value("generation") == "4",
              "mixed transaction accepted stale metadata and partially persisted an entity");
        server.stop();
    }
    {
        native::ProjectService reopened(root.root());
        const native::WorkingCopyInfo draft = reopened.workingCopy(draftId);
        check(!draft.baseRevision && draft.generation == 4 && draft.journal.size() == 3 &&
              draft.candidateChanges.size() >= 4 &&
              !reopened.optionalHead(projectId, "main").has_value(),
              "first hydraulic distribution network, member and root edit did not survive service restart");
        TestServer server(reopened, native::BundleLimits{4ULL * 1024ULL * 1024ULL,
                                                        10000ULL, 1024ULL * 1024ULL});
        Client client(server.port());
        const QString reopenedPath = QStringLiteral("/dvmdb/v1/working-copies/")
            + QString::fromStdString(draftId);
        const Response recoveredSummary = client.call("GET", reopenedPath + "/networks/summary");
        check(recoveredSummary.status == 200 && recoveredSummary.json.value("generation") == "4" &&
              recoveredSummary.json.value("networkCount") == 1 &&
              recoveredSummary.json.value("networks").toArray().first().toObject()
                  .value("counts").toObject().value("junctions") == 2,
              "private network summary was lost after DVMDB restart");
        const Response recovered = client.call("GET", QStringLiteral("/dvmdb/v1/working-copies/")
            + QString::fromStdString(draftId) + "/networks/" + networkId);
        const std::optional<NetworkHydraulicDistribution> restored = native::ModelCodec<NetworkHydraulicDistribution>::decode(
            QByteArray::fromBase64(recovered.json.value("payloadBase64").toString().toLatin1()));
        check(recovered.status == 200 && recovered.json.value("generation") == "4" &&
              restored && restored->title_line_1 == QStringLiteral("Saved root metadata") &&
              restored->options_hydraulic.headloss_formula == HydraulicHeadlossFormula::DarcyWeisbach &&
              restored->nodes_junctions.size() == 2 &&
              restored->nodes_junctions.first().uuid == junctionUuid &&
              restored->title_line_2 == QStringLiteral("Mixed atomic root and entity edit"),
              "restart failed to hydrate saved unborn hydraulic draft");
        server.stop();
    }
}

void exerciseAnnotationDraftHttp() {
    namespace native = aowis::dvmdb;
    TemporaryRoot root;
    native::ProjectService service(root.root());
    const native::ProjectInfo project = service.createUnbornProject("annotation draft");
    service.configureRepositoryRole(native::RepositoryRole::Private);
    TestServer server(service, native::BundleLimits{4ULL * 1024ULL * 1024ULL,
                                                    10000ULL, 1024ULL * 1024ULL});
    Client client(server.port());
    const QString session = QString::fromStdString(native::UuidV7::generate().toString());
    const QString branchId = QString::fromStdString(service.branchId(project.id, "main")->toString());
    const QString networkId = QString::fromStdString(native::UuidV7::generate().toString());
    const QUuid layerUuid = native::annotations::distributionLayerUuid(QUuid(networkId));
    const QString layerId = layerUuid.toString(QUuid::WithoutBraces);
    const Response created = client.call("POST", "/dvmdb/v1/working-copies",
        QJsonObject{{"projectId", QString::fromStdString(project.id)},
                    {"branchId", branchId}, {"baseRevision", QJsonValue::Null},
                    {"sessionId", session}});
    check(created.status == 201, "annotation draft creation failed");
    const QString draftPath = QStringLiteral("/dvmdb/v1/working-copies/") +
        created.json.value("id").toString();
    const QString layerPath = draftPath + QStringLiteral("/annotation-layers/") + layerId;
    const Response lease = client.call("POST", draftPath + "/lease",
        QJsonObject{{"sessionId", session}, {"seconds", 90}});
    check(lease.status == 200, "annotation draft lease failed");
    const QString token = lease.json.value("token").toString();
    const QJsonObject initialize{{"sessionId", session}, {"fencingToken", token},
        {"expectedGeneration", "0"}, {"networkId", networkId}, {"name", "annotation-test"}};
    const Response started = client.call("POST", draftPath + "/initialize-network", initialize);
    check(started.status == 200 && started.json.value("generation") == "1" &&
          started.json.value("changes").toArray().size() == 2,
          "annotation layer not atomically initialized with network");
    const Response empty = client.call("GET", layerPath);
    check(empty.status == 200 && empty.json.value("generation") == "1" &&
          empty.json.value("ownerNetworkId") == networkId &&
          empty.json.value("count") == 0,
          "initialized annotation layer not readable in draft");

    MapAnnotation annotation;
    annotation.uuid = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    annotation.text = QStringLiteral("Pumping station");
    annotation.position_wgs84.longitude_deg = 14.1;
    annotation.position_wgs84.latitude_deg = 11.2;
    const QByteArray initial = native::ModelCodec<MapAnnotation>::encode(annotation);
    check(!initial.isEmpty(), "MODEL rejected valid annotation");
    const QString annotationId = annotation.uuid.toString(QUuid::WithoutBraces);
    const std::function<QByteArray(const MapAnnotation&, const std::optional<QByteArray>&,
                                   const std::optional<QByteArray>&)> frame = [&layerId](const MapAnnotation& item, const std::optional<QByteArray>& before,
                          const std::optional<QByteArray>& after) -> QByteArray {
        aowis::model::ModelEditTransaction transaction;
        transaction.transactionId = QUuid(QString::fromStdString(
            aowis::dvmdb::UuidV7::generate().toString()));
        transaction.operation = QStringLiteral("map.annotation.edit");
        transaction.edits.append(aowis::model::ModelEntityEdit{
            item.uuid, QStringLiteral("map.annotation"), before, after, QUuid(layerId)});
        return aowis::model::ModelEditTransactionCodec::encode(transaction);
    };
    const std::function<QJsonObject(const QString&, const QByteArray&)> request = [&session, &token, &layerId](
        const QString& generation, const QByteArray& encoded) -> QJsonObject {
        return QJsonObject{{"sessionId", session}, {"fencingToken", token},
            {"expectedGeneration", generation}, {"annotationLayerId", layerId},
            {"changes", QJsonArray{}},
            {"events", QJsonArray{QString::fromLatin1(encoded.toBase64())}}};
    };
    const QByteArray createFrame = frame(annotation, std::nullopt, initial);
    check(!createFrame.isEmpty(), "annotation create event could not be encoded");
    const QJsonObject createRequest = request("1", createFrame);
    const Response added = client.call("POST", draftPath + "/save-events", createRequest);
    if (added.status != 200)
        throw std::runtime_error("annotation CREATE failed: " +
            QJsonDocument(added.json).toJson(QJsonDocument::Compact).toStdString());
    check(added.json.value("generation") == "2" &&
          added.json.value("acceptedEventIds").toArray().size() == 1 &&
          client.call("GET", layerPath).json.value("count") == 1,
          "annotation create did not atomically persist member and event");
    const Response replay = client.call("POST", draftPath + "/save-events", createRequest);
    check(replay.status == 200 && replay.json.value("generation") == "2" &&
          client.call("GET", layerPath).json.value("count") == 1,
          "lost-response annotation create replay was not idempotent");

    MapAnnotation changed = annotation;
    changed.text = QStringLiteral("Pumping station — updated");
    const QByteArray revised = native::ModelCodec<MapAnnotation>::encode(changed);
    const Response edited = client.call("POST", draftPath + "/save-events",
        request("2", frame(annotation, initial, revised)));
    check(edited.status == 200 && edited.json.value("generation") == "3",
          "annotation update event failed");
    const QJsonArray members = client.call("GET", layerPath).json.value("annotations").toArray();
    check(members.size() == 1 && members.first().toObject().value("uuid") == annotationId &&
          QByteArray::fromBase64(members.first().toObject().value("payloadBase64")
              .toString().toLatin1()) == revised,
          "annotation update did not return canonical MODEL bytes");
    check(client.call("POST", draftPath + "/save-events",
        request("3", frame(annotation, initial, revised))).status == 409,
        "stale annotation before-image was accepted");
    MapAnnotation foreignAnchor = changed;
    MapAnnotationAnchor anchor;
    anchor.entity_uuid = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    anchor.entity_type = QStringLiteral("HydraulicNodeJunction");
    foreignAnchor.anchor = anchor;
    const Response refusedAnchor = client.call("POST", draftPath + "/save-events",
        request("3", frame(annotation, revised,
            native::ModelCodec<MapAnnotation>::encode(foreignAnchor))));
    check(refusedAnchor.status == 400 &&
          client.call("GET", draftPath).json.value("generation") == "3",
          "candidate preflight accepted an annotation anchored outside its network");
    const Response removed = client.call("POST", draftPath + "/save-events",
        request("3", frame(annotation, revised, std::nullopt)));
    check(removed.status == 200 && removed.json.value("generation") == "4" &&
          client.call("GET", layerPath).json.value("count") == 0,
          "annotation delete did not remove native membership and member");
    // Raw clients must not forge native member records, including the deleted
    // UUID, even when sending a syntactically valid canonical payload.
    const QJsonObject forge{{"sessionId", session}, {"fencingToken", token},
        {"expectedGeneration", "4"},
        {"changes", QJsonArray{QJsonObject{{"uuid", annotationId}, {"kind", 112},
            {"references", QJsonArray{}},
            {"payloadBase64", QString::fromLatin1(initial.toBase64())}}}}};
    check(client.call("POST", draftPath + "/save", forge).status == 400 &&
          client.call("GET", draftPath).json.value("generation") == "4",
          "generic Save accepted a forged annotation record");

    // A valid annotation history may Commit through HTTP and the SAME
    // operationId must remain idempotent after the draft was rebased.
    const QString operation = QString::fromStdString(native::UuidV7::generate().toString());
    const QString commitPath = QStringLiteral("/dvmdb/v1/local-commits");
    const QJsonObject normalCommit{{"operationId", operation},
        {"draftId", created.json.value("id")},
        {"sessionId", session}, {"fencingToken", token},
        {"expectedGeneration", "4"},
        {"expectedHead", QJsonValue(QJsonValue::Null)},
        {"author", Client::testAuthor()}, {"message", "Annotation history"}};
    const Response committed = client.call("POST", commitPath, normalCommit);
    const QString committedRevision = committed.json.value("revision").toString();
    check(committed.status == 200 && !committedRevision.isEmpty() &&
          client.call("POST", commitPath, normalCommit).json.value("revision") == committedRevision,
          "valid annotation Commit or idempotent replay was rejected");

    // Generic direct MODEL Commit must reject any engineering snapshot,
    // even when the submitted delta itself uses an ordinary generic kind.
    const QJsonObject directModelCommit{{"expectedHead", committedRevision},
        {"author", Client::testAuthor()}, {"message", "Unjournaled generic delta"},
        {"changes", QJsonArray{QJsonObject{{"uuid", annotationId},
            {"kind", 31}, {"references", QJsonArray{}},
            {"payloadBase64", QString::fromLatin1(initial.toBase64())}}}}};
    check(client.call("POST", QStringLiteral("/dvmdb/v1/projects/") +
              QString::fromStdString(project.id) + "/branches/main/model-commits",
              directModelCommit).status == 400 &&
          service.optionalHead(project.id, "main") ==
              native::RevisionId::parse(committedRevision.toStdString()),
          "generic direct MODEL Commit bypassed annotation action journal");

    // The first Commit is a checkpoint: it has no immutable parent to replay.
    // A subsequent real local Commit with two annotation actions must provide
    // an authenticated revision witness, including order, native membership
    // records, parent/child snapshot closure, and both intermediate boundaries.
    const std::optional<native::RevisionId> initialRevision =
        native::RevisionId::parse(committedRevision.toStdString());
    check(initialRevision.has_value(), "first annotation Commit has invalid revision identity");
    QString historyError;
    check(!native::annotations::verifyIndexedAnnotationActionBoundary(
              service, project.id, "main", *initialRevision, 0, &historyError) &&
          !historyError.isEmpty(),
          "first Commit without immutable parent was exposed as annotation actions");
    const native::WorkingCopyInfo archivedDraft = service.createWorkingCopy(
        project.id, "main", initialRevision, session.toStdString());
    const native::WorkingCopyLease archivedLease = service.acquireWorkingCopyLease(
        archivedDraft.id, session.toStdString(), 90);
    const QString archivedPath = QStringLiteral("/dvmdb/v1/working-copies/") +
        QString::fromStdString(archivedDraft.id);
    const std::function<QJsonObject(const QString&, const QByteArray&)> archivedRequest =
        [&session, &archivedLease, &layerId](const QString& generation,
                                            const QByteArray& eventBytes) -> QJsonObject {
            return QJsonObject{{"sessionId", session},
                {"fencingToken", QString::fromStdString(archivedLease.token)},
                {"expectedGeneration", generation}, {"annotationLayerId", layerId},
                {"changes", QJsonArray{}},
                {"events", QJsonArray{QString::fromLatin1(eventBytes.toBase64())}}};
        };
    check(client.call("POST", archivedPath + "/save-events",
              archivedRequest("0", frame(annotation, std::nullopt, initial))).status == 200 &&
          client.call("POST", archivedPath + "/save-events",
              archivedRequest("1", frame(annotation, initial, revised))).status == 200,
          "could not persist two actions for indexed annotation archive replay");
    const QString secondOperation =
        QString::fromStdString(native::UuidV7::generate().toString());
    const Response secondCommitted = client.call("POST", commitPath,
        QJsonObject{{"operationId", secondOperation},
            {"draftId", QString::fromStdString(archivedDraft.id)},
            {"sessionId", session},
            {"fencingToken", QString::fromStdString(archivedLease.token)},
            {"expectedGeneration", "2"}, {"expectedHead", committedRevision},
            {"author", Client::testAuthor()}, {"message", "Indexed annotation history"}});
    const QString secondCommittedRevision = secondCommitted.json.value("revision").toString();
    const std::optional<native::RevisionId> secondRevision =
        native::RevisionId::parse(secondCommittedRevision.toStdString());
    check(secondCommitted.status == 200 && secondRevision.has_value(),
          "second annotation Commit did not publish a revision");
    const std::optional<native::annotations::VerifiedIndexedAnnotationActionBoundary> zero =
        native::annotations::verifyIndexedAnnotationActionBoundary(
            service, project.id, "main", *secondRevision, 0, &historyError);
    check(zero && zero->boundary.changesFromParent.empty() &&
          zero->commitOperationId == secondOperation.toStdString() &&
          zero->boundary.actionCount == 2,
          "indexed annotation archive did not reproduce its immutable parent");
    const std::optional<native::annotations::VerifiedIndexedAnnotationActionBoundary> middle =
        native::annotations::verifyIndexedAnnotationActionBoundary(
            service, project.id, "main", *secondRevision, 1, &historyError);
    check(middle && !middle->boundary.changesFromParent.empty() &&
          middle->boundary.changedLayerIds.size() == 1 &&
          middle->boundary.changedLayerIds.front() == layerUuid,
          "indexed annotation archive lost its first intermediate action");
    const std::optional<native::annotations::VerifiedIndexedAnnotationActionBoundary> last =
        native::annotations::verifyIndexedAnnotationActionBoundary(
            service, project.id, "main", *secondRevision, 2, &historyError);
    check(last && !last->boundary.changesFromParent.empty() &&
          last->revisionId == secondCommittedRevision.toStdString(),
          "indexed annotation archive did not reconstruct the committed candidate");
    check(!native::annotations::verifyIndexedAnnotationActionBoundary(
              service, project.id, "main", *secondRevision, 3, &historyError) &&
          !historyError.isEmpty(),
          "indexed annotation archive exposed an out-of-range action position");

    // B4.3: publish the verified annotation-only archive through the SAME
    // fenced working-copy History endpoint. Neither the client nor the draft
    // supplies native record changes. The immutable HEAD never moves.
    const native::WorkingCopyInfo navigationDraft = service.createWorkingCopy(
        project.id, "main", secondRevision, session.toStdString());
    const native::WorkingCopyLease navigationLease = service.acquireWorkingCopyLease(
        navigationDraft.id, session.toStdString(), 90);
    const QString navigationPath = QStringLiteral("/dvmdb/v1/working-copies/") +
        QString::fromStdString(navigationDraft.id);
    const QString navigationLayerPath = navigationPath + "/annotation-layers/" + layerId;
    const std::function<QJsonObject(const QString&, const QString&, const QString&)> navigationBody =
        [&](const QString& operationId, const QString& generation,
            const QString& position) -> QJsonObject {
            return QJsonObject{{"operationId", operationId},
                {"sessionId", session},
                {"fencingToken", QString::fromStdString(navigationLease.token)},
                {"expectedGeneration", generation},
                {"expectedHead", secondCommittedRevision},
                {"revisionId", secondCommittedRevision},
                {"actionPosition", position}};
        };
    const QString firstNavigationId =
        QString::fromStdString(native::UuidV7::generate().toString());
    const QJsonObject zeroNavigation = navigationBody(firstNavigationId, "0", "0");
    const Response atZero = client.call("POST", navigationPath + "/history/navigate", zeroNavigation);
    const Response emptyLayer = client.call("GET", navigationLayerPath);
    check(atZero.status == 200 && atZero.json.value("generation") == "1" &&
          atZero.json.value("archivedActionPosition") == "0" &&
          emptyLayer.status == 200 && emptyLayer.json.value("count") == 0 &&
          service.optionalHead(project.id, "main") == secondRevision,
          "annotation archived parent could not be selected without changing HEAD");
    check(client.call("POST", navigationPath + "/history/navigate", zeroNavigation).status == 200 &&
          service.workingCopy(navigationDraft.id).generation == 1,
          "annotation archived History retry advanced generation twice");
    check(client.call("POST", navigationPath + "/history/navigate",
              navigationBody(firstNavigationId, "0", "1")).status == 409,
          "annotation archived History accepted a reused operation UUID for another position");
    check(client.call("POST", navigationPath + "/history/navigate",
              navigationBody(QString::fromStdString(native::UuidV7::generate().toString()),
                             "0", "1")).status == 409,
          "annotation archived History accepted a stale generation");
    const Response atOne = client.call("POST", navigationPath + "/history/navigate",
        navigationBody(QString::fromStdString(native::UuidV7::generate().toString()), "1", "1"));
    const Response firstLayer = client.call("GET", navigationLayerPath);
    const QJsonArray firstMembers = firstLayer.json.value("annotations").toArray();
    check(atOne.status == 200 && atOne.json.value("generation") == "2" &&
          firstLayer.status == 200 && firstMembers.size() == 1 &&
          QByteArray::fromBase64(firstMembers.first().toObject().value("payloadBase64")
              .toString().toLatin1()) == initial,
          "annotation archived intermediate create was not reconstructed");
    const Response atTwo = client.call("POST", navigationPath + "/history/navigate",
        navigationBody(QString::fromStdString(native::UuidV7::generate().toString()), "2", "2"));
    const Response finalLayer = client.call("GET", navigationLayerPath);
    const QJsonArray finalMembers = finalLayer.json.value("annotations").toArray();
    check(atTwo.status == 200 && atTwo.json.value("generation") == "3" &&
          !atTwo.json.value("dirty").toBool() && finalLayer.status == 200 &&
          finalMembers.size() == 1 &&
          QByteArray::fromBase64(finalMembers.first().toObject().value("payloadBase64")
              .toString().toLatin1()) == revised &&
          service.optionalHead(project.id, "main") == secondRevision,
          "annotation archived Redo to tip did not recover clean committed HEAD");
    check(client.call("POST", navigationPath + "/history/navigate",
              navigationBody(QString::fromStdString(native::UuidV7::generate().toString()),
                             "3", "3")).status == 409 &&
          service.workingCopy(navigationDraft.id).generation == 3,
          "annotation archived History accepted a boundary beyond original Commit");

    // Archived Undo followed by a NEW annotation edit creates an active
    // journal whose logical parent is the verified archive boundary, not
    // the current immutable HEAD. Undo/Redo must preserve that provenance
    // and never resurrect the abandoned archived Redo action.
    const native::WorkingCopyInfo forkDraft = service.createWorkingCopy(
        project.id, "main", secondRevision, session.toStdString());
    const native::WorkingCopyLease forkLease = service.acquireWorkingCopyLease(
        forkDraft.id, session.toStdString(), 90);
    const QString forkPath = QStringLiteral("/dvmdb/v1/working-copies/") +
        QString::fromStdString(forkDraft.id);
    const QString forkToken = QString::fromStdString(forkLease.token);
    const std::function<QJsonObject(const QString&, const QString&, const QString&, bool)> forkNavigate =
        [&session, &forkToken, &secondCommittedRevision](
                const QString& navigationId, const QString& generation,
                const QString& position, bool archivedSource) -> QJsonObject {
            QJsonObject body{{"operationId", navigationId}, {"sessionId", session},
                {"fencingToken", forkToken}, {"expectedGeneration", generation},
                {"expectedHead", secondCommittedRevision}, {"actionPosition", position}};
            if (archivedSource) body.insert(QStringLiteral("revisionId"), secondCommittedRevision);
            return body;
        };
    const Response forkSelected = client.call("POST", forkPath + "/history/navigate",
        forkNavigate(QString::fromStdString(native::UuidV7::generate().toString()), "0", "1", true));
    MapAnnotation forkValue = annotation;
    forkValue.text = QStringLiteral("Forked annotation action");
    const QByteArray forkImage = native::ModelCodec<MapAnnotation>::encode(forkValue);
    const Response forkSaved = client.call("POST", forkPath + "/save-events",
        QJsonObject{{"sessionId", session}, {"fencingToken", forkToken},
            {"expectedGeneration", "1"}, {"annotationLayerId", layerId},
            {"changes", QJsonArray{}},
            {"events", QJsonArray{QString::fromLatin1(
                frame(annotation, initial, forkImage).toBase64())}}});
    const native::WorkingCopyInfo forkInfo = service.workingCopy(forkDraft.id);
    check(forkSelected.status == 200 && forkSaved.status == 200 &&
          forkSaved.json.value("generation") == "2" &&
          forkInfo.historyForkRevisionId == secondCommittedRevision.toStdString() &&
          forkInfo.historyForkActionPosition == 1 &&
          forkInfo.historyForkArchiveCommitOperationId == secondOperation.toStdString() &&
          service.optionalHead(project.id, "main") == secondRevision,
          "new annotation edit did not record authenticated archived-fork provenance");
    const QString forkUndoId = QString::fromStdString(native::UuidV7::generate().toString());
    const QJsonObject forkUndoRequest = forkNavigate(forkUndoId, "2", "0", false);
    const Response forkUndone = client.call("POST", forkPath + "/history/navigate", forkUndoRequest);
    const Response forkUndoLayer = client.call("GET", forkPath + "/annotation-layers/" + layerId);
    check(forkUndone.status == 200 && forkUndone.json.value("generation") == "3" &&
          forkUndone.json.value("draftActionPosition") == "0" &&
          forkUndoLayer.json.value("annotations").toArray().first().toObject()
              .value("payloadBase64").toString().toLatin1() == initial.toBase64() &&
          client.call("POST", forkPath + "/history/navigate", forkUndoRequest).status == 200 &&
          service.workingCopy(forkDraft.id).generation == 3 &&
          service.optionalHead(project.id, "main") == secondRevision,
          "active fork Undo did not restore the verified archive boundary");
    const Response forkRedone = client.call("POST", forkPath + "/history/navigate",
        forkNavigate(QString::fromStdString(native::UuidV7::generate().toString()), "3", "1", false));
    const Response forkRedoLayer = client.call("GET", forkPath + "/annotation-layers/" + layerId);
    check(forkRedone.status == 200 && forkRedone.json.value("generation") == "4" &&
          forkRedoLayer.json.value("annotations").toArray().first().toObject()
              .value("payloadBase64").toString().toLatin1() == forkImage.toBase64() &&
          client.call("POST", forkPath + "/history/navigate",
              forkNavigate(QString::fromStdString(native::UuidV7::generate().toString()),
                           "4", "2", false)).status == 409 &&
          service.optionalHead(project.id, "main") == secondRevision,
          "active fork Redo resurrected abandoned archived history");

    // B4.3 combined History: one immutable action changes both the
    // Distribution root's metadata and an annotation. The authoritative
    // MODEL archive (not a client-supplied classification) selects the mixed
    // verifier, and the common native History publisher fences its overlay.
    const std::function<std::optional<native::ModelRecord>(const native::Key&)> readSecond =
        [&service, &project, &secondRevision](const native::Key& key) {
            return service.getModelAtRevision(project.id, *secondRevision, key);
        };
    const std::optional<NetworkHydraulicDistribution> mixedBefore =
        native::hydraulic::loadDraftHydraulicDistributionNetwork(readSecond, QUuid(networkId));
    check(mixedBefore.has_value(), "mixed History lacks a committed Distribution root");
    NetworkHydraulicDistribution mixedAfter = *mixedBefore;
    mixedAfter.title_line_1 = QStringLiteral("Combined annotation and hydraulic History");
    const QByteArray rootBeforeImage = aowis::model::hydraulicDistributionRootImage(*mixedBefore);
    const QByteArray rootAfterImage = aowis::model::hydraulicDistributionRootImage(mixedAfter);
    MapAnnotation combinedAnnotation = changed;
    combinedAnnotation.text = QStringLiteral("Combined annotation action");
    const QByteArray combinedAnnotationImage = native::ModelCodec<MapAnnotation>::encode(combinedAnnotation);
    aowis::model::ModelEditTransaction combinedEvent;
    combinedEvent.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    combinedEvent.operation = QStringLiteral("hydraulic.annotation.combined");
    combinedEvent.edits.append(aowis::model::ModelEntityEdit{
        QUuid(networkId), QString::fromLatin1(aowis::model::hydraulicDistributionRootEditType),
        rootBeforeImage, rootAfterImage, std::nullopt});
    combinedEvent.edits.append(aowis::model::ModelEntityEdit{
        annotation.uuid, QStringLiteral("map.annotation"),
        revised, combinedAnnotationImage, layerUuid});
    const QByteArray combinedFrame = aowis::model::ModelEditTransactionCodec::encode(combinedEvent);
    check(!combinedFrame.isEmpty(), "could not encode mixed MODEL edit frame");
    const native::WorkingCopyInfo mixedDraft = service.createWorkingCopy(
        project.id, "main", secondRevision, session.toStdString());
    const native::WorkingCopyLease mixedLease = service.acquireWorkingCopyLease(
        mixedDraft.id, session.toStdString(), 90);
    const QString mixedPath = QStringLiteral("/dvmdb/v1/working-copies/") +
        QString::fromStdString(mixedDraft.id);
    const Response mixedSaved = client.call("POST", mixedPath + "/save-events",
        QJsonObject{{"sessionId", session},
            {"fencingToken", QString::fromStdString(mixedLease.token)},
            {"expectedGeneration", "0"}, {"annotationLayerId", layerId},
            {"changes", QJsonArray{}},
            {"events", QJsonArray{QString::fromLatin1(combinedFrame.toBase64())}}});
    check(mixedSaved.status == 200 && mixedSaved.json.value("generation") == "1",
          "mixed hydraulic/annotation MODEL event could not be saved atomically");
    const QString combinedOperation = QString::fromStdString(native::UuidV7::generate().toString());
    const Response mixedCommit = client.call("POST", commitPath,
        QJsonObject{{"operationId", combinedOperation},
            {"draftId", QString::fromStdString(mixedDraft.id)},
            {"sessionId", session},
            {"fencingToken", QString::fromStdString(mixedLease.token)},
            {"expectedGeneration", "1"}, {"expectedHead", secondCommittedRevision},
            {"author", Client::testAuthor()}, {"message", "Combined History proof"}});
    const QString mixedRevisionText = mixedCommit.json.value("revision").toString();
    const std::optional<native::RevisionId> mixedRevision =
        native::RevisionId::parse(mixedRevisionText.toStdString());
    check(mixedCommit.status == 200 && mixedRevision.has_value(),
          "mixed hydraulic/annotation local Commit failed");
    const std::optional<native::annotations::VerifiedIndexedAnnotationActionBoundary> mixedProof =
        native::annotations::verifyIndexedCombinedActionBoundary(
            service, project.id, "main", *mixedRevision, 1, &historyError);
    check(mixedProof && mixedProof->boundary.actionCount == 1 &&
          mixedProof->boundary.changedLayerIds.size() == 1 &&
          !native::annotations::verifyIndexedAnnotationActionBoundary(
              service, project.id, "main", *mixedRevision, 1, &historyError),
          "combined immutable action was not verified by the mixed-only replay engine");
    const native::WorkingCopyInfo combinedNavigation = service.createWorkingCopy(
        project.id, "main", mixedRevision, session.toStdString());
    const native::WorkingCopyLease combinedLease = service.acquireWorkingCopyLease(
        combinedNavigation.id, session.toStdString(), 90);
    const QString combinedPath = QStringLiteral("/dvmdb/v1/working-copies/") +
        QString::fromStdString(combinedNavigation.id);
    const std::function<QJsonObject(const QString&, const QString&, const QString&)> combinedRequest =
        [&](const QString& operationId, const QString& generation, const QString& boundary) {
            return QJsonObject{{"operationId", operationId},
                {"sessionId", session},
                {"fencingToken", QString::fromStdString(combinedLease.token)},
                {"expectedGeneration", generation}, {"expectedHead", mixedRevisionText},
                {"revisionId", mixedRevisionText}, {"actionPosition", boundary}};
        };
    const QString combinedNavigateOperation =
        QString::fromStdString(native::UuidV7::generate().toString());
    const QJsonObject combinedZeroRequest = combinedRequest(combinedNavigateOperation, "0", "0");
    const Response combinedZero = client.call("POST", combinedPath + "/history/navigate",
                                              combinedZeroRequest);
    const Response combinedZeroLayer = client.call("GET", combinedPath + "/annotation-layers/" + layerId);
    const Response combinedZeroNetwork = client.call("GET", combinedPath + "/networks/" + networkId);
    const std::optional<NetworkHydraulicDistribution> restoredBefore =
        native::ModelCodec<NetworkHydraulicDistribution>::decode(
            QByteArray::fromBase64(combinedZeroNetwork.json.value("payloadBase64").toString().toLatin1()));
    check(combinedZero.status == 200 && combinedZero.json.value("generation") == "1" &&
          combinedZeroLayer.status == 200 && combinedZeroNetwork.status == 200 &&
          restoredBefore && restoredBefore->title_line_1 == mixedBefore->title_line_1 &&
          !combinedZeroLayer.json.value("annotations").toArray().isEmpty() &&
          combinedZeroLayer.json.value("annotations").toArray().first().toObject()
              .value("payloadBase64").toString().toLatin1() == revised.toBase64() &&
          service.optionalHead(project.id, "main") == mixedRevision,
          "combined action boundary 0 failed to restore hydraulic and annotation values");
    check(client.call("POST", combinedPath + "/history/navigate", combinedZeroRequest).status == 200 &&
          service.workingCopy(combinedNavigation.id).generation == 1,
          "combined History lost-response retry changed the generation twice");
    const Response combinedOne = client.call("POST", combinedPath + "/history/navigate",
        combinedRequest(QString::fromStdString(native::UuidV7::generate().toString()), "1", "1"));
    const Response combinedOneLayer = client.call("GET", combinedPath + "/annotation-layers/" + layerId);
    const Response combinedOneNetwork = client.call("GET", combinedPath + "/networks/" + networkId);
    const std::optional<NetworkHydraulicDistribution> restoredAfter =
        native::ModelCodec<NetworkHydraulicDistribution>::decode(
            QByteArray::fromBase64(combinedOneNetwork.json.value("payloadBase64").toString().toLatin1()));
    check(combinedOne.status == 200 && combinedOne.json.value("generation") == "2" &&
          !combinedOne.json.value("dirty").toBool() &&
          combinedOneLayer.status == 200 && combinedOneNetwork.status == 200 &&
          restoredAfter && restoredAfter->title_line_1 == mixedAfter.title_line_1 &&
          !combinedOneLayer.json.value("annotations").toArray().isEmpty() &&
          combinedOneLayer.json.value("annotations").toArray().first().toObject()
              .value("payloadBase64").toString().toLatin1() == combinedAnnotationImage.toBase64() &&
          service.optionalHead(project.id, "main") == mixedRevision,
          "combined action boundary 1 failed to restore committed hydraulic and annotation values");
    check(client.call("POST", combinedPath + "/history/navigate",
              combinedRequest(QString::fromStdString(native::UuidV7::generate().toString()),
                              "0", "0")).status == 409 &&
          service.workingCopy(combinedNavigation.id).generation == 2,
          "combined History accepted stale draft generation");

    // B4.3 active annotation History: the retained MODEL journal is verified
    // against the currently saved boundary, not just against the journal tip.
    // This independent draft does not publish a new immutable revision.
    const native::WorkingCopyInfo liveAnnotation = service.createWorkingCopy(
        project.id, "main", mixedRevision, session.toStdString());
    const native::WorkingCopyLease liveAnnotationLease = service.acquireWorkingCopyLease(
        liveAnnotation.id, session.toStdString(), 90);
    const QString liveAnnotationPath = QStringLiteral("/dvmdb/v1/working-copies/") +
        QString::fromStdString(liveAnnotation.id);
    const QString liveAnnotationToken = QString::fromStdString(liveAnnotationLease.token);
    MapAnnotation liveValue = combinedAnnotation;
    liveValue.text = QStringLiteral("Live annotation Undo");
    const QByteArray liveImage = native::ModelCodec<MapAnnotation>::encode(liveValue);
    const std::function<QJsonObject(const QString&, const QByteArray&)> liveSave =
        [&session, &liveAnnotationToken, &layerId](const QString& generation,
                                                    const QByteArray& event) {
            return QJsonObject{{"sessionId", session}, {"fencingToken", liveAnnotationToken},
                {"expectedGeneration", generation}, {"annotationLayerId", layerId},
                {"changes", QJsonArray{}},
                {"events", QJsonArray{QString::fromLatin1(event.toBase64())}}};
        };
    const std::function<QJsonObject(const QString&, const QString&, const QString&)> liveNavigate =
        [&session, &liveAnnotationToken, &mixedRevisionText](
                const QString& operationId, const QString& generation, const QString& position) {
            return QJsonObject{{"operationId", operationId},
                {"sessionId", session}, {"fencingToken", liveAnnotationToken},
                {"expectedGeneration", generation}, {"expectedHead", mixedRevisionText},
                {"actionPosition", position}};
        };
    check(client.call("POST", liveAnnotationPath + "/save-events",
              liveSave("0", frame(annotation, combinedAnnotationImage, liveImage))).status == 200,
          "active annotation-only journal Save failed");
    const QString annotationUndoId = QString::fromStdString(native::UuidV7::generate().toString());
    const QJsonObject annotationUndo = liveNavigate(annotationUndoId, "1", "0");
    const Response annotationUndone = client.call("POST", liveAnnotationPath + "/history/navigate",
                                                        annotationUndo);
    const Response annotationUndoLayer = client.call("GET", liveAnnotationPath +
                                                     "/annotation-layers/" + layerId);
    check(annotationUndone.status == 200 && annotationUndone.json.value("generation") == "2" &&
          annotationUndone.json.value("draftActionPosition") == "0" &&
          annotationUndoLayer.json.value("annotations").toArray().first().toObject()
              .value("payloadBase64").toString().toLatin1() == combinedAnnotationImage.toBase64() &&
          client.call("POST", liveAnnotationPath + "/history/navigate", annotationUndo).status == 200 &&
          service.workingCopy(liveAnnotation.id).generation == 2,
          "active annotation-only Undo or idempotent retry failed");
    check(client.call("POST", liveAnnotationPath + "/history/navigate",
              liveNavigate(QString::fromStdString(native::UuidV7::generate().toString()),
                           "1", "1")).status == 409,
          "active annotation Undo accepted a stale generation");
    const Response annotationRedone = client.call("POST", liveAnnotationPath + "/history/navigate",
        liveNavigate(QString::fromStdString(native::UuidV7::generate().toString()), "2", "1"));
    const Response annotationRedoLayer = client.call("GET", liveAnnotationPath +
                                                      "/annotation-layers/" + layerId);
    check(annotationRedone.status == 200 && annotationRedone.json.value("generation") == "3" &&
          annotationRedoLayer.json.value("annotations").toArray().first().toObject()
              .value("payloadBase64").toString().toLatin1() == liveImage.toBase64() &&
          service.optionalHead(project.id, "main") == mixedRevision,
          "active annotation-only Redo changed committed HEAD or lost the saved image");

    // Two live actions, the second touching both domains. After Undo, the
    // journal still contains the second action; its authenticated before
    // images are replayed from HEAD even though the saved candidate is at 1.
    const native::WorkingCopyInfo liveCombined = service.createWorkingCopy(
        project.id, "main", mixedRevision, session.toStdString());
    const native::WorkingCopyLease liveCombinedLease = service.acquireWorkingCopyLease(
        liveCombined.id, session.toStdString(), 90);
    const QString liveCombinedPath = QStringLiteral("/dvmdb/v1/working-copies/") +
        QString::fromStdString(liveCombined.id);
    const QString liveCombinedToken = QString::fromStdString(liveCombinedLease.token);
    const std::function<QJsonObject(const QString&, const QByteArray&)> combinedLiveSave =
        [&session, &liveCombinedToken, &layerId](const QString& generation,
                                                const QByteArray& event) {
            return QJsonObject{{"sessionId", session}, {"fencingToken", liveCombinedToken},
                {"expectedGeneration", generation}, {"annotationLayerId", layerId},
                {"changes", QJsonArray{}},
                {"events", QJsonArray{QString::fromLatin1(event.toBase64())}}};
        };
    const std::function<QJsonObject(const QString&, const QString&, const QString&)> combinedLiveNav =
        [&session, &liveCombinedToken, &mixedRevisionText](
                const QString& operationId, const QString& generation, const QString& position) {
            return QJsonObject{{"operationId", operationId}, {"sessionId", session},
                {"fencingToken", liveCombinedToken}, {"expectedGeneration", generation},
                {"expectedHead", mixedRevisionText}, {"actionPosition", position}};
        };
    MapAnnotation mixedLiveFirst = combinedAnnotation;
    mixedLiveFirst.text = QStringLiteral("Live mixed stage 1");
    const QByteArray mixedLiveFirstImage = native::ModelCodec<MapAnnotation>::encode(mixedLiveFirst);
    MapAnnotation mixedLiveLast = mixedLiveFirst;
    mixedLiveLast.text = QStringLiteral("Live mixed stage 2");
    const QByteArray mixedLiveLastImage = native::ModelCodec<MapAnnotation>::encode(mixedLiveLast);
    NetworkHydraulicDistribution mixedLiveNetwork = mixedAfter;
    mixedLiveNetwork.title_line_1 = QStringLiteral("Live mixed hydraulic stage 2");
    aowis::model::ModelEditTransaction mixedLiveEvent;
    mixedLiveEvent.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    mixedLiveEvent.operation = QStringLiteral("hydraulic.annotation.live");
    mixedLiveEvent.edits.append(aowis::model::ModelEntityEdit{
        QUuid(networkId), QString::fromLatin1(aowis::model::hydraulicDistributionRootEditType),
        aowis::model::hydraulicDistributionRootImage(mixedAfter),
        aowis::model::hydraulicDistributionRootImage(mixedLiveNetwork), std::nullopt});
    mixedLiveEvent.edits.append(aowis::model::ModelEntityEdit{
        annotation.uuid, QStringLiteral("map.annotation"),
        mixedLiveFirstImage, mixedLiveLastImage, layerUuid});
    const QByteArray mixedLiveFrame = aowis::model::ModelEditTransactionCodec::encode(mixedLiveEvent);
    check(client.call("POST", liveCombinedPath + "/save-events",
              combinedLiveSave("0", frame(annotation, combinedAnnotationImage,
                                          mixedLiveFirstImage))).status == 200 &&
          client.call("POST", liveCombinedPath + "/save-events",
              combinedLiveSave("1", mixedLiveFrame)).status == 200,
          "active mixed MODEL journal could not save both actions");
    const Response mixedLiveUndo = client.call("POST", liveCombinedPath + "/history/navigate",
        combinedLiveNav(QString::fromStdString(native::UuidV7::generate().toString()), "2", "1"));
    const Response mixedLiveUndoLayer = client.call("GET", liveCombinedPath +
                                                    "/annotation-layers/" + layerId);
    const Response mixedLiveUndoNetwork = client.call("GET", liveCombinedPath +
                                                      "/networks/" + networkId);
    const std::optional<NetworkHydraulicDistribution> mixedLiveUndoneNetwork =
        native::ModelCodec<NetworkHydraulicDistribution>::decode(
            QByteArray::fromBase64(mixedLiveUndoNetwork.json.value("payloadBase64")
                .toString().toLatin1()));
    check(mixedLiveUndo.status == 200 && mixedLiveUndo.json.value("generation") == "3" &&
          mixedLiveUndo.json.value("draftActionPosition") == "1" &&
          mixedLiveUndoLayer.json.value("annotations").toArray().first().toObject()
              .value("payloadBase64").toString().toLatin1() == mixedLiveFirstImage.toBase64() &&
          mixedLiveUndoneNetwork && mixedLiveUndoneNetwork->title_line_1 == mixedAfter.title_line_1,
          "active mixed Undo failed to reconstruct saved journal boundary 1");
    const Response mixedLiveRedo = client.call("POST", liveCombinedPath + "/history/navigate",
        combinedLiveNav(QString::fromStdString(native::UuidV7::generate().toString()), "3", "2"));
    const Response mixedLiveRedoNetwork = client.call("GET", liveCombinedPath +
                                                      "/networks/" + networkId);
    const std::optional<NetworkHydraulicDistribution> mixedLiveRedoneNetwork =
        native::ModelCodec<NetworkHydraulicDistribution>::decode(
            QByteArray::fromBase64(mixedLiveRedoNetwork.json.value("payloadBase64")
                .toString().toLatin1()));
    check(mixedLiveRedo.status == 200 && mixedLiveRedo.json.value("generation") == "4" &&
          mixedLiveRedoneNetwork &&
          mixedLiveRedoneNetwork->title_line_1 == mixedLiveNetwork.title_line_1,
          "active mixed Redo failed after previously selected journal boundary");
    const Response mixedLiveZero = client.call("POST", liveCombinedPath + "/history/navigate",
        combinedLiveNav(QString::fromStdString(native::UuidV7::generate().toString()), "4", "0"));
    const Response mixedLiveZeroLayer = client.call("GET", liveCombinedPath +
                                                    "/annotation-layers/" + layerId);
    check(mixedLiveZero.status == 200 && mixedLiveZero.json.value("generation") == "5" &&
          mixedLiveZeroLayer.json.value("annotations").toArray().first().toObject()
              .value("payloadBase64").toString().toLatin1() == combinedAnnotationImage.toBase64() &&
          service.optionalHead(project.id, "main") == mixedRevision,
          "active mixed boundary zero did not restore immutable HEAD image");
    const Response mixedLiveOne = client.call("POST", liveCombinedPath + "/history/navigate",
        combinedLiveNav(QString::fromStdString(native::UuidV7::generate().toString()), "5", "1"));
    MapAnnotation forkedLive = mixedLiveFirst;
    forkedLive.text = QStringLiteral("New edit abandons mixed Redo");
    const Response newBranchAction = client.call("POST", liveCombinedPath + "/save-events",
        combinedLiveSave("6", frame(annotation, mixedLiveFirstImage,
                                    native::ModelCodec<MapAnnotation>::encode(forkedLive))));
    check(mixedLiveOne.status == 200 && mixedLiveOne.json.value("generation") == "6" &&
          newBranchAction.status == 200 && newBranchAction.json.value("generation") == "7" &&
          client.call("GET", liveCombinedPath + "/history/cursor").json
              .value("journalCount") == "2" &&
          client.call("POST", liveCombinedPath + "/history/navigate",
              combinedLiveNav(QString::fromStdString(native::UuidV7::generate().toString()),
                              "7", "3")).status == 409 &&
          service.optionalHead(project.id, "main") == mixedRevision,
          "active mixed new edit failed to invalidate obsolete Redo tail");

    // A direct native/library Save can bypass the HTTP event validator.
    // A new draft with an orphaned annotation must be refused at HTTP Commit,
    // without changing the immutable branch HEAD or the saved draft.
    MapAnnotation unowned = annotation;
    unowned.uuid = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    native::ModelRecord orphan;
    orphan.kind = native::annotations::annotationMemberKind;
    orphan.payload = native::record::bytesFromQByteArray(
        native::ModelCodec<MapAnnotation>::encode(unowned));
    // The combined History Commit advanced HEAD beyond secondRevision. The
    // invalid-candidate regression must start from the current immutable HEAD,
    // otherwise draft creation fails on stale-base fencing before reaching
    // the annotation integrity gate it is intended to exercise.
    const std::optional<native::RevisionId> base = mixedRevision;
    check(base.has_value() && service.optionalHead(project.id, "main") == base,
          "orphan annotation fixture must start at the combined Commit HEAD");
    const native::WorkingCopyInfo unsafe = service.createWorkingCopy(
        project.id, "main", base, session.toStdString());
    const native::WorkingCopyLease unsafeLease = service.acquireWorkingCopyLease(
        unsafe.id, session.toStdString(), 90);
    const native::WorkingCopyInfo injected = service.saveWorkingCopyChanges(
        unsafe.id, session.toStdString(), unsafeLease.token, 0,
        {native::ModelChange{native::record::keyFromUuid(unowned.uuid), orphan}});
    check(injected.generation == 1,
          "could not set up direct native annotation corruption fixture");
    const Response blockedCommit = client.call("POST", commitPath,
        QJsonObject{{"operationId", QString::fromStdString(native::UuidV7::generate().toString())},
                    {"draftId", QString::fromStdString(unsafe.id)},
                    {"sessionId", session},
                    {"fencingToken", QString::fromStdString(unsafeLease.token)},
                    {"expectedGeneration", "1"},
                    {"expectedHead", mixedRevisionText},
                    {"author", Client::testAuthor()},
                    {"message", "Must not publish unowned annotation"}});
    check(blockedCommit.status == 400 &&
          service.workingCopy(unsafe.id).generation == 1 &&
          service.optionalHead(project.id, "main") == base,
          "HTTP local Commit published an invalid direct-native annotation candidate");
    server.stop();
}

// An unborn draft has an unjournaled network+annotation-layer seed. History
// must preserve that seed at action zero instead of treating it as an edit.
void exerciseUnbornAnnotationHistoryHttp() {
    namespace native = aowis::dvmdb;
    TemporaryRoot root;
    native::ProjectService service(root.root());
    const native::ProjectInfo project = service.createUnbornProject("unborn annotation history");
    service.configureRepositoryRole(native::RepositoryRole::Private);
    TestServer server(service, native::BundleLimits{4ULL * 1024ULL * 1024ULL,
                                                    10000ULL, 1024ULL * 1024ULL});
    Client client(server.port());
    const QString session = QString::fromStdString(native::UuidV7::generate().toString());
    const QString branchId = QString::fromStdString(service.branchId(project.id, "main")->toString());
    const QString networkId = QString::fromStdString(native::UuidV7::generate().toString());
    const QString layerId = native::annotations::distributionLayerUuid(QUuid(networkId))
        .toString(QUuid::WithoutBraces);
    const Response created = client.call("POST", "/dvmdb/v1/working-copies",
        QJsonObject{{"projectId", QString::fromStdString(project.id)},
                    {"branchId", branchId}, {"baseRevision", QJsonValue::Null},
                    {"sessionId", session}});
    check(created.status == 201, "unborn annotation History draft creation failed");
    const QString draftPath = QStringLiteral("/dvmdb/v1/working-copies/") +
        created.json.value("id").toString();
    const QString layerPath = draftPath + QStringLiteral("/annotation-layers/") + layerId;
    const Response lease = client.call("POST", draftPath + "/lease",
        QJsonObject{{"sessionId", session}, {"seconds", 90}});
    check(lease.status == 200, "unborn annotation History lease failed");
    const QString token = lease.json.value("token").toString();
    const Response seed = client.call("POST", draftPath + "/initialize-network",
        QJsonObject{{"sessionId", session}, {"fencingToken", token},
                    {"expectedGeneration", "0"}, {"networkId", networkId},
                    {"name", "Unborn annotation seed"}});
    check(seed.status == 200 && seed.json.value("generation") == "1" &&
          seed.json.value("events").toArray().isEmpty() &&
          client.call("GET", layerPath).json.value("count") == 0,
          "unborn annotation History seed was not unjournaled");
    MapAnnotation annotation;
    annotation.uuid = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    annotation.text = QStringLiteral("Unborn create");
    annotation.position_wgs84.longitude_deg = 14.1;
    annotation.position_wgs84.latitude_deg = 11.2;
    const QByteArray createdImage = native::ModelCodec<MapAnnotation>::encode(annotation);
    MapAnnotation revised = annotation;
    revised.text = QStringLiteral("Unborn updated");
    const QByteArray revisedImage = native::ModelCodec<MapAnnotation>::encode(revised);
    check(!createdImage.isEmpty() && !revisedImage.isEmpty(),
          "unborn annotation History MODEL images invalid");
    const std::function<QJsonObject(const QString&, const std::optional<QByteArray>&,
                                   const std::optional<QByteArray>&)> save =
        [&session, &token, &layerId, &annotation](
            const QString& generation, const std::optional<QByteArray>& before,
            const std::optional<QByteArray>& after) -> QJsonObject {
            aowis::model::ModelEditTransaction transaction;
            transaction.transactionId = QUuid(QString::fromStdString(
                aowis::dvmdb::UuidV7::generate().toString()));
            transaction.operation = QStringLiteral("map.annotation.edit");
            transaction.edits.append(aowis::model::ModelEntityEdit{
                annotation.uuid, QStringLiteral("map.annotation"), before, after, QUuid(layerId)});
            const QByteArray frame = aowis::model::ModelEditTransactionCodec::encode(transaction);
            check(!frame.isEmpty(), "unborn annotation History event encoding failed");
            return QJsonObject{{"sessionId", session}, {"fencingToken", token},
                {"expectedGeneration", generation}, {"annotationLayerId", layerId},
                {"changes", QJsonArray{}},
                {"events", QJsonArray{QString::fromLatin1(frame.toBase64())}}};
        };
    const Response createdAction = client.call("POST", draftPath + "/save-events",
        save("1", std::nullopt, createdImage));
    const Response updatedAction = client.call("POST", draftPath + "/save-events",
        save("2", createdImage, revisedImage));
    check(createdAction.status == 200 && updatedAction.status == 200 &&
          updatedAction.json.value("generation") == "3" &&
          !service.optionalHead(project.id, "main"),
          "unborn annotation History events were not saved against null HEAD");
    const std::function<QJsonObject(const QString&, const QString&, const QString&)> navigate =
        [&session, &token](const QString& operation, const QString& generation,
                           const QString& position) -> QJsonObject {
            return QJsonObject{{"operationId", operation}, {"sessionId", session},
                {"fencingToken", token}, {"expectedGeneration", generation},
                {"expectedHead", QJsonValue::Null}, {"actionPosition", position}};
        };
    const Response cursor = client.call("GET", draftPath + "/history/cursor");
    check(cursor.status == 200 && cursor.json.value("actionPosition") == "2" &&
          cursor.json.value("previousAction").toObject().value("actionPosition") == "1",
          "unborn annotation History cursor did not offer provable Undo");
    const QString firstOperation = QString::fromStdString(native::UuidV7::generate().toString());
    const QJsonObject firstUndo = navigate(firstOperation, "3", "1");
    const Response undone = client.call("POST", draftPath + "/history/navigate", firstUndo);
    check(undone.status == 200 && undone.json.value("generation") == "4" &&
          undone.json.value("draftActionPosition") == "1" &&
          client.call("POST", draftPath + "/history/navigate", firstUndo).json.value("generation") == "4" &&
          client.call("GET", layerPath).json.value("annotations").toArray().first()
              .toObject().value("payloadBase64").toString().toLatin1() == createdImage.toBase64(),
          "unborn annotation Undo or lost-response retry failed");
    check(client.call("POST", draftPath + "/history/navigate",
          navigate(QString::fromStdString(native::UuidV7::generate().toString()), "3", "0")).status == 409,
          "unborn annotation History accepted stale generation");
    const Response zero = client.call("POST", draftPath + "/history/navigate",
        navigate(QString::fromStdString(native::UuidV7::generate().toString()), "4", "0"));
    const Response emptyLayer = client.call("GET", layerPath);
    const Response zeroCursor = client.call("GET", draftPath + "/history/cursor");
    check(zero.status == 200 && zero.json.value("generation") == "5" &&
          emptyLayer.status == 200 && emptyLayer.json.value("count") == 0 &&
          zeroCursor.status == 200 && zeroCursor.json.value("previousAction").isNull() &&
          zeroCursor.json.value("nextAction").toObject().value("actionPosition") == "1" &&
          !service.optionalHead(project.id, "main"),
          "unborn annotation boundary zero did not preserve initialization seed");
    const Response redo = client.call("POST", draftPath + "/history/navigate",
        navigate(QString::fromStdString(native::UuidV7::generate().toString()), "5", "2"));
    check(redo.status == 200 && redo.json.value("generation") == "6" &&
          client.call("GET", layerPath).json.value("annotations").toArray().first()
              .toObject().value("payloadBase64").toString().toLatin1() == revisedImage.toBase64() &&
          !service.optionalHead(project.id, "main"),
          "unborn annotation Redo failed to reproduce native snapshot");
    const Response backToFirst = client.call("POST", draftPath + "/history/navigate",
        navigate(QString::fromStdString(native::UuidV7::generate().toString()), "6", "1"));
    MapAnnotation alternative = annotation;
    alternative.text = QStringLiteral("Unborn new edit abandons Redo");
    const QByteArray alternativeImage = native::ModelCodec<MapAnnotation>::encode(alternative);
    const Response replaceRedo = client.call("POST", draftPath + "/save-events",
        save("7", createdImage, alternativeImage));
    const Response forkedCursor = client.call("GET", draftPath + "/history/cursor");
    check(backToFirst.status == 200 && backToFirst.json.value("generation") == "7" &&
          replaceRedo.status == 200 && replaceRedo.json.value("generation") == "8" &&
          forkedCursor.json.value("journalCount") == "2" &&
          client.call("POST", draftPath + "/history/navigate",
              navigate(QString::fromStdString(native::UuidV7::generate().toString()),
                       "8", "3")).status == 409 &&
          !service.optionalHead(project.id, "main"),
          "unborn annotation new edit did not abandon the Redo suffix");
    const Response deleted = client.call("POST", draftPath + "/save-events",
        save("8", alternativeImage, std::nullopt));
    check(deleted.status == 200 && deleted.json.value("generation") == "9" &&
          client.call("GET", draftPath + "/history/cursor").json
              .value("previousAction").isNull() &&
          !client.call("GET", draftPath + "/history/cursor").json
              .value("hasPreviousDraftAction").toBool() &&
          client.call("POST", draftPath + "/history/navigate",
              navigate(QString::fromStdString(native::UuidV7::generate().toString()),
                       "9", "2")).status == 409 &&
          client.call("GET", layerPath).json.value("count") == 0 &&
          !service.optionalHead(project.id, "main"),
          "unborn deleted-member History fabricated missing membership provenance");
}

void exerciseCompleteNetworkDraftImportHttp() {
    namespace native = aowis::dvmdb;
    TemporaryRoot root;
    std::string draftId;
    QString networkId;
    {
        native::ProjectService service(root.root());
        const native::ProjectInfo project = service.createUnbornProject("imported distribution");
        service.configureRepositoryRole(native::RepositoryRole::Private);
        TestServer server(service, native::BundleLimits{4ULL * 1024ULL * 1024ULL,
                                                       10000ULL, 1024ULL * 1024ULL});
        Client client(server.port());
        const QString session = QString::fromStdString(native::UuidV7::generate().toString());
        const QString branchId = QString::fromStdString(service.branchId(project.id, "main")->toString());
        const Response created = client.call("POST", "/dvmdb/v1/working-copies",
            QJsonObject{{"projectId", QString::fromStdString(project.id)},
                        {"branchId", branchId}, {"baseRevision", QJsonValue::Null},
                        {"sessionId", session}});
        check(created.status == 201, "could not create an empty draft for full-network import");
        draftId = created.json.value("id").toString().toStdString();
        const QString path = "/dvmdb/v1/working-copies/" + QString::fromStdString(draftId);
        const Response leased = client.call("POST", path + "/lease",
            QJsonObject{{"sessionId", session}, {"seconds", 90}});
        check(leased.status == 200, "could not acquire full-network import lease");
        NetworkHydraulicDistribution imported;
        imported.uuid = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
        imported.id = QStringLiteral("imported-water");
        HydraulicNodeJunction junction;
        junction.uuid = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
        junction.id = QStringLiteral("J-01");
        junction.elevation_m = 17;
        imported.nodes_junctions.append(junction);
        networkId = imported.uuid.toString(QUuid::WithoutBraces);
        const QByteArray payload = native::ModelCodec<NetworkHydraulicDistribution>::encode(imported);
        const QJsonObject request{{"sessionId", session},
            {"fencingToken", leased.json.value("token")},
            {"expectedGeneration", QStringLiteral("0")},
            {"payloadBase64", QString::fromLatin1(payload.toBase64())}};
        QJsonObject invalid = request;
        invalid.insert("payloadBase64", "%%%%");
        check(client.call("POST", path + "/import-network", invalid).status == 400,
              "full draft import accepted invalid canonical payload");
        check(client.call("GET", path).json.value("generation") == "0",
              "invalid full-network import changed the draft");
        const Response stored = client.call("POST", path + "/import-network", request);
        check(stored.status == 200 && stored.json.value("generation") == "1" &&
              stored.json.value("dirty").toBool() &&
              !service.optionalHead(project.id, "main").has_value(),
              "full-network import did not atomically save a private unborn candidate");
        const Response repeat = client.call("POST", path + "/import-network", request);
        check(repeat.status == 409 &&
              repeat.json.value("error").toObject().value("code") == "conflict",
              "repeat import was not rejected as a conflict");
        NetworkHydraulicDistribution replacement = imported;
        replacement.nodes_junctions[0].elevation_m = 99;
        QJsonObject replacementRequest = request;
        replacementRequest.insert("expectedGeneration", QStringLiteral("1"));
        replacementRequest.insert("payloadBase64", QString::fromLatin1(
            native::ModelCodec<NetworkHydraulicDistribution>::encode(replacement).toBase64()));
        check(client.call("POST", path + "/import-network", replacementRequest).status == 409,
              "populated private HEAD accepted a replacement network");
        const Response summary = client.call("GET", path + "/networks/summary");
        check(summary.status == 200 && summary.json.value("networkCount") == 1 &&
              summary.json.value("networks").toArray().first().toObject()
                  .value("counts").toObject().value("junctions") == 1,
              "full import did not materialize all network collections");
        const Response loaded = client.call("GET", path + "/networks/" + networkId);
        const std::optional<NetworkHydraulicDistribution> restored = native::ModelCodec<NetworkHydraulicDistribution>::decode(
            QByteArray::fromBase64(loaded.json.value("payloadBase64").toString().toLatin1()));
        check(loaded.status == 200 && restored && restored->nodes_junctions.size() == 1
              && restored->nodes_junctions.first().uuid == junction.uuid &&
              restored->nodes_junctions.first().elevation_m == 17 &&
              loaded.json.value("generation") == "1",
              "rejected replacement altered the saved imported hydraulic model");
        server.stop();
    }
    {
        native::ProjectService reopened(root.root());
        TestServer server(reopened, native::BundleLimits{4ULL * 1024ULL * 1024ULL,
                                                        10000ULL, 1024ULL * 1024ULL});
        Client client(server.port());
        const QString path = "/dvmdb/v1/working-copies/" + QString::fromStdString(draftId);
        const Response summary = client.call("GET", path + "/networks/summary");
        check(summary.status == 200 && summary.json.value("networkCount") == 1 &&
              summary.json.value("generation") == "1" &&
              summary.json.value("networks").toArray().first().toObject()
                  .value("id") == networkId,
              "full-network import did not survive a DVMDB restart");
        server.stop();
    }
}

void exerciseHydraulicDraftMembershipHttp() {
    namespace native = aowis::dvmdb;
    TemporaryRoot root;
    native::ProjectService service(root.root());
    const native::ProjectInfo project = service.createUnbornProject("draft-membership-test");
    const native::ProjectId projectUuid = *native::ProjectId::parse(project.id);
    const QUuid networkUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    const QUuid oldUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    const QUuid newUuid(QString::fromStdString(native::UuidV7::generate().toString()));
    const QString networkId = networkUuid.toString(QUuid::WithoutBraces);
    const QString oldId = oldUuid.toString(QUuid::WithoutBraces);
    const QString newId = newUuid.toString(QUuid::WithoutBraces);
    NetworkHydraulicDistribution network;
    network.uuid = networkUuid;
    network.id = QStringLiteral("draft-membership");
    HydraulicNodeJunction initial;
    initial.uuid = oldUuid;
    initial.id = QStringLiteral("J1");
    initial.elevation_m = 20;
    network.nodes_junctions.append(initial);
    // New projects have an unborn HEAD: establish a real empty model revision
    // before exercising the committed-network fixture.
    check(!service.optionalHead(project.id, "main").has_value(),
          "new hydraulic fixture project should have an unborn HEAD");
    const native::RevisionId head = service.commitModel(project.id, "main", {}, commitMetadata());
    check(service.optionalHead(project.id, "main") == head,
          "first hydraulic fixture commit did not establish the HEAD");
    native::RevisionId base = head;
    service.withRepository(project.id, [&](native::Repository& repository) {
        // A Distribution root and its mandatory annotation layer must be
        // published in ONE semantically validated native candidate.
        MapAnnotationLayer layer;
        layer.uuid = native::annotations::distributionLayerUuid(networkUuid);
        layer.domain = MapAnnotationDomain::Distribution;
        layer.owner_network_uuid = networkUuid;
        std::vector<native::ModelChange> changes =
            native::hydraulic::fullDraftHydraulicDistributionNetworkChanges(network);
        std::vector<native::ModelChange> layerChanges =
            native::annotations::fullDraftAnnotationLayerChanges(layer, {});
        changes.insert(changes.end(), layerChanges.begin(), layerChanges.end());
        base = repository.commitModel(projectUuid, "main",
            std::move(changes), commitMetadata(), head, true, std::nullopt,
            native::annotations::validateAnnotationSnapshot);
    });
    service.configureRepositoryRole(native::RepositoryRole::Private);
    TestServer server(service, native::BundleLimits{4ULL * 1024ULL * 1024ULL,
                                                     10000ULL, 1024ULL * 1024ULL});
    Client client(server.port());
    const QString session = QString::fromStdString(native::UuidV7::generate().toString());
    const QString listPath = QStringLiteral("/dvmdb/v1/working-copies");
    const Response created = client.call("POST", listPath,
        QJsonObject{{"projectId", QString::fromStdString(project.id)},
                    {"branchId", QString::fromStdString(service.branchId(project.id, "main")->toString())},
                    {"baseRevision", QString::fromStdString(base.toString())},
                    {"sessionId", session}});
    check(created.status == 201, "hydraulic draft checkout failed");
    const QString path = listPath + "/" + created.json.value("id").toString();
    const Response baseView = client.call("GET", path + "/networks/" + networkId);
    const std::optional<NetworkHydraulicDistribution> baseNetwork = native::ModelCodec<NetworkHydraulicDistribution>::decode(
        QByteArray::fromBase64(baseView.json.value("payloadBase64").toString().toLatin1()));
    check(baseView.status == 200 && baseView.json.value("generation") == "0" &&
          baseNetwork && baseNetwork->nodes_junctions.size() == 1 &&
          baseNetwork->nodes_junctions.first().uuid == oldUuid,
          "pinned committed base did not hydrate into draft");
    const Response leased = client.call("POST", path + "/lease",
                                        QJsonObject{{"sessionId", session}, {"seconds", 60}});
    check(leased.status == 200, "hydraulic draft lease failed");
    QString token = leased.json.value("token").toString();
    HydraulicNodeJunction added;
    added.uuid = newUuid;
    added.id = QStringLiteral("J2");
    added.elevation_m = 55;
    const QByteArray encoded = native::ModelCodec<HydraulicNodeJunction>::encode(added);
    const QJsonObject addition{{"uuid", newId}, {"kind", 201}, {"references", QJsonArray{}},
                               {"payloadBase64", QString::fromLatin1(encoded.toBase64())}};
    const QJsonObject insertion{{"networkId", networkId}, {"entityId", newId},
                                {"kind", 201}, {"collectionIndex", 9}, {"inserted", true}};
    const std::function<QString(const QUuid&, const QString&,
                                std::optional<QByteArray>, std::optional<QByteArray>)> eventFrame =
        [](const QUuid& member, const QString& action,
           std::optional<QByteArray> before, std::optional<QByteArray> after) {
        aowis::model::ModelEditTransaction transaction;
        transaction.transactionId = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
        transaction.operation = action;
        transaction.edits.append(aowis::model::ModelEntityEdit{
            member, QStringLiteral("HydraulicNodeJunction"), std::move(before), std::move(after)});
        QString encodeError;
        const QByteArray frame = aowis::model::ModelEditTransactionCodec::encode(
            transaction, &encodeError);
        if (frame.isEmpty())
            throw std::runtime_error("could not encode hydraulic member edit event: " +
                                     encodeError.toStdString());
        return QString::fromLatin1(frame.toBase64());
    };
    const QString firstEvent = eventFrame(newUuid, QStringLiteral("hydraulic.junction.create"),
                                          std::nullopt, encoded);
    const QJsonObject first{{"sessionId", session}, {"fencingToken", token},
                            {"expectedGeneration", "0"}, {"changes", QJsonArray{addition}},
                            {"events", QJsonArray{firstEvent}},
                            {"membership", QJsonArray{insertion}}};
    // A codec disagreement must expose the decoding reason rather than only
    // a generic invalid_request, while leaving the candidate unchanged.
    QJsonObject invalid = first;
    invalid.insert("events", QJsonArray{QString::fromLatin1(
        QByteArrayLiteral("invalid transaction framing").toBase64())});
    const Response malformedEvent = client.call("POST", path + "/save-events", invalid);
    check(malformedEvent.status == 400 &&
          malformedEvent.json.value("error").toObject().value("message").toString()
              .contains("Invalid edit transaction frame") &&
          client.call("GET", path).json.value("generation") == QStringLiteral("0"),
          "invalid MODEL event did not report codec reason without changing draft");
    invalid = first;
    invalid.insert("membership", QJsonArray{QJsonObject{
        {"networkId", networkId}, {"entityId", newId}, {"kind", 4},
        {"collectionIndex", 9}, {"inserted", true}}});
    check(client.call("POST", path + "/save-events", invalid).status == 400,
          "mismatched membership kind was accepted");
    invalid = first;
    invalid.insert("membership", QJsonArray{QJsonObject{
        {"networkId", networkId}, {"entityId", newId}, {"kind", 201},
        {"collectionIndex", 9}, {"inserted", false}}});
    check(client.call("POST", path + "/save-events", invalid).status == 400,
          "membership deletion without model tombstone accepted");
    invalid = first;
    invalid.insert("events", QJsonArray{eventFrame(
        newUuid, QStringLiteral("hydraulic.junction.create"), std::nullopt,
        QByteArrayLiteral("not a canonical junction"))});
    check(client.call("POST", path + "/save-events", invalid).status == 400 &&
          client.call("GET", path).json.value("generation") == QStringLiteral("0"),
          "mismatched MODEL after-image was published");
    // An entity creation without membership must never reach the candidate:
    // the entity would exist as a record that no membership chunk lists.
    invalid = first;
    invalid.remove("membership");
    const Response unlisted = client.call("POST", path + "/save-events", invalid);
    check(unlisted.status == 400 &&
          unlisted.json.value("error").toObject().value("message").toString()
              .contains("missing membership") &&
          client.call("GET", path).json.value("generation") == QStringLiteral("0") &&
          client.call("GET", path).json.value("changes").toArray().isEmpty(),
          "entity creation without membership was accepted");
    const Response saved = client.call("POST", path + "/save-events", first);
    check(saved.status == 200 && saved.json.value("generation") == QStringLiteral("1") &&
          saved.json.value("eventCount") == QStringLiteral("1"),
          "hydraulic entity and membership did not save atomically");
    const Response combinedView = client.call("GET", path + "/networks/" + networkId);
    const std::optional<NetworkHydraulicDistribution> combinedNetwork = native::ModelCodec<NetworkHydraulicDistribution>::decode(
        QByteArray::fromBase64(combinedView.json.value("payloadBase64").toString().toLatin1()));
    check(combinedView.status == 200 && combinedView.json.value("generation") == "1" &&
          combinedNetwork && combinedNetwork->nodes_junctions.size() == 2 &&
          combinedNetwork->nodes_junctions.first().uuid == oldUuid &&
          combinedNetwork->nodes_junctions.last().uuid == newUuid,
          "pinned base plus membership overlay did not hydrate");
    const Response replay = client.call("POST", path + "/save-events", first);
    check(replay.status == 200 && replay.json.value("generation") == QStringLiteral("1"),
          "hydraulic membership lost-response replay duplicated event");
    const QJsonObject details = client.call("GET", path).json;
    bool nativeChunk = false;
    for (const QJsonValue& value : details.value("changes").toArray())
        nativeChunk |= value.toObject().value("kind").toInt() == 101;
    check(nativeChunk && details.value("events").toArray().size() == 1 &&
          !details.value("changes").toArray().isEmpty(),
          "hydraulic membership chunk not included in durable candidate overlay");
    const QJsonObject deletion{{"uuid", oldId}, {"delete", true}};
    const QJsonObject removal{{"networkId", networkId}, {"entityId", oldId},
                              {"kind", 201}, {"collectionIndex", 9}, {"inserted", false}};
    const QString secondEvent = eventFrame(oldUuid, QStringLiteral("hydraulic.junction.delete"),
                                           native::ModelCodec<HydraulicNodeJunction>::encode(initial),
                                           std::nullopt);
    const QJsonObject second{{"sessionId", session}, {"fencingToken", token},
                             {"expectedGeneration", "1"}, {"changes", QJsonArray{deletion}},
                             {"events", QJsonArray{secondEvent}},
                             {"membership", QJsonArray{removal}}};
    QJsonObject unlistedRemoval = second;
    unlistedRemoval.remove("membership");
    check(client.call("POST", path + "/save-events", unlistedRemoval).status == 400 &&
          client.call("GET", path).json.value("generation") == QStringLiteral("1"),
          "entity removal without membership was accepted");
    check(client.call("POST", path + "/save-events", second).status == 200 &&
          client.call("POST", path + "/save-events", second).status == 200 &&
          client.call("GET", path).json.value("generation") == QStringLiteral("2") &&
          service.optionalHead(project.id, "main") == base,
          "hydraulic deletion or replay modified committed HEAD");
    const Response finalView = client.call("GET", path + "/networks/" + networkId);
    const std::optional<NetworkHydraulicDistribution> finalNetwork = native::ModelCodec<NetworkHydraulicDistribution>::decode(
        QByteArray::fromBase64(finalView.json.value("payloadBase64").toString().toLatin1()));
    check(finalView.status == 200 && finalView.json.value("generation") == "2" &&
          finalNetwork && finalNetwork->nodes_junctions.size() == 1 &&
          finalNetwork->nodes_junctions.first().uuid == newUuid,
          "deleted pinned-base member was resurrected during draft hydration");
    // HDB-03d: real HTTP Undo and Redo across two structural MODEL actions.
    // HEAD remains immutable; each request selects an exact journal position
    // and never accepts caller-supplied native overlay records.
    const QString cursorPath = path + "/history/cursor";
    const QString navigatePath = path + "/history/navigate";
    const Response atTip = client.call("GET", cursorPath);
    check(atTip.status == 200 && atTip.json.value("generation") == "2" &&
          atTip.json.value("journalCount") == "2" &&
          atTip.json.value("actionPosition") == "2" &&
          atTip.json.value("hasPreviousDraftAction").toBool() &&
          !atTip.json.value("hasNextDraftAction").toBool() &&
          atTip.json.value("previousAction").toObject().value("selectionKind") == "draft" &&
          atTip.json.value("previousAction").toObject().value("actionPosition") == "1" &&
          atTip.json.value("nextAction").isNull(),
          "HTTP history cursor did not report initial journal tip");
    const std::function<QJsonObject(const QString&, const QString&, const QString&)> navBody =
        [&](const QString& operation, const QString& generation, const QString& position) {
            return QJsonObject{{"operationId", operation}, {"sessionId", session},
                {"fencingToken", token}, {"expectedGeneration", generation},
                {"expectedHead", QString::fromStdString(base.toString())},
                {"actionPosition", position}};
        };
    const QString undoOneId = QString::fromStdString(native::UuidV7::generate().toString());
    const QJsonObject undoOneBody = navBody(undoOneId, "2", "1");
    const Response undoneOne = client.call("POST", navigatePath, undoOneBody);
    const Response atOne = client.call("GET", path + "/networks/" + networkId);
    const std::optional<NetworkHydraulicDistribution> oneNetwork = native::ModelCodec<NetworkHydraulicDistribution>::decode(
        QByteArray::fromBase64(atOne.json.value("payloadBase64").toString().toLatin1()));
    check(undoneOne.status == 200 && undoneOne.json.value("generation") == "3" &&
          undoneOne.json.value("draftActionPosition") == "1" &&
          atOne.status == 200 && oneNetwork && oneNetwork->nodes_junctions.size() == 2 &&
          oneNetwork->nodes_junctions.first().uuid == oldUuid &&
          oneNetwork->nodes_junctions.last().uuid == newUuid,
          "HDB-03d HTTP Undo did not restore the exact intermediate network");
    check(client.call("POST", navigatePath, undoOneBody).status == 200 &&
          client.call("GET", cursorPath).json.value("generation") == "3",
          "HDB-03d HTTP same-operation replay advanced generation twice");
    check(client.call("POST", navigatePath, navBody(undoOneId, "2", "0")).status == 409,
          "HDB-03d HTTP accepted operation UUID reuse with different position");
    check(client.call("POST", navigatePath, navBody(
              QString::fromStdString(native::UuidV7::generate().toString()), "2", "0")).status == 409,
          "HDB-03d HTTP accepted stale generation");
    check(client.call("POST", navigatePath, navBody("invalid-uuid", "3", "0")).status == 400,
          "HDB-03d HTTP accepted malformed navigation operation ID");
    check(client.call("POST", navigatePath, navBody(
              QString::fromStdString(native::UuidV7::generate().toString()), "3", "0")).status == 200,
          "HDB-03d HTTP Undo to base rejected");
    const Response atBase = client.call("GET", path + "/networks/" + networkId);
    const std::optional<NetworkHydraulicDistribution> baseAgain = native::ModelCodec<NetworkHydraulicDistribution>::decode(
        QByteArray::fromBase64(atBase.json.value("payloadBase64").toString().toLatin1()));
    const Response cursorAtBase = client.call("GET", cursorPath);
    check(atBase.status == 200 && baseAgain && baseAgain->nodes_junctions.size() == 1 &&
          baseAgain->nodes_junctions.first().uuid == oldUuid &&
          cursorAtBase.json.value("actionPosition") == "0" &&
          !cursorAtBase.json.value("hasPreviousDraftAction").toBool() &&
          cursorAtBase.json.value("hasNextDraftAction").toBool() &&
          cursorAtBase.json.value("previousAction").isNull() &&
          cursorAtBase.json.value("nextAction").toObject().value("selectionKind") == "draft" &&
          cursorAtBase.json.value("nextAction").toObject().value("actionPosition") == "1",
          "HDB-03d HTTP Undo to the immutable base failed");
    // The delta-enabled request must return ONLY the newly restored entity,
    // not the full NetworkHydraulicDistribution or the unchanged base member. The source
    // and destination hashes cover the complete canonical model snapshots.
    QJsonObject sparseRedoRequest = navBody(
        QString::fromStdString(native::UuidV7::generate().toString()), "4", "1");
    sparseRedoRequest.insert("networkId", networkId);
    const Response sparseRedo = client.call("POST", navigatePath, sparseRedoRequest);
    const QJsonObject sparseDelta = sparseRedo.json.value("historyDelta").toObject();
    const QJsonArray sparseEdits = sparseDelta.value("edits").toArray();
    const QJsonObject sparseInsert = sparseEdits.isEmpty() ? QJsonObject() :
        sparseEdits.first().toObject();
    const QString baseDigest = baseAgain ? QString::fromLatin1(QCryptographicHash::hash(
        native::ModelCodec<NetworkHydraulicDistribution>::encode(*baseAgain),
        QCryptographicHash::Sha256).toHex()) : QString();
    const QString oneDigest = oneNetwork ? QString::fromLatin1(QCryptographicHash::hash(
        native::ModelCodec<NetworkHydraulicDistribution>::encode(*oneNetwork),
        QCryptographicHash::Sha256).toHex()) : QString();
    // A tiny read-only receipt is enough to identify the exact authoritative
    // state after a lost navigation reply. A matching digest alone is NOT
    // proof that the requested History action was accepted: the operation,
    // generation and action coordinate are equally important.
    const QString reconcilePath = path + "/history/reconcile?networkId=" + networkId;
    const Response reconciled = client.call("GET", reconcilePath);
    const QJsonObject receipt = reconciled.json;
    check(reconciled.status == 200 && receipt.value("draftId") == created.json.value("id") &&
          receipt.value("networkId") == networkId &&
          receipt.value("generation") == "5" &&
          receipt.value("networkSha256") == oneDigest &&
          receipt.value("lastNavigationOperationId") ==
              sparseRedoRequest.value("operationId") &&
          receipt.value("lastNavigationSourceGeneration") == "4" &&
          receipt.value("selectionKind") == "draft" &&
          receipt.value("actionPosition") == "1" &&
          receipt.value("journalCount") == "2" &&
          !receipt.contains("payloadBase64") && !receipt.contains("changes") &&
          !receipt.contains("historyDelta"),
          "HDB-03d SHA receipt did not pin operation, cursor and generation");
    check(client.call("GET", path + "/history/reconcile?networkId=invalid").status == 400 &&
          client.call("GET", path + "/history/reconcile?unexpected=1").status == 400,
          "HDB-03d accepted malformed History SHA reconciliation parameters");
    check(sparseRedo.status == 200 && !sparseRedo.json.contains("payloadBase64") &&
          !sparseRedo.json.contains("changes") &&
          sparseDelta.value("version") == QJsonValue(1) &&
          sparseDelta.value("networkId") == networkId &&
          sparseDelta.value("fromSha256") == baseDigest &&
          sparseDelta.value("toSha256") == oneDigest &&
          sparseEdits.size() == 1 && sparseInsert.value("id") == newId &&
          sparseInsert.value("type") == "HydraulicNodeJunction" &&
          sparseInsert.value("beforeBase64").isNull() &&
          QByteArray::fromBase64(sparseInsert.value("afterBase64").toString().toLatin1()) == encoded &&
          sparseDelta.value("moves").toArray().isEmpty(),
          "HDB-03d incremental Redo sent unrelated network content or invalid membership delta");
    const Response sparseReplay = client.call("POST", navigatePath, sparseRedoRequest);
    check(sparseReplay.status == 200 && sparseReplay.json.value("generation") == "5" &&
          !sparseReplay.json.contains("deltaRecoveryRequired") &&
          sparseReplay.json.value("historyDelta") == QJsonValue(sparseDelta) &&
          !sparseReplay.json.contains("payloadBase64") &&
          !sparseReplay.json.contains("changes") &&
          client.call("GET", cursorPath).json.value("generation") == "5",
          "HDB-03d lost-acknowledgement retry did not regenerate the same sparse delta");
    // A remote GUI can stay disconnected past the old lease expiry. Re-leasing
    // the SAME session rotates its fencing token without changing generation
    // or invalidating the durable source candidate for the last navigation.
    const Response reLeased = client.call("POST", path + "/lease",
        QJsonObject{{"sessionId", session}, {"seconds", 90}});
    const QString rotatedToken = reLeased.json.value("token").toString();
    QJsonObject rotatedReplayRequest = sparseRedoRequest;
    rotatedReplayRequest.insert("fencingToken", rotatedToken);
    const Response rotatedReplay = client.call("POST", navigatePath, rotatedReplayRequest);
    check(reLeased.status == 200 && rotatedToken != token && !rotatedToken.isEmpty() &&
          reLeased.json.value("generation") == "5" &&
          rotatedReplay.status == 200 && rotatedReplay.json.value("generation") == "5" &&
          rotatedReplay.json.value("historyDelta") == QJsonValue(sparseDelta) &&
          !rotatedReplay.json.contains("payloadBase64") &&
          client.call("GET", cursorPath).json.value("generation") == "5",
          "HDB-03d re-lease changed the idempotent sparse History result");
    token = rotatedToken;
    check(client.call("POST", navigatePath, navBody(
              QString::fromStdString(native::UuidV7::generate().toString()), "5", "2")).status == 200,
          "HDB-03d HTTP Redo rejected a valid original action boundary");
    const Response restoredView = client.call("GET", path + "/networks/" + networkId);
    const std::optional<NetworkHydraulicDistribution> restoredNetwork = native::ModelCodec<NetworkHydraulicDistribution>::decode(
        QByteArray::fromBase64(restoredView.json.value("payloadBase64").toString().toLatin1()));
    check(restoredView.status == 200 && restoredNetwork &&
          restoredNetwork->nodes_junctions.size() == 1 &&
          restoredNetwork->nodes_junctions.first().uuid == newUuid &&
          client.call("GET", cursorPath).json.value("actionPosition") == "2" &&
          service.optionalHead(project.id, "main") == base,
          "HDB-03d HTTP Redo did not recover terminal network without moving HEAD");
    check(client.call("POST", navigatePath, sparseRedoRequest).status == 409,
          "HDB-03d stale retry recovered after a newer navigation replaced its receipt");
    check(client.call("POST", navigatePath, navBody(
              QString::fromStdString(native::UuidV7::generate().toString()), "6", "3")).status == 409,
          "HDB-03d HTTP accepted navigation past journal tip");
    // Make a new edit after Undo. The HTTP event Save must replace only the
    // ineligible redo tail; it must not archive the now-discarded deletion.
    check(client.call("POST", navigatePath, navBody(
              QString::fromStdString(native::UuidV7::generate().toString()), "6", "1")).status == 200,
          "HDB-03d cannot select prefix before branching draft history");
    HydraulicNodeJunction revised = added;
    revised.elevation_m = 66;
    const QByteArray revisedBytes = native::ModelCodec<HydraulicNodeJunction>::encode(revised);
    const QString newEvent = eventFrame(newUuid, QStringLiteral("hydraulic.junction.update"),
                                        encoded, revisedBytes);
    const QJsonObject modified{{"uuid", newId}, {"kind", 201},
                               {"references", QJsonArray{}},
                               {"payloadBase64", QString::fromLatin1(revisedBytes.toBase64())}};
    const Response branched = client.call("POST", path + "/save-events",
        QJsonObject{{"sessionId", session}, {"fencingToken", token},
                    {"expectedGeneration", "7"}, {"changes", QJsonArray{modified}},
                    {"events", QJsonArray{newEvent}}});
    const Response journalNow = client.call("GET", path);
    const QJsonArray acceptedEvents = journalNow.json.value("events").toArray();
    check(branched.status == 200 && branched.json.value("generation") == "8" &&
          branched.json.value("eventCount") == "2" &&
          journalNow.json.value("draftActionPosition") == "2" &&
          acceptedEvents.size() == 2 &&
          acceptedEvents.at(0).toObject().value("payloadBase64") == firstEvent &&
          acceptedEvents.at(1).toObject().value("payloadBase64") == newEvent &&
          !client.call("GET", cursorPath).json.value("hasNextDraftAction").toBool(),
          "HDB-03d event Save did not invalidate the redo tail through HTTP");
    const Response branchedView = client.call("GET", path + "/networks/" + networkId);
    const std::optional<NetworkHydraulicDistribution> branchedNetwork = native::ModelCodec<NetworkHydraulicDistribution>::decode(
        QByteArray::fromBase64(branchedView.json.value("payloadBase64").toString().toLatin1()));
    check(branchedView.status == 200 && branchedNetwork &&
          branchedNetwork->nodes_junctions.size() == 2 &&
          branchedNetwork->nodes_junctions.first().uuid == oldUuid &&
          branchedNetwork->nodes_junctions.last().uuid == newUuid &&
          branchedNetwork->nodes_junctions.last().elevation_m == 66,
          "HDB-03d HTTP redo invalidation lost the new edited network state");
    server.stop();
    const native::WorkingCopyInfo persisted = service.workingCopy(
        created.json.value("id").toString().toStdString());
    // A valid native overlay need not contain three records: insertion into
    // an existing membership chunk changes that chunk and the new junction,
    // while the root manifest can remain byte-identical to committed HEAD.
    // Check the actual persisted MODEL frames and reconstruct the candidate
    // from the stored overlay instead of relying on an arbitrary record count.
    const bool persistedJournalMatches = persisted.journal.size() == 2 &&
        native::hydraulic::qByteArrayFromBytes(persisted.journal[0].encoded) ==
            QByteArray::fromBase64(firstEvent.toLatin1()) &&
        native::hydraulic::qByteArrayFromBytes(persisted.journal[1].encoded) ==
            QByteArray::fromBase64(newEvent.toLatin1());
    const native::Key revisedKey = native::hydraulic::keyFromUuid(newUuid);
    bool persistedJunctionMatches = false;
    std::map<native::Key, std::optional<native::ModelRecord>> persistedOverlay;
    for (const native::ModelChange& change : persisted.candidateChanges) {
        check(persistedOverlay.emplace(change.key, change.record).second,
              "branched HTTP history persisted duplicate native keys");
        if (change.key == revisedKey)
            persistedJunctionMatches = change.record &&
                change.record->kind == native::hydraulic::junctionKind &&
                change.record->payload == native::hydraulic::bytesFromQByteArray(revisedBytes);
    }
    const std::function<std::optional<native::ModelRecord>(const native::Key&)> readPersisted =
        [&persistedOverlay, &service, &project, &base](const native::Key& key)
            -> std::optional<native::ModelRecord> {
            const std::map<native::Key, std::optional<native::ModelRecord>>::const_iterator found =
                persistedOverlay.find(key);
            if (found != persistedOverlay.end()) return found->second;
            return service.getModelAtRevision(project.id, base, key);
        };
    const std::optional<NetworkHydraulicDistribution> persistedNetwork =
        native::hydraulic::loadDraftHydraulicDistributionNetwork(readPersisted, networkUuid);
    check(persisted.generation == 8 && persisted.journalCount == 2 &&
          !persisted.draftActionPosition,
          "HDB-03d durable HTTP draft generation, journal count or cursor mismatch");
    check(persistedJournalMatches,
          "HDB-03d durable HTTP journal lost or reordered its original MODEL frames");
    check(persistedJunctionMatches,
          "HDB-03d durable HTTP overlay lost the revised junction record");
    check(persistedNetwork && persistedNetwork->nodes_junctions.size() == 2 &&
          persistedNetwork->nodes_junctions.first().uuid == oldUuid &&
          persistedNetwork->nodes_junctions.last().uuid == newUuid &&
          persistedNetwork->nodes_junctions.last().elevation_m == 66,
          "HDB-03d durable HTTP native overlay does not reconstruct branched hydraulic distribution network");
    check(service.optionalHead(project.id, "main") == base,
          "HDB-03d durable HTTP draft history advanced committed HEAD");

    // HDB-03e: after a real local Commit moves HEAD and clears the active
    // journal, the SAME HTTP History command can address the archived MODEL
    // action boundaries explicitly. The immutable HEAD must never move when
    // selecting a historical state in the private working copy.
    const std::string draftId = created.json.value("id").toString().toStdString();
    const std::string sessionId = session.toStdString();
    const native::WorkingCopyLease renewed =
        service.acquireWorkingCopyLease(draftId, sessionId, 120);
    const native::LocalCommitInfo committed = service.commitWorkingCopy(
        native::UuidV7::generate().toString(), draftId, sessionId, renewed.token,
        8, base, commitMetadata(),
        [](const native::MergeCandidate& candidate) {
            return native::annotations::validateAnnotationSnapshot(candidate);
        });
    const std::optional<native::RevisionId> archivedHead =
        native::RevisionId::parse(committed.reservedRevisionId);
    const native::WorkingCopyInfo rebased = service.workingCopy(draftId);
    check(committed.state == native::LocalCommitState::Succeeded &&
          archivedHead && rebased.generation == 9 &&
          rebased.baseRevision == archivedHead && rebased.journalCount == 0 &&
          !rebased.dirty && service.optionalHead(project.id, "main") == archivedHead,
          "HDB-03e failed to establish committed action archive and clean draft");

    TestServer archiveServer(service, native::BundleLimits{4ULL * 1024ULL * 1024ULL,
                                                          10000ULL, 1024ULL * 1024ULL});
    Client archiveClient(archiveServer.port());
    const QString archivedRevision = QString::fromStdString(committed.reservedRevisionId);
    const QString archivedPath = path + "/history/navigate";
    const QString archivedCursorPath = path + "/history/cursor";
    const std::function<QJsonObject(const QString&, const QString&, const QString&)> archivedBody =
        [&](const QString& operation, const QString& generation, const QString& position) {
            return QJsonObject{{"operationId", operation}, {"sessionId", session},
                {"fencingToken", QString::fromStdString(renewed.token)},
                {"expectedGeneration", generation}, {"expectedHead", archivedRevision},
                {"revisionId", archivedRevision}, {"actionPosition", position}};
        };
    const Response beforeArchived = archiveClient.call("GET", archivedCursorPath);
    check(beforeArchived.status == 200 &&
          beforeArchived.json.value("generation") == "9" &&
          beforeArchived.json.value("journalCount") == "0" &&
          beforeArchived.json.value("selectionKind") == "draft" &&
          beforeArchived.json.value("archivedRevisionId").isNull() &&
          beforeArchived.json.value("previousAction").toObject().value("selectionKind") == "archived" &&
          beforeArchived.json.value("previousAction").toObject().value("revisionId") == archivedRevision &&
          beforeArchived.json.value("previousAction").toObject().value("actionPosition") == "1" &&
          beforeArchived.json.value("nextAction").isNull(),
          "HDB-03e freshly committed draft reported a phantom archived selection");
    QJsonObject malformedArchived = archivedBody(
        QString::fromStdString(native::UuidV7::generate().toString()), "9", "0");
    malformedArchived.insert("revisionId", QStringLiteral("invalid-revision"));
    check(archiveClient.call("POST", archivedPath, malformedArchived).status == 400,
          "HDB-03e accepted malformed archived revision UUID");
    const QString zeroOperation =
        QString::fromStdString(native::UuidV7::generate().toString());
    const QJsonObject zeroBody = archivedBody(zeroOperation, "9", "0");
    const Response atArchivedParent = archiveClient.call("POST", archivedPath, zeroBody);
    const Response parentView = archiveClient.call("GET", path + "/networks/" + networkId);
    const std::optional<NetworkHydraulicDistribution> parentNetwork =
        native::ModelCodec<NetworkHydraulicDistribution>::decode(QByteArray::fromBase64(
            parentView.json.value("payloadBase64").toString().toLatin1()));
    const Response parentCursor = archiveClient.call("GET", archivedCursorPath);
    check(atArchivedParent.status == 200 &&
          atArchivedParent.json.value("generation") == "10" &&
          atArchivedParent.json.value("archivedRevisionId") == archivedRevision &&
          atArchivedParent.json.value("archivedActionPosition") == "0" &&
          parentView.status == 200 && parentNetwork &&
          parentNetwork->nodes_junctions.size() == 1 &&
          parentNetwork->nodes_junctions.first().uuid == oldUuid &&
          parentCursor.json.value("selectionKind") == "archived" &&
          parentCursor.json.value("archivedRevisionId") == archivedRevision &&
          parentCursor.json.value("archivedActionPosition") == "0" &&
          parentCursor.json.value("previousAction").isNull() &&
          parentCursor.json.value("nextAction").toObject().value("revisionId") == archivedRevision &&
          parentCursor.json.value("nextAction").toObject().value("actionPosition") == "1" &&
          !parentCursor.json.value("hasPreviousDraftAction").toBool() &&
          service.optionalHead(project.id, "main") == archivedHead,
          "HDB-03e archived parent selection did not preserve the immutable HEAD");
    check(archiveClient.call("POST", archivedPath, zeroBody).status == 200 &&
          archiveClient.call("GET", archivedCursorPath).json.value("generation") == "10",
          "HDB-03e archived action retry advanced generation twice");
    // A plain Save must not erase the verified archive coordinate or
    // circumvent the reversible MODEL event requirement, even without a
    // suspended journal (immediately after a Commit).
    check(archiveClient.call("POST", path + "/save",
        QJsonObject{{"sessionId", session}, {"fencingToken", QString::fromStdString(renewed.token)},
                    {"expectedGeneration", "10"}, {"changes", QJsonArray{addition}}}).status == 409 &&
          archiveClient.call("GET", archivedCursorPath).json.value("generation") == "10",
          "HDB-03e plain Save discarded a verified archived cursor");
    check(archiveClient.call("POST", archivedPath,
              archivedBody(zeroOperation, "9", "1")).status == 409,
          "HDB-03e accepted conflicting reuse of navigation UUID");
    check(archiveClient.call("POST", archivedPath,
              archivedBody(QString::fromStdString(native::UuidV7::generate().toString()),
                           "9", "1")).status == 409,
          "HDB-03e accepted stale archived action generation");
    const Response atArchivedFirst = archiveClient.call("POST", archivedPath,
        archivedBody(QString::fromStdString(native::UuidV7::generate().toString()), "10", "1"));
    const Response firstView = archiveClient.call("GET", path + "/networks/" + networkId);
    const std::optional<NetworkHydraulicDistribution> firstNetwork =
        native::ModelCodec<NetworkHydraulicDistribution>::decode(QByteArray::fromBase64(
            firstView.json.value("payloadBase64").toString().toLatin1()));
    check(atArchivedFirst.status == 200 &&
          atArchivedFirst.json.value("generation") == "11" &&
          firstView.status == 200 && firstNetwork &&
          firstNetwork->nodes_junctions.size() == 2 &&
          firstNetwork->nodes_junctions.first().uuid == oldUuid &&
          firstNetwork->nodes_junctions.last().uuid == newUuid &&
          firstNetwork->nodes_junctions.last().elevation_m == 55,
          "HDB-03e archived intermediate MODEL action was not reconstructed");
    const Response atArchivedTip = archiveClient.call("POST", archivedPath,
        archivedBody(QString::fromStdString(native::UuidV7::generate().toString()), "11", "2"));
    const Response tipView = archiveClient.call("GET", path + "/networks/" + networkId);
    const std::optional<NetworkHydraulicDistribution> tipNetwork =
        native::ModelCodec<NetworkHydraulicDistribution>::decode(QByteArray::fromBase64(
            tipView.json.value("payloadBase64").toString().toLatin1()));
    const Response tipCursor = archiveClient.call("GET", archivedCursorPath);
    check(atArchivedTip.status == 200 &&
          atArchivedTip.json.value("generation") == "12" &&
          !atArchivedTip.json.value("dirty").toBool() &&
          tipView.status == 200 && tipNetwork &&
          tipNetwork->nodes_junctions.size() == 2 &&
          tipNetwork->nodes_junctions.last().elevation_m == 66 &&
          tipCursor.json.value("archivedActionPosition") == "2" &&
          tipCursor.json.value("previousAction").toObject().value("actionPosition") == "1" &&
          tipCursor.json.value("nextAction").isNull() &&
          service.optionalHead(project.id, "main") == archivedHead,
          "HDB-03e Redo to committed archive tip did not yield clean HEAD");
    check(archiveClient.call("POST", archivedPath,
              archivedBody(QString::fromStdString(native::UuidV7::generate().toString()),
                           "12", "3")).status == 409,
          "HDB-03e accepted action position beyond immutable archive");
    // HDB-03f: a second real MODEL Commit provides an adjacent immutable
    // action sequence. The cursor supplies exact previous/next coordinates
    // across the Commit marker, without asking the GUI to infer ancestry.
    HydraulicNodeJunction finalJunction = revised;
    finalJunction.elevation_m = 77;
    const QByteArray finalBytes = native::ModelCodec<HydraulicNodeJunction>::encode(finalJunction);
    const QString secondCommitEvent = eventFrame(newUuid,
        QStringLiteral("hydraulic.junction.update"), revisedBytes, finalBytes);
    const QJsonObject finalMember{{"uuid", newId}, {"kind", 201},
                                  {"references", QJsonArray{}},
                                  {"payloadBase64", QString::fromLatin1(finalBytes.toBase64())}};
    const Response savedAgain = archiveClient.call("POST", path + "/save-events",
        QJsonObject{{"sessionId", session}, {"fencingToken", QString::fromStdString(renewed.token)},
                    {"expectedGeneration", "12"}, {"changes", QJsonArray{finalMember}},
                    {"events", QJsonArray{secondCommitEvent}}});
    check(savedAgain.status == 200 && savedAgain.json.value("generation") == "13",
          "HDB-03f cannot establish second journalled Commit fixture");
    const native::LocalCommitInfo secondCommit = service.commitWorkingCopy(
        native::UuidV7::generate().toString(), draftId, sessionId, renewed.token,
        13, archivedHead, commitMetadata(),
        [](const native::MergeCandidate& candidate) {
            return native::annotations::validateAnnotationSnapshot(candidate);
        });
    const QString secondRevision = QString::fromStdString(secondCommit.reservedRevisionId);
    check(secondCommit.state == native::LocalCommitState::Succeeded &&
          service.workingCopy(draftId).generation == 14,
          "HDB-03f second archived Commit was not durable");
    const Response committedCursor = archiveClient.call("GET", archivedCursorPath);
    check(committedCursor.status == 200 &&
          committedCursor.json.value("previousAction").toObject().value("revisionId") == secondRevision &&
          committedCursor.json.value("previousAction").toObject().value("actionPosition") == "0" &&
          committedCursor.json.value("nextAction").isNull(),
          "HDB-03f clean post-Commit cursor did not locate latest archived action");
    const std::function<QJsonObject(const QString&, const QString&, const QString&, const QString&)>
        nextArchivedBody = [&](const QString& operation, const QString& generation,
                                const QString& revision, const QString& position) {
            return QJsonObject{{"operationId", operation}, {"sessionId", session},
                {"fencingToken", QString::fromStdString(renewed.token)},
                {"expectedGeneration", generation}, {"expectedHead", secondRevision},
                {"revisionId", revision}, {"actionPosition", position}};
        };
    const Response selectedSecondParent = archiveClient.call("POST", archivedPath,
        nextArchivedBody(QString::fromStdString(native::UuidV7::generate().toString()),
                         "14", secondRevision, "0"));
    const Response secondParentCursor = archiveClient.call("GET", archivedCursorPath);
    check(selectedSecondParent.status == 200 &&
          secondParentCursor.json.value("previousAction").toObject().value("revisionId") == archivedRevision &&
          secondParentCursor.json.value("previousAction").toObject().value("actionPosition") == "1" &&
          secondParentCursor.json.value("nextAction").toObject().value("revisionId") == secondRevision &&
          secondParentCursor.json.value("nextAction").toObject().value("actionPosition") == "1",
          "HDB-03f did not skip duplicate Commit boundary snapshot");
    const Response crossedBack = archiveClient.call("POST", archivedPath,
        nextArchivedBody(QString::fromStdString(native::UuidV7::generate().toString()),
                         "15", archivedRevision, "1"));
    const Response oldActionView = archiveClient.call("GET", path + "/networks/" + networkId);
    const std::optional<NetworkHydraulicDistribution> oldActionNetwork =
        native::ModelCodec<NetworkHydraulicDistribution>::decode(QByteArray::fromBase64(
            oldActionView.json.value("payloadBase64").toString().toLatin1()));
    check(crossedBack.status == 200 && oldActionView.status == 200 &&
          oldActionNetwork && oldActionNetwork->nodes_junctions.size() == 2 &&
          oldActionNetwork->nodes_junctions.last().elevation_m == 55,
          "HDB-03f backward movement across two real Commits changed the wrong state");
    const Response oldTip = archiveClient.call("POST", archivedPath,
        nextArchivedBody(QString::fromStdString(native::UuidV7::generate().toString()),
                         "16", archivedRevision, "2"));
    const Response oldTipCursor = archiveClient.call("GET", archivedCursorPath);
    check(oldTip.status == 200 &&
          oldTipCursor.json.value("nextAction").toObject().value("revisionId") == secondRevision &&
          oldTipCursor.json.value("nextAction").toObject().value("actionPosition") == "1",
          "HDB-03f forward movement did not cross the subsequent Commit marker");
    QJsonObject archivedDeltaRequest = nextArchivedBody(
        QString::fromStdString(native::UuidV7::generate().toString()),
        "17", secondRevision, "1");
    archivedDeltaRequest.insert("networkId", networkId);
    const Response crossedForward = archiveClient.call("POST", archivedPath, archivedDeltaRequest);
    const QJsonObject archivedDelta = crossedForward.json.value("historyDelta").toObject();
    const QJsonArray archivedEdits = archivedDelta.value("edits").toArray();
    const QJsonObject archivedUpdate = archivedEdits.isEmpty() ? QJsonObject() :
        archivedEdits.first().toObject();
    check(crossedForward.status == 200 &&
          !crossedForward.json.contains("payloadBase64") &&
          archivedDelta.value("networkId") == networkId &&
          archivedEdits.size() == 1 &&
          archivedUpdate.value("type") == "HydraulicNodeJunction" &&
          archivedUpdate.value("id") == newId &&
          QByteArray::fromBase64(archivedUpdate.value("beforeBase64").toString().toLatin1()) ==
              revisedBytes &&
          QByteArray::fromBase64(archivedUpdate.value("afterBase64").toString().toLatin1()) ==
              finalBytes &&
          archivedDelta.value("moves").toArray().isEmpty(),
          "HDB-03f cross-Commit History navigation did not return a sparse MODEL delta");
    const Response archivedReplay = archiveClient.call("POST", archivedPath, archivedDeltaRequest);
    check(archivedReplay.status == 200 &&
          archivedReplay.json.value("historyDelta") == QJsonValue(archivedDelta) &&
          archivedReplay.json.value("generation") == crossedForward.json.value("generation") &&
          !archivedReplay.json.contains("payloadBase64"),
          "HDB-03f archived lost-acknowledgement retry changed the MODEL delta");
    const Response finalNetworkView = archiveClient.call("GET", path + "/networks/" + networkId);
    const std::optional<NetworkHydraulicDistribution> finalNetworkState =
        native::ModelCodec<NetworkHydraulicDistribution>::decode(QByteArray::fromBase64(
            finalNetworkView.json.value("payloadBase64").toString().toLatin1()));
    check(crossedForward.status == 200 &&
          !crossedForward.json.value("dirty").toBool() &&
          finalNetworkView.status == 200 && finalNetworkState &&
          finalNetworkState->nodes_junctions.last().elevation_m == 77 &&
          service.optionalHead(project.id, "main") == native::RevisionId::parse(
              secondCommit.reservedRevisionId),
          "HDB-03f forward movement across Commits changed immutable HEAD or model state");

    // HDB-03g: active journal Undo at position zero must cross the latest
    // Commit marker without dropping the original live MODEL Redo action.
    HydraulicNodeJunction liveJunction = finalJunction;
    liveJunction.elevation_m = 88;
    const QByteArray liveBytes = native::ModelCodec<HydraulicNodeJunction>::encode(liveJunction);
    const QString liveEvent = eventFrame(newUuid,
        QStringLiteral("hydraulic.junction.update"), finalBytes, liveBytes);
    const QJsonObject liveMember{{"uuid", newId}, {"kind", 201},
                                 {"references", QJsonArray{}},
                                 {"payloadBase64", QString::fromLatin1(liveBytes.toBase64())}};
    const Response liveSaved = archiveClient.call("POST", path + "/save-events",
        QJsonObject{{"sessionId", session}, {"fencingToken", QString::fromStdString(renewed.token)},
                    {"expectedGeneration", "18"}, {"changes", QJsonArray{liveMember}},
                    {"events", QJsonArray{liveEvent}}});
    check(liveSaved.status == 200 && liveSaved.json.value("generation") == "19",
          "HDB-03g could not record unfinished live action after Commit");
    const std::function<QJsonObject(const QString&, const QString&, const QString&)>
        liveNavigation = [&](const QString& operation, const QString& generation,
                             const QString& position) {
            return QJsonObject{{"operationId", operation}, {"sessionId", session},
                {"fencingToken", QString::fromStdString(renewed.token)},
                {"expectedGeneration", generation}, {"expectedHead", secondRevision},
                {"actionPosition", position}};
        };
    const Response liveUndone = archiveClient.call("POST", archivedPath,
        liveNavigation(QString::fromStdString(native::UuidV7::generate().toString()), "19", "0"));
    const Response atLiveBase = archiveClient.call("GET", archivedCursorPath);
    check(liveUndone.status == 200 && !liveUndone.json.value("dirty").toBool() &&
          atLiveBase.json.value("previousAction").toObject().value("selectionKind") == "archived" &&
          atLiveBase.json.value("previousAction").toObject().value("revisionId") == secondRevision &&
          atLiveBase.json.value("previousAction").toObject().value("actionPosition") == "0" &&
          atLiveBase.json.value("nextAction").toObject().value("selectionKind") == "draft" &&
          atLiveBase.json.value("nextAction").toObject().value("actionPosition") == "1",
          "HDB-03g active position zero did not resolve Commit and draft neighbors");
    const QString suspendedOperation =
        QString::fromStdString(native::UuidV7::generate().toString());
    const QJsonObject suspendedBody = nextArchivedBody(
        suspendedOperation, "20", secondRevision, "0");
    const Response suspended = archiveClient.call("POST", archivedPath, suspendedBody);
    const Response suspendedCursor = archiveClient.call("GET", archivedCursorPath);
    const Response earlierView = archiveClient.call("GET", path + "/networks/" + networkId);
    const std::optional<NetworkHydraulicDistribution> earlierNetwork =
        native::ModelCodec<NetworkHydraulicDistribution>::decode(QByteArray::fromBase64(
            earlierView.json.value("payloadBase64").toString().toLatin1()));
    check(suspended.status == 200 && suspended.json.value("generation") == "21" &&
          suspended.json.value("eventCount") == "1" &&
          suspendedCursor.json.value("journalCount") == "1" &&
          suspended.json.value("draftActionPosition") == "0" &&
          suspendedCursor.json.value("selectionKind") == "archived" &&
          suspendedCursor.json.value("nextAction").toObject().value("selectionKind") == "draft" &&
          suspendedCursor.json.value("nextAction").toObject().value("actionPosition") == "0" &&
          earlierView.status == 200 && earlierNetwork &&
          earlierNetwork->nodes_junctions.last().elevation_m == 66 &&
          service.optionalHead(project.id, "main") ==
              native::RevisionId::parse(secondCommit.reservedRevisionId),
          "HDB-03g committed navigation did not retain draft Redo or exact MODEL state");
    check(archiveClient.call("POST", archivedPath, suspendedBody).status == 200 &&
          archiveClient.call("GET", archivedCursorPath).json.value("generation") == "21",
          "HDB-03g suspended cross-mode selection was not idempotent");
    // Browsing alone must leave Redo intact. Test the untouched route first;
    // a NEW edit from the archived position is tested below as a real fork.
    const Response returned = archiveClient.call("POST", archivedPath,
        liveNavigation(QString::fromStdString(native::UuidV7::generate().toString()), "21", "0"));
    const Response returnedCursor = archiveClient.call("GET", archivedCursorPath);
    check(returned.status == 200 && returned.json.value("generation") == "22" &&
          returned.json.value("eventCount") == "1" &&
          returnedCursor.json.value("journalCount") == "1" &&
          returnedCursor.json.value("selectionKind") == "draft" &&
          returnedCursor.json.value("nextAction").toObject().value("actionPosition") == "1",
          "HDB-03g returning to draft base lost the Redo action");
    const Response redone = archiveClient.call("POST", archivedPath,
        liveNavigation(QString::fromStdString(native::UuidV7::generate().toString()), "22", "1"));
    const Response redoneView = archiveClient.call("GET", path + "/networks/" + networkId);
    const std::optional<NetworkHydraulicDistribution> redoneNetwork =
        native::ModelCodec<NetworkHydraulicDistribution>::decode(QByteArray::fromBase64(
            redoneView.json.value("payloadBase64").toString().toLatin1()));
    check(redone.status == 200 && redone.json.value("generation") == "23" &&
          redoneView.status == 200 && redoneNetwork &&
          redoneNetwork->nodes_junctions.last().elevation_m == 88 &&
          service.workingCopy(draftId, true).journal.size() == 1 &&
          service.optionalHead(project.id, "main") ==
              native::RevisionId::parse(secondCommit.reservedRevisionId),
          "HDB-03g Redo across Commit boundary did not restore the live MODEL action");

    // HDB-03h: after validating ordinary Undo/Redo through the Commit marker,
    // return to the same archived action and START a new edit. That must fork
    // the logical action path, not fail because the old draft has a Redo tail.
    const Response rewindFork = archiveClient.call("POST", archivedPath,
        liveNavigation(QString::fromStdString(native::UuidV7::generate().toString()), "23", "0"));
    const Response selectForkOrigin = archiveClient.call("POST", archivedPath,
        nextArchivedBody(QString::fromStdString(native::UuidV7::generate().toString()),
                         "24", secondRevision, "0"));
    const Response forkOriginCursor = archiveClient.call("GET", archivedCursorPath);
    check(rewindFork.status == 200 && rewindFork.json.value("generation") == "24" &&
          selectForkOrigin.status == 200 && selectForkOrigin.json.value("generation") == "25" &&
          forkOriginCursor.json.value("selectionKind") == "archived" &&
          forkOriginCursor.json.value("archivedRevisionId") == secondRevision &&
          forkOriginCursor.json.value("archivedActionPosition") == "0" &&
          forkOriginCursor.json.value("canBranchFromHistory").toBool() &&
          forkOriginCursor.json.value("journalCount") == "1",
          "HDB-03h could not restore the verified fork origin with suspended Redo");
    HydraulicNodeJunction forkJunction = revised;
    forkJunction.elevation_m = 91;
    const QByteArray forkBytes = native::ModelCodec<HydraulicNodeJunction>::encode(forkJunction);
    const QJsonObject forkMember{{"uuid", newId}, {"kind", 201},
                                 {"references", QJsonArray{}},
                                 {"payloadBase64", QString::fromLatin1(forkBytes.toBase64())}};
    // This deliberately stale before-image belongs to the abandoned 77-metre
    // Commit, not the actual selected 66-metre state. A rejected Save must not
    // fork, drop Redo, or advance the generation.
    const QString staleForkEvent = eventFrame(newUuid,
        QStringLiteral("hydraulic.junction.update"), finalBytes, forkBytes);
    const QJsonObject staleForkBody{{"sessionId", session},
        {"fencingToken", QString::fromStdString(renewed.token)},
        {"expectedGeneration", "25"}, {"changes", QJsonArray{forkMember}},
        {"events", QJsonArray{staleForkEvent}}};
    check(archiveClient.call("POST", path + "/save-events", staleForkBody).status == 400 &&
          archiveClient.call("GET", archivedCursorPath).json.value("generation") == "25" &&
          service.workingCopy(draftId, true).journal.size() == 1,
          "HDB-03h stale MODEL before-image was accepted or damaged suspended Redo");
    const QString forkEvent = eventFrame(newUuid,
        QStringLiteral("hydraulic.junction.update"), revisedBytes, forkBytes);
    const QJsonObject forkBody{{"sessionId", session},
        {"fencingToken", QString::fromStdString(renewed.token)},
        {"expectedGeneration", "25"}, {"changes", QJsonArray{forkMember}},
        {"events", QJsonArray{forkEvent}}};
    const Response forkSaved = archiveClient.call("POST", path + "/save-events", forkBody);
    const Response forkCursor = archiveClient.call("GET", archivedCursorPath);
    const Response forkView = archiveClient.call("GET", path + "/networks/" + networkId);
    const std::optional<NetworkHydraulicDistribution> forkNetwork =
        native::ModelCodec<NetworkHydraulicDistribution>::decode(QByteArray::fromBase64(
            forkView.json.value("payloadBase64").toString().toLatin1()));
    const native::WorkingCopyInfo forkDraft = service.workingCopy(draftId, true);
    check(forkSaved.status == 200 && forkSaved.json.value("generation") == "26" &&
          forkSaved.json.value("eventCount") == "1" &&
          forkCursor.status == 200 && forkCursor.json.value("generation") == "26" &&
          forkCursor.json.value("selectionKind") == "draft" &&
          forkCursor.json.value("journalCount") == "1" &&
          forkCursor.json.value("forkRevisionId") == secondRevision &&
          forkCursor.json.value("forkActionPosition") == "0" &&
          forkCursor.json.value("nextAction").isNull() &&
          forkView.status == 200 && forkNetwork &&
          forkNetwork->nodes_junctions.last().elevation_m == 91 &&
          forkDraft.journal.size() == 1 &&
          forkDraft.journal.front().encoded == native::hydraulic::bytesFromQByteArray(
              QByteArray::fromBase64(forkEvent.toLatin1())) &&
          forkDraft.historyForkRevisionId == secondRevision.toStdString() &&
          forkDraft.historyForkActionPosition == 0 &&
          forkDraft.historyForkArchiveCommitOperationId == secondCommit.id &&
          service.optionalHead(project.id, "main") ==
              native::RevisionId::parse(secondCommit.reservedRevisionId),
          "HDB-03h valid archived MODEL Save did not atomically fork History");
    check(archiveClient.call("POST", path + "/save-events", forkBody).status == 200 &&
          archiveClient.call("GET", archivedCursorPath).json.value("generation") == "26",
          "HDB-03h lost-acknowledgement replay duplicated the fork");
    QJsonObject staleGeneration = forkBody;
    staleGeneration.insert("expectedGeneration", QStringLiteral("24"));
    check(archiveClient.call("POST", path + "/save-events", staleGeneration).status == 409,
          "HDB-03h stale generation accepted a second fork");
    const QString timelinePath = QStringLiteral("/dvmdb/v1/projects/") +
        QString::fromStdString(project.id) + QStringLiteral("/branches/main/action-history");
    const QString draftTimelinePath = timelinePath + "?draftId=" +
        QString::fromStdString(draftId) + "&expectedDraftGeneration=26";
    const Response forkTimeline = archiveClient.call("GET", draftTimelinePath);
    const QJsonArray forkCommits = forkTimeline.json.value("commits").toArray();
    check(forkTimeline.status == 200 && forkCommits.size() >= 2 &&
          forkCommits.at(0).toObject().value("revision").toObject().value("id") == secondRevision &&
          forkCommits.at(0).toObject().value("actionCount") == 0 &&
          forkCommits.at(1).toObject().value("revision").toObject().value("id") == archivedRevision &&
          forkCommits.at(1).toObject().value("actionCount") == 2 &&
          forkTimeline.json.value("draft").toObject().value("journalCount") == "1",
          "HDB-03h draft History resurrected the abandoned committed action suffix");
    const Response forkUndone = archiveClient.call("POST", archivedPath,
        liveNavigation(QString::fromStdString(native::UuidV7::generate().toString()), "26", "0"));
    const Response forkAtBase = archiveClient.call("GET", archivedCursorPath);
    const Response forkRedone = archiveClient.call("POST", archivedPath,
        liveNavigation(QString::fromStdString(native::UuidV7::generate().toString()), "27", "1"));
    const Response forkRedoView = archiveClient.call("GET", path + "/networks/" + networkId);
    const std::optional<NetworkHydraulicDistribution> forkRedoneNetwork =
        native::ModelCodec<NetworkHydraulicDistribution>::decode(QByteArray::fromBase64(
            forkRedoView.json.value("payloadBase64").toString().toLatin1()));
    check(forkUndone.status == 200 && forkUndone.json.value("generation") == "27" &&
          forkAtBase.json.value("selectionKind") == "draft" &&
          forkAtBase.json.value("nextAction").toObject().value("actionPosition") == "1" &&
          forkAtBase.json.value("previousAction").toObject().value("revisionId") == archivedRevision &&
          forkRedone.status == 200 && forkRedone.json.value("generation") == "28" &&
          forkRedoView.status == 200 && forkRedoneNetwork &&
          forkRedoneNetwork->nodes_junctions.last().elevation_m == 91,
          "HDB-03h Undo/Redo of forked MODEL action lost its archived parent");
    const native::LocalCommitInfo forkCommit = service.commitWorkingCopy(
        native::UuidV7::generate().toString(), draftId, sessionId, renewed.token,
        28, native::RevisionId::parse(secondCommit.reservedRevisionId), commitMetadata(),
        [](const native::MergeCandidate& candidate) {
            return native::annotations::validateAnnotationSnapshot(candidate);
        });
    const std::optional<native::RevisionId> forkRevision =
        native::RevisionId::parse(forkCommit.reservedRevisionId);
    const Response committedForkTimeline = archiveClient.call("GET", timelinePath);
    const QJsonArray committedForks = committedForkTimeline.json.value("commits").toArray();
    check(forkCommit.state == native::LocalCommitState::Succeeded && forkRevision &&
          service.optionalHead(project.id, "main") == forkRevision &&
          service.revision(project.id, *forkRevision).parents.front().id ==
              *native::RevisionId::parse(secondCommit.reservedRevisionId) &&
          committedForkTimeline.status == 200 && committedForks.size() >= 3 &&
          committedForks.at(0).toObject().value("revision").toObject().value("id") ==
              QString::fromStdString(forkCommit.reservedRevisionId) &&
          committedForks.at(0).toObject().value("logicalParentRevisionId") == secondRevision &&
          committedForks.at(0).toObject().value("actionCount") == 1 &&
          committedForks.at(1).toObject().value("revision").toObject().value("id") == secondRevision &&
          committedForks.at(1).toObject().value("actionCount") == 0 &&
          committedForks.at(2).toObject().value("revision").toObject().value("id") == archivedRevision &&
          service.localCommitActionHistory(secondCommit.id).actions.size() == 1,
          "HDB-03h Commit rewrote the DAG or lost the forked logical action lineage");
    archiveServer.stop();
}
#endif


// R6 over HTTP: the draft list is paged and project-filtered (no root-wide
// 1000-draft failure), and a draft can be discarded with its fencing token.
void exerciseWorkingCopyListAndDiscardHttp() {
    namespace native = aowis::dvmdb;
    TemporaryRoot root;
    native::ProjectService service(root.root());
    const std::string first = service.createUnbornProject("draft paging A").id;
    const std::string second = service.createUnbornProject("draft paging B").id;
    service.configureRepositoryRole(native::RepositoryRole::Private);
    TestServer server(service, native::BundleLimits{4ULL * 1024ULL * 1024ULL, 10000ULL, 1024ULL * 1024ULL});
    Client client(server.port());
    const std::string session = native::UuidV7::generate().toString();
    std::vector<QString> all;
    std::vector<QString> ofFirst;
    for (unsigned i = 0; i < 5; ++i) {
        const std::string project = i == 2 ? second : first;
        const QString id = QString::fromStdString(
            service.createWorkingCopy(project, "main", std::nullopt, session).id);
        all.push_back(id);
        if (project == first) ofFirst.push_back(id);
    }
    std::sort(all.begin(), all.end());
    std::sort(ofFirst.begin(), ofFirst.end());
    const QString listPath = QStringLiteral("/dvmdb/v1/working-copies");
    std::vector<QString> seen;
    QString cursor;
    for (int page = 0; page < 10; ++page) {
        const Response response = client.call("GET", listPath + "?limit=2" +
            (cursor.isEmpty() ? QString() : QStringLiteral("&after=") + cursor));
        check(response.status == 200 && response.json.value("workingCopies").toArray().size() <= 2,
              "HTTP draft page malformed");
        for (const QJsonValue& item : response.json.value("workingCopies").toArray())
            seen.push_back(item.toObject().value("id").toString());
        if (response.json.value("nextCursor").isNull()) break;
        cursor = response.json.value("nextCursor").toString();
    }
    check(seen == all, "HTTP draft paging skipped, repeated or misordered drafts");
    const Response filtered = client.call("GET", listPath + "?projectId=" +
                                          QString::fromStdString(first) + "&limit=1000");
    std::vector<QString> filteredIds;
    for (const QJsonValue& item : filtered.json.value("workingCopies").toArray())
        filteredIds.push_back(item.toObject().value("id").toString());
    check(filtered.status == 200 && filteredIds == ofFirst &&
          filtered.json.value("nextCursor").isNull(), "HTTP project-filtered draft list is wrong");
    check(client.call("GET", listPath + "?limit=0").status == 400 &&
          client.call("GET", listPath + "?limit=1001").status == 400 &&
          client.call("GET", listPath + "?after=garbage").status == 400 &&
          client.call("GET", listPath + "?unknown=1").status == 400,
          "HTTP draft list accepted invalid paging parameters");

    const QString victim = ofFirst.front();
    const std::string token = service.acquireWorkingCopyLease(victim.toStdString(), session, 120).token;
    const QString discardPath = listPath + "/" + victim + "/discard";
    const QJsonObject stale{{"sessionId", QString::fromStdString(session)},
                            {"fencingToken", QString::fromStdString(native::UuidV7::generate().toString())},
                            {"expectedGeneration", "0"}};
    check(client.call("POST", discardPath, stale).status == 409,
          "HTTP discard accepted a stale fencing token");
    const QJsonObject discard{{"sessionId", QString::fromStdString(session)},
                              {"fencingToken", QString::fromStdString(token)},
                              {"expectedGeneration", "0"}};
    const Response discarded = client.call("POST", discardPath, discard);
    check(discarded.status == 200 && discarded.json.value("discarded") == QJsonValue(true),
          "HTTP discard failed");
    check(client.call("GET", listPath + "/" + victim).status == 404 &&
          client.call("POST", discardPath, discard).status == 404,
          "HTTP discarded draft is still visible or a repeat did not answer 404");
    check(client.call("GET", listPath + "?limit=1000").json.value("workingCopies").toArray().size() ==
              static_cast<qsizetype>(all.size() - 1),
          "HTTP discard removed the wrong number of drafts");
}

// The GUI uses UUID-addressed destructive routes rather than deleting a
// working copy and branch in separate non-atomic HTTP requests. Exercise the
// same paths with real saved drafts, including stale confirmations.
void exerciseHistoryDeletionHttp() {
    namespace native = aowis::dvmdb;
    TemporaryRoot root;
    native::ProjectService service(root.root());
    const native::ProjectInfo empty = service.createUnbornProject("HTTP deletion");
    const std::string projectId = empty.id;
    const native::BranchId doomed = service.createUnbornBranch(projectId, "doomed");
    const native::BranchId surviving = service.createUnbornBranch(projectId, "surviving");
    service.configureRepositoryRole(native::RepositoryRole::Private);
    const std::string session = native::UuidV7::generate().toString();
    const native::WorkingCopyInfo doomedDraft =
        service.createWorkingCopy(projectId, "doomed", std::nullopt, session);
    const native::WorkingCopyInfo survivingDraft =
        service.createWorkingCopy(projectId, "surviving", std::nullopt, session);
    TestServer server(service,
        native::BundleLimits{4ULL * 1024ULL * 1024ULL, 10000ULL, 1024ULL * 1024ULL});
    Client client(server.port());
    const QString prefix = QStringLiteral("/dvmdb/v1/projects/") +
        QString::fromStdString(projectId) + "/branch-ids/";
    const QString doomedPath = prefix + QString::fromStdString(doomed.toString());
    const QString survivingPath = prefix + QString::fromStdString(surviving.toString());
    const QString doomedDraftPath = QStringLiteral("/dvmdb/v1/working-copies/") +
        QString::fromStdString(doomedDraft.id);
    const QString survivingDraftPath = QStringLiteral("/dvmdb/v1/working-copies/") +
        QString::fromStdString(survivingDraft.id);
    const QJsonObject stale{{"expectedHead", revisionText(native::RevisionId::generate())},
                            {"discardWorkingCopies", true}};
    const QJsonObject withoutDiscard{{"expectedHead", QJsonValue(QJsonValue::Null)},
                                     {"discardWorkingCopies", false}};
    const QJsonObject confirmed{{"expectedHead", QJsonValue(QJsonValue::Null)},
                                {"discardWorkingCopies", true}};
    check(client.call("POST", doomedPath + "/delete", stale).status == 409 &&
          client.call("POST", doomedPath + "/delete", withoutDiscard).status == 409 &&
          client.call("POST", doomedPath + "/delete",
                      QJsonObject{{"discardWorkingCopies", true}}).status == 400 &&
          client.call("POST", doomedPath + "/delete",
                      QJsonObject{{"expectedHead", QJsonValue(QJsonValue::Null)},
                                  {"discardWorkingCopies", "yes"}}).status == 400 &&
          client.call("GET", doomedPath).status == 200 &&
          client.call("GET", doomedDraftPath).status == 200,
          "HTTP branch delete bypassed confirmation or changed an unconfirmed draft");
    const Response removed = client.call("POST", doomedPath + "/delete", confirmed);
    check(removed.status == 200 && removed.json.value("deleted") == QJsonValue(true) &&
          removed.json.value("branchId").toString() ==
              QString::fromStdString(doomed.toString()) &&
          client.call("GET", doomedPath).status == 404 &&
          client.call("GET", doomedDraftPath).status == 404 &&
          client.call("GET", survivingPath).status == 200 &&
          client.call("GET", survivingDraftPath).status == 200,
          "HTTP delete failed to atomically remove the selected branch and its drafts");
    check(client.call("POST", doomedPath + "/delete", confirmed).status == 404,
          "HTTP delete unexpectedly replayed an already completed deletion");

    // Removing the first committed revision deletes its complete branch and
    // associated drafts. Invalid revision selection must not discard either.
    const native::ProjectInfo committed = service.createProject(
        "HTTP root truncation", commitMetadata());
    const native::RevisionId rootHead = service.head(committed.id, "main");
    const QString committedPrefix = QStringLiteral("/dvmdb/v1/projects/") +
        QString::fromStdString(committed.id) + "/branch-ids/";
    const QString mainPath = committedPrefix + branchIdOf(
        service, QString::fromStdString(committed.id), "main");
    const native::WorkingCopyInfo committedDraft = service.createWorkingCopy(
        committed.id, "main", rootHead, session);
    const QString committedDraftPath = QStringLiteral("/dvmdb/v1/working-copies/") +
        QString::fromStdString(committedDraft.id);
    const QJsonObject truncate{{"expectedHead", revisionText(rootHead)},
                               {"firstRemoved", revisionText(rootHead)},
                               {"discardWorkingCopies", true}};
    check(client.call("POST", mainPath + "/truncate",
            QJsonObject{{"expectedHead", revisionText(native::RevisionId::generate())},
                        {"firstRemoved", revisionText(rootHead)},
                        {"discardWorkingCopies", true}}).status == 409 &&
          client.call("POST", mainPath + "/truncate",
            QJsonObject{{"expectedHead", revisionText(rootHead)},
                        {"firstRemoved", revisionText(native::RevisionId::generate())},
                        {"discardWorkingCopies", true}}).status != 200 &&
          client.call("POST", mainPath + "/truncate",
            QJsonObject{{"expectedHead", revisionText(rootHead)},
                        {"firstRemoved", revisionText(rootHead)},
                        {"discardWorkingCopies", false}}).status == 409 &&
          client.call("GET", committedDraftPath).status == 200 &&
          client.call("GET", mainPath).status == 200,
          "HTTP truncation accepted a stale HEAD, bad revision or unconfirmed draft discard");
    const Response truncated = client.call("POST", mainPath + "/truncate", truncate);
    check(truncated.status == 200 && truncated.json.value("deleted") == QJsonValue(true) &&
          truncated.json.value("branchDeleted") == QJsonValue(true) &&
          client.call("GET", mainPath).status == 404 &&
          client.call("GET", committedDraftPath).status == 404,
          "HTTP first-revision truncation failed to remove the branch and draft");
}

} // namespace

// Every route that creates a revision requires an author and (except merges
// and project creation, which have defaults) a message; revision reads,
// history and the graph return them with the publication time.
void exerciseCommitMetadataHttp() {
    namespace native = aowis::dvmdb;
    TemporaryRoot temp;
    native::ProjectService service(temp.root());
    TestServer server(service, native::BundleLimits{4ULL * 1024ULL * 1024ULL, 10000, 1024ULL * 1024ULL});
    Client client(server.port());
    const QJsonObject author = Client::testAuthor();

    check(client.callExact("POST", "/dvmdb/v1/projects", QJsonObject{{"name", "anonymous"}}).status == 400,
          "a project with a first revision was created without an author");
    check(client.callExact("POST", "/dvmdb/v1/projects",
              QJsonObject{{"name", "unborn"}, {"unborn", true}, {"author", author}}).status == 400,
          "an unborn project accepted an author for a revision it does not have");
    const qint64 before = QDateTime::currentMSecsSinceEpoch();
    const Response created = client.callExact("POST", "/dvmdb/v1/projects",
        QJsonObject{{"name", "Authored"}, {"author", author}});
    check(created.status == 201, "project creation with an author failed");
    const QString path = QStringLiteral("/dvmdb/v1/projects/") + created.json.value("id").toString();
    const QString root = client.call("GET", path + "/branches/main/head").json.value("id").toString();
    const Response rootRecord = client.call("GET", path + "/revisions/" + root);
    check(rootRecord.json.value("author").toObject() == author &&
          rootRecord.json.value("message").toString() == "Create project Authored",
          "the first revision lost its author or default message");

    const QString commitPath = path + "/branches/main/model-commits";
    const QJsonObject change{{"uuid", "00000000-0000-0000-0000-000000000011"}, {"kind", 31},
                             {"references", QJsonArray{}}, {"payloadBase64", "Cw=="}};
    const QJsonObject base{{"expectedHead", root}, {"changes", QJsonArray{change}}};
    const auto rejected = [&](QJsonObject body, const char* message) {
        check(client.callExact("POST", commitPath, body).status == 400, message);
    };
    rejected(base, "model commit without author accepted");
    QJsonObject body = base;
    body.insert("author", author);
    rejected(body, "model commit without message accepted");
    body.insert("message", "");
    rejected(body, "model commit with an empty message accepted");
    body.insert("message", "carriage\rreturn");
    rejected(body, "model commit with a carriage return accepted");
    body.insert("message", QString(65537, QChar('m')));
    rejected(body, "model commit with an oversized message accepted");
    body.insert("message", "fine");
    body.insert("author", QJsonObject{{"id", "018f84c0-0000-4000-8000-00000000a001"},
                                      {"name", "v4 author"}});
    rejected(body, "model commit with a non-UUIDv7 author accepted");
    body.insert("author", QJsonObject{{"id", author.value("id")}, {"name", "tab\tname"}});
    rejected(body, "model commit with a control character in the author name accepted");
    body.insert("author", QJsonObject{{"id", author.value("id")}, {"name", ""}});
    rejected(body, "model commit with an empty author name accepted");
    body.insert("author", QJsonObject{{"id", author.value("id")}, {"name", "A"}, {"email", "x"}});
    rejected(body, "model commit with an unknown author field accepted");
    check(client.call("GET", path + "/branches/main/head").json.value("id").toString() == root,
          "a rejected model commit moved HEAD");

    const QJsonObject alice{{"id", "018f84c0-0000-7000-8000-00000000a0a1"}, {"name", "Alice Ünal"}};
    body = base;
    body.insert("author", alice);
    body.insert("message", "Add reservoir\n\nwith head 12 m\tok");
    const Response committed = client.callExact("POST", commitPath, body);
    check(committed.status == 201, "authored model commit failed");
    const QString first = committed.json.value("id").toString();
    const qint64 after = QDateTime::currentMSecsSinceEpoch();
    const Response record = client.call("GET", path + "/revisions/" + first);
    bool numeric = false;
    const qint64 committedAt = record.json.value("committedAtMs").toString().toLongLong(&numeric);
    check(record.json.value("author").toObject() == alice &&
          record.json.value("message").toString() == "Add reservoir\n\nwith head 12 m\tok" &&
          record.json.value("committedAtMs").isString() && numeric &&
          committedAt >= before && committedAt <= after,
          "revision read did not return author, message and commit time");
    const QJsonArray history =
        client.call("GET", path + "/branches/main/history").json.value("revisions").toArray();
    check(!history.isEmpty() && history.at(0).toObject().value("author").toObject() == alice &&
          history.at(0).toObject().value("message") == record.json.value("message") &&
          history.at(0).toObject().value("committedAtMs") == record.json.value("committedAtMs"),
          "history did not return revision metadata");
    bool inGraph = false;
    for (const QJsonValue& revision :
         client.call("GET", path + "/revision-graph").json.value("revisions").toArray())
        if (revision.toObject().value("id").toString() == first)
            inGraph = revision.toObject().value("author").toObject() == alice;
    check(inGraph, "revision graph did not return revision metadata");

    // Merges: the message defaults to "Merge <source> into <target>".
    QString mainId;
    for (const QJsonValue& value :
         client.call("GET", path + "/revision-graph?limit=1").json.value("heads").toArray())
        if (value.toObject().value("branch").toString() == QStringLiteral("main"))
            mainId = value.toObject().value("branchId").toString();
    check(client.call("POST", path + "/branches",
                      QJsonObject{{"name", "feature"}, {"fromBranchId", mainId}}).status == 201,
          "feature branch creation failed");
    QJsonObject featureChange = change;
    featureChange.insert("uuid", "00000000-0000-0000-0000-000000000012");
    const QString feature = client.call("POST", path + "/branches/feature/model-commits",
        QJsonObject{{"expectedHead", first}, {"changes", QJsonArray{featureChange}}})
        .json.value("id").toString();
    QJsonObject secondChange = change;
    secondChange.insert("uuid", "00000000-0000-0000-0000-000000000013");
    const QString second = client.call("POST", commitPath,
        QJsonObject{{"expectedHead", first}, {"changes", QJsonArray{secondChange}}})
        .json.value("id").toString();
    const QJsonObject merge{{"target", "main"}, {"source", "feature"},
                            {"expectedTargetHead", second}, {"expectedSourceHead", feature}};
    check(client.callExact("POST", path + "/merges", merge).status == 400,
          "merge without an author accepted");
    QJsonObject authoredMerge = merge;
    authoredMerge.insert("author", alice);
    const Response merged = client.callExact("POST", path + "/merges", authoredMerge);
    check(merged.status == 200 && merged.json.value("status").toString() == "merged",
          "authored merge failed");
    const Response mergeRecord =
        client.call("GET", path + "/revisions/" + merged.json.value("head").toString());
    check(mergeRecord.json.value("author").toObject() == alice &&
          mergeRecord.json.value("message").toString() == "Merge feature into main",
          "merge revision lost its author or default message");
}

void exercisePinnedRepositoryMetadataHttp() {
    namespace native = aowis::dvmdb;
    TemporaryRoot temp;
    native::ProjectService service(temp.root());
    TestServer server(service, native::BundleLimits{4ULL * 1024ULL * 1024ULL, 10000, 1024ULL * 1024ULL});
    Client reader(server.port());
    httplib::Client client("127.0.0.1", server.port());
    client.set_read_timeout(std::chrono::seconds(5));
    const std::string correct = service.repositoryId();
    const std::string wrong = "018f84c0-0000-7000-8000-0000000000af";
    check(correct != wrong, "test repository UUID unexpectedly equals wrong UUID");
    const httplib::Headers wrongHeaders{{"X-AOWIS-Expected-Repository-Id", wrong}};
    const httplib::Headers correctHeaders{{"X-AOWIS-Expected-Repository-Id", correct}};
    const QJsonObject projectBody{{"name", "Pinned"}, {"unborn", true}};
    const std::string projectJson = QJsonDocument(projectBody).toJson(QJsonDocument::Compact).toStdString();
    const httplib::Result refused = client.Post("/dvmdb/v1/projects", wrongHeaders,
                                                projectJson, "application/json");
    check(refused && refused->status == 409, "foreign repository accepted pinned project creation");
    check(QJsonDocument::fromJson(QByteArray::fromStdString(refused->body)).object()
              .value("error").toObject().value("code").toString() == "repository_mismatch",
          "pinned repository mismatch did not return a distinct error code");
    check(reader.call("GET", "/dvmdb/v1/projects").json.value("projects").toArray().isEmpty(),
          "foreign pinned project write mutated the catalogue");
    const httplib::Result created = client.Post("/dvmdb/v1/projects", correctHeaders,
                                                projectJson, "application/json");
    check(created && created->status == 201, "correctly pinned project create failed");
    const QJsonObject project = QJsonDocument::fromJson(QByteArray::fromStdString(created->body)).object();
    const QString projectId = project.value("id").toString();
    check(!projectId.isEmpty(), "pinned project creation lost its identity");
    const std::string path = "/dvmdb/v1/projects/" + projectId.toStdString();
    const std::string renameJson = QJsonDocument(QJsonObject{{"expectedName", "Pinned"},
        {"name", "Incorrect"}}).toJson(QJsonDocument::Compact).toStdString();
    const httplib::Result renameRefused = client.Patch(path, wrongHeaders, renameJson, "application/json");
    check(renameRefused && renameRefused->status == 409,
          "foreign repository accepted pinned project rename");
    check(reader.call("GET", QString::fromStdString(path)).json.value("name").toString() == "Pinned",
          "foreign pinned rename changed project metadata");
    const httplib::Result renameOk = client.Patch(path, correctHeaders, renameJson, "application/json");
    check(renameOk && renameOk->status == 200, "correctly pinned rename failed");
    const std::string branchBody = QJsonDocument(QJsonObject{{"name", "experiment"},
        {"unborn", true}}).toJson(QJsonDocument::Compact).toStdString();
    const httplib::Result branchRefused = client.Post(path + "/branches", wrongHeaders,
        branchBody, "application/json");
    check(branchRefused && branchRefused->status == 409,
          "foreign repository accepted pinned branch creation");
    check(reader.call("GET", QString::fromStdString(path + "/branches")).json
        .value("branches").toArray().size() == 1, "foreign pinned branch write changed branch set");
    const httplib::Result branchOk = client.Post(path + "/branches", correctHeaders,
        branchBody, "application/json");
    check(branchOk && branchOk->status == 201, "correctly pinned branch create failed");
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        check(argc == 2, "expected path to aowis-server-dvmdb executable");
        // Exercise the canonical UUIDv7 HTTP routes; every revision identity
        // on these routes is a UUIDv7.
        exerciseTransferStagingBudget();
        exercisePinnedRepositoryMetadataHttp();
        exercise();
        exerciseConcurrency();
        exerciseRevisionGraphHttp();
        exerciseRevisionBundleHttp();
        exerciseUnbornProjectHttp();
        exerciseUnbornCheckoutHttp();
        exerciseWorkingCopyHttp();
        exerciseWorkingCopyListAndDiscardHttp();
        exerciseHistoryDeletionHttp();
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
        exerciseUnbornNetworkInitializationHttp();
        exerciseAnnotationDraftHttp();
        exerciseUnbornAnnotationHistoryHttp();
        exerciseCompleteNetworkDraftImportHttp();
        exerciseHydraulicDraftMembershipHttp();
#endif
        exerciseUuidv7HistoryHttp();
        exerciseCommitMetadataHttp();
        exerciseExecutable(QString::fromLocal8Bit(argv[1]));
        std::cout << "HTTP project service tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HTTP service test failed: " << error.what() << '\n';
        return 1;
    }
}
