#include "heap.h"
#include <debug.h>
#include <stdlib.h>
#include "threads/malloc.h"

#define HEAP_ROOT 0

#define HEAP_MAGIC (('H' << 24) + ('E' << 16) + ('A' << 8) + 'P')
#define HEAP_ELEM_MAGIC (('H' << 24) + ('e' << 16) + ('E' << 8) + 'l')

void
heap_init (struct heap *heap, heap_less_func *less, void *aux)
{
  ASSERT (heap != NULL);
  ASSERT (less != NULL);

  heap->size = 0;
  heap->elem_cnt = 0;
  heap->less = less;
  heap->aux = aux;
  heap->data = NULL;
  heap->magic = HEAP_MAGIC;
}

void
heap_clear (struct heap *heap)
{
  ASSERT (heap != NULL);
  ASSERT (heap->magic == HEAP_MAGIC);
  
  heap->elem_cnt = 0;
}

void
heap_destroy (struct heap *heap)
{
  ASSERT (heap != NULL);
  ASSERT (heap->magic == HEAP_MAGIC);
  
  free (heap->data);
  heap->magic ^= -1u;
}

static inline bool
heap_is_root (unsigned item)
{
  return item == HEAP_ROOT;
}

static inline unsigned
heap_get_parent (unsigned child)
{
  return (child-1) / 2;
}

static inline unsigned
heap_get_left_child (unsigned parent)
{
  return parent*2 + 1;
}

static inline unsigned
heap_get_right_child (unsigned parent)
{
  return heap_get_left_child (parent) + 1;
}

static inline bool
heap_less (struct heap *heap, unsigned a, unsigned b)
{
  ASSERT (heap->data[a]->index == a);
  ASSERT (heap->data[b]->index == b);
  ASSERT (heap->data[a]->magic == HEAP_ELEM_MAGIC);
  ASSERT (heap->data[b]->magic == HEAP_ELEM_MAGIC);

  return heap->less (heap->data[a], heap->data[b], heap->aux);
}

static inline void
heap_swap (struct heap *heap, unsigned a, unsigned b)
{
  ASSERT (heap->data[a]->index == a);
  ASSERT (heap->data[b]->index == b);
  ASSERT (heap->data[a]->magic == HEAP_ELEM_MAGIC);
  ASSERT (heap->data[b]->magic == HEAP_ELEM_MAGIC);
  
  struct heap_elem *tmp = heap->data[a];
  heap->data[a] = heap->data[b];
  heap->data[b] = tmp;
  
  heap->data[a]->index = a;
  heap->data[b]->index = b;
}

static void
heap_fix_bottom_up (struct heap *heap, unsigned pos)
{
  while (!heap_is_root (pos))
    {
      if (!heap_less (heap, pos, heap_get_parent (pos)))
        break;
      heap_swap (heap, pos, heap_get_parent (pos));
      pos = heap_get_parent (pos);
    }
}

static void
heap_fix_top_down (struct heap *heap, unsigned pos)
{
  for (;;)
    {
      if (heap_get_left_child (pos) >= heap->elem_cnt)
        break;
      if (heap_less (heap, heap_get_left_child (pos), pos))
        {
          heap_swap (heap, heap_get_left_child (pos), pos);
          pos = heap_get_left_child (pos);
          continue;
        }

      if (heap_get_right_child (pos) >= heap->elem_cnt)
        break;
      if (heap_less (heap, heap_get_right_child (pos), pos))
        {
          heap_swap (heap, heap_get_left_child (pos), pos);
          pos = heap_get_left_child (pos);
          continue;
        }

      break;
    }
}

bool
heap_insert (struct heap *heap, struct heap_elem *elem)
{
  ASSERT (heap != NULL);
  ASSERT (heap->magic == HEAP_MAGIC);
  ASSERT (elem != NULL);

  if (heap->elem_cnt >= heap->size)
    {
      // grow if needed
      size_t new_size = 2 * heap->size;
      if (new_size == 0)
        new_size = 4;
      void *new_data = realloc (heap->data, sizeof (*heap->data) * new_size);
      if (!new_data)
        return false;
      heap->size = new_size;
      heap->data = new_data;
    }

  size_t pos = heap->elem_cnt++;
  heap->data[pos] = elem;
  heap->data[pos]->index = pos;
  heap->data[pos]->magic = HEAP_ELEM_MAGIC;
  heap_fix_bottom_up (heap, pos);

  return true;
}

struct heap_elem *
heap_peek_min (struct heap *heap)
{
  ASSERT (heap != NULL);
  ASSERT (heap->magic == HEAP_MAGIC);
  
  if (heap->elem_cnt == 0)
    return 0;
  ASSERT (heap->data[0]->magic == HEAP_ELEM_MAGIC);
  return heap->data[0];
}

struct heap_elem *
heap_delete_min (struct heap *heap)
{
  ASSERT (heap != NULL);
  ASSERT (heap->magic == HEAP_MAGIC);

  if (heap->elem_cnt == 0)
    return NULL;
    
  struct heap_elem *result = heap->data[HEAP_ROOT];
  if (--heap->elem_cnt > 0)
    {
      heap->data[HEAP_ROOT] = heap->data[heap->elem_cnt];
      heap->data[HEAP_ROOT]->index = HEAP_ROOT;
      heap_fix_top_down (heap, HEAP_ROOT);
    }
  ASSERT (result->magic == HEAP_ELEM_MAGIC);
  result->magic ^= -1u;
  return result;
}

void
heap_delete (struct heap *heap, struct heap_elem *elem)
{
  ASSERT (heap != NULL);
  ASSERT (heap->magic == HEAP_MAGIC);
  ASSERT (elem != NULL);
  ASSERT (elem->magic == HEAP_ELEM_MAGIC);
  ASSERT (elem->index < heap->elem_cnt);

  --heap->elem_cnt;
  elem->magic ^= -1u;
  if (elem->index == heap->elem_cnt)
    return;

  unsigned pos = elem->index;
  heap->data[pos] = heap->data[heap->elem_cnt];
  heap->data[pos]->index = pos;
  heap_fix_bottom_up (heap, pos);
  heap_fix_top_down (heap, pos);
}

void
heap_changed_key (struct heap *heap, struct heap_elem *elem)
{
  ASSERT (heap != NULL);
  ASSERT (heap->magic == HEAP_MAGIC);
  ASSERT (elem != NULL);
  ASSERT (elem->magic == HEAP_ELEM_MAGIC);
  ASSERT (elem->index < heap->elem_cnt);
  
  unsigned pos = elem->index;
  heap_fix_bottom_up (heap, pos);
  heap_fix_top_down (heap, pos);
}
