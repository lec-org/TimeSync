#include "sync_coordinator.h"

#include "../platform/cross_process_mutex.h"
#include "../platform/windows_system_clock.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>

#include <system_error>
#include <thread>
#include <utility>

namespace TimeSync {
namespace {

struct MutationResult {
    Result<void> acquired = Result<void>::failure({ErrorCode::InternalError,
                                                    QStringLiteral("system-time mutation did not acquire mutex")});
    SystemTimeResult system;
};

} // namespace

SyncCoordinator::SyncCoordinator(QObject *parent)
    : QObject(parent)
    , ntpClient_(this)
    , clock_(TrustedClock::DefaultFreshnessWindowMs, this)
{
    connect(&ntpClient_, &NtpClient::finished, this, &SyncCoordinator::handleNtpFinished);
    connect(&clock_, &TrustedClock::changed, this, [this] { emit referenceChanged(clock_.state()); });
    mutationWatchdog_.setSingleShot(true);
    mutationWatchdog_.setInterval(SystemClockMutationTimeoutMs);
    connect(&mutationWatchdog_, &QTimer::timeout, this, &SyncCoordinator::handleMutationWatchdog);
}

SyncCoordinator::~SyncCoordinator()
{
    mutationWatchdog_.stop();
    if (mutationCancel_) {
        mutationCancel_->store(true, std::memory_order_relaxed);
    }
}

Result<QList<ServerEndpoint>> SyncCoordinator::endpointsForConfig(const AppConfig &config) const
{
    QList<ServerEndpoint> endpoints;
    endpoints.reserve(config.servers.size());
    for (const QString &server : config.servers) {
        const Result<ServerEndpoint> endpoint = parseServerEndpoint(server);
        if (!endpoint) {
            return Result<QList<ServerEndpoint>>::failure(endpoint.error());
        }
        endpoints.append(endpoint.value());
    }
    if (endpoints.isEmpty()) {
        return Result<QList<ServerEndpoint>>::failure(
            {ErrorCode::InvalidConfiguration, QStringLiteral("no configured NTP source")});
    }
    return Result<QList<ServerEndpoint>>::success(std::move(endpoints));
}

Result<quint64> SyncCoordinator::startRefresh(const AppConfig &config)
{
    return startSync(config, SyncMode::RefreshOnly, false);
}

bool SyncCoordinator::canApplyFreshClockForManualSync() const
{
    return clock_.isFresh() && clock_.utcNow().isValid();
}

Result<quint64> SyncCoordinator::startSync(const AppConfig &config,
                                           const SyncMode mode,
                                           const bool scheduled)
{
    const bool manualSync = mode == SyncMode::AuthorizedDirect && !scheduled;
    if (busy_ || mutationOutstanding_) {
        if (manualSync && mode_ == SyncMode::RefreshOnly && operation_ == OperationKind::Refresh
            && !cancelRequested_ && !mutationOutstanding_) {
            operation_ = OperationKind::ManualSync;
            mode_ = SyncMode::AuthorizedDirect;
            scheduled_ = false;
            return Result<quint64>::success(operationGeneration_);
        }
        return Result<quint64>::failure({ErrorCode::Busy, QStringLiteral("sync operation already running")});
    }

    if (manualSync && canApplyFreshClockForManualSync()) {
        ++operationGeneration_;
        operation_ = OperationKind::ManualSync;
        mode_ = SyncMode::AuthorizedDirect;
        scheduled_ = false;
        cancelRequested_ = false;
        busy_ = true;
        ntpGeneration_ = 0;
        startSystemTimeMutation(clock_.utcNow(), clock_.source());
        return Result<quint64>::success(operationGeneration_);
    }

    const Result<QList<ServerEndpoint>> endpoints = endpointsForConfig(config);
    if (!endpoints) {
        return Result<quint64>::failure(endpoints.error());
    }

    ++operationGeneration_;
    operation_ = mode == SyncMode::RefreshOnly
        ? OperationKind::Refresh
        : (scheduled ? OperationKind::ScheduledSync : OperationKind::ManualSync);
    mode_ = mode;
    scheduled_ = scheduled;
    cancelRequested_ = false;
    busy_ = true;
    const Result<quint64> started = ntpClient_.start(endpoints.value());
    if (!started) {
        busy_ = false;
        return Result<quint64>::failure(started.error());
    }
    ntpGeneration_ = started.value();
    return Result<quint64>::success(operationGeneration_);
}

void SyncCoordinator::cancel()
{
    if (!busy_) {
        return;
    }
    cancelRequested_ = true;
    if (mutationCancel_) {
        mutationCancel_->store(true, std::memory_order_relaxed);
    }
    ntpClient_.cancel();
}

void SyncCoordinator::handleNtpFinished(const quint64 generation, const NtpQueryResult &result)
{
    if (!busy_ || generation != ntpGeneration_) {
        return;
    }
    if (!result.succeeded) {
        clock_.markRefreshFailed();
        if (result.failure.kind == NtpFailureKind::Cancelled || cancelRequested_) {
            finishFailure(ErrorCode::Cancelled);
        } else if (result.failure.kind == NtpFailureKind::InvalidConfiguration) {
            finishFailure(ErrorCode::InvalidConfiguration);
        } else {
            finishFailure(result.failure.kind == NtpFailureKind::NetworkUnavailable
                              ? ErrorCode::NetworkUnavailable
                              : (result.failure.kind == NtpFailureKind::ProtocolRejected
                                     ? ErrorCode::ProtocolRejected
                                     : ErrorCode::NoValidSource));
        }
        return;
    }

    clock_.calibrate(result.sample);
    emit referenceChanged(clock_.state());
    if (mode_ == SyncMode::RefreshOnly || mode_ == SyncMode::DryRun) {
        OperationResult completed;
        completed.generation = operationGeneration_;
        completed.operation = operation_;
        completed.succeeded = true;
        completed.exitCode = ExitCode::Success;
        completed.source = result.sample.source;
        finish(completed);
        return;
    }

    if (cancelRequested_) {
        finishFailure(ErrorCode::Cancelled);
        return;
    }

    startSystemTimeMutation(clock_.utcNow(), result.sample.source);
}

void SyncCoordinator::startSystemTimeMutation(const QDateTime &targetUtc, const QString &source)
{
    const quint64 generation = operationGeneration_;
    const OperationKind operation = operation_;
    const std::shared_ptr<std::atomic_bool> cancel = std::make_shared<std::atomic_bool>(false);
    const std::shared_ptr<MutationResult> mutation = std::make_shared<MutationResult>();
    mutationCancel_ = cancel;

    emit systemClockMutationStarted();
    mutationOutstanding_ = true;

    const QPointer<SyncCoordinator> self(this);
    try {
        std::thread([self, targetUtc, cancel, mutation, generation, operation, source] {
            CrossProcessMutex mutationMutex;
            mutation->acquired = mutationMutex.acquire(CrossProcessMutex::DefaultWaitMs);
            if (!mutation->acquired && cancel->load(std::memory_order_relaxed)) {
                mutation->acquired = Result<void>::failure(
                    {ErrorCode::Cancelled, QStringLiteral("system-time mutation cancelled")});
            } else if (mutation->acquired) {
                SystemTimeHelperSession session = WindowsSystemClock::launchSetUtcHelper(
                    QCoreApplication::applicationFilePath(), targetUtc);
                if (!session.launched) {
                    mutation->system = session.launchResult;
                } else if (self) {
                    QMetaObject::invokeMethod(self.data(), [self, generation, operation, source] {
                        if (self.isNull()) {
                            return;
                        }
                        self->handleMutationAccepted(generation, operation, source);
                    }, Qt::QueuedConnection);
                    mutation->system = WindowsSystemClock::waitSetUtcHelper(session);
                } else {
                    mutation->system = WindowsSystemClock::waitSetUtcHelper(session);
                }
                mutationMutex.release();
            }
            if (self.isNull()) {
                return;
            }
            QMetaObject::invokeMethod(self.data(), [self, generation, operation, source, mutation] {
                if (self.isNull()) {
                    return;
                }
                self->handleMutationFinished(
                    generation, operation, source, mutation->acquired, mutation->system);
            }, Qt::QueuedConnection);
        }).detach();
    } catch (const std::system_error &) {
        mutationOutstanding_ = false;
        emit systemClockMutationEnded();
        finishFailure(ErrorCode::InternalError);
    }
}

void SyncCoordinator::handleMutationWatchdog()
{
    if (!busy_) {
        return;
    }
    if (mutationCancel_) {
        mutationCancel_->store(true, std::memory_order_relaxed);
    }
    finishFailure(ErrorCode::UncertainState, true);
}

void SyncCoordinator::handleMutationAccepted(const quint64 generation,
                                             const OperationKind operation,
                                             const QString &source)
{
    if (!busy_ || generation != operationGeneration_) {
        return;
    }
    mutationWatchdog_.stop();
    OperationResult completed;
    completed.generation = generation;
    completed.operation = operation;
    completed.succeeded = true;
    completed.exitCode = ExitCode::Success;
    completed.source = source;
    finish(completed);
}

void SyncCoordinator::handleMutationFinished(const quint64 generation,
                                              const OperationKind operation,
                                              const QString &source,
                                              const Result<void> &acquired,
                                              const SystemTimeResult &systemResult)
{
    mutationWatchdog_.stop();
    mutationCancel_.reset();
    const bool failed = !acquired || !systemResult.succeeded;
    if (generation == operationGeneration_ && failed) {
        if (busy_) {
            if (!acquired) {
                finishFailure(acquired.error().code, acquired.error().uncertain);
            } else {
                const OperationFailure failure = operationFailureForSystemTimeFailure(
                    static_cast<int>(systemResult.failure));
                OperationResult failedResult;
                failedResult.generation = generation;
                failedResult.operation = operation;
                failedResult.failure = failure;
                failedResult.exitCode = failure == OperationFailure::VerificationFailed
                    ? ExitCode::VerificationFailure
                    : (failure == OperationFailure::PrivilegeMissing
                           ? ExitCode::PermissionFailure
                           : exitCodeForError(systemResult.uncertain ? ErrorCode::UncertainState
                                                                      : ErrorCode::SystemTimeRejected));
                failedResult.uncertain = systemResult.uncertain;
                finish(failedResult);
            }
        } else {
            OperationResult failedResult;
            failedResult.generation = generation;
            failedResult.operation = operation;
            if (!acquired) {
                failedResult.failure = operationFailureForError(acquired.error().code);
                failedResult.exitCode = exitCodeForError(acquired.error().code);
                failedResult.uncertain = acquired.error().uncertain;
            } else {
                failedResult.failure = operationFailureForSystemTimeFailure(
                    static_cast<int>(systemResult.failure));
                failedResult.exitCode = failedResult.failure == OperationFailure::VerificationFailed
                    ? ExitCode::VerificationFailure
                    : (failedResult.failure == OperationFailure::PrivilegeMissing
                           ? ExitCode::PermissionFailure
                           : exitCodeForError(systemResult.uncertain ? ErrorCode::UncertainState
                                                                      : ErrorCode::SystemTimeRejected));
                failedResult.uncertain = systemResult.uncertain;
            }
            emit operationFinished(failedResult);
        }
    } else if (generation == operationGeneration_ && busy_ && !failed) {
        OperationResult completed;
        completed.generation = generation;
        completed.operation = operation;
        completed.succeeded = true;
        completed.exitCode = ExitCode::Success;
        completed.source = source;
        finish(completed);
    }
    mutationOutstanding_ = false;
    emit systemClockMutationEnded();
}

void SyncCoordinator::finishFailure(const ErrorCode code, const bool uncertain)
{
    OperationResult failed;
    failed.generation = operationGeneration_;
    failed.operation = operation_;
    failed.failure = operationFailureForError(code);
    failed.exitCode = exitCodeForError(code);
    failed.uncertain = uncertain;
    finish(failed);
}

void SyncCoordinator::finish(const OperationResult &result)
{
    if (!busy_) {
        return;
    }
    mutationWatchdog_.stop();
    busy_ = false;
    cancelRequested_ = false;
    emit operationFinished(result);
}

void SyncCoordinator::attachPowerResumeMonitor(PowerResumeMonitor *monitor)
{
    if (monitor == nullptr) {
        return;
    }
    connect(monitor, &PowerResumeMonitor::resumed, this, &SyncCoordinator::handleResume, Qt::UniqueConnection);
}

void SyncCoordinator::handleResume()
{
    clock_.markResume();
    emit referenceChanged(clock_.state());
}

} // namespace TimeSync
