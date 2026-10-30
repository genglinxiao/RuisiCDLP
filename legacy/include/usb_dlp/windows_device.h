#pragma once

#ifdef _WIN32

#include "usb_dlp/policy.h"

#include <string>
#include <vector>

namespace usb_dlp {

struct WindowsDevice {
  DeviceInfo policy_device;
  std::wstring instance_id;
  std::wstring friendly_name;
};

class WindowsDeviceEnumerator {
 public:
  bool enumerate(
      std::vector<WindowsDevice>* devices,
      std::wstring* error,
      std::vector<std::wstring>* diagnostics = nullptr) const;
};

class WindowsDeviceEnforcer {
 public:
  bool disable(const std::wstring& instance_id, unsigned long* result_code,
               std::wstring* diagnostic = nullptr) const;
  bool enable(const std::wstring& instance_id, unsigned long* result_code,
              std::wstring* diagnostic = nullptr) const;
  bool is_disabled(
      const std::wstring& instance_id,
      bool* disabled,
      unsigned long* result_code,
      unsigned long* status_bits = nullptr,
      unsigned long* problem_code = nullptr,
      std::wstring* diagnostic = nullptr) const;
};

std::wstring common_application_data_path();
std::string wide_to_utf8(const std::wstring& value);
std::wstring utf8_to_wide(const std::string& value);

}  // namespace usb_dlp

#endif
