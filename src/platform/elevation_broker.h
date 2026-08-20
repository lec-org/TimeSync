#pragma once

#include "../core/result.h"

#include <QString>
#include <QStringList>

#include <functional>

namespace TimeSync {

class ElevationBroker final {
public:
    // This blocking form is intended for command-line operation contexts.
    [[nodiscard]] static Result<int> runAsAdministrator(const QString &executablePath,
                                                        const QStringList &arguments,
                                                        int timeoutMs = 120000,
                                                        const std::function<bool()> &cancelRequested = {});

    [[nodiscard]] static QString quoteWindowsArgument(const QString &argument);
};

} // namespace TimeSync
