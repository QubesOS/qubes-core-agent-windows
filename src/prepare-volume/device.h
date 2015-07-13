#pragma once
#include "prepare-volume.h"

// Returns physical drive number that represents private.img.
BOOL GetPrivateImgDriveNumber(IN ULONG backendId, OUT ULONG *driveNumber);
