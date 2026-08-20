#include "config_repository.h"

#include "server_endpoint.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace TimeSync {
namespace {

Error configurationError(const QString &detail, ErrorCode code = ErrorCode::InvalidConfiguration)
{
    return {code, detail};
}

bool isAsciiDigits(const QString &value)
{
    if (value.isEmpty()) {
        return false;
    }
    for (const QChar character : value) {
        if (character.unicode() < '0' || character.unicode() > '9') {
            return false;
        }
    }
    return true;
}

} // namespace

QString ConfigRepository::defaultPath()
{
    const QString executablePath = QCoreApplication::applicationFilePath();
    if (!executablePath.isEmpty()) {
        return QDir(QFileInfo(executablePath).absolutePath()).filePath(QStringLiteral("config.ini"));
    }
    return QDir::current().filePath(QStringLiteral("config.ini"));
}

QString ConfigRepository::resolvePath(const QString &requestedPath)
{
    const QString path = requestedPath.isEmpty() ? defaultPath() : requestedPath;
    return QFileInfo(path).absoluteFilePath();
}

QStringList ConfigRepository::defaultServers()
{
    return {
        QStringLiteral("ntp1.aliyun.com"),
        QStringLiteral("ntp2.aliyun.com"),
        QStringLiteral("ntp3.aliyun.com"),
        QStringLiteral("ntp4.aliyun.com"),
        QStringLiteral("ntp5.aliyun.com"),
        QStringLiteral("ntp6.aliyun.com"),
        QStringLiteral("ntp7.aliyun.com"),
        QStringLiteral("s1a.time.edu.cn"),
        QStringLiteral("s1b.time.edu.cn"),
        QStringLiteral("s1c.time.edu.cn"),
        QStringLiteral("s1d.time.edu.cn"),
        QStringLiteral("s1e.time.edu.cn"),
        QStringLiteral("s2a.time.edu.cn"),
        QStringLiteral("s2b.time.edu.cn"),
        QStringLiteral("s2c.time.edu.cn"),
        QStringLiteral("s2d.time.edu.cn"),
        QStringLiteral("s2e.time.edu.cn"),
        QStringLiteral("s2f.time.edu.cn"),
        QStringLiteral("s2g.time.edu.cn"),
        QStringLiteral("s2h.time.edu.cn"),
        QStringLiteral("s2j.time.edu.cn"),
        QStringLiteral("s2k.time.edu.cn"),
        QStringLiteral("s2m.time.edu.cn"),
    };
}

AppConfig ConfigRepository::defaultConfig()
{
    AppConfig config;
    config.servers = defaultServers();
    config.scheduleIntervalMinutes = 60;
    config.language = QStringLiteral("zh-CN");
    return config;
}

Result<void> ConfigRepository::validate(const AppConfig &config)
{
    const Result<QStringList> servers = validateServerList(config.servers);
    if (!servers) {
        return Result<void>::failure(servers.error());
    }
    if (config.scheduleIntervalMinutes < MinimumScheduleIntervalMinutes
        || config.scheduleIntervalMinutes > MaximumScheduleIntervalMinutes) {
        return Result<void>::failure(
            configurationError(QStringLiteral("schedule interval out of range")));
    }
    if (config.language != QStringLiteral("zh-CN") && config.language != QStringLiteral("en-US")) {
        return Result<void>::failure(configurationError(QStringLiteral("unsupported language")));
    }
    return Result<void>::success();
}

Result<AppConfig> ConfigRepository::parse(const QByteArray &bytes)
{
    if (bytes.isEmpty() || bytes.contains('\0')) {
        return Result<AppConfig>::failure(configurationError(QStringLiteral("empty or binary config")));
    }

    const QString text = QString::fromUtf8(bytes.constData(), bytes.size());
    if (text.toUtf8() != bytes) {
        return Result<AppConfig>::failure(configurationError(QStringLiteral("config is not UTF-8")));
    }

    QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    if (!lines.isEmpty() && lines.constLast().isEmpty()) {
        lines.removeLast();
    }
    if (lines.size() != 3) {
        return Result<AppConfig>::failure(configurationError(QStringLiteral("config must contain exactly three fields")));
    }

    QHash<QString, QString> fields;
    for (QString line : lines) {
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
        if (line.isEmpty()) {
            return Result<AppConfig>::failure(configurationError(QStringLiteral("blank config line")));
        }
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (equals <= 0 || line.indexOf(QLatin1Char('='), equals + 1) >= 0) {
            return Result<AppConfig>::failure(configurationError(QStringLiteral("invalid config field")));
        }
        const QString key = line.left(equals).trimmed();
        const QString value = line.mid(equals + 1).trimmed();
        if (key.isEmpty() || value.isEmpty()
            || (key != QStringLiteral("servers")
                && key != QStringLiteral("scheduleIntervalMinutes")
                && key != QStringLiteral("language"))
            || fields.contains(key)) {
            return Result<AppConfig>::failure(configurationError(QStringLiteral("invalid or duplicate config field")));
        }
        fields.insert(key, value);
    }

    if (!fields.contains(QStringLiteral("servers"))
        || !fields.contains(QStringLiteral("scheduleIntervalMinutes"))
        || !fields.contains(QStringLiteral("language"))) {
        return Result<AppConfig>::failure(configurationError(QStringLiteral("missing config field")));
    }

    const QStringList servers = fields.value(QStringLiteral("servers")).split(QLatin1Char(','), Qt::KeepEmptyParts);
    QStringList trimmedServers;
    trimmedServers.reserve(servers.size());
    for (const QString &server : servers) {
        const QString trimmed = server.trimmed();
        if (trimmed.isEmpty()) {
            return Result<AppConfig>::failure(configurationError(QStringLiteral("empty server")));
        }
        trimmedServers.append(trimmed);
    }

    const QString intervalText = fields.value(QStringLiteral("scheduleIntervalMinutes"));
    if (!isAsciiDigits(intervalText)) {
        return Result<AppConfig>::failure(configurationError(QStringLiteral("invalid schedule interval")));
    }
    bool converted = false;
    const int interval = intervalText.toInt(&converted, 10);
    if (!converted) {
        return Result<AppConfig>::failure(configurationError(QStringLiteral("invalid schedule interval")));
    }

    AppConfig config;
    config.servers = std::move(trimmedServers);
    config.scheduleIntervalMinutes = interval;
    config.language = fields.value(QStringLiteral("language"));
    const Result<void> validation = validate(config);
    if (!validation) {
        return Result<AppConfig>::failure(validation.error());
    }
    return Result<AppConfig>::success(std::move(config));
}

QByteArray ConfigRepository::serialize(const AppConfig &config)
{
    const QString text = QStringLiteral("servers = %1\n"
                                        "scheduleIntervalMinutes = %2\n"
                                        "language = %3\n")
                             .arg(config.servers.join(QStringLiteral(", ")))
                             .arg(config.scheduleIntervalMinutes)
                             .arg(config.language);
    return text.toUtf8();
}

Result<ConfigLoadResult> ConfigRepository::load(const QString &requestedPath) const
{
    const QString path = resolvePath(requestedPath);
    QFile file(path);
    if (!file.exists()) {
        const AppConfig config = defaultConfig();
        const Result<void> saveResult = save(path, config);
        if (!saveResult) {
            return Result<ConfigLoadResult>::failure(saveResult.error());
        }
        return Result<ConfigLoadResult>::success({config, path, true});
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return Result<ConfigLoadResult>::failure(
            configurationError(QStringLiteral("cannot read config"), ErrorCode::ConfigurationReadFailed));
    }

    const Result<AppConfig> parsed = parse(file.readAll());
    if (!parsed) {
        return Result<ConfigLoadResult>::failure(parsed.error());
    }
    return Result<ConfigLoadResult>::success({parsed.value(), path, false});
}

Result<void> ConfigRepository::save(const QString &requestedPath, const AppConfig &config) const
{
    const Result<void> validation = validate(config);
    if (!validation) {
        return validation;
    }

    const QString path = resolvePath(requestedPath);
    const QFileInfo info(path);
    if (info.fileName().isEmpty() || info.isDir()) {
        return Result<void>::failure(
            configurationError(QStringLiteral("invalid config path"), ErrorCode::ConfigurationWriteFailed));
    }

    if (info.exists()) {
        QFile existing(path);
        if (!existing.open(QIODevice::ReadOnly)) {
            return Result<void>::failure(
                configurationError(QStringLiteral("cannot validate existing config"),
                                   ErrorCode::ConfigurationWriteFailed));
        }
        const Result<AppConfig> existingConfig = parse(existing.readAll());
        if (!existingConfig) {
            return Result<void>::failure(
                configurationError(QStringLiteral("existing config is invalid; refusing overwrite")));
        }
    }

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        return Result<void>::failure(
            configurationError(QStringLiteral("cannot open config for atomic write"),
                               ErrorCode::ConfigurationWriteFailed));
    }
    const QByteArray bytes = serialize(config);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        return Result<void>::failure(
            configurationError(QStringLiteral("atomic config commit failed"),
                               ErrorCode::ConfigurationWriteFailed));
    }
    return Result<void>::success();
}

} // namespace TimeSync
