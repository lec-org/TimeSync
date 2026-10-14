#include "ui_strings.h"

#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

namespace TimeSync::Ui {
namespace {

QString &activeLanguageTag()
{
    static QString tag = QStringLiteral("zh-CN");
    return tag;
}

QHash<QString, QString> &activeCatalog()
{
    static QHash<QString, QString> catalog;
    return catalog;
}

QString normalizedLanguageTag(const QString &languageTag)
{
    const QString normalized = languageTag.trimmed().replace(QLatin1Char('_'), QLatin1Char('-'));
    if (normalized.compare(QStringLiteral("en"), Qt::CaseInsensitive) == 0
        || normalized.startsWith(QStringLiteral("en-"), Qt::CaseInsensitive)) {
        return QStringLiteral("en-US");
    }
    if (normalized.compare(QStringLiteral("zh"), Qt::CaseInsensitive) == 0
        || normalized.startsWith(QStringLiteral("zh-"), Qt::CaseInsensitive)) {
        return QStringLiteral("zh-CN");
    }
    return normalized;
}

} // namespace

QString UiStrings::languageTag()
{
    if (activeCatalog().isEmpty()) {
        loadCatalog(activeLanguageTag());
    }
    return activeLanguageTag();
}

QStringList UiStrings::supportedLanguageTags()
{
    return {QStringLiteral("zh-CN"), QStringLiteral("en-US")};
}

bool UiStrings::setLanguageTag(const QString &languageTag)
{
    return loadCatalog(normalizedLanguageTag(languageTag));
}

QString UiStrings::text(const QString &key)
{
    if (activeCatalog().isEmpty()) {
        loadCatalog(activeLanguageTag());
    }

    const auto value = activeCatalog().constFind(key);
    if (value != activeCatalog().constEnd()) {
        return *value;
    }

    if (activeLanguageTag() != QStringLiteral("en-US")) {
        const QString previousTag = activeLanguageTag();
        const QHash<QString, QString> previousCatalog = activeCatalog();
        if (loadCatalog(QStringLiteral("en-US"))) {
            const QString fallback = activeCatalog().value(key, key);
            activeLanguageTag() = previousTag;
            activeCatalog() = previousCatalog;
            return fallback;
        }
    }

    return key;
}

bool UiStrings::loadCatalog(const QString &languageTag)
{
    if (!supportedLanguageTags().contains(languageTag)) {
        return false;
    }

    const QString resourceTag = languageTag == QStringLiteral("en-US")
        ? QStringLiteral("en")
        : languageTag;
    QFile file(QStringLiteral(":/i18n/%1.json").arg(resourceTag));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }

    QHash<QString, QString> catalog;
    const QJsonObject object = document.object();
    for (auto iterator = object.constBegin(); iterator != object.constEnd(); ++iterator) {
        if (iterator.value().isString()) {
            catalog.insert(iterator.key(), iterator.value().toString());
        }
    }

    if (catalog.isEmpty()) {
        return false;
    }

    // These UI-only strings live here because the startup setting has no config field.
    const bool english = languageTag == QStringLiteral("en-US");
    const auto addStartupText = [&catalog, english](const char *key,
                                                   const char *chinese,
                                                   const char *translation) {
        catalog.insert(QString::fromLatin1(key), QString::fromUtf8(english ? translation : chinese));
    };
    addStartupText("startupSync.enabled", "开机登录后同步一次", "Sync once after sign-in at startup");
    addStartupText("startupSync.description",
                   "登录后在后台同步一次，无需再次授权；与定时同步独立。",
                   "Sync once in the background after sign-in, without another authorization prompt; independent of scheduled sync.");
    addStartupText("startupSync.state.checking", "正在读取登录同步状态…", "Checking sign-in sync status…");
    addStartupText("startupSync.state.disabled", "登录同步已关闭", "Sign-in sync is off");
    addStartupText("startupSync.state.enabled", "登录同步已开启", "Sign-in sync is on");
    addStartupText("startupSync.state.updating", "正在更新登录同步设置…", "Updating sign-in sync…");
    addStartupText("startupSync.state.error",
                   "无法确认登录同步状态，此选项暂不可用。请重新打开应用重试。",
                   "Sign-in sync status could not be confirmed. This option is unavailable. Reopen the app to try again.");
    addStartupText("startupSync.state.updateFailed",
                   "更新失败，已显示当前设置。请重试。",
                   "The update failed. The current setting is shown. Please try again.");
    addStartupText("startupSync.operation.success", "登录同步设置已更新", "Sign-in sync setting updated");
    addStartupText("startupSync.operation.failure",
                   "无法更新登录同步设置，请检查下方状态后重试。",
                   "Sign-in sync could not be updated. Check the status below before trying again.");

    activeCatalog() = std::move(catalog);
    activeLanguageTag() = languageTag;
    return true;
}

} // namespace TimeSync::Ui
