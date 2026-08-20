#include "trusted_clock.h"

#include <QTimeZone>

namespace TimeSync {

TrustedClock::TrustedClock(const qint64 freshnessWindowMs, QObject *parent)
    : QObject(parent)
    , freshnessWindowMs_(qMax<qint64>(1, freshnessWindowMs))
{
}

void TrustedClock::calibrate(const NtpSample &sample)
{
    if (!sample.utcAtReceive.isValid()) {
        markRefreshFailed();
        return;
    }
    calibratedUtc_ = sample.utcAtReceive.toUTC();
    source_ = sample.source;
    anchor_.start();
    hasSample_ = true;
    refreshFailed_ = false;
    resumeInvalidated_ = false;
    emit changed();
}

void TrustedClock::markRefreshFailed()
{
    refreshFailed_ = true;
    emit changed();
}

void TrustedClock::markResume()
{
    if (hasSample_) {
        resumeInvalidated_ = true;
    }
    emit changed();
}

qint64 TrustedClock::calibrationAgeMilliseconds() const
{
    return hasSample_ && anchor_.isValid() ? qMax<qint64>(0, anchor_.elapsed()) : -1;
}

TrustedClockStatus TrustedClock::status() const
{
    if (!hasSample_) {
        return refreshFailed_ ? TrustedClockStatus::Error : TrustedClockStatus::Unavailable;
    }
    if (refreshFailed_) {
        return TrustedClockStatus::Error;
    }
    if (resumeInvalidated_ || calibrationAgeMilliseconds() > freshnessWindowMs_) {
        return TrustedClockStatus::Stale;
    }
    return TrustedClockStatus::Current;
}

QDateTime TrustedClock::utcNow() const
{
    if (!hasSample_) {
        return {};
    }
    return calibratedUtc_.addMSecs(calibrationAgeMilliseconds()).toUTC();
}

QTimeZone TrustedClock::shanghaiTimeZone()
{
    const QTimeZone named(QByteArrayLiteral("Asia/Shanghai"));
    if (named.isValid()) {
        return named;
    }
    return QTimeZone::fromSecondsAheadOfUtc(8 * 60 * 60);
}

QDateTime TrustedClock::shanghaiNow() const
{
    const QDateTime utc = utcNow();
    return utc.isValid() ? utc.toTimeZone(shanghaiTimeZone()) : QDateTime();
}

bool TrustedClock::isAvailable() const
{
    return hasSample_;
}

bool TrustedClock::isFresh() const
{
    return status() == TrustedClockStatus::Current;
}

TrustedClockState TrustedClock::state() const
{
    TrustedClockState result;
    result.status = status();
    result.available = hasSample_;
    result.fresh = result.status == TrustedClockStatus::Current;
    result.utc = utcNow();
    result.shanghai = shanghaiNow();
    result.source = source_;
    const qint64 age = calibrationAgeMilliseconds();
    result.calibrationAgeSeconds = age < 0 ? -1 : age / 1000;
    return result;
}

} // namespace TimeSync
