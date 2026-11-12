#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace usb_dlp {

// Offline approval codes. The byte layout is fixed by
// docs/offline-approval-spec.md and must match the .NET implementation exactly,
// otherwise a code minted for one product line would be rejected by the other.

struct ApprovalGrant {
  std::int64_t not_before_utc_ms = 0;
  std::int64_t not_after_utc_ms = 0;
  int duration_hours = 0;
  bool signed_code = false;
};

// Describes why a code was rejected; the caller logs it verbatim.
enum class ApprovalError {
  none,
  invalid_format,
  unsupported_version,
  invalid_signature,
  invalid_mac,
  wrong_device,
  not_yet_valid,
  expired,
  duration_out_of_range,
};

const char* approval_error_name(ApprovalError error);

// The 16-character code the user reads out to IT, and its grouped form.
std::string device_code_raw(const std::string& instance_id);
std::string device_code_display(const std::string& instance_id);

// Verifies a standard code (RSA-2048 / PKCS#1 v1.5 / SHA-256).
// `modulus` is the 256-byte big-endian public modulus compiled into the agent.
bool verify_signed_approval_code(
    const std::string& code,
    const std::vector<std::uint8_t>& modulus,
    const std::string& instance_id,
    std::int64_t now_utc_ms,
    ApprovalGrant* grant,
    ApprovalError* error);

// Verifies a short code (HMAC-SHA256 with the per-device key).
// Short codes are capped at kShortCodeMaxHours; longer requests must use the
// signed channel.
bool verify_short_approval_code(
    const std::string& code,
    const std::vector<std::uint8_t>& device_key,
    const std::string& instance_id,
    std::int64_t now_utc_ms,
    ApprovalGrant* grant,
    ApprovalError* error);

// SHA-256 of the normalised code text, used as the anti-replay key so the code
// itself never reaches the log.
std::string approval_code_fingerprint(const std::string& code);

const int kShortCodeMaxHours = 8;
const std::size_t kDeviceCodeChars = 16;
const std::size_t kShortCodeChars = 24;
const std::size_t kSignedCodeBytes = 276;

}  // namespace usb_dlp
