#include "swap.h"
#include "lru.h"
#include <stdbool.h>
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/vaddr.h"
#include "threads/interrupt.h"

const char SWAP_FILENAME[] = "swap.dsk";

struct file *swap_file;
size_t swap_pages;

struct bitmap *empty_pages;
struct lru unmodified_pages;

void
swap_init (void)
{
  ASSERT (intr_get_level () == INTR_OFF);
  
  swap_file = filesys_open (SWAP_FILENAME);
  if (!swap_file)
    PANIC ("Could not set up swapping: Swapfile not found: %s", SWAP_FILENAME);
  file_deny_write (swap_file);
  swap_pages = file_length (swap_file) / PGSIZE;
  
  empty_pages = bitmap_create (swap_pages);
  if (!empty_pages)
    PANIC ("Could not set up swapping: Memory exhausted");
  lru_init (&unmodified_pages, 0, NULL, NULL);
}

swap_t
swap_get_disposable_bytes (size_t count)
{
  ASSERT (count > 0);
  return swap_get_disposable_pages ((count+PGSIZE-1) / PGSIZE);
}

swap_t
swap_get_disposable_pages (size_t count)
{
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (count > 0);
  ASSERT (count <= swap_pages);
  
  size_t result = bitmap_scan_and_flip (empty_pages, 0, count, true);
  if (result != BITMAP_ERROR)
    return result;
  
  if (count != 1)  // don't bother to make room for multiple pages for now
    return SWAP_FAIL;
  
  struct lru_entry *e = lru_pop_least (&unmodified_pages);
  if (e == NULL) // swap page is exhausted
    return SWAP_FAIL;
    
  ASSERT (0); // TODO
}
