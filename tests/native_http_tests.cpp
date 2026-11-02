#include "aowis_model_codec_fingerprint.h"
#include "commit_metadata_fixture.h"
#include "model_edit_event_fixture.h"
#include "aowis/dvmdb/dvmdb_http_routes.h"
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
#include "aowis/dvmdb/hydraulic_network_persistence.h"
#include <aowis/model/edit/model_edit_transaction.h>
#endif

#include <httplib.h>

#include <QCoreApplication>
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
        const QString encodedEvent = QString::fromLatin1(eventBytes.toBase64());
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
        const native::ProjectInfo project = service.createUnbornProject("first hydraulic network");
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
        check(initialized.status == 200 && initialized.json.value("generation") == QStringLiteral("1") &&
              initialized.json.value("dirty").toBool() &&
              initialized.json.value("changes").toArray().size() == 1 &&
              initialized.json.value("changes").toArray().first().toObject()
                  .value("kind").toInt() == 100 &&
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
        const std::optional<NetworkHydraulic> firstNetwork =
            native::ModelCodec<NetworkHydraulic>::decode(firstPayload);
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
        QString encodingError;
        const QByteArray frame = aowis::model::ModelEditTransactionCodec::encode(event, &encodingError);
        check(!frame.isEmpty(), "failed to encode first unborn junction event");
        const QJsonObject save{{"sessionId", session}, {"fencingToken", token},
            {"expectedGeneration", QStringLiteral("1")},
            {"changes", QJsonArray{QJsonObject{{"uuid", junctionId}, {"kind", 1},
                {"references", QJsonArray{}},
                {"payloadBase64", QString::fromLatin1(bytes.toBase64())}}}},
            {"membership", QJsonArray{QJsonObject{{"networkId", networkId},
                {"entityId", junctionId}, {"kind", 1}, {"collectionIndex", 9},
                {"inserted", true}}}},
            {"events", QJsonArray{QString::fromLatin1(frame.toBase64())}}};
        const Response saved = client.call("POST", path + "/save-events", save);
        check(saved.status == 200 && saved.json.value("generation") == QStringLiteral("2") &&
              !service.optionalHead(project.id, "main").has_value(),
              "first hydraulic member could not be saved into unborn draft");
        const Response populatedSummary = client.call("GET", path + "/networks/summary");
        check(populatedSummary.status == 200 && populatedSummary.json.value("generation") == "2" &&
              populatedSummary.json.value("networkCount") == 1 &&
              populatedSummary.json.value("networks").toArray().first().toObject()
                  .value("counts").toObject().value("junctions") == 1,
              "private summary missed a saved uncommitted junction");
        const Response populatedView = client.call("GET", path + "/networks/" + networkId);
        const QByteArray populatedPayload = QByteArray::fromBase64(
            populatedView.json.value("payloadBase64").toString().toLatin1());
        const std::optional<NetworkHydraulic> populatedNetwork =
            native::ModelCodec<NetworkHydraulic>::decode(populatedPayload);
        check(populatedView.status == 200 && populatedView.json.value("generation") == "2" &&
              populatedNetwork && populatedNetwork->nodes_junctions.size() == 1 &&
              populatedNetwork->nodes_junctions.first().uuid == junctionUuid &&
              populatedNetwork->nodes_junctions.first().elevation_m == 12 &&
              !service.optionalHead(project.id, "main").has_value(),
              "saved membership did not hydrate canonical draft network");
        const Response after = client.call("POST", path + "/initialize-network", request);
        check(after.status == 409, "initialization replay was accepted after later edit");
        server.stop();
    }
    {
        native::ProjectService reopened(root.root());
        const native::WorkingCopyInfo draft = reopened.workingCopy(draftId);
        check(!draft.baseRevision && draft.generation == 2 && draft.journal.size() == 1 &&
              draft.candidateChanges.size() >= 3 &&
              !reopened.optionalHead(projectId, "main").has_value(),
              "first hydraulic network and member did not survive service restart");
        TestServer server(reopened, native::BundleLimits{4ULL * 1024ULL * 1024ULL,
                                                        10000ULL, 1024ULL * 1024ULL});
        Client client(server.port());
        const QString reopenedPath = QStringLiteral("/dvmdb/v1/working-copies/")
            + QString::fromStdString(draftId);
        const Response recoveredSummary = client.call("GET", reopenedPath + "/networks/summary");
        check(recoveredSummary.status == 200 && recoveredSummary.json.value("generation") == "2" &&
              recoveredSummary.json.value("networkCount") == 1 &&
              recoveredSummary.json.value("networks").toArray().first().toObject()
                  .value("counts").toObject().value("junctions") == 1,
              "private network summary was lost after DVMDB restart");
        const Response recovered = client.call("GET", QStringLiteral("/dvmdb/v1/working-copies/")
            + QString::fromStdString(draftId) + "/networks/" + networkId);
        const std::optional<NetworkHydraulic> restored = native::ModelCodec<NetworkHydraulic>::decode(
            QByteArray::fromBase64(recovered.json.value("payloadBase64").toString().toLatin1()));
        check(recovered.status == 200 && recovered.json.value("generation") == "2" &&
              restored && restored->nodes_junctions.size() == 1 &&
              restored->nodes_junctions.first().uuid == junctionUuid,
              "restart failed to hydrate saved unborn hydraulic draft");
        server.stop();
    }
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
        NetworkHydraulic imported;
        imported.uuid = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
        imported.id = QStringLiteral("imported-water");
        HydraulicNodeJunction junction;
        junction.uuid = QUuid(QString::fromStdString(native::UuidV7::generate().toString()));
        junction.id = QStringLiteral("J-01");
        junction.elevation_m = 17;
        imported.nodes_junctions.append(junction);
        networkId = imported.uuid.toString(QUuid::WithoutBraces);
        const QByteArray payload = native::ModelCodec<NetworkHydraulic>::encode(imported);
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
        NetworkHydraulic replacement = imported;
        replacement.nodes_junctions[0].elevation_m = 99;
        QJsonObject replacementRequest = request;
        replacementRequest.insert("expectedGeneration", QStringLiteral("1"));
        replacementRequest.insert("payloadBase64", QString::fromLatin1(
            native::ModelCodec<NetworkHydraulic>::encode(replacement).toBase64()));
        check(client.call("POST", path + "/import-network", replacementRequest).status == 409,
              "populated private HEAD accepted a replacement network");
        const Response summary = client.call("GET", path + "/networks/summary");
        check(summary.status == 200 && summary.json.value("networkCount") == 1 &&
              summary.json.value("networks").toArray().first().toObject()
                  .value("counts").toObject().value("junctions") == 1,
              "full import did not materialize all network collections");
        const Response loaded = client.call("GET", path + "/networks/" + networkId);
        const std::optional<NetworkHydraulic> restored = native::ModelCodec<NetworkHydraulic>::decode(
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
    NetworkHydraulic network;
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
        base = native::hydraulic::saveHydraulicNetwork(repository, projectUuid,
                                                         "main", network, head, commitMetadata());
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
    const std::optional<NetworkHydraulic> baseNetwork = native::ModelCodec<NetworkHydraulic>::decode(
        QByteArray::fromBase64(baseView.json.value("payloadBase64").toString().toLatin1()));
    check(baseView.status == 200 && baseView.json.value("generation") == "0" &&
          baseNetwork && baseNetwork->nodes_junctions.size() == 1 &&
          baseNetwork->nodes_junctions.first().uuid == oldUuid,
          "pinned committed base did not hydrate into draft");
    const Response leased = client.call("POST", path + "/lease",
                                        QJsonObject{{"sessionId", session}, {"seconds", 60}});
    check(leased.status == 200, "hydraulic draft lease failed");
    const QString token = leased.json.value("token").toString();
    HydraulicNodeJunction added;
    added.uuid = newUuid;
    added.id = QStringLiteral("J2");
    added.elevation_m = 55;
    const QByteArray encoded = native::ModelCodec<HydraulicNodeJunction>::encode(added);
    const QJsonObject addition{{"uuid", newId}, {"kind", 1}, {"references", QJsonArray{}},
                               {"payloadBase64", QString::fromLatin1(encoded.toBase64())}};
    const QJsonObject insertion{{"networkId", networkId}, {"entityId", newId},
                                {"kind", 1}, {"collectionIndex", 9}, {"inserted", true}};
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
        {"networkId", networkId}, {"entityId", newId}, {"kind", 1},
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
    const std::optional<NetworkHydraulic> combinedNetwork = native::ModelCodec<NetworkHydraulic>::decode(
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
                              {"kind", 1}, {"collectionIndex", 9}, {"inserted", false}};
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
    const std::optional<NetworkHydraulic> finalNetwork = native::ModelCodec<NetworkHydraulic>::decode(
        QByteArray::fromBase64(finalView.json.value("payloadBase64").toString().toLatin1()));
    check(finalView.status == 200 && finalView.json.value("generation") == "2" &&
          finalNetwork && finalNetwork->nodes_junctions.size() == 1 &&
          finalNetwork->nodes_junctions.first().uuid == newUuid,
          "deleted pinned-base member was resurrected during draft hydration");
    server.stop();
    const native::WorkingCopyInfo persisted = service.workingCopy(
        created.json.value("id").toString().toStdString());
    check(persisted.generation == 2 && persisted.journal.size() == 2 &&
          persisted.candidateChanges.size() >= 3,
          "hydraulic member changes not durable as one candidate and journal");
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

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        check(argc == 2, "expected path to aowis-server-dvmdb executable");
        // Exercise the canonical UUIDv7 HTTP routes; every revision identity
        // on these routes is a UUIDv7.
        exerciseTransferStagingBudget();
        exercise();
        exerciseConcurrency();
        exerciseRevisionGraphHttp();
        exerciseRevisionBundleHttp();
        exerciseUnbornProjectHttp();
        exerciseUnbornCheckoutHttp();
        exerciseWorkingCopyHttp();
        exerciseWorkingCopyListAndDiscardHttp();
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
        exerciseUnbornNetworkInitializationHttp();
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
