#include "usb_dlp/notifier_bridge.h"

#include "usb_dlp/json.h"

#include <sddl.h>
#include <objbase.h>
#include <tlhelp32.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cctype>
#include <vector>

namespace usb_dlp {
namespace {

const wchar_t* const kPipeName = L"\\\\.\\pipe\\UsbDlp.LegacyAgent.Notifier.v1";

class ScopedHandle {
 public:
  explicit ScopedHandle(HANDLE value = nullptr) : value_(value) {}
  ~ScopedHandle() {
    if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
  }
  HANDLE get() const { return value_; }
  HANDLE release() {
    HANDLE value = value_;
    value_ = nullptr;
    return value;
  }

 private:
  HANDLE value_;
  ScopedHandle(const ScopedHandle&);
  ScopedHandle& operator=(const ScopedHandle&);
};

std::string new_guid_text() {
  GUID value = {};
  if (CoCreateGuid(&value) != S_OK) return std::string();
  wchar_t text[40] = {};
  StringFromGUID2(value, text, 40);
  std::string result = wide_to_utf8(text);
  if (result.size() >= 2) result = result.substr(1, result.size() - 2);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return result;
}

JsonValue device_message(const WindowsDevice& device) {
  JsonValue value = JsonValue::object();
  value.object_items()["instanceId"] = JsonValue(device.policy_device.instance_id);
  value.object_items()["friendlyName"] = JsonValue(wide_to_utf8(device.friendly_name));
  value.object_items()["vid"] = JsonValue(device.policy_device.vid);
  value.object_items()["pid"] = JsonValue(device.policy_device.pid);
  return value;
}

std::string error_response(const std::string& event_id, const std::wstring& message) {
  JsonValue response = JsonValue::object();
  response.object_items()["type"] = JsonValue(std::string("request_result"));
  response.object_items()["eventId"] = JsonValue(event_id);
  response.object_items()["status"] = JsonValue(std::string("error"));
  response.object_items()["error"] = JsonValue(wide_to_utf8(message));
  return write_json(response);
}

bool pipe_security(SECURITY_ATTRIBUTES* attributes, PSECURITY_DESCRIPTOR* descriptor) {
  *descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)", SDDL_REVISION_1,
          descriptor, nullptr)) return false;
  attributes->nLength = sizeof(*attributes);
  attributes->lpSecurityDescriptor = *descriptor;
  attributes->bInheritHandle = FALSE;
  return true;
}

bool read_pipe_message(HANDLE pipe, std::string* message) {
  message->clear();
  char buffer[4096];
  for (;;) {
    DWORD read = 0;
    if (!ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr)) {
      const DWORD error = GetLastError();
      if (error != ERROR_MORE_DATA) return false;
    }
    message->append(buffer, read);
    if (message->size() > 64U * 1024U) return false;
    if (!message->empty() && message->find('\n') != std::string::npos) break;
  }
  const std::string::size_type newline = message->find('\n');
  if (newline != std::string::npos) message->resize(newline);
  return true;
}

void write_pipe_message(HANDLE pipe, const std::string& message) {
  const std::string framed = message + "\n";
  DWORD written = 0;
  WriteFile(pipe, framed.data(), static_cast<DWORD>(framed.size()), &written, nullptr);
  FlushFileBuffers(pipe);
}

bool query_interactive_user_token(DWORD session_id, HANDLE* token) {
  *token = nullptr;
  HMODULE wts = LoadLibraryW(L"wtsapi32.dll");
  if (wts != nullptr) {
    typedef BOOL(WINAPI* QueryUserToken)(ULONG, PHANDLE);
    QueryUserToken query = reinterpret_cast<QueryUserToken>(GetProcAddress(wts, "WTSQueryUserToken"));
    const BOOL queried = query != nullptr && query(session_id, token);
    FreeLibrary(wts);
    if (queried) return true;
  }

  // WTSQueryUserToken is unavailable on XP. LocalSystem can obtain the
  // interactive token from the shell process in the active console session.
  ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  if (snapshot.get() == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W entry = {};
  entry.dwSize = sizeof(entry);
  if (!Process32FirstW(snapshot.get(), &entry)) return false;
  do {
    if (_wcsicmp(entry.szExeFile, L"explorer.exe") != 0) continue;
    DWORD process_session = 0;
    if (!ProcessIdToSessionId(entry.th32ProcessID, &process_session) || process_session != session_id) {
      continue;
    }
    ScopedHandle process(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, entry.th32ProcessID));
    if (process.get() == nullptr) continue;
    if (OpenProcessToken(process.get(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY, token)) {
      return true;
    }
  } while (Process32NextW(snapshot.get(), &entry));
  return false;
}

}  // namespace

NotifierBridge::NotifierBridge(LegacyAgentApi* api, const std::wstring& notifier_path)
    : api_(api), notifier_path_(notifier_path), stop_event_(nullptr), thread_(nullptr),
      has_identity_(false), current_enforcement_verified_(false), has_current_(false) {
  InitializeCriticalSection(&lock_);
}

NotifierBridge::~NotifierBridge() {
  stop();
  DeleteCriticalSection(&lock_);
}

bool NotifierBridge::start(std::wstring* error) {
  if (thread_ != nullptr) return true;
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (stop_event_ == nullptr) {
    if (error != nullptr) *error = L"Could not create notifier stop event.";
    return false;
  }
  thread_ = CreateThread(nullptr, 0, thread_entry, this, 0, nullptr);
  if (thread_ == nullptr) {
    CloseHandle(stop_event_);
    stop_event_ = nullptr;
    if (error != nullptr) *error = L"Could not start notifier pipe thread.";
    return false;
  }
  return true;
}

void NotifierBridge::stop() {
  if (thread_ == nullptr) return;
  SetEvent(stop_event_);
  ScopedHandle wake(CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
  WaitForSingleObject(thread_, 5000);
  CloseHandle(thread_);
  CloseHandle(stop_event_);
  thread_ = nullptr;
  stop_event_ = nullptr;
}

void NotifierBridge::set_identity(const AgentIdentity& identity) {
  EnterCriticalSection(&lock_);
  identity_ = identity;
  has_identity_ = !identity.workstation_id.empty() && !identity.api_credential.empty();
  LeaveCriticalSection(&lock_);
}

void NotifierBridge::publish_denied(
    const WindowsDevice& device,
    const std::string& reason_code,
    bool enforcement_verified,
    bool newly_present) {
  EnterCriticalSection(&lock_);
  if (newly_present) {
    current_device_ = device;
    current_event_id_ = new_guid_text();
    current_reason_code_ = reason_code;
    has_current_ = true;
  }
  if (has_current_ && _wcsicmp(current_device_.instance_id.c_str(), device.instance_id.c_str()) == 0) {
    current_enforcement_verified_ = enforcement_verified;
  }
  LeaveCriticalSection(&lock_);
  if (newly_present) launch_notifier();
}

void NotifierBridge::publish_allowed(const std::wstring& instance_id) {
  publish_removed(instance_id);
}

void NotifierBridge::publish_removed(const std::wstring& instance_id) {
  EnterCriticalSection(&lock_);
  if (has_current_ && _wcsicmp(current_device_.instance_id.c_str(), instance_id.c_str()) == 0) {
    has_current_ = false;
  }
  LeaveCriticalSection(&lock_);
}

DWORD WINAPI NotifierBridge::thread_entry(void* context) {
  static_cast<NotifierBridge*>(context)->run();
  return 0;
}

void NotifierBridge::run() {
  SECURITY_ATTRIBUTES attributes = {};
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!pipe_security(&attributes, &descriptor)) return;
  while (WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
    ScopedHandle pipe(CreateNamedPipeW(
        kPipeName, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES, 64U * 1024U, 64U * 1024U, 1000, &attributes));
    if (pipe.get() == INVALID_HANDLE_VALUE) break;
    const BOOL connected = ConnectNamedPipe(pipe.get(), nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
    if (!connected) continue;
    if (WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0) break;
    std::string request;
    if (read_pipe_message(pipe.get(), &request)) write_pipe_message(pipe.get(), handle_request(request));
    DisconnectNamedPipe(pipe.get());
  }
  LocalFree(descriptor);
}

std::string NotifierBridge::current_message() {
  JsonValue response = JsonValue::object();
  EnterCriticalSection(&lock_);
  if (has_current_) {
    response.object_items()["type"] = JsonValue(std::string("blocked"));
    response.object_items()["eventId"] = JsonValue(current_event_id_);
    response.object_items()["reasonCode"] = JsonValue(current_reason_code_);
    response.object_items()["enforcementVerified"] = JsonValue(current_enforcement_verified_);
    response.object_items()["device"] = device_message(current_device_);
  } else {
    response.object_items()["type"] = JsonValue(std::string("none"));
  }
  LeaveCriticalSection(&lock_);
  return write_json(response);
}

std::string NotifierBridge::handle_request(const std::string& request) {
  JsonValue parsed;
  std::string parse_error;
  if (!parse_json(request, &parsed, &parse_error) || parsed.type() != JsonValue::Type::object) {
    return error_response(std::string(), L"The notifier request is invalid.");
  }
  const JsonValue* type = parsed.find("type");
  if (type == nullptr || type->type() != JsonValue::Type::string) {
    return error_response(std::string(), L"The notifier request type is missing.");
  }
  if (type->string_value() == "get") return current_message();
  if (type->string_value() != "request_access") {
    return error_response(std::string(), L"The notifier request type is unsupported.");
  }

  const JsonValue* event = parsed.find("eventId");
  const JsonValue* days = parsed.find("requestedDurationDays");
  const JsonValue* justification = parsed.find("justification");
  const JsonValue* sid = parsed.find("userSid");
  const JsonValue* username = parsed.find("username");
  if (event == nullptr || days == nullptr || justification == nullptr || sid == nullptr ||
      username == nullptr || event->type() != JsonValue::Type::string ||
      days->type() != JsonValue::Type::number || justification->type() != JsonValue::Type::string ||
      sid->type() != JsonValue::Type::string || username->type() != JsonValue::Type::string) {
    return error_response(std::string(), L"The access request is incomplete.");
  }

  AgentIdentity identity;
  WindowsDevice device;
  bool can_submit = false;
  EnterCriticalSection(&lock_);
  if (has_identity_ && has_current_ && current_event_id_ == event->string_value()) {
    identity = identity_;
    device = current_device_;
    can_submit = true;
  }
  LeaveCriticalSection(&lock_);
  if (!can_submit) return error_response(event->string_value(), L"The blocked device changed or the agent is not enrolled.");

  AccessRequestResult result;
  std::wstring error;
  if (!api_->request_access(identity, device, new_guid_text(), sid->string_value(),
                            username->string_value(), static_cast<int>(days->number_value()),
                            justification->string_value(), &result, &error)) {
    return error_response(event->string_value(), error);
  }
  JsonValue response = JsonValue::object();
  response.object_items()["type"] = JsonValue(std::string("request_result"));
  response.object_items()["eventId"] = JsonValue(event->string_value());
  response.object_items()["requestId"] = JsonValue(result.request_id);
  response.object_items()["status"] = JsonValue(result.status);
  if (result.retry_after_utc.empty()) {
    response.object_items()["retryAfterUtc"] = JsonValue();
  } else {
    response.object_items()["retryAfterUtc"] = JsonValue(result.retry_after_utc);
  }
  return write_json(response);
}

void NotifierBridge::launch_notifier() {
  if (GetFileAttributesW(notifier_path_.c_str()) == INVALID_FILE_ATTRIBUTES) return;
  const DWORD session_id = WTSGetActiveConsoleSessionId();
  if (session_id == 0xFFFFFFFF) return;
  HANDLE token = nullptr;
  if (!query_interactive_user_token(session_id, &token)) return;
  ScopedHandle user_token(token);
  HANDLE primary = nullptr;
  if (!DuplicateTokenEx(user_token.get(), MAXIMUM_ALLOWED, nullptr, SecurityIdentification,
                        TokenPrimary, &primary)) return;
  ScopedHandle primary_token(primary);
  LPVOID environment = nullptr;
  if (!CreateEnvironmentBlock(&environment, primary_token.get(), FALSE)) return;
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  startup.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
  PROCESS_INFORMATION process = {};
  std::vector<wchar_t> command(notifier_path_.begin(), notifier_path_.end());
  command.insert(command.begin(), L'\"');
  command.push_back(L'\"');
  command.push_back(L'\0');
  const BOOL created = CreateProcessAsUserW(
      primary_token.get(), notifier_path_.c_str(), &command[0], nullptr, nullptr, FALSE,
      CREATE_UNICODE_ENVIRONMENT, environment, nullptr, &startup, &process);
  DestroyEnvironmentBlock(environment);
  if (created) {
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
}

}  // namespace usb_dlp
