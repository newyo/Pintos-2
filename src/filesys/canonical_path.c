#include "canonical_path.h"
#include "threads/malloc.h"
#include <string.h>
#include <debug.h>

char *
canonical_path_get (const char *cwd, const char *rel_path)
{
  ASSERT (cwd != NULL);
  ASSERT (cwd[0] == '/');
  ASSERT (rel_path != NULL);
  
  if (rel_path[0] == '\0')
    {
      size_t len = strlen (cwd);
      char *result = malloc (len+1);
      if (result)
        {
          memcpy (result, cwd, len);
          result[len] = 0;
        }
      return result;
    }
  else if (rel_path[0] == '/')
    return canonical_path_get ("/", rel_path+1);
    
  // concat
  return NULL; // TODO
}
