#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include <limits.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/interrupt.h"
#include "devices/input.h"
#include "vm/vm.h"

//#define SYSCALL_DEBUG(...) printf (__VA_ARGS__)
#define SYSCALL_DEBUG(...)

static void syscall_handler (struct intr_frame *);

static struct lock filesys_lock, stdin_lock;

#define SYNC(WHAT)              \
({                              \
  lock_acquire (&filesys_lock); \
  __typeof (WHAT) _r = (WHAT);  \
  lock_release (&filesys_lock); \
  _r;                           \
})
#define SYNC_VOID(WHAT) \
({                      \
  SYNC ( (WHAT, 0) );   \
  (void) 0;             \
})

void
syscall_init (void) 
{
  lock_init (&filesys_lock);
  lock_init (&stdin_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

#define _SYSCALL_HANDLER_ARGS struct vm_ensure_group *g,           \
                              void                   *arg1 UNUSED, \
                              void                   *arg2 UNUSED, \
                              void                   *arg3 UNUSED, \
                              struct intr_frame      *if_  UNUSED

static bool
ensure_user_memory (struct vm_ensure_group *g,
                    void *addr,
                    unsigned size,
                    bool for_writing)
{
  ASSERT (g != NULL);
  
  if (size == 0) // nothing to read
    return true;
  intptr_t start = (intptr_t) addr;
  intptr_t end   = start + size;
  if (end < start) // we have an overflow, cannot be a valid pointer
    return false;
    
  intptr_t i;
  // look for all tangented pages
  void *kpage;
  for (i = start & ~(PGSIZE-1); i < end; i += PGSIZE)
    {
      enum vm_is_readonly_result r = vm_ensure_group_is_readonly (g, (void *) i);
      if (r == VMIR_INVALID)
        return false;
      if (for_writing && r == VMIR_READONLY)
        return false;
      if (vm_ensure_group_add (g, (void *) i, &kpage) != VMER_OK)
        return false;
    }
    
  return true;
}


static void __attribute__ ((noreturn))
kill_segv (struct vm_ensure_group *g)
{
  ASSERT (g != NULL);
  
  vm_ensure_group_destroy (g);
  thread_exit ();
}

/**
 * strlen (c). Ensures c and following (upto result+1) characters are in
 * user memory.
 * 
 * \return -1, if (hits) invalid memory
 */
static signed
user_strlen (struct vm_ensure_group *g, char *c)
{
  ASSERT (g != NULL);
  
  if (!c)
    return -1;
  
  signed result = 0;
  for (;;)
    {
      if ((void *) c >= PHYS_BASE)
        return -1;
      
      char *downfrom = (char *) ((intptr_t) c & ~(PGSIZE-1));
      char *upto     = (char *) ((intptr_t) c |  (PGSIZE-1));
      
      void *kpage;
      if (vm_ensure_group_add (g, downfrom, &kpage) != VMER_OK)
        return -1;
      while (c <= upto)
        if (*(c++))
          ++result;
        else
          return result;
    }
}

static void NO_RETURN
syscall_handler_SYS_HALT (_SYSCALL_HANDLER_ARGS)
{
  // void halt (void) NO_RETURN;
  vm_ensure_group_destroy (g);
  shutdown_power_off ();
}

#define ENSURE_USER_ARGS(COUNT)                                      \
({                                                                   \
  if (!ensure_user_memory (g, arg1, sizeof (arg1) * (COUNT), false)) \
    kill_segv (g);                                                   \
  (void) 0;                                                          \
})

static void NO_RETURN
syscall_handler_SYS_EXIT (_SYSCALL_HANDLER_ARGS)
{
  // void exit (int status) NO_RETURN;
  ENSURE_USER_ARGS (1);
  
  thread_current ()->exit_code = *(int *) arg1;
  vm_ensure_group_destroy (g);
  thread_exit ();
}

static void
syscall_handler_SYS_EXEC (_SYSCALL_HANDLER_ARGS)
{
  // pid_t exec (const char *file);
  ENSURE_USER_ARGS (1);
  
  char *file = *(char **) arg1;
  signed len = user_strlen (g, file);
  if (len < 0)
    kill_segv (g);
  if_->eax = SYNC (process_execute (file));
  vm_ensure_group_destroy (g);
}

static void
syscall_handler_SYS_WAIT (_SYSCALL_HANDLER_ARGS)
{
  // int wait (pid_t);
  ENSURE_USER_ARGS (1);
  
  tid_t child = *(tid_t *) arg1;
  vm_ensure_group_destroy (g);
  if_->eax = process_wait (child);
}

static void
syscall_handler_SYS_CREATE (_SYSCALL_HANDLER_ARGS)
{
  // bool create (const char *file, unsigned initial_size);
  ENSURE_USER_ARGS (2);
  
  char *filename = *(char **) arg1;
  unsigned initial_size = *(unsigned *) arg2;
  signed len = user_strlen (g, filename);
  if (len < 0)
    kill_segv (g);
  if_->eax = SYNC (filesys_create (filename, initial_size));
  vm_ensure_group_destroy (g);
}

static void
syscall_handler_SYS_REMOVE (_SYSCALL_HANDLER_ARGS)
{
  // bool remove (const char *file);
  ENSURE_USER_ARGS (1);
  
  char *filename = *(char **) arg1;
  signed len = user_strlen (g, filename);
  if (len < 0)
    kill_segv (g);
  if_->eax = SYNC (filesys_remove (filename));
  vm_ensure_group_destroy (g);
}

static void
syscall_handler_SYS_OPEN (_SYSCALL_HANDLER_ARGS)
{
  // int open (const char *file);
  ENSURE_USER_ARGS (1);
  
  char *filename = *(char **) arg1;
  signed len = user_strlen (g, filename);
  if (len < 0)
    kill_segv (g);
    
  SYSCALL_DEBUG ("open (\"%s\")\n", filename);
  
  struct fd *fd = calloc (1, sizeof (*fd));
  if (!fd)
    {
      if_->eax = -ENOMEM;
      vm_ensure_group_destroy (g);
      return;
    }
  
  struct heap_elem *e = heap_peek_min (&g->thread->fds_heap);
  if (e != NULL)
    {
      fd->fd = hash_entry (e, struct fd, heap_elem)->fd;
      if (fd->fd >= INT_MAX || fd->fd < 3)
        {
          free (fd);
          if_->eax = -ENFILE;
          vm_ensure_group_destroy (g);
          return;
        }
      ++fd->fd;
    }
  else
    fd->fd = 3;
  
  fd->file = SYNC (filesys_open (filename));
  if (!fd->file)
    {
      free (fd);
      if_->eax = -ENOENT;
      vm_ensure_group_destroy (g);
      return;
    }

  hash_insert (&g->thread->fds_hash, &fd->hash_elem);
  heap_insert (&g->thread->fds_heap, &fd->heap_elem);
  if_->eax = fd->fd;
  vm_ensure_group_destroy (g);
}

static struct fd *
retrieve_fd (struct thread *t, unsigned fd)
{
  struct fd search;
  memset (&search, 0, sizeof (search));
  search.fd = fd;
  struct hash_elem *e = hash_find (&t->fds_hash, &search.hash_elem);
  return e ? hash_entry (e, struct fd, hash_elem) : NULL;
}

static void
syscall_handler_SYS_FILESIZE (_SYSCALL_HANDLER_ARGS)
{
  // int filesize (int fd);
  ENSURE_USER_ARGS (1);
  
  struct fd *fd_data = retrieve_fd (g->thread, *(unsigned *) arg1);
  vm_ensure_group_destroy (g);
  if_->eax = fd_data ? SYNC (file_length (fd_data->file)) : -1;
}

static void
syscall_handler_SYS_READ (_SYSCALL_HANDLER_ARGS)
{
  // int read (int fd, void *buffer, unsigned length);
  ENSURE_USER_ARGS (3);
  
  unsigned fd = *(unsigned *) arg1;
  char *buffer = *(void **) arg2;
  unsigned length = *(unsigned *) arg3;
  
  if (!ensure_user_memory (g, buffer, length, true))
    kill_segv (g);
  
  int result = 0;
    
  if (fd != 0)
    {
      struct fd *fd_data = retrieve_fd (g->thread, fd);
      if (!fd_data)
        {
          if_->eax = -EBADF;
          vm_ensure_group_destroy (g);
          return;
        }
      result = SYNC (file_read (fd_data->file, buffer, length));
    }
  else
    {
      char *dest = buffer;
      lock_acquire (&stdin_lock);
      while (input_full () && (unsigned) result < length)
        {
          *dest = input_getc ();
          ++dest;
          ++result;
        }
      *dest = '\0';
      lock_release (&stdin_lock);
    }
  if_->eax = result;
  vm_kernel_wrote (g->thread, buffer, result);
  vm_ensure_group_destroy (g);
}

static void
syscall_handler_SYS_WRITE (_SYSCALL_HANDLER_ARGS)
{
  // int write (int fd, const void *buffer, unsigned length);
  ENSURE_USER_ARGS (3);
  
  unsigned fd = *(unsigned *) arg1;
  char *buffer = *(void **) arg2;
  unsigned length = *(unsigned *) arg3;
  
  if (!ensure_user_memory (g, buffer, length, false))
    kill_segv (g);
  else if (fd == 0 || fd >= INT_MAX)
    {
      if_->eax = -EBADF;
      vm_ensure_group_destroy (g);
      return;
    }
  else if (fd == 1 || fd == 2)
    {
      putbuf (buffer, length);
      if_->eax = length;
      vm_ensure_group_destroy (g);
      return;
    }
  
  struct fd *fd_data = retrieve_fd (g->thread, fd);
  if (fd_data)
    {
      lock_acquire (&filesys_lock);
      if (!thread_is_file_currently_executed (fd_data->file))
        if_->eax = file_write (fd_data->file, buffer, length);
      else
        if_->eax = 0;
      lock_release (&filesys_lock);
    }
  else
    if_->eax = -EBADF;
  vm_ensure_group_destroy (g);
}

static void
syscall_handler_SYS_SEEK (_SYSCALL_HANDLER_ARGS)
{
  // void seek (int fd, unsigned position);
  ENSURE_USER_ARGS (2);
  
  unsigned fd = *(unsigned *) arg1;
  unsigned position = *(unsigned *) arg2;
  vm_ensure_group_destroy (g);
  
  struct fd *fd_data = retrieve_fd (g->thread, fd);
  if (!fd_data)
    kill_segv (g);
  SYNC_VOID (file_seek (fd_data->file, position));
}

static void
syscall_handler_SYS_TELL (_SYSCALL_HANDLER_ARGS)
{
  // unsigned tell (int fd);
  ENSURE_USER_ARGS (1);
  
  struct fd *fd_data = retrieve_fd (g->thread, *(unsigned *) arg1);
  if (!fd_data)
    kill_segv (g);
  vm_ensure_group_destroy (g);
  if_->eax = SYNC (file_tell (fd_data->file));
}

static void
syscall_handler_SYS_CLOSE (_SYSCALL_HANDLER_ARGS)
{
  // void close (int fd);
  ENSURE_USER_ARGS (1);
  
  struct fd *fd_data = retrieve_fd (g->thread, *(unsigned *) arg1);
  if (!fd_data)
    kill_segv (g);
  
  hash_delete_found (&g->thread->fds_hash, &fd_data->hash_elem);
  heap_delete (&g->thread->fds_heap, &fd_data->heap_elem);
  vm_ensure_group_destroy (g);
  
  SYNC_VOID (file_close (fd_data->file));
  free (fd_data);
}

static void
syscall_handler_SYS_MMAP (_SYSCALL_HANDLER_ARGS)
{
  // mapid_t mmap (int fd, void *addr);
  ENSURE_USER_ARGS (2);
  
  struct fd *fd_data = retrieve_fd (g->thread, *(unsigned *) arg1);
  void *base = *(void **) arg2;
  vm_ensure_group_destroy (g);
  
  if (!fd_data || base < MIN_ALLOC_ADDR || pg_ofs (base) != 0)
    {
      if_->eax = MAP_FAILED;
      return;
    }
  
  mapid_t id = vm_mmap_acquire (g->thread, file_get_inode (fd_data->file));
  if (id == MAP_FAILED || vm_mmap_pages (g->thread, id, base))
    if_->eax = id;
  else
    {
      bool dispose_result UNUSED;
      dispose_result = vm_mmap_dispose (g->thread, id);
      ASSERT (dispose_result);
      if_->eax = MAP_FAILED;
    }
}

static void
syscall_handler_SYS_MUNMAP (_SYSCALL_HANDLER_ARGS)
{
  // void munmap (mapid_t);
  ENSURE_USER_ARGS (1);
  
  mapid_t id = *(mapid_t *) arg1;
  vm_ensure_group_destroy (g);
  
  if (id == MAP_FAILED)
    return;
  vm_mmap_dispose (g->thread, id);
}

static void
syscall_handler_SYS_CHDIR (_SYSCALL_HANDLER_ARGS)
{
  // bool chdir (const char *dir);
  ENSURE_USER_ARGS (1);
  
  char *dir = *(char **) arg1;
  if (user_strlen (g, dir) < 0)
    kill_segv (g);
    
  SYSCALL_DEBUG ("chdir (\"%s\")\n", dir);
    
  struct file *new_cwd_file = filesys_open (dir);
  vm_ensure_group_destroy (g);
  
  if_->eax = new_cwd_file != NULL;
  if (if_->eax)
    {
      struct thread *t = thread_current ();
      pifs_close (t->cwd);
      t->cwd = file_get_inode (new_cwd_file);
      __sync_fetch_and_add(&t->cwd->open_count, 1);
      file_close (new_cwd_file);
    }
}

static void
syscall_handler_SYS_MKDIR (_SYSCALL_HANDLER_ARGS)
{
  // bool mkdir (const char *dir);
  ENSURE_USER_ARGS (1);
  
  char *filename = *(char **) arg1;
  signed len = user_strlen (g, filename);
  if (len < 0)
    kill_segv (g);
    
  SYSCALL_DEBUG ("mkdir (\"%s\")\n", filename);
  
  if_->eax = SYNC (filesys_create_folder (filename));
  vm_ensure_group_destroy (g);
}

#define READDIR_MAX_LEN 14
static void
syscall_handler_SYS_READDIR (_SYSCALL_HANDLER_ARGS)
{
  // bool readdir (int fd, char name[READDIR_MAX_LEN + 1]);
  ENSURE_USER_ARGS (2);
  
  struct fd *fd_data = retrieve_fd (g->thread, *(unsigned *) arg1);
  if (!fd_data)
    kill_segv (g);
  char *name = *(char **) arg2;
  if (!ensure_user_memory (g, name, READDIR_MAX_LEN + 1, true))
    kill_segv (g);
    
  if_->eax = false;
  lock_acquire (&filesys_lock);
    
  struct pifs_inode *inode = file_get_inode (fd_data->file);
  
  if (!inode->is_directory || (inode->length <= fd_data->nth_readdir))
    goto end;
    
  off_t len = READDIR_MAX_LEN + 1;
  if (pifs_readdir (inode, fd_data->nth_readdir, &len, name))
    {
      ++fd_data->nth_readdir;
      if_->eax = true;
    }
  
end:
  lock_release (&filesys_lock);
  vm_ensure_group_destroy (g);
}

static void
syscall_handler_SYS_ISDIR (_SYSCALL_HANDLER_ARGS)
{
  // bool isdir (int fd);
  ENSURE_USER_ARGS (1);
  
  struct fd *fd_data = retrieve_fd (g->thread, *(unsigned *) arg1);
  if (!fd_data)
    kill_segv (g);
    
  vm_ensure_group_destroy (g);
  if_->eax = file_get_inode (fd_data->file)->is_directory;
}

static void
syscall_handler_SYS_INUMBER (_SYSCALL_HANDLER_ARGS)
{
  // int inumber (int fd);
  ENSURE_USER_ARGS (1);
  
  struct fd *fd_data = retrieve_fd (g->thread, *(unsigned *) arg1);
  if (!fd_data)
    kill_segv (g);
  vm_ensure_group_destroy (g);
  if_->eax = SYNC (file_get_inode (fd_data->file)->sector);
}

static void
syscall_handler (struct intr_frame *if_) 
{
  struct vm_ensure_group g;
  vm_ensure_group_init (&g, thread_current (), if_->esp);
  
  int  *nr       = &((int   *) if_->esp)[0];
  void *arg1     = &((void **) if_->esp)[1];
  void *arg2     = &((void **) if_->esp)[2];
  void *arg3     = &((void **) if_->esp)[3];
  
  if (!ensure_user_memory (&g, nr, sizeof (nr), false))
    kill_segv (&g);
  
  #define _HANDLE(NAME) case NAME: \
                          syscall_handler_##NAME (&g, arg1, arg2, arg3, if_); \
                          break;
  
  switch (*nr) {
    _HANDLE (SYS_HALT);
    _HANDLE (SYS_EXIT);
    _HANDLE (SYS_EXEC);
    _HANDLE (SYS_WAIT);
    _HANDLE (SYS_CREATE);
    _HANDLE (SYS_REMOVE);
    _HANDLE (SYS_OPEN);
    _HANDLE (SYS_FILESIZE);
    _HANDLE (SYS_READ);
    _HANDLE (SYS_WRITE);
    _HANDLE (SYS_SEEK);
    _HANDLE (SYS_TELL);
    _HANDLE (SYS_CLOSE);
    _HANDLE (SYS_MMAP);
    _HANDLE (SYS_MUNMAP);
    _HANDLE (SYS_CHDIR);
    _HANDLE (SYS_MKDIR);
    _HANDLE (SYS_READDIR);
    _HANDLE (SYS_ISDIR);
    _HANDLE (SYS_INUMBER);
    default:
      kill_segv (&g);
  }
}
