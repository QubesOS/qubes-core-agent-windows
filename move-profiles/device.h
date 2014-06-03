#pragma once
#include "move-profiles.h"

// Returns physical drive number that represents private.img.
BOOL GetPrivateImgDriveNumber(IN ULONG xenVbdId, OUT PULONG driveNumber);
