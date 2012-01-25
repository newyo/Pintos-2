#ifndef __BITSET_H
#define __BITSET_H

#include <stdbool.h>
#include <stddef.h>
#include "off_t.h"

static inline bool
bitset_get (char *bitset, size_t nth)
{
  size_t byte = nth / 8;
  size_t ofs  = nth % 8;
  return bitset[byte] & (1 << ofs);
}

static inline void
bitset_mark (char *bitset, size_t nth)
{
  size_t byte = nth / 8;
  size_t ofs  = nth % 8;
  bitset[byte] |= 1 << ofs;
}

static inline void
bitset_reset (char *bitset, size_t nth)
{
  size_t byte = nth / 8;
  size_t ofs  = nth % 8;
  bitset[byte] &= ~(1 << ofs);
}

static inline void
bitset_set (char *bitset, size_t nth, bool value)
{
  (value ? &bitset_mark : &bitset_reset) (bitset, nth);
}

static inline int __attribute__ ((always_inline))
_bitset_find_least_one (int num)
{
  int result;
  asm volatile ("bsf %1, %0" : "=r"(result) : "g"(num));
  return result;
}

static inline int __attribute__ ((always_inline))
_bitset_reset_bit (int value, int nth)
{
  asm volatile ("btr %0, %1" : "+r"(value) : "g"(nth));
  return value;
}

static inline size_t
bitset_find_and_set (char *bitset,
                     size_t size,
                     size_t amount,
                     void (*cb) (size_t))
{
  uint32_t pos = 0;
  while (amount > 0 && size >= 4)
    {
      uint32_t rdatum = ~*(uint32_t *) bitset;
      if (rdatum != 0)
        {
          do
            {
              int offs = _bitset_find_least_one (rdatum);
              rdatum = _bitset_reset_bit (rdatum, offs);
              cb (pos + offs);
            }
          while (rdatum != 0 && --amount > 0);
          *(uint32_t *) bitset = ~rdatum;
        }
      bitset += 4;
      size -= 4;
    }
  while (amount > 0 && size-- > 0)
    {
      char rdatum = ~*bitset++;
      if (rdatum != 0)
        {
          do
            {
              int offs = _bitset_find_least_one (rdatum);
              rdatum = _bitset_reset_bit (rdatum, offs);
              cb (pos + offs);
            }
          while (rdatum != 0 && --amount > 0);
          *bitset = (char) ~rdatum;
        }
    }
  return amount;
}

static inline off_t
bitset_find_and_set_1 (char *bitset, size_t size)
{
  uint32_t pos = 0;
  while (size >= 4)
    {
      uint32_t rdatum = ~*(uint32_t *) bitset;
      if (rdatum != 0)
        {
          int offs = _bitset_find_least_one (rdatum);
          rdatum = _bitset_reset_bit (rdatum, offs);
          *(uint32_t *) bitset = ~rdatum;
          
          return pos + offs;
        }
      bitset += 4;
      size -= 4;
    }
  while (size-- > 0)
    {
      char rdatum = ~*bitset++;
      if (rdatum != 0)
        {
          int offs = _bitset_find_least_one (rdatum);
          rdatum = _bitset_reset_bit (rdatum, offs);
          *bitset = (char) ~rdatum;
          
          return pos + offs;
        }
    }
  return -1;
}

#endif
