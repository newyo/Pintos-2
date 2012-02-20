#include "devices/shutdown.h"
#include <console.h>
#include <stdio.h>
#include "devices/kbd.h"
#include "devices/serial.h"
#include "devices/timer.h"
#include "threads/io.h"
#include "threads/thread.h"
#include "threads/interrupt.h"
#include "userprog/exception.h"
#ifdef FILESYS
#include "devices/block.h"
#include "filesys/filesys.h"
#endif

/* Keyboard control register port. */
#define CONTROL_REG 0x64

static void print_stats (void);

/* Reboots the machine via the keyboard controller. */
void
shutdown_reboot (void)
{
  printf ("Rebooting...\n");
  
#ifdef FILESYS
  enum intr_level old_level = intr_enable ();
  filesys_done ();
  intr_set_level (old_level);
#endif

    /* See [kbd] for details on how to program the keyboard
     * controller. */
  for (;;)
    {
      int i;

      /* Poll keyboard controller's status byte until
       * 'input buffer empty' is reported. */
      for (i = 0; i < 0x10000; i++)
        {
          if ((inb (CONTROL_REG) & 0x02) == 0)
            break;
          timer_udelay (2);
        }

      timer_udelay (50);

      /* Pulse bit 0 of the output port P2 of the keyboard controller.
       * This will reset the CPU. */
      outb (CONTROL_REG, 0xfe);
      timer_udelay (50);
    }
}

static int shutdown_power_off_recursion = 0;

/* Powers down the machine we're running on,
   as long as we're running on Bochs or QEMU. */
void
shutdown_power_off (void)
{
  switch (shutdown_power_off_recursion ++)
    {
      case 0:
#ifdef FILESYS
        {
          enum intr_level old_level = intr_enable ();
          filesys_done ();
          intr_set_level (old_level);
        }
#endif
      case 1:
        print_stats ();
        printf ("Powering off...\n");
        serial_flush ();
        break;
        
      default:
        shutdown_power_off_recursion = 2;
    }

  for (;;)
    {
      /* This is a special power-off sequence supported by Bochs and
         QEMU, but not by physical hardware. */
      const char *p;
      for (p = "Shutdown"; *p; ++p)
        outb (0x8900, *p);

      /* This will power off a VMware VM if "gui.exitOnCLIHLT = TRUE"
         is set in its configuration file.  (The "pintos" script does
         that automatically.)  */
      asm volatile ("cli; hlt");
      printf ("still running...\n");
    }
}

/* Print statistics about Pintos execution. */
static void
print_stats (void)
{
  timer_print_stats ();
  thread_print_stats ();
#ifdef FILESYS
  block_print_stats ();
#endif
  console_print_stats ();
  kbd_print_stats ();
  exception_print_stats ();
}
