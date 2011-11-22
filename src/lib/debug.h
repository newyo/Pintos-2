#ifndef __LIB_DEBUG_H
#define __LIB_DEBUG_H

/* GCC lets us add "attributes" to functions, function
   parameters, etc. to indicate their properties.
   See the GCC manual for details. */
#define UNUSED __attribute__ ((unused))
#define NO_RETURN __attribute__ ((noreturn))
#define NO_INLINE __attribute__ ((noinline))
#define PRINTF_FORMAT(FMT, FIRST) __attribute__ ((format (printf, FMT, FIRST)))

/* Halts the OS, printing the source file name, line number, and
   function name, plus a user-specific message. */
#define PANIC(...) debug_panic (__FILE__, __LINE__, __func__, __VA_ARGS__)

void debug_panic (const char *file, int line, const char *function,
                  const char *message, ...) PRINTF_FORMAT (4, 5) NO_RETURN;
void debug_backtrace (void);
void debug_backtrace_all (void);
 
#endif // ifndef __LIB_DEBUG_H



/* This is outside the header guard so that debug.h may be
   included multiple times with different settings of NDEBUG. */
#undef ASSERT
#undef NOT_REACHED

#ifndef NDEBUG
# define ASSERT(CONDITION)                                      \
        if (CONDITION) { } else {                               \
                PANIC ("assertion `%s' failed.", #CONDITION);   \
        }
# define NOT_REACHED() PANIC ("executed an unreachable statement");
# define UNSAFE_PRINTF(...) \
  ({ \
    enum intr_level _old_level = intr_disable (); \
    struct thread *_current_thread = running_thread (); \
    enum thread_status _old_status =  _current_thread->status; \
    _current_thread->status = THREAD_RUNNING; \
    printf (__VA_ARGS__); \
    _current_thread->status = _old_status; \
    intr_set_level (_old_level); \
    (void)0; \
  })
#else
# define ASSERT(CONDITION) ((void) 0)
# define NOT_REACHED() for (;;)
# define UNSAFE_PRINTF(...) ((void) 0)
#endif // ifndef NDEBUG
