#include "usb_dlp/secure_store.h"

#include <windows.h>
#include <aclapi.h>

#include <cstdlib>
#include <iostream>
#include <string>

int wmain() {
  wchar_t temporary[MAX_PATH] = {};
  if (GetTempPathW(MAX_PATH, temporary) == 0) return 1;
  const std::wstring directory = std::wstring(temporary) +
                                 L"UsbDlp Legacy \x4E2D\x6587 Store Test-" +
                                 std::to_wstring(GetCurrentProcessId());
  std::wstring error;
  if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return 1;
  PSECURITY_DESCRIPTOR original_descriptor = nullptr;
  PACL original_dacl = nullptr;
  if (GetNamedSecurityInfoW(
      const_cast<wchar_t*>(directory.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
      nullptr, nullptr, &original_dacl, nullptr, &original_descriptor) != ERROR_SUCCESS ||
      original_descriptor == nullptr) {
    std::cerr << "original DACL capture failed" << std::endl;
    return 1;
  }
  usb_dlp::SecureStore store(directory);
  const std::wstring ini_path = directory + L"\\unicode-config.ini";
  const std::wstring expected_path = L"C:\\Data\\\x4E2D\x6587 path\\token.txt";
  const std::wstring ini_contents =
      L"[Agent]\r\nEnrollmentTokenFile=" + expected_path + L"\r\n";
  if (!usb_dlp::write_utf16_text_file(ini_path, ini_contents, &error)) {
    std::wcerr << error << std::endl;
    return 1;
  }
  wchar_t configured_path[MAX_PATH] = {};
  GetPrivateProfileStringW(L"Agent", L"EnrollmentTokenFile", L"", configured_path,
                           MAX_PATH, ini_path.c_str());
  if (expected_path != configured_path) {
    std::cerr << "Unicode INI round-trip mismatch" << std::endl;
    return 1;
  }
  const std::string secret = "policy-and-credential-fixture";
  if (!store.save(L"state.dat", secret, &error)) {
    std::wcerr << error << std::endl;
    return 1;
  }
  std::string loaded;
  if (!store.load(L"state.dat", &loaded, &error)) {
    std::wcerr << error << std::endl;
    return 1;
  }
  if (loaded != secret) {
    std::cerr << "round-trip mismatch" << std::endl;
    return 1;
  }
  DeleteFileW((directory + L"\\state.dat").c_str());
  DeleteFileW(ini_path.c_str());

  if (!usb_dlp::ensure_protected_directory(directory, &error)) {
    std::wcerr << error << std::endl;
    return 1;
  }
  PSECURITY_DESCRIPTOR protected_descriptor = nullptr;
  PACL protected_dacl = nullptr;
  const DWORD security_result = GetNamedSecurityInfoW(
      const_cast<wchar_t*>(directory.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
      nullptr, nullptr, &protected_dacl, nullptr, &protected_descriptor);
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  const bool protected_acl_valid =
      security_result == ERROR_SUCCESS && protected_descriptor != nullptr &&
      protected_dacl != nullptr &&
      GetSecurityDescriptorControl(protected_descriptor, &control, &revision) &&
      (control & SE_DACL_PROTECTED) != 0 && protected_dacl->AceCount == 2;

  const DWORD restore_result = SetNamedSecurityInfoW(
      const_cast<wchar_t*>(directory.c_str()), SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
      nullptr, nullptr, original_dacl, nullptr);
  if (protected_descriptor != nullptr) LocalFree(protected_descriptor);
  LocalFree(original_descriptor);
  if (!protected_acl_valid || restore_result != ERROR_SUCCESS) {
    std::cerr << "protected DACL verification failed" << std::endl;
    return 1;
  }
  RemoveDirectoryW(directory.c_str());
  std::cout << "Secure store tests passed." << std::endl;
  return 0;
}
