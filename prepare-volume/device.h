#pragma once
#include "prepare-volume.h"

// Returns physical drive number that represents private.img.
BOOL GetPrivateImgDriveNumber(IN ULONG xenVbdId, OUT PULONG driveNumber);
