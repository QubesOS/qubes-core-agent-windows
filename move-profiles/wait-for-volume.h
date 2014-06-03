#pragma once
#include "move-profiles.h"

// Waits until a new volume is recognized by the system and automounted.
// Returns volume's disk letter or 0 if failed.
WCHAR WaitForVolumeArrival();
