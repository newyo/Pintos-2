#include "cache.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "vm/allocator.h"

struct block_cache
{
  struct block     *device;
  struct semaphore  use_count;
  struct allocator  pages_allocator;
  struct lru        pages_disposable;
  struct lru        pages_in_use;
  struct lock       bc_lock;
};

struct block_cache *
block_cache_init (struct block *device, size_t cache_size, bool in_userspace)
{
  ASSERT (device != NULL);
  ASSERT (cache_size > 0);
  
  struct block_cache *bc = malloc (sizeof (*bc));
  if (!bc)
    return NULL;
  
  bc->device = device;
  if (!allocator_init (&bc->pages_allocator, in_userspace, cache_size,
                       sizeof (struct block_page)))
    {
      free (bc);
      return NULL;
    }
  sema_init (&bc->use_count, cache_size);
  lru_init (&bc->pages_disposable, 0, NULL, bc);
  lru_init (&bc->pages_in_use, 0, NULL, bc);
  lock_init (&bc->bc_lock);
  return bc;
}

void
block_cache_destroy (struct block_cache *bc)
{
  ASSERT (bc != NULL);
  ASSERT (lru_is_empty (&bc->pages_in_use));
  
  // TODO
  (void) bc;
}

struct block_page *
block_cache_read (struct block_cache *bc, block_sector_t nth)
{
  ASSERT (bc != NULL);
  // TODO
  (void) bc;
  (void) nth;
  return NULL;
}

struct block_page *
block_cache_write (struct block_cache *bc, block_sector_t nth)
{
  ASSERT (bc != NULL);
  // TODO
  (void) bc;
  (void) nth;
  return NULL;
}

void
block_cache_return (struct block_cache *bc, struct block_page  *page)
{
  ASSERT (bc != NULL);
  // TODO
  (void) bc;
  (void) page;
}

void
block_cache_flush (struct block_cache *bc, struct block_page  *page)
{
  ASSERT (bc != NULL);
  // TODO
  (void) bc;
  (void) page;
}

void
block_cache_flush_all (struct block_cache *bc)
{
  ASSERT (bc != NULL);
  // TODO
  (void) bc;
}

struct block *
block_cache_get_device (struct block_cache *bc)
{
  ASSERT (bc != NULL);
  return bc->device;
}
