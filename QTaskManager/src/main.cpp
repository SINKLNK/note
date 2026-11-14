#include "MainWindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("QtProcessTemplate"));
    // Placeholder: this is what QSettings keys off, so it decides where saved
    // column widths and window state will land under HKCU\Software.
    app.setOrganizationName(QStringLiteral("QtProcessTemplate"));

    MainWindow window;
    window.show();
    return app.exec();
}
