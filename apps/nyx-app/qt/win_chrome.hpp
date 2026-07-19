#pragma once

#include <QIcon>

class QWindow;

/** Тёмный/светлый системный title bar (Windows DWM). На других ОС — no-op. */
void nyxApplyNativeChromeDark(QWindow* window, bool dark);

/** Применить ко всем top-level окнам приложения. */
void nyxApplyNativeChromeDarkAll(bool dark);

/** Иконка приложения: .ico из ресурсов, иначе PNG/SVG/fallback. */
QIcon nyxAppIcon();
