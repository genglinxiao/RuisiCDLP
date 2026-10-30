#pragma once

#ifdef _WIN32

#include "usb_dlp/agent_api.h"
#include "usb_dlp/windows_device.h"

#include <windows.h>

#include <string>

namespace usb_dlp {

class NotifierBridge {
 public:
  NotifierBridge(LegacyAgentApi* api, const std::wstring& notifier_path);
  ~NotifierBridge();

  bool start(std::wstring* error);
  void stop();
  void set_identity(const AgentIdentity& identity);
  void publish_denied(
      const WindowsDevice& device,
      const std::string& reason_code,
      bool enforcement_verified,
      bool newly_present);
  void publish_allowed(const std::wstring& instance_id);
  void publish_removed(const std::wstring& instance_id);

 private:
  static DWORD WINAPI thread_entry(void* context);
  void run();
  std::string handle_request(const std::string& request);
  std::string current_message();
  void launch_notifier();

  LegacyAgentApi* api_;
  std::wstring notifier_path_;
  CRITICAL_SECTION lock_;
  HANDLE stop_event_;
  HANDLE thread_;
  AgentIdentity identity_;
  bool has_identity_;
  WindowsDevice current_device_;
  std::string current_event_id_;
  std::string current_reason_code_;
  bool current_enforcement_verified_;
  bool has_current_;

  NotifierBridge(const NotifierBridge&);
  NotifierBridge& operator=(const NotifierBridge&);
};

}  // namespace usb_dlp

#endif
