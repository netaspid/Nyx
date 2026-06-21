#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include "nyx/log.hpp"
#include "node_controller.hpp"

#include <QString>
#include <cstdlib>
#include <cstdio>

static void stderrQtHandler(QtMsgType, const QMessageLogContext&, const QString& msg) {
  std::fprintf(stderr, "%s\n", msg.toLocal8Bit().constData());
}

int main(int argc, char* argv[]) {
  if (std::getenv("NYX_QT_LOG")) {
    qInstallMessageHandler(stderrQtHandler);
  }

  QApplication app(argc, argv);
  nyx::log_init();
  QQuickStyle::setStyle(QStringLiteral("Basic"));
  QApplication::setApplicationName(QStringLiteral("Nyx"));
  QApplication::setApplicationDisplayName(QStringLiteral("Nyx"));
  QApplication::setOrganizationName(QStringLiteral("Nyx"));

  NodeController node;

  for (int i = 1; i + 1 < argc; ++i) {
    const QString arg = QString::fromLocal8Bit(argv[i]);
    if (arg == QLatin1String("--rendezvous")) {
      node.setRendezvous(QString::fromLocal8Bit(argv[++i]));
    }
  }

  QQmlApplicationEngine engine;
  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreationFailed, &app, []() { QCoreApplication::exit(-1); },
      Qt::QueuedConnection);

  engine.rootContext()->setContextProperty(QStringLiteral("app"), &node);
  engine.load(QUrl(QStringLiteral("qrc:/ui/main.qml")));

  if (engine.rootObjects().isEmpty()) {
    std::fprintf(stderr, "nyx-app: не удалось загрузить QML (qrc:/ui/main.qml)\n");
    return 1;
  }

  return app.exec();
}
