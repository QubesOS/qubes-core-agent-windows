#include "assembly.h"

// _chkstk routine
// This routine is windows specific
// http://msdn.microsoft.com/en-us/library/ms648426.aspx

#ifdef __i386__

.text
.balign 4
DEFINE_COMPILERRT_FUNCTION(__chkstk_ms)
        push   %ecx
        push   %eax
        cmp    $0x1000,%eax
        lea    12(%esp),%ecx
        jb     1f
2:
        sub    $0x1000,%ecx
        orl    $0,(%ecx)
        sub    $0x1000,%eax
        cmp    $0x1000,%eax
        ja     2b
1:
        sub    %eax,%ecx
        orl    $0,(%ecx)
        pop    %eax
        pop    %ecx
        ret
END_COMPILERRT_FUNCTION(__chkstk_ms)

#endif // __i386__

#ifdef __x86_64__

.text
.balign 4
DEFINE_COMPILERRT_FUNCTION(___chkstk_ms)
        push   %rcx
        push   %rax
        cmp    $0x1000,%rax
        lea    24(%rsp),%rcx
        jb     1f
2:
        sub    $0x1000,%rcx
        orl    $0,(%rcx)
        sub    $0x1000,%rax
        cmp    $0x1000,%rax
        ja     2b
1:
        sub    %rax,%rcx
        orl    $0,(%rcx)
        pop    %rax
        pop    %rcx
        ret
END_COMPILERRT_FUNCTION(___chkstk_ms)

#endif // __x86_64__
