/* x64 SEH scope table using the Windows C exception handler. */
.text
.globl fixture_seh
.def fixture_seh; .scl 2; .type 32; .endef
.seh_proc fixture_seh
fixture_seh:
    subq $40, %rsp
    .seh_stackalloc 40
    .seh_endprologue
.Ltry:
    movl $0xe0123456, %ecx
    xorl %edx, %edx
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    call *__imp_RaiseException(%rip)
    nop
.Lend:
    xorl %eax, %eax
.Lreturn:
    addq $40, %rsp
    ret
.Lcatch:
    movl $1, %eax
    jmp .Lreturn
    .seh_handler __C_specific_handler, @except
    .seh_handlerdata
    .long 1
    .rva .Ltry, .Lend
    .long 1
    .rva .Lcatch
.text
.seh_endproc
.section .drectve
.ascii " -export:fixture_seh"
