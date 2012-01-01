#include "mmap.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"

struct mmap_region
{
  void             *start, *end;
  struct file      *file;
  struct list_elem  thread_elem;
};

void
mmap_init (void)
{
  // TODO
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
  
  // TODO
  
  free (ee);
}

void
mmap_clean (struct thread *owner)
{
  ASSERT (owner != NULL);
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
  ASSERT (base != NULL);
  ASSERT (pg_ofs (base) == 0);
  
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

bool
mmap_open (struct thread *owner, void *base, struct file *file)
{
  ASSERT (owner != NULL);
  ASSERT (base != NULL);
  ASSERT (pg_ofs (base) == 0);
  ASSERT (file != NULL);
  
  // TODO
  return false;
}

bool
mmap_close (struct thread *owner, void *base)
{ 
  struct mmap_region *ee = mmap_get_region (owner, base);
  if (!ee)
    return false;
  mmap_close_real (ee);
  return true;
}

bool
mmap_read (struct thread *owner, void *base, void *dest)
{
  ASSERT (dest != NULL);
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
  struct mmap_region *ee = mmap_get_region (owner, base);
  if (!ee)
    return false;
    
  // TODO
  return false;
}
