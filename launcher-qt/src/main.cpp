// Uncharted Launcher, Qt window. The look is in qml/ (edit in Qt Creator or Qt Design Studio); the launcher itself is
// uncharted_native.dll's core (launcher/native/core.cpp), which Backend runs in this process.
// --play u1 | u2 | u3 (or 1 / 2 / 3): that game starts at once; the window stays hidden unless the launcher asks
// something, and the program ends with the game.
// --self-check: the window's QML and the launcher core load, the core says it is ready, then the launcher closes as
// it does for the player; exit code 0 (the package checks in CI start it with QT_QPA_PLATFORM=offscreen).
#include <QApplication>
#include <QFont>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStringList>
#include <QTimer>

#include <cstdio>

#include "backend.h"
#include "navigator.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);   // QApplication (not QGuiApplication): the native file dialogs
    app.setApplicationName("Uncharted Launcher");
    app.setOrganizationName("Uncharted Launcher");
    app.setWindowIcon(QIcon(":/qt/qml/Launcher/assets/UCMR.ico"));
#ifndef _WIN32
    QGuiApplication::setDesktopFileName("UCMR_Launcher");   // Wayland: the window takes the .desktop file's icon
#endif
    QFont font("Segoe UI", 10);
    app.setFont(font);
    QQuickStyle::setStyle("Basic");

    QString direct;
    const QStringList args = app.arguments();
    const int at = args.indexOf("--play");
    if (at >= 0 && at + 1 < args.size()) {
        const QString v = args.at(at + 1).toLower();
        direct = v.startsWith('u') ? v : "u" + v;
        if (direct != "u1" && direct != "u2" && direct != "u3")
            direct.clear();
    }
    Backend backend(direct);
    Navigator navigator(&backend);   // controller and keyboard navigation (navigator.cpp)
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("navigator", &navigator);
    QObject::connect(&backend, &Backend::finished, &app, &QCoreApplication::quit);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
                     Qt::QueuedConnection);
    engine.loadFromModule("Launcher", "Main");
    if (!direct.isEmpty() && !engine.rootObjects().isEmpty()) {   // no window unless a question needs one
        QObject *window = engine.rootObjects().first();
        window->setProperty("visible", false);
        QObject::connect(&backend, &Backend::messageRequested, window, [window] { window->setProperty("visible", true); });
    }
    bool checked = false;   // --self-check: the answer was given
    if (args.contains("--self-check")) {
        if (engine.rootObjects().isEmpty())
            return 1;
        const auto check = [&backend, &checked] {
            if (checked)
                return;
            if (!backend.error().isEmpty()) {
                checked = true;
                std::fprintf(stderr, "self-check: %s\n", qPrintable(backend.error()));
                QCoreApplication::exit(1);
            } else if (backend.ready()) {
                checked = true;
                std::printf("self-check: window and launcher core ready\n");
                std::fflush(stdout);
                backend.close();   // the core answers "closed", which ends the program
            }
        };
        QObject::connect(&backend, &Backend::readyChanged, &app, check, Qt::QueuedConnection);
        QTimer::singleShot(0, &app, check);
        QTimer::singleShot(60000, &app, [] {
            std::fprintf(stderr, "self-check: the launcher core did not get ready within 60 s\n");
            QCoreApplication::exit(1);
        });
    }
    return app.exec();
}
