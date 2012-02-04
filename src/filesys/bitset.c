#include "bitset.h"

static inline uint32_t __attribute__ ((always_inline))
_bitset_find_least_one_and_reset (uint32_t *value_)
{
  uint32_t result;
  asm volatile ("bsf %1, %0\n"
                "btr %0, %1": "=r"(result), "+r"(*value_));
  return result;
}

size_t
bitset_find_and_set (char   *bitset,
                     size_t  size,
                     size_t  amount,
                     void  (*cb) (size_t, void *),
                     void   *aux)
{
  uint32_t pos = 0;
  while (amount > 0 && size >= 4)
    {
      uint32_t rdatum = ~*(uint32_t *) bitset;
      if (rdatum != 0)
        {
          do
            {
              int offs = _bitset_find_least_one_and_reset (&rdatum);
              cb (pos + offs, aux);
              --amount;
            }
          while (rdatum != 0 && amount > 0);
          *(uint32_t *) bitset = ~rdatum;
        }
      pos += 32;
      bitset += 4;
      size -= 4;
    }
  while (amount > 0 && size > 0)
    {
      uint32_t rdatum = ~(uint32_t) *bitset;
      if (rdatum != 0)
        {
          do
            {
              int offs = _bitset_find_least_one_and_reset (&rdatum);
              cb (pos + offs, aux);
              --amount;
            }
          while (rdatum != 0 && amount > 0);
          *bitset = ~(char) rdatum;
        }
      pos += 8;
      ++bitset;
      --size;
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
          int offs = _bitset_find_least_one_and_reset (&rdatum);
          *(uint32_t *) bitset = ~rdatum;
          
          return pos + offs;
        }
      pos += 32;
      bitset += 4;
      size -= 4;
    }
  while (size > 0)
    {
      uint32_t rdatum = ~(uint32_t) *bitset;
      if (rdatum != 0)
        {
          int offs = _bitset_find_least_one_and_reset (&rdatum);
          *bitset = ~(char) rdatum;
          
          return pos + offs;
        }
      pos += 8;
      ++bitset;
      --size;
    }
  return -1;
}
