#pragma once

#include "config_repository.h"
#include "operation_types.h"
#include "trusted_clock.h"
#include "../network/ntp_client.h"
#include "../platform/power_resume_monitor.h"

#include <QList>
#include <QObject>
#include <QTimer>

#include <atomic>
#include <memory>

namespace TimeSync {

struct SystemTimeResult;

enum class SyncMode {
    RefreshOnly = 0,
    DryRun,
    AuthorizedDirect,
};

class SyncCoordinator final : public QObject {
    Q_OBJECT

public:
    explicit SyncCoordinator(QObject *parent = nullptr);
    ~SyncCoordinator() override;

    [[nodiscard]] Result<quint64> startRefresh(const AppConfig &config);
    [[nodiscard]] Result<quint64> startSync(const AppConfig &config,
                                             SyncMode mode,
                                             bool scheduled = false);
    void cancel();

    [[nodiscard]] bool isBusy() const noexcept { return busy_; }
    [[nodiscard]] bool isSystemWriteInProgress() const noexcept { return mutationOutstanding_; }
    [[nodiscard]] bool canApplyFreshClockForManualSync() const;
    [[nodiscard]] const TrustedClock *trustedClock() const noexcept { return &clock_; }
    [[nodiscard]] TrustedClock *trustedClock() noexcept { return &clock_; }
    void attachPowerResumeMonitor(PowerResumeMonitor *monitor);

signals:
    void referenceChanged(const TimeSync::TrustedClockState &state);
    void operationFinished(const TimeSync::OperationResult &result);
    void systemClockMutationStarted();
    void systemClockMutationEnded();

private slots:
    void handleNtpFinished(quint64 generation, const TimeSync::NtpQueryResult &result);
    void handleResume();
    void handleMutationWatchdog();

private:
    static constexpr int SystemClockMutationTimeoutMs = 5000;

    [[nodiscard]] Result<QList<ServerEndpoint>> endpointsForConfig(const AppConfig &config) const;
    void startSystemTimeMutation(const QDateTime &targetUtc, const QString &source);
    void handleMutationAccepted(quint64 generation,
                                OperationKind operation,
                                const QString &source);
    void handleMutationFinished(quint64 generation,
                                OperationKind operation,
                                const QString &source,
                                const Result<void> &acquired,
                                const SystemTimeResult &systemResult);
    void finish(const OperationResult &result);
    void finishFailure(ErrorCode code, bool uncertain = false);

    NtpClient ntpClient_;
    TrustedClock clock_;
    quint64 operationGeneration_ = 0;
    quint64 ntpGeneration_ = 0;
    OperationKind operation_ = OperationKind::Refresh;
    SyncMode mode_ = SyncMode::RefreshOnly;
    bool scheduled_ = false;
    bool busy_ = false;
    bool mutationOutstanding_ = false;
    bool cancelRequested_ = false;
    std::shared_ptr<std::atomic_bool> mutationCancel_;
    QTimer mutationWatchdog_;
};

} // namespace TimeSync
