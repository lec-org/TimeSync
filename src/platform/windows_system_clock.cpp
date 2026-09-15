#include "windows_system_clock.h"

#include <QElapsedTimer>
#include <QTimeZone>

#ifdef Q_OS_WIN
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

namespace TimeSync {
namespace {

SystemTimeResult failure(const SystemTimeFailure kind, const quint32 nativeCode = 0, const bool uncertain = false)
{
    return {false, kind, nativeCode, uncertain};
}

#ifdef Q_OS_WIN
constexpr quint64 WindowsEpochOffsetMs = 11644473600000ULL;

qint64 fileTimeToUnixMilliseconds(const FILETIME &fileTime)
{
    ULARGE_INTEGER value{};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    const quint64 millisecondsSinceWindowsEpoch = value.QuadPart / 10000ULL;
    if (millisecondsSinceWindowsEpoch < WindowsEpochOffsetMs) {
        return 0;
    }
    return static_cast<qint64>(millisecondsSinceWindowsEpoch - WindowsEpochOffsetMs);
}

Result<QDateTime> readWindowsUtc()
{
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);
    const qint64 unixMs = fileTimeToUnixMilliseconds(fileTime);
    if (unixMs <= 0) {
        return Result<QDateTime>::failure({ErrorCode::InternalError, QStringLiteral("invalid Windows time")});
    }
    return Result<QDateTime>::success(
        QDateTime::fromMSecsSinceEpoch(unixMs, QTimeZone(QTimeZone::UTC)));
}

Result<void> enableSystemTimePrivilege(quint32 *nativeCode)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        *nativeCode = GetLastError();
        return Result<void>::failure({ErrorCode::PrivilegeMissing,
                                      QStringLiteral("cannot open process token"),
                                      *nativeCode});
    }

    LUID privilege{};
    if (!LookupPrivilegeValueW(nullptr, SE_SYSTEMTIME_NAME, &privilege)) {
        *nativeCode = GetLastError();
        CloseHandle(token);
        return Result<void>::failure(
            {ErrorCode::PrivilegeAdjustmentFailed, QStringLiteral("cannot resolve system-time privilege"), *nativeCode});
    }

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = privilege;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const BOOL adjusted = AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
    *nativeCode = GetLastError();
    CloseHandle(token);
    if (!adjusted) {
        return Result<void>::failure(
            {ErrorCode::PrivilegeAdjustmentFailed, QStringLiteral("cannot enable system-time privilege"), *nativeCode});
    }
    if (*nativeCode == ERROR_NOT_ALL_ASSIGNED) {
        return Result<void>::failure(
            {ErrorCode::PrivilegeMissing, QStringLiteral("system-time privilege is not assigned"), *nativeCode});
    }
    return Result<void>::success();
}

bool toSystemTime(const QDateTime &utc, SYSTEMTIME *systemTime)
{
    const QDateTime normalized = utc.toUTC();
    if (!normalized.isValid() || normalized.date().year() < 1601 || normalized.date().year() > 9999) {
        return false;
    }
    const QDate date = normalized.date();
    const QTime time = normalized.time();
    systemTime->wYear = static_cast<WORD>(date.year());
    systemTime->wMonth = static_cast<WORD>(date.month());
    systemTime->wDayOfWeek = static_cast<WORD>(date.dayOfWeek() % 7);
    systemTime->wDay = static_cast<WORD>(date.day());
    systemTime->wHour = static_cast<WORD>(time.hour());
    systemTime->wMinute = static_cast<WORD>(time.minute());
    systemTime->wSecond = static_cast<WORD>(time.second());
    systemTime->wMilliseconds = static_cast<WORD>(time.msec());
    return true;
}
#endif

} // namespace

Result<QDateTime> WindowsSystemClock::readUtc()
{
#ifdef Q_OS_WIN
    return readWindowsUtc();
#else
    return Result<QDateTime>::failure({ErrorCode::UnsupportedPlatform, QStringLiteral("Windows system clock unavailable")});
#endif
}

SystemTimeResult WindowsSystemClock::setUtc(const QDateTime &baseUtc,
                                            const std::function<bool()> &cancelRequested,
                                            const QElapsedTimer *sinceCapture)
{
#ifndef Q_OS_WIN
    Q_UNUSED(baseUtc)
    Q_UNUSED(cancelRequested)
    Q_UNUSED(sinceCapture)
    return failure(SystemTimeFailure::UnsupportedPlatform);
#else
    if (!baseUtc.isValid() || baseUtc.timeSpec() != Qt::UTC) {
        return failure(SystemTimeFailure::InvalidInput);
    }
    QElapsedTimer localCapture;
    if (sinceCapture == nullptr) {
        localCapture.start();
        sinceCapture = &localCapture;
    }
    if (cancelRequested && cancelRequested()) {
        return failure(SystemTimeFailure::Cancelled);
    }

    quint32 nativeCode = 0;
    const Result<void> privilege = enableSystemTimePrivilege(&nativeCode);
    if (!privilege) {
        return failure(privilege.error().code == ErrorCode::PrivilegeMissing
                           ? SystemTimeFailure::PrivilegeMissing
                           : SystemTimeFailure::PrivilegeAdjustmentFailed,
                       nativeCode);
    }
    if (cancelRequested && cancelRequested()) {
        return failure(SystemTimeFailure::Cancelled);
    }

    const QDateTime toWrite = baseUtc.addMSecs(sinceCapture->elapsed());
    SYSTEMTIME systemTime{};
    if (!toSystemTime(toWrite, &systemTime)) {
        return failure(SystemTimeFailure::InvalidInput);
    }

    QElapsedTimer verificationAnchor;
    verificationAnchor.start();
    if (!SetSystemTime(&systemTime)) {
        nativeCode = GetLastError();
        if (nativeCode == ERROR_PRIVILEGE_NOT_HELD) {
            return failure(SystemTimeFailure::PrivilegeMissing, nativeCode);
        }
        if (nativeCode == ERROR_ACCESS_DENIED) {
            return failure(SystemTimeFailure::AccessDenied, nativeCode);
        }
        return failure(SystemTimeFailure::ApiRejected, nativeCode);
    }

    if (cancelRequested && cancelRequested()) {
        return failure(SystemTimeFailure::UncertainState, ERROR_CANCELLED, true);
    }

    const Result<QDateTime> actual = readWindowsUtc();
    if (!actual) {
        return failure(SystemTimeFailure::UncertainState, actual.error().nativeCode, true);
    }
    const QDateTime expected = toWrite.addMSecs(verificationAnchor.elapsed());
    const qint64 difference = qAbs(actual.value().msecsTo(expected));
    if (difference > VerificationToleranceMs) {
        return failure(SystemTimeFailure::VerificationFailed, 0, false);
    }
    return {true, SystemTimeFailure::None, 0, false};
#endif
}

qint64 WindowsSystemClock::utcUnixMilliseconds()
{
#ifdef Q_OS_WIN
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);
    return fileTimeToUnixMilliseconds(fileTime);
#else
    return QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
#endif
}

TimeZoneSnapshot WindowsSystemClock::queryTimeZone()
{
    TimeZoneSnapshot snapshot;
#ifdef Q_OS_WIN
    TIME_ZONE_INFORMATION info{};
    const DWORD zoneId = GetTimeZoneInformation(&info);
    LONG biasMinutes = info.Bias;
    const wchar_t *name = info.StandardName;
    if (zoneId == TIME_ZONE_ID_DAYLIGHT) {
        biasMinutes += info.DaylightBias;
        name = info.DaylightName;
    } else if (zoneId == TIME_ZONE_ID_STANDARD) {
        biasMinutes += info.StandardBias;
    }
    snapshot.offsetSeconds = static_cast<int>(-biasMinutes * 60);
    snapshot.abbreviation = QString::fromWCharArray(name).trimmed();
#else
    const QDateTime local = QDateTime::currentDateTime();
    snapshot.offsetSeconds = local.offsetFromUtc();
    snapshot.abbreviation = local.timeZoneAbbreviation().trimmed();
#endif
    return snapshot;
}

} // namespace TimeSync
