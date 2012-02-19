#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

struct file;

#define PROCESS_STACK_SIZE (8*1024*1024)

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

// interface for syscall.c and thread.h
struct fd
{
  unsigned          fd;
  struct hash_elem  hash_elem;
  struct heap_elem  heap_elem;
  struct file      *file;
  size_t            nth_readdir;
};

bool fd_less (const struct hash_elem *a, const struct hash_elem *b, void *t);
unsigned fd_hash (const struct hash_elem *e, void *t);
bool fd_heap_less (const struct heap_elem*, const struct heap_elem*, void*);
void fd_free (struct hash_elem *e, void *aux);

#endif /* userprog/process.h */
