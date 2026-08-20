#include "result_handoff.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>

#include <cmath>

namespace TimeSync {
namespace {

Error handoffError(const QString &detail, const ErrorCode code = ErrorCode::InternalError)
{
    return {code, detail};
}

QString makeNonce()
{
    QString nonce;
    nonce.reserve(32);
    for (int index = 0; index < 2; ++index) {
        const quint64 value = QRandomGenerator::system()->generate64();
        nonce += QStringLiteral("%1").arg(value, 16, 16, QLatin1Char('0'));
    }
    return nonce;
}

bool validExitCode(const int value)
{
    return value == static_cast<int>(ExitCode::Success)
        || value == static_cast<int>(ExitCode::ArgumentOrConfiguration)
        || value == static_cast<int>(ExitCode::RemoteTimeFailure)
        || value == static_cast<int>(ExitCode::PermissionFailure)
        || value == static_cast<int>(ExitCode::VerificationFailure)
        || value == static_cast<int>(ExitCode::OtherFailure);
}

bool validErrorCode(const int value)
{
    return value >= static_cast<int>(ErrorCode::None)
        && value <= static_cast<int>(ErrorCode::InternalError);
}

bool validJsonInteger(const QJsonValue &value, const int minimum, const int maximum)
{
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    return std::isfinite(number) && std::floor(number) == number && number >= minimum && number <= maximum;
}

} // namespace

bool ResultHandoff::isValidNonce(const QString &nonce)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}$"));
    return pattern.match(nonce).hasMatch();
}

bool ResultHandoff::isExpectedPath(const HandoffFile &file)
{
    if (!isValidNonce(file.nonce)) {
        return false;
    }
    const QString expectedName = QStringLiteral("TimeSync-%1.result.json").arg(file.nonce);
    const QFileInfo info(file.path);
    const QFileInfo tempInfo(QDir::tempPath());
    return info.isAbsolute() && info.fileName() == expectedName
        && info.absolutePath() == tempInfo.absoluteFilePath();
}

Result<HandoffFile> ResultHandoff::create()
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        HandoffFile file;
        file.nonce = makeNonce();
        file.path = QDir::temp().filePath(QStringLiteral("TimeSync-%1.result.json").arg(file.nonce));
        if (!QFile::exists(file.path)) {
            return Result<HandoffFile>::success(std::move(file));
        }
    }
    return Result<HandoffFile>::failure(handoffError(QStringLiteral("cannot allocate handoff nonce")));
}

Result<void> ResultHandoff::write(const HandoffFile &file, const HandoffPayload &payload)
{
    if (!isExpectedPath(file)) {
        return Result<void>::failure(handoffError(QStringLiteral("invalid handoff path"), ErrorCode::InvalidArgument));
    }
    const int exitValue = static_cast<int>(payload.exitCode);
    if (!validExitCode(exitValue) || !validErrorCode(static_cast<int>(payload.errorCode))) {
        return Result<void>::failure(handoffError(QStringLiteral("invalid handoff payload"), ErrorCode::InvalidArgument));
    }

    QJsonObject object;
    object.insert(QStringLiteral("schema"), QStringLiteral("TimeSync.Result"));
    object.insert(QStringLiteral("version"), SchemaVersion);
    object.insert(QStringLiteral("nonce"), file.nonce);
    object.insert(QStringLiteral("exitCode"), exitValue);
    object.insert(QStringLiteral("errorCode"), static_cast<int>(payload.errorCode));
    object.insert(QStringLiteral("uncertain"), payload.uncertain);
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);

    QSaveFile output(file.path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)
        || output.write(bytes) != bytes.size() || !output.commit()) {
        return Result<void>::failure(handoffError(QStringLiteral("cannot atomically write handoff")));
    }
    QFile::setPermissions(file.path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return Result<void>::success();
}

Result<HandoffPayload> ResultHandoff::readAndRemove(const HandoffFile &file)
{
    if (!isExpectedPath(file)) {
        return Result<HandoffPayload>::failure(handoffError(QStringLiteral("invalid handoff path"), ErrorCode::InvalidArgument));
    }

    QFile input(file.path);
    if (!input.open(QIODevice::ReadOnly) || input.size() > 64 * 1024) {
        QFile::remove(file.path);
        return Result<HandoffPayload>::failure(handoffError(QStringLiteral("cannot read handoff")));
    }
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(input.readAll(), &parseError);
    input.close();
    const QJsonObject object = document.object();
    const bool valid = parseError.error == QJsonParseError::NoError && document.isObject()
        && object.value(QStringLiteral("schema")).toString() == QStringLiteral("TimeSync.Result")
        && validJsonInteger(object.value(QStringLiteral("version")), SchemaVersion, SchemaVersion)
        && object.value(QStringLiteral("nonce")).toString() == file.nonce
        && validJsonInteger(object.value(QStringLiteral("exitCode")),
                            static_cast<int>(ExitCode::Success),
                            static_cast<int>(ExitCode::OtherFailure))
        && validExitCode(object.value(QStringLiteral("exitCode")).toInt())
        && validJsonInteger(object.value(QStringLiteral("errorCode")),
                            static_cast<int>(ErrorCode::None),
                            static_cast<int>(ErrorCode::InternalError))
        && validErrorCode(object.value(QStringLiteral("errorCode")).toInt())
        && object.value(QStringLiteral("uncertain")).isBool();
    const bool removed = QFile::remove(file.path);
    if (!removed) {
        return Result<HandoffPayload>::failure(handoffError(QStringLiteral("cannot remove handoff")));
    }
    if (!valid) {
        return Result<HandoffPayload>::failure(handoffError(QStringLiteral("invalid handoff payload")));
    }

    HandoffPayload payload;
    payload.exitCode = static_cast<ExitCode>(object.value(QStringLiteral("exitCode")).toInt());
    payload.errorCode = static_cast<ErrorCode>(object.value(QStringLiteral("errorCode")).toInt());
    payload.uncertain = object.value(QStringLiteral("uncertain")).toBool();
    return Result<HandoffPayload>::success(std::move(payload));
}

} // namespace TimeSync
