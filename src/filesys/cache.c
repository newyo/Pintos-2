#include "cache.h"
#include "threads/malloc.h"

struct block_cache
{
  struct block     *device;
  struct bitmap    *use_map;
  block_cache_page *pages;
  struct lru        pages_lru;
};

struct block_cache *
block_cache_init (struct block *device, size_t cache_size)
{
  // TODO
  (void) device;
  (void) cache_size;
  return NULL;
}

void
block_cache_destroy (struct block_cache *bc)
{
  // TODO
  (void) bc;
}

block_cache_page *
block_cache_retrieve (struct block_cache *bc, block_sector_t nth)
{
  // TODO
  (void) bc;
  (void) nth;
  return NULL;
}

void
block_cache_return (struct block_cache *bc,
                    block_sector_t      nth,
                    block_cache_page   *page,
                    bool                wrote)
{
  // TODO
  (void) bc;
  (void) nth;
  (void) page;
  (void) wrote;
}

void
block_cache_write (struct block_cache *bc,
                   block_sector_t      nth,
                   block_cache_page   *data)
{
  // TODO
  (void) bc;
  (void) nth;
  (void) data;
}

void
block_cache_flush (struct block_cache *bc)
{
  // TODO
  (void) bc;
}

struct block *
block_cache_get_device (struct block_cache *bc)
{
  ASSERT (bc != NULL);
  return bc->device;
}
