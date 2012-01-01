#ifndef VM_H__
#define VM_H__

#include <stdbool.h>
#include <hash.h>
#include "threads/thread.h"

void vm_init (void);

void vm_init_thread (struct thread *t);
void vm_clean (struct thread *t);

// vm_(alloc_and_)ensure return kpage or NULL, if failed

bool vm_alloc_zero (struct thread *t, void *addr);
void *vm_alloc_and_ensure (struct thread *t, void *addr);
void vm_dispose (struct thread *t, void *addr);

void vm_swap_disposed (struct thread *t, void *base);

void vm_tick (struct thread *t);

enum vm_ensure_result
{
  VMER_OK,
  VMER_SEGV,
  VMER_OOM,
  
  VMER_LENGTH
};
enum vm_ensure_result vm_ensure (struct thread *t, void *base, void **kpage_);

struct vm_ensure_group
{
  struct thread *thread;
  struct hash    entries;
};
void vm_ensure_group_init (struct vm_ensure_group *g, struct thread *t);
void vm_ensure_group_destroy (struct vm_ensure_group *g);
enum vm_ensure_result vm_ensure_group_add (struct vm_ensure_group *g,
                                           void *upage,
                                           void **kpage_);
bool vm_ensure_group_remove (struct vm_ensure_group *g, void *upage);

#endif
