// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNoGUI/Platform.h"

#include "Core/Config/MainSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "Core/Host.h"

#include <Windows.h>
#include <climits>
#include <dwmapi.h>

#include "VideoCommon/Present.h"
#include "resource.h"

namespace
{
class PlatformWin32 final : public Platform
{
public:
  ~PlatformWin32() override;

  bool Init() override;
  void SetTitle(const std::string& string) override;
  void MainLoop() override;

  WindowSystemInfo GetWindowSystemInfo() const override;
  std::optional<std::string> ResizeClientArea(int width,int height) override;

private:
  static constexpr TCHAR WINDOW_CLASS_NAME[] = _T("DolphinNoGUI");

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

  static bool RegisterRenderWindowClass();
  bool CreateRenderWindow();
  void UpdateWindowPosition();
  void ProcessEvents();

  HWND m_hwnd{};

  int m_window_x = Config::Get(Config::MAIN_RENDER_WINDOW_XPOS);
  int m_window_y = Config::Get(Config::MAIN_RENDER_WINDOW_YPOS);
  int m_window_width = Config::Get(Config::MAIN_RENDER_WINDOW_WIDTH);
  int m_window_height = Config::Get(Config::MAIN_RENDER_WINDOW_HEIGHT);
};

PlatformWin32::~PlatformWin32()
{
  if (m_hwnd)
    DestroyWindow(m_hwnd);
}

bool PlatformWin32::RegisterRenderWindowClass()
{
  WNDCLASSEX wc = {};
  wc.cbSize = sizeof(WNDCLASSEX);
  wc.style = 0;
  wc.lpfnWndProc = WndProc;
  wc.cbClsExtra = 0;
  wc.cbWndExtra = 0;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.hIcon = LoadIcon(nullptr, IDI_ICON1);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszMenuName = nullptr;
  wc.lpszClassName = WINDOW_CLASS_NAME;
  wc.hIconSm = LoadIcon(nullptr, IDI_ICON1);

  if (!RegisterClassEx(&wc))
  {
    MessageBox(nullptr, _T("Window registration failed."), _T("Error"), MB_ICONERROR | MB_OK);
    return false;
  }

  return true;
}

bool PlatformWin32::CreateRenderWindow()
{
  // Render dimensions describe the client area. CreateWindowEx expects the
  // outer rectangle; account for the title bar and borders before creating it.
  RECT bounds{0, 0, m_window_width, m_window_height};
  if (!AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_CLIENTEDGE))
    return false;
  m_hwnd = CreateWindowEx(WS_EX_CLIENTEDGE|(m_keep_minimized?WS_EX_NOACTIVATE:0), WINDOW_CLASS_NAME, _T("Dolphin"), WS_OVERLAPPEDWINDOW|(m_start_minimized?WS_MINIMIZE:0),
                          m_window_x < 0 ? CW_USEDEFAULT : m_window_x,
                          m_window_y < 0 ? CW_USEDEFAULT : m_window_y,
                          bounds.right - bounds.left, bounds.bottom - bounds.top,
                          nullptr, nullptr, GetModuleHandle(nullptr), this);
  if (!m_hwnd)
  {
    MessageBox(nullptr, _T("CreateWindowEx failed."), _T("Error"), MB_ICONERROR | MB_OK);
    return false;
  }

  SetPropW(m_hwnd,L"ModernGekko.ClientWidth",reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(m_window_width)));
  SetPropW(m_hwnd,L"ModernGekko.ClientHeight",reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(m_window_height)));
  ShowWindow(m_hwnd, m_start_minimized?SW_SHOWMINNOACTIVE:SW_SHOW);
  if(m_start_minimized)ShowWindow(m_hwnd,SW_SHOWMINNOACTIVE);
  std::fprintf(stderr,"[window] start_minimized=%d iconic=%d client=%dx%d\n",int(m_start_minimized),int(IsIconic(m_hwnd)),m_window_width,m_window_height);
  UpdateWindow(m_hwnd);
  return true;
}

bool PlatformWin32::Init()
{
  if (!RegisterRenderWindowClass() || !CreateRenderWindow())
    return false;

  // TODO: Enter fullscreen if enabled.
  if (Config::Get(Config::MAIN_FULLSCREEN))
  {
    ProcessEvents();
  }

  if (Config::Get(Config::MAIN_DISABLE_SCREENSAVER))
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);

  UpdateWindowPosition();
  return true;
}

void PlatformWin32::SetTitle(const std::string& string)
{
  SetWindowTextW(m_hwnd, UTF8ToWString(string).c_str());
}

void PlatformWin32::MainLoop()
{
  while (IsRunning())
  {
    UpdateRunningFlag();
    Core::HostDispatchJobs(Core::System::GetInstance());
    ProcessEvents();
    if(m_keep_minimized&&!IsIconic(m_hwnd))ShowWindow(m_hwnd,SW_SHOWMINNOACTIVE);
    HostTick();
    UpdateWindowPosition();

    // TODO: Is this sleep appropriate?
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

WindowSystemInfo PlatformWin32::GetWindowSystemInfo() const
{
  WindowSystemInfo wsi;
  wsi.type = WindowSystemType::Windows;
  wsi.render_window = reinterpret_cast<void*>(m_hwnd);
  wsi.render_surface = reinterpret_cast<void*>(m_hwnd);
  return wsi;
}

std::optional<std::string> PlatformWin32::ResizeClientArea(int width,int height)
{
  // A resolution is a client-area request, independent of maximized state.
  m_window_width=width;m_window_height=height;
  RECT bounds{0,0,width,height};
  if(!AdjustWindowRectEx(&bounds,static_cast<DWORD>(GetWindowLongPtr(m_hwnd,GWL_STYLE)),FALSE,
                        static_cast<DWORD>(GetWindowLongPtr(m_hwnd,GWL_EXSTYLE))))
    return "No se pudo calcular el tamano de ventana";
  if(IsIconic(m_hwnd)) {
    WINDOWPLACEMENT place{sizeof(place)};if(!GetWindowPlacement(m_hwnd,&place))return "No se pudo leer la ventana minimizada";
    place.rcNormalPosition.right=place.rcNormalPosition.left+bounds.right-bounds.left;
    place.rcNormalPosition.bottom=place.rcNormalPosition.top+bounds.bottom-bounds.top;
    place.flags&=~WPF_RESTORETOMAXIMIZED;
    place.showCmd=SW_SHOWMINNOACTIVE;
    if(!SetWindowPlacement(m_hwnd,&place))return "No se pudo preparar el nuevo tamano de ventana";
  } else {
    if(IsZoomed(m_hwnd)) {
      WINDOWPLACEMENT place{sizeof(place)};
      if(!GetWindowPlacement(m_hwnd,&place))return "No se pudo leer la ventana maximizada";
      place.flags&=~WPF_RESTORETOMAXIMIZED;
      place.showCmd=SW_SHOWNOACTIVATE;
      if(!SetWindowPlacement(m_hwnd,&place))return "No se pudo restaurar el tamano de ventana";
    }
    if(!SetWindowPos(m_hwnd,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,
                    SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE))
      return "No se pudo redimensionar la ventana";
    RECT actual{};
    if(!GetClientRect(m_hwnd,&actual)||actual.right-actual.left!=width||actual.bottom-actual.top!=height)
      return "Windows no acepto el tamano solicitado; restaurando";
  }
  SetPropW(m_hwnd,L"ModernGekko.ClientWidth",reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(width)));
  SetPropW(m_hwnd,L"ModernGekko.ClientHeight",reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(height)));
  if(g_presenter)g_presenter->ResizeSurface();
  return {};
}

void PlatformWin32::UpdateWindowPosition()
{
  if (m_window_fullscreen)
    return;

  if(IsIconic(m_hwnd)){
    WINDOWPLACEMENT placement{sizeof(placement)};
    if(GetWindowPlacement(m_hwnd,&placement)){
      m_window_x=placement.rcNormalPosition.left;m_window_y=placement.rcNormalPosition.top;
      m_window_width=static_cast<int>(reinterpret_cast<ULONG_PTR>(GetPropW(m_hwnd,L"ModernGekko.ClientWidth")));
      m_window_height=static_cast<int>(reinterpret_cast<ULONG_PTR>(GetPropW(m_hwnd,L"ModernGekko.ClientHeight")));
    }
    return;
  }

  RECT rc = {};
  if (!GetWindowRect(m_hwnd, &rc))
    return;

  m_window_x = rc.left;
  m_window_y = rc.top;
  RECT client{};
  if(GetClientRect(m_hwnd,&client)){
    m_window_width=client.right-client.left;m_window_height=client.bottom-client.top;
  }
}

void PlatformWin32::ProcessEvents()
{
  MSG msg;
  while (PeekMessage(&msg, m_hwnd, 0, 0, PM_REMOVE))
  {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

LRESULT PlatformWin32::WndProc(const HWND hwnd, const UINT msg, const WPARAM wParam,
                               const LPARAM lParam)
{
  PlatformWin32* platform = reinterpret_cast<PlatformWin32*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
  switch (msg)
  {
  case WM_NCCREATE:
  {
    platform = static_cast<PlatformWin32*>(reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams);
    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(platform));
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  case WM_CREATE:
  {
    if (hwnd)
    {
      // Remove rounded corners from the render window on Windows 11
      constexpr DWM_WINDOW_CORNER_PREFERENCE corner_preference = DWMWCP_DONOTROUND;
      DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner_preference,
                            sizeof(corner_preference));
    }
  }
  break;

  case WM_SIZE:
  {
    if (g_presenter && wParam != SIZE_MINIMIZED)
      g_presenter->ResizeSurface();
  }
  break;

  case WM_GETMINMAXINFO:
  {
    // The normal window must allow resolutions larger than the work area,
    // including a 1920x1080 client on a 1920x1080 monitor with borders.
    DefWindowProc(hwnd,msg,wParam,lParam);
    if(!platform)break;
    RECT bounds{0,0,platform->m_window_width,platform->m_window_height};
    if(AdjustWindowRectEx(&bounds,static_cast<DWORD>(GetWindowLongPtr(hwnd,GWL_STYLE)),FALSE,
                         static_cast<DWORD>(GetWindowLongPtr(hwnd,GWL_EXSTYLE)))) {
      auto* limits=reinterpret_cast<MINMAXINFO*>(lParam);
      if(limits->ptMaxTrackSize.x<bounds.right-bounds.left)limits->ptMaxTrackSize.x=bounds.right-bounds.left;
      if(limits->ptMaxTrackSize.y<bounds.bottom-bounds.top)limits->ptMaxTrackSize.y=bounds.bottom-bounds.top;
    }
  }
  break;

  case WM_KEYDOWN:
    if (wParam == VK_ESCAPE && !Host_UIBlocksControllerState())
      platform->RequestShutdown();
    break;

  case WM_CLOSE:
    platform->RequestShutdown();
    break;

  default:
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  return 0;
}
}  // namespace

std::unique_ptr<Platform> Platform::CreateWin32Platform()
{
  return std::make_unique<PlatformWin32>();
}
