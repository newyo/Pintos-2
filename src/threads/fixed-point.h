#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#include <stdint.h>

#define _FP_T_SGN_LEN  (1)
#define _FP_T_INT_LEN  (17)
#define _FP_T_FRAC_LEN (14)

typedef struct fp_t fp_t;

struct fp_t
{
  int8_t   signedness : _FP_T_SGN_LEN;
  uint16_t int_part   : _FP_T_INT_LEN;
  uint32_t frac_part  : _FP_T_FRAC_LEN;
};

static inline uint32_t
ABS(int32_t val)
{
  if (val >= 0)
    return val;
  return -val;
}

static inline fp_t
fp_from_int (int32_t val)
{
  // ensure abs(val) does not consume more than _FP_T_INT_LEN bits.
  ASSERT (ABS (val) & ~((1<<_FP_T_INT_LEN) - 1) == 0);
  
  struct fp_t result;
  result.signedness = val < 0 ? 1 : 0;
  result.int_part = ABS (val);
  result.frac_part = 0;
  return result;
}

static inline int32_t
fp_to_int (const fp_t val)
{
  int result = val.int_part;
  if (val.frac_part >> (_FP_T_FRAC_LEN-1))
    ++result;
  if (val.signedness)
    result = -result;
  return result;
}

static inline fp_t
add (fp_t left, fp_t right)
{
  return 0; //TODO
}

static inline fp_t
sub (fp_t left, fp_t right)
{
  return 0; //TODO
}

static inline fp_t
mult (fp_t left, fp_t right)
{
  return 0; //TODO
}

static inline fp_t
div (fp_t left, fp_t right)
{
  return 0; //TODO
}


#endif /* threads/fixed_point.h */
