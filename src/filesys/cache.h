#ifndef CACHE_H__
#define CACHE_H__

#include "devices/block.h"
#include "vm/lru.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef char block_data[BLOCK_SECTOR_SIZE];

struct block_cache;

struct block_page
{
  block_data      data;
  bool            dirty;
  struct lru_elem elem;
};

struct block_cache *block_cache_init (struct block *device,
                                      size_t        cache_size,
                                      bool          in_userspace);
void block_cache_destroy (struct block_cache *bc);

struct block_page *block_cache_read (struct block_cache *bc,
                                     block_sector_t      nth);
struct block_page *block_cache_write (struct block_cache *bc,
                                      block_sector_t      nth);
void block_cache_return (struct block_cache *bc,
                         struct block_page  *page);

void block_cache_flush (struct block_cache *bc, struct block_page  *page);
void block_cache_flush_all (struct block_cache *bc);

struct block *block_cache_get_device (struct block_cache *bc);

static inline void
block_cache_write_out (struct block_cache *bc,
                       block_sector_t      nth,
                       block_data         *data)
{
  struct block_page *page = block_cache_write (bc, nth);
  memcpy (&page->data, data, BLOCK_SECTOR_SIZE);
  page->dirty = true;
  block_cache_return (bc, page);
}

static inline void
block_cache_read_in (struct block_cache *bc,
                     block_sector_t      nth,
                     block_data         *dest)
{
  struct block_page *page = block_cache_read (bc, nth);
  memcpy (dest, &page->data, BLOCK_SECTOR_SIZE);
  block_cache_return (bc, page);
}

#endif
