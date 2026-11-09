// Copyright 2026 The simple-engine-2 Authors.

#include "se_platform/display_output.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// windows.h first.
#include <dxgi.h>
#endif

namespace se {

#if defined(_WIN32)
namespace {

std::string Narrow(const wchar_t* text) {
  if (text == nullptr || *text == L'\0') return {};
  const int size =
      WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return {};
  std::string narrow(static_cast<std::size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow.data(), size, nullptr,
                      nullptr);
  return narrow;
}

// QueryDisplayConfig's active path whose source is |device|.
void ReadDisplayConfig(const wchar_t* device, MonitorMode& mode) {
  UINT32 path_count = 0;
  UINT32 mode_count = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count,
                                  &mode_count) != ERROR_SUCCESS) {
    return;
  }
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
                         &mode_count, modes.data(), nullptr) != ERROR_SUCCESS) {
    return;
  }
  for (UINT32 i = 0; i < path_count; ++i) {
    const DISPLAYCONFIG_PATH_INFO& path = paths[i];
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = path.sourceInfo.adapterId;
    source.header.id = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
    if (lstrcmpiW(source.viewGdiDeviceName, device) != 0) continue;
    DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = sizeof(target);
    target.header.adapterId = path.targetInfo.adapterId;
    target.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) {
      mode.friendly_name = Narrow(target.monitorFriendlyDeviceName);
    }
    const DISPLAYCONFIG_RATIONAL rate = path.targetInfo.refreshRate;
    if (rate.Numerator > 0 && rate.Denominator > 0) {
      mode.known = true;
      mode.refresh_numerator = rate.Numerator;
      mode.refresh_denominator = rate.Denominator;
      mode.hz = static_cast<double>(rate.Numerator) /
                static_cast<double>(rate.Denominator);
    }
    return;
  }
}

// What SDL 3.4.20's WIN_GetRefreshRate computes for the current mode: the
// GDI rate (59, 119 and 29 as NTSC fractions), then the refresh of DXGI's
// closest matching mode. Also counts DXGI's modes at the current size.
void ReadDxgiModes(HMONITOR monitor, MonitorMode& mode) {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                reinterpret_cast<void**>(&factory)))) {
    return;
  }
  IDXGIOutput* found = nullptr;
  IDXGIAdapter1* adapter = nullptr;
  for (UINT a = 0; found == nullptr &&
                   factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND;
       ++a) {
    IDXGIOutput* output = nullptr;
    for (UINT o = 0; found == nullptr &&
                     adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND;
         ++o) {
      DXGI_OUTPUT_DESC desc{};
      if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
        found = output;
      } else {
        output->Release();
      }
    }
    adapter->Release();
  }
  factory->Release();
  if (found == nullptr) return;

  UINT numerator = mode.gdi_hz;
  UINT denominator = 1;
  if (mode.gdi_hz == 119 || mode.gdi_hz == 59 || mode.gdi_hz == 29) {
    numerator = (mode.gdi_hz + 1) * 1000;
    denominator = 1001;
  }
  DXGI_MODE_DESC wanted{};
  wanted.Width = static_cast<UINT>(mode.width);
  wanted.Height = static_cast<UINT>(mode.height);
  wanted.RefreshRate.Numerator = numerator;
  wanted.RefreshRate.Denominator = denominator;
  wanted.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  DXGI_MODE_DESC closest{};
  if (SUCCEEDED(found->FindClosestMatchingMode(&wanted, &closest, nullptr)) &&
      closest.RefreshRate.Denominator > 0) {
    mode.sdl_style_hz = static_cast<double>(closest.RefreshRate.Numerator) /
                        static_cast<double>(closest.RefreshRate.Denominator);
  }
  UINT count = 0;
  if (SUCCEEDED(found->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &count,
                                          nullptr)) &&
      count > 0) {
    std::vector<DXGI_MODE_DESC> list(count);
    if (SUCCEEDED(found->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0,
                                            &count, list.data()))) {
      for (UINT i = 0; i < count; ++i) {
        const DXGI_MODE_DESC& entry = list[i];
        if (entry.Width != wanted.Width || entry.Height != wanted.Height ||
            entry.RefreshRate.Denominator == 0) {
          continue;
        }
        ++mode.dxgi_modes_at_size;
        mode.dxgi_max_hz_at_size =
            std::max(mode.dxgi_max_hz_at_size,
                     static_cast<double>(entry.RefreshRate.Numerator) /
                         static_cast<double>(entry.RefreshRate.Denominator));
      }
    }
  }
  found->Release();
}

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC /*dc*/, LPRECT /*rect*/,
                             LPARAM context) {
  // EnumDisplayMonitors hands the context back as an integer.
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  auto* monitors = reinterpret_cast<std::vector<MonitorInfo>*>(context);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (GetMonitorInfoW(monitor, &info) == 0) return TRUE;
  MonitorInfo entry;
  entry.handle = static_cast<MonitorHandle>(monitor);
  entry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
  entry.x = info.rcMonitor.left;
  entry.y = info.rcMonitor.top;
  entry.width = info.rcMonitor.right - info.rcMonitor.left;
  entry.height = info.rcMonitor.bottom - info.rcMonitor.top;
  monitors->push_back(std::move(entry));
  return TRUE;
}

}  // namespace

MonitorHandle MonitorOfWindow(void* native_window) noexcept {
  if (native_window == nullptr) return nullptr;
  return static_cast<MonitorHandle>(MonitorFromWindow(
      static_cast<HWND>(native_window), MONITOR_DEFAULTTONEAREST));
}

MonitorMode QueryMonitorMode(MonitorHandle monitor) {
  MonitorMode mode;
  if (monitor == nullptr) return mode;
  auto* const handle = static_cast<HMONITOR>(monitor);
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (GetMonitorInfoW(handle, &info) == 0) return mode;
  mode.device_name = Narrow(info.szDevice);
  DEVMODEW devmode{};
  devmode.dmSize = sizeof(devmode);
  if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &devmode) !=
      0) {
    mode.width = static_cast<int>(devmode.dmPelsWidth);
    mode.height = static_cast<int>(devmode.dmPelsHeight);
    mode.gdi_hz = devmode.dmDisplayFrequency;
  }
  ReadDisplayConfig(info.szDevice, mode);
  ReadDxgiModes(handle, mode);
  return mode;
}

std::vector<MonitorInfo> ListMonitors() {
  std::vector<MonitorInfo> monitors;
  EnumDisplayMonitors(nullptr, nullptr, &CollectMonitor,
                      reinterpret_cast<LPARAM>(&monitors));
  std::stable_partition(monitors.begin(), monitors.end(),
                        [](const MonitorInfo& m) { return m.primary; });
  for (MonitorInfo& monitor : monitors) {
    monitor.mode = QueryMonitorMode(monitor.handle);
  }
  return monitors;
}

#else

MonitorHandle MonitorOfWindow([[maybe_unused]] void* native_window) noexcept {
  return nullptr;
}

MonitorMode QueryMonitorMode([[maybe_unused]] MonitorHandle monitor) {
  return {};
}

std::vector<MonitorInfo> ListMonitors() { return {}; }

#endif

std::string DescribeMonitorMode(const MonitorMode& mode) {
  if (!mode.known) return "unknown (no active display path)";
  return std::format(
      "{} ({}) {}x{} at {:.3f} Hz ({}/{}); GDI {} Hz; SDL-style {:.3f} Hz "
      "from {} DXGI modes at this size (highest {:.3f} Hz)",
      mode.device_name, mode.friendly_name, mode.width, mode.height, mode.hz,
      mode.refresh_numerator, mode.refresh_denominator, mode.gdi_hz,
      mode.sdl_style_hz, mode.dxgi_modes_at_size, mode.dxgi_max_hz_at_size);
}

}  // namespace se
