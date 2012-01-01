#include "swap.h"
#include <string.h>
#include <limits.h>
#include <list.h>
#include <hash.h>
#include <stdio.h>
#include "threads/vaddr.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "userprog/process.h"
#include "devices/block.h"
#include "lru.h"
#include "vm.h"

struct swapped_page
{
  struct lru_elem   unmodified_pages_elem;
  struct thread    *thread;
  struct hash_elem  hash_elem;
  void             *base;
};

struct lock swap_lock;
struct block *swap_disk;
size_t swap_pages_count;

#define PG_SECTOR_RATIO (PGSIZE / BLOCK_SECTOR_SIZE)
char _CASSERT_INT_PG_SECTOR_RATIO[0 - !(PGSIZE % BLOCK_SECTOR_SIZE == 0)];

struct bitmap *used_pages; // false == free
struct lru unmodified_pages; // list of struct swapped_page
struct swapped_page *swapped_pages_space;

static swap_t
swapped_page_id (struct swapped_page *cur)
{
  ASSERT (cur != NULL);
  ASSERT (cur >= swapped_pages_space);
  ASSERT (cur <  swapped_pages_space + swap_pages_count);
  
  return (uintptr_t) (cur - swapped_pages_space) / sizeof (*cur);
}

static block_sector_t
swap_page_to_sector (swap_t page)
{
  ASSERT (page < swap_pages_count);
  uint64_t result = page * PG_SECTOR_RATIO;
  ASSERT (result < UINT_MAX);
  return (swap_t) result;
}

static struct swapped_page *
swappage_page_of_owner (struct thread *owner, void *base)
{
  ASSERT (owner != NULL);
  ASSERT (base != NULL);
  
  struct swapped_page key;
  memset (&key.hash_elem, 0, sizeof (key.hash_elem));
  key.thread = owner;
  key.base = base;
  struct hash_elem *e = hash_find (&owner->swap_pages, &key.hash_elem);
  if (e == NULL)
    return NULL;
    
  struct swapped_page *ee = hash_entry (e, struct swapped_page, hash_elem);
  ASSERT (ee->thread == owner);
  ASSERT (ee->base == base);
  return ee;
}

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

static void UNUSED
swap_needlessly_zero_out_whole_swap_space (void)
{
  char zero[BLOCK_SECTOR_SIZE];
  memset (zero, 0x66, sizeof (zero));
  
  block_sector_t i;
  for (i = 0; i < swap_pages_count; ++i)
    block_write (swap_disk, i, zero);
}

void
swap_init (void)
{
  lock_init (&swap_lock);
  
  swap_disk = block_get_role (BLOCK_SWAP);
  ASSERT (swap_disk != NULL);
  
  swap_pages_count = block_size (swap_disk) / PG_SECTOR_RATIO;
  ASSERT (swap_pages_count > 0);
  
  swapped_pages_space = calloc (swap_pages_count, sizeof (struct swapped_page));
  if (!swapped_pages_space)
    PANIC ("Could not set up swapping: Memory exhausted (1)");
  
  used_pages = bitmap_create (swap_pages_count);
  if (!used_pages)
    PANIC ("Could not set up swapping: Memory exhausted (2)");
  lru_init (&unmodified_pages, 0, NULL, NULL);
  
#ifndef NDEBUG
  swap_needlessly_zero_out_whole_swap_space ();
#endif
  
  printf ("Initialized swapping.\n");
}

static void
swap_dispose_page_real (struct swapped_page *ee)
{
  ASSERT (ee != NULL);
  ASSERT (ee->thread != NULL);
  ASSERT (ee->base != NULL);
  
  lru_dispose (&unmodified_pages, &ee->unmodified_pages_elem, false);
  memset (ee, 0, sizeof (*ee));
  bitmap_reset (used_pages, swapped_page_id (ee));
}

static void
swap_dispose_page (struct swapped_page *ee)
{
  ASSERT (ee != NULL);
  
  struct hash_elem *e UNUSED;
  e = hash_delete (&ee->thread->swap_pages, &ee->hash_elem);
  ASSERT (e != NULL);
  
  swap_dispose_page_real (ee);
}

static swap_t
swap_get_disposable_pages (size_t count)
{
  ASSERT (count > 0);
  ASSERT (count <= swap_pages_count);
  
  size_t result = bitmap_scan (used_pages, 0, count, false);
  if (result != BITMAP_ERROR)
    return result;
  
  if (count != 1)  // don't bother to make room for multiple pages
    return SWAP_FAIL;
  
  for (;;)
    {
      struct lru_elem *e = lru_peek_least (&unmodified_pages);
      if (e == NULL) // swap space is exhausted
        return SWAP_FAIL;
      
      struct swapped_page *ee;
      ee = lru_entry (e, struct swapped_page, unmodified_pages_elem);
      ASSERT (ee != NULL);
      
      vm_swap_disposed (ee->thread, ee->base);
      swap_dispose_page (ee);
      
      return swapped_page_id (ee);
    }
}

#define MIN(A,B)         \
({                       \
  __typeof (A) _a = (A); \
  __typeof (B) _b = (B); \
  _a <= _b ? _a : _b;    \
})

static void
swap_write_sectors (swap_t start, void *src, size_t len)
{
  ASSERT (src != NULL);
  ASSERT (len > 0);
  
  block_sector_t sector = swap_page_to_sector (start);
  while (len >= BLOCK_SECTOR_SIZE)
    {
      block_write (swap_disk, sector, src);
      src += BLOCK_SECTOR_SIZE;
      len -= BLOCK_SECTOR_SIZE;
      ++sector;
    }
  if (len > 0)
    {
      // cannot write an underfull block
      uint8_t data[BLOCK_SECTOR_SIZE];
      memcpy (data, src, len);
      memset (data+len, 0, BLOCK_SECTOR_SIZE-len);
      block_write (swap_disk, sector, data);
    }
}

static void
swap_read_sectors (swap_t start, void *dest)
{
  ASSERT (start < swap_pages_count);
  ASSERT (dest != NULL);
  
  block_read (swap_disk, swap_page_to_sector (start), dest);
}

static void
swap_write (swap_t         id,
            struct thread *owner,
            void          *base,
            void          *src,
            size_t         length)
{
  ASSERT (intr_get_level () == INTR_ON);
  ASSERT (owner != NULL);
  ASSERT (base != NULL);
  ASSERT (pg_ofs (base) == 0);
  ASSERT (src != NULL);
  ASSERT (length > 0);
  
  size_t amount = (length+PGSIZE-1) / PGSIZE;
  ASSERT (are_valid_swap_ids (id, amount));
  
  swap_t i;
  for (i = id; i < id+amount; ++i)
    {
      struct swapped_page *ee = &swapped_pages_space[i];
      if (ee->thread == NULL)
        {
          ASSERT (ee->base == NULL);
          ASSERT (!bitmap_test (used_pages, i));
          
          bitmap_mark (used_pages, i);
          ee->thread = owner;
          ee->base = base;
          hash_insert (&owner->swap_pages, &ee->hash_elem);
        }
      else
        {
          printf (" ee->thread=%8p, owner=%8p\n", ee->thread, owner);
          ASSERT (ee->thread == owner);
          ASSERT (ee->base == base);
          ASSERT (bitmap_test (used_pages, i));
        }
      lru_use (&unmodified_pages, &ee->unmodified_pages_elem);
      
      size_t len = MIN (length, (size_t) PGSIZE);
      swap_write_sectors (i, src, len);
      base += len;
      src += len;
      length -= len;
    }
}

bool
swap_alloc_and_write (struct thread *owner,
                      void          *base,
                      void          *src)
{
  ASSERT (intr_get_level () == INTR_ON);
  ASSERT (owner != NULL);
  ASSERT (base != NULL);
  ASSERT (pg_ofs (base) == 0);
  ASSERT (src != NULL);
  
  lock_acquire (&swap_lock);
  
  struct swapped_page *ee = swappage_page_of_owner (owner, base);
  if (ee != NULL)
    {
      swap_write (swapped_page_id (ee), owner, base, src, PGSIZE);
      lock_release (&swap_lock);
      return true;
    }
  
  swap_t id = swap_get_disposable_pages (1);
  if (id == SWAP_FAIL)
    return false;
  swap_write (id, owner, base, src, PGSIZE);
  lock_release (&swap_lock);
  return true;
}

bool
swap_dispose (struct thread *owner, void *base_, size_t amount)
{
  uint8_t *base = base_;
  
  ASSERT (owner != NULL);
  ASSERT (base != NULL);
  ASSERT (pg_ofs (base) == 0);
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
        
      swap_dispose_page (ee);
      
      base += PGSIZE;
    }
  while (--amount > 0);
  lock_release (&swap_lock);
  return true;
}

bool
swap_read_and_retain (struct thread *owner,
                      void          *base,
                      void          *dest)
{
  ASSERT (intr_get_level () == INTR_ON);
  ASSERT (owner != NULL);
  ASSERT (base != NULL);
  ASSERT (pg_ofs (base) == 0);
  
  lock_acquire (&swap_lock);
  
  struct swapped_page *ee = swappage_page_of_owner (owner, base);
  if (!ee)
    {
      lock_release (&swap_lock);
      return false;
    }
  
  swap_read_sectors (swapped_page_id (ee), dest);
  lru_use (&unmodified_pages, &ee->unmodified_pages_elem);
  
  lock_release (&swap_lock);
  return true;
}

static unsigned
swapped_page_hash (const struct hash_elem *e, void *t)
{
  typedef char _CASSERT[0 - !(sizeof (unsigned) == sizeof (void *))];
  ASSERT (e != NULL);
  ASSERT (t != NULL);
  struct swapped_page *ee = hash_entry (e, struct swapped_page, hash_elem);
  ASSERT (ee->thread == t);
  return (unsigned) ee->base;
}

static bool
swapped_page_less (const struct hash_elem *a,
                   const struct hash_elem *b,
                   void *t)
{
  return swapped_page_hash (a, t) < swapped_page_hash (b, t);
}

void
swap_init_thread (struct thread *owner)
{
  ASSERT (owner != NULL);
  //printf ("   INITIALISIERE SWAP FÜR %8p.\n", owner);
  hash_init (&owner->swap_pages, &swapped_page_hash, &swapped_page_less, owner);
}

static void
swap_clean_sub (struct hash_elem *e, void *t)
{
  ASSERT (e != NULL);
  struct swapped_page *ee = hash_entry (e, struct swapped_page, hash_elem);
  ASSERT (ee->thread == t);
  swap_dispose_page_real (ee);
}

void
swap_clean (struct thread *owner)
{
  ASSERT (owner != NULL);
  //printf ("   CLEANE SWAP VON %8p.\n", owner);
  
  lock_acquire (&swap_lock);
  hash_destroy (&owner->swap_pages, &swap_clean_sub);
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

bool
swap_must_retain (struct thread *owner,
                  void          *base,
                  size_t         amount)
{
  ASSERT (owner != NULL);
  ASSERT (base != NULL);
  ASSERT ((uintptr_t) base % PGSIZE == 0);
  ASSERT (amount > 0);
  
  lock_acquire (&swap_lock);
  do
    {
      struct swapped_page *ee = swappage_page_of_owner (owner, base);
      if (!ee)
        {
          lock_release (&swap_lock);
          return false;
        }
      lru_dispose (&unmodified_pages, &ee->unmodified_pages_elem, false);
    }
  while (--amount > 0);
  lock_release (&swap_lock);
  return true;
}
