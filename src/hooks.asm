; SBParry 注入到游戏进程里的代码块（ml64 汇编）。
;
; 这些块不会在本程序里执行：game.cpp 把每个 [xxx_begin, xxx_end) 的字节原样拷进游戏里远程分配的 cave，
; 再把下面的占位常量替换成真实地址。块内只用 RIP 相对寻址访问块内数据，拷到哪里都能跑。
;
; 事件日志（环形缓冲区，cave+100h）：
;   +0   dword  写入序号（lock xadd 自增，取低 8 位做槽位）
;   +10h 256 个 LogEntry（30h 字节，见 game.h）
;       +00 tsc  +08 inst  +10 remain  +14 total  +18 kind  +1C seq  +20 row  +28 attacker
;   seq：取槽时先写 ~序号，其余字段写完最后翻成序号。读的一方看到 seq == 序号才读这一条

MAGIC_LOG   equ 5342504C4F474731h   ; 替换为事件日志地址
MAGIC_CTRL  equ 5342504354524C31h   ; 替换为 PadCtrl 地址

; PadCtrl 偏移（与 game.h 一致）
XI_ACTIVE   equ 0
XI_BUTTONS  equ 4
XI_STICK_XY equ 6       ; LX, LY 两个 int16
XI_STICK    equ 10
XI_LAST     equ 12
XI_CALLS    equ 20
PS_ACTIVE   equ 28
PS_BUTTONS  equ 32
PS_STICK_XY equ 36      ; LX, LY 两个 uint8
PS_STICK    equ 38
PS_LAST     equ 40
PS_CALLS    equ 48

; 取一个日志槽：r10 = 条目地址，标记为“未写完”。破坏 eax / edx / 标志位
ALLOC_ENTRY macro
    mov     r10, MAGIC_LOG
    mov     eax, 1
    lock xadd dword ptr [r10], eax
    mov     edx, eax
    not     edx
    and     eax, 0FFh
    imul    eax, eax, 30h
    lea     r10, [r10 + rax + 10h]
    mov     dword ptr [r10 + 1Ch], edx
endm

; 写时间戳，再把 seq 翻成序号（最后写，x86 的写入按顺序可见）。破坏 rax / rdx
STAMP_ENTRY macro
    rdtsc
    shl     rdx, 20h
    or      rax, rdx
    mov     qword ptr [r10], rax
    not     dword ptr [r10 + 1Ch]
endm

.code

; ---------------------------------------------------------------- 判定
; 钩在 IsJustActionActive(inst=rcx, attacker=rdx) 入口，第一条指令 mov [rsp+8],rbx 被覆盖。
; 攻击判定框接触伊芙期间每帧调用一次，游戏在最后一次调用时结算：此时 inst+B8 > 0 即完美。
PUBLIC sbp_judge_begin, sbp_judge_end
sbp_judge_begin:
    push    rax
    push    rdx
    push    r10
    ALLOC_ENTRY
    mov     qword ptr [r10 + 08h], rcx              ; inst
    mov     rax, qword ptr [rsp + 08h]              ; 入口时的 rdx = attacker
    mov     qword ptr [r10 + 28h], rax
    mov     eax, dword ptr [rcx + 0B8h]             ; 完美窗口剩余
    mov     dword ptr [r10 + 10h], eax
    mov     eax, dword ptr [rcx + 0BCh]             ; 完美窗口总长
    mov     dword ptr [r10 + 14h], eax
    mov     dword ptr [r10 + 18h], 1
    mov     rax, qword ptr [rcx + 58h]              ; 技能表行
    mov     qword ptr [r10 + 20h], rax
    STAMP_ENTRY
    pop     r10
    pop     rdx
    pop     rax
    mov     qword ptr [rsp + 8], rbx                ; 被覆盖的原指令
    jmp     qword ptr [judge_ret]
judge_ret dq 0                                      ; 回跳地址（钩子点 + 5）
sbp_judge_end:

; ---------------------------------------------------------------- 按下
; 钩在按下格挡/闪避后设置完美窗口处：movss [r14+BC],xmm0（被覆盖，后面是 test edi,edi 会重设标志位）
PUBLIC sbp_press_begin, sbp_press_end
sbp_press_begin:
    movss   dword ptr [r14 + 0BCh], xmm0            ; 被覆盖的原指令
    push    rax
    push    rdx
    push    r10
    ALLOC_ENTRY
    mov     qword ptr [r10 + 08h], r14              ; inst
    mov     qword ptr [r10 + 28h], 0
    mov     eax, dword ptr [r14 + 0B8h]
    mov     dword ptr [r10 + 10h], eax
    mov     eax, dword ptr [r14 + 0BCh]
    mov     dword ptr [r10 + 14h], eax
    mov     dword ptr [r10 + 18h], 2
    mov     rax, qword ptr [r14 + 58h]
    mov     qword ptr [r10 + 20h], rax
    STAMP_ENTRY
    pop     r10
    pop     rdx
    pop     rax
    jmp     qword ptr [press_ret]
press_ret dq 0                                      ; 钩子点 + 9
sbp_press_end:

; ---------------------------------------------------------------- 步骤切换
; 钩在 mov [r13+B4],ecx（写新步骤时长）。后面紧跟 je，要保存标志位。
; inst=r13，remain=+B0（本步已过时间），total=新步骤时长，row=+60（步骤表行），attacker=+68（技能组件）
PUBLIC sbp_step_begin, sbp_step_end
sbp_step_begin:
    mov     dword ptr [r13 + 0B4h], ecx             ; 被覆盖的原指令
    pushfq
    push    rax
    push    rdx
    push    r10
    ALLOC_ENTRY
    mov     qword ptr [r10 + 08h], r13
    mov     rax, qword ptr [r13 + 68h]
    mov     qword ptr [r10 + 28h], rax
    mov     eax, dword ptr [r13 + 0B0h]
    mov     dword ptr [r10 + 10h], eax
    mov     dword ptr [r10 + 14h], ecx
    mov     dword ptr [r10 + 18h], 3
    mov     rax, qword ptr [r13 + 60h]
    mov     qword ptr [r10 + 20h], rax
    STAMP_ENTRY
    pop     r10
    pop     rdx
    pop     rax
    popfq
    jmp     qword ptr [step_ret]
step_ret dq 0                                       ; 钩子点 + 7
sbp_step_end:

; ---------------------------------------------------------------- XInputGetState
; 游戏导入表里 XINPUT1_3!XInputGetState 的槽位指向这里：先调原函数，再把 PadCtrl 里的模拟按键叠加上去。
; DWORD XInputGetState(DWORD idx, XINPUT_STATE* s)
;   XINPUT_STATE: +0 dwPacketNumber, +4 wButtons, +6 LT, +7 RT, +8 sThumbLX, +0A sThumbLY
PUBLIC sbp_xinput_begin, sbp_xinput_end
sbp_xinput_begin:
    push    rbx
    sub     rsp, 20h
    mov     rbx, rdx
    call    qword ptr [xi_orig]
    test    eax, eax
    jnz     xi_done                                 ; 手柄未连接
    mov     r10, MAGIC_CTRL
    inc     qword ptr [r10 + XI_CALLS]
    ; 玩家自己在操作手柄（有按键或摇杆推出死区）：记下时间，自动操作据此选择输入设备
    movzx   ecx, word ptr [rbx + 4]
    test    ecx, ecx
    jnz     xi_real
    movsx   ecx, word ptr [rbx + 8]
    add     ecx, 12000
    cmp     ecx, 24000
    ja      xi_real
    movsx   ecx, word ptr [rbx + 0Ah]
    add     ecx, 12000
    cmp     ecx, 24000
    jbe     xi_inject
xi_real:
    rdtsc
    shl     rdx, 20h
    or      rax, rdx
    mov     qword ptr [r10 + XI_LAST], rax
xi_inject:
    cmp     dword ptr [r10 + XI_ACTIVE], 0
    je      xi_ok
    movzx   ecx, word ptr [r10 + XI_BUTTONS]
    or      word ptr [rbx + 4], cx
    cmp     word ptr [r10 + XI_STICK], 0
    je      xi_bump
    mov     ecx, dword ptr [r10 + XI_STICK_XY]
    mov     dword ptr [rbx + 8], ecx
xi_bump:
    inc     dword ptr [rbx]                         ; 状态变了，包序号也变
xi_ok:
    xor     eax, eax
xi_done:
    add     rsp, 20h
    pop     rbx
    ret
    align 8
xi_orig dq 0                                        ; 原 XInputGetState
sbp_xinput_end:

; ---------------------------------------------------------------- scePadReadState（DualSense 原生）
; int scePadReadState(int handle, ScePadData* d)：+0 buttons(u32)  +4 左摇杆 x  +5 左摇杆 y（0~255，128 居中，y 向上为 0）
PUBLIC sbp_scepad_begin, sbp_scepad_end
sbp_scepad_begin:
    push    rbx
    sub     rsp, 20h
    mov     rbx, rdx
    call    qword ptr [ps_orig]
    test    eax, eax
    jnz     ps_done
    mov     r10, MAGIC_CTRL
    inc     qword ptr [r10 + PS_CALLS]
    mov     ecx, dword ptr [rbx]
    and     ecx, 0FFFFh                             ; 只看普通按键
    jnz     ps_real
    movzx   ecx, byte ptr [rbx + 4]
    sub     ecx, 128 - 40
    cmp     ecx, 80
    ja      ps_real
    movzx   ecx, byte ptr [rbx + 5]
    sub     ecx, 128 - 40
    cmp     ecx, 80
    jbe     ps_inject
ps_real:
    rdtsc
    shl     rdx, 20h
    or      rax, rdx
    mov     qword ptr [r10 + PS_LAST], rax
ps_inject:
    cmp     dword ptr [r10 + PS_ACTIVE], 0
    je      ps_ok
    mov     ecx, dword ptr [r10 + PS_BUTTONS]
    or      dword ptr [rbx], ecx
    cmp     word ptr [r10 + PS_STICK], 0
    je      ps_ok
    movzx   ecx, word ptr [r10 + PS_STICK_XY]
    mov     word ptr [rbx + 4], cx
ps_ok:
    xor     eax, eax
ps_done:
    add     rsp, 20h
    pop     rbx
    ret
    align 8
ps_orig dq 0                                        ; 原 scePadReadState
sbp_scepad_end:

END
