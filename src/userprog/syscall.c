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
#include "devices/input.h"
#ifdef VM
# include "vm/vm.h"
#endif

#define TODO_NO_RETURN NO_RETURN

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
  signed len = user_strlen (g, filename);
  if (len < 0)
    kill_segv (g);
  if_->eax = SYNC (filesys_create (filename, *(unsigned *) arg2));
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
  struct fd *fd = calloc (1, sizeof (*fd));
  if (!fd)
    {
      if_->eax = -ENOMEM;
      vm_ensure_group_destroy (g);
      return;
    }
  
  for (fd->fd = 3; fd->fd < INT_MAX; ++fd->fd)
    if (hash_find (&g->thread->fds, &fd->elem) == NULL)
      break;
  if (fd->fd == INT_MAX)
    {
      free (fd);
      if_->eax = -ENFILE;
      vm_ensure_group_destroy (g);
      return;
    }
  
  fd->file = SYNC (filesys_open (filename));
  if (!fd->file)
    {
      free (fd);
      if_->eax = -ENOENT;
      vm_ensure_group_destroy (g);
      return;
    }

  hash_insert (&g->thread->fds, &fd->elem);
  if_->eax = fd->fd;
  vm_ensure_group_destroy (g);
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
  ENSURE_USER_ARGS (1);
  
  struct fd *fd_data = retrieve_fd (*(unsigned *) arg1);
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
      struct fd *fd_data = retrieve_fd (fd);
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
  
  struct fd *fd_data = retrieve_fd (fd);
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
  
  struct fd *fd_data = retrieve_fd (fd);
  if (!fd_data)
    kill_segv (g);
  SYNC_VOID (file_seek (fd_data->file, position));
}

static void
syscall_handler_SYS_TELL (_SYSCALL_HANDLER_ARGS)
{
  // unsigned tell (int fd);
  ENSURE_USER_ARGS (1);
  
  struct fd *fd_data = retrieve_fd (*(unsigned *) arg1);
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
  
  struct fd *fd_data = retrieve_fd (*(unsigned *) arg1);
  if (!fd_data)
    kill_segv (g);
  
  hash_delete_found (&g->thread->fds, &fd_data->elem);
  vm_ensure_group_destroy (g);
  
  SYNC_VOID (file_close (fd_data->file));
  free (fd_data);
}

static void
syscall_handler_SYS_MMAP (_SYSCALL_HANDLER_ARGS)
{
  // mapid_t mmap (int fd, void *addr);
  ENSURE_USER_ARGS (2);
  
  struct fd *fd_data = retrieve_fd (*(unsigned *) arg1);
  void *base = *(void **) arg2;
  vm_ensure_group_destroy (g);
  
  // TODO
  (void) fd_data;
  (void) base;
}

static void
syscall_handler_SYS_MUNMAP (_SYSCALL_HANDLER_ARGS)
{
  // void munmap (mapid_t);
  ENSURE_USER_ARGS (1);
  
  mapid_t map = *(mapid_t *) arg2;
  vm_ensure_group_destroy (g);
  
  // TODO
  (void) map;
}

static void TODO_NO_RETURN
syscall_handler_SYS_CHDIR (_SYSCALL_HANDLER_ARGS)
{
  //TODO
  
  kill_segv (g);
}

static void TODO_NO_RETURN
syscall_handler_SYS_MKDIR (_SYSCALL_HANDLER_ARGS)
{
  //TODO
  
  kill_segv (g);
}

static void TODO_NO_RETURN
syscall_handler_SYS_READDIR (_SYSCALL_HANDLER_ARGS)
{
  //TODO
  
  kill_segv (g);
}

static void TODO_NO_RETURN
syscall_handler_SYS_ISDIR (_SYSCALL_HANDLER_ARGS)
{
  //TODO
  
  kill_segv (g);
}

static void TODO_NO_RETURN
syscall_handler_SYS_INUMBER (_SYSCALL_HANDLER_ARGS)
{
  //TODO
  
  kill_segv (g);
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
      printf ("Invalid system call!\n");
      thread_exit ();
  }
}
