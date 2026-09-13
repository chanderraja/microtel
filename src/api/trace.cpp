// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// src/api/ — definitions behind the public API headers. Currently the hex
// formatters on the trace identifier types declared in
// include/microtel/trace.hpp.

#include "microtel/trace.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace microtel
{

namespace
{

constexpr unsigned int kNibbleShift = 4U;
constexpr unsigned int kLowNibbleMask = 0x0FU;
constexpr unsigned int kDecimalDigitCount = 10U;
constexpr std::size_t kHexCharsPerByte = 2U;

/// @brief Render one hex nibble (0–15) as its lower-case character.
[[nodiscard]] constexpr char NibbleToHex(unsigned int nibble) noexcept
{
    return nibble < kDecimalDigitCount ? static_cast<char>('0' + nibble)
                                       : static_cast<char>('a' + (nibble - kDecimalDigitCount));
}

/// @brief Render a fixed-width byte array as lower-case hex, no separators.
///
/// Two chars per byte in array order, so the width is fixed even for an
/// all-zero (invalid) id: that is the W3C `traceparent` and OTLP protojson
/// encoding a collector expects.
template <std::size_t N>
[[nodiscard]] std::string BytesToLowerHex(const std::array<std::uint8_t, N>& bytes)
{
    std::string out;
    out.reserve(N * kHexCharsPerByte);
    for (const std::uint8_t element : bytes)
    {
        const auto byte = static_cast<unsigned int>(element);
        out.push_back(NibbleToHex(byte >> kNibbleShift));
        out.push_back(NibbleToHex(byte & kLowNibbleMask));
    }
    return out;
}

}  // namespace

std::string TraceId::ToHex() const
{
    return BytesToLowerHex(m_bytes);
}

std::string SpanId::ToHex() const
{
    return BytesToLowerHex(m_bytes);
}

}  // namespace microtel
