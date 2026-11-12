#pragma once

#ifdef _WIN32

#include <string>
#include <vector>

namespace usb_dlp {

// Passive installation baseline published through the documented Device
// Installation Restrictions policy. This is the service-down backstop: while
// the agent is stopped, Windows still refuses to install an unapproved device.
//
// The mechanism is OS-version dependent and the two are not interchangeable:
//
//   Windows 7 / 8 / 8.1  no layered evaluation exists ("Apply layered order of
//                        evaluation" requires Windows 10 2106 / Server 2016).
//                        Default-deny is expressed as DenyUnspecified=1 plus an
//                        allow list. DenyRemovableDevices MUST NOT be combined
//                        with the allow list here: without layering every
//                        specific deny outranks every allow, so the allow list
//                        would be destroyed and approvals would stop working.
//
//   Windows 10 / 11       layered evaluation is available and the modern .NET
//                        line uses it. The native compatibility line keeps the
//                        DenyUnspecified form so one code path covers 6.1
//                        through 10.0; the cost is granularity, not safety.
//
// The allow list carries instance IDs only, never hardware or compatible IDs.
// Hardware IDs are model level: whitelisting one would silently approve every
// unit of that model, including devices the administrator never saw. Instance
// IDs keep the approval bound to one physical device. A USB mass-storage device
// installs as several devnodes (the USB function node plus one or more USBSTOR
// children), and every one of them has to be listed or the approved device still
// fails to install, so the chain is expanded before publishing.

struct BackstopResult {
  bool applied = false;
  bool unchanged = false;
  bool conflict = false;
  bool dry_run = false;
  std::size_t allow_values = 0;
  std::size_t expanded_ids = 0;
  std::wstring detail;
};

class RegistryBackstop {
 public:
  // Loads the before-image persisted by a previous run. A manifest that is
  // present means this agent owns the values it lists; anything else in the
  // policy key is treated as externally owned and is never modified.
  bool initialize(const std::wstring& data_directory, std::wstring* error);

  // Publishes the desired baseline. Idempotent: a second call with the same
  // policy produces no registry writes.
  BackstopResult apply(
      bool default_deny,
      const std::vector<std::wstring>& allow_instance_ids,
      const std::vector<std::wstring>& deny_instance_ids,
      bool dry_run);

  // Restores the before-image for every value this agent wrote and that still
  // matches what it wrote. Values changed by someone else are left alone and
  // reported through error.
  bool restore(std::wstring* error);

  bool has_manifest() const { return has_manifest_; }

 private:
  std::wstring manifest_path_;
  bool has_manifest_ = false;
};

}  // namespace usb_dlp

#endif
