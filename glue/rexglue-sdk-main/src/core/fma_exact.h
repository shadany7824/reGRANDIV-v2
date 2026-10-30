/**
 * @file        core/fma_exact.h
 * @brief       [web port] Correctly rounded a * b + c without a fused instruction
 *
 * Boldo and Melquiond, "Emulation of FMA and correctly rounded sums: proved
 * algorithms using rounding to odd" (IEEE Trans. Computers, 2008): with
 * the product split exactly in two (Dekker) and an exact sum (Knuth's
 * 2Sum), rounding the sum of the two low parts to odd and then adding the
 * high part rounds once, as a fused multiply-add does. That holds while
 * nothing overflows or underflows, which FmaExactFast checks first; the
 * other cases (zeros, subnormals, infinities, NaN, extreme exponents) return
 * false for the caller to take a full software fma.
 *
 * Needs plain IEEE double arithmetic: compile without contraction
 * (-ffp-contract=off), or a * b + c here could become the very fma it
 * replaces.
 *
 * @license     BSD 3-Clause License
 */

#pragma once

#include <cstdint>
#include <cstring>

namespace rex::core {

inline uint64_t FmaBits(double x) {
  uint64_t u;
  std::memcpy(&u, &x, 8);
  return u;
}
inline double FmaFromBits(uint64_t u) {
  double x;
  std::memcpy(&x, &u, 8);
  return x;
}

// RN(a * b + c) into `out`, or false when the inputs need the full path.
inline bool FmaExactFast(double a, double b, double c, double* out) {
  const int ea = int((FmaBits(a) >> 52) & 0x7FF) - 1023;
  const int eb = int((FmaBits(b) >> 52) & 0x7FF) - 1023;
  const uint64_t cbits = FmaBits(c) & 0x7FFFFFFFFFFFFFFFull;
  const int ec = int(cbits >> 52) - 1023;
  // a and b normal, small enough to split (|x| * (2^27 + 1) finite), and a
  // product whose low part cannot underflow (2^-1074 below 106 bits of it)
  // or whose sum with c could overflow; c zero or comfortably normal.
  if (ea < -990 || ea > 990 || eb < -990 || eb > 990) return false;
  if (ea + eb < -960 || ea + eb > 1000) return false;
  if (cbits && (ec < -920 || ec > 1000)) return false;
  // The product exactly: p + e (Dekker's split, 2^27 + 1).
  constexpr double kSplit = 134217729.0;
  const double p = a * b;
  const double ta = kSplit * a, ah = ta - (ta - a), al = a - ah;
  const double tb = kSplit * b, bh = tb - (tb - b), bl = b - bh;
  const double e = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
  // c + p exactly: th + tl (2Sum).
  const double th = c + p;
  const double bv = th - c;
  const double tl = (c - (th - bv)) + (p - bv);
  // tl + e rounded to odd: rounded to nearest, then, when that was inexact
  // and landed on an even significand, one step towards the exact sum.
  double v = tl + e;
  const double vb = v - tl;
  const double verr = (tl - (v - vb)) + (e - vb);
  if (verr != 0 && !(FmaBits(v) & 1)) {
    v = FmaFromBits(FmaBits(v) + ((verr > 0) == (v > 0) ? 1 : -1));
  }
  *out = th + v;
  return true;
}

}  // namespace rex::core
