#include "usb_dlp/crypto.h"

#include <cstring>

namespace usb_dlp {
namespace crypto {
namespace {

const std::uint32_t kSha256Round[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

inline std::uint32_t rotr(std::uint32_t value, int bits) {
  return (value >> bits) | (value << (32 - bits));
}

void sha256_block(std::uint32_t* state, const std::uint8_t* block) {
  std::uint32_t w[64];
  for (int index = 0; index < 16; ++index) {
    w[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) |
               (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[index * 4 + 3]);
  }
  for (int index = 16; index < 64; ++index) {
    const std::uint32_t s0 =
        rotr(w[index - 15], 7) ^ rotr(w[index - 15], 18) ^ (w[index - 15] >> 3);
    const std::uint32_t s1 =
        rotr(w[index - 2], 17) ^ rotr(w[index - 2], 19) ^ (w[index - 2] >> 10);
    w[index] = w[index - 16] + s0 + w[index - 7] + s1;
  }

  std::uint32_t a = state[0];
  std::uint32_t b = state[1];
  std::uint32_t c = state[2];
  std::uint32_t d = state[3];
  std::uint32_t e = state[4];
  std::uint32_t f = state[5];
  std::uint32_t g = state[6];
  std::uint32_t h = state[7];

  for (int index = 0; index < 64; ++index) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + ch + kSha256Round[index] + w[index];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

// ---------------------------------------------------------------------------
// Fixed-width big integer helpers for the RSA public operation.
// 2048-bit numbers are held as 64 little-endian 32-bit words.
// ---------------------------------------------------------------------------

const int kWords = 64;
const int kDoubleWords = 128;

void bn_from_be(const std::uint8_t* bytes, std::size_t size, std::uint32_t* out) {
  for (int index = 0; index < kWords; ++index) out[index] = 0;
  for (std::size_t index = 0; index < size; ++index) {
    const std::size_t byte_from_end = size - 1 - index;
    const int word = static_cast<int>(byte_from_end / 4);
    if (word >= kWords) continue;
    const int shift = static_cast<int>(byte_from_end % 4) * 8;
    out[word] |= static_cast<std::uint32_t>(bytes[index]) << shift;
  }
}

void bn_to_be(const std::uint32_t* in, std::uint8_t* out, std::size_t size) {
  for (std::size_t index = 0; index < size; ++index) {
    const std::size_t byte_from_end = size - 1 - index;
    const int word = static_cast<int>(byte_from_end / 4);
    const int shift = static_cast<int>(byte_from_end % 4) * 8;
    out[index] = word < kWords ? static_cast<std::uint8_t>((in[word] >> shift) & 0xFFU) : 0;
  }
}

int bn_compare(const std::uint32_t* left, const std::uint32_t* right, int words) {
  for (int index = words - 1; index >= 0; --index) {
    if (left[index] != right[index]) return left[index] > right[index] ? 1 : -1;
  }
  return 0;
}

void bn_subtract(std::uint32_t* left, const std::uint32_t* right, int words) {
  std::uint64_t borrow = 0;
  for (int index = 0; index < words; ++index) {
    const std::uint64_t current = static_cast<std::uint64_t>(left[index]) - right[index] - borrow;
    left[index] = static_cast<std::uint32_t>(current & 0xFFFFFFFFULL);
    borrow = (current >> 32) != 0 ? 1ULL : 0ULL;
  }
}

void bn_multiply(const std::uint32_t* left, const std::uint32_t* right, std::uint32_t* out) {
  for (int index = 0; index < kDoubleWords; ++index) out[index] = 0;
  for (int i = 0; i < kWords; ++i) {
    std::uint64_t carry = 0;
    for (int j = 0; j < kWords; ++j) {
      const std::uint64_t current = static_cast<std::uint64_t>(left[i]) * right[j] +
                                    out[i + j] + carry;
      out[i + j] = static_cast<std::uint32_t>(current & 0xFFFFFFFFULL);
      carry = current >> 32;
    }
    out[i + kWords] = static_cast<std::uint32_t>(carry & 0xFFFFFFFFULL);
  }
}

// Reduces a 2n-word value modulo an n-word modulus using bitwise long division.
// The public exponent is 65537, so only 17 modular squarings run per verify and
// the simple algorithm is fast enough.
//
// The running remainder needs one word more than the modulus: shifting it left
// before the comparison can set bit n, and with exactly n words that bit would
// be shifted off the top and the reduction would produce a wrong result.
void bn_reduce(const std::uint32_t* value, const std::uint32_t* modulus, std::uint32_t* out) {
  const int words = kWords + 1;
  std::uint32_t wide_modulus[kWords + 1];
  for (int index = 0; index < kWords; ++index) wide_modulus[index] = modulus[index];
  wide_modulus[kWords] = 0;

  std::uint32_t remainder[kWords + 1];
  for (int index = 0; index < words; ++index) remainder[index] = 0;
  for (int bit = kDoubleWords * 32 - 1; bit >= 0; --bit) {
    std::uint32_t carry = 0;
    for (int index = 0; index < words; ++index) {
      const std::uint32_t next = remainder[index] >> 31;
      remainder[index] = (remainder[index] << 1) | carry;
      carry = next;
    }
    const int word = bit / 32;
    const int shift = bit % 32;
    remainder[0] |= (value[word] >> shift) & 1U;
    if (bn_compare(remainder, wide_modulus, words) >= 0) {
      bn_subtract(remainder, wide_modulus, words);
    }
  }
  for (int index = 0; index < kWords; ++index) out[index] = remainder[index];
}

void bn_multiply_mod(const std::uint32_t* left, const std::uint32_t* right,
                     const std::uint32_t* modulus, std::uint32_t* out) {
  std::uint32_t product[kDoubleWords];
  bn_multiply(left, right, product);
  bn_reduce(product, modulus, out);
}

int bn_bit(const std::uint32_t* value, int bit) {
  return static_cast<int>((value[bit / 32] >> (bit % 32)) & 1U);
}

void bn_mod_exp(const std::uint32_t* base, const std::uint32_t* exponent,
                const std::uint32_t* modulus, std::uint32_t* out) {
  std::uint32_t result[kWords];
  for (int index = 0; index < kWords; ++index) result[index] = 0;
  result[0] = 1;
  int top = 0;
  for (int bit = kWords * 32 - 1; bit >= 0; --bit) {
    if (bn_bit(exponent, bit) != 0) {
      top = bit;
      break;
    }
  }
  for (int bit = top; bit >= 0; --bit) {
    bn_multiply_mod(result, result, modulus, result);
    if (bn_bit(exponent, bit) != 0) bn_multiply_mod(result, base, modulus, result);
  }
  for (int index = 0; index < kWords; ++index) out[index] = result[index];
}

// DigestInfo prefix for SHA-256 inside a PKCS#1 v1.5 signature.
const std::uint8_t kSha256DigestInfoPrefix[19] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60,
                                                  0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02,
                                                  0x01, 0x05, 0x00, 0x04, 0x20};

}  // namespace

Digest sha256(const std::uint8_t* data, std::size_t size) {
  std::uint32_t state[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                            0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::size_t offset = 0;
  while (size - offset >= 64) {
    sha256_block(state, data + offset);
    offset += 64;
  }

  std::uint8_t tail[128];
  const std::size_t remaining = size - offset;
  if (remaining > 0) std::memcpy(tail, data + offset, remaining);
  tail[remaining] = 0x80;
  const std::size_t padded = remaining < 56 ? 64 : 128;
  for (std::size_t index = remaining + 1; index < padded; ++index) tail[index] = 0;
  const std::uint64_t bit_length = static_cast<std::uint64_t>(size) * 8U;
  for (int index = 0; index < 8; ++index) {
    tail[padded - 1 - static_cast<std::size_t>(index)] =
        static_cast<std::uint8_t>((bit_length >> (index * 8)) & 0xFFU);
  }
  sha256_block(state, tail);
  if (padded == 128) sha256_block(state, tail + 64);

  Digest result = {};
  for (int index = 0; index < 8; ++index) {
    result[index * 4] = static_cast<std::uint8_t>((state[index] >> 24) & 0xFFU);
    result[index * 4 + 1] = static_cast<std::uint8_t>((state[index] >> 16) & 0xFFU);
    result[index * 4 + 2] = static_cast<std::uint8_t>((state[index] >> 8) & 0xFFU);
    result[index * 4 + 3] = static_cast<std::uint8_t>(state[index] & 0xFFU);
  }
  return result;
}

Digest hmac_sha256(const std::uint8_t* key, std::size_t key_size,
                   const std::uint8_t* data, std::size_t size) {
  std::uint8_t normalized[32];
  const std::uint8_t* effective = key;
  std::size_t effective_size = key_size;
  if (key_size > 64) {
    const Digest hashed = sha256(key, key_size);
    std::memcpy(normalized, hashed.data(), hashed.size());
    effective = normalized;
    effective_size = 32;
  }

  std::uint8_t inner[64];
  std::uint8_t outer[64];
  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint8_t value = index < effective_size ? effective[index] : 0;
    inner[index] = static_cast<std::uint8_t>(value ^ 0x36U);
    outer[index] = static_cast<std::uint8_t>(value ^ 0x5CU);
  }

  std::vector<std::uint8_t> inner_input;
  inner_input.reserve(64 + size);
  inner_input.insert(inner_input.end(), inner, inner + 64);
  inner_input.insert(inner_input.end(), data, data + size);
  const Digest inner_hash = sha256(inner_input.empty() ? nullptr : &inner_input[0],
                                    inner_input.size());

  std::uint8_t outer_input[96];
  std::memcpy(outer_input, outer, 64);
  std::memcpy(outer_input + 64, inner_hash.data(), inner_hash.size());
  return sha256(outer_input, sizeof(outer_input));
}

std::string base32_encode(const std::uint8_t* data, std::size_t size) {
  static const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
  std::string result;
  result.reserve((size * 8 + 4) / 5 + 1);
  std::uint32_t buffer = 0;
  int bits = 0;
  for (std::size_t index = 0; index < size; ++index) {
    buffer = (buffer << 8) | data[index];
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      result.push_back(kAlphabet[(buffer >> bits) & 0x1FU]);
    }
  }
  if (bits > 0) result.push_back(kAlphabet[(buffer << (5 - bits)) & 0x1FU]);
  return result;
}

bool base32_decode(const std::string& text, std::vector<std::uint8_t>* out) {
  // The alphabet skips I, L, O and U, so a character's value cannot be derived
  // arithmetically from its ASCII code - it has to be looked up. Anyone who
  // computes 'Q' - 'A' + 10 lands on 26 instead of 23.
  static const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
  out->clear();
  std::uint32_t buffer = 0;
  int bits = 0;
  for (std::size_t index = 0; index < text.size(); ++index) {
    char character = text[index];
    // Codes arrive from a multi-line edit control, a text message or a phone
    // call, so every form of whitespace is stripped rather than just spaces.
    // A trailing \r\n used to reject an otherwise valid code outright.
    if (character == '-' || character == ' ' || character == '\t' || character == '\r' ||
        character == '\n') {
      continue;
    }
    if (character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
    // Crockford folds these look-alikes so a mistyped code still verifies.
    if (character == 'I' || character == 'L') character = '1';
    if (character == 'O') character = '0';
    const char* position = std::strchr(kAlphabet, character);
    if (position == nullptr) return false;
    const int value = static_cast<int>(position - kAlphabet);
    buffer = (buffer << 5) | static_cast<std::uint32_t>(value);
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFFU));
    }
  }
  return true;
}

std::string base32_group(const std::string& raw, std::size_t group_size) {
  if (group_size == 0) return raw;
  std::string result;
  for (std::size_t index = 0; index < raw.size(); ++index) {
    if (index > 0 && index % group_size == 0) result.push_back('-');
    result.push_back(raw[index]);
  }
  return result;
}

bool equal_constant_time(const std::uint8_t* left, const std::uint8_t* right, std::size_t size) {
  std::uint8_t difference = 0;
  for (std::size_t index = 0; index < size; ++index) {
    difference = static_cast<std::uint8_t>(difference | (left[index] ^ right[index]));
  }
  return difference == 0;
}

bool rsa_verify_sha256(const std::vector<std::uint8_t>& modulus,
                       const std::uint8_t* signature,
                       std::size_t signature_size,
                       const std::uint8_t* message,
                       std::size_t message_size) {
  if (modulus.size() != 256 || signature == nullptr || signature_size != 256) return false;

  std::uint32_t n[kWords];
  std::uint32_t s[kWords];
  bn_from_be(&modulus[0], modulus.size(), n);
  bn_from_be(signature, signature_size, s);

  // Reject a signature that is not smaller than the modulus; it cannot be a
  // valid RSA value and would otherwise be reduced silently.
  if (bn_compare(s, n, kWords) >= 0) return false;

  std::uint32_t e[kWords];
  for (int index = 0; index < kWords; ++index) e[index] = 0;
  e[0] = 65537U;

  std::uint32_t recovered[kWords];
  bn_mod_exp(s, e, n, recovered);

  std::uint8_t encoded[256];
  bn_to_be(recovered, encoded, sizeof(encoded));

  // PKCS#1 v1.5: 0x00 0x01 0xFF..FF 0x00 || DigestInfo || digest
  // The count includes the single 0x00 separator that precedes DigestInfo.
  constexpr std::size_t kDigestSize = 32;
  constexpr std::size_t kPrefixSize = sizeof(kSha256DigestInfoPrefix);
  constexpr std::size_t kSeparator = 256 - kDigestSize - kPrefixSize - 1;
  static_assert(kSeparator >= 10, "PKCS#1 v1.5 needs at least eight padding bytes");
  const std::size_t separator = kSeparator;
  if (encoded[0] != 0x00 || encoded[1] != 0x01) return false;
  for (std::size_t index = 2; index < separator; ++index) {
    if (encoded[index] != 0xFF) return false;
  }
  if (encoded[separator] != 0x00) return false;
  if (std::memcmp(encoded + separator + 1, kSha256DigestInfoPrefix, kPrefixSize) != 0) return false;

  const Digest digest = sha256(message, message_size);
  return equal_constant_time(encoded + separator + 1 + kPrefixSize, digest.data(), kDigestSize);
}

}  // namespace crypto
}  // namespace usb_dlp
