#include "pifs.h"
#include "bitset.h"
#include <stdint.h>
#include <string.h>
#include <debug.h>
#include "threads/malloc.h"
#include "threads/interrupt.h"

#define PIFS_DEFAULT_HEADER_BLOCK 0
#define PIFS_DEFAULT_ROOT_BLOCK 1

typedef char pifs_magic[4];

static const pifs_magic PIFS_HEADER_MAGIC = "PIFS";
static const pifs_magic PIFS_FOLDER_MAGIC = "FLDR";
static const pifs_magic PIFS_FILE_MAGIC   = "FILE";

typedef uint32_t pifs_ptr;

typedef char _CASSERT_PIFS_PTR_SIZE[0 - !(sizeof (pifs_ptr) ==
                                          sizeof (block_sector_t))];

struct pifs_header
{
  pifs_magic magic;
  uint32_t   block_count;
  pifs_ptr   root_folder;
  char       used_map[500];
} PACKED;

struct pifs_folder_entry
{
  char     name[PIFS_NAME_LENGTH];
  pifs_ptr block;
} PACKED;

struct pifs_folder
{
  pifs_magic magic;
  pifs_ptr   extends; // pointer to overflow bucket (struct pifs_folder)
  uint32_t   entries_count;
  struct pifs_folder_entry entries[25]; 
} PACKED;

struct pifs_file_block_ref
{
  pifs_ptr start;
  uint8_t  count;
} PACKED;

struct pifs_file
{
  pifs_magic magic;
  pifs_ptr   extends; // pointer to overflow bucket (struct pifs_file::blocks)
  uint32_t   length;
  uint32_t   ref_blocks_count;
  struct pifs_attrs attrs;
  struct pifs_file_block_ref blocks[99];
} PACKED;

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
  typedef char _CASSERT[0 - !(sizeof (unsigned) == sizeof (ee->inum))];
  ASSERT (ee->pifs == pifs);
  return (unsigned) ee->inum;
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
  
  return true;
}

void
pifs_destroy (struct pifs_device *pifs)
{
  ASSERT (pifs != NULL);
  
  // TODO
}

void
pifs_format (struct pifs_device *pifs)
{
  ASSERT (pifs != NULL);
  ASSERT (pifs->bc != NULL);
  
  block_sector_t blocks = block_size (pifs->bc->device);
  
  struct block_page *page;
  
  // write file system header:
  
  page = block_cache_write (pifs->bc, PIFS_DEFAULT_HEADER_BLOCK);
  ASSERT (page !=  NULL);
  struct pifs_header *header = (void *) &page->data;
  
  memset (header, 0, sizeof (*header));
  memcpy (header->magic, PIFS_HEADER_MAGIC, sizeof (header->magic));
  header->block_count = blocks;
  header->root_folder = PIFS_DEFAULT_ROOT_BLOCK;
  bitset_mark (header->used_map, PIFS_DEFAULT_HEADER_BLOCK);
  bitset_mark (header->used_map, PIFS_DEFAULT_ROOT_BLOCK);
  block_cache_return (pifs->bc, page);
  
  // write root directory:
  
  page = block_cache_write (pifs->bc, PIFS_DEFAULT_ROOT_BLOCK);
  ASSERT (page !=  NULL);
  struct pifs_folder *root = (void *) &page->data;
  
  memset (root, 0, sizeof (*root));
  memcpy (root->magic, PIFS_FOLDER_MAGIC, sizeof (root->magic));
  block_cache_return (pifs->bc, page);
}

static block_sector_t
pifs_get_root_block (struct pifs_device *pifs)
{
  ASSERT (pifs != NULL);
  
  struct block_page *page = block_cache_read (pifs->bc,
                                              PIFS_DEFAULT_HEADER_BLOCK);
  ASSERT (page != NULL);
  struct pifs_header *header = (struct pifs_header *) &page->data;
  block_sector_t result = header->root_folder;
  block_cache_return (pifs->bc, page);
  
  return result;
}

static inline block_sector_t
pifs_open_traverse (struct pifs_device  *pifs,
                    block_sector_t       cur, 
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
  if (memcpy (&page->data, PIFS_FOLDER_MAGIC, sizeof (pifs_magic)) != 0)
    {
      if (memcmp (&page->data, PIFS_FILE_MAGIC, sizeof (pifs_magic)) == 0)
        {
          // we are hit a file, but the path indicated it was a folder
          block_cache_return (pifs->bc, page);
          return 0;
        }
      PANIC ("Block %"PRDSNu" of filesystem is messed up.", cur);
    }
    
  for (;;)
    {
      struct pifs_folder *folder = (struct pifs_folder *) &page->data;
      
      unsigned i;
      for (i = 0; i < folder->entries_count; ++i)
        {
          if (memcmp (folder->entries[i].name, *path_, path_elem_len) != 0 ||
              folder->entries[i].name[path_elem_len] != 0)
            continue;
            
          // found!
            
          block_sector_t next_block = folder->entries[i].block;
          if (next_block == 0)
            PANIC ("Block %"PRDSNu" of filesystem is messed up.", cur);
          block_cache_return (pifs->bc, page);
          
          if (next[0] == 0 || next[1] == 0)
            return next_block;
            
          // We are not finished yet
          *path_ = next;
          return pifs_open_traverse (pifs, next_block, path_); // TCO
        }
          
      block_sector_t extends = folder->extends;
      block_cache_return (pifs->bc, page);
      if (extends == 0)
        return 0;
        
      page = block_cache_read (pifs->bc, extends);
      if (memcpy (&page->data, PIFS_FOLDER_MAGIC, sizeof (pifs_magic)) != 0)
        PANIC ("Block %"PRDSNu" of filesystem is messed up.", cur);
    }
}

struct pifs_inode *
pifs_open (struct pifs_device *pifs,
           const char         *path,
           enum pifs_create    create)
{
  ASSERT (pifs != NULL);
  ASSERT (path != NULL);
  ASSERT (path[0] == '/');
  ASSERT (_IN (create, PIFS_NO_CREATE, PIFS_DO_CREATE, PIFS_MAY_CREATE));
  
  if (create == PIFS_NO_CREATE)
    rwlock_acquire_read (&pifs->pifs_rwlock);
  else
    rwlock_acquire_write (&pifs->pifs_rwlock);
    
  block_sector_t root_block = pifs_get_root_block (pifs);
  block_sector_t found_sector = pifs_open_traverse (pifs, root_block, &path);
  struct pifs_inode *result;
  if (found_sector != 0)
    {
      bool must_be_folder = *path == 0;
      
      // TODO: retreive inode or open new one
      result = NULL;
      (void) must_be_folder;
    }
  else
    result = NULL;
    
  if (create == PIFS_NO_CREATE)
    rwlock_release_read (&pifs->pifs_rwlock);
  else
    rwlock_release_write (&pifs->pifs_rwlock);
  
  return result;
}

void
pifs_close (struct pifs_inode *inode)
{
  if (inode == NULL)
    return;
    
  rwlock_acquire_write (&inode->pifs->pifs_rwlock);
    
  // TODO
  
  rwlock_release_write (&inode->pifs->pifs_rwlock);
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

size_t
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
  
  rwlock_acquire_read (&inode->pifs->pifs_rwlock);
    
  // TODO
  (void) start;
  
  rwlock_release_read (&inode->pifs->pifs_rwlock);
  return 0;
}

size_t
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
  
  rwlock_acquire_write (&inode->pifs->pifs_rwlock);
    
  // TODO
  (void) start;
  
  rwlock_release_write (&inode->pifs->pifs_rwlock);
  return 0;
}

void
pifs_delete (struct pifs_inode *inode)
{
  ASSERT (inode != NULL);
  
  rwlock_acquire_write (&inode->pifs->pifs_rwlock);
  if (!inode->deleted)
    {
      inode->deleted = true;
      
      struct hash_elem *e UNUSED;
      e = hash_delete (&inode->pifs->open_inodes, &inode->elem);
      ASSERT (e == &inode->elem);
    }
  rwlock_release_write (&inode->pifs->pifs_rwlock);
}
