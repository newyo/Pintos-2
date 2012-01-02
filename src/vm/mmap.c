#include "mmap.h"
#include <stdio.h>
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "threads/interrupt.h"

struct mmap_region
{
  void             *start, *end;
  struct file      *file;
  struct list_elem  thread_elem;
  mapid_t           id;
};

void
mmap_init (void)
{
  printf ("Initialized mmapping.\n");
}

void
mmap_init_thread (struct thread *owner)
{
  ASSERT (owner != NULL);
  list_init (&owner->mmap_regions);
}

static void
mmap_close_real (struct mmap_region *ee)
{
  ASSERT (ee != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  // TODO
  
  free (ee);
}

void
mmap_clean (struct thread *owner)
{
  ASSERT (owner != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  while (!list_empty (&owner->mmap_regions))
    {
      struct list_elem *e = list_head (&owner->mmap_regions);
      ASSERT (e != NULL);
      struct mmap_region *ee = list_entry (e, struct mmap_region, thread_elem);
      mmap_close_real (ee);
    }
}

static struct mmap_region *
mmap_get_region (struct thread *owner, void *base)
{
  ASSERT (owner != NULL);
  ASSERT (pg_ofs (base) == 0);
  
  if (base == NULL)
    return NULL;
  
  struct list_elem *e;
  for (e = list_begin (&owner->mmap_regions);
       e != list_end (&owner->mmap_regions);
       e = list_next (e))
    {
      struct mmap_region *ee = list_entry (e, struct mmap_region, thread_elem);
      if (ee->start >= base && ee->end < base)
        return ee;
      if (ee->end >= base)
        return NULL;
    }
  return NULL;
}

static struct mmap_region *
mmap_get_region2 (struct thread *owner, mapid_t map)
{
  ASSERT (owner != NULL);
  
  if (map == MAP_FAILED)
    return NULL;
  
  struct list_elem *e;
  for (e = list_begin (&owner->mmap_regions);
       e != list_end (&owner->mmap_regions);
       e = list_next (e))
    {
      struct mmap_region *ee = list_entry (e, struct mmap_region, thread_elem);
      if (ee->id == map)
        return ee;
    }
  return NULL;
}

mapid_t
mmap_open (struct thread *owner, void *base, struct file *file)
{
  ASSERT (owner != NULL);
  ASSERT (base != NULL);
  ASSERT (pg_ofs (base) == 0);
  ASSERT (file != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  // TODO: new region must not overlap with an existing region
  
  // TODO
  return MAP_FAILED;
}

bool
mmap_close (struct thread *owner, mapid_t map)
{ 
  ASSERT (intr_get_level () == INTR_ON);
  
  struct mmap_region *ee = mmap_get_region2 (owner, map);
  if (!ee)
    return false;
  
  mmap_close_real (ee);
  return true;
}

bool
mmap_read (struct thread *owner, void *base, void *dest)
{
  ASSERT (dest != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  struct mmap_region *ee = mmap_get_region (owner, base);
  if (!ee)
    return false;
    
  // TODO
  return false;
}

bool
mmap_write (struct thread *owner, void *base, void *src)
{
  ASSERT (src != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  struct mmap_region *ee = mmap_get_region (owner, base);
  if (!ee)
    return false;
    
  // TODO
  return false;
}

size_t
mmap_get_size (struct thread *owner, void *base)
{
  ASSERT (owner != NULL);
  typedef char _CASSERT[0 - !(sizeof (size_t) == sizeof (uintptr_t))];
  
  struct mmap_region *ee = mmap_get_region (owner, base);
  if (!ee)
    return MMAP_INVALID_SIZE;
  return (uintptr_t) ee->end - (uintptr_t) ee->start;
}

size_t
mmap_get_size2 (struct thread *owner, mapid_t map)
{
  ASSERT (owner != NULL);
  typedef char _CASSERT[0 - !(sizeof (size_t) == sizeof (uintptr_t))];
  
  struct mmap_region *ee = mmap_get_region2 (owner, map);
  if (!ee)
    return MMAP_INVALID_SIZE;
  return (uintptr_t) ee->end - (uintptr_t) ee->start;
}
