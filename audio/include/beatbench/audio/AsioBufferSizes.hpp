// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace beatbench::audio {

/// Native ASIO frame limits, as returned by PaAsio_GetAvailableBufferSizes.
struct AsioBufferSizes {
    long min = 0;
    long max = 0;
    long preferred = 0;
    long granularity = 0; ///< 0: fixed; -1: powers of two; >0: steps from min.
};

namespace detail {
constexpr bool asioPowerOfTwo(long value) noexcept {
    return value > 0 && (value & (value - 1)) == 0;
}
constexpr bool asioSizeInRange(const AsioBufferSizes& sizes, long value) noexcept {
    if (value < sizes.min || value > sizes.max) return false;
    if (sizes.granularity == 0) return value == sizes.preferred;
    if (sizes.granularity == -1) return asioPowerOfTwo(value);
    // Both operands are positive and ordered: subtraction cannot overflow.
    return (value - sizes.min) % sizes.granularity == 0;
}
} // namespace detail

/// Reject malformed driver limits; no rounding, shifts, allocation or device access.
constexpr bool validAsioBufferSizes(const AsioBufferSizes& sizes) noexcept {
    if (sizes.min <= 0 || sizes.max < sizes.min || sizes.granularity < -1)
        return false;
    if (sizes.granularity == 0)
        return sizes.min == sizes.max && sizes.preferred == sizes.min;
    if (sizes.granularity == -1 &&
        (!detail::asioPowerOfTwo(sizes.min) || !detail::asioPowerOfTwo(sizes.max)))
        return false;
    return detail::asioSizeInRange(sizes, sizes.preferred);
}

/// Resolve 0 to preferred; return 0 for an invalid request or malformed limits.
/// A positive result is a legal request, NOT a measurement of the native stream buffer.
constexpr long resolveAsioBufferSize(const AsioBufferSizes& sizes, long requested) noexcept {
    if (requested < 0 || !validAsioBufferSizes(sizes)) return 0;
    const long value = requested == 0 ? sizes.preferred : requested;
    return detail::asioSizeInRange(sizes, value) ? value : 0;
}

} // namespace beatbench::audio
