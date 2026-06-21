#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>

#include <string>

#pragma comment(lib, "shell32.lib")

static bool remove_dir_recursive(const std::wstring& dir) {
  WIN32_FIND_DATAW fd;
  const std::wstring pattern = dir + L"\\*";
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  do {
    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
    const std::wstring path = dir + L"\\" + fd.cFileName;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      remove_dir_recursive(path);
    } else {
      SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
      DeleteFileW(path.c_str());
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return RemoveDirectoryW(dir.c_str()) != 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  const int r = MessageBoxW(nullptr, L"Remove Nyx from this computer?", L"Uninstall Nyx",
                            MB_YESNO | MB_ICONQUESTION);
  if (r != IDYES) return 0;

  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  std::wstring install_dir = self;
  const auto pos = install_dir.find_last_of(L"\\/");
  if (pos != std::wstring::npos) install_dir = install_dir.substr(0, pos);

  wchar_t programs[MAX_PATH];
  SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0, programs);
  DeleteFileW((std::wstring(programs) + L"\\Nyx\\Nyx.lnk").c_str());
  RemoveDirectoryW((std::wstring(programs) + L"\\Nyx").c_str());

  wchar_t desktop[MAX_PATH];
  SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop);
  DeleteFileW((std::wstring(desktop) + L"\\Nyx.lnk").c_str());

  RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Nyx");

  const int del_data =
      MessageBoxW(nullptr, L"Delete local Nyx data (accounts, chats, keys)?", L"Uninstall Nyx",
                  MB_YESNO | MB_ICONWARNING);
  if (del_data == IDYES) {
    wchar_t appdata[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata);
    remove_dir_recursive(std::wstring(appdata) + L"\\nyx");
  }

  remove_dir_recursive(install_dir);
  MessageBoxW(nullptr, L"Nyx has been removed.", L"Uninstall Nyx", MB_ICONINFORMATION);
  return 0;
}
