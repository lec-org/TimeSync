#include "windows_system_clock.h"

#include "elevation_broker.h"

#include <QElapsedTimer>
#include <QTimeZone>

#include <string>
#include <thread>

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

int helperExitCode(const SystemTimeResult &result)
{
    if (result.succeeded) {
        return 0;
    }
    return static_cast<int>(result.failure) + 1;
}

SystemTimeResult resultFromHelperExitCode(const DWORD exitCode)
{
    if (exitCode == 0) {
        return {true, SystemTimeFailure::None, 0, false};
    }
    const int failureValue = static_cast<int>(exitCode) - 1;
    if (failureValue < static_cast<int>(SystemTimeFailure::None)
        || failureValue > static_cast<int>(SystemTimeFailure::UnsupportedPlatform)) {
        return failure(SystemTimeFailure::UncertainState, exitCode, true);
    }
    const auto kind = static_cast<SystemTimeFailure>(failureValue);
    return failure(kind, 0, kind == SystemTimeFailure::UncertainState);
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

SystemTimeResult WindowsSystemClock::setUtc(const QDateTime &targetUtc,
                                            const std::function<bool()> &cancelRequested)
{
#ifndef Q_OS_WIN
    Q_UNUSED(targetUtc)
    Q_UNUSED(cancelRequested)
    return failure(SystemTimeFailure::UnsupportedPlatform);
#else
    if (!targetUtc.isValid() || targetUtc.timeSpec() != Qt::UTC) {
        return failure(SystemTimeFailure::InvalidInput);
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

    SYSTEMTIME systemTime{};
    if (!toSystemTime(targetUtc, &systemTime)) {
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
    const QDateTime expected = targetUtc.addMSecs(verificationAnchor.elapsed());
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

SystemTimeHelperSession WindowsSystemClock::launchSetUtcHelper(const QString &executablePath,
                                                               const QDateTime &targetUtc)
{
    SystemTimeHelperSession session;
#ifndef Q_OS_WIN
    Q_UNUSED(executablePath)
    Q_UNUSED(targetUtc)
    session.launchResult = failure(SystemTimeFailure::UnsupportedPlatform);
    return session;
#else
    if (executablePath.isEmpty() || !targetUtc.isValid()) {
        session.launchResult = failure(SystemTimeFailure::InvalidInput);
        return session;
    }
    const QDateTime utc = targetUtc.toUTC();
    const QString command = QStringLiteral("%1 --set-system-time %2")
                                .arg(ElevationBroker::quoteWindowsArgument(executablePath),
                                     QString::number(utc.toMSecsSinceEpoch()));
    std::wstring mutableCommand = command.toStdWString();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr,
                        mutableCommand.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                        nullptr,
                        nullptr,
                        &startup,
                        &process)) {
        session.launchResult = failure(SystemTimeFailure::ApiRejected, GetLastError());
        return session;
    }
    CloseHandle(process.hThread);
    session.processHandle = reinterpret_cast<quintptr>(process.hProcess);
    session.launched = true;
    session.launchResult = {true, SystemTimeFailure::None, 0, false};
    return session;
#endif
}

SystemTimeResult WindowsSystemClock::waitSetUtcHelper(SystemTimeHelperSession &session)
{
#ifndef Q_OS_WIN
    Q_UNUSED(session)
    return failure(SystemTimeFailure::UnsupportedPlatform);
#else
    if (!session.launched || session.processHandle == 0) {
        return session.launchResult.succeeded
            ? failure(SystemTimeFailure::ApiRejected)
            : session.launchResult;
    }
    HANDLE process = reinterpret_cast<HANDLE>(session.processHandle);
    session.processHandle = 0;
    const DWORD waited = WaitForSingleObject(process, static_cast<DWORD>(HelperProcessTimeoutMs));
    DWORD exitCode = 0;
    SystemTimeResult result = failure(SystemTimeFailure::UncertainState, 0, true);
    if (waited == WAIT_OBJECT_0 && GetExitCodeProcess(process, &exitCode)) {
        result = resultFromHelperExitCode(exitCode);
        CloseHandle(process);
    } else if (waited == WAIT_TIMEOUT) {
        result = failure(SystemTimeFailure::UncertainState, WAIT_TIMEOUT, true);
        std::thread([process]() {
            WaitForSingleObject(process, INFINITE);
            CloseHandle(process);
        }).detach();
    } else {
        result = failure(SystemTimeFailure::UncertainState, GetLastError(), true);
        CloseHandle(process);
    }
    return result;
#endif
}

SystemTimeResult WindowsSystemClock::setUtcViaHelperProcess(const QString &executablePath,
                                                            const QDateTime &targetUtc)
{
    SystemTimeHelperSession session = launchSetUtcHelper(executablePath, targetUtc);
    if (!session.launched) {
        return session.launchResult;
    }
    return waitSetUtcHelper(session);
}

int WindowsSystemClock::runSetSystemTimeCommand(const qint64 utcUnixMilliseconds)
{
    const QDateTime utc = QDateTime::fromMSecsSinceEpoch(utcUnixMilliseconds, QTimeZone(QTimeZone::UTC));
    const SystemTimeResult result = setUtc(utc);
#ifdef Q_OS_WIN
    return helperExitCode(result);
#else
    Q_UNUSED(result)
    return helperExitCode(failure(SystemTimeFailure::UnsupportedPlatform));
#endif
}

} // namespace TimeSync
