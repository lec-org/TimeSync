#include "../cli/cli_parser.h"
#include "../cli/cli_runner.h"
#include "app_controller.h"

#include <QTextStream>

#ifdef TIMESYNC_HAS_UI
#    include "../ui/main_window.h"
#    include <QApplication>
#else
#    include <QCoreApplication>
#endif

int main(int argc, char *argv[])
{
#ifdef TIMESYNC_HAS_UI
    QApplication application(argc, argv);
#else
    QCoreApplication application(argc, argv);
#endif

    QStringList arguments = application.arguments();
    if (!arguments.isEmpty()) {
        arguments.removeFirst();
    }

    TimeSync::CliParser parser;
    const TimeSync::Result<TimeSync::ParsedCommand> parsed = parser.parse(arguments);
    QTextStream out(stdout);
    QTextStream err(stderr);
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
