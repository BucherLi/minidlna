
#include "metadata.h"
#ifdef HAVE_INOTIFY
int
inotify_remove_file(const char * path);
#ifdef NAS
int
nas_inotify_remove_file(const char * path, const char *name, NAS_DIR dir);
#endif
void *
start_inotify();
#endif
