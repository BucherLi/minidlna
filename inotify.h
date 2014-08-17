
#include "metadata.h"
#ifdef HAVE_INOTIFY
int
inotify_remove_file(const char * path);
#ifdef NAS
int
nas_inotify_remove_file(const char * path, const char *name, NAS_DIR dir);
void
scan_add_dir(path);
#endif
void *
start_inotify();
#endif
