#pragma once

#include "result.h"

#include <QMetaType>
#include <QString>

namespace TimeSync {

enum class OperationKind {
    Refresh = 0,
    ManualSync,
    ScheduledSync,
    InstallTask,
    RemoveTask,
};

enum class OperationFailure {
    None = 0,
    Busy,
    RemoteTimeUnavailable,
    ServerUnavailable,
    Cancelled,
    InvalidConfiguration,
    PrivilegeMissing,
    AuthorizationDenied,
    SystemTimeRejected,
    VerificationFailed,
    UncertainState,
    ScheduleUpdateFailed,
    ScheduleRemoveFailed,
    UnsupportedPlatform,
    Other,
};

struct OperationResult {
    quint64 generation = 0;
    OperationKind operation = OperationKind::Refresh;
    bool succeeded = false;
    OperationFailure failure = OperationFailure::None;
    ExitCode exitCode = ExitCode::Success;
    QString source;
    bool uncertain = false;
};

[[nodiscard]] OperationFailure operationFailureForError(ErrorCode code) noexcept;
[[nodiscard]] OperationFailure operationFailureForSystemTimeFailure(int failureValue) noexcept;

} // namespace TimeSync

Q_DECLARE_METATYPE(TimeSync::OperationResult)
