#pragma once

#ifdef _WIN32

#include <map>
#include <string>

namespace usb_dlp {

struct HttpResponse {
  unsigned long status_code = 0;
  std::string body;
  std::map<std::string, std::string> headers;
};

class HttpClient {
 public:
  HttpClient(const std::wstring& base_url, bool allow_insecure_http);

  bool valid() const { return valid_; }
  const std::wstring& validation_error() const { return validation_error_; }
  bool request(
      const wchar_t* method,
      const std::wstring& path,
      const std::string& body,
      const std::string& bearer,
      HttpResponse* response,
      std::wstring* error) const;

 private:
  std::wstring host_;
  std::wstring base_path_;
  unsigned short port_;
  bool secure_;
  bool valid_;
  std::wstring validation_error_;
};

}  // namespace usb_dlp

#endif
