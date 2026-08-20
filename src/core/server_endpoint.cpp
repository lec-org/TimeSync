#include "server_endpoint.h"

#include <QRegularExpression>
#include <QSet>

namespace TimeSync {
namespace {

Error invalidEndpoint(const QString &detail)
{
    return {ErrorCode::InvalidConfiguration, detail};
}

bool hasForbiddenCharacters(const QString &value)
{
    for (const QChar character : value) {
        const ushort code = character.unicode();
        if (character.isSpace() || code < 0x20 || code == 0x7f) {
            return true;
        }
    }
    return false;
}

Result<quint16> parsePort(const QString &value)
{
    if (value.isEmpty()) {
        return Result<quint16>::failure(invalidEndpoint(QStringLiteral("empty port")));
    }
    for (const QChar character : value) {
        if (!character.isDigit() || character.unicode() > 0x7f) {
            return Result<quint16>::failure(invalidEndpoint(QStringLiteral("invalid port")));
        }
    }

    bool converted = false;
    const int port = value.toInt(&converted, 10);
    if (!converted || port < 1 || port > 65535) {
        return Result<quint16>::failure(invalidEndpoint(QStringLiteral("port out of range")));
    }
    return Result<quint16>::success(static_cast<quint16>(port));
}

bool isValidHostName(const QString &host)
{
    if (host.isEmpty() || host.size() > 253 || host.endsWith(QLatin1Char('.'))) {
        return false;
    }

    static const QRegularExpression namePattern(
        QStringLiteral("^(?=.{1,253}$)(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)(?:\\.(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?))*$"));
    return namePattern.match(host).hasMatch();
}

bool isValidIpv6(const QString &host)
{
    if (host.isEmpty() || host.contains(QLatin1Char('%'))) {
        return false;
    }
    QHostAddress address;
    return address.setAddress(host) && address.protocol() == QAbstractSocket::IPv6Protocol;
}

bool isValidIpv4(const QString &host)
{
    QHostAddress address;
    return address.setAddress(host) && address.protocol() == QAbstractSocket::IPv4Protocol;
}

Result<ServerEndpoint> parseEndpoint(const QString &value)
{
    if (value.isEmpty() || value.size() > 255 || hasForbiddenCharacters(value)) {
        return Result<ServerEndpoint>::failure(invalidEndpoint(QStringLiteral("invalid server characters")));
    }

    QString host;
    quint16 port = DefaultNtpPort;
    if (value.startsWith(QLatin1Char('['))) {
        const qsizetype close = value.indexOf(QLatin1Char(']'));
        if (close <= 1) {
            return Result<ServerEndpoint>::failure(invalidEndpoint(QStringLiteral("invalid bracketed address")));
        }
        host = value.mid(1, close - 1);
        const QString suffix = value.mid(close + 1);
        if (!suffix.isEmpty()) {
            if (!suffix.startsWith(QLatin1Char(':'))) {
                return Result<ServerEndpoint>::failure(invalidEndpoint(QStringLiteral("invalid address suffix")));
            }
            const Result<quint16> parsedPort = parsePort(suffix.mid(1));
            if (!parsedPort) {
                return Result<ServerEndpoint>::failure(parsedPort.error());
            }
            port = parsedPort.value();
        }
        if (!isValidIpv6(host)) {
            return Result<ServerEndpoint>::failure(invalidEndpoint(QStringLiteral("invalid IPv6 address")));
        }
    } else {
        const int colonCount = value.count(QLatin1Char(':'));
        if (colonCount > 1) {
            host = value;
            if (!isValidIpv6(host)) {
                return Result<ServerEndpoint>::failure(invalidEndpoint(QStringLiteral("IPv6 port must be bracketed")));
            }
        } else if (colonCount == 1) {
            const qsizetype separator = value.lastIndexOf(QLatin1Char(':'));
            host = value.left(separator);
            const Result<quint16> parsedPort = parsePort(value.mid(separator + 1));
            if (host.isEmpty() || !parsedPort) {
                return Result<ServerEndpoint>::failure(invalidEndpoint(QStringLiteral("invalid host or port")));
            }
            port = parsedPort.value();
        } else {
            host = value;
        }

        if (!isValidIpv4(host) && !isValidHostName(host)) {
            return Result<ServerEndpoint>::failure(invalidEndpoint(QStringLiteral("invalid host name")));
        }
    }

    ServerEndpoint endpoint;
    endpoint.original = value;
    endpoint.host = host;
    endpoint.port = port;
    endpoint.literalAddress = isValidIpv4(host) || isValidIpv6(host);
    return Result<ServerEndpoint>::success(std::move(endpoint));
}

} // namespace

QString ServerEndpoint::canonicalKey() const
{
    QHostAddress address;
    QString normalizedHost = host.toCaseFolded();
    if (address.setAddress(host)) {
        normalizedHost = address.toString().toCaseFolded();
    }
    return normalizedHost + QLatin1Char(':') + QString::number(port);
}

bool ServerEndpoint::isAliyunPreferred() const
{
    const QString normalized = host.toCaseFolded();
    return normalized == QStringLiteral("ntp1.aliyun.com")
        || normalized == QStringLiteral("ntp2.aliyun.com")
        || normalized == QStringLiteral("ntp3.aliyun.com")
        || normalized == QStringLiteral("ntp4.aliyun.com")
        || normalized == QStringLiteral("ntp5.aliyun.com")
        || normalized == QStringLiteral("ntp6.aliyun.com")
        || normalized == QStringLiteral("ntp7.aliyun.com");
}

Result<ServerEndpoint> parseServerEndpoint(const QString &value)
{
    return parseEndpoint(value);
}

Result<QStringList> validateServerList(const QStringList &values)
{
    if (values.isEmpty()) {
        return Result<QStringList>::failure(
            {ErrorCode::InvalidConfiguration, QStringLiteral("at least one server is required")});
    }

    QStringList normalizedValues;
    normalizedValues.reserve(values.size());
    QSet<QString> seen;
    for (const QString &value : values) {
        const Result<ServerEndpoint> parsed = parseEndpoint(value);
        if (!parsed) {
            return Result<QStringList>::failure(parsed.error());
        }
        const QString key = parsed.value().canonicalKey();
        if (seen.contains(key)) {
            return Result<QStringList>::failure(
                {ErrorCode::InvalidConfiguration, QStringLiteral("duplicate server")});
        }
        seen.insert(key);
        normalizedValues.append(value);
    }
    return Result<QStringList>::success(std::move(normalizedValues));
}

} // namespace TimeSync
