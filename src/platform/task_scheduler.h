#pragma once

#include "../core/result.h"

#include <QString>
#include <QStringList>

namespace TimeSync {

enum class ScheduledTaskKind { Periodic, Startup };

struct ScheduledTaskInfo {
    bool exists = false;
    bool enabled = false;
    int intervalMinutes = 0;
    QString executablePath;
    QString arguments;
};

class TaskSchedulerBackend final {
public:
    explicit TaskSchedulerBackend(ScheduledTaskKind kind = ScheduledTaskKind::Periodic) : kind_(kind) {}
    [[nodiscard]] static QString taskName(ScheduledTaskKind kind = ScheduledTaskKind::Periodic);
    [[nodiscard]] static QString buildActionArguments(const QString &configPath,
                                                      ScheduledTaskKind kind = ScheduledTaskKind::Periodic);
    [[nodiscard]] static int parseRepetitionIntervalMinutes(const QString &iso8601);
    // Semantic action identity, independent of install location and canonical template formatting.
    [[nodiscard]] static bool isOwnedTaskAction(const QString &executablePath,
                                               const QStringList &arguments,
                                               ScheduledTaskKind kind = ScheduledTaskKind::Periodic);

    [[nodiscard]] Result<ScheduledTaskInfo> query() const;
    [[nodiscard]] Result<ScheduledTaskInfo> install(const QString &executablePath,
                                                     const QString &configPath,
                                                     int intervalMinutes) const;
    [[nodiscard]] Result<ScheduledTaskInfo> setEnabled(bool enabled) const;
    [[nodiscard]] Result<void> remove() const;

private:
    ScheduledTaskKind kind_;
};

} // namespace TimeSync
