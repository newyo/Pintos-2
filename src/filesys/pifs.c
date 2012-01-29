#include "pifs.h"
#include "bitset.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <debug.h>
#include <round.h>
#include <limits.h>
#include <list.h>

#include "threads/malloc.h"
#include "threads/interrupt.h"

#define PIFS_DEBUG(...) printf (__VA_ARGS__)

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
#define PIFS_COUNT_FILE_BLOCKS 98 /* ~50 kb per block (w/ max. fragmentation) */
#define PIFS_COUNT_LONG_NAME_CHARS 491

struct pifs_inode_header
{
  pifs_magic        magic;
  pifs_ptr          extends; // pointer to overflow bucket
  pifs_ptr          parent_folder;
  struct pifs_attrs attrs;
  pifs_ptr          long_name;
} PACKED;

struct pifs_long_name // not implemented by us, but for completeness
{
  pifs_magic        magic;
  pifs_ptr          extends; // even more characters!!
  pifs_ptr          belongs_to;
  struct pifs_attrs unused1; // unused for this inode type
  pifs_ptr          unused2; // unused for this inode type
  
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
  
  uint16_t          block_count;
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
  struct pifs_attrs        attrs; // no implemented
  pifs_ptr                 long_name; // not implemented
  
  char                     padding[14];
  
  uint8_t                  entries_count;
  // TODO: One optimization would be sorting the entries in an extend.
  //       I don't think sorting over all extends would be necessary.
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
  
  memset (pifs, 0, sizeof (*pifs));
  pifs->bc = bc;
  hash_init (&pifs->open_inodes, &pifs_open_inodes_hash, &pifs_open_inodes_less,
             pifs);
  rwlock_init (&pifs->pifs_rwlock);
  
  pifs->header_block = block_cache_read (pifs->bc, 0);
  ASSERT (pifs->header_block != NULL);
  
  return true;
}

void
pifs_destroy (struct pifs_device *pifs)
{
  ASSERT (pifs != NULL);
  
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
      h->block_count = PIFS_COUNT_USED_MAP_ENTRIES;
    else
      h->block_count = blocks;
  }
  
  printf ("PIFS is formatting free-maps.\n");
  
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
  
  printf ("PIFS is formatting root directory (%u).\n", nth_block);
   
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
  const struct pifs_header *header = (void *) &pifs->header_block->data;
  return header->magic == PIFS_MAGIC_HEADER;
}

static block_sector_t
pifs_open_traverse (struct pifs_device  *pifs,
                    pifs_ptr             cur, 
                    const char         **path_)
{
  ASSERT (pifs != NULL);
  ASSERT (path_ != NULL && *path_ != NULL);
  ASSERT (**path_ == '/');
  
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
          PIFS_DEBUG ("  Contains '%.*s'.\n", PIFS_NAME_LENGTH, name);
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
        return cur;
        
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
  
  return result;
}

static pifs_ptr
pifs_alloc_block (struct pifs_device *pifs)
{
  pifs_ptr cur = 0, result = 0;
  do
    {
      struct block_page *page = block_cache_read (pifs->bc, cur);
      struct pifs_header *header = (void *) &page->data;
      if (header->magic != PIFS_MAGIC_HEADER)
          PANIC ("Block %"PRDSNu" of filesystem is messed up "
                 "(magic = 0x%08X).", cur, header->magic);
                 
      int len = (header->block_count+7) / 8;
      if (len > PIFS_COUNT_USED_MAP_ENTRIES)
          PANIC ("Block %"PRDSNu" of filesystem is messed up "
                 "(apparent len = %u).", cur, len);
                 
      off_t bit_result = bitset_find_and_set_1 (&header->used_map[0], len);
      if (bit_result > 0)
        {
          result = bit_result;
          page->dirty = true;
        }
      else
        cur = header->extends;
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

// caller has to init. *list
static size_t
pifs_alloc_multiple (struct pifs_device *pifs,
                     size_t              amount,
                     struct list        *list)
{
  ASSERT (list != NULL);
  if (amount == 0)
    return 0;
    
  size_t offset = 0;
  size_t result = 0;
  
  auto void cb (block_sector_t sector);
  void
  cb (block_sector_t sector)
  {
    ASSERT (sector != 0);
    ++result;
    sector += offset;
    
    struct pifs_alloc_multiple_item *ee;
    
    // add to prev. last block if possible:
    
    if (!list_empty (list))
      {
        struct list_elem *e = list_back (list);
        ee = list_entry (e, struct pifs_alloc_multiple_item, elem);
        if (ee->ref.start + ee->ref.count == sector)
          {
            ++ee->ref.count;
            return;
          }
      }
      
    // allocate new reference block:
      
    ee = malloc (sizeof (*ee));
    if (!ee)
      {
        --result;
        return;
      }
    memset (ee, 0, sizeof (*ee));
    ee->ref.start = sector;
    ee->ref.count = 1;
    list_push_back (list, &ee->elem);
  }
  
  pifs_ptr cur = 0;
  do
    {
      struct block_page *page = block_cache_read (pifs->bc, cur);
      struct pifs_header *header = (void *) &page->data;
      if (header->magic != PIFS_MAGIC_HEADER)
          PANIC ("Block %"PRDSNu" of filesystem is messed up "
                 "(magic = 0x%08X).", cur, header->magic);
                 
      int len = (header->block_count+7) / 8;
      if (len > PIFS_COUNT_USED_MAP_ENTRIES)
          PANIC ("Block %"PRDSNu" of filesystem is messed up "
                 "(apparent len = %u).", cur, len);
                 
      size_t left = bitset_find_and_set (&header->used_map[0], len, amount, cb);
      
      ASSERT (left <= amount);
      cur = header->extends;
      offset += header->block_count;
      
      if (left != amount)
        {
          page->dirty = true;
          amount = left;
        }
      block_cache_return (pifs->bc, page);
    }
  while (amount > 0 && cur != 0);
  
  return result;
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
  
  if (folder->entries_count >= PIFS_COUNT_FOLDER_ENTRIES)
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

struct pifs_inode *
pifs_open (struct pifs_device  *pifs,
           const char          *path,
           enum pifs_open_opts  opts)
{
  ASSERT (pifs != NULL);
  ASSERT (path != NULL);
  ASSERT (path[0] == '/');
  
  ASSERT ((opts & ~POO_MASK) == 0);
  ASSERT (!((opts & POO_MASK_FILE) && (opts & POO_MASK_FOLDER)));
  ASSERT (!((opts & POO_MASK_NO)   && (opts & POO_MASK_MUST)));
  ASSERT ((opts & POO_NO_CREATE) || ((opts & POO_MASK_FILE) ||
                                     (opts & POO_MASK_FOLDER)));
  
  rwlock_acquire_write (&pifs->pifs_rwlock);
  
  pifs_ptr found_sector;
  found_sector = pifs_open_traverse (pifs, pifs_header (pifs)->root_folder,
                                     &path);
                                     
  PIFS_DEBUG ("PIFS end path = '%s'\n", path);
  
  struct pifs_inode *result = NULL;
  if (found_sector == 0)
    goto end; // path was invalid
  else if (path[0] == 0 || path[1] == 0)
    { // we found a file or folder
    
      // test if user's requirements were met:
      
      if (opts & POO_MASK_MUST)
        goto end;
        
      bool must_be_file   = (opts & POO_MASK_FILE);
      bool must_be_folder = (opts & POO_MASK_FOLDER) || (path[1] == 0);
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
          if (!((must_be_file   &&  result->is_directory) ||
                (must_be_folder && !result->is_directory)))
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
      
      ASSERT (path[0] != '/');
      
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
  
  if (result != 0)
    {
      struct hash_elem *e UNUSED;
      e = hash_insert (&pifs->open_inodes, &result->elem);
      ASSERT (e == NULL);
    }

end:
  if (result)
    ++result->open_count;
  rwlock_release_write (&pifs->pifs_rwlock);
  return result;
}

void
pifs_close (struct pifs_inode *inode)
{
  if (inode == NULL)
    return;
    
  rwlock_acquire_write (&inode->pifs->pifs_rwlock);
  if (!inode->deleted)
    {
      struct hash_elem *e UNUSED;
      e = hash_delete (&inode->pifs->open_inodes, &inode->elem);
      ASSERT (e == &inode->elem);
    }
  else
    {
      // TODO: free all blocks
    }
  rwlock_release_write (&inode->pifs->pifs_rwlock);
  free (inode);
}

const char *
pifs_readdir (struct pifs_inode *inode, size_t index)
{
  ASSERT (inode !=  NULL);
  
  rwlock_acquire_read (&inode->pifs->pifs_rwlock);
  
  // TODO
  (void) index;
  
  rwlock_release_read (&inode->pifs->pifs_rwlock);
  return NULL;
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
  
  if (length == 0)
    return 0;
  else if (length > INT_MAX || start > INT_MAX || start+length > INT_MAX ||
           start+length < start || start+length < length || inode->is_directory)
    return -1;
  
  off_t result = -1;
  rwlock_acquire_read (&inode->pifs->pifs_rwlock);
    
  if (start > inode->length)
    {
      result = 0;
      goto end;
    }
  else if (start+length > inode->length)
    length = inode->length-start;
    
  if (length > 0)
    {
      // TODO: read
    }
  
end:
  rwlock_release_read (&inode->pifs->pifs_rwlock);
  return result;
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
      if (grow_by < simple_grow)
        simple_grow = grow_by;
      grow_by -= simple_grow;
      inode->length += simple_grow;
    }
    
  // allocate more blocks:
    
  if (grow_by > 0)
    {
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
      pifs_alloc_multiple (inode->pifs, blocks_to_alloc, &allocated_blocks);
      
      // insert blocks:
      
      struct list_elem *e;
      for (e = list_front (&allocated_blocks);
           e != list_end (&allocated_blocks);
           /* done inside of code */)
        {
          // advance:
          
          struct pifs_file_block_ref ee;
          ee = list_entry (e, struct pifs_alloc_multiple_item, elem)->ref;
          e = list_next (e);
          
          // invariants ensured by "traverse to last extend" code block:
          
          ASSERT (cur_extend->magic == PIFS_MAGIC_FILE);
          ASSERT (cur_extend->extends == 0);
          ASSERT (cur_extend->blocks_count <= PIFS_COUNT_FILE_BLOCKS);
          
          // allocate new extend if cur is full:
          
          if (cur_extend->blocks_count == PIFS_COUNT_FILE_BLOCKS)
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
            
          // add block to extend:
          
          struct pifs_file_block_ref *last_block;
          last_block = &cur_extend->blocks[cur_extend->blocks_count];
          if (last_block->start+last_block->count != ee.start)
            {
              // add new block
              ++cur_extend->blocks_count;
              last_block[1] = ee;
            }
          else
            {
              // merge with last ref block if possible
              last_block->count += ee.count;
            }
            
          if (grow_by > ee.count * BLOCK_SECTOR_SIZE)
            {
              file->length += ee.count * BLOCK_SECTOR_SIZE;
              grow_by -= ee.count * BLOCK_SECTOR_SIZE;
            }
          else
            {
              file->length += grow_by;
              break;
            }
        }
        
      // unmark unused block and free allocation list:
      
      pifs_unmark_blocks (inode->pifs, e, list_end (&allocated_blocks));
      pifs_alloc_multiple_free_list (&allocated_blocks);
      
      if (cur_page)
        {
          cur_page->dirty = true;
          block_cache_return (inode->pifs->bc, page);
        }
    }

  // return changed page:
  
end:
  if (file->length != inode->length)
    {
      file->length = inode->length;
      page->dirty = true;
    }
  block_cache_return (inode->pifs->bc, page);
}

off_t
pifs_write (struct pifs_inode *inode,
            size_t             start,
            size_t             length,
            const void        *src_)
{
  const char *src = src_;
  ASSERT (inode != NULL);
  ASSERT (src != NULL);
  
  if (length == 0)
    return 0;
  else if (length > INT_MAX || start > INT_MAX || start+length > INT_MAX ||
           start+length < start || start+length < length || inode->is_directory)
    return -1;
  
  off_t result = -1;
  rwlock_acquire_write (&inode->pifs->pifs_rwlock);
    
  if (inode->length < start+length)
    pifs_grow_file (inode, start+length - inode->length);
    
  // TODO: write
  
  rwlock_release_write (&inode->pifs->pifs_rwlock);
  return result;
}

static void
pifs_delete_sub (struct pifs_inode *inode)
{
  ASSERT (inode != 0);
  ASSERT (!inode->deleted);
  
  inode->deleted = true;
  
  struct hash_elem *e UNUSED;
  e = hash_delete (&inode->pifs->open_inodes, &inode->elem);
  ASSERT (e == &inode->elem);
  
  // TODO: delete from parent_folder
  // TODO: update open parent_folder inode
}

void
pifs_delete_file (struct pifs_inode *inode)
{
  ASSERT (inode != NULL);
  ASSERT (!inode->is_directory);
  
  rwlock_acquire_write (&inode->pifs->pifs_rwlock);
  if (!inode->deleted)
    pifs_delete_sub (inode);
  rwlock_release_write (&inode->pifs->pifs_rwlock);
}

bool
pifs_delete_folder (struct pifs_inode *inode)
{
  ASSERT (inode != NULL);
  ASSERT (inode->is_directory);
  
  rwlock_acquire_write (&inode->pifs->pifs_rwlock);
  
  bool result = (inode->length == 0) &&
                (inode->sector != pifs_header (inode->pifs)->root_folder);
  if (result && !inode->deleted)
    pifs_delete_sub (inode);
  
  rwlock_release_write (&inode->pifs->pifs_rwlock);
  return result;
}