#include "cache.h"
#include "threads/malloc.h"
#include "threads/interrupt.h"

#define BC_MAGIC (('B'<<24) + ('l'<<16) + ('k'<<8) + 'C')
#define BC_PAGE_MAGIC (('B'<<24) + ('l'<<16) + ('C'<<8) + 'P')

// TODO: implement interval-wise flushing (clock?)

static unsigned
block_cache_page_hash (const struct hash_elem *e, void *bc UNUSED)
{
  ASSERT (e != NULL);
  struct block_page *ee = hash_entry (e, struct block_page, hash_elem);
  typedef char _CASSERT[0 - !(sizeof (unsigned) == sizeof (ee->nth))];
  ASSERT (ee->magic == BC_PAGE_MAGIC);
  return (unsigned) ee->nth;
}

static bool
block_cache_page_less (const struct hash_elem *a,
                       const struct hash_elem *b,
                       void                   *bc UNUSED)
{
  ASSERT (a != NULL);
  ASSERT (b != NULL);
  struct block_page *aa = hash_entry (a, struct block_page, hash_elem);
  struct block_page *bb = hash_entry (b, struct block_page, hash_elem);
  ASSERT (aa->magic == BC_PAGE_MAGIC);
  ASSERT (bb->magic == BC_PAGE_MAGIC);
  return aa->nth < bb->nth;
}

bool
block_cache_init (struct block_cache *bc,
                  struct block       *device,
                  size_t              cache_size,
                  bool                in_userspace)
{
  ASSERT (bc != NULL);
  ASSERT (device != NULL);
  ASSERT (cache_size > 0);
  
  memset (bc, 0, sizeof (*bc));
  bc->device = device;
  if (!allocator_init (&bc->pages_allocator, in_userspace, cache_size,
                       sizeof (struct block_page)))
    return false;
  sema_init (&bc->use_count, cache_size);
  lru_init (&bc->pages_disposable, 0, NULL, bc);
  hash_init (&bc->hash, block_cache_page_hash, block_cache_page_less, bc);
  lock_init (&bc->bc_lock);
  bc->magic = BC_MAGIC;
  return true;
}

static void
block_cache_flush_all_sub (struct hash_elem *e, void *bc_)
{
  struct block_cache *bc = bc_;
  ASSERT (bc != NULL);
  ASSERT (e != NULL);
  
  struct block_page *ee = hash_entry (e, struct block_page, hash_elem);
  typedef char _CASSERT[0 - !(sizeof (unsigned) == sizeof (ee->nth))];
  ASSERT (ee->magic == BC_PAGE_MAGIC);
  
  block_cache_flush (bc, ee);
}

void
block_cache_destroy (struct block_cache *bc)
{
  ASSERT (bc != NULL);
  ASSERT (bc->magic == BC_MAGIC);
  ASSERT (intr_get_level () == INTR_ON);
  
  hash_destroy (&bc->hash, &block_cache_flush_all_sub);
  allocator_destroy (&bc->pages_allocator);
  
  bc->magic ^= -1u;
}

// leaves with bc->bc_lock locked!
static struct block_page *
block_cache_retreive (struct block_cache *bc,
                      block_sector_t      nth,
                      bool               *retreived)
{
  ASSERT (bc != NULL);
  ASSERT (intr_get_level () == INTR_ON);
    
  lock_acquire (&bc->bc_lock);
  
  struct block_page *result;
  struct block_page key;
  key.nth = nth;
  key.magic = BC_PAGE_MAGIC;
  struct hash_elem *e = hash_find (&bc->hash, &key.hash_elem);
  if (e != NULL)
    {
      if (retreived)
        *retreived = true;
      result = hash_entry (e, struct block_page, hash_elem);
      goto end;
    }
    
  if (retreived)
    *retreived = false;
  
  if (sema_try_down (&bc->use_count))
    {
      result = allocator_alloc (&bc->pages_allocator, 1);
      result->magic = BC_PAGE_MAGIC;
      memset (&result->lru_elem, 0, sizeof (result->lru_elem));
    }
  else if (lru_is_empty (&bc->pages_disposable))
    {
      sema_down (&bc->use_count);
      
      result = allocator_alloc (&bc->pages_allocator, 1);
      result->magic = BC_PAGE_MAGIC;
      memset (&result->lru_elem, 0, sizeof (result->lru_elem));
    }
  else
    {
      struct lru_elem *e = lru_pop_least (&bc->pages_disposable);
      result = lru_entry (e, struct block_page, lru_elem);
      ASSERT (result->magic == BC_PAGE_MAGIC);
      
      block_cache_flush (bc, result);
      
      struct hash_elem *f UNUSED = hash_delete (&bc->hash, &result->hash_elem);
      ASSERT (f == &result->hash_elem);
    }
    
  result->lease_counter = 0;
  result->nth = nth;
  struct hash_elem *f UNUSED = hash_insert (&bc->hash, &result->hash_elem);
  ASSERT (f == NULL);
  
end:
  lru_dispose (&bc->pages_disposable, &result->lru_elem, false);
  ++result->lease_counter;
  return result;
}

struct block_page *
block_cache_read (struct block_cache *bc, block_sector_t nth)
{
  ASSERT (bc != NULL);
  ASSERT (bc->magic == BC_MAGIC);
  ASSERT (intr_get_level () == INTR_ON);
  
  bool retreived;
  struct block_page *result = block_cache_retreive (bc, nth, &retreived);
  ASSERT (result != NULL);
  if (!retreived)
    {
      block_read (bc->device, nth, &result->data);
      result->dirty = false;
    }
  lock_release (&bc->bc_lock);
  return result;
}

struct block_page *
block_cache_write (struct block_cache *bc, block_sector_t nth)
{
  ASSERT (bc != NULL);
  ASSERT (bc->magic == BC_MAGIC);
  ASSERT (intr_get_level () == INTR_ON);
  
  struct block_page *result = block_cache_retreive (bc, nth, NULL);
  ASSERT (result != NULL);
  result->dirty = true;
  lock_release (&bc->bc_lock);
  return result;
}

void
block_cache_return (struct block_cache *bc, struct block_page *page)
{
  ASSERT (bc != NULL);
  ASSERT (bc->magic == BC_MAGIC);
  ASSERT (page != NULL);
  ASSERT (page->magic == BC_PAGE_MAGIC);
  
  ASSERT (!lru_is_interior (&page->lru_elem));
  ASSERT (page->lease_counter > 0);
  
  lock_acquire (&bc->bc_lock);
  if (--page->lease_counter == 0)
    lru_use (&bc->pages_disposable, &page->lru_elem);
  lock_release (&bc->bc_lock);
}

void
block_cache_flush (struct block_cache *bc, struct block_page *page)
{
  ASSERT (bc != NULL);
  ASSERT (bc->magic == BC_MAGIC);
  ASSERT (page != NULL);
  ASSERT (page->magic == BC_PAGE_MAGIC);
  ASSERT (intr_get_level () == INTR_ON);
  
  if (!page->dirty)
    return;
    
  block_write (bc->device, page->nth, &page->data);
  page->dirty = false;
}

void
block_cache_flush_all (struct block_cache *bc)
{
  ASSERT (bc != NULL);
  ASSERT (bc->magic == BC_MAGIC);
  ASSERT (intr_get_level () == INTR_ON);
  
  lock_acquire (&bc->bc_lock);
  hash_apply (&bc->hash, &block_cache_flush_all_sub);
  lock_release (&bc->bc_lock);
}
