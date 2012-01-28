#include "filesys.h"
#include "cache.h"
#include "pifs.h"
#include "file.h"

#include <debug.h>
#include <stdio.h>
#include <string.h>

#include "threads/malloc.h"

#define FS_CACHE_SIZE 64
#define FS_CACHE_IN_USERSPACE false

static bool fs_initialized;

/* Partition that contains the file system. */
static struct block       *fs_device;
static struct block_cache  fs_cache;
struct pifs_device         fs_pifs;

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");
  if (!block_cache_init (&fs_cache, fs_device, FS_CACHE_SIZE,
                         FS_CACHE_IN_USERSPACE))
    PANIC ("Filesys cache could not be intialized.");
  if (!pifs_init (&fs_pifs, &fs_cache))
    PANIC ("PIFS could not be intialized.");

  if (format) 
    {
      if (!pifs_format (&fs_pifs))
        PANIC ("Your device is either too big or too small.");
    }
  else if (!pifs_sanity_check (&fs_pifs))
    PANIC ("PIFS's basic sanity check failed.");
    
  printf ("Initialized filesystem.\n");
  fs_initialized = true;
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  if (!fs_initialized)
    return;
    
  ASSERT (intr_get_level () == INTR_ON);
    
  pifs_destroy (&fs_pifs);
  block_cache_destroy (&fs_cache);
  printf ("Filesystem has shut down.\n");
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) 
{
  struct pifs_inode *inode;
  inode = pifs_open (&fs_pifs, name, POO_FILE_MUST_CREATE);
  if (!inode)
    return false;
    
  if (initial_size > 0)
    {
      pifs_close (inode);
      return true;
    }
    
  void *space = malloc (512);
  if (!space)
    {
      pifs_delete_file (inode);
      pifs_close (inode);
      return false;
    }
  memset (space, 0, 512);
    
  size_t pos = 0;
  while (initial_size > 0)
    {
      off_t len = initial_size > 512 ? 512  : initial_size;
      off_t wrote = pifs_write (inode, pos, len, space);
      if (wrote < 0)
        {
          free (space);
          pifs_delete_file (inode);
          pifs_close (inode);
          return false;
        }
      pos += wrote;
      initial_size -= wrote;
    }
    
  free (space);
  pifs_delete_file (inode);
  pifs_close (inode);
  return true;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  struct pifs_inode *inode = pifs_open (&fs_pifs, name, POO_NO_CREATE);
  if (!inode)
    return NULL;
  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  return pifs_delete_file_path (&fs_pifs, name);
}
