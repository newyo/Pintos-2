#include "pifs.h"
#include "bitset.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <debug.h>
#include <packed.h>
#include "devices/block.h"

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
  uint32_t   blocks_free;
  char       bitmap[500];
} PACKED;

struct pifs_folder_entry
{
  char     name[PIFS_NAME_LENGTH];
  pifs_ptr block;
} PACKED;

struct pifs_folder
{
  pifs_magic magic;
  pifs_ptr   more;
  uint32_t   count;
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
  uint32_t   length;
  uint32_t   ref_blocks_count;
  struct pifs_file_block_ref blocks[100];
} PACKED;

typedef char _CASSERT_PIFS_HEADER_SIZE[0 - !(sizeof (struct pifs_header) ==
                                             BLOCK_SECTOR_SIZE)];
typedef char _CASSERT_PIFS_FOLDER_SIZE[0 - !(sizeof (struct pifs_folder) ==
                                             BLOCK_SECTOR_SIZE)];
typedef char _CASSERT_PIFS_FILE_SIZE[0 - !(sizeof (struct pifs_file) ==
                                           BLOCK_SECTOR_SIZE)];
