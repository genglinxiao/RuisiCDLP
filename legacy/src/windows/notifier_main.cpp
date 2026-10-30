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

enum ControlId {
  kDevice = 100,
  kInstance = 101,
  kIdentifier = 102,
  kReason = 103,
  kDuration = 104,
  kJustification = 105,
  kSubmit = 106,
  kStatus = 107,
};

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
  HFONT heading_font = nullptr;
  HFONT body_font = nullptr;
  HBRUSH background = nullptr;
  std::string event_id;
  bool submitting = false;
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
  HWND heading = add_control(kStaticClass, L"Request USB device access", SS_LEFT, 0, 42, 28, 470, 34);
  SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(g_state.heading_font), TRUE);
  add_control(kStaticClass, L"Connected to workstation service", SS_LEFT, 0, 42, 62, 470, 24);
  g_state.block_heading = add_control(kStaticClass, L"Checking device restriction", SS_LEFT, 0, 42, 108, 470, 30);
  SendMessageW(g_state.block_heading, WM_SETFONT, reinterpret_cast<WPARAM>(g_state.heading_font), TRUE);
  g_state.reason = add_control(kStaticClass, L"Waiting for device details.", SS_LEFT, kReason, 42, 140, 470, 30);

  add_control(kStaticClass, L"Device", SS_LEFT, 0, 42, 190, 120, 26);
  g_state.device = add_control(kStaticClass, L"-", SS_LEFT | SS_ENDELLIPSIS, kDevice, 180, 190, 330, 26);
  add_control(kStaticClass, L"Instance ID", SS_LEFT, 0, 42, 226, 120, 26);
  g_state.instance = add_control(kStaticClass, L"-", SS_LEFT | SS_ENDELLIPSIS, kInstance, 180, 226, 330, 26);
  add_control(kStaticClass, L"Identifier", SS_LEFT, 0, 42, 262, 120, 26);
  g_state.identifier = add_control(kStaticClass, L"-", SS_LEFT, kIdentifier, 180, 262, 330, 26);

  add_control(kStaticClass, L"Requested duration", SS_LEFT, 0, 42, 312, 470, 26);
  g_state.duration = add_control(kComboClass, L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                                 kDuration, 42, 340, 470, 180);
  SendMessageW(g_state.duration, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"1 day"));
  SendMessageW(g_state.duration, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"7 days"));
  SendMessageW(g_state.duration, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"30 days"));
  SendMessageW(g_state.duration, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"90 days"));
  SendMessageW(g_state.duration, CB_SETCURSEL, 2, 0);

  add_control(kStaticClass, L"Business justification (10-2000 characters)", SS_LEFT, 0, 42, 388, 470, 26);
  g_state.justification = add_control(
      kEditClass, L"", ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
      kJustification, 42, 418, 470, 96);
  SendMessageW(g_state.justification, EM_SETLIMITTEXT, 2000, 0);
  g_state.status = add_control(kStaticClass, L"", SS_LEFT, kStatus, 42, 530, 300, 34);
  g_state.submit = add_control(kButtonClass, L"Submit request", BS_DEFPUSHBUTTON,
                               kSubmit, 352, 526, 160, 38);
  EnableWindow(g_state.submit, FALSE);
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
  const bool changed = g_state.event_id != event->string_value();
  g_state.event_id = event->string_value();
  set_text(g_state.device, friendly->string_value());
  set_text(g_state.instance, instance->string_value());
  set_text(g_state.identifier, "VID " + vid->string_value() + " / PID " + pid->string_value());
  const usb_dlp::JsonValue* reason = message.find("reasonCode");
  const usb_dlp::JsonValue* verified = message.find("enforcementVerified");
  const bool is_verified = verified != nullptr && verified->type() == usb_dlp::JsonValue::Type::boolean &&
                           verified->boolean_value();
  SetWindowTextW(g_state.block_heading, is_verified ? L"This device is blocked" :
                                               L"USB block NOT confirmed");
  if (is_verified) {
    set_text(g_state.reason, reason != nullptr && reason->type() == usb_dlp::JsonValue::Type::string
                                 ? reason->string_value()
                                 : std::string("Blocked by policy"));
  } else {
    SetWindowTextW(g_state.reason, L"Do not use this disk. Contact IT Security.");
  }
  if (changed) {
    g_state.submitting = false;
    SetWindowTextW(g_state.justification, L"");
    SetWindowTextW(g_state.status, L"");
    EnableWindow(g_state.duration, TRUE);
    EnableWindow(g_state.justification, TRUE);
  }
  update_submit();
  ShowWindow(g_state.window, SW_RESTORE);
  SetForegroundWindow(g_state.window);
  return true;
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
    apply_blocked_message(parsed);
  } else {
    DestroyWindow(g_state.window);
  }
}

void submit_request() {
  const std::string justification = edit_text(g_state.justification);
  if (justification.size() < 10 || justification.size() > 2000 || g_state.event_id.empty()) return;
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
  SetWindowTextW(g_state.status, L"Submitting...");
  std::wstring error;
  const std::string response = pipe_request(usb_dlp::write_json(request), &error);
  usb_dlp::JsonValue parsed;
  std::string parse_error;
  if (response.empty() || !usb_dlp::parse_json(response, &parsed, &parse_error)) {
    g_state.submitting = false;
    SetWindowTextW(g_state.status, error.empty() ? L"Request failed." : error.c_str());
    update_submit();
    return;
  }
  const usb_dlp::JsonValue* status = parsed.find("status");
  if (status != nullptr && status->type() == usb_dlp::JsonValue::Type::string &&
      status->string_value() != "error") {
    SetWindowTextW(g_state.status,
                   status->string_value() == "recent" ? L"A recent request already exists."
                                                       : L"Request submitted.");
    EnableWindow(g_state.duration, FALSE);
    EnableWindow(g_state.justification, FALSE);
    SetTimer(g_state.window, kDismissTimer, status->string_value() == "recent" ? 3500 : 1500, nullptr);
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
  g_state.background = CreateSolidBrush(RGB(245, 243, 236));
  g_state.heading_font = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH, L"Segoe UI");
  g_state.body_font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, L"Segoe UI");
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
      WS_EX_APPWINDOW, kWindowClass, L"Request USB device access",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
      CW_USEDEFAULT, CW_USEDEFAULT, 570, 630, nullptr, nullptr, instance, nullptr);
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
  DeleteObject(g_state.background);
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  return 0;
}
