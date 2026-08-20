#pragma once

#include "result.h"

#include <QString>
#include <QStringList>

namespace TimeSync {

struct AppConfig {
    QStringList servers;
    int scheduleIntervalMinutes = 60;
    QString language = QStringLiteral("zh-CN");
};

struct ConfigLoadResult {
    AppConfig config;
    QString path;
    bool created = false;
};

class ConfigRepository final {
public:
    static constexpr int MinimumScheduleIntervalMinutes = 1;
    static constexpr int MaximumScheduleIntervalMinutes = 10080;

    [[nodiscard]] static QString defaultPath();
    [[nodiscard]] static QString resolvePath(const QString &requestedPath);
    [[nodiscard]] static AppConfig defaultConfig();
    [[nodiscard]] static QStringList defaultServers();

    [[nodiscard]] Result<ConfigLoadResult> load(const QString &requestedPath = {}) const;
    [[nodiscard]] Result<void> save(const QString &requestedPath, const AppConfig &config) const;

    [[nodiscard]] static Result<void> validate(const AppConfig &config);

private:
    [[nodiscard]] static Result<AppConfig> parse(const QByteArray &bytes);
    [[nodiscard]] static QByteArray serialize(const AppConfig &config);
};

} // namespace TimeSync
