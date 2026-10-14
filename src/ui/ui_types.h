#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace TimeSync::Ui {

enum class ReferenceStatus {
    NoData,
    Loading,
    Current,
    Stale,
    Error,
};

enum class ReferenceFailure {
    None,
    NetworkUnavailable,
    ServerUnavailable,
    Unknown,
};

enum class ScheduleStatus {
    NotConfigured,
    Active,
    Paused,
    Updating,
    Error,
};

enum class OperationKind {
    Refresh,
    ManualSync,
    SaveServers,
    ChangeSchedule,
    RemoveSchedule,
    ChangeStartupSync,
};

enum class OperationFailure {
    None,
    RemoteTimeUnavailable,
    ServerUnavailable,
    AuthorizationDenied,
    InsufficientPrivilege,
    SystemTimeRejected,
    VerificationFailed,
    Cancelled,
    InvalidConfiguration,
    SaveFailed,
    ScheduleAuthorizationDenied,
    ScheduleUpdateFailed,
    ScheduleRemoveFailed,
    StartupSyncUpdateFailed,
    UncertainState,
    SystemTimeConfirming,
    Unknown,
};

struct ReferenceState {
    ReferenceStatus status = ReferenceStatus::NoData;
    ReferenceFailure failure = ReferenceFailure::None;
    bool hasReferenceTime = false;
    QDateTime beijingTime;
    QString source;
    qint64 calibrationAgeSeconds = -1;
};

struct BusyState {
    bool refreshing = false;
    bool syncing = false;
    bool savingServers = false;
    bool changingSchedule = false;
    bool removingSchedule = false;
    bool changingStartupSync = false;
};

struct ScheduleState {
    bool exists = false;
    bool enabled = false;
    int intervalMinutes = 60;
    ScheduleStatus status = ScheduleStatus::NotConfigured;
};

struct OperationResult {
    OperationKind operation = OperationKind::Refresh;
    bool success = false;
    OperationFailure failure = OperationFailure::Unknown;
};

enum class StartupSyncStatus {
    Checking,
    Disabled,
    Enabled,
    Updating,
    Error,
};

// The OS task is the source of truth; this setting is not stored in AppConfig.
struct StartupSyncState {
    bool known = false;
    bool exists = false;
    bool enabled = false;
    StartupSyncStatus status = StartupSyncStatus::Checking;
    bool updateFailed = false;
};

} // namespace TimeSync::Ui

Q_DECLARE_METATYPE(TimeSync::Ui::ReferenceState)
Q_DECLARE_METATYPE(TimeSync::Ui::BusyState)
Q_DECLARE_METATYPE(TimeSync::Ui::ScheduleState)
Q_DECLARE_METATYPE(TimeSync::Ui::OperationResult)
Q_DECLARE_METATYPE(TimeSync::Ui::StartupSyncState)
