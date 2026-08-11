#include "base64.hpp"

#include <base64.h>

namespace aoi {

namespace {

bool isValidBase64(const std::string& in) {
  // RFC 4648 alphabet + trailing '=' padding only. Whitespace is not accepted
  // by the underlying decoder (b64_lookup returns 255 for it, silently
  // corrupting output), so reject it here — a corrupt TTS chunk must be
  // skipped, not played as a pop.
  size_t eq = std::string::npos;
  for (size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    const bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                       (c >= '0' && c <= '9') || c == '+' || c == '/';
    if (c == '=') {
      if (eq == std::string::npos) eq = i;
      continue;  // allow multiple trailing '='
    }
    if (!alpha) return false;  // any non-alphabet non-padding char is invalid
    if (eq != std::string::npos) return false;  // data after '=' is invalid
  }
  if (eq != std::string::npos && in.size() - eq > 2) return false;  // max 2 padding
  if (in.empty()) return false;
  // Length sanity (RFC 4648): total length must be a multiple of 4 when
  // padded; unpadded lengths ≡1 (mod 4) encode no bytes and always indicate
  // corruption (the underlying decoder would silently truncate them).
  const size_t dataLen = eq == std::string::npos ? in.size() : eq;
  if (eq == std::string::npos) {
    if (in.size() % 4 == 1) return false;
  } else {
    if (in.size() % 4 != 0) return false;
  }
  // Padding must not exceed the data it pads (a pure "=" / "==" input would
  // make the decoder's length math underflow to SIZE_MAX).
  if (eq != std::string::npos && in.size() - eq >= dataLen) return false;
  return true;
}

} // namespace

std::string base64Encode(const unsigned char* data, size_t len) {
  std::string out;
  const std::string in(reinterpret_cast<const char*>(data), len);
  Base64::Encode(in, &out);
  return out;
}

bool base64Decode(const std::string& in, std::vector<uint8_t>& out) {
  if (!isValidBase64(in)) return false;
  std::string decoded;
  if (!Base64::Decode(in, &decoded)) return false;
  out.assign(decoded.begin(), decoded.end());
  return true;
}

} // namespace aoi
