#include "setup_util.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>

#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace {

enum {
  IDC_TITLE = 1001,
  IDC_PATH_LABEL,
  IDC_PATH,
  IDC_BROWSE,
  IDC_DESKTOP,
  IDC_PROGRESS,
  IDC_STATUS,
  IDC_INSTALL,
  IDC_CANCEL,
};

HWND g_path_edit = nullptr;
HWND g_desktop_chk = nullptr;
HWND g_progress = nullptr;
HWND g_status = nullptr;
HWND g_install_btn = nullptr;
std::vector<std::uint8_t> g_payload;

void set_status(HWND hwnd, const wchar_t* text) {
  SetWindowTextW(g_status, text);
}

void set_progress(int pct) {
  SendMessageW(g_progress, PBM_SETPOS, pct, 0);
}

std::wstring get_path_text() {
  wchar_t buf[MAX_PATH * 2];
  GetWindowTextW(g_path_edit, buf, static_cast<int>(std::size(buf)));
  return buf;
}

void run_install(HWND hwnd) {
  std::wstring dir = get_path_text();
  if (dir.empty()) {
    MessageBoxW(hwnd, L"Укажите папку установки.", L"Nyx Setup", MB_ICONWARNING);
    return;
  }
  EnableWindow(g_install_btn, FALSE);
  EnableWindow(g_path_edit, FALSE);
  EnableWindow(GetDlgItem(hwnd, IDC_BROWSE), FALSE);
  ShowWindow(g_progress, SW_SHOW);
  std::wstring err;
  set_status(hwnd, L"Закрытие Nyx...");
  nyx_setup::stop_nyx_for_install(dir, &err);

  set_status(hwnd, L"Установка...");
  set_progress(0);

  err.clear();
  const bool ok = nyx_setup::extract_payload(
      g_payload, dir,
      [hwnd](int pct, const wchar_t* file) {
        set_progress(pct);
        set_status(hwnd, (std::wstring(L"Копирование: ") + file).c_str());
      },
      &err);

  if (!ok) {
    MessageBoxW(hwnd,
                (err + L"\n\nЕсли Nyx уже установлен — закройте приложение и повторите.")
                    .c_str(),
                L"Nyx Setup", MB_ICONERROR);
    EnableWindow(g_install_btn, TRUE);
    EnableWindow(g_path_edit, TRUE);
    EnableWindow(GetDlgItem(hwnd, IDC_BROWSE), TRUE);
    return;
  }

  set_status(hwnd, L"Проверка компонентов...");
  if (!nyx_setup::verify_installation(dir, &err)) {
    set_status(hwnd, L"Доустановка недостающих файлов...");
    if (!nyx_setup::repair_installation(g_payload, dir, &err) ||
        !nyx_setup::verify_installation(dir, &err)) {
      MessageBoxW(hwnd,
                  (L"Установка не прошла проверку:\n" + err +
                   L"\n\nВсе библиотеки входят в установщик — попробуйте другую папку или "
                   L"запустите от имени администратора.")
                      .c_str(),
                  L"Nyx Setup", MB_ICONERROR);
      EnableWindow(g_install_btn, TRUE);
      EnableWindow(g_path_edit, TRUE);
      EnableWindow(GetDlgItem(hwnd, IDC_BROWSE), TRUE);
      return;
    }
  }

  const std::wstring app_exe = dir + L"\\nyx-app.exe";
  const std::wstring uninstall_exe = dir + L"\\nyx-uninstall.exe";

  wchar_t programs[MAX_PATH];
  SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0, programs);
  const std::wstring menu_dir = std::wstring(programs) + L"\\Nyx";
  CreateDirectoryW(menu_dir.c_str(), nullptr);
  nyx_setup::create_shortcut(menu_dir + L"\\Nyx.lnk", app_exe, dir, L"Nyx");

  if (SendMessageW(g_desktop_chk, BM_GETCHECK, 0, 0) == BST_CHECKED) {
    wchar_t desktop[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop);
    nyx_setup::create_shortcut(std::wstring(desktop) + L"\\Nyx.lnk", app_exe, dir, L"Nyx");
  }

  nyx_setup::register_uninstall(dir, uninstall_exe);
  set_progress(100);
  set_status(hwnd, L"Готово.");

  const int launch =
      MessageBoxW(hwnd, L"Nyx установлен.\n\nЗапустить сейчас?", L"Nyx Setup",
                  MB_YESNO | MB_ICONINFORMATION);
  if (launch == IDYES) nyx_setup::launch_app(app_exe);
  DestroyWindow(hwnd);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      CreateWindowW(L"STATIC", L"Установка Nyx", WS_CHILD | WS_VISIBLE, 20, 16, 360, 24, hwnd,
                    reinterpret_cast<HMENU>(IDC_TITLE), nullptr, nullptr);
      CreateWindowW(L"STATIC",
                    L"P2P-мессенджер. Qt и все библиотеки входят в установку — ничего ставить "
                    L"отдельно не нужно.",
                    WS_CHILD | WS_VISIBLE, 20, 40, 360, 32, hwnd, nullptr, nullptr, nullptr);
      CreateWindowW(L"STATIC", L"Папка установки:", WS_CHILD | WS_VISIBLE, 20, 84, 120, 18, hwnd,
                    reinterpret_cast<HMENU>(IDC_PATH_LABEL), nullptr, nullptr);
      g_path_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nyx_setup::default_install_dir().c_str(),
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 20, 104, 280, 24, hwnd,
                                    reinterpret_cast<HMENU>(IDC_PATH), nullptr, nullptr);
      CreateWindowW(L"BUTTON", L"Обзор...", WS_CHILD | WS_VISIBLE, 308, 102, 72, 26, hwnd,
                    reinterpret_cast<HMENU>(IDC_BROWSE), nullptr, nullptr);
      g_desktop_chk = CreateWindowW(L"BUTTON", L"Ярлык на рабочем столе", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                    20, 140, 260, 22, hwnd, reinterpret_cast<HMENU>(IDC_DESKTOP),
                                    nullptr, nullptr);
      SendMessageW(g_desktop_chk, BM_SETCHECK, BST_CHECKED, 0);
      g_progress =
          CreateWindowW(PROGRESS_CLASSW, nullptr, WS_CHILD | PBS_SMOOTH, 20, 172, 360, 18, hwnd,
                        reinterpret_cast<HMENU>(IDC_PROGRESS), nullptr, nullptr);
      ShowWindow(g_progress, SW_HIDE);
      g_status = CreateWindowW(L"STATIC", L"Готово к установке.", WS_CHILD | WS_VISIBLE, 20, 196,
                               360, 18, hwnd, reinterpret_cast<HMENU>(IDC_STATUS), nullptr, nullptr);
      g_install_btn = CreateWindowW(L"BUTTON", L"Установить", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                    200, 232, 88, 30, hwnd, reinterpret_cast<HMENU>(IDC_INSTALL),
                                    nullptr, nullptr);
      CreateWindowW(L"BUTTON", L"Отмена", WS_CHILD | WS_VISIBLE, 292, 232, 88, 30, hwnd,
                    reinterpret_cast<HMENU>(IDC_CANCEL), nullptr, nullptr);
      return 0;
    }
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDC_BROWSE: {
          std::wstring path = get_path_text();
          if (nyx_setup::browse_for_folder(hwnd, path)) SetWindowTextW(g_path_edit, path.c_str());
          return 0;
        }
        case IDC_INSTALL:
          run_install(hwnd);
          return 0;
        case IDC_CANCEL:
          DestroyWindow(hwnd);
          return 0;
      }
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  if (!nyx_setup::read_self_payload(g_payload)) {
    MessageBoxW(nullptr,
                L"Это не установщик Nyx.\n\n"
                L"Соберите проект:\n"
                L"  cmake --build build -j\n\n"
                L"Запустите:\n"
                L"  build\\NyxSetup.exe",
                L"Nyx Setup", MB_ICONERROR);
    return 1;
  }

  std::wstring prereq_err;
  if (!nyx_setup::ensure_system_prerequisites(&prereq_err)) {
    MessageBoxW(nullptr, prereq_err.c_str(), L"Nyx Setup", MB_ICONERROR);
    return 1;
  }

  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS};
  InitCommonControlsEx(&icc);

  const wchar_t* cls = L"NyxSetupWnd";
  WNDCLASSW wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = cls;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&wc);

  HWND hwnd = CreateWindowW(cls, L"Nyx Setup", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, 420, 340, nullptr, nullptr, inst, nullptr);
  ShowWindow(hwnd, show);
  UpdateWindow(hwnd);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return static_cast<int>(msg.wParam);
}
