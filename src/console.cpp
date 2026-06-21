#include "nyx/console.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace nyx {

void setup_console_utf8() {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  if (HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE); out != INVALID_HANDLE_VALUE) {
    DWORD mode = 0;
    if (GetConsoleMode(out, &mode)) {
      SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
  }
  if (HANDLE err = GetStdHandle(STD_ERROR_HANDLE); err != INVALID_HANDLE_VALUE) {
    DWORD mode = 0;
    if (GetConsoleMode(err, &mode)) {
      SetConsoleMode(err, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
  }
#endif
}

}  // namespace nyx
