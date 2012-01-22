#include "swap.h"
#include <string.h>
#include <limits.h>
#include <list.h>
#include <hash.h>
#include <stdio.h>
#include "threads/vaddr.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "userprog/process.h"
#include "devices/block.h"
#include "lru.h"
#include "vm.h"
#include "crc32.h"
#include "allocator.h"

struct swap_page
{
  swap_t            id;
  struct hash_elem  id_elem;
  
  struct lru_elem   lru_elem;
  struct thread    *thread;
  struct hash_elem  hash_elem;
  void             *user_addr;
  uint32_t          cksum;
};

struct block *swap_disk;
size_t swap_pages_count;

#define PG_SECTOR_RATIO (PGSIZE / BLOCK_SECTOR_SIZE)
char _CASSERT_INT_PG_SECTOR_RATIO[0 - !(PGSIZE % BLOCK_SECTOR_SIZE == 0)];

struct bitmap    *used_pages; // false == free
struct lru        swap_lru; // list of struct swap_page
struct allocator  pages_allocator;
struct hash       pages_hash;

static inline block_sector_t
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

static void UNUSED
swap_needlessly_zero_out_whole_swap_space (void)
{
  /*
  char zero[BLOCK_SECTOR_SIZE];
  memset (zero, 0xCC, sizeof (zero));
  
  block_sector_t i;
  for (i = 0; i < swap_pages_count; ++i)
    block_write (swap_disk, i, zero);
  */
}

static unsigned
swap_id_hash (const struct hash_elem *e, void *aux UNUSED)
{
  typedef char _CASSERT[0 - !(sizeof (unsigned) == sizeof (swap_t))];
  ASSERT (e != NULL);
  
  struct swap_page *ee = hash_entry (e, struct swap_page, id_elem);
  ASSERT (ee->id != SWAP_FAIL);
  return (unsigned) ee->id;
}

static bool
swap_id_less (const struct hash_elem *a,
              const struct hash_elem *b,
              void *aux UNUSED)
{
  return swap_id_hash (a, NULL) < swap_id_hash (b, NULL);
}

void
swap_init (void)
{
  swap_disk = block_get_role (BLOCK_SWAP);
  ASSERT (swap_disk != NULL);
  
  swap_pages_count = block_size (swap_disk) / PG_SECTOR_RATIO;
  ASSERT (swap_pages_count > 0);
  
  if (!allocator_init (&pages_allocator, false, swap_pages_count,
                       sizeof (struct swap_page)))
    PANIC ("Could not set up swapping: Memory exhausted (1)");
  
  used_pages = bitmap_create (swap_pages_count);
  if (!used_pages)
    PANIC ("Could not set up swapping: Memory exhausted (2)");
  lru_init (&swap_lru, 0, NULL, NULL);
  
  hash_init (&pages_hash, &swap_id_hash, &swap_id_less, NULL);
  
  swap_needlessly_zero_out_whole_swap_space ();
  
  printf ("Initialized swapping.\n");
}

static void
swap_page_free (struct swap_page *ee)
{
  ASSERT (ee != NULL);
  
  if (ee->thread)
    hash_delete (&ee->thread->swap_pages, &ee->hash_elem);
  lru_dispose (&swap_lru, &ee->lru_elem, false);
  bitmap_reset (used_pages, ee->id);
  
  memset (ee, 0, sizeof (*ee));
  allocator_free (&pages_allocator, ee, 1);
}

static struct swap_page *
swap_page_alloc (void)
{
  size_t result;
  
  int i;
  for (i = 0; i < 2; ++i)
    {
      result = bitmap_scan (used_pages, 0, 1, false);
      if (result != BITMAP_ERROR)
        break;
      
      struct lru_elem *e = lru_peek_least (&swap_lru);
      if (e == NULL) // swap space is exhausted
        return NULL;
        
      struct swap_page *ee;
      ee = lru_entry (e, struct swap_page, lru_elem);
      ASSERT (ee != NULL);
      
      vm_swap_disposed (ee->thread, ee->user_addr);
      swap_page_free (ee);
    }
  if (result == BITMAP_ERROR)
    return NULL;
    
  struct swap_page *page = allocator_alloc (&pages_allocator, 1);
  ASSERT (page != NULL);
  memset (page, 0, sizeof (*page));
  page->id = result;
  return page;
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
  
  struct swap_page *ee = swap_get_page_of_owner (owner, user_addr);
  if (ee == NULL)
    {
      ee = swap_page_alloc ();
      if (ee == NULL)
        return false;
      ee->thread = owner;
      ee->user_addr = user_addr;
      hash_insert (&owner->swap_pages, &ee->hash_elem);
    }
  
  ee->cksum = cksum (src, PGSIZE);
  block_sector_t sector = swap_page_to_sector (ee->id);
  int i;
  for (i = 0; i < PGSIZE/BLOCK_SECTOR_SIZE; ++i)
    {
      block_write (swap_disk, sector, src);
      src += BLOCK_SECTOR_SIZE;
      ++sector;
    }
  
  //printf ("[OUT] %p  -> %4x (0x%8x)\n", ee->user_addr, ee->id, ee->cksum);
    
  bitmap_mark (used_pages, ee->id);
  return true;
}

bool
swap_dispose (struct thread *owner, void *user_addr)
{
  ASSERT (owner != NULL);
  ASSERT (user_addr != NULL);
  ASSERT (pg_ofs (user_addr) == 0);
  
  struct swap_page *ee = swap_get_page_of_owner (owner, user_addr);
  if (ee == NULL)
    return false;
  swap_page_free (ee);
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
  
  struct swap_page *ee = swap_get_page_of_owner (owner, user_addr);
  if (ee == NULL)
    return false;
  
  block_sector_t sector = swap_page_to_sector (ee->id);
  int i;
  uint8_t *v = dest;
  for (i = 0; i < PGSIZE/BLOCK_SECTOR_SIZE; ++i)
    {
      block_read (swap_disk, sector, v);
      v += BLOCK_SECTOR_SIZE;
      ++sector;
    }
  
  uint32_t read_cksum = cksum (dest, PGSIZE);
  bool result = read_cksum == ee->cksum;
  
  if (!result)
    printf ("\n"
            "WARNING: Checksum mismatch in swap page %04u @ %p.\n"
            "Memory: %p, expected: 0x%08x, calcuted: 0x%08x\n"
            "EXPECT ERRORS!\n"
            "\n",
            ee->id, dest, user_addr, ee->cksum, read_cksum);
  
  //printf ("[IN ] %p <-  %4x (0x%8x)\n", ee->user_addr, ee->id, read_cksum);
            
  if (result)
    lru_use (&swap_lru, &ee->lru_elem);
  else
    swap_page_free (ee);
  return result;
}

static unsigned
swap_page_hash (const struct hash_elem *e, void *t UNUSED)
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
swap_clean_sub (struct hash_elem *e, void *t UNUSED)
{
  ASSERT (e != NULL);
  struct swap_page *ee = hash_entry (e, struct swap_page, hash_elem);
  ASSERT (ee->thread == t);
  swap_page_free (ee);
}

void
swap_clean (struct thread *owner)
{
  ASSERT (owner != NULL);
  hash_destroy (&owner->swap_pages, &swap_clean_sub);
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
  
  struct swap_page *ee = swap_get_page_of_owner (owner, user_addr);
  if (ee == NULL)
    return false;
  lru_dispose (&swap_lru, &ee->lru_elem, false);
  return true;
}
