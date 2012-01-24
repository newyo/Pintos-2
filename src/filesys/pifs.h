#ifndef __PIFS_H
#define __PIFS_H

// pifs = Pintos Filesystem :)

#include <stdbool.h>
#include <packed.h>
#include <hash.h>
#include "threads/synch.h"
#include "cache.h"

#define PIFS_NAME_LENGTH 16

struct pifs_attrs
{
  bool readable   : 1;
  bool writable   : 1;
  bool executable : 1;
} PACKED;

typedef char _CASSERT_PIFS_ATTRS_SIZE[0 - !(sizeof (struct pifs_attrs) == 1)];

struct pifs_device
{
  struct block_cache *bc;
  int                 next_inum;
  struct hash         open_inodes; // [sector -> struct pifs_inode]
  struct rwlock       pifs_rwlock;
};

struct pifs_inode
{
  int                 inum;
  struct pifs_device *pifs;
  block_sector_t      sector;
  bool                is_directory;
  size_t              open_count;
  size_t              length; // file = bytes, directory = files
  bool                deleted; // will be deleted when closed
  struct hash_elem    elem; // struct pifs_device::open_inodes
};

#define POO_MASK_NO     0b0001
#define POO_MASK_MUST   0b0010
#define POO_MASK_FILE   0b0100
#define POO_MASK_FOLDER 0b1000
#define POO_MASK        0b1111

enum pifs_open_opts
{
  POO_NO_CREATE          = POO_MASK_NO,
  
  POO_FILE_MAY_CREATE    = POO_MASK_FILE,
  POO_FILE_NO_CREATE     = POO_MASK_FILE   | POO_MASK_NO,
  POO_FILE_MUST_CREATE   = POO_MASK_FILE   | POO_MASK_MUST,
  
  POO_FOLDER_MAY_CREATE  = POO_MASK_FOLDER,
  POO_FOLDER_NO_CREATE   = POO_MASK_FOLDER | POO_MASK_NO,
  POO_FOLDER_MSUT_CREATE = POO_MASK_FOLDER | POO_MASK_MUST,
};

bool pifs_init (struct pifs_device *pifs, struct block_cache *bc);
void pifs_destroy (struct pifs_device *pifs);
void pifs_format (struct pifs_device *pifs);

struct pifs_inode *pifs_open (struct pifs_device  *pifs,
                              const char          *path,
                              enum pifs_open_opts  opts);
void pifs_close (struct pifs_inode *inode);

// Returns nth filename in directory.
// May not be null terminated. Max. PIFS_NAME_LENGTH characters.
const char *pifs_readdir (struct pifs_inode *inode, size_t index);

size_t pifs_read (struct pifs_inode *inode,
                  size_t             start,
                  size_t             length,
                  void              *dest);
size_t pifs_write (struct pifs_inode *inode,
                   size_t             start,
                   size_t             length,
                   const void        *src);

void pifs_delete (struct pifs_inode *inode);

// Convenience methods:

static inline bool
pifs_delete_path (struct pifs_device *pifs, const char *path)
{
  struct pifs_inode *inode = pifs_open (pifs, path, PIFS_NO_CREATE);
  if (!inode)
    return false;
  pifs_delete (inode);
  pifs_close (inode);
  return true;
}

static inline bool
pifs_length_path (struct pifs_device *pifs, const char *path, size_t *result)
{
  ASSERT (result != NULL);
  struct pifs_inode *inode = pifs_open (pifs, path, PIFS_NO_CREATE);
  if (!inode)
    return false;
  *result = inode->length;
  pifs_close (inode);
  return true;
}

static inline bool
pifs_exists_path (struct pifs_device *pifs, const char *path)
{
  struct pifs_inode *inode = pifs_open (pifs, path, PIFS_NO_CREATE);
  if (!inode)
    return false;
  pifs_close (inode);
  return true;
}

static inline bool
pifs_isdir_path (struct pifs_device *pifs, const char *path, bool *result)
{
  ASSERT (result != NULL);
  struct pifs_inode *inode = pifs_open (pifs, path, PIFS_NO_CREATE);
  if (!inode)
    return false;
  *result = inode->is_directory;
  pifs_close (inode);
  return true;
}

#endif
