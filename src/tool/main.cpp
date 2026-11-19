#include <QApplication>
#include <QQmlApplicationEngine>

#include <QIcon>

#include "qmsstylesbackend.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] {
        QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);

    if (argc > 1) {
        QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, [&]() {
            if (engine.rootObjects().isEmpty()) return;

            if (auto *backendInst = qobject_cast<QmsstylesBackend *>(engine.rootObjects().first()->findChild<QObject*>("backend")))
                backendInst->load(QString(argv[1]));
        });
    }

    engine.loadFromModule("userinterface", "Main");

    app.setWindowIcon(QIcon::fromTheme("application-x-theme"));

    return app.exec();
}
