#include "../cli/cli_parser.h"
#include "../cli/cli_runner.h"
#include "../platform/elevation_broker.h"
#include "../platform/windows_system_clock.h"
#include "app_controller.h"

#include <QByteArray>
#include <QTextStream>

#include <cstring>

#ifdef TIMESYNC_HAS_UI
#    include "../ui/main_window.h"
#    include <QApplication>
#else
#    include <QCoreApplication>
#endif

int main(int argc, char *argv[])
{
    if (argc >= 3 && std::strcmp(argv[1], "--set-system-time") == 0) {
        bool ok = false;
        const qint64 utcMs = QByteArray(argv[2]).toLongLong(&ok);
        if (!ok) {
            return static_cast<int>(TimeSync::SystemTimeFailure::InvalidInput) + 1;
        }
        return TimeSync::WindowsSystemClock::runSetSystemTimeCommand(utcMs);
    }

#ifdef TIMESYNC_HAS_UI
    QApplication application(argc, argv);
#else
    QCoreApplication application(argc, argv);
#endif

    QTextStream out(stdout);
    QTextStream err(stderr);

    const TimeSync::Result<bool> elevated = TimeSync::ElevationBroker::isProcessElevated();
    if (!elevated && elevated.error().code != TimeSync::ErrorCode::UnsupportedPlatform) {
        err << "Unable to determine administrator privileges: " << elevated.error().detail << '\n';
        return static_cast<int>(TimeSync::exitCodeForError(elevated.error().code));
    }
    if (elevated && !elevated.value()) {
        QStringList relaunchArguments = application.arguments();
        if (!relaunchArguments.isEmpty()) {
            relaunchArguments.removeFirst();
        }

        const TimeSync::Result<void> relaunched = TimeSync::ElevationBroker::relaunchAsAdministrator(
            QCoreApplication::applicationFilePath(), relaunchArguments);
        if (relaunched) {
            return static_cast<int>(TimeSync::ExitCode::Success);
        }

        if (relaunched.error().code == TimeSync::ErrorCode::ElevationCancelled) {
            err << "Administrator authorization was cancelled: " << relaunched.error().detail << '\n';
        } else {
            err << "Unable to relaunch as administrator: " << relaunched.error().detail << '\n';
        }
        return static_cast<int>(TimeSync::exitCodeForError(relaunched.error().code));
    }

    QStringList arguments = application.arguments();
    if (!arguments.isEmpty()) {
        arguments.removeFirst();
    }

    TimeSync::CliParser parser;
    const TimeSync::Result<TimeSync::ParsedCommand> parsed = parser.parse(arguments);
    if (!parsed) {
        err << "Invalid command-line arguments.\n";
        return static_cast<int>(TimeSync::ExitCode::ArgumentOrConfiguration);
    }

    const TimeSync::ParsedCommand command = parsed.value();
    if (command.mode != TimeSync::CommandMode::Gui) {
        TimeSync::CliRunner runner;
        return static_cast<int>(runner.run(command,
                                           QCoreApplication::applicationFilePath(),
                                           out,
                                           err));
    }

#ifdef TIMESYNC_HAS_UI
    TimeSync::Ui::MainWindow window;
    TimeSync::AppController controller(&window, command);
    window.show();
    controller.start(&application);
    const int exitCode = application.exec();
    controller.shutdown();
    return exitCode;
#else
    err << "GUI sources are not present in this checkout.\n";
    return static_cast<int>(TimeSync::ExitCode::OtherFailure);
#endif
}
