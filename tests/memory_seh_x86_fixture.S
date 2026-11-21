/* x86 SEH chain test: an access violation resumes at the catch label. */
.text
.globl _fixture_seh
.def _fixture_seh; .scl 2; .type 32; .endef
_fixture_seh:
    pushl %ebp
    movl %esp, %ebp
    pushl $.Lcaught32
    pushl $_fixture_seh_handler32
    pushl %fs:0
    movl %esp, %fs:0
    xorl %ecx, %ecx
    movl (%ecx), %eax
    xorl %eax, %eax
    jmp .Lreturn32
.Lcaught32:
    movl $1, %eax
.Lreturn32:
    movl (%esp), %ecx
    movl %ecx, %fs:0
    addl $12, %esp
    leave
    ret
.globl _fixture_seh_handler32
_fixture_seh_handler32:
    movl 4(%esp), %eax
    cmpl $0xc0000005, (%eax)
    jne .Lsearch32
    testl $6, 4(%eax)
    jne .Lsearch32
    movl 8(%esp), %ecx
    movl 12(%esp), %edx
    movl 8(%ecx), %eax
    movl %eax, 184(%edx)
    movl %ecx, 196(%edx)
    xorl %eax, %eax
    ret
.Lsearch32:
    movl $1, %eax
    ret
.section .drectve
.ascii " -export:fixture_seh"
.section .rdata,"dr"
.globl _fixture_seh_table
_fixture_seh_table:
    .rva _fixture_seh_handler32
