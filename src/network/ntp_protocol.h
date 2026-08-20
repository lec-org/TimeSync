#pragma once

#include "../core/result.h"
#include "../core/server_endpoint.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>

#include <QtNetwork/QHostAddress>

namespace TimeSync {

struct NtpTimestamp {
    quint32 seconds = 0;
    quint32 fraction = 0;

    [[nodiscard]] bool isZero() const noexcept { return seconds == 0 && fraction == 0; }
    friend bool operator==(const NtpTimestamp &, const NtpTimestamp &) = default;
};

struct NtpPacket {
    quint8 leapIndicator = 0;
    quint8 version = 4;
    quint8 mode = 4;
    quint8 stratum = 0;
    quint8 poll = 0;
    qint8 precision = 0;
    quint32 rootDelay = 0;
    quint32 rootDispersion = 0;
    quint32 referenceId = 0;
    NtpTimestamp referenceTimestamp;
    NtpTimestamp originateTimestamp;
    NtpTimestamp receiveTimestamp;
    NtpTimestamp transmitTimestamp;
};

struct NtpValidationContext {
    NtpTimestamp expectedOriginate;
    quint8 expectedVersion = 0;
    QList<QHostAddress> expectedAddresses;
    QHostAddress sourceAddress;
    quint16 expectedSourcePort = DefaultNtpPort;
    quint16 sourcePort = 0;
};

struct NtpSample {
    QDateTime utcAtReceive;
    QString source;
    QString remoteAddress;
    quint8 version = 4;
    qint64 offsetMilliseconds = 0;
    qint64 roundTripMilliseconds = 0;
};

class NtpCodec final {
public:
    static constexpr int PacketSize = 48;
    static constexpr quint8 MinimumVersion = 3;
    static constexpr quint8 MaximumVersion = 4;

    [[nodiscard]] static Result<QByteArray> encodeRequest(const NtpTimestamp &transmit,
                                                           quint8 version = MaximumVersion);
    [[nodiscard]] static Result<QByteArray> encodePacket(const NtpPacket &packet);
    [[nodiscard]] static Result<NtpPacket> decodePacket(const QByteArray &datagram);
    [[nodiscard]] static Result<NtpPacket> decodeAndValidate(const QByteArray &datagram,
                                                              const NtpValidationContext &context);

    [[nodiscard]] static Result<NtpSample> calculateSample(const NtpPacket &packet,
                                                            const NtpTimestamp &localTransmit,
                                                            const NtpTimestamp &localReceive,
                                                            const QString &source,
                                                            const QHostAddress &remoteAddress);

    [[nodiscard]] static NtpTimestamp fromDateTime(const QDateTime &utc);
    [[nodiscard]] static QDateTime toDateTime(const NtpTimestamp &timestamp);
    [[nodiscard]] static double secondsBetween(const NtpTimestamp &later,
                                               const NtpTimestamp &earlier);
};

} // namespace TimeSync

Q_DECLARE_METATYPE(TimeSync::NtpSample)
