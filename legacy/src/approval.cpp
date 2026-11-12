#include "usb_dlp/approval.h"

#include "usb_dlp/crypto.h"

#include <cstring>

namespace usb_dlp {
namespace {

// 2025-01-01T00:00:00Z. Minutes are stored relative to this instant so the
// short code can carry a timestamp in three bytes.
const std::int64_t kBaseEpochMs = 1735689600000LL;
const std::uint8_t kVersion = 1;

using DeviceHash = std::array<std::uint8_t, 10>;

DeviceHash device_hash(const std::string& instance_id) {
  const crypto::Digest digest =
      crypto::sha256(reinterpret_cast<const std::uint8_t*>(instance_id.data()), instance_id.size());
  DeviceHash result = {};
  for (std::size_t index = 0; index < result.size(); ++index) result[index] = digest[index];
  return result;
}

std::uint32_t minutes_since_base(std::int64_t utc_ms) {
  if (utc_ms <= kBaseEpochMs) return 0;
  const std::int64_t minutes = (utc_ms - kBaseEpochMs) / 60000;
  if (minutes > 0xFFFFFFFFLL) return 0xFFFFFFFFU;
  return static_cast<std::uint32_t>(minutes);
}

std::int64_t utc_ms_from_minutes(std::uint32_t minutes) {
  return kBaseEpochMs + static_cast<std::int64_t>(minutes) * 60000;
}

std::uint32_t read_be(const std::uint8_t* data, int length) {
  std::uint32_t value = 0;
  for (int index = 0; index < length; ++index) {
    value = (value << 8) | data[index];
  }
  return value;
}

void write_be(std::uint8_t* data, int length, std::uint32_t value) {
  for (int index = 0; index < length; ++index) {
    data[length - 1 - index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFU);
  }
}

void set_error(ApprovalError* error, ApprovalError value) {
  if (error != nullptr) *error = value;
}

// Applies the checks every code must pass: version, device binding, validity
// window and the short-code duration cap.
bool accept_payload(const std::uint8_t* payload, std::size_t payload_size,
                    std::size_t device_hash_size, bool short_code,
                    const std::string& instance_id, std::int64_t now_utc_ms,
                    ApprovalGrant* grant, ApprovalError* error) {
  // A short code packs the duration into the low nibble of byte 0, so the
  // version has to be read from the high nibble there.
  const std::uint8_t version =
      short_code ? static_cast<std::uint8_t>(payload[0] >> 4) : payload[0];
  if (version != kVersion) {
    set_error(error, ApprovalError::unsupported_version);
    return false;
  }

  const DeviceHash expected = device_hash(instance_id);
  if (std::memcmp(payload + (short_code ? 4 : 1), expected.data(), device_hash_size) != 0) {
    set_error(error, ApprovalError::wrong_device);
    return false;
  }

  std::uint32_t not_before_minutes = 0;
  std::uint32_t not_after_minutes = 0;
  int duration_hours = 0;
  if (short_code) {
    duration_hours = payload[0] & 0x0F;
    not_before_minutes = read_be(payload + 1, 3);
    not_after_minutes = not_before_minutes + static_cast<std::uint32_t>(duration_hours) * 60U;
  } else {
    duration_hours = payload[19];
    not_before_minutes = read_be(payload + 11, 4);
    not_after_minutes = read_be(payload + 15, 4);
  }
  (void)payload_size;

  if (short_code && (duration_hours < 1 || duration_hours > kShortCodeMaxHours)) {
    set_error(error, ApprovalError::duration_out_of_range);
    return false;
  }
  if (!short_code && (duration_hours < 1 || duration_hours > 168)) {
    set_error(error, ApprovalError::duration_out_of_range);
    return false;
  }

  const std::int64_t not_before = utc_ms_from_minutes(not_before_minutes);
  const std::int64_t not_after = utc_ms_from_minutes(not_after_minutes);
  if (now_utc_ms < not_before) {
    set_error(error, ApprovalError::not_yet_valid);
    return false;
  }
  if (now_utc_ms >= not_after) {
    set_error(error, ApprovalError::expired);
    return false;
  }

  if (grant != nullptr) {
    grant->not_before_utc_ms = not_before;
    grant->not_after_utc_ms = not_after;
    grant->duration_hours = duration_hours;
    grant->signed_code = !short_code;
  }
  set_error(error, ApprovalError::none);
  return true;
}

}  // namespace

const char* approval_error_name(ApprovalError error) {
  switch (error) {
    case ApprovalError::none: return "ok";
    case ApprovalError::invalid_format: return "invalid_format";
    case ApprovalError::unsupported_version: return "unsupported_version";
    case ApprovalError::invalid_signature: return "invalid_signature";
    case ApprovalError::invalid_mac: return "invalid_mac";
    case ApprovalError::wrong_device: return "wrong_device";
    case ApprovalError::not_yet_valid: return "not_yet_valid";
    case ApprovalError::expired: return "expired";
    case ApprovalError::duration_out_of_range: return "duration_out_of_range";
  }
  return "unknown";
}

std::string device_code_raw(const std::string& instance_id) {
  const DeviceHash hash = device_hash(instance_id);
  return crypto::base32_encode(hash.data(), hash.size());
}

std::string device_code_display(const std::string& instance_id) {
  return crypto::base32_group(device_code_raw(instance_id), 4);
}

std::string approval_code_fingerprint(const std::string& code) {
  std::vector<std::uint8_t> decoded;
  std::string material = code;
  if (crypto::base32_decode(code, &decoded)) {
    // Hash the decoded bytes so different textual spellings of the same code
    // (with or without hyphens) collapse to one fingerprint.
    material.assign(decoded.begin(), decoded.end());
  }
  const crypto::Digest digest = crypto::sha256(
      reinterpret_cast<const std::uint8_t*>(material.data()), material.size());
  return crypto::base32_encode(digest.data(), digest.size());
}

bool verify_signed_approval_code(
    const std::string& code,
    const std::vector<std::uint8_t>& modulus,
    const std::string& instance_id,
    std::int64_t now_utc_ms,
    ApprovalGrant* grant,
    ApprovalError* error) {
  std::vector<std::uint8_t> decoded;
  if (!crypto::base32_decode(code, &decoded) || decoded.size() != kSignedCodeBytes) {
    set_error(error, ApprovalError::invalid_format);
    return false;
  }

  const std::uint8_t* payload = &decoded[0];
  const std::uint8_t* signature = &decoded[20];
  if (!crypto::rsa_verify_sha256(modulus, signature, 256, payload, 20)) {
    set_error(error, ApprovalError::invalid_signature);
    return false;
  }
  return accept_payload(payload, 20, 10, false, instance_id, now_utc_ms, grant, error);
}

bool verify_short_approval_code(
    const std::string& code,
    const std::vector<std::uint8_t>& device_key,
    const std::string& instance_id,
    std::int64_t now_utc_ms,
    ApprovalGrant* grant,
    ApprovalError* error) {
  std::vector<std::uint8_t> decoded;
  if (!crypto::base32_decode(code, &decoded) || decoded.size() != 15) {
    set_error(error, ApprovalError::invalid_format);
    return false;
  }
  if (device_key.empty()) {
    set_error(error, ApprovalError::invalid_mac);
    return false;
  }

  const std::uint8_t* payload = &decoded[0];
  const std::uint8_t* mac = &decoded[8];
  const crypto::Digest computed = crypto::hmac_sha256(&device_key[0], device_key.size(), payload, 8);
  if (!crypto::equal_constant_time(computed.data(), mac, 7)) {
    set_error(error, ApprovalError::invalid_mac);
    return false;
  }
  return accept_payload(payload, 8, 3, true, instance_id, now_utc_ms, grant, error);
}

}  // namespace usb_dlp
