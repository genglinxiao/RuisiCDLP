// Cross-checks the offline approval codec against fixtures produced by
// tools/approval_codec.py, which is the same encoder the IT-side tool and the
// .NET implementation use. A mismatch here means a code minted for one product
// line would be rejected by the other.

#include "usb_dlp/approval.h"
#include "usb_dlp/approval_key.h"
#include "usb_dlp/crypto.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* name) {
  if (condition) {
    std::printf("PASS %s\n", name);
    return;
  }
  std::printf("FAIL %s\n", name);
  ++g_failures;
}

std::string to_hex(const std::uint8_t* data, std::size_t size) {
  static const char kDigits[] = "0123456789abcdef";
  std::string result;
  for (std::size_t index = 0; index < size; ++index) {
    result.push_back(kDigits[(data[index] >> 4) & 0x0F]);
    result.push_back(kDigits[data[index] & 0x0F]);
  }
  return result;
}

std::vector<std::uint8_t> public_modulus() {
  const std::uint8_t* bytes = usb_dlp::approval_modulus();
  return std::vector<std::uint8_t>(bytes, bytes + 256);
}

std::vector<std::uint8_t> test_device_key() {
  std::vector<std::uint8_t> key(32);
  for (int index = 0; index < 32; ++index) key[static_cast<std::size_t>(index)] =
      static_cast<std::uint8_t>(index);
  return key;
}

// Instants shared with the fixture generator.
const std::int64_t kNotBeforeMs = 1767225600000LL;   // 2026-01-01T00:00:00Z
const std::int64_t kNowValidMs = 1767229200000LL;    // +1h
const std::int64_t kNowExpiredMs = 1767232800000LL;  // +2h

const char* const kInstanceId =
    R"(USBSTOR\DISK&VEN_MASS&PROD_STORAGE_DEVICE&REV_1.00\121220160204&0)";
const char* const kOtherInstanceId = R"(USBSTOR\DISK&VEN_OTHER\999&0)";

// 4-hour code bound to kInstanceId, generated with the private key in
// artifacts/approval-keys/.
const char* const kSignedValid =
    "04NC27ZYJP5NYRY39W00G1900040C404KTE7XQC9AYNM4KS9GQ6JXVWBAR7JQJFFFMPMPW8K9METWYGQYS139S"
    "T6JGHYD1G07562YJ2BQJWWRC0YGVDRC6DZ8PPCF256XX698GJHCWPB9NJN2474FZFBCFG7PX2V71677ZNHPA6S"
    "S1EZMJ2YGGVBT8GVE9J5N3F8J7VEEPA8PC3MFA3FA1WNBX1P9KZXZBAS2YTV93JSPTMB2TNGNPVC31188NJBG6Z"
    "93BJNDQYFX4JBYE1A62Q2Z4C8HKTP4T4TA21VHXVG8KE20AGYWJWX9S0758ZW08S67B4CDQY95FJAHEM9BJ6DQF"
    "4E3PNFV2TBJJKNE89JFM26KRJVZ1ABYKF0R7XXPXKPF7FS4D787GN0YRRH6G5HYJZGQA65M5GV2FCF3JGBDYWDR"
    "ZXSM5C5M8";

// Same device, one hour of validity, already past by kNowExpiredMs.
const char* const kSignedExpired =
    "04NC27ZYJP5NYRY39W00G1900040AQ01QVKJGEN1P91SH47P64PZWR6F8N0159RRRMY94BCTKFM5P2ZSQ7YGADW"
    "DBWEE3V3V8PHFBFY1G6YGTC0Y0KXM97QR80XF24W074ZS70D554V806HM89Y3ZM9FAG3GPB1D93K571GSSADARD"
    "10EDQFWH1C6Y94VYDRZH0ADS5M380Y0AD2FPKWX5F2X74F73HH0X8DADVP6XRMT1ZXT5EVW8VYPZFA0RF9ZQEF6"
    "GHSN2HA20EWSTP4AK72VV69X791Z8MC8E8038A8GQXWR51KT5GG9T320G12FRQ1A77SCNCT9X6HCYVNKYZ4TV4A"
    "JGZRM6G4Y5TMHSBG9Q8Y41NTN3C2XG73985ZX274EN87YZT3754BBP6D4J548NQ7C542W6AM1K736Z9X46AC9ZE"
    "T60HYG4";

const char* const kShortValid = "2G40-A81A-R4FG-0J85-T2DG-YFAD";
const char* const kShortExpired = "2440-A81A-R4FG-12Q7-FKVQ-FABG";
const char* const kShortWrongDevice = "2G40-A80S-PSVG-185H-Z0EW-Q7QD";

}  // namespace

int main() {
  // ---- primitive vectors ----
  {
    const char* message = "abc";
    const usb_dlp::crypto::Digest digest = usb_dlp::crypto::sha256(
        reinterpret_cast<const std::uint8_t*>(message), 3);
    check(to_hex(digest.data(), digest.size()) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256 of \"abc\"");
  }
  {
    // RFC 4231 test case 2.
    const char* key = "Jefe";
    const char* data = "what do ya want for nothing?";
    const usb_dlp::crypto::Digest digest = usb_dlp::crypto::hmac_sha256(
        reinterpret_cast<const std::uint8_t*>(key), 4,
        reinterpret_cast<const std::uint8_t*>(data), 28);
    check(to_hex(digest.data(), digest.size()) ==
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
          "hmac-sha256 RFC 4231 case 2");
  }
  {
    const std::uint8_t payload[5] = {0x00, 0x01, 0x7F, 0x80, 0xFF};
    const std::string encoded = usb_dlp::crypto::base32_encode(payload, sizeof(payload));
    std::vector<std::uint8_t> decoded;
    const bool ok = usb_dlp::crypto::base32_decode(encoded, &decoded);
    check(ok && decoded.size() == sizeof(payload) &&
              std::equal(decoded.begin(), decoded.end(), payload),
          "base32 round trip");
  }

  // ---- device code ----
  {
    const std::string display = usb_dlp::device_code_display(kInstanceId);
    check(display == "5B0H-ZZMN-HDFP-7GTF", "device code fixture");
    check(usb_dlp::device_code_raw(kInstanceId).size() == usb_dlp::kDeviceCodeChars,
          "device code length");
  }

  // ---- signed code ----
  {
    usb_dlp::ApprovalGrant grant;
    usb_dlp::ApprovalError error = usb_dlp::ApprovalError::none;
    const bool ok = usb_dlp::verify_signed_approval_code(
        kSignedValid, public_modulus(), kInstanceId, kNowValidMs, &grant, &error);
    check(ok && error == usb_dlp::ApprovalError::none, "signed code accepted");
    check(ok && grant.duration_hours == 4 && grant.signed_code, "signed code grant fields");
    check(ok && grant.not_after_utc_ms - grant.not_before_utc_ms == 4LL * 3600 * 1000,
          "signed code validity window");
  }
  {
    usb_dlp::ApprovalError error = usb_dlp::ApprovalError::none;
    const bool ok = usb_dlp::verify_signed_approval_code(
        kSignedExpired, public_modulus(), kInstanceId, kNowExpiredMs, nullptr, &error);
    check(!ok && error == usb_dlp::ApprovalError::expired, "signed code rejected when expired");
  }
  {
    std::string tampered = kSignedValid;
    tampered[10] = tampered[10] == 'A' ? 'B' : 'A';
    usb_dlp::ApprovalError error = usb_dlp::ApprovalError::none;
    const bool ok = usb_dlp::verify_signed_approval_code(
        tampered, public_modulus(), kInstanceId, kNowValidMs, nullptr, &error);
    check(!ok, "tampered signed code rejected");
  }
  {
    usb_dlp::ApprovalError error = usb_dlp::ApprovalError::none;
    const bool ok = usb_dlp::verify_signed_approval_code(
        kSignedValid, public_modulus(), kOtherInstanceId, kNowValidMs, nullptr, &error);
    check(!ok && error == usb_dlp::ApprovalError::wrong_device,
          "signed code rejected for another device");
  }
  {
    usb_dlp::ApprovalError error = usb_dlp::ApprovalError::none;
    const bool ok = usb_dlp::verify_signed_approval_code(
        "NOT-A-CODE", public_modulus(), kInstanceId, kNowValidMs, nullptr, &error);
    check(!ok && error == usb_dlp::ApprovalError::invalid_format,
          "garbage rejected as invalid format");
  }

  // ---- short code ----
  {
    usb_dlp::ApprovalGrant grant;
    usb_dlp::ApprovalError error = usb_dlp::ApprovalError::none;
    const bool ok = usb_dlp::verify_short_approval_code(
        kShortValid, test_device_key(), kInstanceId, kNowValidMs, &grant, &error);
    check(ok && error == usb_dlp::ApprovalError::none, "short code accepted");
    check(ok && grant.duration_hours == 4 && !grant.signed_code, "short code grant fields");
  }
  {
    usb_dlp::ApprovalError error = usb_dlp::ApprovalError::none;
    const bool ok = usb_dlp::verify_short_approval_code(
        kShortExpired, test_device_key(), kInstanceId, kNowExpiredMs, nullptr, &error);
    check(!ok && error == usb_dlp::ApprovalError::expired, "short code rejected when expired");
  }
  {
    usb_dlp::ApprovalError error = usb_dlp::ApprovalError::none;
    const bool ok = usb_dlp::verify_short_approval_code(
        kShortWrongDevice, test_device_key(), kInstanceId, kNowValidMs, nullptr, &error);
    check(!ok && error == usb_dlp::ApprovalError::wrong_device,
          "short code rejected for another device");
  }
  {
    std::vector<std::uint8_t> wrong_key = test_device_key();
    wrong_key[0] = static_cast<std::uint8_t>(wrong_key[0] ^ 0x5A);
    usb_dlp::ApprovalError error = usb_dlp::ApprovalError::none;
    const bool ok = usb_dlp::verify_short_approval_code(
        kShortValid, wrong_key, kInstanceId, kNowValidMs, nullptr, &error);
    check(!ok && error == usb_dlp::ApprovalError::invalid_mac,
          "short code rejected under a different key");
  }
  {
    // Nine hours exceeds the short-code cap and must be refused even with a
    // correctly computed MAC.
    const std::vector<std::uint8_t> key = test_device_key();
    std::uint8_t payload[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    payload[0] = static_cast<std::uint8_t>((1 << 4) | 9);
    const std::int64_t minutes = (kNotBeforeMs - 1735689600000LL) / 60000;
    payload[1] = static_cast<std::uint8_t>((minutes >> 16) & 0xFF);
    payload[2] = static_cast<std::uint8_t>((minutes >> 8) & 0xFF);
    payload[3] = static_cast<std::uint8_t>(minutes & 0xFF);
    // Bind the payload to this device, otherwise the device check rejects it
    // before the duration cap is ever evaluated.
    const usb_dlp::crypto::Digest device_digest = usb_dlp::crypto::sha256(
        reinterpret_cast<const std::uint8_t*>(kInstanceId), std::strlen(kInstanceId));
    payload[4] = device_digest[0];
    payload[5] = device_digest[1];
    payload[6] = device_digest[2];
    const usb_dlp::crypto::Digest digest =
        usb_dlp::crypto::hmac_sha256(&key[0], key.size(), payload, sizeof(payload));
    std::uint8_t code_bytes[15];
    for (int index = 0; index < 8; ++index) code_bytes[index] = payload[index];
    for (int index = 0; index < 7; ++index) code_bytes[8 + index] = digest[static_cast<std::size_t>(index)];
    const std::string nine_hour_code = usb_dlp::crypto::base32_encode(code_bytes, sizeof(code_bytes));

    usb_dlp::ApprovalError error = usb_dlp::ApprovalError::none;
    const bool ok = usb_dlp::verify_short_approval_code(
        nine_hour_code, key, kInstanceId, kNowValidMs, nullptr, &error);
    check(!ok && error == usb_dlp::ApprovalError::duration_out_of_range,
          "short code rejects durations above 8 hours");
  }

  // ---- anti-replay fingerprint ----
  {
    const std::string grouped = usb_dlp::approval_code_fingerprint("2G40-A81A-R4FG-0J85-T2DG-YFAD");
    const std::string plain = usb_dlp::approval_code_fingerprint("2G40A81AR4FG0J85T2DGYFAD");
    check(grouped == plain && !grouped.empty(),
          "fingerprint ignores grouping so replays cannot dodge it");
  }

  if (g_failures == 0) {
    std::printf("Approval codec tests passed.\n");
    return 0;
  }
  std::printf("Approval codec tests failed: %d\n", g_failures);
  return 1;
}
