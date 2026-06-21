#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nyx_setup {

/** Reads NYXI payload appended to the running executable. */
bool read_self_payload(std::vector<std::uint8_t>& out);

struct PayloadFile {
  std::string relative_path;
  std::vector<std::uint8_t> data;
};

/** Parses NYXI blob into files. */
bool parse_payload(const std::vector<std::uint8_t>& blob, std::vector<PayloadFile>& files);

using ProgressFn = std::function<void(int percent, const wchar_t* status)>;

/** Stops Nyx processes that lock files in install_dir (upgrade/reinstall). */
bool stop_nyx_for_install(const std::wstring& install_dir, std::wstring* err = nullptr);

bool extract_payload(const std::vector<std::uint8_t>& blob, const std::wstring& target_dir,
                     ProgressFn progress, std::wstring* err);

bool create_shortcut(const std::wstring& lnk_path, const std::wstring& target,
                     const std::wstring& work_dir, const std::wstring& desc);

bool register_uninstall(const std::wstring& install_dir, const std::wstring& uninstall_exe);

bool launch_app(const std::wstring& exe_path);

/** Windows 10+ x64; returns false with Russian message in err. */
bool ensure_system_prerequisites(std::wstring* err = nullptr);

/** Checks extracted files and that Qt/runtime DLLs load. */
bool verify_installation(const std::wstring& install_dir, std::wstring* err = nullptr);

/** Re-copies payload files that are missing or empty under install_dir. */
bool repair_installation(const std::vector<std::uint8_t>& blob, const std::wstring& install_dir,
                         std::wstring* err = nullptr);

std::wstring default_install_dir();

bool browse_for_folder(HWND owner, std::wstring& path);

}  // namespace nyx_setup
