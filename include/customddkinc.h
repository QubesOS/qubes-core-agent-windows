#include <devpropdef.h>
#include <strsafe.h>
#include <sal.h>

#define __field_bcount_part(size,init) SAL__notnull SAL__byte_writableTo(size) SAL__byte_readableTo(init)

DEFINE_DEVPROPKEY(DEVPKEY_Device_HardwareIds,            0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 3);     // DEVPROP_TYPE_STRING_LIST
DEFINE_DEVPROPKEY(DEVPKEY_Device_Class,                  0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 9);     // DEVPROP_TYPE_STRING
DEFINE_DEVPROPKEY(DEVPKEY_Device_LocationInfo,           0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 15);    // DEVPROP_TYPE_STRING
#define IOCTL_DISK_SET_DISK_ATTRIBUTES      CTL_CODE(IOCTL_DISK_BASE, 0x003d, METHOD_BUFFERED,     FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define DISK_ATTRIBUTE_OFFLINE              0x0000000000000001
#define DISK_ATTRIBUTE_READ_ONLY            0x0000000000000002

//
// IOCTL_DISK_SET_DISK_ATTRIBUTES
//
// Input Buffer:
//     Structure of type SET_DISK_ATTRIBUTES
//
// Output Buffer:
//     None
//

typedef struct _SET_DISK_ATTRIBUTES {

    //
    // Specifies the size of the
    // structure for versioning.
    //
    ULONG Version;

    //
    // Indicates whether to remember
    // these settings across reboots
    // or not.
    //
    BOOLEAN Persist;

    //
    // Indicates whether the ownership
    // taken earlier is being released.
    //
    BOOLEAN RelinquishOwnership;

    //
    // For alignment purposes.
    //
    BOOLEAN Reserved1[2];

    //
    // Specifies the new attributes.
    //
    ULONGLONG Attributes;

    //
    // Specifies the attributes
    // that are being modified.
    //
    ULONGLONG AttributesMask;

    //
    // Specifies an identifier to be
    // associated  with  the caller.
    // This setting is not persisted
    // across reboots.
    //
    GUID Owner;

} SET_DISK_ATTRIBUTES, *PSET_DISK_ATTRIBUTES;


