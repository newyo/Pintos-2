#include "cache.h"
#include "threads/malloc.h"
#include "threads/interrupt.h"

// TODO: implement interval-wise flushing (clock?)

bool
block_cache_init (struct block_cache *bc,
                  struct block       *device,
                  size_t              cache_size,
                  bool                in_userspace)
{
  ASSERT (bc != NULL);
  ASSERT (device != NULL);
  ASSERT (cache_size > 0);
  
  bc->device = device;
  if (!allocator_init (&bc->pages_allocator, in_userspace, cache_size,
                       sizeof (struct block_page)))
    return false;
  sema_init (&bc->use_count, cache_size);
  lru_init (&bc->pages_disposable, 0, NULL, bc);
  lru_init (&bc->pages_in_use, 0, NULL, bc);
  lock_init (&bc->bc_lock);
  return true;
}

void
block_cache_destroy (struct block_cache *bc)
{
  ASSERT (bc != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  ASSERT (lru_is_empty (&bc->pages_in_use));
  
  block_cache_flush_all (bc);
  allocator_destroy (&bc->pages_allocator);
}

// leaves with bc->bc_lock locked!
static struct block_page *
block_cache_retreive (struct block_cache *bc,
                      block_sector_t      nth,
                      bool               *retreived)
{
  ASSERT (bc != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  if (retreived)
    *retreived = true;
    
  lock_acquire (&bc->bc_lock);
  
  struct block_page *result;
  
  struct list_elem *e;
  for (e = list_begin (&bc->pages_in_use.lru_list);
       e != list_end (&bc->pages_in_use.lru_list);
       e = list_next (e))
    {
      result = list_entry (e, struct block_page, elem);
      if (result->nth == nth)
        goto end;
    }
  for (e = list_begin (&bc->pages_disposable.lru_list);
       e != list_end (&bc->pages_disposable.lru_list);
       e = list_next (e))
    {
      result = list_entry (e, struct block_page, elem);
      if (result->nth == nth)
        goto end;
    }
    
  if (retreived)
    *retreived = false;
  
  if (sema_try_down (&bc->use_count))
    result = allocator_alloc (&bc->pages_allocator, 1);
  else if (!lru_is_empty (&bc->pages_disposable))
    {
      struct lru_elem *e = lru_pop_least (&bc->pages_disposable);
      result = lru_entry (e, struct block_page, elem);
      block_cache_flush (bc, result);
    }
  else
    {
      sema_down (&bc->use_count);
      result = allocator_alloc (&bc->pages_allocator, 1);
    }
    
  ASSERT (result != NULL);
  result->lease_counter = 0;
  result->nth = nth;
  memset (&result->elem, 0, sizeof (result->elem));
  
end:
  lru_use (&bc->pages_in_use, &result->elem);
  ++result->lease_counter;
  
  return result;
}

struct block_page *
block_cache_read (struct block_cache *bc, block_sector_t nth)
{
  ASSERT (bc != NULL);
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
  ASSERT (page != NULL);
  
  ASSERT (page->elem.lru_list == &bc->pages_in_use);
  ASSERT (page->lease_counter > 0);
  
  lock_acquire (&bc->bc_lock);
  if (--page->lease_counter == 0)
    lru_use (&bc->pages_disposable, &page->elem);
  lock_release (&bc->bc_lock);
}

void
block_cache_flush (struct block_cache *bc, struct block_page *page)
{
  ASSERT (bc != NULL);
  ASSERT (page != NULL);
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
  ASSERT (intr_get_level () == INTR_ON);
  
  lock_acquire (&bc->bc_lock);
  struct list_elem *e;
  for (e = list_begin (&bc->pages_disposable.lru_list);
       e != list_end (&bc->pages_disposable.lru_list);
       e = list_next (e))
    {
      struct block_page *ee = list_entry (e, struct block_page, elem);
      block_cache_flush (bc, ee);
    }
  lock_release (&bc->bc_lock);
}
