#pragma once

#include <QString>
#include <QStringList>

namespace TimeSync::Ui {

class UiStrings final {
public:
    static QString languageTag();
    static QStringList supportedLanguageTags();
    static bool setLanguageTag(const QString &languageTag);
    static QString text(const QString &key);

private:
    static bool loadCatalog(const QString &languageTag);
};

} // namespace TimeSync::Ui
