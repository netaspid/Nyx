#pragma once

#include <QIcon>

class QWindow;

/** Dark/light system title bar (Windows DWM). No-op elsewhere. */
void nyxApplyNativeChromeDark(QWindow* window, bool dark);

/** Applies to all top-level application windows. */
void nyxApplyNativeChromeDarkAll(bool dark);

/** Application icon: .ico from resources, else PNG/SVG/fallback. */
QIcon nyxAppIcon();
