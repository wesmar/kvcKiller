; ==============================================================================
; From KvcProject: Security Trace Session Audit & Dispatcher Infrastructure
; Dynamic reconstitution unit for kernel audit logging and dispatcher channels.
; Architecture: x64 | Zero-CRT | Fast register transformation pipeline
; ==============================================================================

.data
ALIGN 8

_EtwTraceSessionMask    dw 08A86h, 0CA99h, 07A87h, 08A86h, 00A87h, 00A84h, 06A84h, 0BA84h, 05A9Ah

_EtwDeviceMask          dw 09A87h, 09A87h, 0BA98h, 09A87h, 01A86h, 04A85h, 00A85h, 08A86h
                        dw 0CA99h, 07A87h, 08A86h, 00A87h, 00A84h, 0BA84h, 01A86h, 07A85h
                        dw 03A85h, 05A9Ah

_EtwSessionSeed         dw 048D6h
_EtwQuantumDelta        dw 024C1h

_EtwStateSession        db 0
_EtwStateDevice         db 0

ALIGN 8
_EtwDecodedBuffer       dw 9 dup(0)
ALIGN 8
_EtwDeviceBuffer        dw 18 dup(0)

.code
ALIGN 16

_EtwDecodeBuffer PROC
    push rdi
    push rsi
    push rbx
    push r12
    mov rsi, rdx
    mov rdi, r8
    mov r12, rcx
    mov r9w, _EtwSessionSeed
etw_xor_loop:
    mov ax, [rsi]
    xor ax, r9w
    mov [rdi], ax
    add rsi, 2
    add rdi, 2
    loop etw_xor_loop

    mov rsi, r8
    mov rdi, r8
    mov rcx, r12
etw_rol_loop:
    mov ax, [rsi]
    rol ax, 4
    mov [rdi], ax
    add rsi, 2
    add rdi, 2
    loop etw_rol_loop

    mov rsi, r8
    mov rdi, r8
    mov rcx, r12
    mov r9w, _EtwQuantumDelta
etw_sub_loop:
    mov ax, [rsi]
    sub ax, r9w
    mov [rdi], ax
    add rsi, 2
    add rdi, 2
    loop etw_sub_loop

    pop r12
    pop rbx
    pop rsi
    pop rdi
    ret
_EtwDecodeBuffer ENDP

PUBLIC EtwGetKernelTraceSessionString
EtwGetKernelTraceSessionString PROC
    sub rsp, 28h
    cmp _EtwStateSession, 2
    je s_return
    cmp _EtwStateSession, 1
    je s_wait
    mov _EtwStateSession, 1
    mov rcx, 9
    lea rdx, _EtwTraceSessionMask
    lea r8, _EtwDecodedBuffer
    call _EtwDecodeBuffer
    mov _EtwStateSession, 2
    jmp s_return
s_wait:
    pause
    cmp _EtwStateSession, 2
    jne s_wait
s_return:
    lea rax, _EtwDecodedBuffer
    add rsp, 28h
    ret
EtwGetKernelTraceSessionString ENDP

PUBLIC EtwGetKernelTraceDeviceString
EtwGetKernelTraceDeviceString PROC
    sub rsp, 28h
    cmp _EtwStateDevice, 2
    je d_return
    cmp _EtwStateDevice, 1
    je d_wait
    mov _EtwStateDevice, 1
    mov rcx, 18
    lea rdx, _EtwDeviceMask
    lea r8, _EtwDeviceBuffer
    call _EtwDecodeBuffer
    mov _EtwStateDevice, 2
    jmp d_return
d_wait:
    pause
    cmp _EtwStateDevice, 2
    jne d_wait
d_return:
    lea rax, _EtwDeviceBuffer
    add rsp, 28h
    ret
EtwGetKernelTraceDeviceString ENDP

END
