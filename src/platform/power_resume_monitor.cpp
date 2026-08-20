#include "power_resume_monitor.h"

#include <QCoreApplication>

#ifdef Q_OS_WIN
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

namespace TimeSync {

PowerResumeMonitor::PowerResumeMonitor(QObject *parent)
    : QObject(parent)
{
}

void PowerResumeMonitor::install(QCoreApplication *application)
{
    if (application == nullptr || installed_) {
        return;
    }
    application->installNativeEventFilter(this);
    installed_ = true;
}

void PowerResumeMonitor::uninstall(QCoreApplication *application)
{
    if (application == nullptr || !installed_) {
        return;
    }
    application->removeNativeEventFilter(this);
    installed_ = false;
}

bool PowerResumeMonitor::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    Q_UNUSED(result)
    if (eventType == QByteArrayLiteral("windows_generic_MSG") && message != nullptr) {
        const MSG *msg = static_cast<const MSG *>(message);
        if (msg->message == WM_POWERBROADCAST
            && (msg->wParam == PBT_APMRESUMEAUTOMATIC || msg->wParam == PBT_APMRESUMESUSPEND
                || msg->wParam == PBT_APMRESUMECRITICAL)) {
            emit resumed();
        }
    }
#else
    Q_UNUSED(eventType)
    Q_UNUSED(message)
    Q_UNUSED(result)
#endif
    return false;
}

} // namespace TimeSync
