#include "swap.h"
#include <string.h>
#include <list.h>
#include <bitmap.h>
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/vaddr.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "userprog/process.h"
#include "lru.h"

typedef size_t swap_t;
#define SWAP_FAIL ((swap_t) BITMAP_ERROR)

const char SWAP_FILENAME[] = "swap.dsk";

struct swapped_page
{
  struct lru_elem   unmodified_pages_elem;
  struct thread    *thread;
  struct list_elem  elem;
  void             *base;
};

struct lock swap_lock;
struct file *swap_file;
size_t swap_pages_count;

struct bitmap *used_pages; // false == free
struct lru unmodified_pages; // list of struct swapped_page
struct swapped_page *swapped_pages_space;

static swap_t
swapped_page_id (struct swapped_page *cur)
{
  ASSERT (cur != NULL);
  ASSERT (cur >= swapped_pages_space);
  ASSERT (cur <  swapped_pages_space + swap_pages_count);
  
  return (uintptr_t) (cur - swap_pages_count) / sizeof (*cur);
}

static struct swapped_page *
swappage_page_of_owner (struct thread *owner, const void *base)
{
  ASSERT (owner != NULL);
  ASSERT (base != NULL);
  
  // TODO: don't iterate but use some hash table
  struct list_elem *e;
  for (e = list_begin (&owner->swap_pages);
       e != list_end (&owner->swap_pages);
       e = list_next (e))
    {
      struct swapped_page *ee = list_entry (e, struct swapped_page, elem);
      if (ee->base == base)
        return ee;
    }
  return false;
}

static void swap_dispose_page (struct swapped_page *ee, bool caller_triggered);

#define is_valid_swap_id(ID) \
({ \
  __typeof (X) _x = (ID); \
  _x != SWAP_FAIL && _x < swap_pages_count; \
})

#define are_valid_swap_ids(X, AMOUNT) \
({ \
  __typeof (X) _x = (X); \
  _x != SWAP_FAIL && _x < swap_pages_count && amount < swap_pages_count; \
})

void
swap_init (void)
{
  lock_init (&swap_lock);
  
  swap_file = filesys_open (SWAP_FILENAME);
  if (!swap_file)
    PANIC ("Could not set up swapping: Swapfile not found: %s", SWAP_FILENAME);
  file_deny_write (swap_file);
  swap_pages_count = file_length (swap_file) / PGSIZE;
  
  swapped_pages_space = calloc (swap_pages_count, sizeof (struct swapped_page));
  if (!swapped_pages_space)
    PANIC ("Could not set up swapping: Memory exhausted (1)");
  
  used_pages = bitmap_create (swap_pages_count);
  if (!used_pages)
    PANIC ("Could not set up swapping: Memory exhausted (2)");
  lru_init (&unmodified_pages, 0, NULL, NULL);
}

static swap_t
swap_get_disposable_pages (size_t count)
{
  ASSERT (intr_get_level () == INTR_ON);
  ASSERT (count > 0);
  ASSERT (count <= swap_pages_count);
  
  size_t result = bitmap_scan (used_pages, 0, count, false);
  if (result != BITMAP_ERROR)
    return result;
  
  if (count != 1)  // don't bother to make room for multiple pages
    return SWAP_FAIL;
  
  struct lru_elem *e = lru_pop_least (&unmodified_pages);
  if (e == NULL) // swap page is exhausted
    return SWAP_FAIL;
  
  struct swapped_page *ee;
  ee = lru_entry (e, struct swapped_page, unmodified_pages_elem);
  ASSERT (ee != NULL);
  result = swapped_page_id (ee);
  
  swap_dispose_page (ee, false);
  return result;
}

#define MIN(A,B)         \
({                       \
  __typeof (A) _a = (A); \
  __typeof (B) _b = (B); \
  _a <= _b ? _a : _b;    \
})

static void
swap_write (swap_t         id,
            struct thread *owner,
            void          *src_,
            size_t         length)
{
  uint8_t *src = src_;
  ASSERT (intr_get_level () == INTR_ON);
  ASSERT (owner != NULL);
  ASSERT (src != NULL);
  ASSERT (length > 0);
  
  size_t amount = (length+PGSIZE-1) / PGSIZE;
  ASSERT (are_valid_swap_ids (id, amount));
  
  file_allow_write (swap_file);
  swap_t i;
  for (i = id; i < id+amount; ++i)
    {
      struct swapped_page *ee = &swapped_pages_space[i];
      ASSERT (ee->thread == NULL);
      ASSERT (!bitmap_test (used_pages, i));
      
      bitmap_mark (used_pages, i);
      ee->thread = owner;
      list_push_front (&owner->swap_pages, &ee->elem);
      lru_use (&unmodified_pages, &ee->unmodified_pages_elem);
      ee->base = src;
      
      off_t len = MIN (length, (size_t) PGSIZE);
      off_t wrote UNUSED = file_write_at (swap_file, src, len, i*PGSIZE);
      ASSERT (wrote == len);
      
      src += len;
      length -= len;
    }
  file_deny_write (swap_file);
}

bool
swap_alloc_and_write (struct thread *owner,
                      void          *src,
                      size_t         length)
{
  ASSERT (intr_get_level () == INTR_ON);
  ASSERT (owner != NULL);
  ASSERT (src != NULL);
  ASSERT ((uintptr_t) src % PGSIZE == 0);
  ASSERT (length > 0);
  
  ASSERT (intr_get_level () == INTR_ON);
  
  size_t amount = (length+PGSIZE-1) / PGSIZE;
  swap_t id = swap_get_disposable_pages (amount);
  if (id != SWAP_FAIL)
    {
      swap_write (id, owner, src, length);
      lock_release (&swap_lock);
      return true;
    }
  
  // Could not find `amount` adjactent swap pages.
  // Break it down to many pages atomically.
  // Find enough pages and write then or write nothing at all.
  // We don't have to care about the retreived swap_ts,
  // b/c if they are not written to, they stay marked free in the used_pages.
  swap_t *ids = calloc (amount, sizeof (swap_t));
  if (!ids)
    return false;
  size_t i;
  for (i = 0; i < amount; ++i)
    {
      id = swap_get_disposable_pages (1);
      if (id == SWAP_FAIL)
        {
          free (ids);
          lock_release (&swap_lock);
          return false;
        }
    }
  for (i = 0; i < amount; ++i)
    {
      off_t len = MIN (length, (size_t) PGSIZE);
      swap_write (ids[i], owner, src, len);
      src += len;
      length -= len;
    }
  free (ids);
  lock_release (&swap_lock);
  return true;
}

static void
swap_dispose_page (struct swapped_page *ee, bool caller_triggered)
{
  ASSERT (ee != NULL);
  
  if (!caller_triggered)
    lru_dispose (&unmodified_pages, &ee->unmodified_pages_elem, false);
  else
    {
      list_remove (&ee->elem);
      process_dispose_unmodified_swap_page (ee->thread, &ee->base);
    }
  memset (ee, sizeof (*ee), 0);
  bitmap_reset (used_pages, swapped_page_id (ee));
}

bool
swap_dispose (struct thread *owner, const void *base_, size_t amount)
{
  const uint8_t *base = base_;
  ASSERT (intr_get_level () == INTR_ON);
  ASSERT (owner != NULL);
  ASSERT (base != NULL);
  ASSERT ((uintptr_t) base % PGSIZE == 0);
  ASSERT (amount > 0);
  ASSERT (amount <= swap_pages_count);
  
  lock_acquire (&swap_lock);
  do
    {
      struct swapped_page *ee = swappage_page_of_owner (owner, base);
      if (!ee)
        {
          lock_release (&swap_lock);
          return false;
        }
        
      swap_dispose_page (ee, true);
      
      base += PGSIZE;
    }
  while (--amount > 0);
  lock_release (&swap_lock);
  return true;
}

bool
swap_read_and_retain (struct thread *owner,
                      const void    *base_,
                      size_t         length)
{
  const uint8_t *base = base_;
  ASSERT (intr_get_level () == INTR_ON);
  ASSERT (owner != NULL);
  ASSERT (base != NULL);
  ASSERT ((uintptr_t) base % PGSIZE == 0);
  
  size_t amount = (length+PGSIZE-1) / PGSIZE;
  ASSERT (amount > 0);
  ASSERT (amount <= swap_pages_count);
  
  lock_acquire (&swap_lock);
  do
    {
      struct swapped_page *ee = swappage_page_of_owner (owner, base);
      if (!ee)
        {
          lock_release (&swap_lock);
          return false;
        }
        
      off_t len = MIN (length, (size_t) PGSIZE);
      off_t wrote UNUSED = file_read_at (swap_file,
                                         ee->base,
                                         len,
                                         swapped_page_id (ee));
      ASSERT (wrote == len);
      length -= len;
      lru_use (&unmodified_pages, &ee->unmodified_pages_elem);
      
      base += PGSIZE;
    }
  while (--amount > 0);
  lock_release (&swap_lock);
  return true;
}

void
swap_clean (struct thread *owner)
{
  ASSERT (owner != NULL);
  
  lock_acquire (&swap_lock);
  while (!list_empty (&owner->swap_pages))
    {
      struct list_elem *e = list_pop_front (&owner->swap_pages);
      ASSERT (e != NULL);
      struct swapped_page *ee = list_entry (e, struct swapped_page, elem);
      swap_dispose_page (ee, true);
    }
  lock_release (&swap_lock);
}

size_t
swap_stats_pages (void)
{
  ASSERT (bitmap_size (used_pages) == swap_pages_count);
  return swap_pages_count;
}

size_t
swap_stats_full_pages (void)
{
  ASSERT (bitmap_size (used_pages) == swap_pages_count);
  
  size_t allocated = bitmap_count (used_pages, 0, swap_pages_count, true);
  size_t unmodified = lru_usage (&unmodified_pages);
  return allocated - unmodified;
}
