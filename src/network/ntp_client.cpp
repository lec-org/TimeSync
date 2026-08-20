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

class NtpClient::Worker final : public QObject {
    Q_OBJECT

public:
    explicit Worker(QObject *parent = nullptr)
        : QObject(parent)
    {
        socket_ = new QUdpSocket(this);
        connect(socket_, &QUdpSocket::readyRead, this, &Worker::readPendingDatagrams);

        phaseTimer_.setSingleShot(true);
        globalTimer_.setSingleShot(true);
        connect(&phaseTimer_, &QTimer::timeout, this, &Worker::phaseTimedOut);
        connect(&globalTimer_, &QTimer::timeout, this, &Worker::globalTimedOut);
    }

public slots:
    void beginQuery(quint64 generation,
                    const QList<ServerEndpoint> &sources,
                    const NtpQueryOptions &options)
    {
        stopRuntime(false);
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

        active_ = true;
        if (!socket_->bind(QHostAddress::Any, 0, QUdpSocket::ShareAddress)) {
            finishFailure(NtpFailureKind::NetworkUnavailable);
            return;
        }

        invalidResponses_ = 0;
        sourcesTried_ = 0;
        usedTimestamps_.clear();
        globalTimer_.start(options_.globalTimeoutMs);
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
        stopRuntime(true);
        emit finished(generation_, {false, {}, {NtpFailureKind::Cancelled, sourcesTried_, invalidResponses_}});
    }

    void shutdown()
    {
        stopRuntime(false);
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
    };

    void stopRuntime(const bool closeSocket)
    {
        phaseTimer_.stop();
        globalTimer_.stop();
        if (closeSocket) {
            socket_->close();
        }
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
        active_ = false;
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

        const int remainingGlobal = qMax(1, static_cast<int>(globalTimer_.remainingTime()));
        phaseTimer_.start(qMin(remainingGlobal, options_.perSourceTimeoutMs + 100));
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
                    completeSource(id);
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
            if (!address.isNull() && !uniqueAddresses.contains(address)) {
                uniqueAddresses.append(address);
            }
        }
        constexpr int MaximumAddressesPerSource = 8;
        if (uniqueAddresses.size() > MaximumAddressesPerSource) {
            uniqueAddresses.erase(uniqueAddresses.begin() + MaximumAddressesPerSource,
                                  uniqueAddresses.end());
        }

        for (const QHostAddress &address : uniqueAddresses) {
            for (const quint8 version : options_.versions) {
                if (version < NtpCodec::MinimumVersion || version > NtpCodec::MaximumVersion) {
                    continue;
                }
                const NtpTimestamp transmit = nextUniqueTimestamp();
                const Result<QByteArray> request = NtpCodec::encodeRequest(transmit, version);
                if (!request) {
                    continue;
                }
                const qint64 written = socket_->writeDatagram(request.value(), address, state.endpoint.port);
                if (written != request.value().size()) {
                    continue;
                }
                PendingRequest pending;
                pending.id = nextRequestId_++;
                pending.sourceId = sourceId;
                pending.address = address;
                pending.port = state.endpoint.port;
                pending.version = version;
                pending.transmit = transmit;
                pending_.insert(pending.id, pending);
                ++state.pendingRequests;
            }
        }

        if (state.pendingRequests == 0) {
            completeSource(sourceId);
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

    void readPendingDatagrams()
    {
        while (socket_->hasPendingDatagrams() && active_) {
            QByteArray datagram;
            datagram.resize(static_cast<int>(socket_->pendingDatagramSize()));
            QHostAddress address;
            quint16 port = 0;
            if (socket_->readDatagram(datagram.data(), datagram.size(), &address, &port) < 0) {
                continue;
            }

            auto matching = pending_.end();
            for (auto iterator = pending_.begin(); iterator != pending_.end(); ++iterator) {
                if (iterator->address == address && iterator->port == port) {
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

    void completeSource(const int sourceId)
    {
        if (!active_ || !sources_.contains(sourceId)) {
            return;
        }
        SourceState &state = sources_[sourceId];
        if (state.completed) {
            return;
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

        phaseTimer_.stop();
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
            completeSource(id);
        }
    }

    void globalTimedOut()
    {
        finishFailure(invalidResponses_ > 0 ? NtpFailureKind::ProtocolRejected
                                             : NtpFailureKind::SourceTimeout);
    }

    void finishSuccess(const NtpSample &sample)
    {
        const quint64 generation = generation_;
        stopRuntime(true);
        NtpQueryResult result;
        result.succeeded = true;
        result.sample = sample;
        emit finished(generation, result);
    }

    void finishFailure(const NtpFailureKind kind)
    {
        if (!active_) {
            return;
        }
        const quint64 generation = generation_;
        const NtpFailure failure{kind, sourcesTried_, invalidResponses_};
        stopRuntime(true);
        emit finished(generation, {false, {}, failure});
    }

    QUdpSocket *socket_ = nullptr;
    QTimer phaseTimer_;
    QTimer globalTimer_;
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
    thread_->start();
}

NtpClient::~NtpClient()
{
    if (worker_ && thread_ && thread_->isRunning()) {
        QMetaObject::invokeMethod(worker_, [worker = worker_] { worker->shutdown(); }, Qt::BlockingQueuedConnection);
        thread_->quit();
        thread_->wait();
        delete worker_;
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
