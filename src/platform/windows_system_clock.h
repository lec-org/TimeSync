#pragma once

#include "../core/result.h"

#include <QDateTime>
#include <QElapsedTimer>

#include <functional>

namespace TimeSync {

enum class SystemTimeFailure {
    None = 0,
    InvalidInput,
    PrivilegeMissing,
    PrivilegeAdjustmentFailed,
    ApiRejected,
    AccessDenied,
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

struct TimeZoneSnapshot {
    int offsetSeconds = 0;
    QString abbreviation;
};

class WindowsSystemClock final {
public:
    // SetSystemTime is verified against a monotonic anchor within this tolerance.
    static constexpr qint64 VerificationToleranceMs = 2000;

    [[nodiscard]] static Result<QDateTime> readUtc();
    [[nodiscard]] static qint64 utcUnixMilliseconds();
    [[nodiscard]] static TimeZoneSnapshot queryTimeZone();
    [[nodiscard]] static SystemTimeResult setUtc(
        const QDateTime &baseUtc,
        const std::function<bool()> &cancelRequested = {},
        const QElapsedTimer *sinceCapture = nullptr);
};

} // namespace TimeSync
