#define SetupDiGetDeviceProperty __MINGW_NAME_AW(SetupDiGetDeviceProperty)

WINSETUPAPI WINBOOL WINAPI SetupDiGetDevicePropertyW(HDEVINFO DeviceInfoSet,PSP_DEVINFO_DATA DeviceInfoData,const DEVPROPKEY *PropertyKey,DEVPROPTYPE *PropertyType,PBYTE PropertyBuffer,DWORD PropertyBufferSize,PDWORD RequiredSize,DWORD Flags);

