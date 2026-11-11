#pragma once

#include <cstdint>

namespace KvcMath {

inline constexpr double kPi = 3.14159265358979323846;

template <typename T>
constexpr const T& Min(const T& a, const T& b) noexcept {
    return (b < a) ? b : a;
}

template <typename T>
constexpr const T& Max(const T& a, const T& b) noexcept {
    return (a < b) ? b : a;
}

// sin(pi * x) using range-reduced Taylor series in Horner form.
// Accurate to machine precision for double, completely eliminates <cmath> and CRT.
inline double SinPi(double x) noexcept {
    const int64_t n = static_cast<int64_t>(x >= 0.0 ? x + 0.5 : x - 0.5);
    const double y = kPi * (x - static_cast<double>(n));
    const double y2 = y * y;
    double p = 1.0;
    for (int k = 11; k >= 1; --k) {
        p = 1.0 - y2 / static_cast<double>((2 * k) * (2 * k + 1)) * p;
    }
    return (n & 1) ? -y * p : y * p;
}

} // namespace KvcMath
