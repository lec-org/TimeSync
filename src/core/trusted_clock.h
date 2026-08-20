#pragma once

#include "../network/ntp_protocol.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QMetaType>
#include <QObject>

namespace TimeSync {

enum class TrustedClockStatus {
    Unavailable = 0,
    Current,
    Stale,
    Error,
};

struct TrustedClockState {
    TrustedClockStatus status = TrustedClockStatus::Unavailable;
    bool available = false;
    bool fresh = false;
    QDateTime utc;
    QDateTime shanghai;
    QString source;
    qint64 calibrationAgeSeconds = -1;
};

class TrustedClock final : public QObject {
    Q_OBJECT

public:
    static constexpr qint64 DefaultFreshnessWindowMs = 15 * 60 * 1000;

    explicit TrustedClock(qint64 freshnessWindowMs = DefaultFreshnessWindowMs,
                          QObject *parent = nullptr);

    void calibrate(const NtpSample &sample);
    void markRefreshFailed();
    void markResume();

    [[nodiscard]] TrustedClockState state() const;
    [[nodiscard]] QDateTime utcNow() const;
    [[nodiscard]] QDateTime shanghaiNow() const;
    [[nodiscard]] bool isAvailable() const;
    [[nodiscard]] bool isFresh() const;
    [[nodiscard]] qint64 calibrationAgeMilliseconds() const;
    [[nodiscard]] QString source() const { return source_; }

signals:
    void changed();

private:
    [[nodiscard]] TrustedClockStatus status() const;
    [[nodiscard]] static QTimeZone shanghaiTimeZone();

    QDateTime calibratedUtc_;
    QElapsedTimer anchor_;
    QString source_;
    qint64 freshnessWindowMs_ = DefaultFreshnessWindowMs;
    bool hasSample_ = false;
    bool refreshFailed_ = false;
    bool resumeInvalidated_ = false;
};

} // namespace TimeSync

Q_DECLARE_METATYPE(TimeSync::TrustedClockState)
