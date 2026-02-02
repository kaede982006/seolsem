#ifndef __SIMA_IDE__
#define __SIMA_IDE__

#include "sima_type.h"

BOOL ide_read_sector(UINT32 lba, UINT8 *buffer);
BOOL ide_write_sector(UINT32 lba, const UINT8 *buffer);

#endif
