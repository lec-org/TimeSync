#pragma once

#include <QApplication>
#include <QPixmap>
#include <QString>

namespace TimeSync::Ui {

inline QPixmap brandLogoPixmap(const int logicalHeight)
{
    const QPixmap source(QStringLiteral(":/branding/lec-logo.png"));
    if (source.isNull() || logicalHeight <= 0) {
        return {};
    }

    const qreal dpr = qApp != nullptr ? qApp->devicePixelRatio() : 1.0;
    const int pixelHeight = qMax(1, qRound(static_cast<qreal>(logicalHeight) * dpr));
    QPixmap scaled = source.scaledToHeight(pixelHeight, Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(dpr);
    return scaled;
}

} // namespace TimeSync::Ui
