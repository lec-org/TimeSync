#include "../src/cli/cli_parser.h"
#include "../src/core/config_repository.h"
#include "../src/core/trusted_clock.h"
#include "../src/network/ntp_protocol.h"
#include "../src/platform/task_scheduler.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

using namespace TimeSync;

class BackendTests final : public QObject {
    Q_OBJECT

private slots:
    void configDefaultsAndCreation();
    void configInvalidFileIsNotOverwritten();
    void ntpCodecAcceptsDeterministicResponse();
    void ntpCodecRejectsInvalidResponse();
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
    QCOMPARE(loaded.value().config.servers.size(), 23);
    QCOMPARE(loaded.value().config.servers.first(), QStringLiteral("ntp1.aliyun.com"));
    QCOMPARE(loaded.value().config.servers.at(6), QStringLiteral("ntp7.aliyun.com"));
    QCOMPARE(loaded.value().config.servers.at(7), QStringLiteral("s1a.time.edu.cn"));
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
