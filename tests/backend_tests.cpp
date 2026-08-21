#include "../src/cli/cli_parser.h"
#include "../src/core/config_repository.h"
#include "../src/core/trusted_clock.h"
#include "../src/network/ntp_client.h"
#include "../src/network/ntp_protocol.h"
#include "../src/platform/task_scheduler.h"

#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>
#include <QUdpSocket>

using namespace TimeSync;

class BackendTests final : public QObject {
    Q_OBJECT

private slots:
    void configDefaultsAndCreation();
    void configInvalidFileIsNotOverwritten();
    void ntpCodecAcceptsDeterministicResponse();
    void ntpCodecRejectsInvalidResponse();
    void ntpSocketFamilySelectionIsDeterministic();
    void ntpFailureDiagnosticIsStable();
    void ntpClientReceivesFromLocalIpv4Server();
    void ntpClientTimesOutUsingWorkerTimer();
    void ntpClientReceivesFromLocalIpv6ServerWhenAvailable();
    void trustedClockAdvancesAndBecomesStale();
    void cliRejectsConflictsAndMapsExitCodes();
    void taskActionArgumentsAreAbsoluteAndQuoted();
};

void BackendTests::configDefaultsAndCreation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("config.ini"));

    ConfigRepository repository;
    const Result<ConfigLoadResult> loaded = repository.load(path);
    QVERIFY(loaded);
    QVERIFY(loaded.value().created);
    QCOMPARE(loaded.value().config.servers.size(), 25);
    QCOMPARE(loaded.value().config.servers.first(), QStringLiteral("ntp1.aliyun.com"));
    QCOMPARE(loaded.value().config.servers.at(6), QStringLiteral("ntp7.aliyun.com"));
    QCOMPARE(loaded.value().config.servers.at(7), QStringLiteral("ntp1.nim.ac.cn"));
    QCOMPARE(loaded.value().config.servers.at(8), QStringLiteral("ntp2.nim.ac.cn"));
    QCOMPARE(loaded.value().config.servers.at(9), QStringLiteral("s1a.time.edu.cn"));
    QCOMPARE(loaded.value().config.servers.last(), QStringLiteral("s2m.time.edu.cn"));
    QCOMPARE(loaded.value().config.scheduleIntervalMinutes, 60);
    QCOMPARE(loaded.value().config.language, QStringLiteral("zh-CN"));

    const Result<ConfigLoadResult> reloaded = repository.load(path);
    QVERIFY(reloaded);
    QVERIFY(!reloaded.value().created);
    QCOMPARE(reloaded.value().config.servers, loaded.value().config.servers);
}

void BackendTests::configInvalidFileIsNotOverwritten()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("config.ini"));
    const QByteArray invalid =
        "servers = ntp1.aliyun.com, ntp1.aliyun.com\n"
        "scheduleIntervalMinutes = 60\n"
        "language = zh-CN\n";
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(invalid), invalid.size());
    file.close();

    ConfigRepository repository;
    const Result<ConfigLoadResult> loaded = repository.load(path);
    QVERIFY(!loaded);
    QCOMPARE(loaded.error().code, ErrorCode::InvalidConfiguration);

    const Result<void> saveAttempt = repository.save(path, ConfigRepository::defaultConfig());
    QVERIFY(!saveAttempt);
    QCOMPARE(saveAttempt.error().code, ErrorCode::InvalidConfiguration);

    QFile unchanged(path);
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), invalid);
}

void BackendTests::ntpCodecAcceptsDeterministicResponse()
{
    const NtpTimestamp transmit{3900000000U, 0U};
    const QDateTime baseTime = NtpCodec::toDateTime(transmit);
    QVERIFY(baseTime.isValid());
    const Result<QByteArray> request = NtpCodec::encodeRequest(transmit, 4);
    QVERIFY(request);
    QCOMPARE(request.value().size(), NtpCodec::PacketSize);
    QCOMPARE(static_cast<quint8>(request.value().at(0)), static_cast<quint8>(0x23));

    NtpPacket response;
    response.leapIndicator = 0;
    response.version = 4;
    response.mode = 4;
    response.stratum = 2;
    response.originateTimestamp = transmit;
    response.receiveTimestamp = NtpCodec::fromDateTime(baseTime.addMSecs(150));
    response.transmitTimestamp = NtpCodec::fromDateTime(baseTime.addMSecs(160));
    const Result<QByteArray> encoded = NtpCodec::encodePacket(response);
    QVERIFY(encoded);

    NtpValidationContext context;
    context.expectedOriginate = transmit;
    context.expectedVersion = 4;
    context.expectedAddresses = {QHostAddress::LocalHost};
    context.sourceAddress = QHostAddress::LocalHost;
    context.expectedSourcePort = 123;
    context.sourcePort = 123;
    const Result<NtpPacket> decoded = NtpCodec::decodeAndValidate(encoded.value(), context);
    QVERIFY(decoded);

    const NtpTimestamp localReceive = NtpCodec::fromDateTime(baseTime.addMSecs(210));
    const Result<NtpSample> sample = NtpCodec::calculateSample(
        decoded.value(), transmit, localReceive, QStringLiteral("test"), QHostAddress::LocalHost);
    QVERIFY(sample);
    QVERIFY(sample.value().utcAtReceive.isValid());
    QCOMPARE(sample.value().offsetMilliseconds, qint64(50));
    QCOMPARE(sample.value().utcAtReceive, baseTime.addMSecs(260));
    QCOMPARE(sample.value().source, QStringLiteral("test"));
    QVERIFY(sample.value().roundTripMilliseconds >= 199);
    QVERIFY(sample.value().roundTripMilliseconds <= 201);
}

void BackendTests::ntpCodecRejectsInvalidResponse()
{
    NtpPacket response;
    response.version = 4;
    response.mode = 4;
    response.stratum = 1;
    response.originateTimestamp = {100U, 1U};
    response.receiveTimestamp = {100U, 2U};
    response.transmitTimestamp = {100U, 3U};
    const QByteArray packet = NtpCodec::encodePacket(response).value();

    NtpValidationContext context;
    context.expectedOriginate = {100U, 1U};
    context.expectedVersion = 4;
    context.expectedAddresses = {QHostAddress::LocalHost};
    context.sourceAddress = QHostAddress::LocalHost;
    context.sourcePort = 123;

    QVERIFY(!NtpCodec::decodeAndValidate(packet.left(47), context));

    context.expectedOriginate = {101U, 1U};
    QVERIFY(!NtpCodec::decodeAndValidate(packet, context));
    context.expectedOriginate = {100U, 1U};

    QByteArray leapAlarm = packet;
    leapAlarm[0] = static_cast<char>(leapAlarm.at(0) | 0xC0);
    QVERIFY(!NtpCodec::decodeAndValidate(leapAlarm, context));

    QByteArray wrongMode = packet;
    wrongMode[0] = static_cast<char>((wrongMode.at(0) & 0xF8) | 3);
    QVERIFY(!NtpCodec::decodeAndValidate(wrongMode, context));

    QByteArray kiss = packet;
    kiss[1] = 0;
    QVERIFY(!NtpCodec::decodeAndValidate(kiss, context));

    context.sourceAddress = QHostAddress::LocalHostIPv6;
    QVERIFY(!NtpCodec::decodeAndValidate(packet, context));

    QByteArray wrongVersion = packet;
    wrongVersion[0] = static_cast<char>((wrongVersion.at(0) & 0xC7) | (3 << 3));
    context.sourceAddress = QHostAddress::LocalHost;
    QVERIFY(!NtpCodec::decodeAndValidate(wrongVersion, context));
}

void BackendTests::ntpSocketFamilySelectionIsDeterministic()
{
    const NtpSocketAvailability both{true, true};
    QCOMPARE(ntpSocketFamilyForAddress(QHostAddress(QStringLiteral("192.0.2.10")), both),
             NtpSocketFamily::IPv4);
    QCOMPARE(ntpSocketFamilyForAddress(QHostAddress(QStringLiteral("2001:db8::10")), both),
             NtpSocketFamily::IPv6);

    const QHostAddress mapped(QStringLiteral("::ffff:192.0.2.10"));
    QCOMPARE(ntpSocketFamilyForAddress(mapped, both), NtpSocketFamily::IPv4);
    QVERIFY(ntpAddressesEquivalent(mapped, QHostAddress(QStringLiteral("192.0.2.10"))));
    QCOMPARE(ntpSocketFamilyForAddress(QHostAddress(QStringLiteral("192.0.2.10")), {false, true}),
             NtpSocketFamily::Unavailable);
    QCOMPARE(ntpSocketFamilyForAddress(QHostAddress(QStringLiteral("2001:db8::10")), {true, false}),
             NtpSocketFamily::Unavailable);
}

void BackendTests::ntpFailureDiagnosticIsStable()
{
    const NtpFailure failure{NtpFailureKind::NoValidSource, 3, 1, 2, 4, 5, 1, 3, 2};
    QCOMPARE(ntpFailureDiagnostic(failure),
             QStringLiteral("category=no-valid-source attempts=3 invalid=1 dns=2 send=4 socket=5 bind=1 timeout=3 family=2"));
}

namespace {

ServerEndpoint loopbackEndpoint(const QHostAddress &address, const quint16 port)
{
    ServerEndpoint endpoint;
    endpoint.host = address.toString();
    endpoint.port = port;
    endpoint.literalAddress = true;
    endpoint.original = address.protocol() == QAbstractSocket::IPv6Protocol
        ? QStringLiteral("[%1]:%2").arg(endpoint.host).arg(port)
        : QStringLiteral("%1:%2").arg(endpoint.host).arg(port);
    return endpoint;
}

void installFakeNtpResponder(QUdpSocket *server)
{
    QObject::connect(server, &QUdpSocket::readyRead, server, [server] {
        while (server->hasPendingDatagrams()) {
            QByteArray requestData(static_cast<int>(server->pendingDatagramSize()), Qt::Uninitialized);
            QHostAddress peerAddress;
            quint16 peerPort = 0;
            if (server->readDatagram(requestData.data(), requestData.size(), &peerAddress, &peerPort) < 0) {
                continue;
            }
            const Result<NtpPacket> request = NtpCodec::decodePacket(requestData);
            if (!request) {
                continue;
            }
            NtpPacket response;
            response.leapIndicator = 0;
            response.version = request.value().version;
            response.mode = 4;
            response.stratum = 2;
            response.originateTimestamp = request.value().transmitTimestamp;
            const QDateTime now = QDateTime::currentDateTimeUtc();
            response.receiveTimestamp = NtpCodec::fromDateTime(now);
            response.transmitTimestamp = NtpCodec::fromDateTime(now.addMSecs(1));
            const Result<QByteArray> encoded = NtpCodec::encodePacket(response);
            if (encoded) {
                server->writeDatagram(encoded.value(), peerAddress, peerPort);
            }
        }
    });
}

NtpQueryResult waitForNtpResult(NtpClient *client,
                                const ServerEndpoint &endpoint,
                                const NtpQueryOptions &options,
                                const int maximumWaitMs)
{
    QSignalSpy spy(client, &NtpClient::finished);
    const Result<quint64> started = client->start({endpoint}, options);
    if (!started || !spy.wait(maximumWaitMs) || spy.isEmpty()) {
        return {false, {}, {NtpFailureKind::InternalError}};
    }
    return spy.takeFirst().at(1).value<NtpQueryResult>();
}

} // namespace

void BackendTests::ntpClientReceivesFromLocalIpv4Server()
{
    QUdpSocket server;
    QVERIFY(server.bind(QHostAddress::LocalHost, 0));
    installFakeNtpResponder(&server);

    NtpQueryOptions options;
    options.perSourceTimeoutMs = 300;
    options.globalTimeoutMs = 1000;
    options.versions = {4};
    NtpClient client;
    const NtpQueryResult result = waitForNtpResult(
        &client, loopbackEndpoint(QHostAddress::LocalHost, server.localPort()), options, 2000);
    QVERIFY(result.succeeded);
    QCOMPARE(result.sample.remoteAddress, QHostAddress(QHostAddress::LocalHost).toString());
}

void BackendTests::ntpClientTimesOutUsingWorkerTimer()
{
    QUdpSocket silentServer;
    QVERIFY(silentServer.bind(QHostAddress::LocalHost, 0));

    NtpQueryOptions options;
    options.perSourceTimeoutMs = 150;
    options.globalTimeoutMs = 700;
    options.versions = {4};
    QElapsedTimer elapsed;
    elapsed.start();
    NtpClient client;
    const NtpQueryResult result = waitForNtpResult(
        &client, loopbackEndpoint(QHostAddress::LocalHost, silentServer.localPort()), options, 1600);
    QVERIFY(!result.succeeded);
    QVERIFY(result.failure.kind != NtpFailureKind::InternalError);
    QVERIFY(result.failure.timeouts >= 1);
    QVERIFY(elapsed.elapsed() >= options.perSourceTimeoutMs - 30);
    QVERIFY(elapsed.elapsed() < 1200);
}

void BackendTests::ntpClientReceivesFromLocalIpv6ServerWhenAvailable()
{
    QUdpSocket server;
    if (!server.bind(QHostAddress::LocalHostIPv6, 0)) {
        QSKIP("IPv6 loopback is unavailable in this test environment");
    }
    installFakeNtpResponder(&server);

    NtpQueryOptions options;
    options.perSourceTimeoutMs = 300;
    options.globalTimeoutMs = 1000;
    options.versions = {4};
    NtpClient client;
    const NtpQueryResult result = waitForNtpResult(
        &client, loopbackEndpoint(QHostAddress::LocalHostIPv6, server.localPort()), options, 2000);
    QVERIFY(result.succeeded);
}

void BackendTests::trustedClockAdvancesAndBecomesStale()
{
    TrustedClock clock(20);
    NtpSample sample;
    sample.utcAtReceive = QDateTime::fromSecsSinceEpoch(1700000000, QTimeZone(QTimeZone::UTC));
    sample.source = QStringLiteral("deterministic");
    clock.calibrate(sample);
    QVERIFY(clock.isAvailable());
    QVERIFY(clock.isFresh());
    const QDateTime first = clock.utcNow();
    QTest::qWait(5);
    const QDateTime second = clock.utcNow();
    QVERIFY(second >= first);

    clock.markResume();
    QCOMPARE(clock.state().status, TrustedClockStatus::Stale);
    QVERIFY(clock.utcNow().isValid());
    clock.markRefreshFailed();
    QCOMPARE(clock.state().status, TrustedClockStatus::Error);
    QVERIFY(clock.utcNow() >= second);
}

void BackendTests::cliRejectsConflictsAndMapsExitCodes()
{
    CliParser parser;
    QVERIFY(parser.parse({}).value().mode == CommandMode::Gui);
    const Result<ParsedCommand> dryRun = parser.parse({QStringLiteral("--sync-once"),
                                                        QStringLiteral("--dry-run")});
    QVERIFY(dryRun);
    QCOMPARE(dryRun.value().mode, CommandMode::SyncOnce);
    QVERIFY(dryRun.value().dryRun);

    QVERIFY(!parser.parse({QStringLiteral("--sync-once"), QStringLiteral("--install-task")}));
    QVERIFY(!parser.parse({QStringLiteral("--dry-run")}));
    QVERIFY(!parser.parse({QStringLiteral("--sync-once"), QStringLiteral("--scheduled"),
                           QStringLiteral("--dry-run")}));
    QVERIFY(!parser.parse({QStringLiteral("--sync-once"), QStringLiteral("--scheduled")}));
    QVERIFY(!parser.parse({QStringLiteral("--sync-once"), QStringLiteral("--result"),
                           QStringLiteral("C:/result.json"), QStringLiteral("--nonce"),
                           QStringLiteral("0123456789abcdef0123456789abcdef")}));
    const Result<ParsedCommand> scheduled = parser.parse(
        {QStringLiteral("--sync-once"), QStringLiteral("--scheduled"), QStringLiteral("--config"),
         QStringLiteral("C:/TimeSync/config.ini")});
    QVERIFY(scheduled);
    QVERIFY(scheduled.value().scheduled);
    const Result<ParsedCommand> removeWithConfig = parser.parse(
        {QStringLiteral("--remove-task"), QStringLiteral("--config"), QStringLiteral("portable.ini")});
    QVERIFY(removeWithConfig);
    QCOMPARE(removeWithConfig.value().mode, CommandMode::RemoveTask);
    const Result<ParsedCommand> guiWithConfig = parser.parse(
        {QStringLiteral("--config"), QStringLiteral("portable.ini")});
    QVERIFY(guiWithConfig);
    QCOMPARE(guiWithConfig.value().mode, CommandMode::Gui);
    QVERIFY(!parser.parse({QStringLiteral("--sync-once"), QStringLiteral("--config")}));
    QVERIFY(!parser.parse({QStringLiteral("--sync-once"), QStringLiteral("--config"), QStringLiteral("--dry-run")}));
    QVERIFY(!parser.parse({QStringLiteral("--help"), QStringLiteral("--config"), QStringLiteral("portable.ini")}));
    QVERIFY(!parser.parse({QStringLiteral("--result"), QStringLiteral("C:/result.json")}));

    const Result<ParsedCommand> internal = parser.parse(
        {QStringLiteral("--elevated-sync"), QStringLiteral("--config"), QStringLiteral("C:/config.ini"),
         QStringLiteral("--result"), QStringLiteral("C:/TimeSync-0123456789abcdef0123456789abcdef.result.json"),
         QStringLiteral("--nonce"), QStringLiteral("0123456789abcdef0123456789abcdef")});
    QVERIFY(internal);
    QCOMPARE(internal.value().mode, CommandMode::ElevatedSync);
    QVERIFY(!parser.parse({QStringLiteral("--elevated-sync"), QStringLiteral("--result"),
                           QStringLiteral("C:/result.json"), QStringLiteral("--nonce"),
                           QStringLiteral("0123456789abcdef0123456789abcdef"),
                           QStringLiteral("--config"), QStringLiteral("relative.ini")}));

    QCOMPARE(exitCodeForError(ErrorCode::InvalidConfiguration), ExitCode::ArgumentOrConfiguration);
    QCOMPARE(exitCodeForError(ErrorCode::NoValidSource), ExitCode::RemoteTimeFailure);
    QCOMPARE(exitCodeForError(ErrorCode::PrivilegeMissing), ExitCode::PermissionFailure);
    QCOMPARE(exitCodeForError(ErrorCode::VerificationFailed), ExitCode::VerificationFailure);
}

void BackendTests::taskActionArgumentsAreAbsoluteAndQuoted()
{
    const QString arguments = TaskSchedulerBackend::buildActionArguments(
        QStringLiteral("C:/Time Sync/config.ini"));
    QCOMPARE(arguments,
             QStringLiteral("--sync-once --scheduled --config \"C:/Time Sync/config.ini\""));
    QVERIFY(!arguments.contains(QStringLiteral("schtasks"), Qt::CaseInsensitive));
}

QTEST_MAIN(BackendTests)

#include "backend_tests.moc"
