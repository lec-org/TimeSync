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
};

struct ParsedCommand {
    CommandMode mode = CommandMode::Gui;
    bool dryRun = false;
    bool scheduled = false;
    QString configPath;
    QString resultPath;
    QString nonce;
    bool taskEnabled = false;
};

class CliParser final {
public:
    [[nodiscard]] Result<ParsedCommand> parse(const QStringList &arguments) const;
    [[nodiscard]] static QString helpText();
};

} // namespace TimeSync
