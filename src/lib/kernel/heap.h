#ifndef HEAP
#define HEAP

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * This structure implements a min-heap.
 */

struct heap_elem
  {
    unsigned index;
    uint32_t magic;
  };

typedef bool heap_less_func (const struct heap_elem *a,
                             const struct heap_elem *b,
                             void *aux);

struct heap
  {
    size_t size, elem_cnt;
    heap_less_func *less;
    void *aux;
    struct heap_elem **data;
    uint32_t magic;
  };

void heap_init (struct heap *heap, heap_less_func *less, void *aux);
void heap_clear (struct heap *heap);
void heap_destroy (struct heap *heap);

bool heap_insert (struct heap *heap, struct heap_elem *elem);
void heap_changed_key (struct heap *heap, struct heap_elem *elem);
struct heap_elem *heap_peek_min (struct heap *heap);
struct heap_elem *heap_delete_min (struct heap *heap);
void heap_delete (struct heap *heap, struct heap_elem *elem);

#define heap_entry(HEAP_ELEM, STRUCT, MEMBER) \
    ((STRUCT*) ((uintptr_t) (HEAP_ELEM) - offsetof (STRUCT, MEMBER)))

#endif
