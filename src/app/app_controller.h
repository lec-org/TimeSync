#pragma once

#include "../cli/cli_parser.h"
#include "../core/config_repository.h"
#include "../core/operation_types.h"
#include "../core/sync_coordinator.h"
#include "../platform/elevation_broker.h"
#include "../platform/power_resume_monitor.h"
#include "../platform/result_handoff.h"
#include "../platform/task_scheduler.h"
#include "../ui/ui_types.h"

#include <QPointer>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>

class QCoreApplication;
class QThread;

namespace TimeSync::Ui {
class MainWindow;
}

namespace TimeSync {

class AppController final : public QObject {
    Q_OBJECT

public:
    explicit AppController(Ui::MainWindow *window,
                           const ParsedCommand &command,
                           QObject *parent = nullptr);
    ~AppController() override;

    void start(QCoreApplication *application);
    void shutdown();

    [[nodiscard]] static Ui::ReferenceState toUiReferenceState(const TrustedClockState &state,
                                                               bool loading,
                                                               Ui::ReferenceFailure failure);
    [[nodiscard]] static Ui::OperationResult toUiOperationResult(const OperationResult &result);

private slots:
    void onRefreshRequested();
    void onManualSyncRequested();
    void onServersSaveRequested(const QStringList &servers);
    void onScheduleChangeRequested(bool enabled, int intervalMinutes);
    void onScheduleRemoveRequested();
    void onLanguageChangeRequested(const QString &languageTag);
    void onReferenceChanged(const TimeSync::TrustedClockState &state);
    void onOperationFinished(const TimeSync::OperationResult &result);
    void onPowerResumed();
    void onClockTimer();

private:
    enum class ElevatedPurpose {
        ManualSync,
        InstallTask,
        DisableTask,
        RemoveTask,
    };

    enum class ScheduleMutation {
        None,
        Install,
        Disable,
        Remove,
    };

    void connectUi();
    void loadConfiguration();
    void initializeUiFromConfiguration();
    void startInitialRefresh();
    void startRefresh(bool forManualSync = false);
    void setLoadingState(bool loading, bool forManualSync = false);
    void applyReferenceState();
    void applyTaskState(const Result<ScheduledTaskInfo> &result);
    void queryTaskState();
    void startTaskJob(std::function<Result<ScheduledTaskInfo>()> operation,
                      std::function<void(const Result<ScheduledTaskInfo> &)> completed);
    void startElevation(ElevatedPurpose purpose, const QStringList &arguments, const HandoffFile &handoff);
    void handleElevationResult(ElevatedPurpose purpose, const Result<HandoffPayload> &result);
    void finishManualSyncFailure(const Error &error);
    void finishManualSyncSuccess();
    void finishScheduleMutationSuccess();
    void finishScheduleMutationFailure(const Error &error, bool uncertain = false);
    void rollbackScheduleConfiguration(bool uncertainOnFailure, const Error &operationError);
    void emitUiOperation(Ui::OperationKind operation,
                         bool success,
                         Ui::OperationFailure failure = Ui::OperationFailure::None,
                         bool uncertain = false,
                         const QString &source = {});
    void emitUiErrorOperation(Ui::OperationKind operation, const Error &error, bool uncertain = false);
    void maybeStartPendingRefresh();
    [[nodiscard]] static Ui::OperationFailure mapErrorToUiFailure(ErrorCode code,
                                                                    Ui::OperationKind operation);
    [[nodiscard]] static Ui::ReferenceFailure mapReferenceFailure(const NtpFailureKind kind);
    [[nodiscard]] static Ui::OperationKind mapOperationKind(OperationKind operation);
    [[nodiscard]] static Ui::ScheduleStatus mapScheduleStatus(const ScheduledTaskInfo &info);
    [[nodiscard]] static QString normalizeLanguageTag(const QString &tag);
    [[nodiscard]] bool canStartOperation() const;
    void clearBackgroundThread();

    QPointer<Ui::MainWindow> window_;
    ParsedCommand command_;
    QCoreApplication *application_ = nullptr;
    ConfigRepository configRepository_;
    AppConfig config_;
    AppConfig scheduleRollbackConfig_;
    Ui::ScheduleState scheduleRollbackState_;
    QString configPath_;
    bool configReady_ = false;
    bool started_ = false;
    bool shuttingDown_ = false;
    bool refreshPending_ = false;
    bool taskJobActive_ = false;
    bool taskStateKnown_ = false;
    bool pendingScheduleChange_ = false;
    bool pendingScheduleEnabled_ = false;
    int pendingScheduleInterval_ = ConfigRepository::defaultConfig().scheduleIntervalMinutes;
    bool pendingScheduleRemove_ = false;

    SyncCoordinator coordinator_;
    PowerResumeMonitor powerMonitor_;
    QTimer *clockTimer_ = nullptr;

    Ui::BusyState busyState_;
    Ui::ScheduleState scheduleState_;
    Ui::ReferenceFailure referenceFailure_ = Ui::ReferenceFailure::None;
    QString languageBeforeChange_;
    bool referenceLoading_ = false;
    bool manualAcquisition_ = false;
    bool elevationActive_ = false;
    bool scheduleMutationActive_ = false;
    bool scheduleDesiredEnabled_ = false;
    int scheduleDesiredInterval_ = ConfigRepository::defaultConfig().scheduleIntervalMinutes;
    ScheduleMutation scheduleMutation_ = ScheduleMutation::None;
    quint64 activeCoordinatorGeneration_ = 0;
    QThread *backgroundThread_ = nullptr;
    std::shared_ptr<std::atomic_bool> elevationCancel_;
    QSet<QString> pendingHandoffPaths_;
};

} // namespace TimeSync
