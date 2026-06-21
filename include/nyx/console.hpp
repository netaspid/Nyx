#pragma once

/** @file console.hpp
 *  Настройка консоли для корректного вывода UTF-8 (Windows).
 */

namespace nyx {

/** Переключает stdin/stdout на UTF-8. На Windows вызывать до первого вывода. */
void setup_console_utf8();

}  // namespace nyx
