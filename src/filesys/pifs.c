#include "pifs.h"
#include "bitset.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <debug.h>
#include <round.h>
#include <limits.h>

#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/interrupt.h"

#define PIFS_NAME_LENGTH 16

//#define PIFS_DEBUG(...) printf (__VA_ARGS__)
#define PIFS_DEBUG(...)

#define MAGIC4(C)                      \
({                                     \
  __extension__ const char _c[] = (C); \
  __extension__ uint32_t _r = 0;       \
  _r |= _c[3];                         \
  _r <<= 8;                            \
  _r |= _c[2];                         \
  _r <<= 8;                            \
  _r |= _c[1];                         \
  _r <<= 8;                            \
  _r |= _c[0];                         \
  _r;                                  \
})

typedef uint32_t pifs_magic;
#define PIFS_MAGIC_HEADER MAGIC4 ("PIFS")
#define PIFS_MAGIC_FOLDER MAGIC4 ("FLDR")
#define PIFS_MAGIC_FILE   MAGIC4 ("FILE")
#define PIFS_MAGIC_NAME   MAGIC4 ("NAME")

typedef uint32_t pifs_ptr;
typedef char _CASSERT_PIFS_PTR_SIZE[0 - !(sizeof (pifs_ptr) ==
                                          sizeof (block_sector_t))];

#define PIFS_COUNT_USED_MAP_ENTRIES 493 /* ~250 kB per block */
#define PIFS_COUNT_FOLDER_ENTRIES 24
#define PIFS_COUNT_FILE_BLOCKS 98 /* ~50 kB per block (w/ max. fragmentation) */
                                  /* ~12 MB per block (w/ min. fragmentation) */
#define PIFS_COUNT_LONG_NAME_CHARS 491
#define PIFS_COUNT_FILE_REF_COUNT_MAX 255

struct pifs_inode_header
{
  pifs_magic        magic;
  pifs_ptr          extends; // pointer to overflow bucket
  pifs_ptr          parent_folder;
  struct pifs_attrs attrs; // not implemented
  pifs_ptr          long_name; // not implemented
} PACKED;

struct pifs_long_name // not implemented by us, but for completeness
{
  pifs_magic        magic;
  pifs_ptr          unused1; // unused for this inode type
  pifs_ptr          unused2; // unused for this inode type
  struct pifs_attrs unused3; // unused for this inode type
  pifs_ptr          unused4; // unused for this inode type
  
  uint32_t          total_len;
  char              used_map[PIFS_COUNT_LONG_NAME_CHARS];
} PACKED;

struct pifs_header
{
  pifs_magic        magic;
  pifs_ptr          extends; // if there are too many blocks
  pifs_ptr          root_folder;
  struct pifs_attrs unused; // unused for this inode type
  pifs_ptr          long_name; // name of device (not implemented)
  
  uint16_t          blocks_count;
  char              used_map[PIFS_COUNT_USED_MAP_ENTRIES];
} PACKED;

struct pifs_folder_entry
{
  char     name[PIFS_NAME_LENGTH];
  pifs_ptr block;
} PACKED;

struct pifs_folder
{
  pifs_magic               magic;
  pifs_ptr                 extends; // if there are too many files
  pifs_ptr                 parent_folder; // 0 for root
  struct pifs_attrs        attrs; // not implemented
  pifs_ptr                 long_name; // not implemented
  
  char                     padding[14];
  
  uint8_t                  entries_count;
  // TODO: An optimization would be sorting the entries in an extend.
  //       I don't think sorting over all extends would be much of a speed-up.
  struct pifs_folder_entry entries[PIFS_COUNT_FOLDER_ENTRIES];
} PACKED;

struct pifs_file_block_ref
{
  pifs_ptr start;
  uint8_t  count;
} PACKED;

struct pifs_file
{
  pifs_magic                 magic;
  pifs_ptr                   extends; // if there are too many blocks
  pifs_ptr                   parent_folder;
  struct pifs_attrs          attrs; // not implemented
  pifs_ptr                   long_name; // not implemented
  
  uint32_t                   length;
  
  uint8_t                    blocks_count;
  struct pifs_file_block_ref blocks[PIFS_COUNT_FILE_BLOCKS];
} PACKED;

typedef char _CASSERT_PIFS_NAME_SIZE[0 - !(sizeof (struct pifs_long_name) ==
                                           BLOCK_SECTOR_SIZE)];
typedef char _CASSERT_PIFS_HEADER_SIZE[0 - !(sizeof (struct pifs_header) ==
                                             BLOCK_SECTOR_SIZE)];
typedef char _CASSERT_PIFS_FOLDER_SIZE[0 - !(sizeof (struct pifs_folder) ==
                                             BLOCK_SECTOR_SIZE)];
typedef char _CASSERT_PIFS_FILE_SIZE[0 - !(sizeof (struct pifs_file) ==
                                           BLOCK_SECTOR_SIZE)];

struct pifs_deletor_item
{
  struct pifs_inode *inode;
  struct list_elem   elem;
};

static void NO_RETURN
pifs_deletor_fun (void *pifs_)
{ 
  struct pifs_device *pifs = pifs_;
  ASSERT (intr_get_level () == INTR_ON);
  
  for(;;)
    {
      // retrieve an item from deletor_list:
      
      sema_down (&pifs->deletor_sema);
      intr_disable (); // file close must not use locks
      struct list_elem *e = list_pop_front (&pifs->deletor_list);
      intr_enable ();
      
      struct pifs_deletor_item *ee;
      ee = list_entry (e, struct pifs_deletor_item, elem);
      struct pifs_inode *inode = ee->inode;
      free (ee);
        
      // proceed:
        
      ASSERT (inode != NULL);
      ASSERT (inode->pifs == pifs);
      rwlock_acquire_write (&pifs->pifs_rwlock);
      
      if (inode->open_count > 0)
        {
          // someone opened the file after its last inode was closed
          continue;
        }
      
      if (!inode->deleted)
        {
          // not accessable anymore:
          
          struct hash_elem *hash_e UNUSED;
          hash_e = hash_delete (&pifs->open_inodes, &inode->elem);
          ASSERT (hash_e == &inode->elem);
        }
      else
        {
          // Inode is already removed from hash.
          // Inode is already removed from parent folder.
          
          // mark allocated blocks as free:
          
          // TODO
        }
      free (inode);
      
      rwlock_release_write (&pifs->pifs_rwlock);
    }
}

static unsigned
pifs_open_inodes_hash (const struct hash_elem *e, void *pifs)
{
  ASSERT (e != NULL);
  struct pifs_inode *ee = hash_entry (e, struct pifs_inode, elem);
  typedef char _CASSERT[0 - !(sizeof (unsigned) == sizeof (ee->sector))];
  ASSERT (ee->pifs == pifs);
  ASSERT (ee->sector != 0);
  return (unsigned) ee->sector;
}

static bool
pifs_open_inodes_less (const struct hash_elem *a,
                       const struct hash_elem *b,
                       void                   *pifs)
{
  return pifs_open_inodes_hash (a, pifs) < pifs_open_inodes_hash (b, pifs);
}

bool
pifs_init (struct pifs_device *pifs, struct block_cache *bc)
{
  ASSERT (pifs != NULL);
  ASSERT (bc != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  memset (pifs, 0, sizeof (*pifs));
  pifs->bc = bc;
  hash_init (&pifs->open_inodes, &pifs_open_inodes_hash, &pifs_open_inodes_less,
             pifs);
  rwlock_init (&pifs->pifs_rwlock);
  sema_init (&pifs->deletor_sema, 0);
  list_init (&pifs->deletor_list);
  
  thread_create ("[PIFS-DELETOR]", PRI_MAX, &pifs_deletor_fun, pifs);
  
  pifs->header_block = block_cache_read (pifs->bc, 0);
  ASSERT (pifs->header_block != NULL);
  
  return true;
}

void
pifs_destroy (struct pifs_device *pifs)
{
  ASSERT (pifs != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  // TODO
}

static inline struct pifs_header *
pifs_header (struct pifs_device *pifs)
{
  return (struct pifs_header *) &pifs->header_block->data;
}

bool
pifs_format (struct pifs_device *pifs)
{
  ASSERT (pifs != NULL);
  ASSERT (pifs->bc != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  block_sector_t blocks = block_size (pifs->bc->device);
  if (blocks % 8 != 0)
    {
      printf ("PIFS will waste %u of %u blocks.\n", blocks % 8, blocks);
      blocks -= blocks % 8;
    }
  if (blocks < 2 || DIV_ROUND_UP (blocks, PIFS_COUNT_USED_MAP_ENTRIES) >
      PIFS_COUNT_USED_MAP_ENTRIES - 1) // ~124 MB
    return false;
  
  // write file system headers:
  
  struct pifs_header *const header = pifs_header (pifs);
  
  auto void init_header (struct block_page *page);
  void
  init_header (struct block_page *page)
  {
    ASSERT (page != 0);
    struct pifs_header *h = (void *) &page->data;
    
    memset (h, 0, sizeof (*h));
    h->magic = PIFS_MAGIC_HEADER;
    if (blocks >= PIFS_COUNT_USED_MAP_ENTRIES)
      h->blocks_count = PIFS_COUNT_USED_MAP_ENTRIES;
    else
      h->blocks_count = blocks;
  }
  
  PIFS_DEBUG ("PIFS is formatting free-maps.\n");
  
  init_header (pifs->header_block);
  bitset_mark (header->used_map, 0);
  
  pifs_ptr nth_block = 1;
  if (blocks > PIFS_COUNT_USED_MAP_ENTRIES)
    {
      struct block_page *page = NULL, *last_page = NULL;
      
      do
        {
          blocks -= PIFS_COUNT_USED_MAP_ENTRIES;
          
          if (last_page != NULL)
            {
              struct pifs_header *last_header = (void *) &last_page->data;
              last_header->extends = nth_block;
              last_page->dirty = true;
              block_cache_return (pifs->bc, last_page);
            }
          else
            {
              header->extends = nth_block;
            }
          last_page = page;
          
          page = block_cache_write (pifs->bc, nth_block);
          init_header (page);
          bitset_mark (header->used_map, nth_block);
          
          ++nth_block;
        }
      while (blocks > PIFS_COUNT_USED_MAP_ENTRIES);
      
      if (last_page != NULL)
        {
          last_page->dirty = true;
          block_cache_return (pifs->bc, last_page);
        }
      page->dirty = true;
      block_cache_return (pifs->bc, page);
    }
  
  // write root directory:
  
  PIFS_DEBUG ("PIFS is formatting root directory (%u).\n", nth_block);
   
  bitset_mark (header->used_map, nth_block);
  header->root_folder = nth_block;
  struct block_page *page = block_cache_write (pifs->bc, nth_block);
  ASSERT (page != NULL);
  struct pifs_folder *root = (void *) &page->data;
  
  memset (root, 0, sizeof (*root));
  root->magic = PIFS_MAGIC_FOLDER;
  
  page->dirty = true;
  block_cache_return (pifs->bc, page);
  
  pifs->header_block->dirty = true;
  return true;
}

bool
pifs_sanity_check (struct pifs_device *pifs)
{
  ASSERT (pifs != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  const struct pifs_header *header = (void *) &pifs->header_block->data;
  return (header->magic == PIFS_MAGIC_HEADER) &&
         (header->blocks_count > 1) &&
         (header->used_map[0] & 1);
}

static block_sector_t
pifs_open_traverse (struct pifs_device  *pifs,
                    pifs_ptr             cur, 
                    const char         **path_)
{
  ASSERT (pifs != NULL);
  ASSERT (path_ != NULL && *path_ != NULL);
  
  PIFS_DEBUG ("pifs_open_traverse (%p, %u, &\"%s\")\n",
              pifs, cur, *path_);
  
  if (**path_ == '/')
    ++*path_; // strip leading slash
    
  if (**path_ == 0)
    {
      // we hit end, a folder was looked up
      return cur;
    }
  
  const char *next = strchrnul (*path_, '/');
  size_t path_elem_len = (uintptr_t) (next - *path_);
  if (path_elem_len > PIFS_NAME_LENGTH)
    {
      // path element is invalid, as it is longer than PIFS_NAME_LENGTH
      return 0;
    }
  
  if (path_elem_len == 0 || (path_elem_len == 1 && **path_ == '.'))
    {
      // stay in current folder if '//' or '/./' was found
      *path_ = next;
      if (*next == 0)
        return cur;
      return pifs_open_traverse (pifs, cur, path_);
    }
    
  struct block_page *page = block_cache_read (pifs->bc, cur);
  struct pifs_inode_header *header = (void *) &page->data;
  if (header->magic != PIFS_MAGIC_FOLDER)
    {
      if (header->magic == PIFS_MAGIC_FILE)
        {
          // we are hit a file, but the path indicated it was a folder
          block_cache_return (pifs->bc, page);
          return 0;
        }
      PANIC ("Block %"PRDSNu" (%p) of filesystem is messed up "
             "(magic = 0x%08X).", cur, header, header->magic);
    }
  
  if (path_elem_len == 2 && memcmp (*path_, "..", 2) == 2)
    {
      // go to upper folder
      block_cache_return (pifs->bc, page);
      if (header->parent_folder != 0)
        cur = header->parent_folder;
      *path_ = next;
      if (*next == 0)
        return cur;
      return pifs_open_traverse (pifs, cur, path_);
    }
    
  for (;;)
    {
      struct pifs_folder *folder = (void *) header;
      if (folder->entries_count > PIFS_COUNT_FOLDER_ENTRIES)
        PANIC ("Block %"PRDSNu" of filesystem is messed up.", cur);
        
      PIFS_DEBUG ("PIFS: In %u, looking for '%.*s'. Folder size = %u.\n",
                  cur, path_elem_len, *path_, folder->entries_count);
      
      unsigned i;
      for (i = 0; i < folder->entries_count; ++i)
        {
          const char *name = &folder->entries[i].name[0];
          PIFS_DEBUG ("    %u contains '%.*s'.\n", cur, PIFS_NAME_LENGTH, name);
          if (!((memcmp (name, *path_, path_elem_len) == 0) &&
                (path_elem_len < PIFS_NAME_LENGTH ? name[path_elem_len] == 0
                                                  : true)))
            continue;
            
          // found!
            
          pifs_ptr next_block = folder->entries[i].block;
          PIFS_DEBUG ("  Found '%.*s' in %u. Next '%s'.\n", PIFS_NAME_LENGTH,
                      name, next_block, next);
          if (next_block == 0)
            PANIC ("Block %"PRDSNu" of filesystem is messed up.", cur);
          block_cache_return (pifs->bc, page);
          
          *path_ = next;
          if (*next == 0)
            return next_block;
            
          // We are not finished yet
          return pifs_open_traverse (pifs, next_block, path_); // TCO
        }
          
      pifs_ptr extends = folder->extends;
      block_cache_return (pifs->bc, page);
      if (extends == 0)
        {
          PIFS_DEBUG ("  Nothing found for '%s' in %u.\n", *path_, cur);
          return cur;
        }
        
      page = block_cache_read (pifs->bc, extends);
      header = (void *) &page->data;
      if (header->magic != PIFS_MAGIC_FOLDER)
        PANIC ("Block %"PRDSNu" of filesystem is messed up (magic = 0x%08X).",
               cur, header->magic);
    }
}

static struct pifs_inode *
pifs_alloc_inode (struct pifs_device  *pifs, pifs_ptr cur)
{
  ASSERT (pifs != NULL);
  ASSERT (cur != 0);
  
  struct pifs_inode *result = malloc (sizeof (*result));
  if (!result)
    return NULL;
  memset (result, 0, sizeof (*result));
  result->pifs = pifs;
  result->sector = cur;
  
  struct block_page *page = block_cache_read (pifs->bc, cur);
  struct pifs_folder *folder = (void *) &page->data;
  if (folder->magic == PIFS_MAGIC_FOLDER)
    {
      result->is_directory = true;
      for (;;)
        {
          if (folder->entries_count > PIFS_COUNT_FOLDER_ENTRIES)
            PANIC ("Block %"PRDSNu" of filesystem is messed up.", cur);
          result->length += folder->entries_count;
          
          cur = folder->extends;
          if (cur == 0)
            break;
          block_cache_return (pifs->bc, page);
          
          page = block_cache_read (pifs->bc, cur);
          folder = (void *) &page->data;
          if (folder->magic != PIFS_MAGIC_FOLDER)
            PANIC ("Block %"PRDSNu" of filesystem is messed up "
                   "(magic = 0x%08X).", cur, folder->magic);
        }
    }
  else if (folder->magic == PIFS_MAGIC_FILE)
    {
      struct pifs_file *file = (struct pifs_file *) folder;
      result->is_directory = false;
      result->length = file->length;
    }
  else
    PANIC ("Block %"PRDSNu" of filesystem is messed up.", cur);
  block_cache_return (pifs->bc, page);
  
  if (result != NULL)
    {
      struct hash_elem *e UNUSED;
      e = hash_insert (&pifs->open_inodes, &result->elem);
      ASSERT (e == NULL);
    }
  
  return result;
}

static pifs_ptr
pifs_alloc_block (struct pifs_device *pifs)
{
  pifs_ptr cur = 0, result = 0, offset = 0;
  do
    {
      struct block_page *page = block_cache_read (pifs->bc, cur);
      struct pifs_header *header = (void *) &page->data;
      if (header->magic != PIFS_MAGIC_HEADER)
          PANIC ("Block %"PRDSNu" of filesystem is messed up "
                 "(magic = 0x%08X).", cur, header->magic);
      if (header->magic % 8 != 0)
          PANIC ("Block %"PRDSNu" of filesystem is messed up "
                 "(blocks_count = 0x%08X).", cur, header->blocks_count);
                 
      int len = header->blocks_count / 8;
      if (len > PIFS_COUNT_USED_MAP_ENTRIES)
          PANIC ("Block %"PRDSNu" of filesystem is messed up "
                 "(apparent len = %u).", cur, len);
                 
      off_t bit_result = bitset_find_and_set_1 (&header->used_map[0], len);
      if (bit_result > 0)
        {
          result = bit_result + offset;
          page->dirty = true;
        }
      else
        {
          offset += PIFS_COUNT_USED_MAP_ENTRIES;
          cur = header->extends;
        }
      block_cache_return (pifs->bc, page);
    }
  while (result == 0 && cur != 0);
  return result;
}

struct pifs_alloc_multiple_item
{
  struct pifs_file_block_ref ref;
  struct list_elem           elem;
};

struct pifs_alloc_multiple_aux
{
  struct list *list;
  size_t       offset;
  size_t       result;
};

static void
pifs_alloc_multiple_cb (block_sector_t sector, void *aux_)
{
  struct pifs_alloc_multiple_aux *aux = aux_;
  
  ++aux->result;
  sector += aux->offset;
  ASSERT (sector != 0);
  
  /*
  static size_t next = 11;
  ASSERT (next ++ == sector);
  */
  
  struct pifs_alloc_multiple_item *ee;
  
  // add to prev. last block if possible:
  
  if (!list_empty (aux->list))
    {
      struct list_elem *e = list_back (aux->list);
      ee = list_entry (e, struct pifs_alloc_multiple_item, elem);
      if (ee->ref.start + ee->ref.count == sector &&
          ee->ref.count < PIFS_COUNT_FILE_BLOCKS)
        {
          ++ee->ref.count;
          return;
        }
    }
    
  // allocate new reference block:
    
  ee = malloc (sizeof (*ee));
  if (!ee)
    {
      --aux->result;
      return;
    }
  memset (ee, 0, sizeof (*ee));
  ee->ref.start = sector;
  ee->ref.count = 1;
  list_push_back (aux->list, &ee->elem);
}

// caller has to init. *list
static size_t
pifs_alloc_multiple (struct pifs_device *pifs,
                     size_t              amount,
                     struct list        *list)
{
  ASSERT (list != NULL);
  if (amount == 0)
    return 0;
    
  struct pifs_alloc_multiple_aux aux = {
    .list   = list,
    .offset = 0,
    .result = 0,
  };
  
  pifs_ptr cur = 0;
  do
    {
      struct block_page *page = block_cache_read (pifs->bc, cur);
      struct pifs_header *header = (void *) &page->data;
      if (header->magic != PIFS_MAGIC_HEADER)
          PANIC ("Block %"PRDSNu" of filesystem is messed up "
                 "(magic = 0x%08X).", cur, header->magic);
      if (header->magic % 8 != 0)
          PANIC ("Block %"PRDSNu" of filesystem is messed up "
                 "(blocks_count = 0x%08X).", cur, header->blocks_count);
                 
      int len = header->blocks_count / 8;
      if (len > PIFS_COUNT_USED_MAP_ENTRIES)
          PANIC ("Block %"PRDSNu" of filesystem is messed up "
                 "(apparent len = %u).", cur, len);
                 
      size_t left = bitset_find_and_set (&header->used_map[0], len, amount,
                                         pifs_alloc_multiple_cb, &aux);
      
      ASSERT (left <= amount);
      cur = header->extends;
      aux.offset += header->blocks_count;
      
      if (left != amount)
        {
          page->dirty = true;
          amount = left;
        }
      block_cache_return (pifs->bc, page);
    }
  while (amount > 0 && cur != 0);
  
  return aux.result;
}

static void
pifs_alloc_multiple_free_list (struct list *list)
{
  ASSERT (list != NULL);
  while (!list_empty (list))
    {
      struct list_elem *e = list_pop_front (list);
      struct pifs_alloc_multiple_item *ee;
      ee = list_entry (e, struct pifs_alloc_multiple_item, elem);
      free (ee);
    }
}

static struct block_page *
pifs_create (struct pifs_device  *pifs,
             pifs_ptr             parent_folder_ptr,
             const char          *name,
             size_t               name_len,
             pifs_ptr            *new_block_)
{
  ASSERT (pifs != NULL);
  ASSERT (parent_folder_ptr != 0);
  ASSERT (name != NULL);
  ASSERT (*name != 0);
  ASSERT (name_len > 0 && name_len <= PIFS_NAME_LENGTH);
  ASSERT (new_block_ != NULL);
  
  PIFS_DEBUG ("PIFS creating '%.*s' in %u.\n", name_len, name,
              parent_folder_ptr);
  
  // find space for new folder:
    
  pifs_ptr new_block = pifs_alloc_block (pifs);
  if (new_block == 0)
    return NULL;
    
  // open parent folder:
  
  struct block_page *page = block_cache_read (pifs->bc, parent_folder_ptr);
  if (page == NULL)
    {
      bitset_reset (&pifs_header (pifs)->used_map[0], new_block);
      return NULL;
    }
  struct pifs_folder *folder = (void *) &page->data;
  if (folder->magic != PIFS_MAGIC_FOLDER)
    PANIC ("Block %"PRDSNu" of filesystem is messed up (magic = 0x%08X).",
           parent_folder_ptr, folder->magic);
    
  // extend parent folder if needed:
  
  while (folder->entries_count >= PIFS_COUNT_FOLDER_ENTRIES)
    {
      // TODO: traverse extends and allocate a new extend if needed
      ASSERT (0);
      block_cache_return (pifs->bc, page);
    }
    
  // update parent folder's children list (or its extend):
  
  memcpy (folder->entries[folder->entries_count].name, name, name_len);
  if (name_len < PIFS_NAME_LENGTH)
    folder->entries[folder->entries_count].name[name_len] = 0;
  folder->entries[folder->entries_count].block = new_block;
  ++folder->entries_count;
  page->dirty = true;
  block_cache_return (pifs->bc, page);
  
  // update parent_folder inode:
  
  struct pifs_inode key;
  memset (&key, 0, sizeof (key));
  key.pifs = pifs;
  key.sector = parent_folder_ptr;
  struct hash_elem *e = hash_find (&pifs->open_inodes, &key.elem);
  if (e != NULL)
    {
      struct pifs_inode *parent_inode = hash_entry (e, struct pifs_inode, elem);
      ++parent_inode->length;
    }
    
  // return and clear page for new inode:
  
  page = block_cache_write (pifs->bc, new_block);
  ASSERT (page != NULL); // we made room for one page w/ a full rw lock
  memset (&page->data, 0, sizeof (page->data));
  page->dirty = true;
  struct pifs_inode_header *inode_header = (void *) &page->data;
  inode_header->parent_folder = parent_folder_ptr;
  
  *new_block_ = new_block;
  return page;
}

static struct pifs_inode *
pifs_open_rel (struct pifs_device  *pifs,
               const char          *path,
               enum pifs_open_opts  opts,
               pifs_ptr             root_sector)
{
  ASSERT (pifs != NULL);
  ASSERT (path != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  PIFS_DEBUG ("pifs_open_rel (%p, \"%s\", 0x%x, %u)\n",
              pifs, path, opts, root_sector);
  
  ASSERT ((opts & ~POO_MASK) == 0);
  ASSERT (!((opts & POO_MASK_FILE) && (opts & POO_MASK_FOLDER)));
  ASSERT (!((opts & POO_MASK_NO)   && (opts & POO_MASK_MUST)));
  ASSERT ((opts & POO_NO_CREATE) || ((opts & POO_MASK_FILE) ||
                                     (opts & POO_MASK_FOLDER)));

  if (!*path)
    return NULL;
  
  rwlock_acquire_write (&pifs->pifs_rwlock);
  
  pifs_ptr found_sector;
  found_sector = pifs_open_traverse (pifs, root_sector, &path);
  PIFS_DEBUG ("PIFS end path = '%s' (%u)\n", path, found_sector);
  
  struct pifs_inode *result = NULL;
  if (found_sector == 0)
    { // path was invalid
      goto end;
    }
  else if (path[0] == 0 || (path[0] == '/' && path[1] == 0))
    { // we found a file or folder
    
      // test if user's requirements were met:
      
      if (opts & POO_MASK_MUST)
        goto end;
        
      bool must_be_file   = (opts & POO_MASK_FILE);
      bool must_be_folder = (opts & POO_MASK_FOLDER) || (path[0] != 0);
      if (must_be_file && must_be_folder)
        goto end;
        
      // look if already open:
      
      struct pifs_inode key;
      memset (&key, 0, sizeof (key));
      key.pifs = pifs;
      key.sector = found_sector;
      struct hash_elem *e = hash_find (&pifs->open_inodes, &key.elem);
      if (e != NULL)
        {
          result = hash_entry (e, struct pifs_inode, elem);
          if (((must_be_file   &&  result->is_directory) ||
               (must_be_folder && !result->is_directory)))
            result = NULL; // it is indeed open, but not the kind of inode the
                           // user was looking for
          goto end;
        }
        
      // alloc an inode:
      
      result = pifs_alloc_inode (pifs, found_sector);
    }
  else
    {
      // We did not find a file or folder.
      // If there is only "abc" or "abc/" left, the path is valid for creation.
      // Otherwise path is invalid, as we do not support "mkdir -p".
      
      if (path[0] == '/')
        ++path;
      
      if (opts & POO_NO_CREATE)
        goto end;
      // POO_NO_CREATE not being set implies POO_MASK_FILE ^ POO_MASK_FOLDER
        
      // test path validity:
      
      bool must_be_file   = (opts & POO_MASK_FILE);
      bool must_be_folder = (opts & POO_MASK_FOLDER);
        
      const char *end = strchrnul (path, '/');
      if (end[0] == 0)
        {
          // points to simple a file/folder
        }
      else if (end[1] == 0)
        {
          // points to a folder
          must_be_folder = true;
          --end; // strip trailing slash
        }
      else
        {
          // path is of type abc/def ...
          goto end;
        }
        
      if (must_be_file == must_be_folder)
        goto end;
        
      uintptr_t elem_len = end - path;
      if (elem_len > PIFS_NAME_LENGTH)
        goto end; // invalid file name
        
      // create file or folder:
      
      pifs_ptr new_block;
      struct block_page *page;
      page = pifs_create (pifs, found_sector, path, elem_len, &new_block);
      if (!page)
        goto end;
      
      struct pifs_inode_header *inode_header = (void *) &page->data;
      inode_header->magic = must_be_folder ? PIFS_MAGIC_FOLDER
                                           : PIFS_MAGIC_FILE;
      block_cache_return (pifs->bc, page);
      
      result = pifs_alloc_inode (pifs, new_block);
    }

end:
  if (result)
    ++result->open_count;
  rwlock_release_write (&pifs->pifs_rwlock);
  return result;
}

struct pifs_inode *
pifs_open (struct pifs_device  *pifs,
           const char          *path,
           enum pifs_open_opts  opts)
{
  pifs_ptr root = pifs_header (pifs)->root_folder;
  return pifs_open_rel (pifs, path, opts, root);
}

struct pifs_inode *
pifs_open2 (struct pifs_device  *pifs,
            const char          *path,
            enum pifs_open_opts  opts,
            struct pifs_inode   *folder)
{
  ASSERT (path != NULL);
  
  if (folder == NULL)
    return pifs_open (pifs, path, opts);
  else if (!folder->is_directory)
    return NULL;
  else if (path[0] != '/')
    return pifs_open_rel (pifs, path, opts, folder->sector);
  else
    return pifs_open (pifs, path, opts);
}

void
pifs_close (struct pifs_inode *inode)
{
  if (inode == NULL)
    return;
    
  // Rational: We must not use locks in here as thread_exit may happen in a
  //           interrupt handler.
  
  enum intr_level old_level = intr_disable ();
  
  ASSERT (inode->open_count > 0);
  
  --inode->open_count;
  if (inode->open_count == 0)
    {
      struct pifs_deletor_item *item = malloc (sizeof (*item));
      if (item == NULL)
        PANIC ("PIFS: Out of memory.");
      memset (item, 0, sizeof (*item));
      item->inode = inode;
      
      list_push_back (&inode->pifs->deletor_list, &item->elem);
      sema_up (&inode->pifs->deletor_sema);
    }
  
  intr_set_level (old_level);
}

const char *
pifs_readdir (struct pifs_inode *inode, size_t index, off_t *len)
{
  ASSERT (inode != NULL);
  ASSERT (len != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  rwlock_acquire_read (&inode->pifs->pifs_rwlock);
  
  // TODO
  (void) index;
  
  rwlock_release_read (&inode->pifs->pifs_rwlock);
  return NULL;
}

static void
pifs_unmark_blocks (struct pifs_device *pifs,
                    struct list_elem   *i,
                    struct list_elem   *end)
{
  ASSERT (pifs != NULL);
  ASSERT (i != NULL);
  ASSERT (end != NULL);
  
  while (i != end)
    {
      // TODO
    }
}

// may grow by less bytes than requested or even not at all
static void
pifs_grow_file (struct pifs_inode *inode, size_t grow_by)
{
  ASSERT (inode != NULL);
  
  if (grow_by == 0)
    return;
    
  PIFS_DEBUG ("PIFS growing %u by %u.\n", inode->sector, grow_by);
    
  struct block_page *page = block_cache_read (inode->pifs->bc, inode->sector);
  if (!page)
    return; // fail silently
  struct pifs_file *file = (void *) &page->data;
  // test our bookkeeping
  ASSERT (file->magic == PIFS_MAGIC_FILE);
  ASSERT (file->length == inode->length);
    
  // first use all currently allocated space:
  
  if (inode->length % BLOCK_SECTOR_SIZE != 0)
    {
      size_t simple_grow = BLOCK_SECTOR_SIZE - inode->length%BLOCK_SECTOR_SIZE;
      PIFS_DEBUG ("  Simple growth by %u.\n", simple_grow);
      if (grow_by < simple_grow)
        simple_grow = grow_by;
      grow_by -= simple_grow;
      inode->length += simple_grow;
    }
    
  if (grow_by == 0)
    goto end;
    
  // allocate more blocks:
    
  pifs_ptr cur = 0;
  struct block_page *cur_page = NULL;
  struct pifs_file *cur_extend = NULL;
  
  auto inline bool open_cur (bool test);
  bool
  open_cur (bool test)
  {
    ASSERT (cur != 0);
    cur_page = block_cache_read (inode->pifs->bc, cur);
    if (!cur_page)
      return false;
    cur_extend = (void *) &cur_page->data;
    if (test)
      {
        if (cur_extend->magic != PIFS_MAGIC_FILE)
            PANIC ("Block %"PRDSNu" of filesystem is messed up "
                   "(magic = 0x%08X).", cur, cur_extend->magic);
        if (cur_extend->blocks_count > PIFS_COUNT_FILE_BLOCKS)
          PANIC ("Block %"PRDSNu" of filesystem is messed up "
                 "(blocks_count = %u).", cur, cur_extend->blocks_count);
      }
    return true;
  }
  
  // traverse to last extend:
  
  cur = inode->sector;
  for (;;)
    {
      if (!open_cur (true))
        goto end;
      if (cur_extend->extends == 0)
        break;
      cur = cur_extend->extends;
      block_cache_return (inode->pifs->bc, cur_page);
  }
  
  // allocated blocks to use:
  
  size_t blocks_to_alloc = DIV_ROUND_UP (grow_by, BLOCK_SECTOR_SIZE);
  
  struct list allocated_blocks;
  list_init (&allocated_blocks);
  size_t alloc_count UNUSED = pifs_alloc_multiple (inode->pifs,
                                                   blocks_to_alloc,
                                                   &allocated_blocks);
  PIFS_DEBUG ("  Allocated %u block(s).\n", alloc_count);
  
  // insert blocks:
  
  struct list_elem *e;
  for (e = list_begin (&allocated_blocks);
       e != list_end (&allocated_blocks);
       /* done inside of code */)
    {
      // advance:
      
      ASSERT (grow_by != 0);
      
      struct pifs_file_block_ref ee;
      ee = list_entry (e, struct pifs_alloc_multiple_item, elem)->ref;
      e = list_next (e);
      
      ASSERT (ee.start != 0);
      ASSERT (ee.count != 0);
      
      // invariants ensured by "traverse to last extend" code block:
      
      ASSERT (cur_extend->magic == PIFS_MAGIC_FILE);
      ASSERT (cur_extend->extends == 0);
      ASSERT (cur_extend->blocks_count <= PIFS_COUNT_FILE_BLOCKS);
      
      // insert block by block:
      
      // TODO: refactor, the indent level is too high
      while (ee.count > 0)
        {
          if (cur_extend->blocks_count != 0)
            {
              // merge with last ref block if possible:
              
              struct pifs_file_block_ref *last_ref;
              last_ref = &cur_extend->blocks[cur_extend->blocks_count-1];
              
              if (last_ref->count == 0)
                PANIC ("Block %"PRDSNu" of filesystem is messed up "
                       "(blocks[%u].count == 0).",
                       cur, cur_extend->blocks_count-1);
              
              if (last_ref->count < PIFS_COUNT_FILE_REF_COUNT_MAX &&
                  last_ref->start+last_ref->count == ee.start)
                {
                  size_t amount = ee.count;
                  if (amount + last_ref->count > PIFS_COUNT_FILE_REF_COUNT_MAX)
                    amount = PIFS_COUNT_FILE_REF_COUNT_MAX - last_ref->count;
                    
                  last_ref->count += amount;
                  ee.start += amount;
                  ee.count -= amount;
            
                  // increase size:
                  
                  if (grow_by > amount * BLOCK_SECTOR_SIZE)
                    {
                      grow_by -= amount * BLOCK_SECTOR_SIZE;
                      inode->length += amount * BLOCK_SECTOR_SIZE;
                    }
                  else
                    {
                      inode->length += grow_by;
                      grow_by = 0;
                    }
                  continue;
                }
              
              // allocate new extend if cur is full:
              
              if (cur_extend->blocks_count == PIFS_COUNT_FILE_REF_COUNT_MAX)
                {
                  pifs_ptr new_cur = pifs_alloc_block (inode->pifs);
                  if (new_cur == 0)
                    break;
                  cur_extend->extends = new_cur;
                  cur_page->dirty = true;
                  block_cache_return (inode->pifs->bc, cur_page);
                  
                  cur = new_cur;
                  if (!open_cur (false))
                    break;
                  memset (cur_extend, 0, sizeof (*cur_extend));
                  cur_extend->magic = PIFS_MAGIC_FILE;
                }
            } // if (cur_extend->blocks_count != 0)
            
          // add ref block:
      
          cur_extend->blocks[cur_extend->blocks_count] = ee;
          ++cur_extend->blocks_count;
          
          if (grow_by > ee.count * (size_t) BLOCK_SECTOR_SIZE)
            {
              grow_by -= ee.count * BLOCK_SECTOR_SIZE;
              inode->length += ee.count * BLOCK_SECTOR_SIZE;
            }
          else
            {
              inode->length += grow_by;
              grow_by = 0;
            }
          break;
        } // while (ee.count > 0)
    }
    
  // unmark unused block and free allocation list:
  
  pifs_unmark_blocks (inode->pifs, e, list_end (&allocated_blocks));
  pifs_alloc_multiple_free_list (&allocated_blocks);
  
  if (cur_page)
    {
      cur_page->dirty = true;
      block_cache_return (inode->pifs->bc, cur_page);
    }

  // return changed page:
  
end:
  ASSERT (file->length <= inode->length);
  PIFS_DEBUG ("  Growth: %u -> %u.\n", file->length, inode->length);

  if (file->length != inode->length)
    {
      file->length = inode->length;
      page->dirty = true;
    }
  block_cache_return (inode->pifs->bc, page);
}

typedef void (*pifs_iterater_file_cb) (struct pifs_inode *inode,
                                       size_t             start,
                                       size_t             length,
                                       pifs_ptr           nth,
                                       char              *data);

static off_t
pifs_iterater_file (struct pifs_inode     *inode,
                    size_t                 start,
                    size_t                 length,
                    pifs_iterater_file_cb  cb,
                    char                  *data)
{
  off_t result = 0;
  pifs_ptr cur_extend = inode->sector;
  size_t nth_block = 0, nth_block_offs = 0;
  
  seek:while (cur_extend != 0 && length > 0)
    {
      // seek to block:
     
      struct block_page *extend_page = block_cache_read (inode->pifs->bc,
                                                         cur_extend);
      struct pifs_file *extend = (void *) &extend_page->data;
      if (extend->magic != PIFS_MAGIC_FILE)
          PANIC ("Block %"PRDSNu" of filesystem is messed up "
                 "(magic = 0x%08X).", cur_extend, extend->magic);
      if (extend->blocks_count > PIFS_COUNT_FILE_BLOCKS)
        PANIC ("Block %"PRDSNu" of filesystem is messed up "
               "(blocks_count = %u).", cur_extend, extend->blocks_count);
      if (extend->blocks_count == 0)
        {
          block_cache_return (inode->pifs->bc, extend_page);
          break;
        }
      
      while (start >= BLOCK_SECTOR_SIZE)
        {
          ASSERT (nth_block < extend->blocks_count);
          struct pifs_file_block_ref *ref = &extend->blocks[nth_block];
          if (ref->start == 0 || ref->count == 0)
            PANIC ("Block %"PRDSNu" of filesystem is messed up "
                   "(ref[%u].start == %u, count == %u).",
                   cur_extend, nth_block, ref->start, ref->count);
          ASSERT (nth_block_offs  < ref->count);
          
          // advance to next ref if needed:
          
          // TODO: advance by multiple block of ref->count at once
          ++nth_block_offs;
          if (nth_block_offs == ref->count)
            {
              nth_block_offs = 0;
              ++nth_block;
            }
          
          // advance to next extend if reached end of current one:
          
          if (nth_block == extend->blocks_count)
            {
              cur_extend = extend->extends;
              block_cache_return (inode->pifs->bc, extend_page);
              nth_block = 0;
              goto seek;
            }
            
          // advance:
          
          start -= BLOCK_SECTOR_SIZE;
        }
        
      if (cur_extend == 0)
        break;
      struct pifs_file_block_ref ref = extend->blocks[nth_block];
      size_t blocks_count = extend->blocks_count;
      block_cache_return (inode->pifs->bc, extend_page);
      if (nth_block >= blocks_count || ref.start == 0 ||
          nth_block_offs >= ref.count || start >= BLOCK_SECTOR_SIZE)
        break;
        
      // callback and advance:
      
      size_t len;
      if (start == 0 && length >= BLOCK_SECTOR_SIZE)
        len = BLOCK_SECTOR_SIZE;
      else
        {
          len = BLOCK_SECTOR_SIZE - start;
          if (len > length)
            len = length;
        }
        
      cb (inode, start, len, ref.start+nth_block_offs, data);
      
      result += len;
      start += len; // we need to seek
      length -= len;
      data += len;
    }
  
  return result;
}

static void
pifs_write_cb (struct pifs_inode  *inode,
               size_t              start,
               size_t              len,
               pifs_ptr            nth,
               char               *src)
{
  // write a block:
  
  struct block_page *dest;
  if (start == 0 && len == BLOCK_SECTOR_SIZE)
    {
      // write a full block:
      
      PIFS_DEBUG ("  Writing full block to %u.\n", nth);
      dest = block_cache_write (inode->pifs->bc, nth);
    }
  else
    {
      // partially overwrite a block:
      
      PIFS_DEBUG ("  Writing partially to %u [%u,%u[.\n", nth, start,
                  start+len);
      dest = block_cache_read (inode->pifs->bc, nth);
    }
  memcpy (&dest->data[start], src, len);
  dest->dirty = true;
  block_cache_return (inode->pifs->bc, dest);
}

static void
pifs_read_cb (struct pifs_inode  *inode,
               size_t              start,
               size_t              len,
               pifs_ptr            nth,
               char               *dest)
{
  // read a block:
  
  PIFS_DEBUG ("  Reading from %u [%u,%u[.\n", nth, start, start+len);
  
  struct block_page *src = block_cache_read (inode->pifs->bc, nth);
  memcpy (dest, &src->data[start], len);
  block_cache_return (inode->pifs->bc, src);
}

off_t
pifs_write (struct pifs_inode *inode,
            size_t             start,
            size_t             length,
            const void        *src_)
{
  char *src = (void *) src_;
  ASSERT (inode != NULL);
  ASSERT (src != NULL);
  ASSERT (intr_get_level () == INTR_ON);
  
  if (length == 0)
    return 0;
  else if (length > INT_MAX || start > INT_MAX || start+length > INT_MAX ||
           start+length < start || start+length < length || inode->is_directory)
    return -1;
  
  rwlock_acquire_write (&inode->pifs->pifs_rwlock);
  
  // grow file if needed:
    
  if (inode->length < start+length)
    pifs_grow_file (inode, start+length - inode->length);
  PIFS_DEBUG ("PIFS size of %u: %u.\n", inode->sector, inode->length);
  
  // write data:
  
  off_t result = pifs_iterater_file (inode, start, length, pifs_write_cb, src);
  
  rwlock_release_write (&inode->pifs->pifs_rwlock);
  
  return result;
}

off_t
pifs_read (struct pifs_inode *inode,
           size_t             start,
           size_t             length,
           void              *dest_)
{
  char *dest = dest_;
  ASSERT (inode != NULL);
  ASSERT (dest != NULL);
  
  PIFS_DEBUG ("PIFS read %u [%u,%u[\n", inode->sector, start, start+length);
  
  if (length == 0)
    return 0;
  else if (length > INT_MAX || start > INT_MAX || start+length > INT_MAX ||
           start+length < start || start+length < length || inode->is_directory)
    return -1;
  
  rwlock_acquire_read (&inode->pifs->pifs_rwlock);
  
  // read data:
  
  off_t result = pifs_iterater_file (inode, start, length, pifs_read_cb, dest);
  
  rwlock_release_read (&inode->pifs->pifs_rwlock);
  
  return result;
}

static bool
pifs_delete_sub (struct pifs_inode *inode)
{
  ASSERT (inode != 0);
  ASSERT (!inode->deleted);
  
  // Retreive parent folder sector number:
  
  struct block_page *page = block_cache_read (inode->pifs->bc, inode->sector);
  if (page == NULL)
    return false;
  struct pifs_inode_header *header = (void *) &page->data[0];
  if (header->magic != PIFS_MAGIC_FILE && header->magic != PIFS_MAGIC_FOLDER)
      PANIC ("Block %"PRDSNu" of filesystem is messed up "
             "(magic = 0x%08X).", inode->sector, header->magic);
  pifs_ptr parent_folder_sector = header->parent_folder;
  block_cache_return (inode->pifs->bc, page);
  if (parent_folder_sector == 0)
    return false; // inode is the root folder
  
  // Delete from parent_folder:
  
  pifs_ptr s = parent_folder_sector;
  do
    {
      struct block_page *folder_page = block_cache_read (inode->pifs->bc, s);
      struct pifs_folder *folder = (void *) &folder_page->data[0];
      if (folder->magic != PIFS_MAGIC_FOLDER)
        PANIC ("Block %"PRDSNu" of filesystem is messed up (magic = 0x%08X).",
               s, folder->magic);
      if (folder->entries_count > PIFS_COUNT_FOLDER_ENTRIES)
        PANIC ("Block %"PRDSNu" of filesystem is messed up "
               "(entries_count = %u)", s, folder->entries_count);
      
      s = folder->extends;
      
      size_t i;
      for (i = 0; i < folder->entries_count; ++i)
        if (folder->entries[i].block == inode->sector)
          {
            --folder->entries_count;
            memmove (&folder->entries[i], &folder->entries[i+1],
                     sizeof (struct pifs_folder_entry) *
                     (folder->entries_count - i));
            folder_page->dirty = true;
            
            s = 0;
            break;
          }
      
      block_cache_return (inode->pifs->bc, folder_page);
    }
  while (s != 0);
  
  // Update an open parent_folder inode:
  
  struct pifs_inode key;
  memset (&key, 0, sizeof (key));
  key.pifs = inode->pifs;
  key.sector = parent_folder_sector;
  struct hash_elem *e = hash_find (&inode->pifs->open_inodes, &key.elem);
  if (e)
    {
      struct pifs_inode *ee = hash_entry (e, struct pifs_inode, elem);
      ASSERT (ee->length > 0);
      --ee->length;
    }
  
  // Delete from pifs's hash:
  
  struct hash_elem *e2 UNUSED;
  e2 = hash_delete (&inode->pifs->open_inodes, &inode->elem);
  ASSERT (e2 == &inode->elem);
  
  inode->deleted = true;
  return true;
}

bool
pifs_delete_file (struct pifs_inode *inode)
{
  ASSERT (inode != NULL);
  ASSERT (!inode->is_directory);
  ASSERT (intr_get_level () == INTR_ON);
  
  rwlock_acquire_write (&inode->pifs->pifs_rwlock);
  
  bool result = !inode->deleted;
  if (result)
    result = pifs_delete_sub (inode);
    
  rwlock_release_write (&inode->pifs->pifs_rwlock);
  return result;
}

bool
pifs_delete_folder (struct pifs_inode *inode)
{
  ASSERT (inode != NULL);
  ASSERT (inode->is_directory);
  ASSERT (intr_get_level () == INTR_ON);
  
  rwlock_acquire_write (&inode->pifs->pifs_rwlock);
  
  bool result = !inode->deleted && (inode->length == 0);
  if (result)
    result = pifs_delete_sub (inode);
  
  rwlock_release_write (&inode->pifs->pifs_rwlock);
  return result;
}
