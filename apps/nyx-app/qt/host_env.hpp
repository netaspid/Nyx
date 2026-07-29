#pragma once

#include <QProcessEnvironment>
#include <QString>

namespace nyx_app {

inline QProcessEnvironment host_process_environment() {
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.remove(QStringLiteral("LD_LIBRARY_PATH"));
  env.remove(QStringLiteral("QT_PLUGIN_PATH"));
  env.remove(QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"));
  env.remove(QStringLiteral("QML2_IMPORT_PATH"));
  env.remove(QStringLiteral("QML_IMPORT_PATH"));
  return env;
}

}
