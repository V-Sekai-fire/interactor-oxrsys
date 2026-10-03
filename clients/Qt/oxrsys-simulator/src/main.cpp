// SPDX-License-Identifier: MPL-2.0

#include "SimulatorWidget.h"

#include <QApplication>
#include <QCommandLineParser>

int main(int argc, char** argv)
{
    // As a GUI app Qt would log to the debugger; redirected stderr is where scripts read these lines.
    qputenv("QT_FORCE_STDERR_LOGGING", "1");
    QApplication app(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription("OXRSys desktop simulator client");
    parser.addHelpOption();
    const QCommandLineOption noAutoConnect("no-autoconnect", "Stay disconnected until Search is clicked.");
    const QCommandLineOption snapshot("snapshot", "Save the 90th decoded frame to <file>.", "file");
    parser.addOption(noAutoConnect);
    parser.addOption(snapshot);
    parser.process(app);

    SimulatorWidget widget;
    widget.setAutoConnect(!parser.isSet(noAutoConnect));
    widget.setSnapshotPath(parser.value(snapshot));
    widget.setWindowTitle("OXRSys Simulator");
    widget.resize(1280, 720);
    widget.show();

    return app.exec();
}
