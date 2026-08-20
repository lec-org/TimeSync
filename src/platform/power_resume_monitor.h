#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>

class QCoreApplication;

namespace TimeSync {

class PowerResumeMonitor final : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    explicit PowerResumeMonitor(QObject *parent = nullptr);

    void install(QCoreApplication *application);
    void uninstall(QCoreApplication *application);

    [[nodiscard]] bool isInstalled() const noexcept { return installed_; }

signals:
    void resumed();

protected:
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    bool installed_ = false;
};

} // namespace TimeSync
