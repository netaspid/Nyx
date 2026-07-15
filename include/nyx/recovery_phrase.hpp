#pragma once

/** @file recovery_phrase.hpp
 *  Локальная recovery-фраза (12 слов, BIP39 English) для сброса пароля аккаунта.
 */

#include <string>
#include <vector>

namespace nyx {

/** Генерирует 12-словную фразу (128 бит энтропии + checksum биты BIP39). */
std::string generate_recovery_phrase();

/** Нормализует пробелы/регистр и проверяет слова + checksum.
 *  @param phrase ввод пользователя
 *  @param normalized_out каноническая строка (слова через один пробел), если ok
 *  @return false при неизвестном слове или неверном checksum
 */
bool normalize_recovery_phrase(const std::string& phrase, std::string* normalized_out,
                               std::string* err = nullptr);

/** Разбивает нормализованную фразу на слова. */
std::vector<std::string> split_recovery_words(const std::string& normalized_phrase);

}  // namespace nyx
