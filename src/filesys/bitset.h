#ifndef __BITSET_H
#define __BITSET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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

size_t bitset_find_and_set (char   *bitset,
                            size_t  size,
                            size_t  amount,
                            void   (*cb) (size_t, void *),
                            void    *aux);
off_t bitset_find_and_set_1 (char *bitset, size_t size);

static inline void
bitset_mark_range (char *bitset, size_t start, size_t amount)
{
  // TODO: optimize
  size_t i;
  for (i = start; i < start+amount; ++i)
    bitset_mark (bitset, i);
}

static inline void
bitset_reset_range (char *bitset, size_t start, size_t amount)
{
  // TODO: optimize
  size_t i;
  for (i = start; i < start+amount; ++i)
    bitset_reset (bitset, i);
}

static inline void
bitset_set_range (char *bitset, size_t start, size_t amount, bool value)
{
  (value ? &bitset_mark_range : &bitset_reset_range) (bitset, start, amount);
}

#endif
