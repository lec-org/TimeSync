#pragma once

#include "cli_parser.h"

#include "../core/config_repository.h"
#include "../network/ntp_client.h"
#include "../platform/elevation_broker.h"
#include "../platform/result_handoff.h"
#include "../platform/windows_system_clock.h"

#include <QTextStream>

namespace TimeSync {

class CliRunner final {
public:
    [[nodiscard]] ExitCode run(const ParsedCommand &command,
                               const QString &executablePath,
                               QTextStream &out,
                               QTextStream &err) const;

private:
    [[nodiscard]] Result<NtpSample> acquireFresh(const AppConfig &config) const;
    [[nodiscard]] ExitCode runSync(const ParsedCommand &command,
                                    const QString &executablePath,
                                    const AppConfig &config,
                                    QTextStream &out,
                                    QTextStream &err) const;
    [[nodiscard]] ExitCode runElevatedSync(const ParsedCommand &command,
                                           const QString &executablePath,
                                           QTextStream &out) const;
    [[nodiscard]] ExitCode runTaskMutation(const ParsedCommand &command,
                                           const QString &executablePath,
                                           QTextStream &out) const;
    [[nodiscard]] Result<HandoffPayload> runBroker(const HandoffFile &handoff,
                                                   const QString &executablePath,
                                                   const QStringList &arguments) const;
    [[nodiscard]] static ErrorCode errorCodeForNtpFailure(NtpFailureKind kind);
    [[nodiscard]] static ErrorCode errorCodeForSystemFailure(SystemTimeFailure failure);
};

} // namespace TimeSync
