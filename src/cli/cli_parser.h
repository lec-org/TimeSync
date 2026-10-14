#pragma once

#include "../core/result.h"

#include <QString>
#include <QStringList>

namespace TimeSync {

enum class CommandMode {
    Gui = 0,
    Help,
    SyncOnce,
    InstallTask,
    RemoveTask,
    ElevatedSync,
    ElevatedInstallTask,
    ElevatedRemoveTask,
    ElevatedSetTaskEnabled,
    InstallStartupTask,
    RemoveStartupTask,
};

struct ParsedCommand {
    CommandMode mode = CommandMode::Gui;
    bool dryRun = false;
    bool scheduled = false;
    bool startup = false;
    QString configPath;
    QString resultPath;
    QString nonce;
    bool taskEnabled = false;

    // Scheduled commands are rejected before elevation; GUI relaunch remains asynchronous.
    [[nodiscard]] bool requiresSynchronousElevation() const noexcept
    {
        return mode != CommandMode::Gui && mode != CommandMode::Help && !scheduled;
    }
};

class CliParser final {
public:
    [[nodiscard]] Result<ParsedCommand> parse(const QStringList &arguments) const;
    [[nodiscard]] static QString helpText();
};

} // namespace TimeSync
