#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace usb_dlp {
namespace crypto {

// Self-contained primitives for the offline approval channel.
//
// The Windows CryptoAPI cannot be used uniformly here: XP only offers RSA with
// SHA-1, so a SHA-256 PKCS#1 v1.5 verification would either be impossible or
// silently weaker on the oldest supported platform. These implementations are
// plain algorithms with no OS dependency, so every profile - XP through Win11 -
// verifies exactly the same bytes.

using Digest = std::array<std::uint8_t, 32>;

Digest sha256(const std::uint8_t* data, std::size_t size);
Digest hmac_sha256(const std::uint8_t* key, std::size_t key_size,
                   const std::uint8_t* data, std::size_t size);

// Crockford Base32: the alphabet omits I, L, O and U so a code can be read out
// loud and typed back without ambiguity. Decoding additionally folds i/l to 1
// and o to 0, and ignores hyphens and spaces.
std::string base32_encode(const std::uint8_t* data, std::size_t size);
bool base32_decode(const std::string& text, std::vector<std::uint8_t>* out);

// Groups a code for display, e.g. ABCD-EFGH-IJKM.
std::string base32_group(const std::string& raw, std::size_t group_size);

// Verifies an RSA PKCS#1 v1.5 signature over SHA-256(message).
// `modulus` and `signature` are big-endian byte strings, both the same length
// (256 bytes for a 2048-bit key). Only the public operation is performed, so
// no private material is needed - or trusted - on the endpoint.
bool rsa_verify_sha256(const std::vector<std::uint8_t>& modulus,
                       const std::uint8_t* signature,
                       std::size_t signature_size,
                       const std::uint8_t* message,
                       std::size_t message_size);

// Constant-time comparison for MACs and digests.
bool equal_constant_time(const std::uint8_t* left, const std::uint8_t* right, std::size_t size);

}  // namespace crypto
}  // namespace usb_dlp
