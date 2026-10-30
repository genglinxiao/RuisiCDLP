#include "usb_dlp/http_client.h"

#include <windows.h>
#include <winhttp.h>

#include <vector>

namespace usb_dlp {
namespace {

class InternetHandle {
 public:
  explicit InternetHandle(HINTERNET value = nullptr) : value_(value) {}
  ~InternetHandle() { if (value_ != nullptr) WinHttpCloseHandle(value_); }
  HINTERNET get() const { return value_; }

 private:
  HINTERNET value_;
  InternetHandle(const InternetHandle&);
  InternetHandle& operator=(const InternetHandle&);
};

bool set_error(std::wstring* error, const wchar_t* operation) {
  if (error != nullptr) *error = std::wstring(operation) + L" failed: " + std::to_wstring(GetLastError());
  return false;
}

}  // namespace

HttpClient::HttpClient(const std::wstring& base_url, bool allow_insecure_http)
    : port_(0), secure_(false), valid_(false) {
  URL_COMPONENTS components = {};
  components.dwStructSize = sizeof(components);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(base_url.c_str(), 0, 0, &components)) {
    validation_error_ = L"Server URL is invalid.";
    return;
  }
  secure_ = components.nScheme == INTERNET_SCHEME_HTTPS;
  if (!secure_ && components.nScheme != INTERNET_SCHEME_HTTP) {
    validation_error_ = L"Server URL must use HTTP or HTTPS.";
    return;
  }
  if (secure_) {
    validation_error_ = L"Portable pinned TLS is not installed in this compatibility build.";
    return;
  }
  if (!allow_insecure_http) {
    validation_error_ = L"HTTP requires AllowInsecureHttp=1 for an isolated test deployment.";
    return;
  }
  host_.assign(components.lpszHostName, components.dwHostNameLength);
  base_path_.assign(components.lpszUrlPath, components.dwUrlPathLength);
  if (base_path_.empty()) base_path_ = L"/";
  if (base_path_[base_path_.size() - 1] != L'/') base_path_ += L'/';
  port_ = components.nPort;
  valid_ = !host_.empty() && port_ != 0;
  if (!valid_) validation_error_ = L"Server URL is missing a host or port.";
}

bool HttpClient::request(
    const wchar_t* method,
    const std::wstring& path,
    const std::string& body,
    const std::string& bearer,
    HttpResponse* response,
    std::wstring* error) const {
  if (!valid_ || response == nullptr) return false;
  InternetHandle session(WinHttpOpen(L"RuisiUsbDlpLegacy/1.1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (session.get() == nullptr) return set_error(error, L"WinHttpOpen");
  WinHttpSetTimeouts(session.get(), 10000, 10000, 20000, 20000);
  InternetHandle connection(WinHttpConnect(session.get(), host_.c_str(), port_, 0));
  if (connection.get() == nullptr) return set_error(error, L"WinHttpConnect");
  std::wstring request_path = base_path_ + (path.empty() || path[0] != L'/' ? path : path.substr(1));
  InternetHandle request_handle(WinHttpOpenRequest(connection.get(), method, request_path.c_str(),
                                                   nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
  if (request_handle.get() == nullptr) return set_error(error, L"WinHttpOpenRequest");
  std::wstring headers = L"Accept: application/json\r\n";
  if (!body.empty()) headers += L"Content-Type: application/json; charset=utf-8\r\n";
  if (!bearer.empty()) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, bearer.c_str(), static_cast<int>(bearer.size()), nullptr, 0);
    std::wstring wide_bearer(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bearer.c_str(), static_cast<int>(bearer.size()), &wide_bearer[0], length);
    headers += L"Authorization: Bearer " + wide_bearer + L"\r\n";
  }
  LPVOID optional = body.empty() ? WINHTTP_NO_REQUEST_DATA :
      const_cast<char*>(body.data());
  if (!WinHttpSendRequest(request_handle.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
                          optional, static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
      !WinHttpReceiveResponse(request_handle.get(), nullptr)) {
    return set_error(error, L"WinHttp request");
  }
  DWORD status = 0;
  DWORD status_size = sizeof(status);
  if (!WinHttpQueryHeaders(request_handle.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX)) {
    return set_error(error, L"WinHttpQueryHeaders");
  }
  response->status_code = status;
  response->body.clear();
  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request_handle.get(), &available)) return set_error(error, L"WinHttpQueryDataAvailable");
    if (available == 0) break;
    if (response->body.size() + available > 1024U * 1024U) {
      if (error != nullptr) *error = L"HTTP response exceeds 1 MiB.";
      return false;
    }
    const std::size_t prior = response->body.size();
    response->body.resize(prior + available);
    DWORD read = 0;
    if (!WinHttpReadData(request_handle.get(), &response->body[prior], available, &read)) {
      return set_error(error, L"WinHttpReadData");
    }
    response->body.resize(prior + read);
  }
  return true;
}

}  // namespace usb_dlp
