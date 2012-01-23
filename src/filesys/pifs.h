#ifndef __PIFS_H
#define __PIFS_H

// pifs = Pintos Filesystem :)

#include "cache.h"
#include <stdbool.h>
#include <packed.h>

struct pifs_attrs
{
  bool readable   : 1;
  bool writable   : 1;
  bool executable : 1;
} PACKED;

typedef char _CASSERT_PIFS_ATTRS_SIZE[0 - !(sizeof (struct pifs_attrs) == 1)];

struct pifs_device
{
  struct block_cache *device;
  uint32_t            clock;
};

void pifs_format (struct pifs_device *bc);

#endif
