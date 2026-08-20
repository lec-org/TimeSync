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

    activeCatalog() = std::move(catalog);
    activeLanguageTag() = languageTag;
    return true;
}

} // namespace TimeSync::Ui
