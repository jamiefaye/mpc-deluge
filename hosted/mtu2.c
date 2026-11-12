// The MTU2 timer block, as ordinary memory. The patched mtu2_iodefine.h points the firmware's
// MTU2 at this instead of at the hardware address. See hosted.h. SPDX: GPL-3.0-or-later
#include <stdint.h>

#include "RZA1/system/iodefines/mtu2_iodefine.h"

struct st_mtu2 deluge_hosted_mtu2;
