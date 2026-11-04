#include "aowis_model_codec_fingerprint.h"
#include "aowis/dvmdb/dvmdb_http_routes.h"
#include "aowis/dvmdb/storage_error.h"
#include "aowis/dvmdb/revision_record.h"

#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
#include "aowis/dvmdb/hydraulic_network_persistence.h"
#include <QByteArray>
#include <aowis/model/edit/model_edit_transaction.h>
#include <aowis/model/edit/hydraulic_root_edit.h>
#include <variant>
#include <type_traits>
#endif

#include <httplib.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QCryptographicHash>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

namespace aowis::dvmdb::http {
namespace {

// Response size bounds of list-like routes; larger results return 413 and
// must use the native library. Enforced before the result is materialized.
constexpr std::size_t maxHttpDiffRecords = 1000;
constexpr std::size_t maxHttpEntityIds = 10000;


// Exact route patterns (also used to select per-route JSON body limits).
const char* const routeProjects = "/dvmdb/v1/projects";
const char* const routeRepositoryRole = "/dvmdb/v1/repository/role";
const char* const routeWorkingCopies = "/dvmdb/v1/working-copies";
const char* const routeWorkingCopySave = "/dvmdb/v1/working-copies/:draft/save";
const char* const routeWorkingCopyInitializeNetwork = "/dvmdb/v1/working-copies/:draft/initialize-network";
const char* const routeWorkingCopyImportNetwork = "/dvmdb/v1/working-copies/:draft/import-network";
const char* const routeWorkingCopyEventSave = "/dvmdb/v1/working-copies/:draft/save-events";
const char* const routeWorkingCopyHistoryNavigate = "/dvmdb/v1/working-copies/:draft/history/navigate";
const char* const routeWorkingCopyLease = "/dvmdb/v1/working-copies/:draft/lease";
const char* const routeWorkingCopyRelease = "/dvmdb/v1/working-copies/:draft/release";
const char* const routeWorkingCopyDiscard = "/dvmdb/v1/working-copies/:draft/discard";
const char* const routeBranches = "/dvmdb/v1/projects/:id/branches";
const char* const routeRevisionBundle = "/dvmdb/v1/projects/:id/branches/:branch/bundle";
const char* const revisionBundleMime = "application/vnd.aowis.revision-bundle-v2";
const char* const routeNetworks = "/dvmdb/v1/projects/:id/branches/:branch/networks";
const char* const routeEdits = "/dvmdb/v1/projects/:id/branches/:branch/networks/:network/edits";
const char* const routeModelCommits = "/dvmdb/v1/projects/:id/branches/:branch/model-commits";
const char* const routeMerges = "/dvmdb/v1/projects/:id/merges";
const char* const routeHydraulicMerges = "/dvmdb/v1/projects/:id/hydraulic-merges";

// A handled request produces a JSON body and a status.
struct Reply {
    int status = 200;
    QJsonObject body;
    int retryAfterSeconds = 0; // > 0: send a Retry-After header
};

void send(httplib::Response& response, const Reply& reply) {
    response.status = reply.status;
    if (reply.retryAfterSeconds > 0)
        response.set_header("Retry-After", std::to_string(reply.retryAfterSeconds));
    response.set_content(QJsonDocument(reply.body).toJson(QJsonDocument::Compact).toStdString(),
                         "application/json");
}

Reply errorReply(int status, const QString& code, const QString& message) {
    return Reply{status, QJsonObject{{"error", QJsonObject{{"code", code}, {"message", message}}}}};
}

// Rejections that leave a request body unread must close the connection;
// otherwise httplib would drain the (possibly huge) body to keep it alive.
void sendAndClose(httplib::Response& response, const Reply& reply) {
    send(response, reply);
    response.set_header("Connection", "close");
}

Reply projectFailure(const ProjectError& error) {
    int status = 400;
    QString code = QStringLiteral("invalid_request");
    switch (error.kind()) {
    case ProjectErrorKind::InvalidInput: break;
    case ProjectErrorKind::NotFound: status = 404; code = QStringLiteral("not_found"); break;
    case ProjectErrorKind::Conflict: status = 409; code = QStringLiteral("conflict"); break;
    case ProjectErrorKind::Unavailable: status = 503; code = QStringLiteral("unavailable"); break;
    case ProjectErrorKind::Busy: {
        // The project's operation queue is full; other projects are unaffected.
        Reply reply = errorReply(503, QStringLiteral("busy"), QString::fromUtf8(error.what()));
        reply.retryAfterSeconds = 1;
        return reply;
    }
    }
    return errorReply(status, code, QString::fromUtf8(error.what()));
}

Reply internalFailure(const std::exception& exception) {
    std::cerr << "AOWIS database request failed: " << exception.what() << '\n';
    return errorReply(500, QStringLiteral("internal_error"), QStringLiteral("database operation failed"));
}

// Classification is by StorageErrorKind only. Client-caused failures return
// their message; server-side failures are logged and answered generically so
// paths, errno text and corruption details never reach remote clients.
Reply storageFailure(const StorageError& error) {
    const QString message = QString::fromUtf8(error.what());
    switch (error.kind()) {
    case StorageErrorKind::InvalidInput:
        return errorReply(400, QStringLiteral("invalid_request"), message);
    case StorageErrorKind::NotFound:
        return errorReply(404, QStringLiteral("not_found"), message);
    case StorageErrorKind::Conflict:
        return errorReply(409, QStringLiteral("conflict"), message);
    case StorageErrorKind::LimitExceeded:
        return errorReply(413, QStringLiteral("limit_exceeded"), message);
    case StorageErrorKind::Busy: {
        Reply reply = errorReply(503, QStringLiteral("scratch_busy"),
                                 QStringLiteral("native internal scratch budget is busy"));
        reply.retryAfterSeconds = 1;
        return reply;
    }
    case StorageErrorKind::Unavailable:
        std::cerr << "AOWIS database unavailable: " << error.what() << '\n';
        return errorReply(503, QStringLiteral("unavailable"),
                          QStringLiteral("repository temporarily unavailable"));
    case StorageErrorKind::ResourceExhausted:
        std::cerr << "AOWIS database storage exhausted: " << error.what() << '\n';
        return errorReply(507, QStringLiteral("insufficient_storage"),
                          QStringLiteral("database storage is exhausted"));
    case StorageErrorKind::Corruption:
    case StorageErrorKind::Io:
    case StorageErrorKind::Internal:
        break;
    }
    std::cerr << "AOWIS database request failed [" << StorageError::kindName(error.kind())
              << "]: " << error.what() << '\n';
    return errorReply(500, QStringLiteral("internal_error"), QStringLiteral("database operation failed"));
}

Reply failureReply(const std::exception_ptr& failure) {
    try {
        std::rethrow_exception(failure);
    } catch (const ProjectError& error) {
        return projectFailure(error);
    } catch (const StorageError& error) {
        return storageFailure(error);
    } catch (const std::exception& error) {
        return internalFailure(error);
    } catch (...) {
        return internalFailure(std::runtime_error("non-standard exception"));
    }
}

template <typename Function>
void guarded(httplib::Response& response, Function&& function) {
    try {
        send(response, function());
    } catch (...) {
        send(response, failureReply(std::current_exception()));
    }
}

void requireInput(bool valid, const char* message) {
    if (!valid) throw ProjectError(ProjectErrorKind::InvalidInput, message);
}


// INT-01: a client that pins its selected repository must never mutate an
// endpoint which has changed ownership since discovery. The comparison is
// performed by the HTTP server immediately before entering the native write,
// not just in a client-side identity preflight. Existing native/HTTP callers
// which do not supply the header are not implicitly treated as pinned.
std::optional<Reply> repositoryPinFailure(const httplib::Request& request,
                                          const ProjectService& service) {
    constexpr const char* header = "X-AOWIS-Expected-Repository-Id";
    if (!request.has_header(header)) return std::nullopt;
    requireInput(request.get_header_value_count(header) == 1,
                 "expected repository header must appear exactly once");
    const std::string expected = request.get_header_value(header);
    requireInput(!expected.empty(), "expected repository header must not be empty");
    if (expected != service.repositoryId())
        return errorReply(409, QStringLiteral("repository_mismatch"),
                          QStringLiteral("repository identity changed"));
    return std::nullopt;
}

std::string pathParam(const httplib::Request& request, const char* name) {
    const std::unordered_map<std::string, std::string>::const_iterator found =
        request.path_params.find(name);
    requireInput(found != request.path_params.end(), "missing path parameter");
    return found->second;
}

std::optional<std::string> queryValue(const httplib::Request& request, const char* name) {
    if (!request.has_param(name)) return std::nullopt;
    requireInput(request.get_param_value_count(name) == 1, "query parameter given more than once");
    return request.get_param_value(name);
}

QJsonObject parseBody(const httplib::Request& request, std::size_t maxBytes) {
    requireInput(!request.body.empty() && request.body.size() <= maxBytes,
                 "request body is empty or exceeds the endpoint size limit");
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray(request.body.data(), static_cast<qsizetype>(request.body.size())), &error);
    requireInput(error.error == QJsonParseError::NoError && document.isObject(),
                 "expected a JSON object");
    return document.object();
}

void requireKeys(const QJsonObject& object, const QStringList& accepted) {
    for (QJsonObject::const_iterator it = object.begin(); it != object.end(); ++it)
        requireInput(accepted.contains(it.key()), "unknown JSON field");
}

std::string requiredString(const QJsonObject& object, const QString& key) {
    requireInput(object.value(key).isString(), "missing or invalid string field");
    return object.value(key).toString().toUtf8().toStdString();
}

Hash parseHash(const std::string& value) {
    requireInput(value.size() == 64, "revision hash must contain 64 lowercase hex digits");
    for (char character : value)
        requireInput((character >= '0' && character <= '9') ||
                     (character >= 'a' && character <= 'f'), "invalid revision hash");
    const Hash parsed = fromHex(value);
    requireInput(parsed != Hash{}, "zero revision hash is invalid");
    return parsed;
}

RevisionId parseRevisionId(const std::string& text) {
    const std::optional<RevisionId> id = RevisionId::parse(text);
    requireInput(id.has_value(), "revision ID must be a valid UUIDv7");
    return *id;
}

QString revisionIdString(const RevisionId& id) {
    return QString::fromStdString(id.toString());
}

Key parseModelKey(const std::string& text) {
    requireInput(text.size() == 36, "model key must be a canonical UUID");
    const QString value = QString::fromStdString(text);
    const QUuid uuid(value);
    requireInput(!uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == value,
                 "invalid model key UUID");
    const QByteArray bytes = uuid.toRfc4122();
    requireInput(bytes.size() == 16, "invalid model UUID byte length");
    Key result{};
    std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes.constData()), 16, result.begin());
    return result;
}

QString modelKeyString(const Key& key) {
    return QUuid::fromRfc4122(QByteArray(reinterpret_cast<const char*>(key.data()), 16))
        .toString(QUuid::WithoutBraces);
}

Bytes decodeModelPayload(const QJsonObject& object) {
    requireInput(object.value("payloadBase64").isString(), "missing model payloadBase64");
    const QByteArray encoded = object.value("payloadBase64").toString().toLatin1();
    requireInput(encoded.size() <= static_cast<qsizetype>(maxJsonModelBytes),
                 "model payloadBase64 exceeds size limit");
    const QByteArray::FromBase64Result decoded = QByteArray::fromBase64Encoding(
        encoded, QByteArray::AbortOnBase64DecodingErrors);
    requireInput(static_cast<bool>(decoded), "invalid model payloadBase64");
    const QByteArray& payload = decoded.decoded;
    if (payload.isEmpty()) return {};
    const std::uint8_t* first = reinterpret_cast<const std::uint8_t*>(payload.constData());
    return Bytes(first, first + payload.size());
}


// Use decimal strings for uint64 values: JSON numbers lose precision above
// 2^53 and must not silently alter the optimistic draft generation.
std::uint64_t strictUint64(const QJsonObject& object, const QString& field) {
    const std::string value = requiredString(object, field);
    requireInput(!value.empty() && value.size() <= 20 &&
                 std::all_of(value.begin(), value.end(), [](char ch) { return ch >= '0' && ch <= '9'; }),
                 "invalid unsigned decimal field");
    std::uint64_t result = 0;
    const std::from_chars_result parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    requireInput(parsed.ec == std::errc() && parsed.ptr == value.data() + value.size(),
                 "invalid or overflowing unsigned decimal field");
    return result;
}

QJsonValue optionalRevisionJson(const std::optional<RevisionId>& id) {
    return id ? QJsonValue(revisionIdString(*id)) : QJsonValue(QJsonValue::Null);
}

QJsonObject workingCopyJson(const WorkingCopyInfo& draft, bool includeChanges = true) {
    QJsonArray changes;
    if (includeChanges) for (const ModelChange& change : draft.candidateChanges) {
        const QString uuid = modelKeyString(change.key);
        if (!change.record) {
            changes.append(QJsonObject{{"uuid", uuid}, {"delete", true}});
            continue;
        }
        QJsonArray references;
        for (const Key& reference : change.record->references)
            references.append(modelKeyString(reference));
        const Bytes& payload = change.record->payload;
        const QByteArray bytes = payload.empty() ? QByteArray() :
            QByteArray(reinterpret_cast<const char*>(payload.data()),
                       static_cast<qsizetype>(payload.size()));
        changes.append(QJsonObject{{"uuid", uuid}, {"kind", static_cast<int>(change.record->kind)},
                                   {"references", references},
                                   {"payloadBase64", QString::fromLatin1(bytes.toBase64())}});
    }
    QJsonObject result{
        {"id", QString::fromStdString(draft.id)},
        {"projectId", QString::fromStdString(draft.projectId)},
        {"branchId", QString::fromStdString(draft.branchId)},
        {"baseRevision", optionalRevisionJson(draft.baseRevision)},
        {"sessionId", QString::fromStdString(draft.sessionId)},
        {"generation", QString::number(static_cast<qulonglong>(draft.generation))},
        {"dirty", draft.dirty},
        {"leaseSessionId", draft.leaseSessionId
            ? QJsonValue(QString::fromStdString(*draft.leaseSessionId)) : QJsonValue(QJsonValue::Null)},
        {"leaseExpiresAtMs", QString::number(static_cast<qulonglong>(draft.leaseExpiresAtMs))}
    };
    result.insert(QStringLiteral("eventCount"),
                  QString::number(static_cast<qulonglong>(draft.journalCount)));
    // Absent native cursor means the tip; do not confuse this active-journal
    // coordinate with the separate committed-checkpoint selection marker.
    result.insert(QStringLiteral("draftActionPosition"),
                  QString::number(static_cast<qulonglong>(
                      draft.draftActionPosition.value_or(draft.journalCount))));
    // Historical Commit positions and live draft-journal positions are
    // different coordinates. Expose the persisted provenance explicitly so
    // clients can recover the selected marker after an uncertain response.
    result.insert(QStringLiteral("archivedRevisionId"),
                  draft.historyCheckpointRevisionId
                      ? QJsonValue(QString::fromStdString(*draft.historyCheckpointRevisionId))
                      : QJsonValue(QJsonValue::Null));
    result.insert(QStringLiteral("archivedActionPosition"),
                  draft.historyActionPosition
                      ? QJsonValue(QString::number(static_cast<qulonglong>(*draft.historyActionPosition)))
                      : QJsonValue(QJsonValue::Null));
    result.insert(QStringLiteral("archiveCommitOperationId"),
                  draft.historyArchiveCommitOperationId
                      ? QJsonValue(QString::fromStdString(*draft.historyArchiveCommitOperationId))
                      : QJsonValue(QJsonValue::Null));
    if (includeChanges) {
        result.insert(QStringLiteral("changes"), changes);
        QJsonArray events;
        for (const WorkingCopyEvent& event : draft.journal) {
            const QByteArray bytes(reinterpret_cast<const char*>(event.encoded.data()),
                                   static_cast<qsizetype>(event.encoded.size()));
            events.append(QJsonObject{{"id", QString::fromStdString(event.id)},
                                      {"payloadBase64", QString::fromLatin1(bytes.toBase64())}});
        }
        result.insert(QStringLiteral("events"), events);
    }
    return result;
}

// Identical native model-record encoding for committed batches and private
// draft overlays. Draft saves never invoke the commit operation.
std::vector<ModelChange> parseModelChanges(const QJsonObject& object, bool forCommit = false,
                                          bool allowEmpty = false) {
    requireInput(object.value("changes").isArray(), "missing changes array");
    const QJsonArray entries = object.value("changes").toArray();
    requireInput((allowEmpty || !entries.isEmpty()) && entries.size() <= 128,
                 forCommit ? "model commit requires 1..128 changes" :
                             "model save requires 1..128 changes");
    std::vector<ModelChange> changes;
    std::set<Key> seen;
    changes.reserve(static_cast<std::size_t>(entries.size()));
    for (const QJsonValue& value : entries) {
        requireInput(value.isObject(), "model change must be an object");
        const QJsonObject item = value.toObject();
        requireKeys(item, {"uuid", "delete", "kind", "references", "payloadBase64"});
        const Key key = parseModelKey(requiredString(item, "uuid"));
        requireInput(seen.insert(key).second,
                     forCommit ? "duplicate model UUID in commit" : "duplicate model UUID in save");
        if (item.value("delete") == QJsonValue(true)) {
            requireInput(item.size() == 2 && item.contains("uuid"),
                         "deletion may only contain uuid and delete");
            changes.push_back(ModelChange{key, std::nullopt});
            continue;
        }
        requireInput(!item.contains("delete"), "delete must be true or absent");
        requireInput(item.value("kind").isDouble(), "missing model record kind");
        const double number = item.value("kind").toDouble();
        requireInput(number >= 0 && number <= 65535 &&
                     number == static_cast<double>(static_cast<std::uint16_t>(number)),
                     "model record kind must be uint16");
        requireInput(item.value("references").isArray(), "missing model references");
        const QJsonArray references = item.value("references").toArray();
        requireInput(references.size() <= 65535, "too many model references");
        ModelRecord record;
        record.kind = static_cast<std::uint16_t>(number);
        for (const QJsonValue& reference : references) {
            requireInput(reference.isString(), "model reference must be a UUID");
            record.references.push_back(parseModelKey(reference.toString().toStdString()));
        }
        record.payload = decodeModelPayload(item);
        changes.push_back(ModelChange{key, std::move(record)});
    }
    return changes;
}


QJsonObject revisionJson(const RevisionGraphRevision& revision) {
    QJsonArray parents;
    QJsonArray parentHashes;
    for (const RevisionGraphParent& parent : revision.parents) {
        parents.append(revisionIdString(parent.id));
        parentHashes.append(QString::fromStdString(toHex(parent.expectedRecordHash)));
    }
    return QJsonObject{{"id", revisionIdString(revision.id)},
                       {"recordHash", QString::fromStdString(toHex(revision.recordHash))},
                       {"root", QString::fromStdString(toHex(revision.root))},
                       {"referencesRoot", QString::fromStdString(toHex(revision.referencesRoot))},
                       {"modelIndexed", revision.modelIndexed}, {"parents", parents},
                       {"parentRecordHashes", parentHashes},
                       {"author", QJsonObject{
                           {"id", QString::fromStdString(revision.metadata.authorId.toString())},
                           {"name", QString::fromStdString(revision.metadata.authorName)}}},
                       {"message", QString::fromStdString(revision.displayMessage.value_or(revision.metadata.message))},
                       {"originalMessage", QString::fromStdString(revision.metadata.message)},
                       {"committedAtMs",
                        QString::number(static_cast<qulonglong>(revision.committedAtMs))}};
}

// The author and message of a request that creates a revision:
// "author": {"id": UUIDv7, "name": text} and "message": text. Merges and
// project creation may omit the message and get `defaultMessage`.
CommitMetadata parseCommitMetadata(const QJsonObject& object,
                                   std::optional<std::string> defaultMessage = std::nullopt) {
    requireInput(object.value("author").isObject(), "missing author object");
    const QJsonObject author = object.value("author").toObject();
    requireKeys(author, {"id", "name"});
    const std::optional<UuidV7> id = UuidV7::parse(requiredString(author, "id"));
    requireInput(id.has_value(), "author id must be a UUIDv7");
    CommitMetadata metadata{*id, requiredString(author, "name"), {}};
    if (object.contains("message")) {
        metadata.message = requiredString(object, "message");
    } else {
        requireInput(defaultMessage.has_value(), "missing commit message");
        metadata.message = *defaultMessage;
    }
    validateCommitMetadata(metadata);
    return metadata;
}

QString hashString(const Hash& hash) {
    return QString::fromStdString(toHex(hash));
}

QJsonValue optionalHashJson(const std::optional<Hash>& hash) {
    return hash ? QJsonValue(hashString(*hash)) : QJsonValue(QJsonValue::Null);
}

QJsonObject mergeJson(const MergeResult& result) {
    QString status = QStringLiteral("up_to_date");
    switch (result.status) {
    case MergeStatus::UpToDate: break;
    case MergeStatus::FastForward: status = QStringLiteral("fast_forward"); break;
    case MergeStatus::Merged: status = QStringLiteral("merged"); break;
    case MergeStatus::Conflicts: status = QStringLiteral("conflicts"); break;
    }
    const std::function<QJsonValue(const std::optional<RevisionId>&)> revisionValue =
        [](const std::optional<RevisionId>& id) -> QJsonValue {
            return id ? QJsonValue(revisionIdString(*id)) : QJsonValue(QJsonValue::Null);
        };
    QJsonArray conflicts;
    for (const MergeConflict& conflict : result.conflicts) {
        conflicts.append(QJsonObject{{"uuid", modelKeyString(conflict.key)},
            {"base", optionalHashJson(conflict.base)},
            {"target", optionalHashJson(conflict.target)},
            {"source", optionalHashJson(conflict.source)},
            {"reason", QString::fromStdString(conflict.reason)}});
    }
    return QJsonObject{{"status", status}, {"base", revisionValue(result.base)},
        {"targetBefore", revisionValue(result.targetBefore)},
        {"sourceHead", revisionValue(result.sourceHead)},
        {"head", revisionValue(result.resultHead)}, {"conflicts", conflicts}};
}


// A page token is authenticated, not merely encoded: callers cannot forge a
// snapshot rooted at an arbitrary immutable object. A new server process has
// a new key, so tokens issued before a restart must be reacquired.
const QByteArray& graphTokenKey() {
    static const QByteArray key = [] {
        QByteArray result(32, '\0');
        for (int i = 0; i < 32; i += 4) {
            const quint32 word = QRandomGenerator::system()->generate();
            for (int j = 0; j < 4; ++j)
                result[i + j] = static_cast<char>(word >> (8 * j));
        }
        return result;
    }();
    return key;
}

QByteArray graphSignature(const QByteArray& payload) {
    return QMessageAuthenticationCode::hash(payload, graphTokenKey(), QCryptographicHash::Sha256);
}

QJsonArray branchTagsJson(const BranchMetadata& metadata) {
    QJsonArray result;
    for (const std::string& tag : metadata.tags)
        result.append(QString::fromStdString(tag));
    return result;
}

BranchMetadata parseBranchMetadata(const QJsonObject& object) {
    BranchMetadata metadata;
    if (object.contains("description")) {
        requireInput(object.value("description").isString(), "branch description must be a string");
        metadata.description = object.value("description").toString().toUtf8().toStdString();
        requireInput(metadata.description.size() <= 512, "branch description exceeds 512 UTF-8 bytes");
    }
    if (object.contains("tags")) {
        requireInput(object.value("tags").isArray(), "branch tags must be an array");
        const QJsonArray tags = object.value("tags").toArray();
        requireInput(tags.size() <= 12, "too many branch tags");
        std::set<std::string> unique;
        for (const QJsonValue& value : tags) {
            requireInput(value.isString(), "branch tag must be a string");
            const std::string tag = value.toString().toUtf8().toStdString();
            requireInput(!tag.empty() && tag.size() <= 32 &&
                         tag.find_first_of("\n\r") == std::string::npos &&
                         unique.insert(tag).second, "invalid or duplicate branch tag");
            metadata.tags.push_back(tag);
        }
    }
    return metadata;
}

// One branch HEAD as the graph page and the branch-by-UUID route report it.
QJsonObject branchHeadJson(const RevisionGraphHead& head) {
    return QJsonObject{{"branch", QString::fromStdString(head.name)},
        {"branchId", QString::fromStdString(head.branchId.toString())},
        {"revision", head.revision
            ? QJsonValue(revisionIdString(*head.revision)) : QJsonValue(QJsonValue::Null)},
        {"recordHash", head.revision
            ? QJsonValue(hashString(head.recordHash)) : QJsonValue(QJsonValue::Null)},
        {"originBranchId", head.originBranchId
            ? QJsonValue(QString::fromStdString(head.originBranchId->toString()))
            : QJsonValue(QJsonValue::Null)},
        {"forkRevisionId", head.forkRevisionId
            ? QJsonValue(revisionIdString(*head.forkRevisionId))
            : QJsonValue(QJsonValue::Null)},
        {"description", QString::fromStdString(head.metadata.description)},
        {"tags", branchTagsJson(head.metadata)}};
}

// Graph cursors pin both logical identities and content digests. Reuse the
// process-local HMAC key so callers cannot forge a historical HEAD snapshot.
QString graphToken(const RevisionGraphCursor& cursor, const std::string& projectId) {
    QJsonArray heads;
    for (const RevisionGraphHead& head : cursor.heads) {
        heads.append(QJsonArray{QString::fromStdString(head.branchId.toString()),
                                QString::fromStdString(head.name),
                                head.revision ? QJsonValue(revisionIdString(*head.revision))
                                              : QJsonValue(QJsonValue::Null),
                                head.revision ? QJsonValue(hashString(head.recordHash))
                                              : QJsonValue(QJsonValue::Null),
                                head.originBranchId
                                    ? QJsonValue(QString::fromStdString(head.originBranchId->toString()))
                                    : QJsonValue(QJsonValue::Null),
                                head.forkRevisionId
                                    ? QJsonValue(revisionIdString(*head.forkRevisionId))
                                    : QJsonValue(QJsonValue::Null),
                                QString::fromStdString(head.metadata.description),
                                branchTagsJson(head.metadata)});
    }
    const QByteArray payload = QJsonDocument(QJsonObject{
        {"v", 2}, {"project", QString::fromStdString(projectId)},
        {"skip", static_cast<qint64>(cursor.skip)}, {"heads", heads}
    }).toJson(QJsonDocument::Compact);
    return QString::fromLatin1((payload + graphSignature(payload)).toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

RevisionGraphCursor readGraphToken(const std::string& encoded,
                                       const std::string& projectId) {
    requireInput(!encoded.empty() && encoded.size() <= 230000,
                 "invalid UUIDv7 graph cursor size");
    for (const char ch : encoded)
        requireInput((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                     (ch >= '0' && ch <= '9') || ch == '-' || ch == '_',
                     "invalid UUIDv7 graph cursor characters");
    const QByteArray bytes = QByteArray::fromBase64(QByteArray::fromStdString(encoded),
                                                    QByteArray::Base64UrlEncoding);
    requireInput(bytes.size() > 32, "invalid UUIDv7 graph cursor");
    const QByteArray payload = bytes.left(bytes.size() - 32);
    const QByteArray signature = bytes.right(32);
    const QByteArray expected = graphSignature(payload);
    unsigned difference = 0;
    for (int i = 0; i < 32; ++i)
        difference |= static_cast<unsigned char>(signature[i] ^ expected[i]);
    requireInput(difference == 0, "UUIDv7 graph cursor expired or invalid");
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    requireInput(error.error == QJsonParseError::NoError && document.isObject(),
                 "invalid UUIDv7 graph cursor JSON");
    const QJsonObject object = document.object();
    requireInput(object.size() == 4 && object.value("v").toInt() == 2 &&
                 object.value("project").toString().toStdString() == projectId &&
                 object.value("heads").isArray() && object.value("skip").isDouble(),
                 "invalid UUIDv7 graph cursor fields");
    const double skip = object.value("skip").toDouble();
    requireInput(skip >= 1 && skip <= 250000 &&
                 skip == static_cast<double>(static_cast<std::size_t>(skip)),
                 "invalid UUIDv7 graph cursor offset");
    RevisionGraphCursor cursor;
    cursor.skip = static_cast<std::size_t>(skip);
    const QJsonArray heads = object.value("heads").toArray();
    requireInput(heads.size() <= 1024, "too many UUIDv7 graph heads");
    for (const QJsonValue& value : heads) {
        requireInput(value.isArray(), "invalid UUIDv7 graph head");
        const QJsonArray head = value.toArray();
        requireInput((head.size() == 5 || head.size() == 6 || head.size() == 8) && head.at(0).isString() &&
                     head.at(1).isString() &&
                     ((head.at(2).isString() && head.at(3).isString()) ||
                      (head.at(2).isNull() && head.at(3).isNull())) &&
                     (head.at(4).isNull() || head.at(4).isString()) &&
                     (head.size() == 5 || head.at(5).isNull() || head.at(5).isString()) &&
                     (head.size() != 8 || (head.at(6).isString() && head.at(7).isArray())),
                     "invalid UUIDv7 graph head fields");
        const std::optional<BranchId> branch = BranchId::parse(head.at(0).toString().toStdString());
        requireInput(branch.has_value(), "invalid graph branch UUIDv7");
        std::optional<BranchId> origin;
        if (head.at(4).isString()) {
            origin = BranchId::parse(head.at(4).toString().toStdString());
            requireInput(origin && *origin != *branch, "invalid branch creation origin");
        }
        requireInput(head.size() < 6 || head.at(5).isNull() ||
                     (origin.has_value() && head.at(2).isString()),
                     "fork revision requires a source and committed branch HEAD");
        cursor.heads.push_back({*branch, head.at(1).toString().toStdString(),
              head.at(2).isString()
                  ? std::optional<RevisionId>(parseRevisionId(head.at(2).toString().toStdString()))
                  : std::nullopt,
              head.at(3).isString() ? parseHash(head.at(3).toString().toStdString()) : Hash{},
              origin, head.size() >= 6 && head.at(5).isString()
                  ? std::optional<RevisionId>(parseRevisionId(head.at(5).toString().toStdString()))
                  : std::nullopt,
              head.size() == 8
                  ? parseBranchMetadata(QJsonObject{{"description", head.at(6)}, {"tags", head.at(7)}})
                  : BranchMetadata{}});
    }
    return cursor;
}

QJsonObject projectJson(const ProjectInfo& project) {
    return QJsonObject{{"id", QString::fromStdString(project.id)},
                       {"name", QString::fromStdString(project.name)}};
}

void requireBranch(Repository& repository, const std::string& branch) {
    requireInput(ProjectService::validBranchName(branch), "invalid branch name");
    if (!repository.hasBranch(branch))
        throw ProjectError(ProjectErrorKind::NotFound, "branch not found");
}

// Content-Length of a request, if present and well-formed (httplib rejects
// malformed values before routing).
std::optional<std::uint64_t> declaredLength(const httplib::Request& request) {
    if (!request.has_header("Content-Length")) return std::nullopt;
    return static_cast<std::uint64_t>(std::stoull(request.get_header_value("Content-Length")));
}

std::uint64_t descriptorSize(int fd) {
    struct stat data{};
    if (::fstat(fd, &data) != 0) throwSystemError("stat exported bundle");
    return static_cast<std::uint64_t>(data.st_size);
}

#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
using HydraulicEntity = hydraulic::HydraulicEntity;

QUuid parseUuid(const std::string& value) {
    requireInput(value.size() == 36, "UUID must be canonical lowercase without braces");
    const QString text = QString::fromStdString(value);
    const QUuid uuid(text);
    requireInput(!uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == text,
                 "invalid canonical UUID");
    return uuid;
}

QString uuidString(const QUuid& uuid) {
    return uuid.toString(QUuid::WithoutBraces);
}

QByteArray decodePayload(const QJsonObject& object, const QString& field,
                         qsizetype maxEncoded = 8 * 1024 * 1024) {
    requireInput(object.value(field).isString(), "missing base64 payload");
    const QByteArray encoded = object.value(field).toString().toLatin1();
    requireInput(encoded.size() <= maxEncoded, "base64 payload exceeds size limit");
    const QByteArray::FromBase64Result decoded = QByteArray::fromBase64Encoding(
        encoded, QByteArray::AbortOnBase64DecodingErrors);
    requireInput(static_cast<bool>(decoded), "invalid base64 payload");
    return decoded.decoded;
}

QString encodePayload(const QByteArray& bytes) {
    return QString::fromLatin1(bytes.toBase64());
}

template <std::size_t Index = 0>
HydraulicEntity decodeEntity(std::size_t requested, const QByteArray& payload) {
    if constexpr (Index < std::variant_size_v<HydraulicEntity>) {
        if (requested == Index) {
            using Type = std::variant_alternative_t<Index, HydraulicEntity>;
            QString error;
            std::optional<Type> entity = ModelCodec<Type>::decode(payload, &error);
            if (!entity) throw ProjectError(ProjectErrorKind::InvalidInput,
                                            "invalid canonical hydraulic entity payload: " + error.toStdString());
            return HydraulicEntity{std::move(*entity)};
        }
        return decodeEntity<Index + 1>(requested, payload);
    } else {
        throw ProjectError(ProjectErrorKind::InvalidInput, "invalid hydraulic entity type");
    }
}

QByteArray encodeEntity(const HydraulicEntity& entity) {
    return std::visit([]<typename Type>(const Type& item) {
        return ModelCodec<Type>::encode(item);
    }, entity);
}

QUuid entityUuid(const HydraulicEntity& entity) {
    return std::visit([]<typename Type>(const Type& item) {
        return item.uuid;
    }, entity);
}

hydraulic::HydraulicMergeSide mergeSide(const QJsonValue& value) {
    requireInput(value.isString() && (value.toString() == "target" || value.toString() == "source"),
                 "merge side must be target or source");
    return value.toString() == "target" ? hydraulic::HydraulicMergeSide::Target :
                                          hydraulic::HydraulicMergeSide::Source;
}

#endif

#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
// Pin a branch at one UUIDv7 revision for all hydraulic network reads. A
// historical revision must be an ancestor through either merge parent.
RevisionId revisionForRead(Repository& repository, const ProjectId& project,
                             const std::string& branch, const httplib::Request& request) {
    requireBranch(repository, branch);
    const RevisionId tip = repository.head(project, branch);
    const std::optional<std::string> requested = queryValue(request, "revision");
    if (!requested) return tip;
    const RevisionId selected = parseRevisionId(*requested);
    if (!repository.revisionReachableFrom(project, tip, selected))
        throw StorageError(StorageErrorKind::NotFound, "revision is not an ancestor of branch HEAD");
    return selected;
}

ProjectId requestProject(const httplib::Request& request) {
    const std::optional<ProjectId> project = ProjectId::parse(pathParam(request, "id"));
    requireInput(project.has_value(), "project ID must be a UUIDv7");
    return *project;
}

void requireModelIndexed(Repository& repository, const ProjectId& project,
                           const RevisionId& revisionId, const char* missing) {
    const std::optional<Bytes> bytes = repository.readRevision(project, revisionId);
    if (!bytes) throw StorageError(StorageErrorKind::NotFound, "revision UUID not found");
    const RevisionRecord snapshot = decodeRevisionRecord(*bytes);
    if (snapshot.modelIndexed) return;
    if (snapshot.root == Hash{}) throw ProjectError(ProjectErrorKind::NotFound, missing);
    throw ProjectError(ProjectErrorKind::InvalidInput, "revision is not model-indexed");
}

std::vector<hydraulic::HydraulicEntityChange> parseHydraulicChanges(const QJsonObject& object) {
    requireKeys(object, {"expectedHead", "changes", "author", "message"});
    requireInput(object.value("changes").isArray(), "missing changes array");
    const QJsonArray input = object.value("changes").toArray();
    requireInput(input.size() <= 128, "hydraulic edit batch exceeds 128 entities");
    std::vector<hydraulic::HydraulicEntityChange> changes;
    std::set<QString> unique;
    changes.reserve(static_cast<std::size_t>(input.size()));
    for (const QJsonValue& value : input) {
        requireInput(value.isObject(), "edit change must be an object");
        const QJsonObject change = value.toObject();
        requireKeys(change, {"uuid", "delete", "type", "payloadBase64"});
        const QUuid uuid = parseUuid(requiredString(change, "uuid"));
        requireInput(unique.insert(uuidString(uuid)).second, "duplicate entity UUID in edit batch");
        if (change.value("delete") == QJsonValue(true)) {
            requireInput(!change.contains("type") && !change.contains("payloadBase64"),
                         "delete must not include an entity payload");
            changes.push_back(hydraulic::HydraulicEntityChange{uuid, std::nullopt});
        } else {
            requireInput(!change.contains("delete"), "delete must be true or omitted");
            requireInput(change.value("type").isDouble(), "missing entity type");
            const int type = change.value("type").toInt(-1);
            requireInput(type >= 0 && type < 18 && change.value("type").toDouble() == type,
                         "invalid entity type");
            HydraulicEntity decoded = decodeEntity(static_cast<std::size_t>(type),
                                                   decodePayload(change, "payloadBase64"));
            requireInput(entityUuid(decoded) == uuid, "entity payload UUID differs from edit UUID");
            changes.push_back(hydraulic::HydraulicEntityChange{uuid, std::move(decoded)});
        }
    }
    return changes;
}
#endif

// Per-route JSON body ceilings, checked from Content-Length before the body
// is read. Bundle uploads are bounded by the global payload limit instead.
std::optional<std::size_t> jsonBodyLimit(const httplib::Request& request) {
    if (request.method != "POST") return std::nullopt;
    const std::string& route = request.matched_route;
    if (route == routeProjects || route == routeBranches ||
        route == routeRepositoryRole || route == routeWorkingCopies || route == routeWorkingCopyLease ||
        route == routeWorkingCopyInitializeNetwork ||
        route == routeWorkingCopyHistoryNavigate ||
        route == routeWorkingCopyRelease || route == routeWorkingCopyDiscard) return maxJsonSmallBytes;
    if (route == routeWorkingCopySave || route == routeWorkingCopyEventSave ||
        route == routeWorkingCopyImportNetwork) return maxJsonModelBytes;
    if (route == routeMerges ||
        route == routeHydraulicMerges) return maxJsonMergeBytes;
    if (route == routeNetworks || route == routeEdits || route == routeModelCommits) return maxJsonModelBytes;
    return std::nullopt;
}

#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
// The MODEL event journal is the undo/redo history of exactly the saved
// candidate, so both must agree. Applied in order, every edit's before-image
// must equal the entity's current state (earlier edit of this batch, else the
// draft candidate, else the pinned base), and the final state of every edited
// entity must be the batch's candidate record (deleted: a tombstone).
void verifyEditImages(ProjectService& service, const WorkingCopyInfo& draft,
                      const std::vector<aowis::model::ModelEditTransaction>& transactions,
                      const std::vector<ModelChange>& changes) {
    std::map<Key, std::optional<ModelRecord>> candidate;
    for (const ModelChange& change : draft.candidateChanges) candidate.emplace(change.key, change.record);
    const std::function<std::optional<Bytes>(const Key&)> stored =
        [&service, &draft, &candidate](const Key& key) -> std::optional<Bytes> {
        const std::map<Key, std::optional<ModelRecord>>::const_iterator overlay = candidate.find(key);
        if (overlay != candidate.end())
            return overlay->second ? std::optional<Bytes>(overlay->second->payload) : std::nullopt;
        if (!draft.baseRevision) return std::nullopt;
        const std::optional<ModelRecord> base =
            service.getModelAtRevision(draft.projectId, *draft.baseRevision, key);
        return base ? std::optional<Bytes>(base->payload) : std::nullopt;
    };
    std::map<Key, std::optional<Bytes>> state;
    for (const aowis::model::ModelEditTransaction& transaction : transactions) {
        for (const aowis::model::ModelEntityEdit& edit : transaction.edits) {
            // The native root record is a server-owned manifest, not the
            // MODEL metadata image. A separate materializer verifies its
            // before-image and creates the matching manifest change.
            if (edit.entityType == QString::fromLatin1(aowis::model::hydraulicRootEditType))
                continue;
            const Key key = hydraulic::keyFromUuid(edit.entityId);
            const std::map<Key, std::optional<Bytes>>::const_iterator known = state.find(key);
            const std::optional<Bytes> current = known != state.end() ? known->second : stored(key);
            if (edit.before) {
                requireInput(current && *current == hydraulic::bytesFromQByteArray(*edit.before),
                             "MODEL event before-image differs from the saved draft");
            } else {
                requireInput(!current, "MODEL event creates an entity that already exists");
            }
            state[key] = edit.after ? std::optional<Bytes>(hydraulic::bytesFromQByteArray(*edit.after))
                                    : std::nullopt;
        }
    }
    for (const std::pair<const Key, std::optional<Bytes>>& entry : state) {
        const std::vector<ModelChange>::const_iterator change =
            std::find_if(changes.begin(), changes.end(),
                         [&entry](const ModelChange& item) { return item.key == entry.first; });
        requireInput(change != changes.end(), "candidate record missing for an entity edited by MODEL events");
        if (entry.second) {
            requireInput(change->record && change->record->payload == *entry.second,
                         "candidate record differs from MODEL after-image");
        } else {
            requireInput(!change->record, "candidate keeps an entity that MODEL events removed");
        }
    }
    // A root-only event must never smuggle additional typed writes now that
    // mixed root/entity batches are supported. More generally, every client
    // record must be represented by a reversible MODEL entity edit.
    for (const ModelChange& change : changes)
        requireInput(state.contains(change.key),
                     "candidate contains a model change absent from MODEL events");
}
#endif

void registerBundleRoutes(httplib::Server& server, ProjectService& service, BundleLimits limits,
                          const std::shared_ptr<TransferStagingBudget>& stagingBudget) {
    // Only AOWREV21/22 bundles are exposed over HTTP. The catalogue project
    // identity is authoritative; no name-derived filenames are used.
    server.Get(routeRevisionBundle, [&service, limits, stagingBudget](
        const httplib::Request& request, httplib::Response& response) {
        try {
            const std::string id = pathParam(request, "id");
            const std::optional<ProjectId> project = ProjectId::parse(id);
            requireInput(project.has_value(), "project must be UUIDv7");
            const std::string branch = pathParam(request, "branch");
            requireInput(ProjectService::validBranchName(branch), "invalid branch name");
            for (const httplib::Params::value_type& item : request.params)
                requireInput(item.first == "base", "unknown revision-bundle query parameter");
            const std::optional<std::string> baseText = queryValue(request, "base");
            const std::optional<RevisionId> base = baseText ? RevisionId::parse(*baseText) : std::nullopt;
            requireInput(!baseText || base.has_value(), "base must be a revision UUIDv7");
            std::optional<TransferStagingBudget::Reservation> slot = stagingBudget->reserve(limits.maxBytes);
            if (!slot) {
                send(response, errorReply(503, "staging_busy",
                                          QStringLiteral("revision bundle staging capacity exhausted")));
                response.set_header("Retry-After", "1");
                return;
            }
            std::shared_ptr<TransferStagingBudget::Reservation> reservation =
                std::make_shared<TransferStagingBudget::Reservation>(std::move(*slot));
            std::shared_ptr<ScratchFile> exported = std::make_shared<ScratchFile>();
            service.withRepository(id, [&](Repository& repository) {
                *exported = repository.createScratchFile();
                if (base) repository.exportIncrementalRevisionBundle(*project, branch, *base,
                                                                         exported->fd(), limits);
                else repository.exportRevisionBundle(*project, branch, exported->fd(), limits);
            });
            const std::uint64_t size = descriptorSize(exported->fd());
            if (limits.maxBytes != 0 && size > limits.maxBytes) {
                send(response, errorReply(413, "transfer_too_large",
                                          QStringLiteral("revision bundle exceeds transfer limit")));
                return;
            }
            response.status = 200;
            response.set_content_provider(static_cast<std::size_t>(size), revisionBundleMime,
                [exported, reservation](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
                    (void)reservation;
                    std::array<char, 64 * 1024> buffer{};
                    const std::size_t chunk = std::min(length, buffer.size());
                    const ssize_t count = ::pread(exported->fd(), buffer.data(), chunk,
                                                  static_cast<off_t>(offset));
                    if (count <= 0) return false;
                    return sink.write(buffer.data(), static_cast<std::size_t>(count));
                });
        } catch (...) {
            send(response, failureReply(std::current_exception()));
        }
    });

    server.Post(routeRevisionBundle, [&service, limits, stagingBudget](
        const httplib::Request& request, httplib::Response& response,
        const httplib::ContentReader& reader) {
        bool bodyConsumed = false;
        try {
            const std::string id = pathParam(request, "id");
            const std::optional<ProjectId> project = ProjectId::parse(id);
            requireInput(project.has_value(), "project must be UUIDv7");
            const std::string branch = pathParam(request, "branch");
            requireInput(ProjectService::validBranchName(branch), "invalid branch name");
            requireInput(request.params.empty(), "revision-bundle upload does not accept query parameters");
            const std::optional<std::uint64_t> length = declaredLength(request);
            if (length && limits.maxBytes != 0 && *length > limits.maxBytes) {
                sendAndClose(response, errorReply(413, "transfer_too_large",
                                                  QStringLiteral("revision bundle exceeds transfer limit")));
                return;
            }
            ProjectService::Operation operation = service.reserveOperation(id);
            std::optional<TransferStagingBudget::Reservation> reservation =
                stagingBudget->reserve(limits.maxBytes);
            if (!reservation) {
                sendAndClose(response, errorReply(503, "staging_busy",
                                                  QStringLiteral("revision bundle staging capacity exhausted")));
                response.set_header("Retry-After", "1");
                return;
            }
            ScratchFile spool = service.createScratchFile(id);
            std::uint64_t received = 0;
            bool tooLarge = false;
            std::optional<int> writeError;
            const bool complete = reader([&](const char* data, std::size_t size) {
                if (limits.maxBytes != 0 && size > limits.maxBytes - std::min(received, limits.maxBytes)) {
                    tooLarge = true;
                    return false;
                }
                std::size_t done = 0;
                while (done < size) {
                    const ssize_t written = ::pwrite(spool.fd(), data + done, size - done,
                                                     static_cast<off_t>(received + done));
                    if (written < 0 && errno == EINTR) continue;
                    if (written <= 0) {
                        writeError = written == 0 ? EIO : errno;
                        return false;
                    }
                    done += static_cast<std::size_t>(written);
                }
                received += size;
                return true;
            });
            bodyConsumed = complete;
            if (tooLarge) {
                sendAndClose(response, errorReply(413, "transfer_too_large",
                                                  QStringLiteral("revision bundle exceeds transfer limit")));
                return;
            }
            if (writeError) {
                errno = *writeError;
                throwSystemError("spool revision bundle");
            }
            if (!complete) {
                response.set_header("Connection", "close");
                return;
            }
            requireInput(received != 0, "empty revision bundle");
            std::optional<RevisionBundleImport> imported;
            service.withRepository(operation, [&](Repository& repository) {
                imported = repository.importRevisionBundle(*project, spool.fd(), branch, limits);
            });
            const QString status = imported->status == BundleImportStatus::Created ?
                QStringLiteral("created") : imported->status == BundleImportStatus::FastForward ?
                QStringLiteral("fast_forward") : QStringLiteral("up_to_date");
            send(response, Reply{imported->status == BundleImportStatus::Created ? 201 : 200,
                                 QJsonObject{{"status", status},
                                             {"head", QString::fromStdString(imported->head.toString())},
                                             {"objectsImported", static_cast<qint64>(imported->objectsImported)}}});
        } catch (...) {
            if (bodyConsumed) send(response, failureReply(std::current_exception()));
            else sendAndClose(response, failureReply(std::current_exception()));
        }
    });


}

#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
// Both immutable-revision and private-draft summaries use the same entity
// inventory. Keep the counts derived from the hydrated canonical MODEL object,
// not from native membership records (which are implementation details).
QJsonObject hydraulicNetworkSummaryJson(const NetworkHydraulic& network) {
    const QJsonObject counts{
        {"junctions", network.nodes_junctions.size()},
        {"pipes", network.links_pipes.size()},
        {"reservoirs", network.nodes_reservoirs.size()},
        {"tanks", network.nodes_tanks.size()},
        {"pumps", network.links_pumps.size()},
        {"valves", network.links_valves.size()},
        {"demandPoints", network.demand_points.size()},
        {"controls", network.controls_simple.size()},
        {"rules", network.controls_rules.size()},
        {"mapLabels", network.map_labels.size()},
        {"pipeMaterials", network.pipe_materials.size()},
        {"timePatterns", network.patterns_time.size()},
        {"tankVolumeCurves", network.curves_tank_volume.size()},
        {"pumpHeadCurves", network.curves_pump_head.size()},
        {"pumpEfficiencyCurves", network.curves_pump_efficiency.size()},
        {"valveHeadlossCurves", network.curves_valve_headloss.size()},
        {"valveCharacteristicCurves", network.curves_valve_characteristic.size()},
        {"genericCurves", network.curves_generic.size()}
    };
    int total = 0;
    for (QJsonObject::const_iterator it = counts.constBegin();
         it != counts.constEnd(); ++it) total += it.value().toInt();
    return QJsonObject{{"id", uuidString(network.uuid)},
                       {"title", network.title_line_1},
                       {"counts", counts}, {"entityCount", total}};
}

void registerHydraulicRoutes(httplib::Server& server, ProjectService& service) {
    // Distinct domain-validated merge route; generic merges do not perform
    // hydraulic semantic validation.
    server.Post(routeHydraulicMerges, [&service](const httplib::Request& request,
                                                   httplib::Response& response) {
        guarded(response, [&]() {
            const ProjectId project = requestProject(request);
            const QJsonObject object = parseBody(request, maxJsonMergeBytes);
            requireKeys(object, {"target", "source", "expectedTargetHead", "expectedSourceHead",
                                 "fieldChoices", "objectChoices", "author", "message"});
            const std::string target = requiredString(object, "target");
            const std::string source = requiredString(object, "source");
            requireInput(target != source, "hydraulic merge requires different branches");
            const CommitMetadata metadata =
                parseCommitMetadata(object, "Merge " + source + " into " + target);
            hydraulic::HydraulicMergeSelections selections{
                parseRevisionId(requiredString(object, "expectedTargetHead")),
                parseRevisionId(requiredString(object, "expectedSourceHead")), {}, {}};
            requireInput(!object.contains("fieldChoices") || object.value("fieldChoices").isArray(),
                         "fieldChoices must be an array");
            requireInput(!object.contains("objectChoices") || object.value("objectChoices").isArray(),
                         "objectChoices must be an array");
            const QJsonArray fields = object.value("fieldChoices").toArray();
            const QJsonArray objects = object.value("objectChoices").toArray();
            requireInput(fields.size() <= 128 && objects.size() <= 128 &&
                         fields.size() + objects.size() <= 128,
                         "too many hydraulic resolution choices");
            for (const QJsonValue& value : fields) {
                requireInput(value.isObject(), "field choice must be an object");
                const QJsonObject choice = value.toObject();
                requireKeys(choice, {"entityUuid", "field", "side"});
                selections.fields.push_back(hydraulic::HydraulicFieldChoice{
                    parseUuid(requiredString(choice, "entityUuid")),
                    QString::fromStdString(requiredString(choice, "field")),
                    mergeSide(choice.value("side"))});
            }
            for (const QJsonValue& value : objects) {
                requireInput(value.isObject(), "object choice must be an object");
                const QJsonObject choice = value.toObject();
                requireKeys(choice, {"entityUuid", "side"});
                selections.objects.push_back(hydraulic::HydraulicObjectChoice{
                    parseUuid(requiredString(choice, "entityUuid")),
                    mergeSide(choice.value("side"))});
            }
            MergeResult result;
            service.withRepository(pathParam(request, "id"), [&](Repository& repository) {
                result = hydraulic::mergeHydraulicBranches(
                    repository, project, target, source, selections, metadata);
            });
            return Reply{result.status == MergeStatus::Conflicts ? 409 : 200, mergeJson(result)};
        });
    });
    // Revision-pinned, read-only collection summary for the Projects inspector.
    // Never infer editable HEAD contents from this response: private drafts may
    // contain additional uncommitted changes not present in this snapshot.
    server.Get("/dvmdb/v1/projects/:id/branches/:branch/networks/summary",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const ProjectId project = requestProject(request);
            QJsonObject result;
            service.withRepository(pathParam(request, "id"), [&](Repository& repository) {
                const std::string branch = pathParam(request, "branch");
                requireBranch(repository, branch);
                const std::optional<RevisionId> tip = repository.optionalHead(project, branch);
                if (!tip) {
                    if (queryValue(request, "revision"))
                        throw ProjectError(ProjectErrorKind::NotFound,
                                           "unborn branch has no committed revision");
                    result = QJsonObject{{"scope", "committed"},
                                         {"revision", QJsonValue(QJsonValue::Null)},
                                         {"networkCount", 0}, {"networks", QJsonArray{}}};
                    return;
                }
                const RevisionId revision = revisionForRead(repository, project, branch, request);
                requireModelIndexed(repository, project, revision, "hydraulic network not found");
                QJsonArray networks;
                // Only canonical network roots (kind 100), never membership
                // chunks or standalone hydraulic entities, contribute entries.
                const std::vector<std::pair<Key, Bytes>> records =
                    repository.scanAtRevision(project, revision);
                for (const std::pair<Key, Bytes>& item : records) {
                    const ModelRecord record = decodeModelRecord(item.second);
                    if (record.kind != hydraulic::hydraulicNetworkKind) continue;
                    const QUuid id = hydraulic::uuidFromKey(item.first);
                    const std::optional<NetworkHydraulic> network =
                        hydraulic::loadHydraulicNetworkAtRevision(repository, project, revision, id);
                    if (!network)
                        throw ProjectError(ProjectErrorKind::Unavailable,
                                           "hydraulic network root could not be loaded");
                    networks.append(hydraulicNetworkSummaryJson(*network));
                }
                result = QJsonObject{{"scope", "committed"},
                                     {"revision", revisionIdString(revision)},
                                     {"networkCount", static_cast<int>(networks.size())},
                                     {"networks", networks}};
            });
            return Reply{200, result};
        });
    });
    // Canonical hydraulic routes use UUIDv7 revision identities and preserve
    // the existing hydraulic payload codec and semantic validation.
    server.Get("/dvmdb/v1/projects/:id/branches/:branch/networks/:network",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const ProjectId project = requestProject(request);
            const QUuid networkUuid = parseUuid(pathParam(request, "network"));
            QJsonObject result;
            service.withRepository(pathParam(request, "id"), [&](Repository& repository) {
                const RevisionId revision = revisionForRead(repository, project,
                                                               pathParam(request, "branch"), request);
                requireModelIndexed(repository, project, revision, "hydraulic network not found");
                const std::optional<NetworkHydraulic> network =
                    hydraulic::loadHydraulicNetworkAtRevision(repository, project, revision, networkUuid);
                if (!network) throw ProjectError(ProjectErrorKind::NotFound, "hydraulic network not found");
                result = QJsonObject{{"revision", revisionIdString(revision)},
                                     {"uuid", uuidString(networkUuid)},
                                     {"payloadBase64", encodePayload(ModelCodec<NetworkHydraulic>::encode(*network))}};
            });
            return Reply{200, result};
        });
    });
    server.Post(routeNetworks, [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const ProjectId project = requestProject(request);
            const QJsonObject object = parseBody(request, maxJsonModelBytes);
            requireKeys(object, {"expectedHead", "payloadBase64", "author", "message"});
            const RevisionId expected = parseRevisionId(requiredString(object, "expectedHead"));
            const CommitMetadata metadata = parseCommitMetadata(object);
            QString error;
            const std::optional<NetworkHydraulic> network = ModelCodec<NetworkHydraulic>::decode(
                decodePayload(object, "payloadBase64"), &error);
            if (!network) throw ProjectError(ProjectErrorKind::InvalidInput,
                "invalid canonical hydraulic network payload: " + error.toStdString());
            requireInput(!network->uuid.isNull(), "hydraulic network UUID is null");
            RevisionId committed = expected;
            service.withRepository(pathParam(request, "id"), [&](Repository& repository) {
                const std::string branch = pathParam(request, "branch");
                requireBranch(repository, branch);
                committed = hydraulic::saveHydraulicNetwork(repository, project, branch, *network,
                                                            expected, metadata);
            });
            return Reply{201, QJsonObject{{"id", revisionIdString(committed)}}};
        });
    });
    server.Get("/dvmdb/v1/projects/:id/branches/:branch/networks/:network/entities",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const ProjectId project = requestProject(request);
            const QUuid networkUuid = parseUuid(pathParam(request, "network"));
            const std::optional<std::string> typeText = queryValue(request, "type");
            bool validType = false;
            const unsigned type = typeText ? QString::fromStdString(*typeText).toUInt(&validType) : 0U;
            requireInput(validType && type < 18, "entity type must be integer in 0..17");
            QJsonObject result;
            service.withRepository(pathParam(request, "id"), [&](Repository& repository) {
                const RevisionId revision = revisionForRead(repository, project,
                                                               pathParam(request, "branch"), request);
                requireModelIndexed(repository, project, revision, "hydraulic network not found");
                const std::optional<QList<QUuid>> ids = hydraulic::listHydraulicEntityIdsAtRevisionLimited(
                    repository, project, revision, networkUuid,
                    static_cast<hydraulic::HydraulicEntityType>(type), maxHttpEntityIds);
                if (!ids) throw ProjectError(ProjectErrorKind::NotFound, "hydraulic network not found");
                QJsonArray entries;
                for (const QUuid& uuid : *ids) entries.append(uuidString(uuid));
                result = QJsonObject{{"revision", revisionIdString(revision)},
                                     {"type", static_cast<int>(type)}, {"uuids", entries}};
            });
            return Reply{200, result};
        });
    });
    server.Get("/dvmdb/v1/projects/:id/branches/:branch/networks/:network/entities/:entity",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const ProjectId project = requestProject(request);
            const QUuid networkUuid = parseUuid(pathParam(request, "network"));
            const QUuid entity = parseUuid(pathParam(request, "entity"));
            QJsonObject result;
            service.withRepository(pathParam(request, "id"), [&](Repository& repository) {
                const RevisionId revision = revisionForRead(repository, project,
                                                               pathParam(request, "branch"), request);
                requireModelIndexed(repository, project, revision, "hydraulic network not found");
                const std::optional<HydraulicEntity> item = hydraulic::loadHydraulicEntityAtRevision(
                    repository, project, revision, networkUuid, entity);
                if (!item) throw ProjectError(ProjectErrorKind::NotFound, "hydraulic entity not found");
                result = QJsonObject{{"revision", revisionIdString(revision)},
                                     {"uuid", uuidString(entity)}, {"type", static_cast<int>(item->index())},
                                     {"payloadBase64", encodePayload(encodeEntity(*item))}};
            });
            return Reply{200, result};
        });
    });
    server.Post(routeEdits, [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const ProjectId project = requestProject(request);
            const QUuid networkUuid = parseUuid(pathParam(request, "network"));
            const QJsonObject object = parseBody(request, maxJsonModelBytes);
            const RevisionId expected = parseRevisionId(requiredString(object, "expectedHead"));
            const std::vector<hydraulic::HydraulicEntityChange> changes = parseHydraulicChanges(object);
            const CommitMetadata metadata = parseCommitMetadata(object);
            RevisionId committed = expected;
            service.withRepository(pathParam(request, "id"), [&](Repository& repository) {
                const std::string branch = pathParam(request, "branch");
                requireBranch(repository, branch);
                committed = hydraulic::editHydraulicNetwork(repository, project, branch, networkUuid,
                                                               changes, expected, metadata);
            });
            return Reply{200, QJsonObject{{"id", revisionIdString(committed)}}};
        });
    });


}
#endif

} // namespace

// Display metadata is derived from retained MODEL frames, never a second
// mutable source of truth. Native-only callers may persist opaque test frames:
// those remain identifiable but do not receive invented operation names.
QJsonObject historyActionJson(const WorkingCopyEvent& event, bool includeFrame) {
    QJsonObject result{
        {"id", QString::fromStdString(event.id)},
        {"operation", QJsonValue(QJsonValue::Null)},
        {"description", QJsonValue(QJsonValue::Null)},
        {"label", QStringLiteral("Action ") + QString::fromStdString(event.id.substr(0, 8))},
        {"editCount", QJsonValue(QJsonValue::Null)}};
    const QByteArray frame(reinterpret_cast<const char*>(event.encoded.data()),
                           static_cast<qsizetype>(event.encoded.size()));
    if (includeFrame)
        result.insert("frameBase64", QString::fromLatin1(frame.toBase64()));
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
    QString decodeError;
    const std::optional<aowis::model::ModelEditTransaction> decoded =
        aowis::model::ModelEditTransactionCodec::decode(frame, &decodeError);
    if (decoded &&
        decoded->transactionId.toString(QUuid::WithoutBraces) ==
            QString::fromStdString(event.id)) {
        result.insert("operation", decoded->operation);
        if (!decoded->description.isEmpty())
            result.insert("description", decoded->description);
        result.insert("label", decoded->description.isEmpty()
            ? decoded->operation : decoded->description);
        result.insert("editCount", static_cast<int>(decoded->edits.size()));
    }
#endif
    return result;
}

void registerRoutes(httplib::Server& server, ProjectService& service,
                    BundleLimits transferLimits,
                    std::shared_ptr<TransferStagingBudget> stagingBudget) {
    // Standalone library integrations have a bounded default even when they
    // use the three-argument API. The production executable configures this.
    if (!stagingBudget) {
        const std::uint64_t maximumTransfer = transferLimits.maxBytes == 0
            ? 32ULL * 1024ULL * 1024ULL : transferLimits.maxBytes;
        stagingBudget = std::make_shared<TransferStagingBudget>(maximumTransfer * 4ULL);
    }
    if (transferLimits.maxBytes == 0 || stagingBudget->maximumBytes() < transferLimits.maxBytes)
        throw std::invalid_argument("HTTP transfer limit must be finite and fit the staging budget");
    // Global ceiling: no request body larger than the bundle transfer limit
    // (or the largest JSON body) is ever read. Exceeding it yields 413.
    const std::size_t payloadLimit = static_cast<std::size_t>(std::max<std::uint64_t>(
        transferLimits.maxBytes == 0 ? std::numeric_limits<std::uint32_t>::max()
                                     : transferLimits.maxBytes, maxJsonModelBytes));
    server.set_payload_max_length(payloadLimit);
    server.set_read_timeout(std::chrono::seconds(30));
    server.set_write_timeout(std::chrono::seconds(60));
    server.set_keep_alive_timeout(std::chrono::seconds(10));
    // Per-route JSON limits, decided from Content-Length before the body is
    // read. JSON bodies must declare their length (no chunked encoding).
    server.set_pre_request_handler([](const httplib::Request& request, httplib::Response& response) {
        const std::optional<std::size_t> limit = jsonBodyLimit(request);
        if (!limit) return httplib::Server::HandlerResponse::Unhandled;
        if (!request.has_header("Content-Length")) {
            sendAndClose(response, errorReply(411, "length_required",
                                              QStringLiteral("JSON requests must declare Content-Length")));
            return httplib::Server::HandlerResponse::Handled;
        }
        if (*declaredLength(request) > *limit) {
            sendAndClose(response, errorReply(413, "limit_exceeded",
                                              QStringLiteral("request body exceeds the endpoint size limit")));
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    // Anything a handler did not catch (should not happen) is a generic 500.
    server.set_exception_handler([](const httplib::Request&, httplib::Response& response,
                                    std::exception_ptr failure) {
        send(response, failureReply(failure));
    });
    // httplib's own rejections (413/414/431, malformed requests) get JSON bodies.
    server.set_error_handler([](const httplib::Request&, httplib::Response& response) {
        if (!response.body.empty()) return httplib::Server::HandlerResponse::Unhandled;
        const int status = response.status;
        const QString code = status == 404 ? QStringLiteral("not_found")
            : status == 413 ? QStringLiteral("limit_exceeded")
            : status == 414 ? QStringLiteral("uri_too_long")
            : status == 431 ? QStringLiteral("header_too_large")
            : status >= 500 ? QStringLiteral("internal_error") : QStringLiteral("invalid_request");
        const QString message = status == 404 ? QStringLiteral("no such route")
            : QString::fromStdString(httplib::status_message(status));
        send(response, errorReply(status, code, message));
        return httplib::Server::HandlerResponse::Handled;
    });

    server.Get("/dvmdb/v1/health", [](const httplib::Request&, httplib::Response& response) {
        send(response, Reply{200, QJsonObject{{"status", QStringLiteral("ok")}}});
    });
    server.Get("/dvmdb/v1/repository", [&service](const httplib::Request&, httplib::Response& response) {
        guarded(response, [&]() {
            return Reply{200, QJsonObject{{"repositoryId", QString::fromStdString(service.repositoryId())},
                {"modelCodecFingerprint", QString::fromLatin1(AOWIS_MODEL_CODEC_FINGERPRINT)}}};
        });
    });
    // Read-only VC-03 repository-qualified tracking metadata. The registry
    // is configuration, not proof of endpoint identity or authorization.
    server.Get("/dvmdb/v1/repository/tracking", [&service](const httplib::Request&,
                                                            httplib::Response& response) {
        guarded(response, [&]() {
            const RepositoryRole role = service.repositoryRole();
            const QString roleName = role == RepositoryRole::Private ? QStringLiteral("private") :
                role == RepositoryRole::Shared ? QStringLiteral("shared") :
                QStringLiteral("unconfigured");
            QJsonArray remotes;
            for (const RemoteRepository& remote : service.remoteRepositories()) {
                remotes.append(QJsonObject{
                    {"repositoryId", QString::fromStdString(remote.repositoryId)},
                    {"endpoint", QString::fromStdString(remote.endpoint)}});
            }
            QJsonArray tracked;
            for (const RemoteTrackingRef& ref : service.remoteTrackingRefs()) {
                tracked.append(QJsonObject{
                    {"remoteRepositoryId", QString::fromStdString(ref.remoteRepositoryId)},
                    {"projectId", QString::fromStdString(ref.projectId)},
                    {"branchId", QString::fromStdString(ref.branchId)},
                    {"observedHeadRevisionId", ref.observedHead
                        ? QJsonValue(QString::fromStdString(ref.observedHead->toString()))
                        : QJsonValue(QJsonValue::Null)}});
            }
            return Reply{200, QJsonObject{
                {"repositoryId", QString::fromStdString(service.repositoryId())},
                {"modelCodecFingerprint", QString::fromLatin1(AOWIS_MODEL_CODEC_FINGERPRINT)},
                {"role", roleName}, {"remotes", remotes}, {"tracking", tracked}}};
        });
    });
    // One-time, idempotent repository-role configuration for an explicitly
    // selected personal/shared database. The caller must echo the identity it
    // just read; this prevents re-targeting an unrelated repository by mistake.
    // Role changes from private to shared (or vice versa) remain forbidden.
    server.Post(routeRepositoryRole, [&service](const httplib::Request& request,
                                                        httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject body = parseBody(request, maxJsonSmallBytes);
            requireKeys(body, {"expectedRepositoryId", "role"});
            const std::string expectedId = requiredString(body, QStringLiteral("expectedRepositoryId"));
            requireInput(UuidV7::parse(expectedId).has_value(),
                         "expectedRepositoryId must be a UUIDv7");
            const std::string name = requiredString(body, QStringLiteral("role"));
            requireInput(name == "private" || name == "shared",
                         "role must be private or shared");
            if (expectedId != service.repositoryId())
                throw ProjectError(ProjectErrorKind::Conflict, "repository identity changed");
            const RepositoryRole requested = name == "private"
                ? RepositoryRole::Private : RepositoryRole::Shared;
            service.configureRepositoryRole(requested);
            return Reply{200, QJsonObject{
                {"repositoryId", QString::fromStdString(service.repositoryId())},
                {"role", QString::fromStdString(name)}}};
        });
    });
    server.Get("/dvmdb/v1/ready", [&service](const httplib::Request&, httplib::Response& response) {
        guarded(response, [&]() {
            (void)service.listProjects();
            return Reply{200, QJsonObject{{"status", QStringLiteral("ready")}}};
        });
    });
    // VC-04: private, persistent working copies. These are draft operations,
    // not revision-graph commits. Repository role is checked in the native API.
    // Paged in draft-UUID order: ?limit=1..1000 (default 100), ?after=<draft
    // UUIDv7> from the previous page's nextCursor, optional ?projectId=.
    server.Get(routeWorkingCopies, [&service](const httplib::Request& request,
                                              httplib::Response& response) {
        guarded(response, [&]() {
            for (const httplib::Params::value_type& item : request.params)
                requireInput(item.first == "limit" || item.first == "after" ||
                             item.first == "projectId", "unknown working-copy list query parameter");
            std::size_t limit = 100;
            if (const std::optional<std::string> value = queryValue(request, "limit")) {
                bool ok = false;
                const unsigned number = QString::fromStdString(*value).toUInt(&ok);
                requireInput(ok && number > 0 && number <= 1000, "invalid working-copy page limit");
                limit = number;
            }
            const std::optional<std::string> after = queryValue(request, "after");
            const std::optional<std::string> projectId = queryValue(request, "projectId");
            const WorkingCopyPage page = service.listWorkingCopies(
                projectId ? std::optional<std::string_view>(*projectId) : std::nullopt, limit,
                after ? std::optional<std::string_view>(*after) : std::nullopt);
            QJsonArray items;
            for (const WorkingCopyInfo& draft : page.workingCopies)
                items.append(workingCopyJson(draft, false));
            return Reply{200, QJsonObject{{"workingCopies", items},
                {"nextCursor", page.next ? QJsonValue(QString::fromStdString(*page.next))
                                         : QJsonValue(QJsonValue::Null)}}};
        });
    });
    server.Post(routeWorkingCopies, [&service](const httplib::Request& request,
                                               httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject body = parseBody(request, maxJsonSmallBytes);
            requireKeys(body, {"projectId", "branchId", "baseRevision", "sessionId", "id"});
            requireInput(body.contains("baseRevision"), "missing baseRevision (use null for unborn)");
            const QJsonValue base = body.value("baseRevision");
            requireInput(base.isNull() || base.isString(), "baseRevision must be UUIDv7 or null");
            const std::optional<RevisionId> revision = base.isString()
                ? std::optional<RevisionId>(parseRevisionId(base.toString().toStdString()))
                : std::nullopt;
            // Branch by UUIDv7 (DG-01). A caller-chosen draft `id` makes the
            // creation idempotent: a resent request returns the same draft.
            const std::optional<std::string> draftId = body.contains("id")
                ? std::optional<std::string>(requiredString(body, "id")) : std::nullopt;
            return Reply{201, workingCopyJson(service.createWorkingCopyForBranch(
                requiredString(body, "projectId"), requiredString(body, "branchId"), revision,
                requiredString(body, "sessionId"),
                draftId ? std::optional<std::string_view>(*draftId) : std::nullopt))};
        });
    });
    server.Get("/dvmdb/v1/working-copies/:draft", [&service](const httplib::Request& request,
                                                              httplib::Response& response) {
        guarded(response, [&]() {
            return Reply{200, workingCopyJson(service.workingCopy(pathParam(request, "draft")))};
        });
    });
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
    // Resolve neighboring MODEL boundaries on the server, not in the GUI.
    // An archived revision's position zero equals the preceding Commit's
    // tip; crossing that marker skips the duplicate snapshot. Results are
    // coordinates for the existing fenced navigation endpoint, not a claim
    // that MODEL verification will necessarily accept an unknown archive.
    // A retained journal at position zero can safely hand off into the
    // committed history; the original frames remain available for Redo.
    server.Get("/dvmdb/v1/working-copies/:draft/history/cursor",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const WorkingCopyInfo draft = service.workingCopy(pathParam(request, "draft"), false);
            const std::uint64_t position = draft.draftActionPosition.value_or(draft.journalCount);
            QJsonValue previous(QJsonValue::Null);
            QJsonValue next(QJsonValue::Null);
            const std::function<QJsonValue(const QString&, const QString&, std::size_t)> target =
                [](const QString& kind, const QString& revision, std::size_t positionValue)
                    -> QJsonValue {
                    return QJsonObject{{"selectionKind", kind},
                        {"revisionId", revision.isEmpty() ? QJsonValue(QJsonValue::Null)
                                                          : QJsonValue(revision)},
                        {"actionPosition", QString::number(static_cast<qulonglong>(positionValue))}};
                };
            const bool archived = draft.historyCheckpointRevisionId &&
                                  draft.historyActionPosition;
            if (draft.journalCount > 0 && !archived &&
                !draft.historyCheckpointRevisionId) {
                if (position > 0)
                    previous = target(QStringLiteral("draft"), QString(),
                                      static_cast<std::size_t>(position - 1));
                if (position < draft.journalCount)
                    next = target(QStringLiteral("draft"), QString(),
                                  static_cast<std::size_t>(position + 1));
                // An edit after an archived Undo branches the active timeline.
                // Reaching position zero returns to the chosen immutable
                // source, never to the abandoned newer Commit actions.
                if (position == 0 && draft.historyForkRevisionId &&
                    draft.historyForkActionPosition) {
                    if (*draft.historyForkActionPosition > 0) {
                        previous = target(QStringLiteral("archived"),
                            QString::fromStdString(*draft.historyForkRevisionId),
                            static_cast<std::size_t>(*draft.historyForkActionPosition - 1));
                    } else {
                        // A fork at action zero starts at the preceding
                        // Commit's final state. Do not mistakenly revive the
                        // abandoned suffix of the fork-source revision.
                        const std::optional<RevisionGraphHead> branch =
                            service.branchById(draft.projectId, draft.branchId);
                        if (branch && branch->revision == draft.baseRevision) {
                            std::optional<RevisionId> after;
                            bool passedSource = false;
                            for (std::size_t pageIndex = 0; pageIndex < 256 &&
                                 previous.isNull(); ++pageIndex) {
                                const BranchActionHistoryPage page =
                                    service.branchActionHistory(draft.projectId,
                                        branch->name, 10, after, draft.id);
                                for (const BranchActionHistoryEntry& entry : page.commits) {
                                    if (!passedSource) {
                                        passedSource = entry.revision.id.toString() ==
                                            *draft.historyForkRevisionId;
                                    } else if (entry.commitOperationId &&
                                               !entry.actionIds.empty()) {
                                        previous = target(QStringLiteral("archived"),
                                            QString::fromStdString(entry.revision.id.toString()),
                                            entry.actionIds.size() - 1);
                                        break;
                                    }
                                }
                                if (!page.next) break;
                                after = page.next;
                            }
                        }
                    }
                }
                if (position == 0 && draft.baseRevision && !draft.dirty &&
                    !draft.historyForkRevisionId) {
                    const std::optional<RevisionGraphHead> branch =
                        service.branchById(draft.projectId, draft.branchId);
                    if (branch && branch->revision == draft.baseRevision) {
                        const BranchActionHistoryPage page = service.branchActionHistory(
                            draft.projectId, branch->name, 1);
                        if (!page.commits.empty() &&
                            page.commits.front().revision.id == *draft.baseRevision &&
                            page.commits.front().commitOperationId &&
                            !page.commits.front().actionIds.empty())
                            previous = target(QStringLiteral("archived"),
                                QString::fromStdString(draft.baseRevision->toString()),
                                page.commits.front().actionIds.size() - 1);
                    }
                }
            } else if ((draft.journalCount == 0 ||
                        (draft.draftActionPosition == 0 && archived)) &&
                       draft.baseRevision &&
                       (!draft.dirty || draft.historyCheckpointRevisionId) &&
                       (archived || !draft.historyCheckpointRevisionId)) {
                const std::optional<RevisionGraphHead> branch =
                    service.branchById(draft.projectId, draft.branchId);
                if (branch && branch->revision == draft.baseRevision) {
                    // Paged first-parent order, newest to oldest. Bound the
                    // scan so a malformed or enormous history cannot turn a
                    // cursor read into an unbounded request.
                    const std::string wanted = archived
                        ? *draft.historyCheckpointRevisionId : draft.baseRevision->toString();
                    std::optional<RevisionId> after;
                    std::optional<BranchActionHistoryEntry> newer;
                    std::optional<BranchActionHistoryEntry> selected;
                    std::optional<BranchActionHistoryEntry> older;
                    for (std::size_t pageIndex = 0; pageIndex < 256 && !older; ++pageIndex) {
                        const BranchActionHistoryPage page = service.branchActionHistory(
                            draft.projectId, branch->name, 10, after);
                        for (const BranchActionHistoryEntry& entry : page.commits) {
                            if (selected) {
                                older = entry;
                                break;
                            }
                            if (entry.revision.id.toString() == wanted) {
                                selected = entry;
                            } else {
                                newer = entry;
                            }
                        }
                        if (!page.next) break;
                        after = page.next;
                    }
                    if (selected && selected->commitOperationId &&
                        (!selected->actionIds.empty() ||
                         (archived && *draft.historyActionPosition == 0))) {
                        const std::size_t count = selected->actionIds.size();
                        const std::uint64_t archivedPosition = archived
                            ? *draft.historyActionPosition : count;
                        if (archivedPosition <= count) {
                            if (archivedPosition > 0) {
                                previous = target(QStringLiteral("archived"),
                                    QString::fromStdString(wanted),
                                    static_cast<std::size_t>(archivedPosition - 1));
                            } else if (older && older->commitOperationId &&
                                       !older->actionIds.empty()) {
                                previous = target(QStringLiteral("archived"),
                                    QString::fromStdString(older->revision.id.toString()),
                                    older->actionIds.size() - 1);
                            }
                            if (archived) {
                                if (draft.journalCount > 0 && draft.historyForkRevisionId &&
                                    draft.historyForkActionPosition &&
                                    wanted == *draft.historyForkRevisionId &&
                                    archivedPosition == *draft.historyForkActionPosition) {
                                    // This is the parent of our new editing
                                    // lane. The superseded Commit suffix must
                                    // not become Redo after the fork.
                                    next = target(QStringLiteral("draft"), QString(), 1);
                                } else if (draft.journalCount > 0 && !newer &&
                                    wanted == draft.baseRevision->toString() &&
                                    archivedPosition + 1 == count) {
                                    // Completing the last archived action
                                    // returns to active position zero (HEAD).
                                    next = target(QStringLiteral("draft"), QString(), 0);
                                } else if (archivedPosition < count) {
                                    next = target(QStringLiteral("archived"),
                                        QString::fromStdString(wanted),
                                        static_cast<std::size_t>(archivedPosition + 1));
                                } else if (newer && newer->commitOperationId &&
                                           !newer->actionIds.empty()) {
                                    next = target(QStringLiteral("archived"),
                                        QString::fromStdString(newer->revision.id.toString()), 1);
                                } else if (draft.journalCount > 0 && !newer &&
                                           wanted == draft.baseRevision->toString()) {
                                    next = target(QStringLiteral("draft"), QString(), 1);
                                }
                            }
                        }
                    }
                }
            }
            // An unborn draft has no immutable parent to supply membership
            // ordering. Offer a target only when the same strict MODEL replay
            // planner can actually verify its destination. A failed proof
            // must disable the button rather than offer a misleading Undo.
            if (!draft.baseRevision && !draft.historyCheckpointRevisionId) {
                const std::function<bool(const QJsonValue&)> eligible =
                    [&service, &draft](const QJsonValue& coordinate) -> bool {
                        if (!coordinate.isObject()) return false;
                        const QJsonObject object = coordinate.toObject();
                        bool valid = false;
                        const std::size_t wanted = static_cast<std::size_t>(
                            object.value("actionPosition").toString().toULongLong(&valid));
                        if (!valid) return false;
                        QString reason;
                        return hydraulic::planVerifiedHydraulicDraftActionNavigation(
                            service, draft.id, wanted, draft.generation, &reason).has_value();
                    };
                if (!previous.isNull() && !eligible(previous))
                    previous = QJsonValue(QJsonValue::Null);
                if (!next.isNull() && !eligible(next))
                    next = QJsonValue(QJsonValue::Null);
            }
            // An intervening Save/Commit invalidates even a freshly read
            // target: the POST must still supply and match this generation.
            const WorkingCopyInfo current = service.workingCopy(draft.id, false);
            const std::optional<RevisionGraphHead> currentBranch =
                service.branchById(draft.projectId, draft.branchId);
            if (!currentBranch || currentBranch->revision != draft.baseRevision ||
                current.generation != draft.generation ||
                current.baseRevision != draft.baseRevision ||
                current.historyCheckpointRevisionId != draft.historyCheckpointRevisionId ||
                current.historyActionPosition != draft.historyActionPosition ||
                current.draftActionPosition != draft.draftActionPosition ||
                current.journalCount != draft.journalCount)
                throw ProjectError(ProjectErrorKind::Conflict,
                                   "history cursor changed while resolving adjacent actions");
            return Reply{200, QJsonObject{
                {"draftId", QString::fromStdString(draft.id)},
                {"generation", QString::number(static_cast<qulonglong>(draft.generation))},
                {"baseRevision", optionalRevisionJson(draft.baseRevision)},
                {"journalCount", QString::number(static_cast<qulonglong>(draft.journalCount))},
                {"actionPosition", QString::number(static_cast<qulonglong>(position))},
                {"hasPreviousDraftAction", !archived && position > 0},
                {"hasNextDraftAction", !archived && position < draft.journalCount},
                {"canBranchFromHistory", archived &&
                    draft.historyArchiveCommitOperationId.has_value()},
                {"forkRevisionId", draft.historyForkRevisionId
                    ? QJsonValue(QString::fromStdString(*draft.historyForkRevisionId))
                    : QJsonValue(QJsonValue::Null)},
                {"forkActionPosition", draft.historyForkActionPosition
                    ? QJsonValue(QString::number(static_cast<qulonglong>(*draft.historyForkActionPosition)))
                    : QJsonValue(QJsonValue::Null)},
                {"previousAction", previous},
                {"nextAction", next},
                {"selectionKind", draft.historyActionPosition
                    ? QStringLiteral("archived") : draft.historyCheckpointRevisionId
                        ? QStringLiteral("checkpoint") : QStringLiteral("draft")},
                {"archivedRevisionId", draft.historyCheckpointRevisionId
                    ? QJsonValue(QString::fromStdString(*draft.historyCheckpointRevisionId))
                    : QJsonValue(QJsonValue::Null)},
                {"archivedActionPosition", draft.historyActionPosition
                    ? QJsonValue(QString::number(static_cast<qulonglong>(*draft.historyActionPosition)))
                    : QJsonValue(QJsonValue::Null)},
                {"archiveCommitOperationId", draft.historyArchiveCommitOperationId
                    ? QJsonValue(QString::fromStdString(*draft.historyArchiveCommitOperationId))
                    : QJsonValue(QJsonValue::Null)}}};
        });
    });
    // Fenced action-boundary selection. With revisionId this selects an
    // archived local Commit's global MODEL action boundary; without it the
    // current single-network draft journal is selected. The client never
    // supplies a native candidate. A zero-position active journal can enter
    // verified archived history without discarding its original redo frames.
    // Explicit coordinates permit exact retries after a lost response.
    server.Post(routeWorkingCopyHistoryNavigate,
                [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            if (const std::optional<Reply> pin = repositoryPinFailure(request, service))
                return *pin;
            const QJsonObject body = parseBody(request, maxJsonSmallBytes);
            requireKeys(body, {"operationId", "sessionId", "fencingToken",
                               "expectedGeneration", "expectedHead", "actionPosition",
                               "revisionId"});
            const std::string operationId = requiredString(body, "operationId");
            requireInput(UuidV7::parse(operationId).has_value(),
                         "history navigation operationId must be UUIDv7");
            requireInput(body.contains("expectedHead") &&
                         (body.value("expectedHead").isNull() ||
                          body.value("expectedHead").isString()),
                         "expectedHead must be a revision UUIDv7 or null");
            const std::optional<RevisionId> expectedHead = body.value("expectedHead").isNull()
                ? std::nullopt : std::optional<RevisionId>(parseRevisionId(
                    requiredString(body, "expectedHead")));
            const std::uint64_t position = strictUint64(body, "actionPosition");
            requireInput(position <= 4096, "draft actionPosition exceeds journal limit");
            const bool archived = body.contains("revisionId");
            requireInput(!archived || body.value("revisionId").isString(),
                         "revisionId must be a revision UUIDv7 when provided");
            requireInput(!archived || expectedHead.has_value(),
                         "archived action navigation requires a committed expectedHead");
            const std::optional<RevisionId> revision = archived
                ? std::optional<RevisionId>(parseRevisionId(requiredString(body, "revisionId")))
                : std::nullopt;
            const std::string draftId = pathParam(request, "draft");
            const std::string sessionId = requiredString(body, "sessionId");
            const std::string fencingToken = requiredString(body, "fencingToken");
            const std::uint64_t generation = strictUint64(body, "expectedGeneration");
            QString reason;
            const std::optional<WorkingCopyInfo> result = archived
                ? hydraulic::navigateVerifiedHydraulicAction(service, operationId,
                      draftId, sessionId, fencingToken, *expectedHead, *revision,
                      static_cast<std::size_t>(position), generation, &reason)
                : hydraulic::navigateVerifiedHydraulicDraftAction(service, operationId,
                      draftId, sessionId, fencingToken, expectedHead,
                      static_cast<std::size_t>(position), generation, &reason);
            if (!result)
                return errorReply(409, QStringLiteral("history_not_navigable"),
                    QStringLiteral("Action navigation is stale, unsupported or failed MODEL verification"));
            return Reply{200, workingCopyJson(*result, false)};
        });
    });
    // Summarize the SAVED candidate of one specific private draft generation.
    // Unlike the committed branch-tip summary, this includes unpublished edits
    // and also works for unborn branches. This is read-only and needs no lease.
    server.Get("/dvmdb/v1/working-copies/:draft/networks/summary",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const WorkingCopyInfo draft = service.workingCopy(pathParam(request, "draft"), false);
            std::map<Key, std::optional<ModelRecord>> overlay;
            for (const ModelChange& change : draft.candidateChanges)
                requireInput(overlay.emplace(change.key, change.record).second,
                             "duplicate draft candidate model record");
            std::set<Key> roots;
            if (draft.baseRevision) {
                const std::optional<ProjectId> project = ProjectId::parse(draft.projectId);
                requireInput(project.has_value(), "invalid private draft project UUIDv7");
                service.withRepository(draft.projectId, [&](Repository& repository) {
                    const std::vector<std::pair<Key, Bytes>> records =
                        repository.scanAtRevision(*project, *draft.baseRevision);
                    for (const std::pair<Key, Bytes>& item : records)
                        if (decodeModelRecord(item.second).kind == hydraulic::hydraulicNetworkKind)
                            roots.insert(item.first);
                });
            }
            for (const std::pair<const Key, std::optional<ModelRecord>>& item : overlay) {
                if (item.second && item.second->kind == hydraulic::hydraulicNetworkKind)
                    roots.insert(item.first);
                else
                    roots.erase(item.first);
            }
            const std::function<std::optional<ModelRecord>(const Key&)> reader =
                [&service, &draft, &overlay](const Key& key) -> std::optional<ModelRecord> {
                const std::map<Key, std::optional<ModelRecord>>::const_iterator found = overlay.find(key);
                if (found != overlay.end()) return found->second;
                if (!draft.baseRevision) return std::nullopt;
                return service.getModelAtRevision(draft.projectId, *draft.baseRevision, key);
            };
            QJsonArray networks;
            for (const Key& key : roots) {
                const QUuid id = hydraulic::uuidFromKey(key);
                const std::optional<NetworkHydraulic> network =
                    hydraulic::loadDraftHydraulicNetwork(reader, id);
                if (!network)
                    throw ProjectError(ProjectErrorKind::Unavailable,
                                       "saved draft hydraulic network could not be loaded");
                networks.append(hydraulicNetworkSummaryJson(*network));
            }
            return Reply{200, QJsonObject{
                {"scope", "working-copy"},
                {"draftId", QString::fromStdString(draft.id)},
                {"generation", QString::number(static_cast<qulonglong>(draft.generation))},
                {"networkCount", networks.size()},
                {"networks", networks}
            }};
        });
    });
    // Rehydrate one pinned draft candidate without publishing a revision.
    // Read the entire candidate once: no writer can mix generations inside
    // this response, and base records always come from its immutable revision.
    server.Get("/dvmdb/v1/working-copies/:draft/networks/:network",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            // The candidate only: the event journal is not needed to hydrate.
            const WorkingCopyInfo draft = service.workingCopy(pathParam(request, "draft"), false);
            const std::string networkText = pathParam(request, "network");
            requireInput(UuidV7::parse(networkText).has_value(), "networkId must be UUIDv7");
            const QUuid network(QString::fromStdString(networkText));
            std::map<Key, std::optional<ModelRecord>> overlay;
            for (const ModelChange& change : draft.candidateChanges)
                requireInput(overlay.emplace(change.key, change.record).second,
                             "duplicate draft candidate model record");
            const std::function<std::optional<ModelRecord>(const Key&)> reader =
                [&service, &draft, &overlay](const Key& key) -> std::optional<ModelRecord> {
                const std::map<Key, std::optional<ModelRecord>>::const_iterator found = overlay.find(key);
                if (found != overlay.end()) return found->second;
                if (!draft.baseRevision) return std::nullopt;
                return service.getModelAtRevision(draft.projectId, *draft.baseRevision, key);
            };
            const std::optional<NetworkHydraulic> result =
                hydraulic::loadDraftHydraulicNetwork(reader, network);
            if (!result)
                throw ProjectError(ProjectErrorKind::NotFound, "hydraulic network not found in draft");
            const QByteArray payload = ModelCodec<NetworkHydraulic>::encode(*result);
            if (payload.size() > static_cast<qsizetype>(8 * 1024 * 1024))
                throw StorageError(StorageErrorKind::LimitExceeded,
                                   "draft hydraulic network exceeds HTTP payload limit");
            return Reply{200, QJsonObject{
                {"draftId", QString::fromStdString(draft.id)},
                {"generation", QString::number(static_cast<qulonglong>(draft.generation))},
                {"networkId", QString::fromStdString(networkText)},
                {"payloadBase64", encodePayload(payload)}
            }};
        });
    });
#endif
    // Durable local Commit: the operation UUID pins an exact saved candidate,
    // lease, generation, expected HEAD and message. Retry with the same ID.
    server.Post("/dvmdb/v1/local-commits", [&service](const httplib::Request& request,
                                                     httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject body = parseBody(request, maxJsonMergeBytes);
            requireKeys(body, {"operationId", "draftId", "sessionId", "fencingToken",
                               "expectedGeneration", "expectedHead", "author", "message"});
            const QJsonValue expectedValue = body.value("expectedHead");
            requireInput(expectedValue.isString() || expectedValue.isNull(),
                         "expectedHead must be a revision UUID or null");
            const std::optional<RevisionId> expected = expectedValue.isNull()
                ? std::nullopt : std::optional<RevisionId>(parseRevisionId(
                    requiredString(body, "expectedHead")));
            const CommitMetadata metadata = parseCommitMetadata(body);
            requireInput(metadata.message.find_first_not_of(" \t\n\r") != std::string::npos,
                         "commit message cannot be whitespace only");
            const LocalCommitInfo result = service.commitWorkingCopy(
                requiredString(body, "operationId"), requiredString(body, "draftId"),
                requiredString(body, "sessionId"), requiredString(body, "fencingToken"),
                strictUint64(body, "expectedGeneration"), expected, metadata);
            const QString state = result.state == LocalCommitState::Succeeded
                ? QStringLiteral("succeeded") : result.state == LocalCommitState::Pending
                    ? QStringLiteral("pending") : QStringLiteral("rejected");
            return Reply{result.state == LocalCommitState::Succeeded ? 200
                : result.state == LocalCommitState::Pending ? 202 : 409,
                QJsonObject{{"operationId", QString::fromStdString(result.id)},
                            {"state", state},
                            {"revision", result.state == LocalCommitState::Succeeded
                                ? QJsonValue(QString::fromStdString(result.reservedRevisionId))
                                : QJsonValue(QJsonValue::Null)},
                            {"reason", QString::fromStdString(result.rejection)}}};
        });
    });
    server.Get("/dvmdb/v1/local-commits/:operationId",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const LocalCommitInfo result = service.localCommitStatus(pathParam(request, "operationId"));
            const QString state = result.state == LocalCommitState::Succeeded
                ? QStringLiteral("succeeded") : result.state == LocalCommitState::Pending
                    ? QStringLiteral("pending") : QStringLiteral("rejected");
            return Reply{200, QJsonObject{{"operationId", QString::fromStdString(result.id)},
                {"state", state}, {"revision", result.state == LocalCommitState::Succeeded
                    ? QJsonValue(QString::fromStdString(result.reservedRevisionId))
                    : QJsonValue(QJsonValue::Null)},
                {"reason", QString::fromStdString(result.rejection)}}};
        });
    });
    server.Post(routeWorkingCopyLease, [&service](const httplib::Request& request,
                                                  httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject body = parseBody(request, maxJsonSmallBytes);
            requireKeys(body, {"sessionId", "seconds"});
            requireInput(body.value("seconds").isDouble(), "missing lease seconds");
            const double seconds = body.value("seconds").toDouble();
            requireInput(seconds >= 1 && seconds <= 120 && seconds == static_cast<double>(static_cast<int>(seconds)),
                         "lease seconds must be 1..120 integer");
            const WorkingCopyLease lease = service.acquireWorkingCopyLease(
                pathParam(request, "draft"), requiredString(body, "sessionId"),
                static_cast<std::uint32_t>(seconds));
            return Reply{200, QJsonObject{{"token", QString::fromStdString(lease.token)},
                {"expiresAtMs", QString::number(static_cast<qulonglong>(lease.expiresAtMs))},
                {"generation", QString::number(static_cast<qulonglong>(lease.generation))}}};
        });
    });
    server.Post(routeWorkingCopySave, [&service](const httplib::Request& request,
                                                 httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject body = parseBody(request, maxJsonModelBytes);
            requireKeys(body, {"sessionId", "fencingToken", "expectedGeneration", "changes"});
            const std::vector<ModelChange> changes = parseModelChanges(body, false, true);
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
            for (const ModelChange& change : changes)
                requireInput(!change.record ||
                             (change.record->kind != hydraulic::hydraulicNetworkKind &&
                              change.record->kind != 101),
                             "native hydraulic manifest/chunk writes are server-owned");
#endif
            return Reply{200, workingCopyJson(service.saveWorkingCopyChanges(
                pathParam(request, "draft"), requiredString(body, "sessionId"),
                requiredString(body, "fencingToken"), strictUint64(body, "expectedGeneration"), changes))};
        });
    });
    // Atomic offline-event replay: event bytes and candidate records publish in
    // the same fsynced working-copy record. No event can be acknowledged alone.
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
    // Import an entire network into an untouched private unborn draft. The
    // importer cannot write to a project or committed revision. The native
    // hydraulic adapter owns all manifest/chunk encoding; writer fencing and
    // generation CAS are enforced by ProjectService's durable Save.
    server.Post(routeWorkingCopyImportNetwork, [&service](const httplib::Request& request,
                                                         httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject body = parseBody(request, maxJsonModelBytes);
            requireKeys(body, {"sessionId", "fencingToken", "expectedGeneration", "payloadBase64"});
            const std::string session = requiredString(body, "sessionId");
            const std::string token = requiredString(body, "fencingToken");
            const std::uint64_t expected = strictUint64(body, "expectedGeneration");
            const QByteArray payload = decodePayload(body, "payloadBase64", maxJsonModelBytes);
            QString error;
            const std::optional<NetworkHydraulic> network = ModelCodec<NetworkHydraulic>::decode(payload, &error);
            if (!network || ModelCodec<NetworkHydraulic>::encode(*network) != payload)
                throw ProjectError(ProjectErrorKind::InvalidInput,
                                   "invalid canonical imported hydraulic network: " + error.toStdString());
            requireInput(UuidV7::parse(uuidString(network->uuid).toStdString()).has_value(),
                         "imported hydraulic network UUID must be UUIDv7");
            const std::vector<ModelChange> changes =
                hydraulic::fullDraftHydraulicNetworkChanges(*network);
            const std::string draftId = pathParam(request, "draft");
            const WorkingCopyInfo draft = service.workingCopy(draftId, false);
            if (draft.baseRevision.has_value() || !draft.candidateChanges.empty() ||
                draft.journalCount != 0)
                throw ProjectError(ProjectErrorKind::Conflict,
                                   "import requires a new empty unborn draft; existing network data cannot be replaced");
            if (draft.generation != expected)
                throw ProjectError(ProjectErrorKind::Conflict, "working copy generation changed");
            const WorkingCopyInfo saved = service.saveWorkingCopyChanges(
                draftId, session, token, expected, changes);
            return Reply{200, workingCopyJson(saved)};
        });
    });
    // Bootstrap the first hydraulic network in a private unborn working copy.
    // The server owns manifest encoding. There is no implicit revision, no
    // synthetic initial commit and no direct arbitrary kind-100 client write.
    server.Post(routeWorkingCopyInitializeNetwork, [&service](const httplib::Request& request,
                                                               httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject body = parseBody(request, maxJsonSmallBytes);
            requireKeys(body, {"sessionId", "fencingToken", "expectedGeneration",
                               "networkId", "name"});
            const std::string session = requiredString(body, "sessionId");
            const std::string token = requiredString(body, "fencingToken");
            const std::uint64_t expected = strictUint64(body, "expectedGeneration");
            const std::string networkId = requiredString(body, "networkId");
            requireInput(UuidV7::parse(networkId).has_value(),
                         "new network identity must be UUIDv7");
            const QString name = body.value("name").toString();
            requireInput(body.value("name").isString() && !name.trimmed().isEmpty() &&
                         name.size() <= 255, "invalid new hydraulic network name");
            const ModelChange initial = hydraulic::emptyDraftHydraulicNetworkRoot(
                QUuid(QString::fromStdString(networkId)), name);
            const std::string draftId = pathParam(request, "draft");
            const WorkingCopyInfo draft = service.workingCopy(draftId, false);
            requireInput(!draft.baseRevision.has_value(),
                         "first-network initialization requires an unborn working copy");
            // A successful response may be lost. Only the same still-leased
            // writer with the exact same root at exactly generation+1 can
            // recover it; an expired lease is never a valid acknowledgement.
            if (expected < std::numeric_limits<std::uint64_t>::max() &&
                service.workingCopyLeaseLive(draft) &&
                draft.generation == expected + 1 &&
                draft.leaseSessionId == std::optional<std::string>(session) &&
                draft.leaseToken == std::optional<std::string>(token) &&
                draft.journalCount == 0 && draft.candidateChanges.size() == 1 &&
                draft.candidateChanges.front().key == initial.key &&
                draft.candidateChanges.front().record.has_value() &&
                initial.record.has_value() &&
                draft.candidateChanges.front().record->kind == initial.record->kind &&
                draft.candidateChanges.front().record->references == initial.record->references &&
                draft.candidateChanges.front().record->payload == initial.record->payload) {
                return Reply{200, workingCopyJson(draft)};
            }
            if (draft.generation != expected || !draft.candidateChanges.empty() ||
                draft.journalCount != 0)
                throw ProjectError(ProjectErrorKind::Conflict,
                                   "first network requires an untouched working copy");
            const WorkingCopyInfo saved = service.saveWorkingCopyChanges(
                draftId, session, token, expected, {initial});
            return Reply{200, workingCopyJson(saved)};
        });
    });
#endif
    server.Post(routeWorkingCopyEventSave, [&service](const httplib::Request& request,
                                                      httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject body = parseBody(request, maxJsonModelBytes);
            requireKeys(body, {"sessionId", "fencingToken", "expectedGeneration", "changes", "events", "membership"});
            // Root-only MODEL events intentionally carry no client model records.
            // Other empty batches are rejected below after decoding the events.
            const std::vector<ModelChange> changes = parseModelChanges(body, false, true);
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
            for (const ModelChange& change : changes)
                requireInput(!change.record ||
                             (change.record->kind != hydraulic::hydraulicNetworkKind &&
                              change.record->kind != 101),
                             "native hydraulic manifest/chunk writes are server-owned");
#endif
            requireInput(body.value("events").isArray(), "missing edit events array");
            const QJsonArray items = body.value("events").toArray();
            requireInput(!items.isEmpty() && items.size() <= 128,
                         "event save requires 1..128 transactions");
            std::vector<Bytes> events;
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
            // Decoded once here; the membership checks below reuse them.
            std::vector<aowis::model::ModelEditTransaction> transactions;
            transactions.reserve(static_cast<std::size_t>(items.size()));
#endif
            for (const QJsonValue& item : items) {
                requireInput(item.isString(), "edit event must be base64");
                const QByteArray encoded = item.toString().toLatin1();
                requireInput(!encoded.isEmpty() && encoded.size() < 12 * 1024 * 1024,
                             "invalid edit event base64 size");
                const QByteArray decoded = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
                requireInput(!decoded.isEmpty() && decoded.toBase64() == encoded,
                             "invalid edit event base64 encoding");
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
                QString error;
                std::optional<aowis::model::ModelEditTransaction> transaction =
                    aowis::model::ModelEditTransactionCodec::decode(decoded, &error);
                // The GUI has already decoded this event before sending it.
                // Preserve the precise reason when this server's MODEL codec
                // rejects the same frame (for example, a codec-version mismatch).
                // Never acknowledge or alter the private draft on decode failure.
                if (!transaction.has_value())
                    throw ProjectError(ProjectErrorKind::InvalidInput,
                        "invalid MODEL edit transaction: " + error.toStdString());
                transactions.push_back(std::move(*transaction));
#endif
                events.emplace_back(reinterpret_cast<const std::uint8_t*>(decoded.constData()),
                                    reinterpret_cast<const std::uint8_t*>(decoded.constData()) + decoded.size());
            }
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
            // Creating or removing an entity changes its network's membership.
            // Without explicit membership intents the candidate would hold an
            // entity record that no membership chunk lists (or a chunk that
            // still lists a removed entity). Reject before reading the draft,
            // independent of generation: a lost-response replay of an accepted
            // request always carried its membership, so it is not affected.
            if (!body.contains("membership")) {
                for (const aowis::model::ModelEditTransaction& transaction : transactions)
                    for (const aowis::model::ModelEntityEdit& edit : transaction.edits)
                        requireInput(edit.before.has_value() && edit.after.has_value(),
                                     "missing membership for created or removed entity");
            }
#endif
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
            // One read of the draft serves both image checks. A concurrent
            // Save changes the generation, which the Save below rejects.
            const WorkingCopyInfo current = service.workingCopy(pathParam(request, "draft"), false);
            if (current.generation == strictUint64(body, "expectedGeneration"))
                verifyEditImages(service, current, transactions, changes);
#endif
            std::vector<ModelChange> materialized = changes;
            std::optional<std::string> logicalRequestFingerprint;
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
            // The root image belongs to the same reversible MODEL transaction
            // as its ordinary entity edits. Native kind-100 and kind-101
            // records remain entirely server-owned. Materialize root metadata
            // *after* membership so both changes produce one final manifest.
            const aowis::model::ModelEntityEdit* rootEdit = nullptr;
            for (const aowis::model::ModelEditTransaction& transaction : transactions) {
                for (const aowis::model::ModelEntityEdit& edit : transaction.edits) {
                    if (edit.entityType != QString::fromLatin1(aowis::model::hydraulicRootEditType))
                        continue;
                    requireInput(rootEdit == nullptr && transactions.size() == 1 &&
                                 edit.before.has_value() && edit.after.has_value(),
                                 "only one reversible network-root edit is allowed in one event batch");
                    rootEdit = &edit;
                }
            }
            requireInput(rootEdit != nullptr || !changes.empty(),
                         "ordinary edit events require model records");
            if (rootEdit) {
                // Materialized manifest/chunk bytes depend on the current
                // generation; fingerprint the logical request for exact
                // lost-acknowledgement replay instead.
                const QByteArray bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);
                logicalRequestFingerprint = QCryptographicHash::hash(
                    bytes, QCryptographicHash::Sha256).toHex().toStdString();
            }
#endif
            if (body.contains("membership")) {
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
                requireInput(body.value("membership").isArray(), "membership must be an array");
                const QJsonArray membership = body.value("membership").toArray();
                requireInput(!membership.isEmpty() && membership.size() <= 128,
                             "membership requires 1..128 bounded operations");
                std::vector<hydraulic::DraftMembershipIntent> intents;
                std::set<Key> identities;
                for (const QJsonValue& item : membership) {
                    requireInput(item.isObject(), "membership entry must be an object");
                    const QJsonObject entry = item.toObject();
                    requireKeys(entry, {"networkId", "entityId", "kind", "collectionIndex", "inserted"});
                    requireInput(entry.value("kind").isDouble() &&
                                 entry.value("collectionIndex").isDouble() &&
                                 entry.value("inserted").isBool(),
                                 "invalid membership kind, collection or operation");
                    const double kind = entry.value("kind").toDouble();
                    const double collection = entry.value("collectionIndex").toDouble();
                    requireInput(kind >= 1 && kind <= 65535 &&
                                 kind == static_cast<double>(static_cast<std::uint16_t>(kind)) &&
                                 collection >= 0 && collection < 18 &&
                                 collection == static_cast<double>(static_cast<std::uint16_t>(collection)),
                                 "invalid membership kind or collection index");
                    hydraulic::DraftMembershipIntent intent;
                    intent.network = parseModelKey(requiredString(entry, "networkId"));
                    intent.member = parseModelKey(requiredString(entry, "entityId"));
                    intent.kind = static_cast<std::uint16_t>(kind);
                    intent.collection = static_cast<std::uint16_t>(collection);
                    intent.inserted = entry.value("inserted").toBool();
                    requireInput(identities.insert(intent.member).second,
                                 "duplicate membership entity in save");
                    intents.push_back(intent);
                }
                // The fingerprint is of the *logical* request, not its derived
                // native chunk records. Replays after a lost response must
                // return the prior acknowledgement without re-applying intents.
                const QByteArray requestBytes = QJsonDocument(body).toJson(QJsonDocument::Compact);
                logicalRequestFingerprint = QCryptographicHash::hash(
                    requestBytes, QCryptographicHash::Sha256).toHex().toStdString();
                const WorkingCopyInfo& draft = current;
                const std::uint64_t expected = strictUint64(body, "expectedGeneration");
                if (draft.generation == expected && !intents.empty()) {
                    std::map<Key, std::optional<ModelRecord>> existing;
                    for (const ModelChange& change : draft.candidateChanges)
                        existing.emplace(change.key, change.record);
                    const std::function<std::optional<ModelRecord>(const Key&)> reader =
                        [&service, &draft, &existing](const Key& key) -> std::optional<ModelRecord> {
                        const std::map<Key, std::optional<ModelRecord>>::const_iterator it =
                            existing.find(key);
                        if (it != existing.end()) return it->second;
                        if (!draft.baseRevision) return std::nullopt;
                        return service.getModelAtRevision(draft.projectId, *draft.baseRevision, key);
                    };
                    // Every membership transition must be the exact
                    // before/after image in the MODEL event journal. Otherwise
                    // future undo/redo could reverse a different model.
                    const std::array<QString, 18> memberTypes = {
                        QStringLiteral("HydraulicPipeMaterial"),
                        QStringLiteral("HydraulicPatternTime"),
                        QStringLiteral("HydraulicCurveTankVolume"),
                        QStringLiteral("HydraulicCurvePumpHead"),
                        QStringLiteral("HydraulicCurvePumpEfficiency"),
                        QStringLiteral("HydraulicCurveValveHeadloss"),
                        QStringLiteral("HydraulicCurveValveCharacteristic"),
                        QStringLiteral("HydraulicCurveGeneric"),
                        QStringLiteral("HydraulicNodeReservoir"),
                        QStringLiteral("HydraulicNodeJunction"),
                        QStringLiteral("HydraulicNodeTank"),
                        QStringLiteral("HydraulicDemandPoint"),
                        QStringLiteral("HydraulicLinkPipe"),
                        QStringLiteral("HydraulicLinkPump"),
                        QStringLiteral("HydraulicLinkValve"),
                        QStringLiteral("HydraulicControlSimple"),
                        QStringLiteral("HydraulicControlRule"),
                        QStringLiteral("HydraulicMapLabel")
                    };
                    std::map<Key, aowis::model::ModelEntityEdit> eventMembership;
                    std::set<Key> intentKeys;
                    for (const hydraulic::DraftMembershipIntent& intent : intents)
                        intentKeys.insert(intent.member);
                    for (const aowis::model::ModelEditTransaction& transaction : transactions) {
                        for (const aowis::model::ModelEntityEdit& edit : transaction.edits) {
                            const Key key = hydraulic::keyFromUuid(edit.entityId);
                            if ((!edit.before || !edit.after) && !intentKeys.contains(key))
                                requireInput(false, "missing membership for created or removed entity");
                            if (intentKeys.contains(key)) {
                                requireInput(eventMembership.emplace(key, edit).second,
                                             "duplicate membership edit in event batch");
                            }
                        }
                    }
                    for (const hydraulic::DraftMembershipIntent& intent : intents) {
                        const std::map<Key, aowis::model::ModelEntityEdit>::const_iterator match =
                            eventMembership.find(intent.member);
                        requireInput(match != eventMembership.end() &&
                                     match->second.entityType == memberTypes.at(intent.collection),
                                     "MODEL event does not describe requested membership type");
                        const aowis::model::ModelEntityEdit& edit = match->second;
                        const std::optional<ModelRecord> previous = reader(intent.member);
                        const std::vector<ModelChange>::const_iterator incoming =
                            std::find_if(changes.begin(), changes.end(),
                                [&intent](const ModelChange& change) {
                                    return change.key == intent.member;
                                });
                        requireInput(incoming != changes.end(), "missing entity record for membership");
                        if (intent.inserted) {
                            requireInput(!edit.before && edit.after && incoming->record &&
                                         incoming->record->payload == hydraulic::bytesFromQByteArray(*edit.after),
                                         "membership insertion differs from MODEL after-image");
                        } else {
                            requireInput(edit.before && !edit.after && previous &&
                                         previous->payload == hydraulic::bytesFromQByteArray(*edit.before) &&
                                         !incoming->record,
                                         "membership removal differs from MODEL before-image");
                        }
                    }
                    hydraulic::materializeDraftMembership(reader, intents, materialized);
                } else if (draft.generation != expected + (expected < std::numeric_limits<std::uint64_t>::max() ? 1 : 0)) {
                    throw ProjectError(ProjectErrorKind::Conflict, "draft generation changed");
                }
#else
                throw ProjectError(ProjectErrorKind::InvalidInput,
                                   "hydraulic membership requires the model persistence build");
#endif
            }
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
            // Membership may have produced an updated kind-100 manifest.
            // Build the final candidate overlay and apply the metadata edit to
            // that manifest, never emit two changes for the same root UUID.
            if (rootEdit && current.generation == strictUint64(body, "expectedGeneration")) {
                std::map<Key, std::optional<ModelRecord>> overlay;
                for (const ModelChange& change : current.candidateChanges)
                    overlay[change.key] = change.record;
                for (const ModelChange& change : materialized)
                    overlay[change.key] = change.record;
                const std::function<std::optional<ModelRecord>(const Key&)> reader =
                    [&service, &current, &overlay](const Key& key) -> std::optional<ModelRecord> {
                    const std::map<Key, std::optional<ModelRecord>>::const_iterator item = overlay.find(key);
                    if (item != overlay.end()) return item->second;
                    if (!current.baseRevision) return std::nullopt;
                    return service.getModelAtRevision(current.projectId, *current.baseRevision, key);
                };
                ModelChange metadata = hydraulic::materializeDraftRootEdit(reader, *rootEdit);
                std::vector<ModelChange>::iterator existing =
                    std::find_if(materialized.begin(), materialized.end(),
                        [&metadata](const ModelChange& item) { return item.key == metadata.key; });
                if (existing != materialized.end())
                    *existing = std::move(metadata);
                else
                    materialized.push_back(std::move(metadata));
            }
#endif
            const WorkingCopyInfo saved = service.saveWorkingCopyTransactions(
                pathParam(request, "draft"), requiredString(body, "sessionId"),
                requiredString(body, "fencingToken"), strictUint64(body, "expectedGeneration"),
                materialized, events, logicalRequestFingerprint);
            // Small ACK for weak connections. Full candidate/journal is fetched
            // explicitly only for recovery or undo history inspection. The
            // result's journal holds exactly this (or the replayed) Save's events.
            QJsonArray confirmedIds;
            for (const WorkingCopyEvent& event : saved.journal)
                confirmedIds.append(QString::fromStdString(event.id));
            return Reply{200, QJsonObject{
                {"id", QString::fromStdString(saved.id)},
                {"generation", QString::number(static_cast<qulonglong>(saved.generation))},
                {"eventCount", QString::number(static_cast<qulonglong>(saved.journalCount))},
                {"acceptedEventIds", confirmedIds}
            }};
        });
    });
    server.Post(routeWorkingCopyRelease, [&service](const httplib::Request& request,
                                                    httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject body = parseBody(request, maxJsonSmallBytes);
            requireKeys(body, {"sessionId", "fencingToken"});
            service.releaseWorkingCopyLease(pathParam(request, "draft"),
                requiredString(body, "sessionId"), requiredString(body, "fencingToken"));
            return Reply{200, QJsonObject{{"released", true}}};
        });
    });
    // Fenced like a Save. A repeated request after success answers 404.
    server.Post(routeWorkingCopyDiscard, [&service](const httplib::Request& request,
                                                    httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject body = parseBody(request, maxJsonSmallBytes);
            requireKeys(body, {"sessionId", "fencingToken", "expectedGeneration"});
            service.discardWorkingCopy(pathParam(request, "draft"),
                requiredString(body, "sessionId"), requiredString(body, "fencingToken"),
                strictUint64(body, "expectedGeneration"));
            return Reply{200, QJsonObject{{"discarded", true}}};
        });
    });
    server.Get(routeProjects, [&service](const httplib::Request&, httplib::Response& response) {
        guarded(response, [&]() {
            QJsonArray projects;
            for (const ProjectInfo& project : service.listProjects()) projects.append(projectJson(project));
            return Reply{200, QJsonObject{{"projects", projects}}};
        });
    });
    server.Post(routeProjects, [&service](const httplib::Request& request,
                                          httplib::Response& response) {
        guarded(response, [&]() {
            if (const std::optional<Reply> pin = repositoryPinFailure(request, service))
                return *pin;
            const QJsonObject object = parseBody(request, maxJsonSmallBytes);
            requireKeys(object, {"name", "unborn", "id", "author", "message"});
            const QJsonValue unborn = object.value("unborn");
            requireInput(unborn.isUndefined() || unborn.isBool(), "unborn must be a boolean");
            const std::string name = requiredString(object, "name");
            // An unborn project has no revision, so nothing has an author.
            requireInput(!unborn.toBool(false) ||
                         (!object.contains("author") && !object.contains("message")),
                         "an unborn project has no first revision to give an author or message");
            // A caller-chosen UUIDv7 makes the creation idempotent: a resent
            // request (lost response, transport retry) gets the same 201
            // answer instead of creating a second project.
            if (object.contains("id")) {
                requireInput(unborn.toBool(false), "a caller-chosen project id requires unborn: true");
                return Reply{201, projectJson(service.createUnbornProject(
                    name, requiredString(object, "id")))};
            }
            if (unborn.toBool(false)) return Reply{201, projectJson(service.createUnbornProject(name))};
            return Reply{201, projectJson(service.createProject(
                name, parseCommitMetadata(object, "Create project " + name)))};
        });
    });
    server.Post("/dvmdb/v1/projects/clone-target", [&service](const httplib::Request& request,
                                                                httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject object = parseBody(request, maxJsonSmallBytes);
            requireKeys(object, {"projectId", "name"});
            const std::string id = requiredString(object, "projectId");
            const std::string name = requiredString(object, "name");
            return Reply{200, projectJson(service.provisionCloneTarget(id, name))};
        });
    });
    // An unborn branch has no revision to export as a bundle. Only an empty
    // private clone target may acquire its original UUIDv7 and null HEAD.
    server.Post("/dvmdb/v1/projects/:id/unborn-checkout",
                [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject object = parseBody(request, maxJsonSmallBytes);
            requireKeys(object, {"branchId", "name"});
            const std::string project = pathParam(request, "id");
            const std::string branchId = requiredString(object, "branchId");
            const std::string name = requiredString(object, "name");
            service.provisionUnbornBranch(project, name, branchId);
            return Reply{200, QJsonObject{{"branchId", QString::fromStdString(branchId)},
                {"branch", QString::fromStdString(name)},
                {"revision", QJsonValue(QJsonValue::Null)}}};
        });
    });
    server.Get("/dvmdb/v1/projects/:id", [&service](const httplib::Request& request,
                                                  httplib::Response& response) {
        guarded(response, [&]() {
            return Reply{200, projectJson(service.project(pathParam(request, "id")))};
        });
    });
    server.Patch("/dvmdb/v1/projects/:id", [&service](const httplib::Request& request,
                                                    httplib::Response& response) {
        guarded(response, [&]() {
            if (const std::optional<Reply> pin = repositoryPinFailure(request, service))
                return *pin;
            const QJsonObject object = parseBody(request, maxJsonSmallBytes);
            requireKeys(object, {"name", "expectedName"});
            return Reply{200, projectJson(service.renameProject(pathParam(request, "id"),
                requiredString(object, "expectedName"), requiredString(object, "name")))};
        });
    });
    registerBundleRoutes(server, service, transferLimits, stagingBudget);
    server.Get(routeBranches, [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            QJsonArray branches;
            for (const std::string& branch : service.listBranches(pathParam(request, "id")))
                branches.append(QString::fromStdString(branch));
            return Reply{200, QJsonObject{{"branches", branches}}};
        });
    });
    // Canonical API: UUIDv7 is the only revision identity on these routes.
    // SHA-256 is integrity metadata, never an address or branch HEAD.
    server.Post(routeBranches,
                [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            if (const std::optional<Reply> pin = repositoryPinFailure(request, service))
                return *pin;
            const QJsonObject object = parseBody(request, maxJsonSmallBytes);
            requireKeys(object, {"name", "fromBranchId", "revision", "id", "unborn", "description", "tags"});
            const std::string project = pathParam(request, "id");
            const std::string name = requiredString(object, "name");
            if (object.contains("unborn")) {
                requireInput(object.value("unborn").isBool(), "unborn must be boolean");
                if (object.value("unborn").toBool()) {
                    requireInput(!object.contains("fromBranchId") &&
                                 !object.contains("revision"),
                                 "unborn branch cannot have an origin revision");
                    const std::string branchId = object.contains("id")
                        ? requiredString(object, "id") : BranchId::generate().toString();
                    const BranchMetadata metadata = parseBranchMetadata(object);
                    const BranchId created = service.createUnbornBranch(
                        project, name, branchId, metadata);
                    return Reply{201, QJsonObject{
                        {"id", QJsonValue(QJsonValue::Null)},
                        {"branch", QString::fromStdString(name)},
                        {"branchId", QString::fromStdString(created.toString())},
                        {"originBranchId", QJsonValue(QJsonValue::Null)},
                        {"forkRevisionId", QJsonValue(QJsonValue::Null)},
                        {"description", QString::fromStdString(metadata.description)},
                        {"tags", branchTagsJson(metadata)}}};
                }
            }
            requireInput(!object.contains("description") && !object.contains("tags"),
                         "branch metadata is supported for unborn creation only");
            // The source is addressed by its UUIDv7, never by its renamable
            // name (DG-01). `id` is the new branch's caller-chosen UUIDv7:
            // repeating the request returns the original creation instead of
            // a conflict. Without it the server chooses one (not retry-safe).
            const std::string source = requiredString(object, "fromBranchId");
            const std::string identity = object.contains("id")
                ? requiredString(object, "id") : BranchId::generate().toString();
            const std::optional<RevisionId> revision = object.contains("revision")
                ? std::optional<RevisionId>(parseRevisionId(requiredString(object, "revision")))
                : std::nullopt;
            // Return the identity/provenance captured by the atomic native
            // branch write. Never re-resolve a mutable branch name afterward.
            const BranchCreationResult created =
                service.createBranchFromId(project, name, source, revision, identity);
            return Reply{201, QJsonObject{{"id", revisionIdString(created.revisionId)},
                {"branch", QString::fromStdString(name)},
                {"branchId", QString::fromStdString(created.branchId.toString())},
                {"originBranchId", created.originBranchId
                    ? QJsonValue(QString::fromStdString(created.originBranchId->toString()))
                    : QJsonValue(QJsonValue::Null)},
                {"forkRevisionId", created.forkRevisionId
                    ? QJsonValue(revisionIdString(*created.forkRevisionId))
                    : QJsonValue(QJsonValue::Null)}}};
        });
    });
    server.Patch("/dvmdb/v1/projects/:id/branches/:branch",
                 [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            if (const std::optional<Reply> pin = repositoryPinFailure(request, service))
                return *pin;
            const QJsonObject object = parseBody(request, maxJsonSmallBytes);
            requireKeys(object, {"name", "expectedHead"});
            const std::string projectId = pathParam(request, "id");
            const std::string previous = pathParam(request, "branch");
            const std::string renamed = requiredString(object, "name");
            const RevisionId expected = parseRevisionId(requiredString(object, "expectedHead"));
            const std::optional<ProjectId> project = ProjectId::parse(projectId);
            requireInput(project.has_value(), "project must be UUIDv7");
            service.withRepository(projectId, [&](Repository& repository) {
                repository.renameBranch(*project, previous, renamed, expected);
            });
            return Reply{200, QJsonObject{{"name", QString::fromStdString(renamed)},
                                           {"head", revisionIdString(expected)}}};
        });
    });
    // Destructive branch operations use an explicit expected HEAD. They only
    // rewrite or remove a branch reference, never shared immutable merge nodes.
    server.Post("/dvmdb/v1/projects/:id/branch-ids/:branchId/delete",
                [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            if (const std::optional<Reply> pin = repositoryPinFailure(request, service))
                return *pin;
            const QJsonObject body = parseBody(request, maxJsonSmallBytes);
            requireKeys(body, {"expectedHead", "discardWorkingCopies"});
            requireInput(body.contains("expectedHead"), "expectedHead is required, null for unborn");
            const std::optional<RevisionId> expected = body.value("expectedHead").isNull()
                ? std::nullopt
                : std::optional<RevisionId>(parseRevisionId(requiredString(body, "expectedHead")));
            service.deleteBranch(pathParam(request, "id"), pathParam(request, "branchId"), expected,
                body.value("discardWorkingCopies").toBool(false));
            return Reply{200, QJsonObject{{"deleted", true}}};
        });
    });
    server.Post("/dvmdb/v1/projects/:id/branch-ids/:branchId/truncate",
                [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            if (const std::optional<Reply> pin = repositoryPinFailure(request, service))
                return *pin;
            const QJsonObject body = parseBody(request, maxJsonSmallBytes);
            requireKeys(body, {"expectedHead", "firstRemoved", "discardWorkingCopies"});
            requireInput(body.contains("expectedHead"), "expectedHead is required");
            const RevisionId expected = parseRevisionId(requiredString(body, "expectedHead"));
            const RevisionId firstRemoved = parseRevisionId(requiredString(body, "firstRemoved"));
            const bool deleted = service.truncateBranch(pathParam(request, "id"),
                pathParam(request, "branchId"), expected, firstRemoved,
                body.value("discardWorkingCopies").toBool(false));
            return Reply{200, QJsonObject{{"branchDeleted", deleted}}};
        });
    });
    // Identity-addressed metadata update also supports unborn branches.
    server.Patch("/dvmdb/v1/projects/:id/branch-ids/:branchId",
                 [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            if (const std::optional<Reply> pin = repositoryPinFailure(request, service))
                return *pin;
            const QJsonObject object = parseBody(request, maxJsonSmallBytes);
            requireKeys(object, {"expectedName", "name", "description", "tags"});
            const std::string project = pathParam(request, "id");
            const std::string branchId = pathParam(request, "branchId");
            const std::string expected = requiredString(object, "expectedName");
            const std::string name = requiredString(object, "name");
            const BranchMetadata metadata = parseBranchMetadata(object);
            service.updateBranchProperties(project, branchId, expected, name, metadata);
            return Reply{200, QJsonObject{{"branchId", QString::fromStdString(branchId)},
                {"name", QString::fromStdString(name)},
                {"description", QString::fromStdString(metadata.description)},
                {"tags", branchTagsJson(metadata)}}};
        });
    });
    // A branch by UUIDv7 (DG-01): its current name, HEAD and provenance.
    // Clients pin a branch by identity here, then use the name-addressed
    // routes and verify the identity they return (e.g. a bundle header).
    server.Get("/dvmdb/v1/projects/:id/branch-ids/:branchId",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const std::optional<RevisionGraphHead> head = service.branchById(
                pathParam(request, "id"), pathParam(request, "branchId"));
            if (!head) return errorReply(404, QStringLiteral("not_found"),
                                         QStringLiteral("branch UUIDv7 not found"));
            return Reply{200, branchHeadJson(*head)};
        });
    });
    server.Get("/dvmdb/v1/projects/:id/branches/:branch/head",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const std::string project = pathParam(request, "id");
            const std::string branch = pathParam(request, "branch");
            const std::optional<RevisionId> head = service.optionalHead(project, branch);
            return Reply{200, QJsonObject{{"id", head ? QJsonValue(revisionIdString(*head))
                                                      : QJsonValue(QJsonValue::Null)}}};
        });
    });
    // HIST-02: read-only branch timeline. A Commit is a checkpoint; original
    // archived action UUIDs appear only when an actual local Commit archive
    // exists. Draft event pages are separate and generation-labelled.
    server.Get("/dvmdb/v1/projects/:id/branches/:branch/action-history",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            std::size_t limit = 5;
            if (const std::optional<std::string> value = queryValue(request, "limit")) {
                bool ok = false;
                const unsigned number = QString::fromStdString(*value).toUInt(&ok);
                requireInput(ok && number >= 1 && number <= 10, "invalid action-history limit");
                limit = number;
            }
            std::optional<RevisionId> after;
            if (const std::optional<std::string> value = queryValue(request, "after"))
                after = parseRevisionId(*value);
            const std::optional<std::string> draft = queryValue(request, "draftId");
            std::optional<std::uint64_t> expectedDraftGeneration;
            if (const std::optional<std::string> value = queryValue(request, "expectedDraftGeneration")) {
                requireInput(!value->empty() && value->size() <= 20 &&
                    std::all_of(value->begin(), value->end(), [](char ch) {
                        return ch >= '0' && ch <= '9';
                    }), "invalid expected draft generation");
                std::uint64_t parsed = 0;
                const std::from_chars_result converted =
                    std::from_chars(value->data(), value->data() + value->size(), parsed);
                requireInput(converted.ec == std::errc() &&
                             converted.ptr == value->data() + value->size(),
                             "invalid expected draft generation");
                expectedDraftGeneration = parsed;
            }
            std::size_t draftOffset = 0;
            if (const std::optional<std::string> value = queryValue(request, "draftOffset")) {
                const std::string& text = *value;
                requireInput(!text.empty() && text.size() <= 5, "invalid draft offset");
                std::size_t parsed = 0;
                const std::from_chars_result result =
                    std::from_chars(text.data(), text.data() + text.size(), parsed);
                requireInput(result.ec == std::errc() &&
                             result.ptr == text.data() + text.size() && parsed <= 4096,
                             "invalid draft offset");
                draftOffset = parsed;
            }
            const BranchActionHistoryPage page = service.branchActionHistory(
                pathParam(request, "id"), pathParam(request, "branch"), limit, after,
                draft ? std::optional<std::string_view>(*draft) : std::nullopt,
                draftOffset, expectedDraftGeneration);
            QJsonArray checkpoints;
            for (const BranchActionHistoryEntry& entry : page.commits) {
                QJsonArray actions;
                for (const std::string& id : entry.actionIds)
                    actions.append(QString::fromStdString(id));
                checkpoints.append(QJsonObject{
                    {"revision", revisionJson(entry.revision)},
                    {"operationId", entry.commitOperationId
                        ? QJsonValue(QString::fromStdString(*entry.commitOperationId))
                        : QJsonValue(QJsonValue::Null)},
                    {"actions", actions},
                    {"actionCount", static_cast<int>(entry.actionIds.size())},
                    {"logicalParentRevisionId", entry.logicalParentRevisionId
                        ? QJsonValue(QString::fromStdString(*entry.logicalParentRevisionId))
                        : QJsonValue(QJsonValue::Null)},
                    {"checkpointOnly", entry.actionIds.empty()}});
            }
            QJsonValue draftValue = QJsonValue(QJsonValue::Null);
            if (page.draftId) {
                QJsonArray actions;
                for (const std::string& id : page.draftActionIds)
                    actions.append(QString::fromStdString(id));
                QJsonArray details;
                for (const WorkingCopyEvent& event : page.draftActionFrames)
                    details.append(historyActionJson(event, false));
                draftValue = QJsonObject{
                    {"id", QString::fromStdString(*page.draftId)},
                    {"generation", QString::number(static_cast<qulonglong>(page.draftGeneration))},
                    {"journalCount", QString::number(static_cast<qulonglong>(page.draftEventCount))},
                    {"actions", actions},
                    {"actionDetails", details},
                    {"nextOffset", page.nextDraftOffset
                        ? QJsonValue(static_cast<int>(*page.nextDraftOffset))
                        : QJsonValue(QJsonValue::Null)}};
            }
            return Reply{200, QJsonObject{{"commits", checkpoints},
                {"nextCursor", optionalRevisionJson(page.next)}, {"draft", draftValue}}};
        });
    });
    // HIST-02: original archived MODEL frames for a specific first-parent
    // Commit. Checkpoint-only revisions return an empty page; no events are
    // fabricated and no navigation or HEAD mutation occurs.
    server.Get("/dvmdb/v1/projects/:id/branches/:branch/action-history/:revisionId/events",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            std::size_t offset = 0;
            std::size_t limit = 100;
            const std::function<std::size_t(const std::string&, std::size_t)> parseCount =
                [](const std::string& value, std::size_t max) -> std::size_t {
                    requireInput(!value.empty() && value.size() <= 5,
                                 "invalid action frame pagination parameter");
                    std::size_t parsed = 0;
                    const std::from_chars_result result =
                        std::from_chars(value.data(), value.data() + value.size(), parsed);
                    requireInput(result.ec == std::errc() &&
                                 result.ptr == value.data() + value.size() && parsed <= max,
                                 "invalid action frame pagination parameter");
                    return parsed;
                };
            if (const std::optional<std::string> value = queryValue(request, "offset"))
                offset = parseCount(*value, 4096);
            if (const std::optional<std::string> value = queryValue(request, "limit")) {
                limit = parseCount(*value, 100);
                requireInput(limit > 0, "invalid action frame page limit");
            }
            const std::optional<std::string> draftId = queryValue(request, "draftId");
            const RevisionActionPage page = service.revisionActionPage(
                pathParam(request, "id"), pathParam(request, "branch"),
                parseRevisionId(pathParam(request, "revisionId")), offset, limit,
                draftId ? std::optional<std::string_view>(*draftId) : std::nullopt);
            QJsonArray actions;
            for (const WorkingCopyEvent& event : page.actions)
                actions.append(historyActionJson(event, true));
            return Reply{200, QJsonObject{
                {"revisionId", QString::fromStdString(page.revisionId)},
                {"operationId", page.commitOperationId
                    ? QJsonValue(QString::fromStdString(*page.commitOperationId))
                    : QJsonValue(QJsonValue::Null)},
                {"total", static_cast<int>(page.total)},
                {"actions", actions},
                {"nextOffset", page.nextOffset
                    ? QJsonValue(static_cast<int>(*page.nextOffset))
                    : QJsonValue(QJsonValue::Null)}}};
        });
    });
    server.Get("/dvmdb/v1/projects/:id/branches/:branch/history",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            std::size_t limit = 100;
            if (const std::optional<std::string> value = queryValue(request, "limit")) {
                bool ok = false;
                const unsigned number = QString::fromStdString(*value).toUInt(&ok);
                requireInput(ok && number > 0 && number <= 1000, "invalid history limit");
                limit = number;
            }
            std::optional<RevisionId> after;
            if (const std::optional<std::string> value = queryValue(request, "after"))
                after = parseRevisionId(*value);
            QJsonArray revisions;
            for (const RevisionGraphRevision& revision : service.history(
                    pathParam(request, "id"), pathParam(request, "branch"), limit, after))
                revisions.append(revisionJson(revision));
            return Reply{200, QJsonObject{{"revisions", revisions}}};
        });
    });
    server.Get("/dvmdb/v1/projects/:id/revisions/:revision",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const std::string projectId = pathParam(request, "id");
            const RevisionId revisionId = parseRevisionId(pathParam(request, "revision"));
            const std::optional<ProjectId> project = ProjectId::parse(projectId);
            requireInput(project.has_value(), "project must be UUIDv7");
            std::optional<RevisionRecord> record;
            service.withRepository(projectId, [&](Repository& repository) {
                const std::optional<Bytes> bytes = repository.readRevision(*project, revisionId);
                if (bytes) record = decodeRevisionRecord(*bytes);
            });
            if (!record) throw ProjectError(ProjectErrorKind::NotFound, "revision not found");
            const Hash digest = hashRevisionRecord(*record);
            RevisionGraphRevision described{record->id, digest, record->root,
                                              record->referencesRoot, record->modelIndexed, {},
                                              record->metadata, record->committedAtMs, std::nullopt};
            for (const RevisionParent& parent : record->parents)
                described.parents.push_back({parent.id, parent.expectedRecordHash});
            described.displayMessage = service.revisionDisplayMessage(projectId,
                revisionId.toString());
            return Reply{200, revisionJson(described)};
        });
    });
    // This changes only the human-facing message annotation, never the
    // cryptographically authenticated revision record or its original message.
    server.Patch("/dvmdb/v1/projects/:id/revisions/:revision/message",
                 [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            if (const std::optional<Reply> pin = repositoryPinFailure(request, service))
                return *pin;
            const QJsonObject object = parseBody(request, maxJsonMergeBytes);
            requireKeys(object, {"expectedMessage", "message"});
            const std::string project = pathParam(request, "id");
            const std::string revision = pathParam(request, "revision");
            const std::string expected = requiredString(object, "expectedMessage");
            const std::string message = requiredString(object, "message");
            service.updateRevisionDisplayMessage(project, revision, expected, message);
            return Reply{200, QJsonObject{{"revision", QString::fromStdString(revision)},
                {"message", QString::fromStdString(message)}}};
        });
    });
    server.Get("/dvmdb/v1/projects/:id/revision-graph",
               [&service](const httplib::Request& request, httplib::Response& response) {
        guarded(response, [&]() {
            const std::string project = pathParam(request, "id");
            std::size_t limit = 100;
            if (const std::optional<std::string> value = queryValue(request, "limit")) {
                bool ok = false;
                const unsigned number = QString::fromStdString(*value).toUInt(&ok);
                requireInput(ok && number > 0 && number <= 1000, "invalid graph limit");
                limit = number;
            }
            std::optional<RevisionGraphCursor> cursor;
            if (const std::optional<std::string> value = queryValue(request, "cursor"))
                cursor = readGraphToken(*value, project);
            const RevisionGraphPage page = service.revisionGraph(project, limit, std::move(cursor));
            QJsonArray heads;
            for (const RevisionGraphHead& head : page.heads)
                heads.append(branchHeadJson(head));
            QJsonArray revisions;
            for (const RevisionGraphRevision& revision : page.revisions)
                revisions.append(revisionJson(revision));
            return Reply{200, QJsonObject{{"heads", heads}, {"revisions", revisions},
                {"complete", !page.next.has_value()},
                {"nextCursor", page.next ? QJsonValue(graphToken(*page.next, project))
                                         : QJsonValue(QJsonValue::Null)}}};
        });
    });
    // This is a generic native ModelRecord batch, not the high-level hydraulic
    // editor: the caller supplies opaque model payload bytes. The native store
    // enforces reference integrity but does not decode hydraulic field semantics.
    server.Post(routeModelCommits, [&service](const httplib::Request& request,
                                                 httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject object = parseBody(request, maxJsonModelBytes);
            requireKeys(object, {"expectedHead", "changes", "author", "message"});
            const RevisionId expected = parseRevisionId(requiredString(object, "expectedHead"));
            const CommitMetadata metadata = parseCommitMetadata(object);
            std::vector<ModelChange> changes = parseModelChanges(object, true);
            const RevisionId saved = service.commitModel(pathParam(request, "id"),
                                           pathParam(request, "branch"), std::move(changes),
                                           metadata, expected);
            return Reply{201, QJsonObject{{"id", revisionIdString(saved)}}};
        });
    });
    server.Post(routeMerges, [&service](const httplib::Request& request,
                                          httplib::Response& response) {
        guarded(response, [&]() {
            const QJsonObject object = parseBody(request, maxJsonMergeBytes);
            requireKeys(object, {"target", "source", "expectedTargetHead", "expectedSourceHead",
                                 "author", "message"});
            const std::string target = requiredString(object, "target");
            const std::string source = requiredString(object, "source");
            requireInput(target != source, "merge branches must differ");
            const CommitMetadata metadata =
                parseCommitMetadata(object, "Merge " + source + " into " + target);
            const RevisionId expectedTarget = parseRevisionId(requiredString(object, "expectedTargetHead"));
            const RevisionId expectedSource = parseRevisionId(requiredString(object, "expectedSourceHead"));
            const MergeResult result = service.mergeBranches(pathParam(request, "id"),
                                             target, source, metadata, expectedTarget, expectedSource);
            return Reply{result.status == MergeStatus::Conflicts ? 409 : 200, mergeJson(result)};
        });
    });
    server.Get("/dvmdb/v1/projects/:id/diff", [&service](const httplib::Request& request,
                                                            httplib::Response& response) {
        guarded(response, [&]() {
            const std::optional<std::string> from = queryValue(request, "from");
            const std::optional<std::string> to = queryValue(request, "to");
            requireInput(from && to, "diff requires from and to UUIDv7 revisions");
            const RevisionId before = parseRevisionId(*from);
            const RevisionId after = parseRevisionId(*to);
            const std::string project = pathParam(request, "id");
            const std::optional<ProjectId> projectId = ProjectId::parse(project);
            requireInput(projectId.has_value(), "project ID must be UUIDv7");
            QJsonArray changes;
            service.withRepository(project, [&](Repository& repository) {
                for (const Difference& difference : repository.diffRevisions(
                        *projectId, before, after, maxHttpDiffRecords)) {
                    changes.append(QJsonObject{{"uuid", modelKeyString(difference.key)},
                        {"before", optionalHashJson(difference.before)},
                        {"after", optionalHashJson(difference.after)}});
                }
            });
            return Reply{200, QJsonObject{{"from", revisionIdString(before)},
                {"to", revisionIdString(after)}, {"changes", changes}}};
        });
    });
#ifdef AOWIS_DVMDB_HTTP_WITH_HYDRAULIC
    registerHydraulicRoutes(server, service);
#endif
}

} // namespace aowis::dvmdb::http
