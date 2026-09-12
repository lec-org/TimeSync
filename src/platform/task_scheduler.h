#pragma once

#include "../core/result.h"

#include <QString>

namespace TimeSync {

struct ScheduledTaskInfo {
    bool exists = false;
    bool enabled = false;
    int intervalMinutes = 0;
    QString executablePath;
    QString arguments;
};

class TaskSchedulerBackend final {
public:
    [[nodiscard]] static QString taskName();
    [[nodiscard]] static QString buildActionArguments(const QString &configPath);
    [[nodiscard]] static int parseRepetitionIntervalMinutes(const QString &iso8601);

    [[nodiscard]] Result<ScheduledTaskInfo> query() const;
    [[nodiscard]] Result<ScheduledTaskInfo> install(const QString &executablePath,
                                                     const QString &configPath,
                                                     int intervalMinutes) const;
    [[nodiscard]] Result<ScheduledTaskInfo> setEnabled(bool enabled) const;
    [[nodiscard]] Result<void> remove() const;
};

} // namespace TimeSync
