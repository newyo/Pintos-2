#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>
#include "filesys/off_t.h"

extern struct pifs_device fs_pifs;

void filesys_init (bool format);
void filesys_done (void);

bool filesys_create (const char *name, off_t initial_size);
bool filesys_create_folder (const char *name);
struct file *filesys_open (const char *name);
bool filesys_remove (const char *name);

#endif /* filesys/filesys.h */
