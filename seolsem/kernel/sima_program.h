#ifndef __SIMA_PROGRAM__
#define __SIMA_PROGRAM__

#include "sima_type.h"

BOOL program_load(const char *name);
BOOL program_load_in_dir(const char *dir_name, const char *name);
BOOL program_run(void);
void program_unload(void);

#endif
