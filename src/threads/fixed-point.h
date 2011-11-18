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
  uint32_t int_part   : _FP_T_INT_LEN;
  uint16_t frac_part  : _FP_T_FRAC_LEN;
} __attribute__ ((packed));

static inline uint32_t
int_abs (int32_t val)
{
  if (val >= 0)
    return val;
  return -val;
}

static inline fp_t
fp_abs (const fp_t val)
{
  fp_t result = val;
  result.signedness = 0;
  return result;
}

static inline int
fp_value_exceeds_bits (int64_t value, int bits)
{
  return value & ~((1<<bits) - 1);
}

static inline fp_t
fp_from_int (int32_t val)
{
  // ensure abs(val) does not consume more than _FP_T_INT_LEN bits.
  ASSERT (!fp_value_exceeds_bits (int_abs (val), _FP_T_INT_LEN));
  
  struct fp_t result;
  result.signedness = val < 0 ? 1 : 0;
  result.int_part = int_abs (val);
  result.frac_part = 0;
  return result;
}

/** rounds to nearest integer */
static inline int32_t
fp_round (const fp_t val)
{
  int result = val.int_part;
  if (val.frac_part >> (_FP_T_FRAC_LEN-1))
    ++result;
  if (val.signedness)
    result = -result;
  return result;
}

static inline int32_t
fp_truncate (const fp_t val)
{
  if (!val.signedness)
    return +val.int_part;
  else
    return -val.int_part;
}

static inline fp_t
fp_negate (const fp_t value)
{
  struct fp_t result;
  result.signedness = !value.signedness;
  result.int_part = value.int_part;
  result.frac_part = value.frac_part;
  return result;
}

static inline fp_t
fp_add (const fp_t left, const fp_t right)
{
  struct fp_t result;
  int32_t int_part, frac_part;
  result.signedness = left.signedness;
  
  if (!!left.signedness == !!right.signedness)
    {
      int_part = left.int_part + right.int_part;
      frac_part = left.frac_part + right.frac_part;
      
      if (fp_value_exceeds_bits(frac_part, _FP_T_FRAC_LEN))
        ++int_part;
      ASSERT (!fp_value_exceeds_bits (int_part, _FP_T_INT_LEN));
        
      result.int_part = int_part;
      result.frac_part = frac_part;
      return result;
    }
  
  // From now on left and right differ in signedness ...
  
  if (left.int_part == right.int_part && left.frac_part == right.frac_part)
    return fp_from_int (0);
  
  if (left.int_part < right.int_part || (left.int_part == right.int_part &&
                                         left.frac_part < right.frac_part))
    {
      // if abs(left) < abs(right)
      return fp_add (right, left);
    }
  else
    {
      // abs(left) > abs(right)
      
      int_part = left.int_part - right.int_part;
      frac_part = left.frac_part - right.frac_part;
      if (frac_part < 0)
        {
          frac_part = -frac_part;
          --int_part;
        }
      
      result.int_part = int_part;
      result.frac_part = frac_part;
      return result;
    }
}

static inline fp_t
fp_sub (const fp_t left, const fp_t right)
{
  struct fp_t neg_right = fp_negate (right);
  return fp_add (left, neg_right);
}

static inline fp_t
fp_mult (const fp_t left, const fp_t right)
{
  struct fp_t result;
  int32_t int_part, frac_part;
  
  int_part = left.int_part*right.int_part;
  frac_part = left.int_part*right.frac_part + left.frac_part*right.frac_part;
  
  while (fp_value_exceeds_bits (frac_part, _FP_T_FRAC_LEN))
    {
      ++int_part;
      frac_part -= 1 << _FP_T_FRAC_LEN;
    }
  
  result.signedness = !!left.signedness != !!right.signedness;
  result.int_part = int_part;
  result.frac_part = frac_part;
  return result;
}

static inline fp_t
fp_div (const fp_t left, const fp_t right)
{ 
  if (!!right.signedness)
    return fp_div (fp_negate (left), fp_negate (right));
  if (!!left.signedness)
    return fp_negate (fp_div (fp_negate (left), right));
    
  ASSERT (right.int_part != 0 || right.frac_part != 0);
  
  // now we only have to handle positive numerators und denominators
  
  struct fp_t result;
  int64_t num, denom, frac;
  
  num = (int64_t)*(int32_t*)&left << 32;
  denom = *(int32_t*)&right;
  frac = num / denom;
  
  *(int32_t*)&result = (int32_t)frac;
  return result;
}

static inline fp_t*
fp_incr_inplace (fp_t *member)
{
  if (!member->signedness)
    {
      ASSERT (member->int_part < (1 << _FP_T_INT_LEN) - 1);
      ++member->int_part;
    }
  else if (member->int_part > 0)
    --member->int_part;
  else
    {
      fp_t tmp = fp_add (fp_from_int (1), *member);
      *member = tmp;
    }
  return member;
}

static inline fp_t
fp_pow_int (const fp_t base, int expo)
{
  if (expo < 0)
    return fp_div (fp_from_int (1), fp_pow_int (base, -expo));
  
  fp_t result;
  while (--expo >= 0)
    result = fp_mult (result, base);
  return result;
}

#endif /* threads/fixed_point.h */
