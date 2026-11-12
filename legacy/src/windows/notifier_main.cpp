#include "usb_dlp/json.h"
#include "usb_dlp/windows_device.h"

#include <windows.h>
#include <lmcons.h>
#include <sddl.h>

#include <string>
#include <vector>

namespace {

const wchar_t* const kPipeName = L"\\\\.\\pipe\\UsbDlp.LegacyAgent.Notifier.v1";
const wchar_t* const kWindowClass = L"UsbDlp.LegacyNotifier.Window";
const wchar_t* const kStaticClass = L"STATIC";
const wchar_t* const kEditClass = L"EDIT";
const wchar_t* const kComboClass = L"COMBOBOX";
const wchar_t* const kButtonClass = L"BUTTON";
const UINT_PTR kRefreshTimer = 1;
const UINT_PTR kDismissTimer = 2;

// The agent stops reporting a blocked device as soon as the driver is disabled,
// because the device drops out of the USB enumeration. Closing the window on the
// first "none" reply made the prompt flash for well under a second, so the user
// could never read it or submit a request. Keep the window for a grace period
// instead; this only guards against an agent that died and left us orphaned.
const int kIdleTicksBeforeClose = 120;

enum ControlId {
  kDevice = 100,
  kInstance = 101,
  kIdentifier = 102,
  kReason = 103,
  kDuration = 104,
  kJustification = 105,
  kSubmit = 106,
  kStatus = 107,
  kDeviceCode = 108,
  kApprovalCode = 109,
  kSubmitApproval = 110,
};

// Every label the prompt shows, in the language of the interactive user.
// Retail store staff are not expected to read English, and the earlier build
// hard-coded English only.
struct Text {
  const wchar_t* window_title;
  const wchar_t* heading;
  const wchar_t* service_connected;
  const wchar_t* checking;
  const wchar_t* waiting_details;
  const wchar_t* label_device;
  const wchar_t* label_instance;
  const wchar_t* label_identifier;
  const wchar_t* identifier_unknown;
  const wchar_t* label_duration;
  const wchar_t* label_justification;
  const wchar_t* button_submit;
  const wchar_t* blocked_heading;
  const wchar_t* unverified_heading;
  const wchar_t* unverified_reason;
  const wchar_t* blocked_default;
  const wchar_t* reason_default_deny;
  const wchar_t* reason_entry_block;
  const wchar_t* reason_mandatory;
  const wchar_t* reason_invalid_policy;
  const wchar_t* submitting;
  const wchar_t* submitted;
  const wchar_t* recent_exists;
  const wchar_t* request_failed;
  const wchar_t* label_device_code;
  const wchar_t* label_approval_code;
  const wchar_t* button_submit_approval;
  const wchar_t* approval_hint;
  const wchar_t* approval_ok;
  const wchar_t* approval_active;
  const wchar_t* approval_failed;
  const wchar_t* font_family;
  const wchar_t* durations[4];
};

const Text kEnglish = {
    L"Request USB device access",
    L"Request USB device access",
    L"Connected to workstation service",
    L"Checking device restriction",
    L"Waiting for device details.",
    L"Device",
    L"Instance ID",
    L"Identifier",
    L"Not identified",
    L"Requested duration",
    L"Business justification (10-2000 characters)",
    L"Submit request",
    L"This device is blocked",
    L"USB block NOT confirmed",
    L"Do not use this disk. Contact IT Security.",
    L"Blocked by policy",
    L"Default deny: this storage device is not authorized",
    L"This device is explicitly blocked by policy",
    L"Blocked by a mandatory global policy",
    L"Policy is invalid; the strictest rule was applied",
    L"Submitting...",
    L"Request submitted.",
    L"A recent request already exists.",
    L"Request failed.",
    L"Device code",
    L"Approval code",
    L"Submit code",
    L"Read the device code to IT, or paste the approval code they send.",
    L"Approved. Unplug and re-insert the device.",
    L"Approved. The device is available now.",
    L"Code rejected: ",
    L"Segoe UI",
    {L"1 day", L"7 days", L"30 days", L"90 days"},
};

const Text kChinese = {
    L"\u8bf7\u6c42 USB \u8bbe\u5907\u8bbf\u95ee\u6743\u9650",
    L"\u8bf7\u6c42 USB \u8bbe\u5907\u8bbf\u95ee\u6743\u9650",
    L"\u5df2\u8fde\u63a5\u5de5\u4f5c\u7ad9\u670d\u52a1",
    L"\u6b63\u5728\u68c0\u67e5\u8bbe\u5907\u9650\u5236",
    L"\u6b63\u5728\u7b49\u5f85\u8bbe\u5907\u4fe1\u606f\u3002",
    L"\u8bbe\u5907",
    L"\u5b9e\u4f8b ID",
    L"\u6807\u8bc6",
    L"\u672a\u8bc6\u522b",
    L"\u7533\u8bf7\u65f6\u957f",
    L"\u4e1a\u52a1\u7406\u7531\uff0810-2000 \u5b57\u7b26\uff09",
    L"\u63d0\u4ea4\u7533\u8bf7",
    L"\u6b64\u8bbe\u5907\u5df2\u88ab\u963b\u6b62",
    L"USB \u963b\u6b62\u672a\u786e\u8ba4",
    L"\u8bf7\u52ff\u4f7f\u7528\u6b64\u78c1\u76d8\uff0c\u8bf7\u8054\u7cfb IT \u5b89\u5168\u3002",
    L"\u88ab\u7b56\u7565\u963b\u6b62",
    L"\u9ed8\u8ba4\u62d2\u7edd\uff1a\u8be5\u79fb\u52a8\u5b58\u50a8\u8bbe\u5907\u672a\u6388\u6743",
    L"\u8be5\u8bbe\u5907\u5df2\u88ab\u7b56\u7565\u660e\u786e\u7981\u6b62",
    L"\u88ab\u5168\u5c40\u5f3a\u5236\u7b56\u7565\u963b\u6b62",
    L"\u7b56\u7565\u65e0\u6548\uff0c\u5df2\u6309\u6700\u4e25\u683c\u89c4\u5219\u5904\u7406",
    L"\u6b63\u5728\u63d0\u4ea4\u2026",
    L"\u7533\u8bf7\u5df2\u63d0\u4ea4\u3002",
    L"\u5df2\u6709\u8fd1\u671f\u7533\u8bf7\uff0c\u8bf7\u52ff\u91cd\u590d\u63d0\u4ea4\u3002",
    L"\u7533\u8bf7\u5931\u8d25\u3002",
    L"\u8bbe\u5907\u7801",
    L"\u5ba1\u6279\u7801",
    L"\u63d0\u4ea4\u5ba1\u6279\u7801",
    L"\u628a\u8bbe\u5907\u7801\u62a5\u7ed9 IT\uff0c\u6216\u8f93\u5165\u4e0b\u53d1\u7684\u5ba1\u6279\u7801",
    L"\u5df2\u6279\u51c6\uff0c\u8bf7\u91cd\u65b0\u63d2\u62d4\u8bbe\u5907",
    L"\u5df2\u6279\u51c6\uff0c\u8bbe\u5907\u73b0\u5728\u53ef\u7528",
    L"\u5ba1\u6279\u7801\u65e0\u6548\uff1a",
    L"Microsoft YaHei",
    {L"1 \u5929", L"7 \u5929", L"30 \u5929", L"90 \u5929"},
};

bool use_chinese() {
  static const bool chinese = [] {
    const LANGID language = GetUserDefaultUILanguage();
    return PRIMARYLANGID(language) == LANG_CHINESE;
  }();
  return chinese;
}

const Text& strings() { return use_chinese() ? kChinese : kEnglish; }

struct State {
  HWND window = nullptr;
  HWND block_heading = nullptr;
  HWND device = nullptr;
  HWND instance = nullptr;
  HWND identifier = nullptr;
  HWND reason = nullptr;
  HWND duration = nullptr;
  HWND justification = nullptr;
  HWND submit = nullptr;
  HWND status = nullptr;
  HWND device_code = nullptr;
  HWND approval_code = nullptr;
  HWND submit_approval = nullptr;
  HFONT heading_font = nullptr;
  HFONT body_font = nullptr;
  HFONT code_font = nullptr;
  HBRUSH background = nullptr;
  std::string event_id;
  bool submitting = false;
  int idle_ticks = 0;
};

State g_state;

std::string pipe_request(const std::string& request, std::wstring* error) {
  if (!WaitNamedPipeW(kPipeName, 3000)) {
    if (error != nullptr) *error = L"Workstation service is unavailable.";
    return std::string();
  }
  HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    if (error != nullptr) *error = L"Could not connect to the workstation service.";
    return std::string();
  }
  const std::string framed = request + "\n";
  DWORD written = 0;
  if (!WriteFile(pipe, framed.data(), static_cast<DWORD>(framed.size()), &written, nullptr)) {
    CloseHandle(pipe);
    if (error != nullptr) *error = L"Could not send a request to the workstation service.";
    return std::string();
  }
  std::string response;
  char buffer[4096];
  for (;;) {
    DWORD read = 0;
    const BOOL success = ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr);
    if (!success && GetLastError() != ERROR_MORE_DATA) break;
    response.append(buffer, read);
    if (response.size() > 64U * 1024U || response.find('\n') != std::string::npos) break;
  }
  CloseHandle(pipe);
  const std::string::size_type newline = response.find('\n');
  if (newline != std::string::npos) response.resize(newline);
  if (response.empty() && error != nullptr) *error = L"The workstation service did not respond.";
  return response;
}

std::wstring user_sid() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return L"S-1-0-0";
  DWORD needed = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
  std::vector<BYTE> buffer(needed);
  std::wstring result = L"S-1-0-0";
  if (needed != 0 && GetTokenInformation(token, TokenUser, &buffer[0], needed, &needed)) {
    LPWSTR text = nullptr;
    if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(&buffer[0])->User.Sid, &text)) {
      result = text;
      LocalFree(text);
    }
  }
  CloseHandle(token);
  return result;
}

std::wstring username() {
  wchar_t value[UNLEN + 1] = {};
  DWORD size = UNLEN + 1;
  return GetUserNameW(value, &size) ? value : L"UNKNOWN";
}

void set_text(HWND control, const std::string& value) {
  SetWindowTextW(control, usb_dlp::utf8_to_wide(value).c_str());
}

std::string edit_text(HWND control) {
  const int length = GetWindowTextLengthW(control);
  std::vector<wchar_t> value(static_cast<std::size_t>(length) + 1, L'\0');
  GetWindowTextW(control, &value[0], length + 1);
  return usb_dlp::wide_to_utf8(&value[0]);
}

void update_submit() {
  const std::string justification = edit_text(g_state.justification);
  EnableWindow(g_state.submit, !g_state.submitting && !g_state.event_id.empty() &&
                                   justification.size() >= 10 && justification.size() <= 2000);
}

HWND add_control(
    const wchar_t* class_name,
    const wchar_t* text,
    DWORD style,
    int id,
    int x,
    int y,
    int width,
    int height) {
  HWND control = CreateWindowExW(
      class_name == kEditClass ? WS_EX_CLIENTEDGE : 0, class_name, text,
      WS_CHILD | WS_VISIBLE | style, x, y, width, height, g_state.window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_state.body_font), TRUE);
  return control;
}

void create_controls() {
  const Text& t = strings();
  HWND heading = add_control(kStaticClass, t.heading, SS_LEFT, 0, 42, 28, 540, 34);
  SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(g_state.heading_font), TRUE);
  add_control(kStaticClass, t.service_connected, SS_LEFT, 0, 42, 62, 540, 24);
  g_state.block_heading = add_control(kStaticClass, t.checking, SS_LEFT, 0, 42, 108, 540, 30);
  SendMessageW(g_state.block_heading, WM_SETFONT, reinterpret_cast<WPARAM>(g_state.heading_font), TRUE);
  g_state.reason = add_control(kStaticClass, t.waiting_details, SS_LEFT, kReason, 42, 140, 540, 30);

  add_control(kStaticClass, t.label_device, SS_LEFT, 0, 42, 190, 120, 26);
  g_state.device = add_control(kStaticClass, L"-", SS_LEFT | SS_ENDELLIPSIS, kDevice, 180, 190, 400, 26);
  add_control(kStaticClass, t.label_instance, SS_LEFT, 0, 42, 226, 120, 26);
  g_state.instance = add_control(kStaticClass, L"-", SS_LEFT | SS_ENDELLIPSIS, kInstance, 180, 226, 400, 26);
  add_control(kStaticClass, t.label_identifier, SS_LEFT, 0, 42, 262, 120, 26);
  g_state.identifier = add_control(kStaticClass, L"-", SS_LEFT, kIdentifier, 180, 262, 400, 26);

  // The code the user reads out to IT: short enough to dictate, and bound to
  // this exact device instance.
  add_control(kStaticClass, t.label_device_code, SS_LEFT, 0, 42, 298, 120, 26);
  g_state.device_code = add_control(kStaticClass, L"-", SS_LEFT, kDeviceCode, 180, 296, 400, 32);
  SendMessageW(g_state.device_code, WM_SETFONT, reinterpret_cast<WPARAM>(g_state.code_font), TRUE);

  add_control(kStaticClass, t.label_duration, SS_LEFT, 0, 42, 334, 540, 26);
  g_state.duration = add_control(kComboClass, L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                                 kDuration, 42, 356, 540, 180);
  for (int index = 0; index < 4; ++index) {
    SendMessageW(g_state.duration, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.durations[index]));
  }
  SendMessageW(g_state.duration, CB_SETCURSEL, 2, 0);

  add_control(kStaticClass, t.label_justification, SS_LEFT, 0, 42, 394, 540, 26);
  g_state.justification = add_control(
      kEditClass, L"", ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
      kJustification, 42, 414, 540, 56);
  SendMessageW(g_state.justification, EM_SETLIMITTEXT, 2000, 0);

  add_control(kStaticClass, t.label_approval_code, SS_LEFT, 0, 42, 480, 540, 26);
  g_state.approval_code = add_control(
      kEditClass, L"", ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
      kApprovalCode, 42, 500, 540, 56);
  add_control(kStaticClass, t.approval_hint, SS_LEFT, 0, 42, 562, 540, 22);

  g_state.submit_approval = add_control(kButtonClass, t.button_submit_approval, BS_PUSHBUTTON,
                                        kSubmitApproval, 250, 590, 160, 38);
  g_state.submit = add_control(kButtonClass, t.button_submit, BS_DEFPUSHBUTTON,
                               kSubmit, 422, 590, 160, 38);
  EnableWindow(g_state.submit, FALSE);
  // Both channels report into one status line inside the client area. A second
  // label further down fell outside the window and made every result invisible.
  g_state.status = add_control(kStaticClass, L"", SS_LEFT, kStatus, 42, 634, 540, 34);
}

// The reason codes are protocol values, not user-facing text, so they are
// translated here rather than shown verbatim.
const wchar_t* reason_text(const std::string& code) {
  const Text& t = strings();
  if (code == "default_deny") return t.reason_default_deny;
  if (code == "entry_block") return t.reason_entry_block;
  if (code == "mandatory_global_block") return t.reason_mandatory;
  if (code == "invalid_policy") return t.reason_invalid_policy;
  return t.blocked_default;
}

bool apply_blocked_message(const usb_dlp::JsonValue& message) {
  const usb_dlp::JsonValue* event = message.find("eventId");
  const usb_dlp::JsonValue* device = message.find("device");
  if (event == nullptr || device == nullptr || event->type() != usb_dlp::JsonValue::Type::string ||
      device->type() != usb_dlp::JsonValue::Type::object) return false;
  const usb_dlp::JsonValue* instance = device->find("instanceId");
  const usb_dlp::JsonValue* friendly = device->find("friendlyName");
  const usb_dlp::JsonValue* vid = device->find("vid");
  const usb_dlp::JsonValue* pid = device->find("pid");
  if (instance == nullptr || friendly == nullptr || vid == nullptr || pid == nullptr) return false;
  const Text& t = strings();
  const bool changed = g_state.event_id != event->string_value();
  g_state.event_id = event->string_value();
  set_text(g_state.device, friendly->string_value());
  set_text(g_state.instance, instance->string_value());
  const std::string vid_text = vid->string_value();
  const std::string pid_text = pid->string_value();
  if (vid_text.empty() && pid_text.empty()) {
    // Mirror nodes have no USB identifiers; showing "VID / PID" reads as a bug.
    SetWindowTextW(g_state.identifier, t.identifier_unknown);
  } else {
    set_text(g_state.identifier, "VID " + vid_text + " / PID " + pid_text);
  }
  const usb_dlp::JsonValue* device_code = message.find("deviceCode");
  if (device_code != nullptr && device_code->type() == usb_dlp::JsonValue::Type::string) {
    set_text(g_state.device_code, device_code->string_value());
  }
  const usb_dlp::JsonValue* reason = message.find("reasonCode");
  const usb_dlp::JsonValue* verified = message.find("enforcementVerified");
  const bool is_verified = verified != nullptr && verified->type() == usb_dlp::JsonValue::Type::boolean &&
                           verified->boolean_value();
  SetWindowTextW(g_state.block_heading, is_verified ? t.blocked_heading : t.unverified_heading);
  if (is_verified) {
    SetWindowTextW(g_state.reason, reason_text(reason != nullptr &&
                                                       reason->type() == usb_dlp::JsonValue::Type::string
                                                   ? reason->string_value()
                                                   : std::string()));
  } else {
    SetWindowTextW(g_state.reason, t.unverified_reason);
  }
  if (changed) {
    g_state.submitting = false;
    SetWindowTextW(g_state.justification, L"");
    SetWindowTextW(g_state.status, L"");
    SetWindowTextW(g_state.approval_code, L"");
    SetWindowTextW(g_state.status, L"");
    EnableWindow(g_state.duration, TRUE);
    EnableWindow(g_state.justification, TRUE);
    EnableWindow(g_state.approval_code, TRUE);
    EnableWindow(g_state.submit_approval, TRUE);
    // Raise the window only when the blocked device actually changes. The poll
    // runs every second and the prompt now stays open, so restoring focus on
    // every tick would steal keyboard focus from the user once per second.
    ShowWindow(g_state.window, SW_RESTORE);
    SetForegroundWindow(g_state.window);
  }
  update_submit();
  return true;
}

void submit_approval_code() {
  const Text& t = strings();
  const std::string code = edit_text(g_state.approval_code);
  if (code.empty()) return;
  SetWindowTextW(g_state.status, L"");
  usb_dlp::JsonValue request = usb_dlp::JsonValue::object();
  request.object_items()["type"] = usb_dlp::JsonValue(std::string("submit_code"));
  request.object_items()["code"] = usb_dlp::JsonValue(code);

  std::wstring error;
  const std::string response = pipe_request(usb_dlp::write_json(request), &error);
  usb_dlp::JsonValue parsed;
  std::string parse_error;
  if (response.empty() || !usb_dlp::parse_json(response, &parsed, &parse_error)) {
    SetWindowTextW(g_state.status, error.empty() ? t.approval_failed : error.c_str());
    return;
  }
  const usb_dlp::JsonValue* accepted = parsed.find("accepted");
  const usb_dlp::JsonValue* message = parsed.find("message");
  const bool ok = accepted != nullptr && accepted->type() == usb_dlp::JsonValue::Type::boolean &&
                  accepted->boolean_value();
  if (ok) {
    const std::string kind = message != nullptr &&
                                     message->type() == usb_dlp::JsonValue::Type::string
                                 ? message->string_value()
                                 : std::string();
    // "ok_replug_*" means the node could not be re-enabled in place.
    SetWindowTextW(g_state.status,
                   kind.compare(0, 9, "ok_replug") == 0 ? t.approval_ok : t.approval_active);
    EnableWindow(g_state.approval_code, FALSE);
    EnableWindow(g_state.submit_approval, FALSE);
    return;
  }
  const std::wstring reason =
      message != nullptr && message->type() == usb_dlp::JsonValue::Type::string
          ? usb_dlp::utf8_to_wide(message->string_value())
          : std::wstring();
  SetWindowTextW(g_state.status, (std::wstring(t.approval_failed) + reason).c_str());
}

void refresh_device() {
  std::wstring error;
  const std::string response = pipe_request("{\"type\":\"get\"}", &error);
  if (response.empty()) {
    SetWindowTextW(g_state.status, error.c_str());
    return;
  }
  usb_dlp::JsonValue parsed;
  std::string parse_error;
  if (!usb_dlp::parse_json(response, &parsed, &parse_error)) return;
  const usb_dlp::JsonValue* type = parsed.find("type");
  if (type == nullptr || type->type() != usb_dlp::JsonValue::Type::string) return;
  if (type->string_value() == "blocked") {
    g_state.idle_ticks = 0;
    apply_blocked_message(parsed);
    return;
  }
  // Disabling the device removes it from the enumeration, so the agent reports
  // "none" almost immediately after a successful block. Closing here is what
  // made the prompt flash. Wait out the grace period instead.
  if (++g_state.idle_ticks >= kIdleTicksBeforeClose) DestroyWindow(g_state.window);
}

void submit_request() {
  const std::string justification = edit_text(g_state.justification);
  if (justification.size() < 10 || justification.size() > 2000 || g_state.event_id.empty()) return;
  const Text& t = strings();
  const LRESULT selection = SendMessageW(g_state.duration, CB_GETCURSEL, 0, 0);
  const int durations[] = {1, 7, 30, 90};
  const int days = selection >= 0 && selection < 4 ? durations[selection] : 30;
  usb_dlp::JsonValue request = usb_dlp::JsonValue::object();
  request.object_items()["type"] = usb_dlp::JsonValue(std::string("request_access"));
  request.object_items()["eventId"] = usb_dlp::JsonValue(g_state.event_id);
  request.object_items()["requestedDurationDays"] =
      usb_dlp::JsonValue(static_cast<std::int64_t>(days));
  request.object_items()["justification"] = usb_dlp::JsonValue(justification);
  request.object_items()["userSid"] = usb_dlp::JsonValue(usb_dlp::wide_to_utf8(user_sid()));
  request.object_items()["username"] = usb_dlp::JsonValue(usb_dlp::wide_to_utf8(username()));
  g_state.submitting = true;
  update_submit();
  SetWindowTextW(g_state.status, t.submitting);
  std::wstring error;
  const std::string response = pipe_request(usb_dlp::write_json(request), &error);
  usb_dlp::JsonValue parsed;
  std::string parse_error;
  if (response.empty() || !usb_dlp::parse_json(response, &parsed, &parse_error)) {
    g_state.submitting = false;
    SetWindowTextW(g_state.status, error.empty() ? t.request_failed : error.c_str());
    update_submit();
    return;
  }
  const usb_dlp::JsonValue* status = parsed.find("status");
  if (status != nullptr && status->type() == usb_dlp::JsonValue::Type::string &&
      status->string_value() != "error") {
    const bool recent = status->string_value() == "recent";
    SetWindowTextW(g_state.status, recent ? t.recent_exists : t.submitted);
    EnableWindow(g_state.duration, FALSE);
    EnableWindow(g_state.justification, FALSE);
    SetTimer(g_state.window, kDismissTimer, recent ? 3500 : 1500, nullptr);
  } else {
    const usb_dlp::JsonValue* message = parsed.find("error");
    set_text(g_state.status, message != nullptr && message->type() == usb_dlp::JsonValue::Type::string
                                 ? message->string_value()
                                 : std::string("Request failed."));
    g_state.submitting = false;
    update_submit();
  }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      g_state.window = window;
      create_controls();
      SetTimer(window, kRefreshTimer, 1000, nullptr);
      refresh_device();
      return 0;
    case WM_COMMAND:
      if (LOWORD(wparam) == kJustification && HIWORD(wparam) == EN_CHANGE) update_submit();
      if (LOWORD(wparam) == kSubmit && HIWORD(wparam) == BN_CLICKED) submit_request();
      if (LOWORD(wparam) == kSubmitApproval && HIWORD(wparam) == BN_CLICKED) submit_approval_code();
      return 0;
    case WM_TIMER:
      if (wparam == kRefreshTimer && !g_state.submitting) refresh_device();
      if (wparam == kDismissTimer) DestroyWindow(window);
      return 0;
    case WM_CTLCOLORSTATIC:
      SetBkColor(reinterpret_cast<HDC>(wparam), RGB(245, 243, 236));
      return reinterpret_cast<LRESULT>(g_state.background);
    case WM_DESTROY:
      KillTimer(window, kRefreshTimer);
      KillTimer(window, kDismissTimer);
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int show) {
  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\UsbDlp.LegacyNotifier.v1");
  if (mutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
    if (mutex != nullptr) CloseHandle(mutex);
    return 0;
  }
  const Text& t = strings();
  // Chinese glyphs need a font with CJK coverage; Segoe UI has none.
  const bool chinese = use_chinese();
  const int heading_height = chinese ? -26 : -24;
  const int body_height = chinese ? -19 : -18;
  g_state.background = CreateSolidBrush(RGB(245, 243, 236));
  g_state.heading_font = CreateFontW(heading_height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH, t.font_family);
  g_state.body_font = CreateFontW(body_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, t.font_family);
  g_state.code_font = CreateFontW(chinese ? -21 : -20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, t.font_family);
  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.hInstance = instance;
  window_class.lpfnWndProc = window_proc;
  window_class.lpszClassName = kWindowClass;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  window_class.hbrBackground = g_state.background;
  RegisterClassExW(&window_class);
  HWND window = CreateWindowExW(
      WS_EX_APPWINDOW, kWindowClass, t.window_title,
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
      CW_USEDEFAULT, CW_USEDEFAULT, 640, 700, nullptr, nullptr, instance, nullptr);
  // The status line sits at y=634; the client area must reach past it or every
  // result is drawn outside the window and the user sees nothing happen.
  if (window != nullptr) {
    RECT desired = {0, 0, 640, 680};
    AdjustWindowRectEx(&desired, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                       FALSE, WS_EX_APPWINDOW);
    SetWindowPos(window, nullptr, 0, 0, desired.right - desired.left,
                 desired.bottom - desired.top, SWP_NOMOVE | SWP_NOZORDER);
  }
  if (window != nullptr) {
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  DeleteObject(g_state.heading_font);
  DeleteObject(g_state.body_font);
  DeleteObject(g_state.code_font);
  DeleteObject(g_state.background);
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  return 0;
}
