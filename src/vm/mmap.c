#include "mmap.h"
#include <stdio.h>
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "threads/interrupt.h"

void
mmap_init (void)
{
  // TODO
  
  printf ("Initialized mmapping.\n");
}

void
mmap_init_thread (struct thread *owner)
{
  ASSERT (owner != NULL);
  
  // TODO
}

void
mmap_clean (struct thread *owner)
{
  ASSERT (owner != NULL);
  ASSERT (intr_get_level () == INTR_OFF);
  
  // TODO
}
