#include "elevation_broker.h"

#include <QElapsedTimer>
#include <QFileInfo>

#ifdef Q_OS_WIN
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#    include <shellapi.h>
#endif

namespace TimeSync {
namespace {

Error brokerError(const ErrorCode code, const QString &detail, const quint32 nativeCode = 0, const bool uncertain = false)
{
    return {code, detail, nativeCode, uncertain};
}

} // namespace

QString ElevationBroker::quoteWindowsArgument(const QString &argument)
{
    if (argument.isEmpty()) {
        return QStringLiteral("\"\"");
    }

    QString result;
    result.reserve(argument.size() + 2);
    result += QLatin1Char('"');
    int backslashes = 0;
    for (const QChar character : argument) {
        if (character == QLatin1Char('\\')) {
            ++backslashes;
            continue;
        }
        if (character == QLatin1Char('"')) {
            result += QString(backslashes * 2 + 1, QLatin1Char('\\'));
            result += QLatin1Char('"');
            backslashes = 0;
            continue;
        }
        if (backslashes > 0) {
            result += QString(backslashes, QLatin1Char('\\'));
            backslashes = 0;
        }
        result += character;
    }
    if (backslashes > 0) {
        result += QString(backslashes * 2, QLatin1Char('\\'));
    }
    result += QLatin1Char('"');
    return result;
}

Result<bool> ElevationBroker::isProcessElevated()
{
#ifndef Q_OS_WIN
    return Result<bool>::failure(
        brokerError(ErrorCode::UnsupportedPlatform, QStringLiteral("UAC elevation is only available on Windows")));
#else
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return Result<bool>::failure(
            brokerError(ErrorCode::ElevationFailed, QStringLiteral("cannot query process elevation"), GetLastError()));
    }

    TOKEN_ELEVATION elevation{};
    DWORD returnedLength = 0;
    const BOOL queried = GetTokenInformation(token,
                                             TokenElevation,
                                             &elevation,
                                             sizeof(elevation),
                                             &returnedLength);
    const DWORD nativeCode = queried ? ERROR_SUCCESS : GetLastError();
    CloseHandle(token);
    if (!queried) {
        return Result<bool>::failure(
            brokerError(ErrorCode::ElevationFailed, QStringLiteral("cannot query process elevation"), nativeCode));
    }
    return Result<bool>::success(elevation.TokenIsElevated != 0);
#endif
}

Result<void> ElevationBroker::relaunchAsAdministrator(const QString &executablePath, const QStringList &arguments)
{
#ifndef Q_OS_WIN
    Q_UNUSED(executablePath)
    Q_UNUSED(arguments)
    return Result<void>::failure(
        brokerError(ErrorCode::UnsupportedPlatform, QStringLiteral("UAC elevation is only available on Windows")));
#else
    const QFileInfo executable(QFileInfo(executablePath).absoluteFilePath());
    if (!executable.isAbsolute() || !executable.exists() || !executable.isFile()) {
        return Result<void>::failure(
            brokerError(ErrorCode::InvalidArgument, QStringLiteral("invalid elevation executable")));
    }

    QStringList quotedArguments;
    quotedArguments.reserve(arguments.size());
    for (const QString &argument : arguments) {
        quotedArguments.append(quoteWindowsArgument(argument));
    }
    const std::wstring fileName = executable.absoluteFilePath().toStdWString();
    const std::wstring parameters = quotedArguments.join(QLatin1Char(' ')).toStdWString();

    SHELLEXECUTEINFOW executeInfo{};
    executeInfo.cbSize = sizeof(executeInfo);
    executeInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    executeInfo.lpVerb = L"runas";
    executeInfo.lpFile = fileName.c_str();
    executeInfo.lpParameters = parameters.c_str();
    executeInfo.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&executeInfo)) {
        const DWORD nativeCode = GetLastError();
        return Result<void>::failure(
            brokerError(nativeCode == ERROR_CANCELLED ? ErrorCode::ElevationCancelled : ErrorCode::ElevationFailed,
                        QStringLiteral("elevated process launch failed"),
                        nativeCode));
    }

    if (executeInfo.hProcess != nullptr) {
        CloseHandle(executeInfo.hProcess);
    }
    return Result<void>::success();
#endif
}

Result<int> ElevationBroker::runAsAdministrator(const QString &executablePath,
                                                const QStringList &arguments,
                                                const int timeoutMs,
                                                const std::function<bool()> &cancelRequested)
{
#ifndef Q_OS_WIN
    Q_UNUSED(executablePath)
    Q_UNUSED(arguments)
    Q_UNUSED(timeoutMs)
    Q_UNUSED(cancelRequested)
    return Result<int>::failure(brokerError(ErrorCode::UnsupportedPlatform,
                                            QStringLiteral("UAC elevation is only available on Windows")));
#else
    const QFileInfo executable(QFileInfo(executablePath).absoluteFilePath());
    if (!executable.isAbsolute() || !executable.exists() || !executable.isFile() || timeoutMs < 1) {
        return Result<int>::failure(
            brokerError(ErrorCode::InvalidArgument, QStringLiteral("invalid elevation executable")));
    }

    QStringList quotedArguments;
    quotedArguments.reserve(arguments.size());
    for (const QString &argument : arguments) {
        quotedArguments.append(quoteWindowsArgument(argument));
    }
    const std::wstring fileName = executable.absoluteFilePath().toStdWString();
    const std::wstring parameters = quotedArguments.join(QLatin1Char(' ')).toStdWString();

    SHELLEXECUTEINFOW executeInfo{};
    executeInfo.cbSize = sizeof(executeInfo);
    executeInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    executeInfo.lpVerb = L"runas";
    executeInfo.lpFile = fileName.c_str();
    executeInfo.lpParameters = parameters.c_str();
    executeInfo.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&executeInfo)) {
        const DWORD nativeCode = GetLastError();
        return Result<int>::failure(
            brokerError(nativeCode == ERROR_CANCELLED ? ErrorCode::ElevationCancelled : ErrorCode::ElevationFailed,
                        QStringLiteral("elevated process launch failed"),
                        nativeCode));
    }

    // SEE_MASK_NOCLOSEPROCESS requests a handle, but Windows does not promise
    // to return one for every successful shell launch. Without a handle there
    // is no safe way to wait for or validate the elevated result handoff.
    if (executeInfo.hProcess == nullptr) {
        return Result<int>::failure(
            brokerError(ErrorCode::UncertainState,
                        QStringLiteral("elevated process handle unavailable"),
                        0,
                        true));
    }

    QElapsedTimer timer;
    timer.start();
    bool timedOut = false;
    while (true) {
        if (cancelRequested && cancelRequested()) {
            TerminateProcess(executeInfo.hProcess, ERROR_CANCELLED);
            WaitForSingleObject(executeInfo.hProcess, 2000);
            CloseHandle(executeInfo.hProcess);
            return Result<int>::failure(
                brokerError(ErrorCode::UncertainState, QStringLiteral("elevated process cancelled"), ERROR_CANCELLED, true));
        }
        const DWORD waitResult = WaitForSingleObject(executeInfo.hProcess, 50);
        if (waitResult == WAIT_OBJECT_0) {
            break;
        }
        if (waitResult == WAIT_FAILED) {
            const DWORD nativeCode = GetLastError();
            CloseHandle(executeInfo.hProcess);
            return Result<int>::failure(
                brokerError(ErrorCode::ElevationFailed, QStringLiteral("elevated process wait failed"), nativeCode));
        }
        if (timer.elapsed() >= timeoutMs) {
            timedOut = true;
            break;
        }
    }

    if (timedOut) {
        TerminateProcess(executeInfo.hProcess, ERROR_CANCELLED);
        WaitForSingleObject(executeInfo.hProcess, 2000);
        CloseHandle(executeInfo.hProcess);
        return Result<int>::failure(
            brokerError(ErrorCode::UncertainState, QStringLiteral("elevated process timed out"), WAIT_TIMEOUT, true));
    }

    DWORD exitValue = 0;
    if (!GetExitCodeProcess(executeInfo.hProcess, &exitValue)) {
        const DWORD nativeCode = GetLastError();
        CloseHandle(executeInfo.hProcess);
        return Result<int>::failure(
            brokerError(ErrorCode::ElevationFailed, QStringLiteral("cannot read elevated exit code"), nativeCode));
    }
    CloseHandle(executeInfo.hProcess);
    if (exitValue == STILL_ACTIVE) {
        return Result<int>::failure(
            brokerError(ErrorCode::ElevationFailed, QStringLiteral("invalid elevated exit code"), exitValue));
    }
    return Result<int>::success(static_cast<int>(exitValue));
#endif
}

} // namespace TimeSync
