#pragma once

#include <QRegularExpression>
#include <QString>

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
