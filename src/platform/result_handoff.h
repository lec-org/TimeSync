#pragma once

#include "../core/result.h"

#include <QString>

namespace TimeSync {

struct HandoffFile {
    QString path;
    QString nonce;
};

struct HandoffPayload {
    ExitCode exitCode = ExitCode::OtherFailure;
    ErrorCode errorCode = ErrorCode::InternalError;
    bool uncertain = false;
};

class ResultHandoff final {
public:
    static constexpr int SchemaVersion = 1;

    [[nodiscard]] static Result<HandoffFile> create();
    [[nodiscard]] static Result<void> write(const HandoffFile &file, const HandoffPayload &payload);
    [[nodiscard]] static Result<HandoffPayload> readAndRemove(const HandoffFile &file);

private:
    [[nodiscard]] static bool isValidNonce(const QString &nonce);
    [[nodiscard]] static bool isExpectedPath(const HandoffFile &file);
};

} // namespace TimeSync
