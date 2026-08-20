#pragma once

#include "result.h"

#include <QString>
#include <QStringList>

#include <QtNetwork/QHostAddress>

namespace TimeSync {

constexpr quint16 DefaultNtpPort = 123;

struct ServerEndpoint {
    QString original;
    QString host;
    quint16 port = DefaultNtpPort;
    bool literalAddress = false;

    [[nodiscard]] QString canonicalKey() const;
    [[nodiscard]] bool isAliyunPreferred() const;
};

[[nodiscard]] Result<ServerEndpoint> parseServerEndpoint(const QString &value);
[[nodiscard]] Result<QStringList> validateServerList(const QStringList &values);

} // namespace TimeSync
