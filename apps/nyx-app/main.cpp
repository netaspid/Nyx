#include <QGuiApplication>
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

  QGuiApplication app(argc, argv);
  nyx::log_init();
  QQuickStyle::setStyle(QStringLiteral("Basic"));
  QGuiApplication::setApplicationName(QStringLiteral("Nyx"));
  QGuiApplication::setApplicationDisplayName(QStringLiteral("Nyx"));
  QGuiApplication::setOrganizationName(QStringLiteral("Nyx"));

  NodeController node;

  QString profilePath;
  QString rendezvous;
  QString nickname;
  for (int i = 1; i + 1 < argc; ++i) {
    const QString arg = QString::fromLocal8Bit(argv[i]);
    if (arg == QLatin1String("--profile")) {
      profilePath = QString::fromLocal8Bit(argv[++i]);
    } else if (arg == QLatin1String("--rendezvous")) {
      rendezvous = QString::fromLocal8Bit(argv[++i]);
    } else if (arg == QLatin1String("--nickname")) {
      nickname = QString::fromLocal8Bit(argv[++i]);
    }
  }

  if (!nickname.isEmpty()) node.setNickname(nickname);
  if (!profilePath.isEmpty()) node.setProfilePath(profilePath);
  if (!rendezvous.isEmpty()) node.setRendezvous(rendezvous);

  QQmlApplicationEngine engine;
  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreationFailed, &app, []() { QCoreApplication::exit(-1); },
      Qt::QueuedConnection);

  engine.rootContext()->setContextProperty(QStringLiteral("node"), &node);
  engine.load(QUrl(QStringLiteral("qrc:/ui/main.qml")));

  if (engine.rootObjects().isEmpty()) {
    std::fprintf(stderr, "nyx-app: не удалось загрузить QML (qrc:/ui/main.qml)\n");
    std::fprintf(stderr, "  см. сообщения Qt выше; частая причина — нет platforms/qwindows.dll рядом с exe\n");
    return 1;
  }

  return app.exec();
}
