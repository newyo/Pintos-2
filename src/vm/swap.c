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

struct swap_page
{
  struct lru_elem   lru_elem;
  struct thread    *thread;
  struct hash_elem  hash_elem;
  void             *user_addr;
};

struct lock swap_lock;
struct block *swap_disk;
size_t swap_pages_count;

#define PG_SECTOR_RATIO (PGSIZE / BLOCK_SECTOR_SIZE)
char _CASSERT_INT_PG_SECTOR_RATIO[0 - !(PGSIZE % BLOCK_SECTOR_SIZE == 0)];

struct bitmap *used_pages; // false == free
struct lru swap_lru; // list of struct swap_page
struct swap_page *swapped_pages_space;

static swap_t
swap_page_get_id (struct swap_page *cur)
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

static struct swap_page *
swap_get_page_of_owner (struct thread *owner, void *user_addr)
{
  ASSERT (owner != NULL);
  ASSERT (user_addr != NULL);
  
  struct swap_page key;
  memset (&key.hash_elem, 0, sizeof (key.hash_elem));
  key.thread = owner;
  key.user_addr = user_addr;
  struct hash_elem *e = hash_find (&owner->swap_pages, &key.hash_elem);
  if (e == NULL)
    return NULL;
    
  struct swap_page *ee = hash_entry (e, struct swap_page, hash_elem);
  ASSERT (ee->thread == owner);
  ASSERT (ee->user_addr == user_addr);
  return ee;
}

#define is_valid_swap_id(ID) \
({ \
  __typeof (ID) _x = (ID); \
  _x != SWAP_FAIL && _x < swap_pages_count; \
})

static void UNUSED
swap_needlessly_zero_out_whole_swap_space (void)
{
  char zero[BLOCK_SECTOR_SIZE];
  memset (zero, 0xCC, sizeof (zero));
  
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
  
  swapped_pages_space = calloc (swap_pages_count, sizeof (struct swap_page));
  if (!swapped_pages_space)
    PANIC ("Could not set up swapping: Memory exhausted (1)");
  
  used_pages = bitmap_create (swap_pages_count);
  if (!used_pages)
    PANIC ("Could not set up swapping: Memory exhausted (2)");
  lru_init (&swap_lru, 0, NULL, NULL);
  
#ifndef NDEBUG
  swap_needlessly_zero_out_whole_swap_space ();
#endif
  
  printf ("Initialized swapping.\n");
}

static void
swap_dispose_page_real (struct swap_page *ee)
{
  ASSERT (ee != NULL);
  ASSERT (ee->thread != NULL);
  ASSERT (ee->user_addr != NULL);
  
  lru_dispose (&swap_lru, &ee->lru_elem, false);
  memset (ee, 0, sizeof (*ee));
  bitmap_reset (used_pages, swap_page_get_id (ee));
}

static void
swap_dispose_page (struct swap_page *ee)
{
  ASSERT (ee != NULL);
  
  struct hash_elem *e UNUSED;
  e = hash_delete (&ee->thread->swap_pages, &ee->hash_elem);
  ASSERT (e != NULL);
  
  swap_dispose_page_real (ee);
}

static swap_t
swap_get_disposable_page (void)
{
  ASSERT (lock_held_by_current_thread (&swap_lock));
  
  size_t result = bitmap_scan (used_pages, 0, 1, false);
  if (result != BITMAP_ERROR)
    return result;
  
  struct lru_elem *e = lru_peek_least (&swap_lru);
  if (e == NULL) // swap space is exhausted
    return SWAP_FAIL;
  
  struct swap_page *ee;
  ee = lru_entry (e, struct swap_page, lru_elem);
  ASSERT (ee != NULL);
  
  vm_swap_disposed (ee->thread, ee->user_addr);
  swap_dispose_page (ee);
  
  return swap_page_get_id (ee);
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
            void          *user_addr,
            void          *src)
{
  ASSERT (intr_get_level () == INTR_ON);
  ASSERT (owner != NULL);
  ASSERT (user_addr != NULL);
  ASSERT (pg_ofs (user_addr) == 0);
  ASSERT (src != NULL);
  ASSERT (lock_held_by_current_thread (&swap_lock));
  
  printf ("SWAPPING %8p TO %04u\n", user_addr, id);
  
  ASSERT (is_valid_swap_id (id));
  
  struct swap_page *ee = &swapped_pages_space[id];
  if (ee->thread == NULL)
    {
      ASSERT (ee->user_addr == NULL);
      ASSERT (!bitmap_test (used_pages, id));
      
      bitmap_mark (used_pages, id);
      ee->thread = owner;
      ee->user_addr = user_addr;
      hash_insert (&owner->swap_pages, &ee->hash_elem);
    }
  else
    {
      printf (" ee->thread=%8p, owner=%8p\n", ee->thread, owner);
      ASSERT (ee->thread == owner);
      ASSERT (ee->user_addr == user_addr);
      ASSERT (bitmap_test (used_pages, id));
    }
  lru_use (&swap_lru, &ee->lru_elem);
  
  block_sector_t sector = swap_page_to_sector (id);
  int i;
  for (i = 0; i < PGSIZE/BLOCK_SECTOR_SIZE; ++i)
    {
      block_write (swap_disk, sector, src);
      src += BLOCK_SECTOR_SIZE;
      ++sector;
    }
}

bool
swap_alloc_and_write (struct thread *owner,
                      void          *user_addr,
                      void          *src)
{
  ASSERT (intr_get_level () == INTR_ON);
  ASSERT (owner != NULL);
  ASSERT (user_addr != NULL);
  ASSERT (pg_ofs (user_addr) == 0);
  ASSERT (src != NULL);
  
  lock_acquire (&swap_lock);
  
  struct swap_page *ee = swap_get_page_of_owner (owner, user_addr);
  if (ee != NULL)
    {
      swap_write (swap_page_get_id (ee), owner, user_addr, src);
      lock_release (&swap_lock);
      return true;
    }
  
  swap_t id = swap_get_disposable_page ();
  if (id == SWAP_FAIL)
    {
      lock_release (&swap_lock);
      return false;
    }
  swap_write (id, owner, user_addr, src);
  lock_release (&swap_lock);
  return true;
}

bool
swap_dispose (struct thread *owner, void *user_addr)
{
  ASSERT (owner != NULL);
  ASSERT (user_addr != NULL);
  ASSERT (pg_ofs (user_addr) == 0);
  
  lock_acquire (&swap_lock);
  
  struct swap_page *ee = swap_get_page_of_owner (owner, user_addr);
  if (!ee)
    {
      lock_release (&swap_lock);
      return false;
    }
  swap_dispose_page (ee);
  
  lock_release (&swap_lock);
  return true;
}

bool
swap_read_and_retain (struct thread *owner,
                      void          *user_addr,
                      void          *dest)
{
  ASSERT (intr_get_level () == INTR_ON);
  ASSERT (owner != NULL);
  ASSERT (user_addr != NULL);
  ASSERT (pg_ofs (user_addr) == 0);
  ASSERT (dest != NULL);
  
  lock_acquire (&swap_lock);
  
  struct swap_page *ee = swap_get_page_of_owner (owner, user_addr);
  if (!ee)
    {
      lock_release (&swap_lock);
      return false;
    }
  
  block_sector_t sector = swap_page_to_sector (swap_page_get_id (ee));
  int i;
  for (i = 0; i < PGSIZE/BLOCK_SECTOR_SIZE; ++i)
    {
      block_read (swap_disk, sector, dest);
      dest += BLOCK_SECTOR_SIZE;
      ++sector;
    }
  lru_use (&swap_lru, &ee->lru_elem);
  
  lock_release (&swap_lock);
  return true;
}

static unsigned
swap_page_hash (const struct hash_elem *e, void *t)
{
  typedef char _CASSERT[0 - !(sizeof (unsigned) == sizeof (void *))];
  ASSERT (e != NULL);
  ASSERT (t != NULL);
  struct swap_page *ee = hash_entry (e, struct swap_page, hash_elem);
  ASSERT (ee->thread == t);
  return (unsigned) ee->user_addr;
}

static bool
swap_page_less (const struct hash_elem *a,
                   const struct hash_elem *b,
                   void *t)
{
  return swap_page_hash (a, t) < swap_page_hash (b, t);
}

void
swap_init_thread (struct thread *owner)
{
  ASSERT (owner != NULL);
  //printf ("   INITIALISIERE SWAP FÜR %8p.\n", owner);
  hash_init (&owner->swap_pages, &swap_page_hash, &swap_page_less, owner);
}

static void
swap_clean_sub (struct hash_elem *e, void *t)
{
  ASSERT (e != NULL);
  struct swap_page *ee = hash_entry (e, struct swap_page, hash_elem);
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
  size_t unmodified = lru_usage (&swap_lru);
  return allocated - unmodified;
}

bool
swap_must_retain (struct thread *owner,
                  void          *user_addr)
{
  ASSERT (owner != NULL);
  ASSERT (user_addr != NULL);
  ASSERT (pg_ofs (user_addr) == 0);
  
  lock_acquire (&swap_lock);
  
  struct swap_page *ee = swap_get_page_of_owner (owner, user_addr);
  if (!ee)
    {
      lock_release (&swap_lock);
      return false;
    }
  lru_dispose (&swap_lru, &ee->lru_elem, false);
  
  lock_release (&swap_lock);
  return true;
}
