#include "win_chrome.hpp"

#include <QGuiApplication>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QWindow>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <dwmapi.h>
#endif

void nyxApplyNativeChromeDark(QWindow* window, bool dark) {
#ifdef Q_OS_WIN
  if (!window) return;
  const HWND hwnd = reinterpret_cast<HWND>(window->winId());
  if (!hwnd) return;
  BOOL use_dark = dark ? TRUE : FALSE;
  // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (Win10 20H1+), 19 = pre-20H1
  ::DwmSetWindowAttribute(hwnd, 20, &use_dark, sizeof(use_dark));
  ::DwmSetWindowAttribute(hwnd, 19, &use_dark, sizeof(use_dark));
#else
  Q_UNUSED(window);
  Q_UNUSED(dark);
#endif
}

void nyxApplyNativeChromeDarkAll(bool dark) {
#ifdef Q_OS_WIN
  const auto windows = QGuiApplication::topLevelWindows();
  for (QWindow* w : windows)
    nyxApplyNativeChromeDark(w, dark);
#else
  Q_UNUSED(dark);
#endif
}

QIcon nyxAppIcon() {
  QIcon ico(QStringLiteral(":/icons/nyx.ico"));
  if (!ico.isNull() && !ico.availableSizes().isEmpty())
    return ico;

  QIcon png(QStringLiteral(":/icons/nyx-256.png"));
  if (!png.isNull() && !png.availableSizes().isEmpty())
    return png;

  QIcon svg(QStringLiteral(":/icons/nyx-mark.svg"));
  if (!svg.isNull() && !svg.availableSizes().isEmpty())
    return svg;

  QPixmap pm(64, 64);
  pm.fill(Qt::transparent);
  QPainter painter(&pm);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setBrush(QColor(QStringLiteral("#17212b")));
  painter.setPen(Qt::NoPen);
  painter.drawEllipse(2, 2, 60, 60);
  painter.setBrush(QColor(QStringLiteral("#5288c1")));
  painter.drawEllipse(22, 22, 20, 20);
  return QIcon(pm);
}
