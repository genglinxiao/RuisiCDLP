#pragma once

#include <cstdint>

namespace usb_dlp {

// Big-endian RSA-2048 modulus compiled into the agent. The private key lives
// only with IT; verifying a code needs nothing else.
const std::uint8_t* approval_modulus();

}  // namespace usb_dlp
