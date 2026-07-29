#include "setup_util.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <fstream>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace nyx_setup {

namespace {

constexpr char kMagic[4] = {'N', 'Y', 'X', 'I'};
constexpr char kFooterMagic[4] = {'N', 'Y', 'X', 'F'};
constexpr std::size_t kFooterSize = 12;

std::uint32_t read_u32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

std::uint64_t read_u64(const std::uint8_t* p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  return v;
}

bool ensure_parent_dirs(const std::wstring& file_path) {
  const auto pos = file_path.find_last_of(L"\\/");
  if (pos == std::wstring::npos) return true;
  std::wstring dir = file_path.substr(0, pos);
  for (size_t i = 0; i < dir.size(); ++i) {
    if (dir[i] == L'\\' || dir[i] == L'/') {
      const std::wstring sub = dir.substr(0, i);
      if (sub.size() >= 3) CreateDirectoryW(sub.c_str(), nullptr);
    }
  }
  return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::wstring win32_error_text(DWORD code) {
  wchar_t* buf = nullptr;
  const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                     FORMAT_MESSAGE_IGNORE_INSERTS,
                                 nullptr, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
  std::wstring out;
  if (n && buf) {
    out.assign(buf, n);
    while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' ')) {
      out.pop_back();
    }
  }
  LocalFree(buf);
  return out;
}

std::wstring normalize_dir(std::wstring path) {
  while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
  return path;
}

bool write_file_bytes(const std::wstring& path, const void* data, std::size_t size,
                      std::wstring* err) {
  const std::wstring temp = path + L".nyx.tmp";
  for (int attempt = 0; attempt < 10; ++attempt) {
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(temp.c_str());
    if (attempt > 0) DeleteFileW(path.c_str());

    const HANDLE h =
        CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      Sleep(200 * static_cast<DWORD>(attempt + 1));
      continue;
    }
    DWORD written = 0;
    const bool wrote =
        WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr) && written == size;
    CloseHandle(h);
    if (!wrote) {
      DeleteFileW(temp.c_str());
      Sleep(200 * static_cast<DWORD>(attempt + 1));
      continue;
    }
    if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      return true;
    }
    DeleteFileW(temp.c_str());
    Sleep(200 * static_cast<DWORD>(attempt + 1));
  }

  if (err) {
    const DWORD code = GetLastError();
    *err = L"Не удалось записать:\n" + path;
    const std::wstring sys = win32_error_text(code);
    if (!sys.empty()) *err += L"\n" + sys;
    if (code == ERROR_SHARING_VIOLATION || code == ERROR_ACCESS_DENIED) {
      *err += L"\n\nЗакройте Nyx и окно проводника с папкой установки, затем повторите.";
    }
  }
  return false;
}

bool process_exe_matches_dir(DWORD pid, const std::wstring& install_dir) {
  const HANDLE proc =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid);
  if (!proc) return false;
  wchar_t image[MAX_PATH]{};
  DWORD image_len = MAX_PATH;
  const bool got = QueryFullProcessImageNameW(proc, 0, image, &image_len) != 0;
  CloseHandle(proc);
  if (!got) return true;

  std::wstring path(image);
  const std::wstring dir = normalize_dir(install_dir);
  if (dir.empty()) return true;
  if (path.size() < dir.size()) return false;
  if (_wcsnicmp(path.c_str(), dir.c_str(), dir.size()) != 0) return false;
  return path[dir.size()] == L'\\' || path[dir.size()] == L'/';
}

bool terminate_by_name_in_dir(const wchar_t* exe_name, const std::wstring& install_dir) {
  bool stopped = false;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;

  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, exe_name) != 0) continue;
      if (!process_exe_matches_dir(pe.th32ProcessID, install_dir)) continue;
      const HANDLE proc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
      if (!proc) continue;
      TerminateProcess(proc, 0);
      WaitForSingleObject(proc, 5000);
      CloseHandle(proc);
      stopped = true;
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return stopped;
}

}  // namespace

bool stop_nyx_for_install(const std::wstring& install_dir, std::wstring* err) {
  (void)err;
  const wchar_t* names[] = {L"nyx-app.exe", L"nyx-node.exe", L"nyx-rendezvous.exe"};
  bool any = false;
  for (const wchar_t* name : names) {
    if (terminate_by_name_in_dir(name, install_dir)) any = true;
  }
  if (any) Sleep(1000);
  return true;
}

bool read_self_payload(std::vector<std::uint8_t>& out) {
  wchar_t self[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return false;
  std::ifstream in(self, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < static_cast<std::streamoff>(kFooterSize + 12)) return false;
  std::vector<std::uint8_t> exe(static_cast<size_t>(size));
  in.seekg(0);
  in.read(reinterpret_cast<char*>(exe.data()), size);
  if (!in) return false;

  const std::size_t footer_at = static_cast<std::size_t>(size) - kFooterSize;
  if (std::memcmp(exe.data() + footer_at + 8, kFooterMagic, 4) != 0) return false;

  const std::uint64_t payload_off = read_u64(exe.data() + footer_at);
  if (payload_off + 12 > footer_at) return false;
  if (std::memcmp(exe.data() + payload_off, kMagic, 4) != 0) return false;

  out.assign(exe.begin() + static_cast<std::ptrdiff_t>(payload_off),
             exe.begin() + static_cast<std::ptrdiff_t>(footer_at));
  return true;
}

bool parse_payload(const std::vector<std::uint8_t>& blob, std::vector<PayloadFile>& files) {
  files.clear();
  if (blob.size() < 12 || std::memcmp(blob.data(), kMagic, 4) != 0) return false;
  const std::uint32_t version = read_u32(blob.data() + 4);
  if (version != 1) return false;
  const std::uint32_t count = read_u32(blob.data() + 8);
  size_t off = 12;
  files.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    if (off + 4 > blob.size()) return false;
    const std::uint32_t path_len = read_u32(blob.data() + off);
    off += 4;
    if (off + path_len + 8 > blob.size()) return false;
    PayloadFile f;
    f.relative_path.assign(reinterpret_cast<const char*>(blob.data() + off), path_len);
    off += path_len;
    const std::uint64_t data_len = read_u64(blob.data() + off);
    off += 8;
    if (off + data_len > blob.size()) return false;
    f.data.assign(blob.begin() + off, blob.begin() + off + static_cast<size_t>(data_len));
    off += static_cast<size_t>(data_len);
    files.push_back(std::move(f));
  }
  return true;
}

bool extract_payload(const std::vector<std::uint8_t>& blob, const std::wstring& target_dir,
                     ProgressFn progress, std::wstring* err) {
  std::vector<PayloadFile> files;
  if (!parse_payload(blob, files)) {
    if (err) *err = L"Invalid installer payload";
    return false;
  }
  CreateDirectoryW(target_dir.c_str(), nullptr);
  const size_t total = files.size();
  for (size_t i = 0; i < total; ++i) {
    const auto& f = files[i];
    const std::wstring rel = utf8_to_wide(f.relative_path);
    std::wstring full = target_dir;
    if (!full.empty() && full.back() != L'\\') full += L'\\';
    full += rel;
    for (auto& c : full) {
      if (c == L'/') c = L'\\';
    }
    if (!ensure_parent_dirs(full)) {
      if (err) *err = L"Cannot create folder: " + full;
      return false;
    }
    if (f.data.empty()) {
      CreateDirectoryW(full.c_str(), nullptr);
    } else {
      if (!write_file_bytes(full, f.data.data(), f.data.size(), err)) return false;
    }
    if (progress) {
      const int pct = static_cast<int>((100 * (i + 1)) / total);
      progress(pct, rel.c_str());
    }
  }
  return true;
}

bool create_shortcut(const std::wstring& lnk_path, const std::wstring& target,
                     const std::wstring& work_dir, const std::wstring& desc) {
  CoInitialize(nullptr);
  IShellLinkW* link = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                              reinterpret_cast<void**>(&link)))) {
    return false;
  }
  link->SetPath(target.c_str());
  link->SetWorkingDirectory(work_dir.c_str());
  link->SetDescription(desc.c_str());
  IPersistFile* file = nullptr;
  if (FAILED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
    link->Release();
    return false;
  }
  const bool ok = SUCCEEDED(file->Save(lnk_path.c_str(), TRUE));
  file->Release();
  link->Release();
  CoUninitialize();
  return ok;
}

bool register_uninstall(const std::wstring& install_dir, const std::wstring& uninstall_exe) {
  HKEY key = nullptr;
  const std::wstring subkey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Nyx";
  if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                      nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const std::wstring uninstall = L"\"" + uninstall_exe + L"\"";
  RegSetValueExW(key, L"DisplayName", 0, REG_SZ, reinterpret_cast<const BYTE*>(L"Nyx"),
                 static_cast<DWORD>((wcslen(L"Nyx") + 1) * sizeof(wchar_t)));
  RegSetValueExW(key, L"DisplayVersion", 0, REG_SZ, reinterpret_cast<const BYTE*>(L"0.1.0"),
                 static_cast<DWORD>((wcslen(L"0.1.0") + 1) * sizeof(wchar_t)));
  RegSetValueExW(key, L"Publisher", 0, REG_SZ, reinterpret_cast<const BYTE*>(L"Nyx"),
                 static_cast<DWORD>((wcslen(L"Nyx") + 1) * sizeof(wchar_t)));
  RegSetValueExW(key, L"InstallLocation", 0, REG_SZ,
                 reinterpret_cast<const BYTE*>(install_dir.c_str()),
                 static_cast<DWORD>((install_dir.size() + 1) * sizeof(wchar_t)));
  RegSetValueExW(key, L"UninstallString", 0, REG_SZ,
                 reinterpret_cast<const BYTE*>(uninstall.c_str()),
                 static_cast<DWORD>((uninstall.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  return true;
}

bool launch_app(const std::wstring& exe_path) {
  return reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", exe_path.c_str(), nullptr,
                                                  nullptr, SW_SHOWNORMAL)) > 32;
}

namespace {

bool path_exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool can_load_library(const std::wstring& full_path) {
  const HMODULE mod = LoadLibraryExW(full_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!mod) return false;
  FreeLibrary(mod);
  return true;
}

/** Real OS version (IsWindows10OrGreater lies without an app manifest). */
bool windows_build_at_least(DWORD major, DWORD minor, DWORD build) {
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) return true;
  const auto rtl =
      reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
  if (!rtl) return true;

  OSVERSIONINFOEXW ver{};
  ver.dwOSVersionInfoSize = sizeof(ver);
  if (rtl(&ver) != 0) return true;

  if (ver.dwMajorVersion != major) return ver.dwMajorVersion > major;
  if (ver.dwMinorVersion != minor) return ver.dwMinorVersion > minor;
  return ver.dwBuildNumber >= build;
}

}  // namespace

bool ensure_system_prerequisites(std::wstring* err) {
  if (!windows_build_at_least(10, 0, 0)) {
    if (err) {
      *err = L"Nyx требует Windows 10 или новее. Обновите систему через «Параметры → Обновление и безопасность».";
    }
    return false;
  }
  SYSTEM_INFO info{};
  GetNativeSystemInfo(&info);
  if (info.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64) {
    if (err) *err = L"Nyx работает только на 64-bit Windows.";
    return false;
  }
  return true;
}

namespace {

bool path_exists_w(const std::wstring& path) {
  const DWORD attrs = GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES;
}

bool tool_on_path_w(const wchar_t* name) {
  wchar_t buf[MAX_PATH];
  const DWORD n = SearchPathW(nullptr, name, L".exe", MAX_PATH, buf, nullptr);
  return n > 0 && n < MAX_PATH;
}

std::wstring expand_env_w(const wchar_t* pattern) {
  wchar_t buf[MAX_PATH * 2];
  const DWORD cap = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
  const DWORD n = ExpandEnvironmentStringsW(pattern, buf, cap);
  if (n == 0 || n > cap) return {};
  return buf;
}

std::wstring app_paths_soffice_w(HKEY root) {
  HKEY key = nullptr;
  const REGSAM access = KEY_READ | KEY_WOW64_64KEY;
  if (RegOpenKeyExW(root,
                    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\soffice.exe", 0,
                    access, &key) != ERROR_SUCCESS) {
    return {};
  }
  wchar_t path[MAX_PATH];
  DWORD type = 0;
  DWORD size = sizeof(path);
  const LONG rc =
      RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<LPBYTE>(path), &size);
  RegCloseKey(key);
  if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
  std::wstring resolved = (type == REG_EXPAND_SZ) ? expand_env_w(path) : std::wstring(path);
  if (!path_exists_w(resolved)) return {};
  return resolved;
}

std::wstring find_soffice_exe_w() {
  wchar_t buf[MAX_PATH];
  if (SearchPathW(nullptr, L"soffice.exe", nullptr, MAX_PATH, buf, nullptr) > 0 &&
      path_exists_w(buf)) {
    return buf;
  }
  if (SearchPathW(nullptr, L"soffice", L".exe", MAX_PATH, buf, nullptr) > 0 &&
      path_exists_w(buf)) {
    return buf;
  }

  for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
    std::wstring from_reg = app_paths_soffice_w(root);
    if (!from_reg.empty()) return from_reg;
  }

  static const wchar_t* kCandidates[] = {
      L"%ProgramFiles%\\LibreOffice\\program\\soffice.exe",
      L"%ProgramFiles(x86)%\\LibreOffice\\program\\soffice.exe",
      L"%LOCALAPPDATA%\\Programs\\LibreOffice\\program\\soffice.exe",
  };
  for (const wchar_t* pattern : kCandidates) {
    const std::wstring path = expand_env_w(pattern);
    if (!path.empty() && path_exists_w(path)) return path;
  }
  return {};
}

bool has_pdf_tools_w(const std::wstring& install_dir) {
  if (tool_on_path_w(L"mutool") || tool_on_path_w(L"pdftoppm")) return true;
  if (!install_dir.empty()) {
    if (path_exists_w(install_dir + L"\\tools\\mutool.exe")) return true;
    if (path_exists_w(install_dir + L"\\tools\\pdftoppm.exe")) return true;
  }
  return false;
}

bool has_office_tools_w() { return !find_soffice_exe_w().empty(); }

int run_hidden(const std::wstring& cmd) {
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  std::wstring mutable_cmd = cmd;
  if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &si, &pi)) {
    return -1;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return static_cast<int>(code);
}

bool winget_available() {
  return tool_on_path_w(L"winget");
}

bool winget_install(const wchar_t* package_id) {
  // Use cmd so winget from App Installer resolves on PATH.
  const std::wstring cmd =
      std::wstring(L"cmd.exe /C winget install --id ") + package_id +
      L" -e --accept-package-agreements --accept-source-agreements --disable-interactivity";
  return run_hidden(cmd) == 0;
}

bool download_file_ps(const std::wstring& url, const std::wstring& dest) {
  const std::wstring cmd =
      L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
      L"\"[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; "
      L"Invoke-WebRequest -Uri '" +
      url + L"' -OutFile '" + dest + L"'\"";
  return run_hidden(cmd) == 0 && path_exists_w(dest);
}

bool expand_archive_ps(const std::wstring& zip, const std::wstring& dest_dir) {
  CreateDirectoryW(dest_dir.c_str(), nullptr);
  const std::wstring cmd =
      L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
      L"\"Expand-Archive -Force -Path '" +
      zip + L"' -DestinationPath '" + dest_dir + L"'\"";
  return run_hidden(cmd) == 0;
}

bool find_and_copy_mutool(const std::wstring& extract_root, const std::wstring& tools_dir) {
  // Recursively look for mutool.exe under extract_root (limited depth via PowerShell).
  const std::wstring cmd =
      L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
      L"\"$f = Get-ChildItem -Path '" +
      extract_root +
      L"' -Filter mutool.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1; "
      L"if ($null -eq $f) { exit 2 }; "
      L"New-Item -ItemType Directory -Force -Path '" +
      tools_dir +
      L"' | Out-Null; "
      L"Copy-Item -Force $f.FullName (Join-Path '" +
      tools_dir + L"' 'mutool.exe'); "
      L"$dir = Split-Path $f.FullName; "
      L"Get-ChildItem $dir -Filter *.dll | Copy-Item -Force -Destination '" +
      tools_dir + L"'\"";
  return run_hidden(cmd) == 0 && path_exists_w(tools_dir + L"\\mutool.exe");
}

bool install_mupdf_tools(const std::wstring& install_dir, std::wstring* err) {
  if (has_pdf_tools_w(install_dir)) return true;

  if (winget_available()) {
    // Best-effort package ids; ignore failure and fall through to download.
    winget_install(L"ArtifexSoftware.MuPDF");
    if (has_pdf_tools_w(install_dir)) return true;
    winget_install(L"oschwartz10612.Poppler");
    if (has_pdf_tools_w(install_dir)) return true;
  }

  wchar_t temp_path[MAX_PATH];
  if (GetTempPathW(MAX_PATH, temp_path) == 0) {
    if (err) *err = L"Не удалось получить временный каталог";
    return false;
  }
  const std::wstring zip = std::wstring(temp_path) + L"nyx-mupdf.zip";
  const std::wstring extract = std::wstring(temp_path) + L"nyx-mupdf";
  const std::wstring tools = install_dir + L"\\tools";

  // Official MuPDF Windows build (includes mutool.exe).
  const std::wstring url =
      L"https://github.com/ArtifexSoftware/mupdf-downloads/releases/download/1.28.0/"
      L"mupdf-1.28.0-windows.zip";

  DeleteFileW(zip.c_str());
  if (!download_file_ps(url, zip)) {
    if (err) {
      *err =
          L"Не удалось скачать MuPDF. Установите вручную (winget install ArtifexSoftware.MuPDF) "
          L"или положите mutool.exe в папку tools рядом с nyx-app.exe.";
    }
    return false;
  }
  CreateDirectoryW(extract.c_str(), nullptr);
  if (!expand_archive_ps(zip, extract)) {
    if (err) *err = L"Не удалось распаковать MuPDF";
    DeleteFileW(zip.c_str());
    return false;
  }
  const bool ok = find_and_copy_mutool(extract, tools);
  DeleteFileW(zip.c_str());
  if (!ok && err) {
    *err = L"В архиве MuPDF не найден mutool.exe";
  }
  return ok;
}

bool install_libreoffice(std::wstring* err) {
  if (has_office_tools_w()) return true;
  if (winget_available()) {
    winget_install(L"TheDocumentFoundation.LibreOffice");
    if (has_office_tools_w()) return true;
  }
  if (err) {
    *err = winget_available()
               ? L"Не удалось установить LibreOffice через winget. Установите вручную с "
                 L"https://www.libreoffice.org/"
               : L"LibreOffice не найден, а winget недоступен. Установите LibreOffice вручную для "
                 L"просмотра Office-документов.";
  }
  return false;
}

}  // namespace

bool ensure_document_dependencies(const std::wstring& install_dir, std::wstring* err,
                                  ProgressFn progress) {
  std::wstring notes;
  auto append = [&](const std::wstring& msg) {
    if (!notes.empty()) notes += L"\n";
    notes += msg;
  };

  if (progress) progress(92, L"Зависимости: PDF…");
  std::wstring pdf_err;
  if (!has_pdf_tools_w(install_dir)) {
    if (!install_mupdf_tools(install_dir, &pdf_err)) {
      append(pdf_err.empty() ? L"PDF-утилиты не установлены" : pdf_err);
    }
  }

  if (progress) progress(96, L"Зависимости: LibreOffice…");
  std::wstring office_err;
  if (!has_office_tools_w()) {
    if (!install_libreoffice(&office_err)) {
      append(office_err.empty() ? L"LibreOffice не установлен" : office_err);
    }
  }

  if (err) *err = notes;
  // Soft-fail: Nyx install succeeds even if optional document deps are incomplete.
  return true;
}

bool verify_installation(const std::wstring& install_dir, std::wstring* err) {
  const std::wstring app = install_dir + L"\\nyx-app.exe";
  if (!path_exists(app)) {
    if (err) *err = L"nyx-app.exe not found after install";
    return false;
  }

  const wchar_t* required[] = {
      L"Qt6Core.dll",
      L"Qt6Gui.dll",
      L"Qt6Qml.dll",
      L"Qt6Quick.dll",
      L"Qt6Network.dll",
      L"Qt6Multimedia.dll",
      L"libgcc_s_seh-1.dll",
      L"libstdc++-6.dll",
      L"libwinpthread-1.dll",
      L"platforms\\qwindows.dll",
      L"multimedia\\windowsmediaplugin.dll",
  };

  for (const wchar_t* rel : required) {
    const std::wstring full = install_dir + L"\\" + rel;
    if (!path_exists(full)) {
      if (err) *err = L"Missing component: " + std::wstring(rel);
      return false;
    }
  }

  if (!can_load_library(install_dir + L"\\Qt6Core.dll")) {
    if (err) {
      *err = L"Qt6Core.dll не загружается. Установите обновления Windows и повторите установку.";
    }
    return false;
  }

  return true;
}

bool repair_installation(const std::vector<std::uint8_t>& blob, const std::wstring& install_dir,
                         std::wstring* err) {
  std::vector<PayloadFile> files;
  if (!parse_payload(blob, files)) {
    if (err) *err = L"Invalid installer payload";
    return false;
  }
  for (const auto& f : files) {
    if (f.data.empty()) continue;
    const std::wstring rel = utf8_to_wide(f.relative_path);
    std::wstring full = install_dir;
    if (!full.empty() && full.back() != L'\\') full += L'\\';
    full += rel;
    for (auto& c : full) {
      if (c == L'/') c = L'\\';
    }
    bool needs_copy = true;
    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    if (GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &attrs)) {
      const ULONGLONG existing =
          (static_cast<ULONGLONG>(attrs.nFileSizeHigh) << 32) | attrs.nFileSizeLow;
      if (existing == f.data.size()) needs_copy = false;
    }
    if (!needs_copy) continue;
    if (!ensure_parent_dirs(full)) {
      if (err) *err = L"Cannot create folder: " + full;
      return false;
    }
    if (!write_file_bytes(full, f.data.data(), f.data.size(), err)) return false;
  }
  return true;
}

std::wstring default_install_dir() {
  wchar_t base[MAX_PATH];
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))) return L"C:\\Nyx";
  std::wstring out = base;
  out += L"\\Programs\\Nyx";
  return out;
}

bool browse_for_folder(HWND owner, std::wstring& path) {
  BROWSEINFOW bi{};
  bi.hwndOwner = owner;
  bi.lpszTitle = L"Select installation folder";
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
  if (!pidl) return false;
  wchar_t buf[MAX_PATH];
  if (!SHGetPathFromIDListW(pidl, buf)) {
    CoTaskMemFree(pidl);
    return false;
  }
  CoTaskMemFree(pidl);
  path = buf;
  return true;
}

}  // namespace nyx_setup
