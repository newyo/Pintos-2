#include "allocator.h"
#include <debug.h>
#include <string.h>
#include "threads/palloc.h"
#include "threads/vaddr.h"

bool
allocator_init (struct allocator *a,
                bool              userspace,
                size_t            members,
                size_t            item_size)
{
  ASSERT (a != NULL);
  ASSERT (members > 0);
  ASSERT (item_size > 0);
  
  memset (a, 0, sizeof (*a));
  a->item_size = item_size;
  a->used_map = bitmap_create (members);
  if (a->used_map == NULL)
    return false;
  size_t pages = (members*item_size + PGSIZE-1) / PGSIZE;
  a->items = palloc_get_multiple (userspace ? PAL_USER : 0, pages);
  if (!a->items)
    {
      bitmap_destroy (a->used_map);
      return false;
    }
  return true;
}

void
allocator_destroy (struct allocator *a)
{
  ASSERT (a != NULL);
  ASSERT (bitmap_none (a->used_map, 0, bitmap_size (a->used_map)));
  size_t pages = (bitmap_size (a->used_map)*a->item_size + PGSIZE-1) / PGSIZE;
  palloc_free_multiple (a->items, pages);
  bitmap_destroy (a->used_map);
}

static inline void *
item_pos (struct allocator *a, size_t pos)
{
  return ((uint8_t *) a->items) + a->item_size*pos;
}

void *
allocator_alloc (struct allocator *a, size_t amount)
{
  ASSERT (a != NULL);
  if (amount == 0)
    return NULL;
  size_t result = bitmap_scan_and_flip (a->used_map, 0, amount, false);
  if (result == BITMAP_ERROR)
    return NULL;
  return item_pos (a, result);
}

void allocator_free (struct allocator *a, void *base, size_t amount)
{
  ASSERT (a != NULL);
  if (base == NULL || amount == 0)
    return;
  ASSERT (base >= a->items);
  ASSERT (base <= item_pos (a, bitmap_size (a->used_map)-1));
  size_t pos = (uintptr_t) (base - a->items) / a->item_size;
  ASSERT (bitmap_all (a->used_map, pos, amount));
  bitmap_set_multiple (a->used_map, pos, amount, false);
}
