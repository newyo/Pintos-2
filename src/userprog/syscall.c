#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "devices/shutdown.h"

static void syscall_handler (struct intr_frame *);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

#define _SYSCALL_HANDLER_ARGS void              *arg1 UNUSED, \
                              void              *arg2 UNUSED, \
                              void              *arg3 UNUSED, \
                              struct intr_frame *if_  UNUSED

static void
syscall_handler_SYS_HALT (_SYSCALL_HANDLER_ARGS)
{
  shutdown_power_off ();
}

static void
syscall_handler_SYS_EXIT (_SYSCALL_HANDLER_ARGS)
{
  // TODO
  thread_exit ();
}

static void
syscall_handler_SYS_EXEC (_SYSCALL_HANDLER_ARGS)
{
  //TODO
}

static void
syscall_handler_SYS_WAIT (_SYSCALL_HANDLER_ARGS)
{
  //TODO
}

static void
syscall_handler_SYS_CREATE (_SYSCALL_HANDLER_ARGS)
{
  //TODO
}

static void
syscall_handler_SYS_REMOVE (_SYSCALL_HANDLER_ARGS)
{
  //TODO
}

static void
syscall_handler_SYS_OPEN (_SYSCALL_HANDLER_ARGS)
{
  //TODO
}

static void
syscall_handler_SYS_FILESIZE (_SYSCALL_HANDLER_ARGS)
{
  //TODO
}

static void
syscall_handler_SYS_READ (_SYSCALL_HANDLER_ARGS)
{
  //TODO
}

static void
syscall_handler_SYS_WRITE (_SYSCALL_HANDLER_ARGS)
{
  // write (*(int *) arg1, *(void **) arg2, *(size_t *) arg3);
  // TODO
  putbuf (*(void **) arg2, *(size_t *) arg3);
}

static void
syscall_handler_SYS_SEEK (_SYSCALL_HANDLER_ARGS)
{
  //TODO
}

static void
syscall_handler_SYS_TELL (_SYSCALL_HANDLER_ARGS)
{
  //TODO
}

static void
syscall_handler_SYS_CLOSE (_SYSCALL_HANDLER_ARGS)
{
  //TODO
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
  // TODO: ensure readability
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
