// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <functional>
#include <optional>

#include "Common/Flag.h"
#include "Common/WindowSystemInfo.h"

class Platform
{
public:
  virtual ~Platform();

  bool IsRunning() const { return m_running.IsSet(); }
  bool IsWindowFocused() const { return m_window_focus; }
  bool IsWindowFullscreen() const { return m_window_fullscreen; }

  virtual bool Init();
  virtual void SetTitle(const std::string& title);
  virtual void MainLoop() = 0;
  virtual void SaveWindowGeometry() {}
  virtual std::optional<std::string> ResizeClientArea(int, int) { return "Window resizing is unavailable on this platform"; }
  void SetStartMinimized(bool value) { m_start_minimized=value; }
  void SetKeepMinimized(bool value) { m_keep_minimized=value; }
  void SetHostTickCallback(std::function<void()> callback) { m_host_tick=std::move(callback); }

  virtual WindowSystemInfo GetWindowSystemInfo() const = 0;

  // Requests a graceful shutdown, from SIGINT/SIGTERM.
  void RequestShutdown();

  // Requests a slot-1 savestate save/load, from SIGUSR1/SIGUSR2 (nogui has no
  // hotkey scheduler, so signals are the external savestate trigger).
  void RequestSaveState();
  void RequestLoadState();

  // Request an immediate shutdown.
  void Stop();

  static std::unique_ptr<Platform> CreateHeadlessPlatform();
#ifdef HAVE_WAYLAND
  static std::unique_ptr<Platform> CreateWaylandPlatform();
#endif
#ifdef HAVE_X11
  static std::unique_ptr<Platform> CreateX11Platform();
#endif

#ifdef __linux__
  static std::unique_ptr<Platform> CreateFBDevPlatform();
#endif

#ifdef _WIN32
  static std::unique_ptr<Platform> CreateWin32Platform();
#endif

#ifdef __APPLE__
  static std::unique_ptr<Platform> CreateMacOSPlatform();
#endif

protected:
  void HostTick() { if(m_host_tick)m_host_tick(); }
  bool m_start_minimized=false;
  bool m_keep_minimized=false;
  std::function<void()> m_host_tick;
  void UpdateRunningFlag();

  Common::Flag m_running{true};
  Common::Flag m_shutdown_requested{false};
  Common::Flag m_tried_graceful_shutdown{false};
  Common::Flag m_save_state_requested{false};
  Common::Flag m_load_state_requested{false};

  bool m_window_focus = true;  // Should be made atomic if actually implemented
  bool m_window_fullscreen = false;
};
