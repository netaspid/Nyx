#pragma once

#include "nyx/identity.hpp"
#include "nyx/util.hpp"

#include <QRegularExpression>
#include <QString>

#include <algorithm>
#include <vector>

inline QString nyxSafeMediaPathPart(QString value) {
  value = value.trimmed();
  value.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
  value.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
  if (value.isEmpty())
    value = QStringLiteral("Чат");
  return value.left(64);
}

inline QString
nyxMediaRelativeDir(const QString& chatKey, const QString&, const QString& mediaKind) {
  QString stable = chatKey.section(QLatin1Char(':'), 1).toLower();
  if (stable.isEmpty())
    stable = chatKey.toLower();
  stable.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
  if (stable.isEmpty())
    stable = QStringLiteral("local");
  const QString label = chatKey.startsWith(QLatin1String("group:")) ? QStringLiteral("Поле")
                                                                    : QStringLiteral("Личный чат");
  const QString conversation = label + QStringLiteral(" (") + stable.left(8) + QLatin1Char(')');
  const QString leaf = mediaKind == QLatin1String("circle") ? QStringLiteral("Видеокружки")
                                                            : QStringLiteral("Голосовые сообщения");
  return QStringLiteral("Медиа/") + conversation + QLatin1Char('/') + leaf;
}

inline QString nyxNormalizeSessionKey(const QString& key) {
  if (key.startsWith(QLatin1String("group:")) || key.startsWith(QLatin1String("dm:"))) {
    return key.section(QLatin1Char(':'), 0, 0) + QLatin1Char(':') +
           key.section(QLatin1Char(':'), 1).toLower();
  }
  return key;
}

inline bool nyxParseUserIdHex(const QString& hex, nyx::UserId& out) {
  std::vector<uint8_t> bytes;
  if (!nyx::from_hex(hex.toStdString(), bytes) || bytes.size() != out.size())
    return false;
  std::copy(bytes.begin(), bytes.end(), out.begin());
  return true;
}
