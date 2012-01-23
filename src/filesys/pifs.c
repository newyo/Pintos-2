#include "pifs.h"
#include "bitset.h"
#include <stdint.h>
#include <string.h>
#include <debug.h>
#include "threads/malloc.h"

typedef char pifs_magic[4];

const pifs_magic PIFS_HEADER_MAGIC = "PIFS";
const pifs_magic PIFS_FOLDER_MAGIC = "FLDR";
const pifs_magic PIFS_FILE_MAGIC   = "FILE";

#define PIFS_NAME_LENGTH 16

typedef uint32_t pifs_ptr;

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

void
pifs_format (struct pifs_device *pifs)
{
  ASSERT (pifs != NULL);
  ASSERT (pifs->device != NULL);
  
  block_sector_t blocks = block_size (block_cache_get_device (pifs->device));
  
  struct pifs_header *header = malloc (sizeof (*header));
  ASSERT (header != NULL);
  memset (header, 0, sizeof (*header));
  memcpy (header->magic, PIFS_HEADER_MAGIC, sizeof (header->magic));
  header->block_count = blocks;
  header->root_folder = 1;
  bitset_mark (header->used_map, 0);
  bitset_mark (header->used_map, 1);
  block_cache_write (pifs->device, 0, (block_cache_page *) header);
  free (header);
  
  struct pifs_folder *root = malloc (sizeof (*root));
  ASSERT (root != NULL);
  memset (root, 0, sizeof (*root));
  memcpy (root->magic, PIFS_FOLDER_MAGIC, sizeof (root->magic));
  block_cache_write (pifs->device, 0, (block_cache_page *) root);
  free (root);
}
