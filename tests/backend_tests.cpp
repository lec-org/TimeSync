#include "../src/cli/cli_parser.h"
#include "../src/cli/cli_runner.h"
#include "../src/core/config_repository.h"
#include "../src/core/operation_types.h"
#include "../src/core/sync_coordinator.h"
#include "../src/core/trusted_clock.h"
#include "../src/network/ntp_client.h"
#include "../src/network/ntp_protocol.h"
#include "../src/platform/elevation_broker.h"
#include "../src/platform/task_scheduler.h"
#include "../src/platform/windows_system_clock.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
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
    void syncCoordinatorRejectsConcurrentRequests();
    void syncCoordinatorUpgradesRefreshToManualSync();
    void syncCoordinatorUsesFreshClockWithoutNtp();
    void trustedClockAdvancesAndBecomesStale();
    void systemTimeFailureMappingsStayDistinct();
    void cliRejectsConflictsAndMapsExitCodes();
    void taskActionArgumentsAreAbsoluteAndQuoted();
    void taskRepetitionIntervalParsesNormalizedIso8601();
    void startupCliIsIndependentAndStrict();
    void startupTaskNameAndArgumentsAreIndependent();
    void taskActionIdentitySurvivesPortableMigration();
    void cliElevationWaitPolicyPreservesScheduledIsolation();
    void startupAcquisitionRetriesAreBounded();
    void scheduledRunnerRejectsMissingElevationBeforeConfiguration();
    void startupTaskBackendReportsUnsupportedPlatform();
    void elevationBrokerQuotesArgumentsAndReportsPlatformSupport();
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
    QVERIFY2(result.succeeded, qPrintable(ntpFailureDiagnostic(result.failure)));
}

void BackendTests::syncCoordinatorRejectsConcurrentRequests()
{
    QUdpSocket silentServer;
    QVERIFY(silentServer.bind(QHostAddress::LocalHost, 0));

    AppConfig config = ConfigRepository::defaultConfig();
    config.servers = {QStringLiteral("127.0.0.1:%1").arg(silentServer.localPort())};

    SyncCoordinator coordinator;
    const Result<quint64> first = coordinator.startSync(config, SyncMode::AuthorizedDirect);
    QVERIFY(first);
    const Result<quint64> repeated = coordinator.startSync(config, SyncMode::AuthorizedDirect);
    QVERIFY(!repeated);
    QCOMPARE(repeated.error().code, ErrorCode::Busy);
    coordinator.cancel();
}

void BackendTests::syncCoordinatorUpgradesRefreshToManualSync()
{
    QUdpSocket silentServer;
    QVERIFY(silentServer.bind(QHostAddress::LocalHost, 0));

    AppConfig config = ConfigRepository::defaultConfig();
    config.servers = {QStringLiteral("127.0.0.1:%1").arg(silentServer.localPort())};

    SyncCoordinator coordinator;
    const Result<quint64> refresh = coordinator.startRefresh(config);
    QVERIFY(refresh);
    const Result<quint64> sync = coordinator.startSync(config, SyncMode::AuthorizedDirect);
    QVERIFY(sync);
    QCOMPARE(sync.value(), refresh.value());
    coordinator.cancel();
}

void BackendTests::syncCoordinatorUsesFreshClockWithoutNtp()
{
    SyncCoordinator coordinator;
    QVERIFY(!coordinator.canApplyFreshClockForManualSync());

    AppConfig empty;
    empty.servers.clear();
    const Result<quint64> withoutSample = coordinator.startSync(empty, SyncMode::AuthorizedDirect);
    QVERIFY(!withoutSample);
    QCOMPARE(withoutSample.error().code, ErrorCode::InvalidConfiguration);

    NtpSample sample;
    sample.utcAtReceive = QDateTime::currentDateTimeUtc();
    sample.source = QStringLiteral("cached-source");
    coordinator.trustedClock()->calibrate(sample);
    QVERIFY(coordinator.canApplyFreshClockForManualSync());

    coordinator.trustedClock()->markResume();
    QVERIFY(!coordinator.canApplyFreshClockForManualSync());
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

void BackendTests::systemTimeFailureMappingsStayDistinct()
{
    QCOMPARE(operationFailureForSystemTimeFailure(static_cast<int>(SystemTimeFailure::AccessDenied)),
             OperationFailure::SystemTimeRejected);
    QCOMPARE(operationFailureForSystemTimeFailure(static_cast<int>(SystemTimeFailure::PrivilegeMissing)),
             OperationFailure::PrivilegeMissing);
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

void BackendTests::taskRepetitionIntervalParsesNormalizedIso8601()
{
    QCOMPARE(TaskSchedulerBackend::parseRepetitionIntervalMinutes(QStringLiteral("PT60M")), 60);
    QCOMPARE(TaskSchedulerBackend::parseRepetitionIntervalMinutes(QStringLiteral("PT1H")), 60);
    QCOMPARE(TaskSchedulerBackend::parseRepetitionIntervalMinutes(QStringLiteral("PT2H")), 120);
    QCOMPARE(TaskSchedulerBackend::parseRepetitionIntervalMinutes(QStringLiteral("PT1H30M")), 90);
    QCOMPARE(TaskSchedulerBackend::parseRepetitionIntervalMinutes(QStringLiteral("P1DT1H")), 1500);
    QCOMPARE(TaskSchedulerBackend::parseRepetitionIntervalMinutes(QStringLiteral("pt1h")), 60);
    QCOMPARE(TaskSchedulerBackend::parseRepetitionIntervalMinutes(QString()), 0);
    QCOMPARE(TaskSchedulerBackend::parseRepetitionIntervalMinutes(QStringLiteral("PT")), 0);
}

void BackendTests::startupCliIsIndependentAndStrict()
{
    CliParser parser;
    QVERIFY(!parser.parse({}).value().startup);
    const auto install = parser.parse({QStringLiteral("--install-startup-task"),
                                      QStringLiteral("--config"), QStringLiteral("portable.ini")});
    QVERIFY(install);
    QCOMPARE(install.value().mode, CommandMode::InstallStartupTask);
    QVERIFY(!install.value().scheduled);
    const auto remove = parser.parse({QStringLiteral("--remove-startup-task")});
    QVERIFY(remove);
    QCOMPARE(remove.value().mode, CommandMode::RemoveStartupTask);

    const QString absoluteConfig = QDir::current().absoluteFilePath(QStringLiteral("startup config.ini"));
    const QStringList valid{QStringLiteral("--sync-once"), QStringLiteral("--scheduled"),
                            QStringLiteral("--startup"), QStringLiteral("--config"), absoluteConfig};
    const auto startup = parser.parse(valid);
    QVERIFY(startup);
    QCOMPARE(startup.value().mode, CommandMode::SyncOnce);
    QVERIFY(startup.value().scheduled);
    QVERIFY(startup.value().startup);
    QVERIFY(!startup.value().dryRun);
    QVERIFY(!parser.parse(valid + QStringList{QStringLiteral("--startup")}));
    QVERIFY(!parser.parse(valid + QStringList{QStringLiteral("--dry-run")}));
    QVERIFY(!parser.parse(valid + QStringList{QStringLiteral("--install-startup-task")}));

    const QList<QStringList> invalid{
        {QStringLiteral("--startup")},
        {QStringLiteral("--sync-once"), QStringLiteral("--startup")},
        {QStringLiteral("--help"), QStringLiteral("--startup")},
        {QStringLiteral("--sync-once"), QStringLiteral("--scheduled"), QStringLiteral("--startup"),
         QStringLiteral("--config"), QStringLiteral("relative.ini")},
        {QStringLiteral("--install-startup-task"), QStringLiteral("--install-task")},
        {QStringLiteral("--install-startup-task"), QStringLiteral("--remove-startup-task")},
        {QStringLiteral("--remove-startup-task"), QStringLiteral("--remove-task")},
        {QStringLiteral("--install-startup-task"), QStringLiteral("--install-startup-task")},
        {QStringLiteral("--remove-startup-task"), QStringLiteral("--remove-startup-task")},
        {QStringLiteral("--install-startup-task"), QStringLiteral("--scheduled")},
        {QStringLiteral("--remove-startup-task"), QStringLiteral("--dry-run")},
        {QStringLiteral("--elevated-sync"), QStringLiteral("--startup")},
        {QStringLiteral("--install-startup-task"), QStringLiteral("--enabled"), QStringLiteral("true")},
    };
    for (const QStringList &arguments : invalid) {
        QVERIFY2(!parser.parse(arguments), qPrintable(arguments.join(QLatin1Char(' '))));
    }
    QVERIFY(CliParser::helpText().contains(QStringLiteral("--install-startup-task")));
    QVERIFY(CliParser::helpText().contains(QStringLiteral("--remove-startup-task")));
}

void BackendTests::startupTaskNameAndArgumentsAreIndependent()
{
    QCOMPARE(TaskSchedulerBackend::taskName(), TaskSchedulerBackend::taskName(ScheduledTaskKind::Periodic));
    QCOMPARE(TaskSchedulerBackend::taskName(ScheduledTaskKind::Startup),
             QStringLiteral("TimeSync.StandardClock.StartupSync.v1"));
    QVERIFY(TaskSchedulerBackend::taskName(ScheduledTaskKind::Startup) != TaskSchedulerBackend::taskName());
    const QString config = QDir::current().absoluteFilePath(QStringLiteral("Time Sync/中文配置.ini"));
    QCOMPARE(TaskSchedulerBackend::buildActionArguments(config, ScheduledTaskKind::Startup),
             QStringLiteral("--sync-once --scheduled --startup --config %1")
                 .arg(ElevationBroker::quoteWindowsArgument(config)));
    QCOMPARE(TaskSchedulerBackend::buildActionArguments(config, ScheduledTaskKind::Periodic),
             TaskSchedulerBackend::buildActionArguments(config));
    QVERIFY(!TaskSchedulerBackend::buildActionArguments(config).contains(QStringLiteral("--startup")));
}

void BackendTests::taskActionIdentitySurvivesPortableMigration()
{
    // Old paths need not exist or equal the currently running EXE. Registry author/current
    // user checks are separate; these assertions cover the semantic action identity layer.
    const QString oldExecutable = QDir::current().absoluteFilePath(QStringLiteral("old portable/TimeSync.exe"));
    const QString movedExecutable = QDir::current().absoluteFilePath(QStringLiteral("new portable/TimeSync.exe"));
    const QString config = QDir::current().absoluteFilePath(QStringLiteral("old portable/config.ini"));
    const QStringList periodic{QStringLiteral("--sync-once"), QStringLiteral("--scheduled"),
                               QStringLiteral("--config"), config};
    const QStringList startup{QStringLiteral("--sync-once"), QStringLiteral("--scheduled"),
                              QStringLiteral("--startup"), QStringLiteral("--config"), config};
    for (const QString &executable : {oldExecutable, movedExecutable}) {
        QVERIFY(TaskSchedulerBackend::isOwnedTaskAction(executable, periodic));
        QVERIFY(TaskSchedulerBackend::isOwnedTaskAction(executable, startup, ScheduledTaskKind::Startup));
        QVERIFY(!TaskSchedulerBackend::isOwnedTaskAction(executable, periodic, ScheduledTaskKind::Startup));
        QVERIFY(!TaskSchedulerBackend::isOwnedTaskAction(executable, startup, ScheduledTaskKind::Periodic));
    }
    // Noncanonical ordering remains recognizable for repair/removal, rather than being
    // confused with an unrelated task. Strict Startup query validates the full template separately.
    QVERIFY(TaskSchedulerBackend::isOwnedTaskAction(oldExecutable,
        {QStringLiteral("--config"), config, QStringLiteral("--scheduled"), QStringLiteral("--sync-once")}));
    QVERIFY(TaskSchedulerBackend::isOwnedTaskAction(oldExecutable,
        {QStringLiteral("--config"), config, QStringLiteral("--startup"),
         QStringLiteral("--scheduled"), QStringLiteral("--sync-once")}, ScheduledTaskKind::Startup));
    QVERIFY(!TaskSchedulerBackend::isOwnedTaskAction(QStringLiteral("relative.exe"), periodic));
    QVERIFY(!TaskSchedulerBackend::isOwnedTaskAction(oldExecutable, {}));
    QVERIFY(!TaskSchedulerBackend::isOwnedTaskAction(oldExecutable,
        {QStringLiteral("--sync-once"), QStringLiteral("--scheduled"), QStringLiteral("--config"),
         QStringLiteral("relative.ini")}));
    QVERIFY(!TaskSchedulerBackend::isOwnedTaskAction(oldExecutable,
        periodic + QStringList{QStringLiteral("--dry-run")}));
    QVERIFY(!TaskSchedulerBackend::isOwnedTaskAction(oldExecutable,
        periodic + QStringList{QStringLiteral("--scheduled")}));
    QVERIFY(!TaskSchedulerBackend::isOwnedTaskAction(oldExecutable,
        startup + QStringList{QStringLiteral("--startup")}, ScheduledTaskKind::Startup));
    QVERIFY(!TaskSchedulerBackend::isOwnedTaskAction(oldExecutable,
        {QStringLiteral("--install-task"), QStringLiteral("--config"), config}));
}

void BackendTests::cliElevationWaitPolicyPreservesScheduledIsolation()
{
    CliParser parser;
    QVERIFY(!parser.parse({}).value().requiresSynchronousElevation());
    QVERIFY(!parser.parse({QStringLiteral("--help")}).value().requiresSynchronousElevation());
    for (const QString &operation : {QStringLiteral("--sync-once"), QStringLiteral("--install-task"),
                                    QStringLiteral("--remove-task"), QStringLiteral("--install-startup-task"),
                                    QStringLiteral("--remove-startup-task")}) {
        const auto parsed = parser.parse({operation});
        QVERIFY(parsed);
        QVERIFY(parsed.value().requiresSynchronousElevation());
    }
    const auto dryRun = parser.parse({QStringLiteral("--sync-once"), QStringLiteral("--dry-run")});
    QVERIFY(dryRun);
    QVERIFY(dryRun.value().requiresSynchronousElevation());
    const QString config = QDir::current().absoluteFilePath(QStringLiteral("config.ini"));
    const QStringList scheduled{QStringLiteral("--sync-once"), QStringLiteral("--scheduled"),
                                QStringLiteral("--config"), config};
    const auto periodic = parser.parse(scheduled);
    QVERIFY(periodic);
    QVERIFY(!periodic.value().requiresSynchronousElevation());
    const auto startup = parser.parse(scheduled + QStringList{QStringLiteral("--startup")});
    QVERIFY(startup);
    QVERIFY(!startup.value().requiresSynchronousElevation());
    const auto internal = parser.parse({QStringLiteral("--elevated-sync"), QStringLiteral("--result"),
        QDir::current().absoluteFilePath(QStringLiteral("result.json")), QStringLiteral("--nonce"),
        QStringLiteral("0123456789abcdef0123456789abcdef")});
    QVERIFY(internal);
    QVERIFY(internal.value().requiresSynchronousElevation());
}

void BackendTests::startupAcquisitionRetriesAreBounded()
{
    for (const ErrorCode error : {ErrorCode::NetworkUnavailable, ErrorCode::SourceTimeout, ErrorCode::NoValidSource}) {
        QVERIFY(CliRunner::shouldRetryStartupAcquisition(error, 0, 1));
        QVERIFY(CliRunner::shouldRetryStartupAcquisition(error, 50000, 5));
        QVERIFY(!CliRunner::shouldRetryStartupAcquisition(error, CliRunner::StartupAcquisitionBudgetMs, 1));
        QVERIFY(!CliRunner::shouldRetryStartupAcquisition(error, 113000, 1));
        QVERIFY(!CliRunner::shouldRetryStartupAcquisition(error, 0, CliRunner::StartupMaximumAttempts));
        QVERIFY(!CliRunner::shouldRetryStartupAcquisition(error, -1, 1));
        QVERIFY(!CliRunner::shouldRetryStartupAcquisition(error, 0, 0));
    }
    // In particular, success and any system-write/verification failure cannot initiate another acquisition/write.
    for (const ErrorCode error : {ErrorCode::None, ErrorCode::InvalidConfiguration, ErrorCode::InvalidArgument,
                                 ErrorCode::ProtocolRejected, ErrorCode::Cancelled, ErrorCode::Busy,
                                 ErrorCode::InternalError, ErrorCode::PrivilegeMissing,
                                 ErrorCode::SystemTimeRejected, ErrorCode::VerificationFailed,
                                 ErrorCode::UncertainState}) {
        QVERIFY(!CliRunner::shouldRetryStartupAcquisition(error, 0, 1));
    }
}

void BackendTests::scheduledRunnerRejectsMissingElevationBeforeConfiguration()
{
    const auto elevated = ElevationBroker::isProcessElevated();
#ifdef Q_OS_WIN
    if (!elevated || elevated.value()) {
        QSKIP("Requires an unelevated Windows token; this test never mutates time or tasks");
    }
#endif
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ParsedCommand command;
    command.mode = CommandMode::SyncOnce;
    command.scheduled = true;
    command.configPath = temporary.filePath(QStringLiteral("must-not-be-created.ini"));
    QString output;
    QString errors;
    QTextStream out(&output);
    QTextStream err(&errors);
    CliRunner runner;
    for (const bool startup : {false, true}) {
        command.startup = startup;
        const ExitCode result = runner.run(command, QCoreApplication::applicationFilePath(), out, err);
        QCOMPARE(result, elevated ? ExitCode::PermissionFailure : exitCodeForError(elevated.error().code));
        QVERIFY(!QFileInfo::exists(command.configPath));
    }
    QVERIFY(output.isEmpty());
    QVERIFY(errors.contains(QStringLiteral("elevated task token")));
}

void BackendTests::startupTaskBackendReportsUnsupportedPlatform()
{
#ifndef Q_OS_WIN
    for (const ScheduledTaskKind kind : {ScheduledTaskKind::Periodic, ScheduledTaskKind::Startup}) {
        TaskSchedulerBackend backend(kind);
        const auto query = backend.query();
        QVERIFY(!query);
        QCOMPARE(query.error().code, ErrorCode::UnsupportedPlatform);
        const auto install = backend.install(QStringLiteral("TimeSync"), QStringLiteral("config.ini"), 0);
        QVERIFY(!install);
        QCOMPARE(install.error().code, ErrorCode::UnsupportedPlatform);
        const auto disable = backend.setEnabled(false);
        QVERIFY(!disable);
        QCOMPARE(disable.error().code, ErrorCode::UnsupportedPlatform);
        const auto remove = backend.remove();
        QVERIFY(!remove);
        QCOMPARE(remove.error().code, ErrorCode::UnsupportedPlatform);
    }
#else
    QSKIP("Real Task Scheduler mutation/ownership tests belong to the Windows validation lane");
#endif
}

void BackendTests::elevationBrokerQuotesArgumentsAndReportsPlatformSupport()
{
    QCOMPARE(ElevationBroker::quoteWindowsArgument(QString()), QStringLiteral("\"\""));
    QCOMPARE(ElevationBroker::quoteWindowsArgument(QStringLiteral("C:/Time Sync/TimeSync.exe")),
             QStringLiteral("\"C:/Time Sync/TimeSync.exe\""));
    QCOMPARE(ElevationBroker::quoteWindowsArgument(QStringLiteral("a\"b")), QStringLiteral("\"a\\\"b\""));
    QCOMPARE(ElevationBroker::quoteWindowsArgument(QStringLiteral("C:/path\\")),
             QStringLiteral("\"C:/path\\\\\""));

#ifndef Q_OS_WIN
    const Result<bool> elevated = ElevationBroker::isProcessElevated();
    QVERIFY(!elevated);
    QCOMPARE(elevated.error().code, ErrorCode::UnsupportedPlatform);

    const Result<void> relaunched = ElevationBroker::relaunchAsAdministrator(QStringLiteral("TimeSync"), {});
    QVERIFY(!relaunched);
    QCOMPARE(relaunched.error().code, ErrorCode::UnsupportedPlatform);
#endif
}

QTEST_MAIN(BackendTests)

#include "backend_tests.moc"
