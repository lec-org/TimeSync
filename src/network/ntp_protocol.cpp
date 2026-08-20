#include "ntp_protocol.h"

#include <QTimeZone>
#include <QtGlobal>

#include <cmath>

namespace TimeSync {
namespace {

constexpr qint64 NtpEpochOffsetSeconds = 2208988800LL;
constexpr long double NtpFractionScale = 4294967296.0L;

Error protocolError(const QString &detail)
{
    return {ErrorCode::ProtocolRejected, detail};
}

void putUInt32(QByteArray &bytes, const int offset, const quint32 value)
{
    bytes[offset] = static_cast<char>((value >> 24) & 0xff);
    bytes[offset + 1] = static_cast<char>((value >> 16) & 0xff);
    bytes[offset + 2] = static_cast<char>((value >> 8) & 0xff);
    bytes[offset + 3] = static_cast<char>(value & 0xff);
}

quint32 getUInt32(const QByteArray &bytes, const int offset)
{
    return (static_cast<quint32>(static_cast<quint8>(bytes.at(offset))) << 24)
        | (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 1))) << 16)
        | (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 2))) << 8)
        | static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 3)));
}

void putTimestamp(QByteArray &bytes, const int offset, const NtpTimestamp &timestamp)
{
    putUInt32(bytes, offset, timestamp.seconds);
    putUInt32(bytes, offset + 4, timestamp.fraction);
}

NtpTimestamp getTimestamp(const QByteArray &bytes, const int offset)
{
    return {getUInt32(bytes, offset), getUInt32(bytes, offset + 4)};
}

quint64 rawTimestamp(const NtpTimestamp &timestamp)
{
    return (static_cast<quint64>(timestamp.seconds) << 32) | timestamp.fraction;
}

} // namespace

Result<QByteArray> NtpCodec::encodeRequest(const NtpTimestamp &transmit, const quint8 version)
{
    if (version < MinimumVersion || version > MaximumVersion || transmit.isZero()) {
        return Result<QByteArray>::failure(protocolError(QStringLiteral("invalid NTP request")));
    }

    NtpPacket packet;
    packet.version = version;
    packet.mode = 3;
    packet.transmitTimestamp = transmit;
    return encodePacket(packet);
}

Result<QByteArray> NtpCodec::encodePacket(const NtpPacket &packet)
{
    if (packet.version < MinimumVersion || packet.version > MaximumVersion || packet.mode > 7) {
        return Result<QByteArray>::failure(protocolError(QStringLiteral("invalid NTP header")));
    }

    QByteArray bytes(PacketSize, Qt::Uninitialized);
    bytes[0] = static_cast<char>(((packet.leapIndicator & 0x3) << 6)
                                  | ((packet.version & 0x7) << 3) | (packet.mode & 0x7));
    bytes[1] = static_cast<char>(packet.stratum);
    bytes[2] = static_cast<char>(packet.poll);
    bytes[3] = static_cast<char>(packet.precision);
    putUInt32(bytes, 4, packet.rootDelay);
    putUInt32(bytes, 8, packet.rootDispersion);
    putUInt32(bytes, 12, packet.referenceId);
    putTimestamp(bytes, 16, packet.referenceTimestamp);
    putTimestamp(bytes, 24, packet.originateTimestamp);
    putTimestamp(bytes, 32, packet.receiveTimestamp);
    putTimestamp(bytes, 40, packet.transmitTimestamp);
    return Result<QByteArray>::success(std::move(bytes));
}

Result<NtpPacket> NtpCodec::decodePacket(const QByteArray &datagram)
{
    if (datagram.size() < PacketSize || datagram.size() > 4096) {
        return Result<NtpPacket>::failure(protocolError(QStringLiteral("invalid NTP packet length")));
    }

    const quint8 first = static_cast<quint8>(datagram.at(0));
    NtpPacket packet;
    packet.leapIndicator = (first >> 6) & 0x3;
    packet.version = (first >> 3) & 0x7;
    packet.mode = first & 0x7;
    packet.stratum = static_cast<quint8>(datagram.at(1));
    packet.poll = static_cast<quint8>(datagram.at(2));
    packet.precision = static_cast<qint8>(datagram.at(3));
    packet.rootDelay = getUInt32(datagram, 4);
    packet.rootDispersion = getUInt32(datagram, 8);
    packet.referenceId = getUInt32(datagram, 12);
    packet.referenceTimestamp = getTimestamp(datagram, 16);
    packet.originateTimestamp = getTimestamp(datagram, 24);
    packet.receiveTimestamp = getTimestamp(datagram, 32);
    packet.transmitTimestamp = getTimestamp(datagram, 40);
    return Result<NtpPacket>::success(std::move(packet));
}

Result<NtpPacket> NtpCodec::decodeAndValidate(const QByteArray &datagram,
                                              const NtpValidationContext &context)
{
    const Result<NtpPacket> decoded = decodePacket(datagram);
    if (!decoded) {
        return decoded;
    }
    const NtpPacket &packet = decoded.value();
    if (packet.leapIndicator == 3) {
        return Result<NtpPacket>::failure(protocolError(QStringLiteral("NTP clock alarm")));
    }
    if (packet.version < MinimumVersion || packet.version > MaximumVersion
        || (context.expectedVersion != 0 && packet.version != context.expectedVersion)) {
        return Result<NtpPacket>::failure(protocolError(QStringLiteral("unsupported NTP version")));
    }
    if (packet.mode != 4) {
        return Result<NtpPacket>::failure(protocolError(QStringLiteral("unexpected NTP mode")));
    }
    if (packet.stratum == 0 || packet.stratum >= 16) {
        return Result<NtpPacket>::failure(protocolError(QStringLiteral("invalid NTP stratum")));
    }
    if (packet.originateTimestamp != context.expectedOriginate) {
        return Result<NtpPacket>::failure(protocolError(QStringLiteral("originate timestamp mismatch")));
    }
    if (packet.receiveTimestamp.isZero() || packet.transmitTimestamp.isZero()) {
        return Result<NtpPacket>::failure(protocolError(QStringLiteral("missing NTP timestamps")));
    }
    if (context.sourcePort != 0 && context.sourcePort != context.expectedSourcePort) {
        return Result<NtpPacket>::failure(protocolError(QStringLiteral("unexpected NTP source port")));
    }
    if (!context.expectedAddresses.isEmpty()) {
        bool addressMatched = false;
        for (const QHostAddress &expected : context.expectedAddresses) {
            if (expected == context.sourceAddress) {
                addressMatched = true;
                break;
            }
        }
        if (!addressMatched) {
            return Result<NtpPacket>::failure(protocolError(QStringLiteral("unexpected NTP source address")));
        }
    }
    return decoded;
}

double NtpCodec::secondsBetween(const NtpTimestamp &later, const NtpTimestamp &earlier)
{
    const quint64 difference = rawTimestamp(later) - rawTimestamp(earlier);
    return static_cast<double>(static_cast<qint64>(difference)) / static_cast<double>(NtpFractionScale);
}

Result<NtpSample> NtpCodec::calculateSample(const NtpPacket &packet,
                                            const NtpTimestamp &localTransmit,
                                            const NtpTimestamp &localReceive,
                                            const QString &source,
                                            const QHostAddress &remoteAddress)
{
    if (localTransmit.isZero() || localReceive.isZero() || packet.receiveTimestamp.isZero()
        || packet.transmitTimestamp.isZero()) {
        return Result<NtpSample>::failure(protocolError(QStringLiteral("incomplete NTP sample")));
    }

    const double offsetSeconds = (secondsBetween(packet.receiveTimestamp, localTransmit)
                                  + secondsBetween(packet.transmitTimestamp, localReceive))
        / 2.0;
    const double delaySeconds = secondsBetween(localReceive, localTransmit)
        - secondsBetween(packet.transmitTimestamp, packet.receiveTimestamp);
    if (!std::isfinite(offsetSeconds) || !std::isfinite(delaySeconds) || delaySeconds < -1.0) {
        return Result<NtpSample>::failure(protocolError(QStringLiteral("invalid NTP timing")));
    }

    const qint64 offsetMilliseconds = qRound64(offsetSeconds * 1000.0);
    const qint64 delayMilliseconds = qRound64(delaySeconds * 1000.0);
    const QDateTime serverTransmit = toDateTime(packet.transmitTimestamp);
    const QDateTime localReceiveTime = toDateTime(localReceive);
    if (!serverTransmit.isValid() || !localReceiveTime.isValid()) {
        return Result<NtpSample>::failure(protocolError(QStringLiteral("invalid NTP transmit time")));
    }

    NtpSample sample;
    sample.utcAtReceive = localReceiveTime.addMSecs(offsetMilliseconds).toUTC();
    sample.source = source;
    sample.remoteAddress = remoteAddress.toString();
    sample.version = packet.version;
    sample.offsetMilliseconds = offsetMilliseconds;
    sample.roundTripMilliseconds = delayMilliseconds;
    return Result<NtpSample>::success(std::move(sample));
}

NtpTimestamp NtpCodec::fromDateTime(const QDateTime &utc)
{
    const QDateTime normalized = utc.toUTC();
    if (!normalized.isValid()) {
        return {};
    }
    const qint64 seconds = normalized.toSecsSinceEpoch() + NtpEpochOffsetSeconds;
    const qint64 milliseconds = normalized.time().msecsSinceStartOfDay();
    const quint64 fraction = (static_cast<quint64>(milliseconds % 1000) << 32) / 1000;
    return {static_cast<quint32>(seconds), static_cast<quint32>(fraction)};
}

QDateTime NtpCodec::toDateTime(const NtpTimestamp &timestamp)
{
    if (timestamp.isZero()) {
        return {};
    }
    const qint64 unixSeconds = static_cast<qint64>(timestamp.seconds) - NtpEpochOffsetSeconds;
    QDateTime result = QDateTime::fromSecsSinceEpoch(unixSeconds, QTimeZone(QTimeZone::UTC));
    const qint64 milliseconds =
        static_cast<qint64>((static_cast<quint64>(timestamp.fraction) * 1000 + 0x80000000ULL) >> 32);
    if (milliseconds >= 1000) {
        result = result.addSecs(1);
    } else {
        result = result.addMSecs(milliseconds);
    }
    return result;
}

} // namespace TimeSync
