#include "usb_dlp/secure_store.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <wincrypt.h>

#include <vector>

namespace usb_dlp {
namespace {

// XP's SDDL parser does not accept the protected-DACL control token. Apply
// protection separately with SetNamedSecurityInfo after creating the ACL.
const wchar_t* const kRestrictedDacl = L"D:(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";

bool set_error_code(std::wstring* error, const wchar_t* operation, DWORD code) {
  if (error != nullptr) {
    *error = std::wstring(operation) + L" failed: " + std::to_wstring(code);
  }
  return false;
}

bool set_error(std::wstring* error, const wchar_t* operation) {
  return set_error_code(error, operation, GetLastError());
}

bool supports_persistent_acls(const std::wstring& path, std::wstring* error) {
  wchar_t volume_path[MAX_PATH] = {};
  if (!GetVolumePathNameW(path.c_str(), volume_path, MAX_PATH)) {
    return set_error(error, L"GetVolumePathName");
  }
  wchar_t filesystem[MAX_PATH] = {};
  DWORD flags = 0;
  if (!GetVolumeInformationW(
          volume_path, nullptr, 0, nullptr, nullptr, &flags, filesystem, MAX_PATH)) {
    return set_error(error, L"GetVolumeInformation");
  }
  if ((flags & FILE_PERSISTENT_ACLS) == 0) {
    if (error != nullptr) {
      *error = L"filesystem does not support persistent ACLs: " + std::wstring(filesystem);
    }
    SetLastError(ERROR_NOT_SUPPORTED);
    return false;
  }
  return true;
}

bool read_all(const std::wstring& path, std::vector<BYTE>* bytes, std::wstring* error) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return set_error(error, L"CreateFile(read)");
  LARGE_INTEGER size = {};
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 1 || size.QuadPart > 2 * 1024 * 1024) {
    CloseHandle(file);
    SetLastError(ERROR_FILE_INVALID);
    return set_error(error, L"GetFileSizeEx");
  }
  bytes->assign(static_cast<std::size_t>(size.QuadPart), 0);
  DWORD read = 0;
  const BOOL success = ReadFile(file, &(*bytes)[0], static_cast<DWORD>(bytes->size()), &read, nullptr);
  CloseHandle(file);
  if (!success || read != bytes->size()) return set_error(error, L"ReadFile");
  return true;
}

bool write_all(const std::wstring& path, const BYTE* bytes, DWORD size, std::wstring* error) {
  const std::wstring temporary = path + L".tmp";
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (file == INVALID_HANDLE_VALUE) return set_error(error, L"CreateFile(write)");
  DWORD written = 0;
  const BOOL success = WriteFile(file, bytes, size, &written, nullptr) && FlushFileBuffers(file);
  CloseHandle(file);
  if (!success || written != size) {
    DeleteFileW(temporary.c_str());
    return set_error(error, L"WriteFile");
  }
  if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temporary.c_str());
    return set_error(error, L"MoveFileEx");
  }
  return true;
}

}  // namespace

bool ensure_protected_directory(const std::wstring& path, std::wstring* error) {
  if (path.empty()) return set_error_code(error, L"validate data directory", ERROR_INVALID_PARAMETER);
  if (!supports_persistent_acls(path, error)) return false;

  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          kRestrictedDacl, SDDL_REVISION_1, &descriptor, nullptr)) {
    return set_error(error, L"ConvertStringSecurityDescriptor");
  }
  SECURITY_ATTRIBUTES attributes = {};
  attributes.nLength = sizeof(attributes);
  attributes.lpSecurityDescriptor = descriptor;
  BOOL created = CreateDirectoryW(path.c_str(), &attributes);
  const DWORD create_error = GetLastError();
  if (created == FALSE && create_error != ERROR_ALREADY_EXISTS) {
    LocalFree(descriptor);
    return set_error_code(error, L"CreateDirectory", create_error);
  }

  const DWORD attributes_value = GetFileAttributesW(path.c_str());
  if (attributes_value == INVALID_FILE_ATTRIBUTES ||
      (attributes_value & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    const DWORD attributes_error = attributes_value == INVALID_FILE_ATTRIBUTES
                                       ? GetLastError()
                                       : ERROR_DIRECTORY;
    LocalFree(descriptor);
    return set_error_code(error, L"validate created directory", attributes_error);
  }

  BOOL dacl_present = FALSE;
  BOOL dacl_defaulted = FALSE;
  PACL dacl = nullptr;
  if (!GetSecurityDescriptorDacl(descriptor, &dacl_present, &dacl, &dacl_defaulted) ||
      !dacl_present || dacl == nullptr) {
    const DWORD descriptor_error = GetLastError() == ERROR_SUCCESS
                                       ? ERROR_INVALID_SECURITY_DESCR
                                       : GetLastError();
    LocalFree(descriptor);
    return set_error_code(error, L"GetSecurityDescriptorDacl", descriptor_error);
  }

  std::vector<wchar_t> mutable_path(path.begin(), path.end());
  mutable_path.push_back(L'\0');
  const DWORD security_error = SetNamedSecurityInfoW(
      &mutable_path[0], SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
      nullptr, nullptr, dacl, nullptr);
  LocalFree(descriptor);
  if (security_error != ERROR_SUCCESS) {
    return set_error_code(error, L"SetNamedSecurityInfo", security_error);
  }
  return true;
}

bool write_utf16_text_file(
    const std::wstring& path,
    const std::wstring& contents,
    std::wstring* error) {
  const std::wstring temporary = path + L".tmp";
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return set_error(error, L"CreateFile(config)");
  std::vector<wchar_t> payload;
  payload.reserve(contents.size() + 1);
  payload.push_back(static_cast<wchar_t>(0xfeff));
  payload.insert(payload.end(), contents.begin(), contents.end());
  DWORD written = 0;
  const DWORD byte_count = static_cast<DWORD>(payload.size() * sizeof(wchar_t));
  const BOOL write_success =
      WriteFile(file, &payload[0], byte_count, &written, nullptr) && FlushFileBuffers(file);
  const DWORD write_error = GetLastError();
  CloseHandle(file);
  if (!write_success || written != byte_count) {
    DeleteFileW(temporary.c_str());
    return set_error_code(error, L"WriteFile(config)", write_error);
  }
  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const DWORD move_error = GetLastError();
    DeleteFileW(temporary.c_str());
    return set_error_code(error, L"MoveFileEx(config)", move_error);
  }
  return true;
}

std::wstring SecureStore::path(const std::wstring& name) const {
  return data_directory_ + (data_directory_.empty() || data_directory_[data_directory_.size() - 1] == L'\\'
                                ? L""
                                : L"\\") + name;
}

bool SecureStore::save(
    const std::wstring& name,
    const std::string& value,
    std::wstring* error) const {
  DATA_BLOB input = {};
  input.cbData = static_cast<DWORD>(value.size());
  input.pbData = value.empty() ? nullptr : reinterpret_cast<BYTE*>(const_cast<char*>(value.data()));
  DATA_BLOB output = {};
  if (!CryptProtectData(&input, L"Ruisi USB DLP legacy state", nullptr, nullptr, nullptr,
                        CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    return set_error(error, L"CryptProtectData");
  }
  const bool result = write_all(path(name), output.pbData, output.cbData, error);
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
  return result;
}

bool SecureStore::load(
    const std::wstring& name,
    std::string* value,
    std::wstring* error) const {
  if (value == nullptr) return false;
  std::vector<BYTE> encrypted;
  if (!read_all(path(name), &encrypted, error)) return false;
  DATA_BLOB input = {};
  input.cbData = static_cast<DWORD>(encrypted.size());
  input.pbData = &encrypted[0];
  DATA_BLOB output = {};
  if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    return set_error(error, L"CryptUnprotectData");
  }
  value->assign(reinterpret_cast<const char*>(output.pbData), output.cbData);
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
  SecureZeroMemory(&encrypted[0], encrypted.size());
  return true;
}

bool SecureStore::exists(const std::wstring& name) const {
  const DWORD attributes = GetFileAttributesW(path(name).c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

}  // namespace usb_dlp
