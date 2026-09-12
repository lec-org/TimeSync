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

struct SystemTimeHelperSession {
    quintptr processHandle = 0;
    SystemTimeResult launchResult;
    bool launched = false;
};

class WindowsSystemClock final {
public:
    // SetSystemTime is verified against a monotonic anchor within this tolerance.
    static constexpr qint64 VerificationToleranceMs = 2000;
    static constexpr int HelperProcessTimeoutMs = 5000;

    [[nodiscard]] static Result<QDateTime> readUtc();
    [[nodiscard]] static qint64 utcUnixMilliseconds();
    [[nodiscard]] static TimeZoneSnapshot queryTimeZone();
    [[nodiscard]] static SystemTimeResult setUtc(
        const QDateTime &targetUtc,
        const std::function<bool()> &cancelRequested = {});
    [[nodiscard]] static SystemTimeHelperSession launchSetUtcHelper(const QString &executablePath,
                                                                    const QDateTime &targetUtc);
    [[nodiscard]] static SystemTimeResult waitSetUtcHelper(SystemTimeHelperSession &session);
    [[nodiscard]] static SystemTimeResult setUtcViaHelperProcess(const QString &executablePath,
                                                                 const QDateTime &targetUtc);
    static int runSetSystemTimeCommand(qint64 utcUnixMilliseconds);
};

} // namespace TimeSync
