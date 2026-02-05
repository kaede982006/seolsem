#ifndef __SIMA_ENV__
#define __SIMA_ENV__

#include "sima_type.h"

BOOL env_init(void);
const char *env_get(const char *name);
BOOL env_set_public(const char *name, const char *value);

#endif
