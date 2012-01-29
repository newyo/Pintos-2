#include "bitset.h"

static inline int __attribute__ ((always_inline))
_bitset_find_least_one (int value)
{
  int result;
  asm volatile ("bsf %1, %0" : "=r"(result) : "g"(value));
  return result;
}

static inline int __attribute__ ((always_inline))
_bitset_reset_bit (int value, int nth)
{
  asm volatile ("btr %1, %0" : "+g"(value) : "r"(nth));
  return value;
}

size_t
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

off_t
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
