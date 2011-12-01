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
#include "lib/user/syscall.h"

static void syscall_handler (struct intr_frame *);

static struct lock filesys_lock;

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
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

#define _SYSCALL_HANDLER_ARGS void              *arg1 UNUSED, \
                              void              *arg2 UNUSED, \
                              void              *arg3 UNUSED, \
                              struct intr_frame *if_  UNUSED

static bool
is_user_memory (const void *addr, unsigned size)
{
  if (size == 0)
    return true;
  if (addr >= PHYS_BASE)
    return false;
  intptr_t start = (intptr_t) addr;
  intptr_t end   = start + size;
  if (end < start)
    return false;
  if ((void *) end > PHYS_BASE)
    return false;
  struct thread *t = thread_current ();
  intptr_t i;
  for (i = start & ~(PGSIZE-1); i < end; i += PGSIZE)
    if (pagedir_get_page (t->pagedir, (void *) i) == NULL)
      return false;
  return true;
}


static void __attribute__ ((noreturn))
kill_segv (void)
{
  struct thread *t = thread_current ();
  // printf ("Killed %d (%.*s), because of bad memory usage.\n",
  //         t->tid, sizeof (t->name), t->name);
  thread_exit ();
}

/**
 * strlen (c). Ensures c and following (upto result+1) characters are in
 * user memory.
 * 
 * \return -1, if (hits) invalid memory
 */
static signed
user_strlen (const char *c)
{
  if (!c)
    return -1;
  
  signed result = 0;
  struct thread *t = thread_current ();
  for (;;)
    {
      if ((void *) c >= PHYS_BASE)
        return -1;
      
      const char *downfrom = (const char *) ((intptr_t) c & ~(PGSIZE-1));
      const char *upto     = (const char *) ((intptr_t) c |  (PGSIZE-1));
      
      if (pagedir_get_page (t->pagedir, downfrom) == NULL)
        return -1;
      while (c <= upto)
        if (*(c++))
          ++result;
        else
          return result;
    }
}

static void
syscall_handler_SYS_HALT (_SYSCALL_HANDLER_ARGS)
{
  // void halt (void) NO_RETURN;
  shutdown_power_off ();
}

static void
syscall_handler_SYS_EXIT (_SYSCALL_HANDLER_ARGS)
{
  // void exit (int status) NO_RETURN;
  
  thread_current ()->exit_code = *(int *) arg1;
  thread_exit ();
}

static void
syscall_handler_SYS_EXEC (_SYSCALL_HANDLER_ARGS)
{
  // pid_t exec (const char *file);
  
  const char *file = *(const char **) arg1;
  signed len = user_strlen (file);
  if (len < 0)
    kill_segv ();
  if ((unsigned) len >= sizeof (((struct thread *) 0)->name))
    thread_exit ();
  if_->eax = process_execute (file);
}

static void
syscall_handler_SYS_WAIT (_SYSCALL_HANDLER_ARGS)
{
  // int wait (pid_t);
  
  tid_t child = *(tid_t *) arg1;
  if_->eax = process_wait (child);
}

static void
syscall_handler_SYS_CREATE (_SYSCALL_HANDLER_ARGS)
{
  // bool create (const char *file, unsigned initial_size);
  
  const char *filename = *(const char **) arg1;
  signed len = user_strlen (filename);
  if (len < 0)
    kill_segv ();
  if (len > READDIR_MAX_LEN)
    thread_exit ();
  if_->eax = SYNC (filesys_create (filename, *(off_t *) arg2));
}

static void
syscall_handler_SYS_REMOVE (_SYSCALL_HANDLER_ARGS)
{
  // bool remove (const char *file);
  
  const char *filename = *(const char **) arg1;
  signed len = user_strlen (filename);
  if (len < 0)
    kill_segv ();
  if (len > READDIR_MAX_LEN)
    thread_exit ();
  if_->eax = SYNC (filesys_remove (filename));
}

static void
syscall_handler_SYS_OPEN (_SYSCALL_HANDLER_ARGS)
{
  // int open (const char *file);
  
  const char *filename = *(const char **) arg1;
  signed len = user_strlen (filename);
  if (len < 0)
    kill_segv ();
  if (len > READDIR_MAX_LEN)
    {
      if_->eax = -ENAMETOOLONG;
      return;
    }
    
  struct fd *fd = calloc (1, sizeof (*fd));
  if (!fd)
    {
      if_->eax = -ENOMEM;
      return;
    }
  
  struct thread *current_thread = thread_current ();
  for (fd->fd = 3; fd->fd < INT_MAX; ++fd->fd)
      if (hash_find (&current_thread->fds, &fd->elem) == NULL)
        break;
  if (fd->fd == INT_MAX)
    {
      free (fd);
      if_->eax = -ENFILE;
      return;
    }
  
  fd->file = SYNC (filesys_open (filename));
  if (!fd->file)
    {
      free (fd);
      if_->eax = -ENOENT;
      return;
    }

  hash_insert (&current_thread->fds, &fd->elem);
  if_->eax = fd->fd;
}

static struct fd *
retrieve_fd (unsigned fd)
{
  struct fd search;
  memset (&search, 0, sizeof (search));
  search.fd = fd;
  struct hash_elem *e = hash_find (&thread_current ()->fds, &search.elem);
  return e ? hash_entry (e, struct fd, elem) : NULL;
}

static void
syscall_handler_SYS_FILESIZE (_SYSCALL_HANDLER_ARGS)
{
  // int filesize (int fd);
  
  struct fd *fd_data = retrieve_fd (*(unsigned *) arg1);
  if_->eax = fd_data ? SYNC (file_length (fd_data->file)) : -1;
}

static void
syscall_handler_SYS_READ (_SYSCALL_HANDLER_ARGS)
{
  // int read (int fd, void *buffer, unsigned length);
  
  unsigned fd = *(unsigned *) arg1;
  char *buffer = *(void **) arg2;
  unsigned length = *(unsigned *) arg3;
  
  if (!is_user_memory (buffer, length))
    kill_segv ();
  struct fd *fd_data = retrieve_fd (fd);
  if (!fd_data)
    {
      if_->eax = -EBADF;
      return;
    }
  if_->eax = SYNC (file_read (fd_data->file, buffer, length));
}

static void
syscall_handler_SYS_WRITE (_SYSCALL_HANDLER_ARGS)
{
  // int write (int fd, const void *buffer, unsigned length);
  
  unsigned fd = *(unsigned *) arg1;
  const char *buffer = *(const void **) arg2;
  unsigned length = *(unsigned *) arg3;
  
  if (!is_user_memory (buffer, length))
    kill_segv ();
  else if (fd == 0 || fd >= INT_MAX)
    {
      if_->eax = -EBADF;
      return;
    }
  else if (fd == 1 || fd == 2)
    {
      putbuf (buffer, length);
      if_->eax = length;
      return;
    }
  
  struct fd *fd_data = retrieve_fd (fd);
  if_->eax = fd_data ? SYNC (file_write (fd_data->file, buffer, length))
                     : -EBADF;
}

static void
syscall_handler_SYS_SEEK (_SYSCALL_HANDLER_ARGS)
{
  // void seek (int fd, unsigned position);
  
  unsigned fd = *(unsigned *) arg1;
  unsigned position = *(unsigned *) arg2;
  
  struct fd *fd_data = retrieve_fd (fd);
  if (!fd_data)
    kill_segv ();
  SYNC_VOID (file_seek (fd_data->file, position));
}

static void
syscall_handler_SYS_TELL (_SYSCALL_HANDLER_ARGS)
{
  // unsigned tell (int fd);
  
  struct fd *fd_data = retrieve_fd (*(unsigned *) arg1);
  if (!fd_data)
    kill_segv ();
  if_->eax = SYNC (file_tell (fd_data->file));
}

static void
syscall_handler_SYS_CLOSE (_SYSCALL_HANDLER_ARGS)
{
  // void close (int fd);
  
  struct fd search;
  memset (&search, 0, sizeof (search));
  search.fd = *(unsigned *) arg1;
  struct hash_elem *e = hash_delete (&thread_current ()->fds, &search.elem);
  if (!e)
    kill_segv ();
  struct fd *fd_data = hash_entry (e, struct fd, elem);
  SYNC_VOID (file_close (fd_data->file));
  free (fd_data);
}

static void
syscall_handler_SYS_MMAP (_SYSCALL_HANDLER_ARGS)
{
  //TODO
}

static void
syscall_handler_SYS_MUNMAP (_SYSCALL_HANDLER_ARGS)
{
  //TODO
}

static void
syscall_handler_SYS_CHDIR (_SYSCALL_HANDLER_ARGS)
{
  //TODO
}

static void
syscall_handler_SYS_MKDIR (_SYSCALL_HANDLER_ARGS)
{
  //TODO
}

static void
syscall_handler_SYS_READDIR (_SYSCALL_HANDLER_ARGS)
{
  //TODO
}

static void
syscall_handler_SYS_ISDIR (_SYSCALL_HANDLER_ARGS)
{
  //TODO
}

static void
syscall_handler_SYS_INUMBER (_SYSCALL_HANDLER_ARGS)
{
  //TODO
}

static void
syscall_handler (struct intr_frame *if_) 
{
  if (!is_user_memory (if_->esp, 4*sizeof (void *)))
    kill_segv();
  
  int  *nr       = &((int   *) if_->esp)[0];
  void *arg1     = &((void **) if_->esp)[1];
  void *arg2     = &((void **) if_->esp)[2];
  void *arg3     = &((void **) if_->esp)[3];
  
  #define _HANDLE(NAME) case NAME: \
                          syscall_handler_##NAME (arg1, arg2, arg3, if_); \
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
      printf ("Invalid system call!\n");
      thread_exit ();
  }
}
