#pragma once

#ifdef _WIN32

#include <string>

namespace usb_dlp {

bool ensure_protected_directory(const std::wstring& path, std::wstring* error);
bool write_utf16_text_file(
    const std::wstring& path,
    const std::wstring& contents,
    std::wstring* error);

class SecureStore {
 public:
  explicit SecureStore(const std::wstring& data_directory) : data_directory_(data_directory) {}

  bool save(const std::wstring& name, const std::string& value, std::wstring* error) const;
  bool load(const std::wstring& name, std::string* value, std::wstring* error) const;
  bool exists(const std::wstring& name) const;

 private:
  std::wstring data_directory_;
  std::wstring path(const std::wstring& name) const;
};

}  // namespace usb_dlp

#endif
