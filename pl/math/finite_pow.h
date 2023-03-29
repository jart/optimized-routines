/*
 * Double-precision x^y function.
 *
 * Copyright (c) 2018-2023, Arm Limited.
 * SPDX-License-Identifier: MIT OR Apache-2.0 WITH LLVM-exception
 */

#include "math_config.h"

/* Scalar version of pow used for fallbacks for vector implementations.
   This is almost a copy-paste of the math/pow.c algorithm, where we kept only
   round to nearest mode and removed: exception/errno handling, tricks for
   extra accuracy in subnormal range.  */

/* Data is defined in pl/math/v_pow_log_data.c.  */
#define INVC __v_pow_log_data.invc
#define LOGC __v_pow_log_data.logc
#define LOGCTAIL __v_pow_log_data.logctail
#define A __v_pow_log_data.poly
#define Ln2hi __v_pow_log_data.ln2hi
#define Ln2lo __v_pow_log_data.ln2lo
#define N_LOG (1 << V_POW_LOG_TABLE_BITS)
#define OFF 0x3fe6955500000000

/* Data is defined in pl/math/v_pow_exp_data.c.  */
#define N_EXP (1 << V_POW_EXP_TABLE_BITS)
#define InvLn2N __v_pow_exp_data.invln2N
#define NegLn2hiN __v_pow_exp_data.negln2hiN
#define NegLn2loN __v_pow_exp_data.negln2loN
#define Shift __v_pow_exp_data.shift
#define SBits __v_pow_exp_data.sbits
#define C2 __v_pow_exp_data.poly[5 - V_POW_EXP_POLY_ORDER]
#define C3 __v_pow_exp_data.poly[6 - V_POW_EXP_POLY_ORDER]
#define C4 __v_pow_exp_data.poly[7 - V_POW_EXP_POLY_ORDER]
#define C5 __v_pow_exp_data.poly[8 - V_POW_EXP_POLY_ORDER]

/* Top 12 bits of a double (sign and exponent bits).  */
static inline uint32_t
top12 (double x)
{
  return asuint64 (x) >> 52;
}

/* Compute y+TAIL = log(x) where the rounded result is y and TAIL has about
   additional 15 bits precision.  IX is the bit representation of x, but
   normalized in the subnormal range using the sign bit for the exponent.  */
static inline double
log_inline (uint64_t ix, double *tail)
{
  /* x = 2^k z; where z is in range [OFF,2*OFF) and exact.
     The range is split into N subintervals.
     The ith subinterval contains z and c is near its center.  */
  uint64_t tmp = ix - OFF;
  int i = (tmp >> (52 - V_POW_LOG_TABLE_BITS)) & (N_LOG - 1);
  int k = (int64_t) tmp >> 52; /* arithmetic shift.  */
  uint64_t iz = ix - (tmp & 0xfffULL << 52);
  double z = asdouble (iz);
  double kd = (double) k;

  /* log(x) = k*Ln2 + log(c) + log1p(z/c-1).  */
  double invc = INVC[i];
  double logc = LOGC[i];
  double logctail = LOGCTAIL[i];

  /* Note: 1/c is j/N or j/N/2 where j is an integer in [N,2N) and
     |z/c - 1| < 1/N, so r = z/c - 1 is exactly representible.  */
  double r = fma (z, invc, -1.0);

  /* k*Ln2 + log(c) + r.  */
  double t1 = kd * Ln2hi + logc;
  double t2 = t1 + r;
  double lo1 = kd * Ln2lo + logctail;
  double lo2 = t1 - t2 + r;

  /* Evaluation is optimized assuming superscalar pipelined execution.  */
  double ar = A[0] * r; /* A[0] = -0.5.  */
  double ar2 = r * ar;
  double ar3 = r * ar2;
  /* k*Ln2 + log(c) + r + A[0]*r*r.  */
  double hi = t2 + ar2;
  double lo3 = fma (ar, r, -ar2);
  double lo4 = t2 - hi + ar2;
  /* p = log1p(r) - r - A[0]*r*r.  */
  double p
    = (ar3
       * (A[1] + r * A[2] + ar2 * (A[3] + r * A[4] + ar2 * (A[5] + r * A[6]))));
  double lo = lo1 + lo2 + lo3 + lo4 + p;
  double y = hi + lo;
  *tail = hi - y + lo;
  return y;
}

/* Handle cases that may overflow or underflow when computing the result that
   is scale*(1+TMP) without intermediate rounding.  The bit representation of
   scale is in SBITS, however it has a computed exponent that may have
   overflown into the sign bit so that needs to be adjusted before using it as
   a double.  (int32_t)KI is the k used in the argument reduction and exponent
   adjustment of scale, positive k here means the result may overflow and
   negative k means the result may underflow.  */
static inline double
specialcase (double tmp, uint64_t sbits, uint64_t ki)
{
  double scale, y;

  if ((ki & 0x80000000) == 0)
    {
      /* k > 0, the exponent of scale might have overflowed by <= 460.  */
      sbits -= 1009ull << 52;
      scale = asdouble (sbits);
      y = 0x1p1009 * (scale + scale * tmp);
      return check_oflow (eval_as_double (y));
    }
  /* k < 0, need special care in the subnormal range.  */
  sbits += 1022ull << 52;
  /* Note: sbits is signed scale.  */
  scale = asdouble (sbits);
  y = scale + scale * tmp;
#if WANT_SIMD_EXCEPT
  if (fabs (y) < 1.0)
    {
      /* Round y to the right precision before scaling it into the subnormal
	 range to avoid double rounding that can cause 0.5+E/2 ulp error where
	 E is the worst-case ulp error outside the subnormal range.  So this
	 is only useful if the goal is better than 1 ulp worst-case error.  */
      double hi, lo, one = 1.0;
      if (y < 0.0)
	one = -1.0;
      lo = scale - y + scale * tmp;
      hi = one + y;
      lo = one - hi + y + lo;
      y = eval_as_double (hi + lo) - one;
      /* Fix the sign of 0.  */
      if (y == 0.0)
	y = asdouble (sbits & 0x8000000000000000);
      /* The underflow exception needs to be signaled explicitly.  */
      force_eval_double (opt_barrier_double (0x1p-1022) * 0x1p-1022);
    }
#endif
  y = 0x1p-1022 * y;
  return check_uflow (eval_as_double (y));
}

#define SIGN_BIAS (0x800 << V_POW_EXP_TABLE_BITS)

/* Computes sign*exp(x+xtail) where |xtail| < 2^-8/N and |xtail| <= |x|.
   The sign_bias argument is SIGN_BIAS or 0 and sets the sign to -1 or 1.  */
static inline double
exp_inline (double x, double xtail, uint32_t sign_bias)
{
  uint32_t abstop = top12 (x) & 0x7ff;
  if (unlikely (abstop - top12 (0x1p-54) >= top12 (512.0) - top12 (0x1p-54)))
    {
      if (abstop - top12 (0x1p-54) >= 0x80000000)
	{
	  /* Avoid spurious underflow for tiny x.  */
	  /* Note: 0 is common input.  */
	  return sign_bias ? -1.0 : 1.0;
	}
      if (abstop >= top12 (1024.0))
	{
	  /* Note: inf and nan are already handled.  */
	  /* Skip errno handling.  */
#if WANT_SIMD_EXCEPT
	  return asuint64 (x) >> 63 ? __math_uflow (sign_bias)
				    : __math_oflow (sign_bias);
#else
	  double res_uoflow = asuint64 (x) >> 63 ? 0.0 : INFINITY;
	  return sign_bias ? -res_uoflow : res_uoflow;
#endif
	}
      /* Large x is special cased below.  */
      abstop = 0;
    }

  /* exp(x) = 2^(k/N) * exp(r), with exp(r) in [2^(-1/2N),2^(1/2N)].  */
  /* x = ln2/N*k + r, with int k and r in [-ln2/2N, ln2/2N].  */
  double z = InvLn2N * x;
  double kd = roundtoint (z);
  uint64_t ki = converttoint (z);
  double r = x + kd * NegLn2hiN + kd * NegLn2loN;
  /* The code assumes 2^-200 < |xtail| < 2^-8/N.  */
  r += xtail;
  /* 2^(k/N) ~= scale.  */
  uint64_t idx = ki & (N_EXP - 1);
  uint64_t top = (ki + sign_bias) << (52 - V_POW_EXP_TABLE_BITS);
  /* This is only a valid scale when -1023*N < k < 1024*N.  */
  uint64_t sbits = SBits[idx] + top;
  /* exp(x) = 2^(k/N) * exp(r) ~= scale + scale * (exp(r) - 1).  */
  /* Evaluation is optimized assuming superscalar pipelined execution.  */
  double r2 = r * r;
  double tmp = r + r2 * C2 + r * r2 * (C3 + r * C4);
  if (unlikely (abstop == 0))
    return specialcase (tmp, sbits, ki);
  double scale = asdouble (sbits);
  /* Note: tmp == 0 or |tmp| > 2^-200 and scale > 2^-739, so there
     is no spurious underflow here even without fma.  */
  return eval_as_double (scale + scale * tmp);
}

/* Returns 0 if not int, 1 if odd int, 2 if even int.  The argument is
   the bit representation of a non-zero finite floating-point value.  */
static inline int
checkint (uint64_t iy)
{
  int e = iy >> 52 & 0x7ff;
  if (e < 0x3ff)
    return 0;
  if (e > 0x3ff + 52)
    return 2;
  if (iy & ((1ULL << (0x3ff + 52 - e)) - 1))
    return 0;
  if (iy & (1ULL << (0x3ff + 52 - e)))
    return 1;
  return 2;
}

/* Returns 1 if input is the bit representation of 0, infinity or nan.  */
static inline int
zeroinfnan (uint64_t i)
{
  return 2 * i - 1 >= 2 * asuint64 (INFINITY) - 1;
}

static NOINLINE double
__pl_finite_pow (double x, double y)
{
  uint32_t sign_bias = 0;
  uint64_t ix, iy;
  uint32_t topx, topy;

  ix = asuint64 (x);
  iy = asuint64 (y);
  topx = top12 (x);
  topy = top12 (y);
  if (unlikely (topx - 0x001 >= 0x7ff - 0x001
		|| (topy & 0x7ff) - 0x3be >= 0x43e - 0x3be))
    {
      /* Note: if |y| > 1075 * ln2 * 2^53 ~= 0x1.749p62 then pow(x,y) = inf/0
	 and if |y| < 2^-54 / 1075 ~= 0x1.e7b6p-65 then pow(x,y) = +-1.  */
      /* Special cases: (x < 0x1p-126 or inf or nan) or
	 (|y| < 0x1p-65 or |y| >= 0x1p63 or nan).  */
      if (unlikely (zeroinfnan (iy)))
	{
	  if (2 * iy == 0)
	    return issignaling_inline (x) ? x + y : 1.0;
	  if (ix == asuint64 (1.0))
	    return issignaling_inline (y) ? x + y : 1.0;
	  if (2 * ix > 2 * asuint64 (INFINITY)
	      || 2 * iy > 2 * asuint64 (INFINITY))
	    return x + y;
	  if (2 * ix == 2 * asuint64 (1.0))
	    return 1.0;
	  if ((2 * ix < 2 * asuint64 (1.0)) == !(iy >> 63))
	    return 0.0; /* |x|<1 && y==inf or |x|>1 && y==-inf.  */
	  return y * y;
	}
      if (unlikely (zeroinfnan (ix)))
	{
	  double x2 = x * x;
	  if (ix >> 63 && checkint (iy) == 1)
	    {
	      x2 = -x2;
	      sign_bias = 1;
	    }
#if WANT_SIMD_EXCEPT
	  if (2 * ix == 0 && iy >> 63)
	    return __math_divzero (sign_bias);
#endif
	  /* Without the barrier some versions of clang hoist the 1/x2 and
	     thus division by zero exception can be signaled spuriously.  */
	  return iy >> 63 ? opt_barrier_double (1 / x2) : x2;
	}
      /* Here x and y are non-zero finite.  */
      if (ix >> 63)
	{
	  /* Finite x < 0.  */
	  int yint = checkint (iy);
	  if (yint == 0)
#if WANT_SIMD_EXCEPT
	    return __math_invalid (x);
#else
	    return __builtin_nan ("");
#endif
	  if (yint == 1)
	    sign_bias = SIGN_BIAS;
	  ix &= 0x7fffffffffffffff;
	  topx &= 0x7ff;
	}
      if ((topy & 0x7ff) - 0x3be >= 0x43e - 0x3be)
	{
	  /* Note: sign_bias == 0 here because y is not odd.  */
	  if (ix == asuint64 (1.0))
	    return 1.0;
	  /* |y| < 2^-65, x^y ~= 1 + y*log(x).  */
	  if ((topy & 0x7ff) < 0x3be)
	    return 1.0;
#if WANT_SIMD_EXCEPT
	  return (ix > asuint64 (1.0)) == (topy < 0x800) ? __math_oflow (0)
							 : __math_uflow (0);
#else
	  return (ix > asuint64 (1.0)) == (topy < 0x800) ? INFINITY : 0;
#endif
	}
      if (topx == 0)
	{
	  /* Normalize subnormal x so exponent becomes negative.  */
	  /* Without the barrier some versions of clang evalutate the mul
	     unconditionally causing spurious overflow exceptions.  */
	  ix = asuint64 (opt_barrier_double (x) * 0x1p52);
	  ix &= 0x7fffffffffffffff;
	  ix -= 52ULL << 52;
	}
    }

  double lo;
  double hi = log_inline (ix, &lo);
  double ehi = y * hi;
  double elo = y * lo + fma (y, hi, -ehi);
  return exp_inline (ehi, elo, sign_bias);
}