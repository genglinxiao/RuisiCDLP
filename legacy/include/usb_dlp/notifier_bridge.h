#pragma once

#ifdef _WIN32

#include "usb_dlp/agent_api.h"
#include "usb_dlp/windows_device.h"

#include <windows.h>

#include <map>
#include <string>

namespace usb_dlp {

// Called on the notifier pipe thread when the user submits an approval code.
// `instance_id` is the device the prompt is currently about. Returns true when
// the code was accepted; `message` carries either the validity or the reason.
typedef bool (*ApprovalHandler)(void* context, const std::wstring& instance_id,
                                const std::string& code, std::string* message);

class NotifierBridge {
 public:
  NotifierBridge(LegacyAgentApi* api, const std::wstring& notifier_path);
  ~NotifierBridge();

  bool start(std::wstring* error);
  void stop();
  void set_identity(const AgentIdentity& identity);
  void set_approval_handler(ApprovalHandler handler, void* context);
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
  // A blocked device disappears from the enumeration as soon as its driver is
  // disabled, so the bridge stops receiving updates for it. The prompt must not
  // depend on those updates, otherwise it closes before the user can read it.
  // Re-open it on an interval instead, and hold off while a request for the same
  // device is still pending, so the user is not prompted over and over.
  DWORD last_launch_ms_;
  std::map<std::wstring, DWORD> suppress_until_ms_;
  ApprovalHandler approval_handler_;
  void* approval_context_;

  NotifierBridge(const NotifierBridge&);
  NotifierBridge& operator=(const NotifierBridge&);
};

}  // namespace usb_dlp

#endif
