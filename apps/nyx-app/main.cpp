#include <QApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTimer>

#include "android_platform.hpp"
#include "nyx/log.hpp"
#include "nyx/nat.hpp"
#include "nyx/paths.hpp"
#include "call_frame_provider.hpp"
#include "node_controller.hpp"
#include "win_chrome.hpp"

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

#if defined(_WIN32)
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
    qputenv("QT_QPA_PLATFORM", "windows:darkmode=1");
#endif

  QApplication app(argc, argv);

#if defined(Q_OS_ANDROID)
  // Writable app storage before any nyx:: paths / logging (no reliable HOME on Android).
  {
    const QString root =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/nyx");
    nyx::set_base_data_root(root.toStdString());
    nyx::ensure_data_dir();
  }
#endif

  nyx::log_init();

#if defined(Q_OS_ANDROID)
  // After QApplication + log_init — JNI context is ready.
  nyx_android::acquire_multicast_lock();
  {
    const std::string wifi = nyx_android::wifi_ipv4();
    if (!wifi.empty()) {
      nyx::set_lan_ipv4_override(wifi);
      nyx::log_write(nyx::LogLevel::Info, std::string("Android Wi‑Fi IPv4: ") + wifi);
    }
  }
#endif

  QQuickStyle::setStyle(QStringLiteral("Basic"));
  QApplication::setApplicationName(QStringLiteral("Nyx"));
  QApplication::setApplicationDisplayName(QStringLiteral("Nyx"));
  QApplication::setOrganizationName(QStringLiteral("Nyx"));
  const QIcon app_icon = nyxAppIcon();
  QApplication::setWindowIcon(app_icon);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
  if (auto* hints = QGuiApplication::styleHints())
    hints->setColorScheme(Qt::ColorScheme::Dark);
#endif

  NodeController node;

  for (int i = 1; i + 1 < argc; ++i) {
    const QString arg = QString::fromLocal8Bit(argv[i]);
    if (arg == QLatin1String("--rendezvous")) {
      node.setRendezvous(QString::fromLocal8Bit(argv[++i]));
    }
  }

  QQmlApplicationEngine engine;
  auto* call_frames = new CallFrameProvider();
  node.setCallFrameProvider(call_frames);
  engine.addImageProvider(QStringLiteral("nyxcall"), call_frames);
  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreationFailed, &app, []() { QCoreApplication::exit(-1); },
      Qt::QueuedConnection);
  QObject::connect(&engine, &QQmlEngine::warnings, [](const QList<QQmlError>& warnings) {
    for (const QQmlError& w : warnings) {
      std::fprintf(stderr, "QML: %s\n", w.toString().toLocal8Bit().constData());
    }
  });

  engine.rootContext()->setContextProperty(QStringLiteral("app"), &node);
  engine.load(QUrl(QStringLiteral("qrc:/ui/main.qml")));

  if (engine.rootObjects().isEmpty()) {
    std::fprintf(stderr, "nyx-app: не удалось загрузить QML (qrc:/ui/main.qml)\n");
    return 1;
  }

  QTimer::singleShot(0, &app, []() {
    nyxApplyNativeChromeDarkAll(true);
  });

  const int rc = app.exec();
#if defined(Q_OS_ANDROID)
  nyx_android::release_multicast_lock();
#endif
  return rc;
}
