#pragma once

/** @file util.hpp
 *  Вспомогательные функции: CRC, hex, little-endian, случайные байты.
 */

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nyx {

/** CRC32 (IEEE), используется для опционального флага FLAG_CRC. */
uint32_t crc32(const uint8_t* data, std::size_t len);

/** Байты в строку hex (нижний регистр). */
std::string to_hex(const uint8_t* data, std::size_t len);

/** Парсинг hex в байты. @return false при нечётной длине или неверном символе. */
bool from_hex(const std::string& hex, std::vector<uint8_t>& out);

void write_u16_le(std::vector<uint8_t>& buf, uint16_t v);
void write_u32_le(std::vector<uint8_t>& buf, uint32_t v);
void write_u64_le(std::vector<uint8_t>& buf, uint64_t v);
uint16_t read_u16_le(const uint8_t* p);
uint32_t read_u32_le(const uint8_t* p);
uint64_t read_u64_le(const uint8_t* p);

/** Заполняет буфер криптографически стойкими случайными байтами. */
void random_bytes(uint8_t* out, std::size_t len);

/** Парсит "host:port". @return false при ошибке формата или порта. */
bool parse_host_port(const std::string& addr, std::string& host, uint16_t& port);

/** Сравнивает адрес отправителя UDP с ожидаемым host:port (учитывает DNS → IP). */
bool endpoint_matches(const std::string& from_host, uint16_t from_port,
                      const std::string& expected_host, uint16_t expected_port);

/** Путь из UTF-8 (Qt, JSON). На Windows — через wide API. */
std::filesystem::path path_from_utf8(const std::string& utf8);

/** Путь в UTF-8 для хранения и UI. */
std::string path_to_utf8(const std::filesystem::path& path);

/** Нормализованный UTF-8 путь (lexically_normal). */
std::string normalize_utf8_path(const std::string& utf8);

/** Нормализует share-корень для сравнения в grants (на Windows — нижний регистр). */
std::string normalize_grant_root(const std::string& root_path);

}  // namespace nyx
