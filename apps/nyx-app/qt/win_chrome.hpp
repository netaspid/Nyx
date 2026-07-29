#pragma once

#include <QIcon>

class QWindow;

void nyxApplyNativeChromeDark(QWindow* window, bool dark);

void nyxApplyNativeChromeDarkAll(bool dark);

QIcon nyxAppIcon();
