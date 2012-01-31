#ifndef CACHE_H__
#define CACHE_H__

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <hash.h>
#include "threads/synch.h"
#include "devices/block.h"
#include "vm/lru.h"
#include "vm/allocator.h"

// block_cache may be used concurrently w/o destroying metadata,
// but caller must be aware of any races for the actual (block device's) data.
// Reading, writing and flushing depends on INTR_ON.

typedef char block_data[BLOCK_SECTOR_SIZE];

struct block_cache
{
/* public */
  struct block     *device;           // associated block device
/* private: */
  struct semaphore  use_count;        // count of items left in pages_allocator
  struct allocator  pages_allocator;  // allocator of struct block_page
  struct lru        pages_disposable; // returned pages (the cache)
  struct hash       hash;             // [nth -> struct block_page]
  struct lock       bc_lock;          // concurrent modification lock
  
  uint32_t          magic;
};

struct block_page
{
/* public: */
  block_data       data;
  bool             dirty;
/* private: */
  uint32_t         magic; // I put the magic here so it may get overwritten
  
  size_t           lease_counter;
  block_sector_t   nth;
  struct lru_elem  lru_elem;
  struct hash_elem hash_elem;
};

bool block_cache_init (struct block_cache *bc,
                       struct block       *device,
                       size_t              cache_size,
                       bool                in_userspace);
void block_cache_destroy (struct block_cache *bc);

// result::data will contain blocks data
struct block_page *block_cache_read (struct block_cache *bc,
                                     block_sector_t      nth);
// result::data is entirely random!
struct block_page *block_cache_write (struct block_cache *bc,
                                      block_sector_t      nth);
// page is no longer used by current operation.
// Holding more than one page at a time may render a deadlock!
void block_cache_return (struct block_cache *bc, struct block_page *page);

void block_cache_flush (struct block_cache *bc, struct block_page *page);
void block_cache_flush_all (struct block_cache *bc);

// Convenience methods:

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
