#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include <util/expected.h>

namespace util {

// Serialise a JSON value in the canonical form of RFC 8785 (JSON
// Canonicalization Scheme, JCS):
//  - object keys are sorted by their UTF-16 code units;
//  - no whitespace;
//  - strings use the minimal escapes of ECMAScript JSON.stringify;
//  - numbers use the ECMAScript Number serialisation.
//
// Only integers in [-2^53, 2^53] are accepted as numbers: uenv never writes a
// floating point number, and an integer outside that range has no exact
// representation as an IEEE 754 double, which is what JCS serialises.
util::expected<std::string, std::string> jcs(const nlohmann::json& value);

} // namespace util
