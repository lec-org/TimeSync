#pragma once

#include "ntp_protocol.h"

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QThread>

namespace TimeSync {

enum class NtpFailureKind {
    None = 0,
    InvalidConfiguration,
    NetworkUnavailable,
    SourceTimeout,
    ProtocolRejected,
    NoValidSource,
    Cancelled,
    Busy,
    InternalError,
};

enum class NtpSocketFamily {
    Unavailable = 0,
    IPv4,
    IPv6,
};

struct NtpSocketAvailability {
    bool ipv4 = false;
    bool ipv6 = false;
};

struct NtpFailure {
    NtpFailureKind kind = NtpFailureKind::None;
    int sourcesTried = 0;
    int invalidResponses = 0;
    int dnsFailures = 0;
    int sendFailures = 0;
    int socketErrors = 0;
    int bindFailures = 0;
    int timeouts = 0;
    int unavailableFamilies = 0;
};

[[nodiscard]] QHostAddress normalizedNtpAddress(const QHostAddress &address);
[[nodiscard]] bool ntpAddressesEquivalent(const QHostAddress &left,
                                          const QHostAddress &right);
[[nodiscard]] NtpSocketFamily ntpSocketFamilyForAddress(
    const QHostAddress &address,
    const NtpSocketAvailability &availability) noexcept;
[[nodiscard]] QString ntpFailureKindName(NtpFailureKind kind);
[[nodiscard]] QString ntpFailureDiagnostic(const NtpFailure &failure);

struct NtpQueryResult {
    bool succeeded = false;
    NtpSample sample;
    NtpFailure failure;
};

struct NtpQueryOptions {
    int perSourceTimeoutMs = 1200;
    int globalTimeoutMs = 5000;
    QList<quint8> versions = {NtpCodec::MaximumVersion, NtpCodec::MinimumVersion};
};

class NtpClient final : public QObject {
    Q_OBJECT

public:
    explicit NtpClient(QObject *parent = nullptr);
    ~NtpClient() override;

    [[nodiscard]] Result<quint64> start(const QList<ServerEndpoint> &sources,
                                        const NtpQueryOptions &options = {});
    void cancel();
    [[nodiscard]] bool isBusy() const noexcept { return busy_; }

signals:
    void finished(quint64 generation, const TimeSync::NtpQueryResult &result);

private:
    class Worker;

    void handleFinished(quint64 generation, const TimeSync::NtpQueryResult &result);

    QThread *thread_ = nullptr;
    Worker *worker_ = nullptr;
    quint64 generation_ = 0;
    bool busy_ = false;
};

} // namespace TimeSync

Q_DECLARE_METATYPE(TimeSync::NtpFailure)
Q_DECLARE_METATYPE(TimeSync::NtpQueryResult)
Q_DECLARE_METATYPE(TimeSync::NtpQueryOptions)
