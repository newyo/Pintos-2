#include "file.h"
#include "pifs.h"
#include <debug.h>
#include "threads/malloc.h"

#define FILE_MAGIC (('F' << 24) + ('I' << 16) + ('L' << 8) + 'E')

struct file 
  {
    uint32_t           magic;
    
    struct pifs_inode *inode;      /* File's inode. */
    off_t              pos;        /* Current position. */
    bool               deny_write; /* Has file_deny_write() been called? */
  };

/* Opens a file for the given INODE, of which it takes ownership,
   and returns the new file.  Returns a null pointer if an
   allocation fails or if INODE is null. */
struct file *
file_open (struct pifs_inode *inode) 
{
  if (inode == NULL)
    return NULL;
    
  struct file *file = calloc (1, sizeof *file);
  if (file != NULL)
    {
      file->magic = FILE_MAGIC;
      file->inode = inode;
      file->pos = 0;
      file->deny_write = false;
      return file;
    }
  else
    {
      pifs_close (inode);
      free (file);
      return NULL; 
    }
}

/* Opens and returns a new file for the same inode as FILE.
   Returns a null pointer if unsuccessful. */
struct file *
file_reopen (struct file *file) 
{
  ASSERT (file != NULL);
  ASSERT (file->magic == FILE_MAGIC);
  
  __sync_add_and_fetch (&file->inode->open_count, 1);
  return file_open (file->inode);
}

/* Closes FILE. */
void
file_close (struct file *file) 
{
  if (file != NULL)
    {
      ASSERT (file->magic == FILE_MAGIC);
      
      file_allow_write (file);
      pifs_close (file->inode);
      free (file); 
    }
}

/* Returns the inode encapsulated by FILE. */
struct pifs_inode *
file_get_inode (struct file *file) 
{
  ASSERT (file != NULL);
  //ASSERT (file->magic == FILE_MAGIC);
  
  return file->inode;
}

/* Reads SIZE bytes from FILE into BUFFER,
   starting at the file's current position.
   Returns the number of bytes actually read,
   which may be less than SIZE if end of file is reached.
   Advances FILE's position by the number of bytes read. */
off_t
file_read (struct file *file, void *buffer, off_t size) 
{
  ASSERT (file != NULL);
  ASSERT (file->magic == FILE_MAGIC);
  
  off_t bytes_read = pifs_read (file->inode, file->pos, size, buffer);
  if (bytes_read > 0)
    file->pos += bytes_read;
  return bytes_read;
}

/* Reads SIZE bytes from FILE into BUFFER,
   starting at offset FILE_OFS in the file.
   Returns the number of bytes actually read,
   which may be less than SIZE if end of file is reached.
   The file's current position is unaffected. */
off_t
file_read_at (struct file *file, void *buffer, off_t size, off_t file_ofs) 
{
  ASSERT (file != NULL);
  ASSERT (file->magic == FILE_MAGIC);
  
  return pifs_read (file->inode, file_ofs, size, buffer);
}

/* Writes SIZE bytes from BUFFER into FILE,
   starting at the file's current position.
   Returns the number of bytes actually written,
   which may be less than SIZE if end of file is reached.
   (Normally we'd grow the file in that case, but file growth is
   not yet implemented.)
   Advances FILE's position by the number of bytes read. */
off_t
file_write (struct file *file, const void *buffer, off_t size) 
{
  ASSERT (file != NULL);
  ASSERT (file->magic == FILE_MAGIC);
  
  off_t bytes_written = pifs_write (file->inode, file->pos, size, buffer);
  if (bytes_written > 0)
    file->pos += bytes_written;
  return bytes_written;
}

/* Writes SIZE bytes from BUFFER into FILE,
   starting at offset FILE_OFS in the file.
   Returns the number of bytes actually written,
   which may be less than SIZE if end of file is reached.
   (Normally we'd grow the file in that case, but file growth is
   not yet implemented.)
   The file's current position is unaffected. */
off_t
file_write_at (struct file *file, const void *buffer, off_t size,
               off_t file_ofs) 
{
  ASSERT (file != NULL);
  ASSERT (file->magic == FILE_MAGIC);
  
  return pifs_write (file->inode, file_ofs, size, buffer);
}

/* Prevents write operations on FILE's underlying inode
   until file_allow_write() is called or FILE is closed. */
void
file_deny_write (struct file *file) 
{
  ASSERT (file != NULL);
  ASSERT (file->magic == FILE_MAGIC);
  
  if (!file->deny_write) 
    {
      file->deny_write = true;
      ++file->inode->deny_write_cnt;
    }
}

/* Re-enables write operations on FILE's underlying inode.
   (Writes might still be denied by some other file that has the
   same inode open.) */
void
file_allow_write (struct file *file) 
{
  ASSERT (file != NULL);
  ASSERT (file->magic == FILE_MAGIC);
  
  if (file->deny_write) 
    {
      file->deny_write = false;
      ASSERT (file->inode->deny_write_cnt > 0);
      --file->inode->deny_write_cnt;
    }
}

/* Returns the size of FILE in bytes. */
off_t
file_length (struct file *file) 
{
  ASSERT (file != NULL);
  ASSERT (file->magic == FILE_MAGIC);
  
  return file->inode->length;
}

/* Sets the current position in FILE to NEW_POS bytes from the
   start of the file. */
void
file_seek (struct file *file, off_t new_pos)
{
  ASSERT (file != NULL);
  ASSERT (file->magic == FILE_MAGIC);
  
  ASSERT (new_pos >= 0);
  file->pos = new_pos;
}

/* Returns the current position in FILE as a byte offset from the
   start of the file. */
off_t
file_tell (struct file *file) 
{
  ASSERT (file != NULL);
  ASSERT (file->magic == FILE_MAGIC);
  
  return file->pos;
}
