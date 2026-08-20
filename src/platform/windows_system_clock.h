#pragma once

#include "../core/result.h"

#include <QDateTime>

#include <functional>

namespace TimeSync {

enum class SystemTimeFailure {
    None = 0,
    InvalidInput,
    PrivilegeMissing,
    PrivilegeAdjustmentFailed,
    ApiRejected,
    VerificationFailed,
    Cancelled,
    UncertainState,
    UnsupportedPlatform,
};

struct SystemTimeResult {
    bool succeeded = false;
    SystemTimeFailure failure = SystemTimeFailure::None;
    quint32 nativeCode = 0;
    bool uncertain = false;
};

class WindowsSystemClock final {
public:
    // SetSystemTime is verified against a monotonic anchor within this tolerance.
    static constexpr qint64 VerificationToleranceMs = 2000;

    [[nodiscard]] static Result<QDateTime> readUtc();
    [[nodiscard]] static SystemTimeResult setUtc(
        const QDateTime &targetUtc,
        const std::function<bool()> &cancelRequested = {});
};

} // namespace TimeSync
