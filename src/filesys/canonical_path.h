#ifndef __CANONICAL_PATH_H
#define __CANONICAL_PATH_H

// returns a canonical path, caller cleans up!
char *canonical_path_get (const char *cwd, const char *rel_path);

#endif
