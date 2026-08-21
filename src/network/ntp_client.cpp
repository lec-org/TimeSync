#include "ntp_client.h"

#include "../core/server_endpoint.h"

#include <QElapsedTimer>
#include <QHash>
#include <QHostInfo>
#include <QMetaObject>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>

#include <algorithm>

namespace TimeSync {

QHostAddress normalizedNtpAddress(const QHostAddress &address)
{
    bool isIPv4 = false;
    const quint32 ipv4 = address.toIPv4Address(&isIPv4);
    return isIPv4 ? QHostAddress(ipv4) : address;
}

bool ntpAddressesEquivalent(const QHostAddress &left, const QHostAddress &right)
{
    return normalizedNtpAddress(left) == normalizedNtpAddress(right);
}

NtpSocketFamily ntpSocketFamilyForAddress(const QHostAddress &address,
                                          const NtpSocketAvailability &availability) noexcept
{
    const QHostAddress normalized = normalizedNtpAddress(address);
    if (normalized.protocol() == QAbstractSocket::IPv4Protocol) {
        return availability.ipv4 ? NtpSocketFamily::IPv4 : NtpSocketFamily::Unavailable;
    }
    if (normalized.protocol() == QAbstractSocket::IPv6Protocol) {
        return availability.ipv6 ? NtpSocketFamily::IPv6 : NtpSocketFamily::Unavailable;
    }
    return NtpSocketFamily::Unavailable;
}

QString ntpFailureKindName(const NtpFailureKind kind)
{
    switch (kind) {
    case NtpFailureKind::None:
        return QStringLiteral("none");
    case NtpFailureKind::InvalidConfiguration:
        return QStringLiteral("invalid-configuration");
    case NtpFailureKind::NetworkUnavailable:
        return QStringLiteral("network-unavailable");
    case NtpFailureKind::SourceTimeout:
        return QStringLiteral("source-timeout");
    case NtpFailureKind::ProtocolRejected:
        return QStringLiteral("protocol-rejected");
    case NtpFailureKind::NoValidSource:
        return QStringLiteral("no-valid-source");
    case NtpFailureKind::Cancelled:
        return QStringLiteral("cancelled");
    case NtpFailureKind::Busy:
        return QStringLiteral("busy");
    case NtpFailureKind::InternalError:
        return QStringLiteral("internal-error");
    }
    return QStringLiteral("internal-error");
}

QString ntpFailureDiagnostic(const NtpFailure &failure)
{
    return QStringLiteral("category=%1 attempts=%2 invalid=%3 dns=%4 send=%5 socket=%6 bind=%7 timeout=%8 family=%9")
        .arg(ntpFailureKindName(failure.kind))
        .arg(failure.sourcesTried)
        .arg(failure.invalidResponses)
        .arg(failure.dnsFailures)
        .arg(failure.sendFailures)
        .arg(failure.socketErrors)
        .arg(failure.bindFailures)
        .arg(failure.timeouts)
        .arg(failure.unavailableFamilies);
}

class NtpClient::Worker final : public QObject {
    Q_OBJECT

public:
    explicit Worker(QObject *parent = nullptr)
        : QObject(parent)
    {
        ipv4Socket_ = new QUdpSocket(this);
        ipv6Socket_ = new QUdpSocket(this);
        connect(ipv4Socket_, &QUdpSocket::readyRead, this, [this] {
            readPendingDatagrams(ipv4Socket_, NtpSocketFamily::IPv4);
        });
        connect(ipv6Socket_, &QUdpSocket::readyRead, this, [this] {
            readPendingDatagrams(ipv6Socket_, NtpSocketFamily::IPv6);
        });
        connect(ipv4Socket_, &QUdpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
            if (active_) {
                ++socketErrors_;
            }
        });
        connect(ipv6Socket_, &QUdpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
            if (active_) {
                ++socketErrors_;
            }
        });

        phaseTimer_ = new QTimer(this);
        globalTimer_ = new QTimer(this);
        phaseTimer_->setSingleShot(true);
        globalTimer_->setSingleShot(true);
        connect(phaseTimer_, &QTimer::timeout, this, &Worker::phaseTimedOut);
        connect(globalTimer_, &QTimer::timeout, this, &Worker::globalTimedOut);
    }

public slots:
    void beginQuery(quint64 generation,
                    const QList<ServerEndpoint> &sources,
                    const NtpQueryOptions &options)
    {
        stopRuntime();
        generation_ = generation;
        options_ = options;
        if (options_.perSourceTimeoutMs < 100 || options_.perSourceTimeoutMs > 30000
            || options_.globalTimeoutMs < options_.perSourceTimeoutMs
            || options_.globalTimeoutMs > 60000 || options_.versions.isEmpty()) {
            emit finished(generation_, {false, {}, {NtpFailureKind::InvalidConfiguration}});
            return;
        }

        QList<ServerEndpoint> validSources;
        for (const ServerEndpoint &source : sources) {
            if (!source.host.isEmpty() && source.port != 0) {
                validSources.append(source);
            }
        }
        if (validSources.isEmpty()) {
            emit finished(generation_, {false, {}, {NtpFailureKind::InvalidConfiguration}});
            return;
        }

        int id = 0;
        for (const ServerEndpoint &endpoint : validSources) {
            SourceState state;
            state.id = id;
            state.endpoint = endpoint;
            state.preferred = endpoint.isAliyunPreferred();
            sources_.insert(id, state);
            if (state.preferred) {
                preferredIds_.append(id);
            } else {
                fallbackIds_.append(id);
            }
            ++id;
        }

        resetDiagnostics();
        active_ = true;
        socketAvailability_.ipv4 = ipv4Socket_->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress);
        if (!socketAvailability_.ipv4) {
            ++bindFailures_;
        }
        socketAvailability_.ipv6 = ipv6Socket_->bind(QHostAddress::AnyIPv6, 0, QUdpSocket::ShareAddress);
        if (!socketAvailability_.ipv6) {
            ++bindFailures_;
        }
        if (!socketAvailability_.ipv4 && !socketAvailability_.ipv6) {
            finishFailure(NtpFailureKind::NetworkUnavailable);
            return;
        }

        usedTimestamps_.clear();
        globalTimer_->start(options_.globalTimeoutMs);
        if (!preferredIds_.isEmpty()) {
            startPhase(preferredIds_, true);
        } else {
            startPhase(fallbackIds_, false);
        }
    }

    void cancelQuery(quint64 generation)
    {
        if (!active_ || generation != generation_) {
            return;
        }
        const NtpFailure failure = currentFailure(NtpFailureKind::Cancelled);
        stopRuntime();
        emit finished(generation_, {false, {}, failure});
    }

    void shutdown()
    {
        stopRuntime();
    }

signals:
    void finished(quint64 generation, const TimeSync::NtpQueryResult &result);

private:
    struct SourceState {
        int id = -1;
        ServerEndpoint endpoint;
        bool preferred = false;
        bool completed = false;
        int lookupId = -1;
        int pendingRequests = 0;
    };

    struct PendingRequest {
        quint64 id = 0;
        int sourceId = -1;
        QHostAddress address;
        quint16 port = DefaultNtpPort;
        quint8 version = NtpCodec::MaximumVersion;
        NtpTimestamp transmit;
        NtpSocketFamily socketFamily = NtpSocketFamily::Unavailable;
    };

    void resetDiagnostics()
    {
        sourcesTried_ = 0;
        invalidResponses_ = 0;
        dnsFailures_ = 0;
        sendFailures_ = 0;
        socketErrors_ = 0;
        bindFailures_ = 0;
        timeouts_ = 0;
        unavailableFamilies_ = 0;
        resolvedAddresses_ = 0;
        usableAddresses_ = 0;
    }

    NtpFailure currentFailure(const NtpFailureKind kind) const
    {
        return {kind,
                sourcesTried_,
                invalidResponses_,
                dnsFailures_,
                sendFailures_,
                socketErrors_,
                bindFailures_,
                timeouts_,
                unavailableFamilies_};
    }

    void stopRuntime()
    {
        phaseTimer_->stop();
        globalTimer_->stop();
        active_ = false;
        ipv4Socket_->close();
        ipv6Socket_->close();
        for (auto iterator = sources_.begin(); iterator != sources_.end(); ++iterator) {
            if (iterator->lookupId >= 0) {
                QHostInfo::abortHostLookup(iterator->lookupId);
                iterator->lookupId = -1;
            }
        }
        pending_.clear();
        activePhaseIds_.clear();
        sources_.clear();
        preferredIds_.clear();
        fallbackIds_.clear();
        socketAvailability_ = {};
    }

    void startPhase(const QList<int> &ids, const bool preferred)
    {
        activePhaseIds_.clear();
        phasePreferred_ = preferred;
        ++phaseSerial_;
        if (ids.isEmpty()) {
            if (preferred && !fallbackIds_.isEmpty()) {
                startPhase(fallbackIds_, false);
            } else {
                finishFailure(NtpFailureKind::NoValidSource);
            }
            return;
        }

        const int remainingGlobal = qMax(1, static_cast<int>(globalTimer_->remainingTime()));
        phaseTimer_->start(qMin(remainingGlobal, options_.perSourceTimeoutMs + 100));
        for (const int id : ids) {
            if (!sources_.contains(id)) {
                continue;
            }
            SourceState &state = sources_[id];
            state.completed = false;
            state.pendingRequests = 0;
            activePhaseIds_.insert(id);
            ++sourcesTried_;
            resolveSource(id, phaseSerial_);
            const quint64 generation = generation_;
            const int phase = phaseSerial_;
            QTimer::singleShot(options_.perSourceTimeoutMs, this, [this, generation, phase, id] {
                if (active_ && generation == generation_ && phase == phaseSerial_) {
                    completeSource(id, true);
                }
            });
        }
        if (activePhaseIds_.isEmpty()) {
            phaseTimedOut();
        }
    }

    void resolveSource(const int sourceId, const int phase)
    {
        SourceState &state = sources_[sourceId];
        QHostAddress literal;
        if (literal.setAddress(state.endpoint.host)) {
            sendToAddresses(sourceId, phase, {literal});
            return;
        }

        const quint64 generation = generation_;
        state.lookupId = QHostInfo::lookupHost(
            state.endpoint.host,
            this,
            [this, generation, phase, sourceId](const QHostInfo &info) {
                if (!active_ || generation != generation_ || phase != phaseSerial_
                    || !sources_.contains(sourceId)) {
                    return;
                }
                sources_[sourceId].lookupId = -1;
                if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
                    ++dnsFailures_;
                    completeSource(sourceId);
                    return;
                }
                sendToAddresses(sourceId, phase, info.addresses());
            });
    }

    void sendToAddresses(const int sourceId, const int phase, const QList<QHostAddress> &addresses)
    {
        if (!active_ || phase != phaseSerial_ || !sources_.contains(sourceId)) {
            return;
        }
        SourceState &state = sources_[sourceId];
        QList<QHostAddress> uniqueAddresses;
        for (const QHostAddress &address : addresses) {
            const QHostAddress normalized = normalizedNtpAddress(address);
            if (!normalized.isNull() && !uniqueAddresses.contains(normalized)) {
                uniqueAddresses.append(normalized);
            }
        }
        constexpr int MaximumAddressesPerSource = 8;
        if (uniqueAddresses.size() > MaximumAddressesPerSource) {
            uniqueAddresses.erase(uniqueAddresses.begin() + MaximumAddressesPerSource,
                                  uniqueAddresses.end());
        }

        for (const QHostAddress &address : uniqueAddresses) {
            ++resolvedAddresses_;
            const NtpSocketFamily socketFamily = ntpSocketFamilyForAddress(address, socketAvailability_);
            QUdpSocket *socket = socketFamily == NtpSocketFamily::IPv4
                ? ipv4Socket_
                : (socketFamily == NtpSocketFamily::IPv6 ? ipv6Socket_ : nullptr);
            if (socket == nullptr) {
                ++unavailableFamilies_;
                continue;
            }
            ++usableAddresses_;
            for (const quint8 version : options_.versions) {
                if (version < NtpCodec::MinimumVersion || version > NtpCodec::MaximumVersion) {
                    continue;
                }
                const NtpTimestamp transmit = nextUniqueTimestamp();
                const Result<QByteArray> request = NtpCodec::encodeRequest(transmit, version);
                if (!request) {
                    continue;
                }
                const qint64 written = socket->writeDatagram(request.value(), address, state.endpoint.port);
                if (written != request.value().size()) {
                    ++sendFailures_;
                    continue;
                }
                PendingRequest pending;
                pending.id = nextRequestId_++;
                pending.sourceId = sourceId;
                pending.address = address;
                pending.port = state.endpoint.port;
                pending.version = version;
                pending.transmit = transmit;
                pending.socketFamily = socketFamily;
                pending_.insert(pending.id, pending);
                ++state.pendingRequests;
            }
        }

        if (state.pendingRequests == 0) {
            const quint64 generation = generation_;
            QTimer::singleShot(0, this, [this, generation, phase, sourceId] {
                if (active_ && generation == generation_ && phase == phaseSerial_) {
                    completeSource(sourceId);
                }
            });
        }
    }

    NtpTimestamp nextUniqueTimestamp()
    {
        NtpTimestamp timestamp = NtpCodec::fromDateTime(QDateTime::currentDateTimeUtc());
        if (timestamp.isZero()) {
            timestamp = {1, 1};
        }
        quint64 raw = (static_cast<quint64>(timestamp.seconds) << 32) | timestamp.fraction;
        while (usedTimestamps_.contains(raw)) {
            ++raw;
        }
        usedTimestamps_.insert(raw);
        return {static_cast<quint32>(raw >> 32), static_cast<quint32>(raw & 0xffffffffULL)};
    }

    void readPendingDatagrams(QUdpSocket *socket, const NtpSocketFamily receivingFamily)
    {
        while (socket->hasPendingDatagrams() && active_) {
            QByteArray datagram;
            datagram.resize(static_cast<int>(socket->pendingDatagramSize()));
            QHostAddress address;
            quint16 port = 0;
            if (socket->readDatagram(datagram.data(), datagram.size(), &address, &port) < 0) {
                ++socketErrors_;
                continue;
            }
            address = normalizedNtpAddress(address);

            auto matching = pending_.end();
            for (auto iterator = pending_.begin(); iterator != pending_.end(); ++iterator) {
                if (iterator->socketFamily == receivingFamily
                    && ntpAddressesEquivalent(iterator->address, address) && iterator->port == port) {
                    const Result<NtpPacket> decoded = NtpCodec::decodePacket(datagram);
                    if (decoded && decoded.value().originateTimestamp == iterator->transmit) {
                        matching = iterator;
                        break;
                    }
                }
            }
            if (matching == pending_.end()) {
                continue;
            }

            const PendingRequest request = matching.value();
            SourceState &state = sources_[request.sourceId];
            NtpValidationContext context;
            context.expectedOriginate = request.transmit;
            context.expectedVersion = request.version;
            context.expectedAddresses = {request.address};
            context.sourceAddress = address;
            context.expectedSourcePort = request.port;
            context.sourcePort = port;
            const Result<NtpPacket> validated = NtpCodec::decodeAndValidate(datagram, context);
            if (!validated) {
                ++invalidResponses_;
                continue;
            }

            const NtpTimestamp receivedAt = NtpCodec::fromDateTime(QDateTime::currentDateTimeUtc());
            const Result<NtpSample> sample = NtpCodec::calculateSample(
                validated.value(), request.transmit, receivedAt, state.endpoint.original, address);
            if (!sample) {
                ++invalidResponses_;
                continue;
            }
            finishSuccess(sample.value());
            return;
        }
    }

    void completeSource(const int sourceId, const bool timedOut = false)
    {
        if (!active_ || !sources_.contains(sourceId)) {
            return;
        }
        SourceState &state = sources_[sourceId];
        if (state.completed) {
            return;
        }
        if (timedOut) {
            ++timeouts_;
        }
        state.completed = true;
        if (state.lookupId >= 0) {
            QHostInfo::abortHostLookup(state.lookupId);
            state.lookupId = -1;
        }
        for (auto iterator = pending_.begin(); iterator != pending_.end();) {
            if (iterator->sourceId == sourceId) {
                iterator = pending_.erase(iterator);
            } else {
                ++iterator;
            }
        }
        activePhaseIds_.remove(sourceId);
        if (!activePhaseIds_.isEmpty()) {
            return;
        }

        phaseTimer_->stop();
        if (phasePreferred_ && !fallbackIds_.isEmpty()) {
            startPhase(fallbackIds_, false);
        } else {
            finishFailure(invalidResponses_ > 0 ? NtpFailureKind::ProtocolRejected
                                                 : NtpFailureKind::NoValidSource);
        }
    }

    void phaseTimedOut()
    {
        const QList<int> ids = activePhaseIds_.values();
        for (const int id : ids) {
            completeSource(id, true);
        }
    }

    void globalTimedOut()
    {
        ++timeouts_;
        finishFailure(invalidResponses_ > 0 ? NtpFailureKind::ProtocolRejected
                                             : NtpFailureKind::SourceTimeout);
    }

    void finishSuccess(const NtpSample &sample)
    {
        const quint64 generation = generation_;
        stopRuntime();
        NtpQueryResult result;
        result.succeeded = true;
        result.sample = sample;
        emit finished(generation, result);
    }

    void finishFailure(NtpFailureKind kind)
    {
        if (!active_) {
            return;
        }
        if ((kind == NtpFailureKind::NoValidSource || kind == NtpFailureKind::SourceTimeout)
            && resolvedAddresses_ > 0 && usableAddresses_ == 0) {
            kind = NtpFailureKind::NetworkUnavailable;
        }
        const quint64 generation = generation_;
        const NtpFailure failure = currentFailure(kind);
        stopRuntime();
        emit finished(generation, {false, {}, failure});
    }

    QUdpSocket *ipv4Socket_ = nullptr;
    QUdpSocket *ipv6Socket_ = nullptr;
    QTimer *phaseTimer_ = nullptr;
    QTimer *globalTimer_ = nullptr;
    QHash<int, SourceState> sources_;
    QHash<quint64, PendingRequest> pending_;
    QSet<int> activePhaseIds_;
    QList<int> preferredIds_;
    QList<int> fallbackIds_;
    QSet<quint64> usedTimestamps_;
    NtpQueryOptions options_;
    quint64 generation_ = 0;
    quint64 nextRequestId_ = 1;
    int phaseSerial_ = 0;
    int sourcesTried_ = 0;
    int invalidResponses_ = 0;
    int dnsFailures_ = 0;
    int sendFailures_ = 0;
    int socketErrors_ = 0;
    int bindFailures_ = 0;
    int timeouts_ = 0;
    int unavailableFamilies_ = 0;
    int resolvedAddresses_ = 0;
    int usableAddresses_ = 0;
    NtpSocketAvailability socketAvailability_;
    bool phasePreferred_ = false;
    bool active_ = false;
};

NtpClient::NtpClient(QObject *parent)
    : QObject(parent)
    , thread_(new QThread(this))
    , worker_(new Worker)
{
    qRegisterMetaType<NtpQueryOptions>();
    qRegisterMetaType<NtpQueryResult>();
    qRegisterMetaType<NtpFailure>();
    worker_->moveToThread(thread_);
    connect(worker_, &Worker::finished, this, &NtpClient::handleFinished, Qt::QueuedConnection);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_->start();
}

NtpClient::~NtpClient()
{
    if (worker_ && thread_ && thread_->isRunning()) {
        QMetaObject::invokeMethod(worker_, [worker = worker_] { worker->shutdown(); }, Qt::BlockingQueuedConnection);
        thread_->quit();
        thread_->wait();
        worker_ = nullptr;
    }
}

Result<quint64> NtpClient::start(const QList<ServerEndpoint> &sources, const NtpQueryOptions &options)
{
    if (busy_) {
        return Result<quint64>::failure({ErrorCode::Busy, QStringLiteral("NTP query already running")});
    }
    if (sources.isEmpty()) {
        return Result<quint64>::failure({ErrorCode::InvalidConfiguration, QStringLiteral("no NTP sources")});
    }

    ++generation_;
    busy_ = true;
    const quint64 generation = generation_;
    const QList<ServerEndpoint> copiedSources = sources;
    const NtpQueryOptions copiedOptions = options;
    QMetaObject::invokeMethod(worker_,
                              [worker = worker_, generation, copiedSources, copiedOptions] {
                                  worker->beginQuery(generation, copiedSources, copiedOptions);
                              },
                              Qt::QueuedConnection);
    return Result<quint64>::success(generation);
}

void NtpClient::cancel()
{
    if (!busy_) {
        return;
    }
    const quint64 generation = generation_;
    QMetaObject::invokeMethod(worker_, [worker = worker_, generation] {
        worker->cancelQuery(generation);
    }, Qt::QueuedConnection);
}

void NtpClient::handleFinished(const quint64 generation, const NtpQueryResult &result)
{
    if (generation != generation_ || !busy_) {
        return;
    }
    busy_ = false;
    emit finished(generation, result);
}

} // namespace TimeSync

#include "ntp_client.moc"
