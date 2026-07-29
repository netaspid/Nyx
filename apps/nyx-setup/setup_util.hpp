#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "payload.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nyx_setup {

using InstallProgressFn = std::function<void(int percent, const wchar_t* status)>;

bool stop_nyx_for_install(const std::wstring& install_dir, std::wstring* err = nullptr);

bool extract_payload(const std::vector<std::uint8_t>& blob,
                     const std::wstring& target_dir,
                     InstallProgressFn progress,
                     std::wstring* err);

bool create_shortcut(const std::wstring& lnk_path,
                     const std::wstring& target,
                     const std::wstring& work_dir,
                     const std::wstring& desc);

bool register_uninstall(const std::wstring& install_dir, const std::wstring& uninstall_exe);

bool launch_app(const std::wstring& exe_path);

bool ensure_system_prerequisites(std::wstring* err = nullptr);

bool ensure_document_dependencies(const std::wstring& install_dir,
                                  std::wstring* err = nullptr,
                                  InstallProgressFn progress = nullptr);

bool verify_installation(const std::wstring& install_dir, std::wstring* err = nullptr);

bool repair_installation(const std::vector<std::uint8_t>& blob,
                         const std::wstring& install_dir,
                         std::wstring* err = nullptr);

std::wstring default_install_dir();

bool browse_for_folder(HWND owner, std::wstring& path);

} // namespace nyx_setup
