#ifndef __PIFS_H
#define __PIFS_H

// pifs = Pintos Filesystem :)

#include <stdbool.h>
#include <packed.h>
#include <hash.h>
#include <list.h>
#include "threads/synch.h"
#include "off_t.h"
#include "cache.h"

struct pifs_attrs // Even though we won't implement attributes ...
{
  bool readable   : 1;
  bool writable   : 1;
  bool executable : 1;
} PACKED;

typedef char _CASSERT_PIFS_ATTRS_SIZE[0 - !(sizeof (struct pifs_attrs) == 1)];

struct pifs_device
{
/* public (readonly): */
  struct block_cache *bc;
/* private: */
  struct hash         open_inodes; // [sector -> struct pifs_inode]
  struct rwlock       pifs_rwlock;
  struct block_page  *header_block;
  
  struct semaphore    deletor_sema;
  struct list         deletor_list;
};

struct pifs_inode
{
/* public (readonly): */
  bool                is_directory;
  size_t              length; // file size in bytes, or items in folder
  size_t              deny_write_cnt;
/* private: */
  struct pifs_device *pifs;
  block_sector_t      sector;
  size_t              open_count;
  bool                deleted; // will be deleted when closed
  struct hash_elem    elem; // struct pifs_device::open_inodes
};

#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 4)
# define POO_MASK_NO     0b0001
# define POO_MASK_MUST   0b0010
# define POO_MASK_FILE   0b0100
# define POO_MASK_FOLDER 0b1000
# define POO_MASK        0b1111
#else
# define POO_MASK_NO     1
# define POO_MASK_MUST   2
# define POO_MASK_FILE   4
# define POO_MASK_FOLDER 8
# define POO_MASK        15
#endif
enum pifs_open_opts
{
  POO_NO_CREATE          = POO_MASK_NO,
  
  POO_FILE_MAY_CREATE    = POO_MASK_FILE,
  POO_FILE_NO_CREATE     = POO_MASK_FILE   | POO_MASK_NO,
  POO_FILE_MUST_CREATE   = POO_MASK_FILE   | POO_MASK_MUST,
  
  POO_FOLDER_MAY_CREATE  = POO_MASK_FOLDER,
  POO_FOLDER_NO_CREATE   = POO_MASK_FOLDER | POO_MASK_NO,
  POO_FOLDER_MUST_CREATE = POO_MASK_FOLDER | POO_MASK_MUST,
};

bool pifs_init (struct pifs_device *pifs, struct block_cache *bc);
void pifs_destroy (struct pifs_device *pifs);
bool pifs_format (struct pifs_device *pifs);
bool pifs_sanity_check (struct pifs_device *pifs);

// folder may be NULL
struct pifs_inode *pifs_open (struct pifs_device  *pifs,
                              const char          *path,
                              enum pifs_open_opts  opts);
struct pifs_inode *pifs_open2 (struct pifs_device  *pifs,
                               const char          *path,
                               enum pifs_open_opts  opts,
                               struct pifs_inode   *folder);
void pifs_close (struct pifs_inode *inode);

/**
 * Returns nth filename in directory.   Cost: ceil((index+1) / 24).
 * 
 * \param inode [in, null, folder]
 * \param len [out] When *len >= null, result is not null terminated.
 */
const char *pifs_readdir (struct pifs_inode *inode, size_t index, off_t *len);

off_t pifs_read (struct pifs_inode *inode,
                 size_t             start,
                 size_t             length,
                 void              *dest);
off_t pifs_write (struct pifs_inode *inode,
                  size_t             start,
                  size_t             length,
                  const void        *src);

void pifs_delete_file (struct pifs_inode *inode);
bool pifs_delete_folder (struct pifs_inode *inode);

#endif
