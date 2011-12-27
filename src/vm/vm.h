#ifndef VM_H__
#define VM_H__

#include <stdbool.h>
#include "threads/thread.h"

void vm_init (void);

void vm_init_thread (struct thread *t);
void vm_clean (struct thread *t);

bool vm_alloc_zero (struct thread *t, void *addr);
bool vm_alloc_and_ensure (struct thread *t, void *addr);
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
enum vm_ensure_result vm_ensure (struct thread *t, void *base);

#endif
