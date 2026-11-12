#include "usb_dlp/policy_registry.h"

#include <windows.h>
#include <cfgmgr32.h>

#include <algorithm>
#include <cstdio>
#include <set>

namespace usb_dlp {
namespace {

const wchar_t* kRestrictionsPath =
    L"SOFTWARE\\Policies\\Microsoft\\Windows\\DeviceInstall\\Restrictions";
const wchar_t* kAllowListName = L"AllowDeviceIDs";
const wchar_t* kDenyListName = L"DenyDeviceIDs";
const wchar_t* kManifestName = L"backstop-manifest.dat";
const char* kManifestHeader = "usbd-lp-backstop 1";

struct Snapshot {
  bool du_present = false;
  unsigned long du = 0;
  bool allow_flag_present = false;
  unsigned long allow_flag = 0;
  std::vector<std::wstring> allow_list;
  bool deny_flag_present = false;
  unsigned long deny_flag = 0;
  std::vector<std::wstring> deny_list;
};

std::string narrow(const std::wstring& value) {
  if (value.empty()) return std::string();
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                                       static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (size <= 0) return std::string();
  std::string out(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                      &out[0], size, nullptr, nullptr);
  return out;
}

std::wstring widen(const std::string& value) {
  if (value.empty()) return std::wstring();
  const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
                                       static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) return std::wstring();
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), &out[0], size);
  return out;
}

std::wstring lower_copy(const std::wstring& value) {
  std::wstring out(value);
  for (wchar_t& ch : out) {
    if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
  }
  return out;
}

bool key_exists(HKEY root, const std::wstring& sub, REGSAM wow64, HKEY* out) {
  return RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ | wow64, out) == ERROR_SUCCESS;
}

// Accepts the 64-bit view first: on a 64-bit OS the policy key lives there even
// when the agent runs as a 32-bit process. KEY_WOW64_64KEY is ignored on 32-bit
// Windows, but older builds reject it, so fall back to the default view.
bool open_restrictions(bool create, REGSAM access, HKEY* out) {
  const REGSAM flags[2] = {KEY_WOW64_64KEY, 0};
  for (int i = 0; i < 2; ++i) {
    const REGSAM combined = access | flags[i];
    if (create) {
      const LONG result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRestrictionsPath, 0, nullptr,
                                          REG_OPTION_NON_VOLATILE, combined, nullptr, out, nullptr);
      if (result == ERROR_SUCCESS) return true;
      if (result != ERROR_INVALID_PARAMETER) return false;
    } else {
      if (key_exists(HKEY_LOCAL_MACHINE, kRestrictionsPath, flags[i], out)) return true;
    }
  }
  return false;
}

bool open_child(HKEY parent, const wchar_t* name, bool create, REGSAM access, HKEY* out) {
  if (create) {
    const LONG result = RegCreateKeyExW(parent, name, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                        access, nullptr, out, nullptr);
    return result == ERROR_SUCCESS;
  }
  return RegOpenKeyExW(parent, name, 0, access, out) == ERROR_SUCCESS;
}

bool read_dword(HKEY key, const wchar_t* name, bool* present, unsigned long* value) {
  DWORD type = 0;
  DWORD data = 0;
  DWORD size = sizeof(data);
  const LONG result = RegQueryValueExW(key, name, nullptr, &type,
                                       reinterpret_cast<BYTE*>(&data), &size);
  if (result == ERROR_FILE_NOT_FOUND) {
    *present = false;
    *value = 0;
    return true;
  }
  if (result != ERROR_SUCCESS || type != REG_DWORD) return false;
  *present = true;
  *value = data;
  return true;
}

bool read_list(HKEY parent, const wchar_t* name, std::vector<std::wstring>* out) {
  out->clear();
  HKEY list = nullptr;
  if (!open_child(parent, name, false, KEY_READ, &list)) return true;
  for (int index = 1; index <= 4096; ++index) {
    wchar_t value_name[16] = {};
    _snwprintf_s(value_name, _countof(value_name), _TRUNCATE, L"%d", index);
    wchar_t buffer[1024] = {};
    DWORD type = 0;
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    const LONG result = RegQueryValueExW(list, value_name, nullptr, &type,
                                         reinterpret_cast<BYTE*>(buffer), &size);
    if (result == ERROR_FILE_NOT_FOUND) break;
    if (result != ERROR_SUCCESS) break;
    if (type != REG_SZ) break;
    out->push_back(std::wstring(buffer));
  }
  RegCloseKey(list);
  return true;
}

bool write_dword(HKEY key, const wchar_t* name, bool present, unsigned long value) {
  if (!present) {
    const LONG result = RegDeleteValueW(key, name);
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
  }
  const DWORD data = value;
  return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&data),
                        sizeof(data)) == ERROR_SUCCESS;
}

bool write_list(HKEY parent, const wchar_t* name, const std::vector<std::wstring>& values) {
  if (values.empty()) {
    const LONG removed = RegDeleteKeyW(parent, name);
    return removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND;
  }
  HKEY list = nullptr;
  if (!open_child(parent, name, true, KEY_ALL_ACCESS, &list)) return false;
  bool ok = true;
  for (int index = 1; index <= 4096; ++index) {
    wchar_t value_name[16] = {};
    _snwprintf_s(value_name, _countof(value_name), _TRUNCATE, L"%d", index);
    if (RegDeleteValueW(list, value_name) == ERROR_FILE_NOT_FOUND) break;
  }
  for (std::size_t index = 0; index < values.size(); ++index) {
    wchar_t value_name[16] = {};
    _snwprintf_s(value_name, _countof(value_name), _TRUNCATE, L"%llu",
                 static_cast<unsigned long long>(index + 1));
    const DWORD bytes = static_cast<DWORD>((values[index].size() + 1) * sizeof(wchar_t));
    if (RegSetValueExW(list, value_name, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(values[index].c_str()), bytes) != ERROR_SUCCESS) {
      ok = false;
      break;
    }
  }
  RegCloseKey(list);
  return ok;
}

bool read_snapshot(Snapshot* out) {
  HKEY key = nullptr;
  if (!open_restrictions(false, KEY_READ, &key)) {
    *out = Snapshot();
    return true;
  }
  bool ok = read_dword(key, L"DenyUnspecified", &out->du_present, &out->du) &&
            read_dword(key, L"AllowDeviceIDs", &out->allow_flag_present, &out->allow_flag) &&
            read_dword(key, L"DenyDeviceIDs", &out->deny_flag_present, &out->deny_flag);
  ok = ok && read_list(key, kAllowListName, &out->allow_list) &&
       read_list(key, kDenyListName, &out->deny_list);
  RegCloseKey(key);
  return ok;
}

bool write_snapshot(const Snapshot& snapshot) {
  HKEY key = nullptr;
  if (!open_restrictions(true, KEY_ALL_ACCESS, &key)) return false;
  bool ok = write_dword(key, L"DenyUnspecified", snapshot.du_present, snapshot.du) &&
            write_dword(key, L"AllowDeviceIDs", snapshot.allow_flag_present, snapshot.allow_flag) &&
            write_dword(key, L"DenyDeviceIDs", snapshot.deny_flag_present, snapshot.deny_flag) &&
            write_list(key, kAllowListName, snapshot.allow_list) &&
            write_list(key, kDenyListName, snapshot.deny_list);
  RegCloseKey(key);
  return ok;
}

bool same_list(const std::vector<std::wstring>& left, const std::vector<std::wstring>& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (lower_copy(left[index]) != lower_copy(right[index])) return false;
  }
  return true;
}

bool same_snapshot(const Snapshot& left, const Snapshot& right) {
  return left.du_present == right.du_present && left.du == right.du &&
         left.allow_flag_present == right.allow_flag_present &&
         left.allow_flag == right.allow_flag &&
         left.deny_flag_present == right.deny_flag_present &&
         left.deny_flag == right.deny_flag &&
         same_list(left.allow_list, right.allow_list) &&
         same_list(left.deny_list, right.deny_list);
}

std::string encode_snapshot(const Snapshot& snapshot, const char* prefix) {
  std::string out;
  char line[64] = {};
  _snprintf_s(line, _countof(line), _TRUNCATE, "%s.du=%d:%lu\n", prefix,
              snapshot.du_present ? 1 : 0, snapshot.du);
  out += line;
  _snprintf_s(line, _countof(line), _TRUNCATE, "%s.af=%d:%lu\n", prefix,
              snapshot.allow_flag_present ? 1 : 0, snapshot.allow_flag);
  out += line;
  _snprintf_s(line, _countof(line), _TRUNCATE, "%s.df=%d:%lu\n", prefix,
              snapshot.deny_flag_present ? 1 : 0, snapshot.deny_flag);
  out += line;
  const std::string head(prefix);
  for (const std::wstring& id : snapshot.allow_list) out += head + ".a=" + narrow(id) + "\n";
  for (const std::wstring& id : snapshot.deny_list) out += head + ".d=" + narrow(id) + "\n";
  return out;
}

bool decode_snapshot(const std::string& text, const char* prefix, Snapshot* out) {
  *out = Snapshot();
  const std::string marker(prefix);
  std::size_t start = 0;
  bool seen = false;
  while (start < text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(start, end - start);
    start = end + 1;
    if (line.compare(0, marker.size(), marker) != 0) continue;
    const std::size_t dot = line.find('=', marker.size() + 1);
    if (dot == std::string::npos) continue;
    const std::string key = line.substr(marker.size() + 1, dot - marker.size() - 1);
    const std::string value = line.substr(dot + 1);
    if (key == "du" || key == "af" || key == "df") {
      const std::size_t colon = value.find(':');
      if (colon == std::string::npos) return false;
      const bool present = value.substr(0, colon) == "1";
      const unsigned long number = std::strtoul(value.c_str() + colon + 1, nullptr, 10);
      if (key == "du") { out->du_present = present; out->du = number; }
      if (key == "af") { out->allow_flag_present = present; out->allow_flag = number; }
      if (key == "df") { out->deny_flag_present = present; out->deny_flag = number; }
      seen = true;
    } else if (key == "a") {
      out->allow_list.push_back(widen(value));
      seen = true;
    } else if (key == "d") {
      out->deny_list.push_back(widen(value));
      seen = true;
    }
  }
  return seen;
}

bool read_text_file(const std::wstring& path, std::string* out) {
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size = {};
  bool ok = GetFileSizeEx(handle, &size) != FALSE && size.QuadPart < (16LL * 1024 * 1024);
  if (ok && size.QuadPart > 0) {
    out->assign(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    ok = ReadFile(handle, &(*out)[0], static_cast<DWORD>(out->size()), &read, nullptr) != FALSE;
    out->resize(ok ? read : 0);
  }
  CloseHandle(handle);
  return ok;
}

bool write_text_file(const std::wstring& path, const std::string& text) {
  const std::wstring staging = path + L".tmp";
  HANDLE handle = CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE &&
            written == text.size();
  FlushFileBuffers(handle);
  CloseHandle(handle);
  if (!ok) {
    DeleteFileW(staging.c_str());
    return false;
  }
  return MoveFileExW(staging.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool device_id_of(DEVINST node, std::wstring* id) {
  ULONG length = 0;
  if (CM_Get_Device_ID_Size(&length, node, 0) != CR_SUCCESS || length == 0) return false;
  std::vector<wchar_t> buffer(static_cast<std::size_t>(length) + 2, L'\0');
  if (CM_Get_Device_IDW(node, &buffer[0], length + 1, 0) != CR_SUCCESS) return false;
  id->assign(&buffer[0]);
  return true;
}

bool is_usb_family(const std::wstring& id) {
  return id.size() >= 4 && _wcsnicmp(id.c_str(), L"USB\\", 4) == 0;
}

bool is_usbstor(const std::wstring& id) {
  return id.size() >= 8 && _wcsnicmp(id.c_str(), L"USBSTOR\\", 8) == 0;
}

void add_unique(const std::wstring& id, std::set<std::wstring>* seen,
                std::vector<std::wstring>* out) {
  if (id.empty()) return;
  const std::wstring key = lower_copy(id);
  if (!seen->insert(key).second) return;
  out->push_back(id);
}

void collect_children(DEVINST node, int depth, std::set<std::wstring>* seen,
                      std::vector<std::wstring>* out) {
  if (depth <= 0) return;
  DEVINST child = 0;
  if (CM_Get_Child(&child, node, 0) != CR_SUCCESS) return;
  for (;;) {
    std::wstring id;
    if (device_id_of(child, &id)) {
      add_unique(id, seen, out);
      collect_children(child, depth - 1, seen, out);
    }
    DEVINST sibling = 0;
    if (CM_Get_Sibling(&sibling, child, 0) != CR_SUCCESS) break;
    child = sibling;
  }
}

// A USB mass-storage device installs as a USB function node plus one or more
// USBSTOR children. DenyUnspecified blocks installation of any devnode that no
// allow entry describes, so the whole chain has to be listed or an approved
// device still fails to install. Only instance IDs are collected, never hardware
// or compatible IDs, which would widen the approval to every unit of the model.
void expand_chain(const std::wstring& instance_id, std::set<std::wstring>* seen,
                  std::vector<std::wstring>* out) {
  std::vector<wchar_t> mutable_id(instance_id.begin(), instance_id.end());
  mutable_id.push_back(L'\0');
  DEVINST node = 0;
  CONFIGRET result = CM_Locate_DevNodeW(&node, &mutable_id[0], CM_LOCATE_DEVNODE_NORMAL);
  if (result != CR_SUCCESS) {
    result = CM_Locate_DevNodeW(&node, &mutable_id[0], CM_LOCATE_DEVNODE_PHANTOM);
  }
  if (result != CR_SUCCESS) {
    add_unique(instance_id, seen, out);
    return;
  }
  std::wstring id;
  if (device_id_of(node, &id)) add_unique(id, seen, out);
  collect_children(node, 2, seen, out);
  DEVINST parent = node;
  for (int level = 0; level < 4; ++level) {
    DEVINST next = 0;
    if (CM_Get_Parent(&next, parent, 0) != CR_SUCCESS) break;
    std::wstring parent_id;
    if (!device_id_of(next, &parent_id)) break;
    if (!is_usb_family(parent_id) && !is_usbstor(parent_id)) break;
    add_unique(parent_id, seen, out);
    collect_children(next, 1, seen, out);
    parent = next;
  }
}

bool has_manifest_header(const std::string& text) {
  const std::string header(kManifestHeader);
  return text.size() >= header.size() && text.compare(0, header.size(), header) == 0;
}

}  // namespace

bool RegistryBackstop::initialize(const std::wstring& data_directory, std::wstring* error) {
  manifest_path_ = data_directory;
  if (!manifest_path_.empty() &&
      manifest_path_[manifest_path_.size() - 1] != L'\\') {
    manifest_path_ += L'\\';
  }
  manifest_path_ += kManifestName;
  const DWORD attributes = GetFileAttributesW(manifest_path_.c_str());
  has_manifest_ = attributes != INVALID_FILE_ATTRIBUTES &&
                  (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
  if (error != nullptr) error->clear();
  return true;
}

BackstopResult RegistryBackstop::apply(
    bool default_deny,
    const std::vector<std::wstring>& allow_instance_ids,
    const std::vector<std::wstring>& deny_instance_ids,
    bool dry_run) {
  BackstopResult result;
  result.dry_run = dry_run;

  Snapshot current;
  if (!read_snapshot(&current)) {
    result.detail = L"read_failed";
    return result;
  }

  Snapshot before;
  Snapshot written;
  bool have_manifest = false;
  if (has_manifest_) {
    std::string text;
    if (read_text_file(manifest_path_, &text) && has_manifest_header(text)) {
      have_manifest = decode_snapshot(text, "before", &before) &&
                      decode_snapshot(text, "written", &written);
    }
    if (!have_manifest) {
      // A manifest we cannot parse is worse than none: refuse to touch values
      // whose history is unknown rather than guess.
      result.conflict = true;
      result.detail = L"manifest_unreadable";
      return result;
    }
  }

  if (have_manifest) {
    if (!same_snapshot(current, written)) {
      // Someone else changed the policy after we published. Leave their values
      // alone; the caller reports external_policy_conflict and keeps enforcing
      // through the active path.
      result.conflict = true;
      result.detail = L"external_change";
      return result;
    }
  } else {
    const bool externally_owned =
        (current.du_present && current.du != 0) ||
        (current.allow_flag_present && current.allow_flag != 0) ||
        (current.deny_flag_present && current.deny_flag != 0) ||
        !current.allow_list.empty() || !current.deny_list.empty();
    if (externally_owned) {
      result.conflict = true;
      result.detail = L"external_policy_present";
      return result;
    }
    before = current;
  }

  std::set<std::wstring> seen;
  std::vector<std::wstring> expanded;
  if (default_deny) {
    for (const std::wstring& id : allow_instance_ids) expand_chain(id, &seen, &expanded);
  }
  std::sort(expanded.begin(), expanded.end());

  Snapshot desired;
  desired.du_present = true;
  desired.du = default_deny ? 1UL : 0UL;
  desired.allow_flag_present = true;
  desired.allow_flag = default_deny && !expanded.empty() ? 1UL : 0UL;
  desired.allow_list = default_deny ? expanded : std::vector<std::wstring>();
  desired.deny_flag_present = true;
  desired.deny_flag = (!default_deny && !deny_instance_ids.empty()) ? 1UL : 0UL;
  desired.deny_list = default_deny ? std::vector<std::wstring>() : deny_instance_ids;

  result.allow_values = desired.allow_list.size();
  result.expanded_ids = expanded.size();

  if (same_snapshot(current, desired)) {
    result.unchanged = true;
    return result;
  }
  if (dry_run) {
    result.detail = L"dry_run";
    return result;
  }

  if (!write_snapshot(desired)) {
    write_snapshot(before);
    result.detail = L"write_failed_rolled_back";
    return result;
  }

  Snapshot verify;
  if (!read_snapshot(&verify) || !same_snapshot(verify, desired)) {
    write_snapshot(before);
    result.detail = L"verify_failed_rolled_back";
    return result;
  }

  std::string text = std::string(kManifestHeader) + "\n";
  text += encode_snapshot(before, "before");
  text += encode_snapshot(desired, "written");
  if (!write_text_file(manifest_path_, text)) {
    write_snapshot(before);
    result.detail = L"manifest_write_failed_rolled_back";
    return result;
  }

  has_manifest_ = true;
  result.applied = true;
  return result;
}

bool RegistryBackstop::restore(std::wstring* error) {
  if (error != nullptr) error->clear();
  if (!has_manifest_) return true;

  std::string text;
  Snapshot before;
  Snapshot written;
  if (!read_text_file(manifest_path_, &text) || !has_manifest_header(text) ||
      !decode_snapshot(text, "before", &before) ||
      !decode_snapshot(text, "written", &written)) {
    if (error != nullptr) *error = L"manifest_unreadable";
    return false;
  }

  Snapshot current;
  if (!read_snapshot(&current)) {
    if (error != nullptr) *error = L"read_failed";
    return false;
  }

  // Restore field by field, and only where the current value still equals what
  // this agent wrote. A value someone else changed is not ours to revert.
  Snapshot target = current;
  bool any = false;
  if (current.du_present == written.du_present && current.du == written.du) {
    target.du_present = before.du_present;
    target.du = before.du;
    any = true;
  }
  if (current.allow_flag_present == written.allow_flag_present &&
      current.allow_flag == written.allow_flag) {
    target.allow_flag_present = before.allow_flag_present;
    target.allow_flag = before.allow_flag;
    any = true;
  }
  if (same_list(current.allow_list, written.allow_list)) {
    target.allow_list = before.allow_list;
    any = true;
  }
  if (current.deny_flag_present == written.deny_flag_present &&
      current.deny_flag == written.deny_flag) {
    target.deny_flag_present = before.deny_flag_present;
    target.deny_flag = before.deny_flag;
    any = true;
  }
  if (same_list(current.deny_list, written.deny_list)) {
    target.deny_list = before.deny_list;
    any = true;
  }

  if (any && !write_snapshot(target)) {
    if (error != nullptr) *error = L"restore_write_failed";
    return false;
  }
  DeleteFileW(manifest_path_.c_str());
  has_manifest_ = false;
  if (!same_snapshot(current, written) && error != nullptr) {
    *error = L"partial_restore_external_values_kept";
  }
  return true;
}

}  // namespace usb_dlp
