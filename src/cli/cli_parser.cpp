#include "cli_parser.h"

#include <QFileInfo>
#include <QRegularExpression>

namespace TimeSync {
namespace {

Result<ParsedCommand> invalid(const QString &detail)
{
    return Result<ParsedCommand>::failure({ErrorCode::InvalidArgument, detail});
}

} // namespace

Result<ParsedCommand> CliParser::parse(const QStringList &arguments) const
{
    ParsedCommand command;
    bool help = false;
    bool syncOnce = false;
    bool installTask = false;
    bool removeTask = false;
    bool configSeen = false;
    bool dryRunSeen = false;
    bool scheduledSeen = false;
    bool elevatedSync = false;
    bool elevatedInstall = false;
    bool elevatedRemove = false;
    bool elevatedSetEnabled = false;
    bool resultSeen = false;
    bool nonceSeen = false;
    bool enabledSeen = false;

    for (int index = 0; index < arguments.size(); ++index) {
        const QString argument = arguments.at(index);
        if (argument == QStringLiteral("--help")) {
            if (help) {
                return invalid(QStringLiteral("duplicate --help"));
            }
            help = true;
        } else if (argument == QStringLiteral("--sync-once")) {
            if (syncOnce) {
                return invalid(QStringLiteral("duplicate --sync-once"));
            }
            syncOnce = true;
        } else if (argument == QStringLiteral("--install-task")) {
            if (installTask) {
                return invalid(QStringLiteral("duplicate --install-task"));
            }
            installTask = true;
        } else if (argument == QStringLiteral("--remove-task")) {
            if (removeTask) {
                return invalid(QStringLiteral("duplicate --remove-task"));
            }
            removeTask = true;
        } else if (argument == QStringLiteral("--dry-run")) {
            if (dryRunSeen) {
                return invalid(QStringLiteral("duplicate --dry-run"));
            }
            dryRunSeen = true;
            command.dryRun = true;
        } else if (argument == QStringLiteral("--scheduled")) {
            if (scheduledSeen) {
                return invalid(QStringLiteral("duplicate --scheduled"));
            }
            scheduledSeen = true;
            command.scheduled = true;
        } else if (argument == QStringLiteral("--elevated-sync")) {
            if (elevatedSync) {
                return invalid(QStringLiteral("duplicate --elevated-sync"));
            }
            elevatedSync = true;
        } else if (argument == QStringLiteral("--elevated-install-task")) {
            if (elevatedInstall) {
                return invalid(QStringLiteral("duplicate internal task flag"));
            }
            elevatedInstall = true;
        } else if (argument == QStringLiteral("--elevated-remove-task")) {
            if (elevatedRemove) {
                return invalid(QStringLiteral("duplicate internal task flag"));
            }
            elevatedRemove = true;
        } else if (argument == QStringLiteral("--elevated-set-task-enabled")) {
            if (elevatedSetEnabled) {
                return invalid(QStringLiteral("duplicate internal task flag"));
            }
            elevatedSetEnabled = true;
        } else if (argument == QStringLiteral("--enabled")) {
            if (enabledSeen || index + 1 >= arguments.size()) {
                return invalid(QStringLiteral("missing or duplicate --enabled value"));
            }
            const QString value = arguments.at(++index);
            if (value != QStringLiteral("true") && value != QStringLiteral("false")) {
                return invalid(QStringLiteral("--enabled must be true or false"));
            }
            enabledSeen = true;
            command.taskEnabled = value == QStringLiteral("true");
        } else if (argument == QStringLiteral("--config")) {
            if (configSeen || index + 1 >= arguments.size() || arguments.at(index + 1).isEmpty()
                || arguments.at(index + 1).startsWith(QStringLiteral("--"))) {
                return invalid(QStringLiteral("missing or duplicate --config value"));
            }
            configSeen = true;
            command.configPath = arguments.at(++index);
        } else if (argument == QStringLiteral("--result")) {
            if (resultSeen || index + 1 >= arguments.size() || arguments.at(index + 1).isEmpty()
                || arguments.at(index + 1).startsWith(QStringLiteral("--"))) {
                return invalid(QStringLiteral("missing or duplicate --result value"));
            }
            resultSeen = true;
            command.resultPath = arguments.at(++index);
        } else if (argument == QStringLiteral("--nonce")) {
            if (nonceSeen || index + 1 >= arguments.size() || arguments.at(index + 1).isEmpty()
                || arguments.at(index + 1).startsWith(QStringLiteral("--"))) {
                return invalid(QStringLiteral("missing or duplicate --nonce value"));
            }
            nonceSeen = true;
            command.nonce = arguments.at(++index);
        } else {
            return invalid(QStringLiteral("unknown command-line argument"));
        }
    }

    const int internalCount = static_cast<int>(elevatedSync) + static_cast<int>(elevatedInstall)
        + static_cast<int>(elevatedRemove) + static_cast<int>(elevatedSetEnabled);
    const int publicCount = static_cast<int>(syncOnce) + static_cast<int>(installTask)
        + static_cast<int>(removeTask);
    if (help) {
        if (internalCount != 0 || publicCount != 0 || configSeen || dryRunSeen || scheduledSeen || resultSeen
            || nonceSeen || enabledSeen) {
            return invalid(QStringLiteral("--help cannot be combined with an operation"));
        }
        command.mode = CommandMode::Help;
        return Result<ParsedCommand>::success(std::move(command));
    }
    if (internalCount > 1 || (internalCount > 0 && publicCount > 0) || (internalCount > 0 && dryRunSeen)
        || (internalCount > 0 && scheduledSeen)) {
        return invalid(QStringLiteral("conflicting internal command"));
    }
    if (internalCount > 0) {
        if (!resultSeen || !nonceSeen || !QFileInfo(command.resultPath).isAbsolute()) {
            return invalid(QStringLiteral("internal command requires absolute result and nonce"));
        }
        static const QRegularExpression noncePattern(QStringLiteral("^[0-9a-f]{32}$"));
        if (!noncePattern.match(command.nonce).hasMatch()) {
            return invalid(QStringLiteral("invalid internal nonce"));
        }
        if (elevatedSync) {
            if (publicCount != 0 || enabledSeen) {
                return invalid(QStringLiteral("invalid elevated sync combination"));
            }
            command.mode = CommandMode::ElevatedSync;
        } else if (elevatedInstall) {
            if (publicCount != 0 || !configSeen || command.configPath.isEmpty() || enabledSeen) {
                return invalid(QStringLiteral("invalid elevated install combination"));
            }
            command.mode = CommandMode::ElevatedInstallTask;
        } else if (elevatedRemove) {
            if (publicCount != 0 || enabledSeen || configSeen) {
                return invalid(QStringLiteral("invalid elevated remove combination"));
            }
            command.mode = CommandMode::ElevatedRemoveTask;
        } else {
            if (!enabledSeen || publicCount != 0 || configSeen) {
                return invalid(QStringLiteral("internal task enable state is required"));
            }
            command.mode = CommandMode::ElevatedSetTaskEnabled;
        }
        if (configSeen && !QFileInfo(command.configPath).isAbsolute()) {
            return invalid(QStringLiteral("internal config path must be absolute"));
        }
        return Result<ParsedCommand>::success(std::move(command));
    }

    if (publicCount > 1) {
        return invalid(QStringLiteral("conflicting public commands"));
    }
    if (dryRunSeen && !syncOnce) {
        return invalid(QStringLiteral("--dry-run requires --sync-once"));
    }
    if (scheduledSeen && !syncOnce) {
        return invalid(QStringLiteral("--scheduled requires --sync-once"));
    }
    if (scheduledSeen && dryRunSeen) {
        return invalid(QStringLiteral("--scheduled cannot be combined with --dry-run"));
    }
    if (scheduledSeen && (!configSeen || !QFileInfo(command.configPath).isAbsolute())) {
        return invalid(QStringLiteral("--scheduled requires an absolute --config path"));
    }
    if (syncOnce) {
        command.mode = CommandMode::SyncOnce;
    } else if (installTask) {
        command.mode = CommandMode::InstallTask;
    } else if (removeTask) {
        command.mode = CommandMode::RemoveTask;
    } else {
        command.mode = CommandMode::Gui;
    }
    if (publicCount == 0 && (dryRunSeen || scheduledSeen || resultSeen || nonceSeen || enabledSeen)) {
        return invalid(QStringLiteral("flag requires an operation"));
    }
    if (publicCount > 0 && (resultSeen || nonceSeen || enabledSeen)) {
        return invalid(QStringLiteral("internal result flags require an elevated command"));
    }
    return Result<ParsedCommand>::success(std::move(command));
}

QString CliParser::helpText()
{
    return QStringLiteral(
        "TimeSync\n"
        "Usage:\n"
        "  TimeSync.exe\n"
        "  TimeSync.exe --sync-once [--dry-run] [--config <path>]\n"
        "  TimeSync.exe --install-task [--config <path>]\n"
        "  TimeSync.exe --remove-task [--config <path>]\n"
        "  TimeSync.exe --help\n");
}

} // namespace TimeSync
