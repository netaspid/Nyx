#pragma once

/** @file console.hpp
 *  Console setup for correct UTF-8 output (Windows).
 */

namespace nyx {

/** Switches stdin/stdout to UTF-8. Call before any output on Windows. */
void setup_console_utf8();

}  // namespace nyx
