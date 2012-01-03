#ifndef VM_H__
#define VM_H__

#include <stdbool.h>
#include <hash.h>
#include "threads/thread.h"
#include "filesys/file.h"

struct vm_page;

typedef int mapid_t;
#define MAP_FAILED ((mapid_t) -1)

void vm_init (void);

void vm_init_thread (struct thread *t);
void vm_clean (struct thread *t);

// vm_(alloc_and_)ensure return kpage or NULL, if failed

bool vm_alloc_zero (struct thread *t, void *user_addr, bool readonly);
void *vm_alloc_and_ensure (struct thread *t, void *user_addr, bool readonly);
void vm_dispose (struct thread *t, void *user_addr);

void vm_swap_disposed (struct thread *t, void *base);

void vm_tick (struct thread *t);

enum vm_ensure_result
{
  VMER_OK,
  VMER_SEGV,
  VMER_OOM,
  
  VMER_LENGTH
};
enum vm_ensure_result vm_ensure (struct thread  *t,
                                 void           *user_addr,
                                 void          **kpage_);

mapid_t vm_mmap_open (struct thread *t, void *user_addr, struct file *file);
bool vm_mmap_close (struct thread *t, mapid_t map);

enum vm_is_readonly_result
{
  VMIR_INVALID,
  VMIR_READONLY,
  VMIR_READWRITE,
  
  VMIR_LENGTH
};
enum vm_is_readonly_result vm_is_readonly (struct thread *t, void *user_addr);

struct vm_ensure_group
{
  struct thread *thread;
  struct hash    entries;
};
void vm_ensure_group_init (struct vm_ensure_group *g, struct thread *t);
void vm_ensure_group_destroy (struct vm_ensure_group *g);
enum vm_ensure_result vm_ensure_group_add (struct vm_ensure_group *g,
                                           void *user_addr,
                                           void **kpage_);
bool vm_ensure_group_remove (struct vm_ensure_group *g, void *user_addr);

#endif
