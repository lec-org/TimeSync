#include "cli_runner.h"

#include "../core/trusted_clock.h"
#include "../platform/cross_process_mutex.h"
#include "../platform/task_scheduler.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

namespace TimeSync {
namespace {

Error runnerError(const ErrorCode code, const QString &detail, const quint32 nativeCode = 0, const bool uncertain = false)
{
    return {code, detail, nativeCode, uncertain};
}

QList<ServerEndpoint> endpointsFor(const AppConfig &config, Error *error)
{
    QList<ServerEndpoint> endpoints;
    endpoints.reserve(config.servers.size());
    for (const QString &server : config.servers) {
        const Result<ServerEndpoint> parsed = parseServerEndpoint(server);
        if (!parsed) {
            if (error != nullptr) {
                *error = parsed.error();
            }
            return {};
        }
        endpoints.append(parsed.value());
    }
    return endpoints;
}

HandoffPayload payloadFor(const ExitCode exitCode,
                          const ErrorCode errorCode,
                          const bool uncertain = false)
{
    return {exitCode, errorCode, uncertain};
}

ExitCode exitCodeForSystemResult(const SystemTimeResult &result)
{
    if (result.failure == SystemTimeFailure::VerificationFailed) {
        return ExitCode::VerificationFailure;
    }
    if (result.failure == SystemTimeFailure::PrivilegeMissing
        || result.failure == SystemTimeFailure::PrivilegeAdjustmentFailed) {
        return ExitCode::PermissionFailure;
    }
    if (result.failure == SystemTimeFailure::AccessDenied) {
        return ExitCode::PermissionFailure;
    }
    if (result.failure == SystemTimeFailure::UnsupportedPlatform) {
        return ExitCode::OtherFailure;
    }
    return exitCodeForError(result.uncertain ? ErrorCode::UncertainState : ErrorCode::SystemTimeRejected);
}

} // namespace

ErrorCode CliRunner::errorCodeForNtpFailure(const NtpFailureKind kind)
{
    switch (kind) {
    case NtpFailureKind::InvalidConfiguration:
        return ErrorCode::InvalidConfiguration;
    case NtpFailureKind::NetworkUnavailable:
        return ErrorCode::NetworkUnavailable;
    case NtpFailureKind::SourceTimeout:
        return ErrorCode::SourceTimeout;
    case NtpFailureKind::ProtocolRejected:
        return ErrorCode::ProtocolRejected;
    case NtpFailureKind::NoValidSource:
        return ErrorCode::NoValidSource;
    case NtpFailureKind::Cancelled:
        return ErrorCode::Cancelled;
    case NtpFailureKind::Busy:
        return ErrorCode::Busy;
    case NtpFailureKind::None:
        return ErrorCode::InternalError;
    case NtpFailureKind::InternalError:
        return ErrorCode::InternalError;
    }
    return ErrorCode::InternalError;
}

ErrorCode CliRunner::errorCodeForSystemFailure(const SystemTimeFailure failure)
{
    switch (failure) {
    case SystemTimeFailure::None:
        return ErrorCode::None;
    case SystemTimeFailure::InvalidInput:
        return ErrorCode::InvalidConfiguration;
    case SystemTimeFailure::PrivilegeMissing:
        return ErrorCode::PrivilegeMissing;
    case SystemTimeFailure::PrivilegeAdjustmentFailed:
        return ErrorCode::PrivilegeAdjustmentFailed;
    case SystemTimeFailure::ApiRejected:
    case SystemTimeFailure::AccessDenied:
        return ErrorCode::SystemTimeRejected;
    case SystemTimeFailure::VerificationFailed:
        return ErrorCode::VerificationFailed;
    case SystemTimeFailure::Cancelled:
        return ErrorCode::Cancelled;
    case SystemTimeFailure::UncertainState:
        return ErrorCode::UncertainState;
    case SystemTimeFailure::UnsupportedPlatform:
        return ErrorCode::UnsupportedPlatform;
    }
    return ErrorCode::InternalError;
}

Result<NtpSample> CliRunner::acquireFresh(const AppConfig &config) const
{
    Error endpointError;
    const QList<ServerEndpoint> endpoints = endpointsFor(config, &endpointError);
    if (endpoints.isEmpty()) {
        return Result<NtpSample>::failure(endpointError.isValid()
                                              ? endpointError
                                              : runnerError(ErrorCode::InvalidConfiguration,
                                                            QStringLiteral("no configured NTP source")));
    }

    NtpClient client;
    NtpQueryOptions options;
    options.perSourceTimeoutMs = 1200;
    options.globalTimeoutMs = 5000;
    const Result<quint64> started = client.start(endpoints, options);
    if (!started) {
        return Result<NtpSample>::failure(started.error());
    }

    QEventLoop loop;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    bool completed = false;
    NtpQueryResult queryResult;
    QObject::connect(&client, &NtpClient::finished, &loop, [&](quint64, const NtpQueryResult &result) {
        completed = true;
        queryResult = result;
        loop.quit();
    });
    QObject::connect(&watchdog, &QTimer::timeout, &loop, [&] { client.cancel(); });
    watchdog.start(options.globalTimeoutMs + 2000);
    loop.exec();
    if (!completed) {
        return Result<NtpSample>::failure(
            runnerError(ErrorCode::SourceTimeout, QStringLiteral("NTP acquisition did not complete")));
    }
    if (!queryResult.succeeded) {
        const ErrorCode code = errorCodeForNtpFailure(queryResult.failure.kind);
        return Result<NtpSample>::failure(
            runnerError(code,
                        QStringLiteral("NTP failure: %1; check DNS and outbound UDP/123")
                            .arg(ntpFailureDiagnostic(queryResult.failure))));
    }
    return Result<NtpSample>::success(queryResult.sample);
}

Result<HandoffPayload> CliRunner::runBroker(const HandoffFile &handoff,
                                            const QString &executablePath,
                                            const QStringList &arguments) const
{
    const Result<int> launched = ElevationBroker::runAsAdministrator(executablePath, arguments);
    if (!launched) {
        QFile::remove(handoff.path);
        return Result<HandoffPayload>::failure(launched.error());
    }
    const Result<HandoffPayload> payload = ResultHandoff::readAndRemove(handoff);
    if (!payload) {
        return payload;
    }
    if (static_cast<int>(payload.value().exitCode) != launched.value()) {
        return Result<HandoffPayload>::failure(
            runnerError(ErrorCode::UncertainState, QStringLiteral("elevated result exit mismatch"), 0, true));
    }
    return payload;
}

ExitCode CliRunner::runSync(const ParsedCommand &command,
                            const QString &executablePath,
                            const AppConfig &config,
                            QTextStream &out,
                            QTextStream &err) const
{
    const Result<NtpSample> acquired = acquireFresh(config);
    if (!acquired) {
        err << "Time acquisition failed: " << acquired.error().detail << "\n";
        return exitCodeForError(acquired.error().code);
    }
    out << "Source: " << acquired.value().source << "\n"
        << "UTC: " << acquired.value().utcAtReceive.toString(Qt::ISODateWithMs) << "\n";
    if (command.dryRun) {
        return ExitCode::Success;
    }
    if (command.scheduled) {
        TrustedClock clock;
        clock.calibrate(acquired.value());
        CrossProcessMutex mutex;
        const Result<void> locked = mutex.acquire();
        if (!locked) {
            err << "Another time mutation is already running.\n";
            return exitCodeForError(locked.error().code);
        }
        const SystemTimeResult result = WindowsSystemClock::setUtc(clock.utcNow());
        mutex.release();
        if (!result.succeeded) {
            err << "System time synchronization failed.\n";
            return exitCodeForSystemResult(result);
        }
        return ExitCode::Success;
    }

    const Result<HandoffFile> created = ResultHandoff::create();
    if (!created) {
        return exitCodeForError(created.error().code);
    }
    const HandoffFile handoff = created.value();
    const QString absoluteConfig = ConfigRepository::resolvePath(command.configPath);
    const QStringList arguments = {QStringLiteral("--elevated-sync"),
                                   QStringLiteral("--config"),
                                   absoluteConfig,
                                   QStringLiteral("--result"),
                                   handoff.path,
                                   QStringLiteral("--nonce"),
                                   handoff.nonce};
    const Result<HandoffPayload> elevated = runBroker(handoff, executablePath, arguments);
    if (!elevated) {
        err << "Authorization or elevated synchronization failed.\n";
        return exitCodeForError(elevated.error().code);
    }
    return elevated.value().exitCode;
}

ExitCode CliRunner::runElevatedSync(const ParsedCommand &command,
                                    const QString &executablePath,
                                    QTextStream &out) const
{
    Q_UNUSED(executablePath)
    const HandoffFile handoff{command.resultPath, command.nonce};
    const auto writeResult = [&](const HandoffPayload &payload) {
        const Result<void> written = ResultHandoff::write(handoff, payload);
        return written ? payload.exitCode : ExitCode::OtherFailure;
    };

    ConfigRepository repository;
    const Result<ConfigLoadResult> loaded = repository.load(command.configPath);
    if (!loaded) {
        return writeResult(payloadFor(exitCodeForError(loaded.error().code), loaded.error().code));
    }
    const Result<NtpSample> acquired = acquireFresh(loaded.value().config);
    if (!acquired) {
        return writeResult(payloadFor(exitCodeForError(acquired.error().code), acquired.error().code));
    }

    TrustedClock clock;
    clock.calibrate(acquired.value());
    CrossProcessMutex mutex;
    const Result<void> locked = mutex.acquire();
    if (!locked) {
        return writeResult(payloadFor(exitCodeForError(locked.error().code), locked.error().code));
    }
    const SystemTimeResult systemResult = WindowsSystemClock::setUtc(clock.utcNow());
    mutex.release();
    const ExitCode exitCode = systemResult.succeeded ? ExitCode::Success : exitCodeForSystemResult(systemResult);
    const ErrorCode errorCode = systemResult.succeeded ? ErrorCode::None
                                                       : errorCodeForSystemFailure(systemResult.failure);
    out << (systemResult.succeeded ? "System time synchronized.\n" : "System time synchronization failed.\n");
    return writeResult(payloadFor(exitCode, errorCode, systemResult.uncertain));
}

ExitCode CliRunner::runTaskMutation(const ParsedCommand &command,
                                    const QString &executablePath,
                                    QTextStream &out) const
{
    const bool install = command.mode == CommandMode::InstallTask
        || command.mode == CommandMode::ElevatedInstallTask;
    const bool remove = command.mode == CommandMode::RemoveTask
        || command.mode == CommandMode::ElevatedRemoveTask;
    ConfigRepository repository;
    ConfigLoadResult loaded;
    if (install || command.mode == CommandMode::InstallTask) {
        const Result<ConfigLoadResult> config = repository.load(command.configPath);
        if (!config) {
            return exitCodeForError(config.error().code);
        }
        loaded = config.value();
    }

    if (command.mode == CommandMode::ElevatedInstallTask
        || command.mode == CommandMode::ElevatedRemoveTask
        || command.mode == CommandMode::ElevatedSetTaskEnabled) {
        const HandoffFile handoff{command.resultPath, command.nonce};
        const auto writeResult = [&](const HandoffPayload &payload) {
            const Result<void> written = ResultHandoff::write(handoff, payload);
            return written ? payload.exitCode : ExitCode::OtherFailure;
        };
        if (command.mode == CommandMode::ElevatedInstallTask) {
            TaskSchedulerBackend backend;
            const Result<ScheduledTaskInfo> result = backend.install(
                QFileInfo(executablePath).absoluteFilePath(), loaded.path, loaded.config.scheduleIntervalMinutes);
            if (!result) {
                return writeResult(payloadFor(exitCodeForError(result.error().code), result.error().code));
            }
            out << "Scheduled task installed.\n";
            return writeResult(payloadFor(ExitCode::Success, ErrorCode::None));
        }
        if (command.mode == CommandMode::ElevatedSetTaskEnabled) {
            TaskSchedulerBackend backend;
            const Result<ScheduledTaskInfo> result = backend.setEnabled(command.taskEnabled);
            if (!result) {
                return writeResult(payloadFor(exitCodeForError(result.error().code), result.error().code));
            }
            out << (command.taskEnabled ? "Scheduled task enabled.\n" : "Scheduled task paused.\n");
            return writeResult(payloadFor(ExitCode::Success, ErrorCode::None));
        }
        TaskSchedulerBackend backend;
        const Result<void> result = backend.remove();
        if (!result) {
            return writeResult(payloadFor(exitCodeForError(result.error().code), result.error().code));
        }
        out << "Scheduled task removed.\n";
        return writeResult(payloadFor(ExitCode::Success, ErrorCode::None));
    }

    const Result<HandoffFile> created = ResultHandoff::create();
    if (!created) {
        return exitCodeForError(created.error().code);
    }
    const HandoffFile handoff = created.value();
    const QString absoluteConfig = ConfigRepository::resolvePath(command.configPath);
    QStringList arguments = {
        install ? QStringLiteral("--elevated-install-task") : QStringLiteral("--elevated-remove-task"),
        QStringLiteral("--result"), handoff.path,
        QStringLiteral("--nonce"), handoff.nonce,
    };
    if (install) {
        arguments << QStringLiteral("--config") << absoluteConfig;
    }
    const Result<HandoffPayload> elevated = runBroker(handoff, executablePath, arguments);
    if (!elevated) {
        return exitCodeForError(elevated.error().code);
    }
    Q_UNUSED(remove)
    return elevated.value().exitCode;
}

ExitCode CliRunner::run(const ParsedCommand &command,
                        const QString &executablePath,
                        QTextStream &out,
                        QTextStream &err) const
{
    if (command.mode == CommandMode::Gui) {
        return ExitCode::Success;
    }
    if (command.mode == CommandMode::Help) {
        out << CliParser::helpText();
        return ExitCode::Success;
    }
    if (command.mode == CommandMode::ElevatedSync) {
        return runElevatedSync(command, executablePath, out);
    }
    if (command.mode == CommandMode::ElevatedInstallTask
        || command.mode == CommandMode::ElevatedRemoveTask
        || command.mode == CommandMode::ElevatedSetTaskEnabled) {
        return runTaskMutation(command, executablePath, out);
    }
    if (command.mode == CommandMode::InstallTask || command.mode == CommandMode::RemoveTask) {
        return runTaskMutation(command, executablePath, out);
    }

    ConfigRepository repository;
    const Result<ConfigLoadResult> loaded = repository.load(command.configPath);
    if (!loaded) {
        err << "Configuration is invalid or unavailable.\n";
        return exitCodeForError(loaded.error().code);
    }
    return runSync(command, executablePath, loaded.value().config, out, err);
}

} // namespace TimeSync
