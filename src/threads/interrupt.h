#ifndef THREADS_INTERRUPT_H
#define THREADS_INTERRUPT_H

#include "threads/flags.h"

#include <stdbool.h>
#include <stdint.h>
#include <debug.h>

#define INTR_FUN static inline enum intr_level __attribute__ ((always_inline))

/* Interrupts on or off? */
enum intr_level 
  {
    INTR_OFF,             /* Interrupts disabled. */
    INTR_ON               /* Interrupts enabled. */
  };

bool __attribute__ ((pure)) intr_context (void);

/* Returns the current interrupt status. */
INTR_FUN __attribute__ ((pure)) intr_get_level (void)
{
  uint32_t flags;

  /* Push the flags register on the processor stack, then pop the
     value off the stack into `flags'.  See [IA32-v2b] "PUSHF"
     and "POP" and [IA32-v3a] 5.8.1 "Masking Maskable Hardware
     Interrupts". */
  asm volatile ("pushfl; popl %0" : "=g" (flags));

  return flags & FLAG_IF ? INTR_ON : INTR_OFF;
}

/* Enables interrupts and returns the previous interrupt status. */
INTR_FUN intr_enable (void)
{
  enum intr_level old_level = intr_get_level ();

  /* Enable interrupts by setting the interrupt flag.

     See [IA32-v2b] "STI" and [IA32-v3a] 5.8.1 "Masking Maskable
     Hardware Interrupts". */
  if (old_level != INTR_ON)
    {
      ASSERT (!intr_context ());
      asm volatile ("sti");
    }

  return old_level;
}

/* Disables interrupts and returns the previous interrupt status. */
INTR_FUN intr_disable (void)
{
  enum intr_level old_level = intr_get_level ();

  /* Disable interrupts by clearing the interrupt flag.
     See [IA32-v2b] "CLI" and [IA32-v3a] 5.8.1 "Masking Maskable
     Hardware Interrupts". */
  if (old_level != INTR_OFF)
    asm volatile ("cli");

  return old_level;
}

/* Enables or disables interrupts as specified by LEVEL and
   returns the previous interrupt status. */
INTR_FUN intr_set_level (enum intr_level level)
{
  return level == INTR_ON ? intr_enable () : intr_disable ();
}

/* Interrupt stack frame. */
struct intr_frame
  {
    /* Pushed by intr_entry in intr-stubs.S.
       These are the interrupted task's saved registers. */
    uint32_t edi;               /* Saved EDI. */
    uint32_t esi;               /* Saved ESI. */
    uint32_t ebp;               /* Saved EBP. */
    uint32_t esp_dummy;         /* Not used. */
    uint32_t ebx;               /* Saved EBX. */
    uint32_t edx;               /* Saved EDX. */
    uint32_t ecx;               /* Saved ECX. */
    uint32_t eax;               /* Saved EAX. */
    uint16_t gs, :16;           /* Saved GS segment register. */
    uint16_t fs, :16;           /* Saved FS segment register. */
    uint16_t es, :16;           /* Saved ES segment register. */
    uint16_t ds, :16;           /* Saved DS segment register. */

    /* Pushed by intrNN_stub in intr-stubs.S. */
    uint32_t vec_no;            /* Interrupt vector number. */

    /* Sometimes pushed by the CPU,
       otherwise for consistency pushed as 0 by intrNN_stub.
       The CPU puts it just under `eip', but we move it here. */
    uint32_t error_code;        /* Error code. */

    /* Pushed by intrNN_stub in intr-stubs.S.
       This frame pointer eases interpretation of backtraces. */
    void *frame_pointer;        /* Saved EBP (frame pointer). */

    /* Pushed by the CPU.
       These are the interrupted task's saved registers. */
    void (*eip) (void);         /* Next instruction to execute. */
    uint16_t cs, :16;           /* Code segment for eip. */
    uint32_t eflags;            /* Saved CPU flags. */
    void *esp;                  /* Saved stack pointer. */
    uint16_t ss, :16;           /* Data segment for esp. */
  };

typedef void intr_handler_func (struct intr_frame *);

void intr_init (void);
void intr_register_ext (uint8_t vec, intr_handler_func *, const char *name);
void intr_register_int (uint8_t vec, int dpl, enum intr_level,
                        intr_handler_func *, const char *name);
void intr_yield_on_return (void);

void intr_dump_frame (const struct intr_frame *);
const char *intr_name (uint8_t vec);

#endif /* threads/interrupt.h */
