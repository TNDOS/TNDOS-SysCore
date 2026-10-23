/* ============================================================================
 * TNDDOS -- CPU 异常处理
 *
 * 在这之前，任何一个异常都会让系统**静默僵死**：没有 IDT 属于自己，异常交给固件的
 * 处理器，而它在我们这个环境里什么也不说。代价是极难查 —— NVMe 那个 #GP
 * 花了十几轮，最后是靠串口里固件顺手打的一行 RIP 才看出「CPU 跳进了字符串」。
 *
 * 这一版只为**看见**：#GP / #PF / #UD / #DE 四个向量，打印向量号、出错码、
 * RIP、CS、RFLAGS、CR2，然后停机。不试图恢复 —— 恢复需要的东西现在都没有。
 *
 * **低风险的装法**：先用 sidt 读出固件现有的 IDT，整张复制过来，只替换这四个入口。
 * 其余 252 个向量仍然归固件处理，所以不会把「本来能看到的错误」变成三连故障。
 *
 * 内联汇编：这里能用。当初失败的是 __inbyte 那个 **MS 内建函数**（链接期找不到符号），
 * 不是内联汇编本身 —— 那个结论当时概括过头了。
 * ==========================================================================*/
#include "tnd.h"

typedef struct {
    UINT16 off0;
    UINT16 sel;
    UINT8  ist;
    UINT8  type;       /* 0x8E = present, DPL=0, 64 位中断门 */
    UINT16 off1;
    UINT32 off2;
    UINT32 zero;
} __attribute__((packed)) TND_IDT_ENTRY;

typedef struct {
    UINT16 limit;
    UINT64 base;
} __attribute__((packed)) TND_IDTR;

static TND_IDT_ENTRY gIdt[256];
static TND_IDTR      gIdtr;
static UINT16        gCs = 0;

/* **CR2 必须在汇编桩的第一条指令就存到这里。**
 *
 * 如果在 C 里读，而处理函数自己又踩了坏地址，CR2 会被后一次覆盖 ——
 * 记下来的就不是最初那次故障了。RIP 相对寻址的全局存储不碰栈，
 * 所以可以在第一条就干。 */
volatile UINT64 gTrapCr2 = 0;

/* ------------------------------------------------------------------ 桩
 * **必须是 naked**：不然编译器会在入口插 prologue，rsp 就不是 CPU 压好的那个了。
 *
 * 三件事，顺序不能换：
 *
 *   1  **先存 CR2**。在 C 里读的话，万一处理函数自己又踩了坏地址，CR2 会被
 *      后一次覆盖 —— 记下来的就不是最初那次故障了。
 *   2  **把 15 个通用寄存器全压栈**。只留 RIP/CR2 是不够的：像 movdqa 那种
 *      对齐异常，光看 RIP 根本不知道地址是怎么算出来的，必须看寄存器。
 *   3  **用 call 不用 jmp**。MS ABI 要求调用方预留 32 字节 shadow space 且入口
 *      16 字节对齐，硬件异常入口这两条都不保证；jmp 进去会让被调函数的 spill
 *      踩到异常帧。
 *
 * 参数：rcx=向量号  rdx=硬件帧指针  r8=寄存器区指针。
 *
 * 同特权级的 #PF **不压 RSP/SS**（那是跨特权级才有的），所以硬件帧只有四项：
 *   [0]=errcode  [1]=RIP  [2]=CS  [3]=RFLAGS
 * ==========================================================================*/
/* **%rax 不在这里压 —— 它必须在那条 movq %cr2,%rax 之前就压好。**
 *
 * 桩的第一条要读 CR2，而读它就得用一个寄存器，最自然是 rax。
 * 如果先读再压，压进去的是 CR2 而不是真正的 RAX —— 于是 exc.rax 永远是
 * CR2 的值（#UD 的 CR2 是 0，所以看上去永远是 0）。
 * udtest 自测抓到的：设 rax=1111 再 ud2，trap 报 0。 */
#define TRAP_PUSH_GPRS_AFTER_RAX                              \
        "pushq %rbx\n"                                        \
        "pushq %rcx\n"                                        \
        "pushq %rdx\n"                                        \
        "pushq %rsi\n"                                        \
        "pushq %rdi\n"                                        \
        "pushq %rbp\n"                                        \
        "pushq %r8\n"                                         \
        "pushq %r9\n"                                         \
        "pushq %r10\n"                                        \
        "pushq %r11\n"                                        \
        "pushq %r12\n"                                        \
        "pushq %r13\n"                                        \
        "pushq %r14\n"                                        \
        "pushq %r15\n"

/* 有出错码的：栈上已经是 [errcode][RIP][CS][RFLAGS]，15 次 push 之后帧在 +120 */
#define TRAP_STUB_ERR(name, vec)                              \
    __attribute__((naked)) void name(void) {                  \
        __asm__ __volatile__("pushq %rax\n"                   \
                             "movq %cr2, %rax\n"            \
                             "movq %rax, gTrapCr2(%rip)\n"   \
                             TRAP_PUSH_GPRS_AFTER_RAX                   \
                             "movq %rsp, %r8\n"              \
                             "leaq 120(%rsp), %rdx\n"        \
                             "movl $" #vec ", %ecx\n"       \
                             "andq $-16, %rsp\n"             \
                             "subq $0x28, %rsp\n"            \
                             "call trap_common\n"            \
                             "hlt\n");                        \
    }

/* 不压出错码的：先补一个 0，再压 15 个寄存器。
 *
 * **帧指针还是 +120，不是 +128。**
 *
 * 算一遍：异常发生时 rsp = R，[R]=RIP [R+8]=CS [R+16]=RFLAGS。
 *   pushq $0        -> 0 落在 R-8，正是 frame[0] 该在的位置
 *   pushq %rax      -> R-16
 *   再 14 次 push   -> R-128
 *   所以 frame = R-8 = rsp + 120，和 ERR 桩一致
 *
 * **$0 必须先压。** 试过先压 %rax 再压 $0：rax 占了 frame[0] 的位置，
 * 于是帧整体上移 8 字节，errcode 报成 rax。
 *
 * 我原来写成 +128（多算了那 8 字节两次），结果 frame 指到了 RIP 上，
 * 于是 errcode 报成 RIP、RIP 报成 CS、CS 报成 RFLAGS —— 整体错位一格。
 * 用 udtest 自测抓到的：故意设 rax=1111 rbx=2222 … 再 ud2，一眼看出错位。 */
#define TRAP_STUB_NOERR(name, vec)                            \
    __attribute__((naked)) void name(void) {                  \
        __asm__ __volatile__("pushq $0\n"                     \
                             "pushq %rax\n"                   \
                             "movq %cr2, %rax\n"            \
                             "movq %rax, gTrapCr2(%rip)\n"   \
                             TRAP_PUSH_GPRS_AFTER_RAX                   \
                             "movq %rsp, %r8\n"              \
                             "leaq 120(%rsp), %rdx\n"        \
                             "movl $" #vec ", %ecx\n"       \
                             "andq $-16, %rsp\n"             \
                             "subq $0x28, %rsp\n"            \
                             "call trap_common\n"            \
                             "hlt\n");                        \
    }

/* **定时器中断（向量 0x20）。**
 *
 * 为什么必须自己接：trap_init 把固件的 IDT 整张抄了过来，只替换了
 * 0/6/13/14 四个向量。0x20 仍然指向固件的处理程序 —— 而 ExitBootServices
 * 之后固件已经把那块内存交还给我们了。结果就是定时器每打断一次，
 * 就执行一遍那块地址上的东西，被中断的指令流寄存器被踩。
 *
 * 症状（-d int 的日志可以证实）：
 *   - 被中断的指令流里 %rax 变成 29 / 31 这种该循环里根本不可能的值
 *   - 控制台偶尔丢一个字符
 *   - RIP 有时落在一条指令的中间
 *
 * 这里自己接住：保存全部通用寄存器，给 PIC 和 LAPIC 都发 EOI
 *（不知道这台机器用哪一套，两个都发是无害的），恢复，iretq。
 * **不能只发 PIC** —— 实测屏蔽 8259 之后中断照样来，说明走的是 LAPIC。 */
__attribute__((naked)) static void trap_irq0(void) {
    __asm__ __volatile__(
        "pushq %rax\n"  "pushq %rbx\n"  "pushq %rcx\n"  "pushq %rdx\n"
        "pushq %rsi\n"  "pushq %rdi\n"  "pushq %rbp\n"  "pushq %r8\n"
        "pushq %r9\n"   "pushq %r10\n"  "pushq %r11\n"  "pushq %r12\n"
        "pushq %r13\n"  "pushq %r14\n"  "pushq %r15\n"
        /* PIC EOI（主片） */
        "movb $0x20, %al\n"
        "outb %al, $0x20\n"
        /* LAPIC EOI（默认基址 0xFEE00000 + 0xB0）*/
        "movq $0xFEE000B0, %rbx\n"
        "movl $0, (%rbx)\n"
        "popq %r15\n"  "popq %r14\n"  "popq %r13\n"  "popq %r12\n"
        "popq %r11\n"  "popq %r10\n"  "popq %r9\n"   "popq %r8\n"
        "popq %rbp\n"  "popq %rdi\n"  "popq %rsi\n"  "popq %rdx\n"
        "popq %rcx\n"  "popq %rbx\n"  "popq %rax\n"
        "iretq\n");
}

TRAP_STUB_NOERR(trap_de,  0)    /* #DE 除零            */
TRAP_STUB_NOERR(trap_ud,  6)    /* #UD 非法指令        */
TRAP_STUB_ERR  (trap_gp, 13)    /* #GP 通用保护        */
TRAP_STUB_ERR  (trap_pf, 14)    /* #PF 页错误          */

static const char *trap_name(UINT64 v) {
    switch (v) {
    case 0:  return "divide error";
    case 6:  return "invalid opcode";
    case 13: return "general protection";
    case 14: return "page fault";
    default: return "unknown";
    }
}

/* ------------------------------------------------------------------ 公共处理
 * 只打印然后停机。**不要试图返回** —— 返回就是重新执行那条出错指令。 */
void trap_common(UINT64 vec, UINT64 *frame, UINT64 *gprs) {
    UINT64 cr3 = 0;
    /* gprs 的布局（桩里按 rax,rbx,rcx,rdx,rsi,rdi,rbp,r8..r15 的顺序 push，
     * 所以最后压的在最低地址）：
     *   [0]r15 [1]r14 [2]r13 [3]r12 [4]r11 [5]r10 [6]r9 [7]r8
     *   [8]rbp [9]rdi [10]rsi [11]rdx [12]rcx [13]rbx [14]rax */
    enum { R15, R14, R13, R12, R11, R10, R9, R8, RBP, RDI, RSI, RDX, RCX, RBX, RAX };
    /* CR2 由汇编桩第一条指令存进 gTrapCr2 —— **不在这里读**。
     * 原因见 gTrapCr2 的注释：在 C 里读会被二次缺页覆盖。 */
    __asm__ __volatile__("movq %%cr3, %0" : "=r"(cr3));

    /* 先写日志 —— 控制台可能已经不可靠了，日志更稳 */
    log_puts("[log] *** CPU EXCEPTION \"");
    log_puts(trap_name(vec));
    log_puts("\" ***\r\n");
    log_kv_u64("exc.vector",  vec);
    log_kv_u64("exc.errcode", frame[0]);
    log_kv_u64("exc.rip",     frame[1]);
    log_kv_u64("exc.cs",      frame[2]);
    log_kv_u64("exc.rflags",  frame[3]);
    /* 硬件帧只有四项（errcode/RIP/CS/RFLAGS）。frame[4]/[5] 不再解释为 RSP/SS。 */
    log_kv_u64("exc.cr2",     gTrapCr2);
    log_kv_u64("exc.cr3",     cr3);
    log_kv_u64("exc.frame_ptr",(UINT64)(UINTN)frame);
    /* **通用寄存器全打出来。**
     * 光有 RIP 和 CR2 不够：像 movdqa 那种对齐异常，必须看地址是怎么算出来的。 */
    log_kv_u64("exc.rax", gprs[RAX]);
    log_kv_u64("exc.rbx", gprs[RBX]);
    log_kv_u64("exc.rcx", gprs[RCX]);
    log_kv_u64("exc.rdx", gprs[RDX]);
    log_kv_u64("exc.rsi", gprs[RSI]);
    log_kv_u64("exc.rdi", gprs[RDI]);
    log_kv_u64("exc.rbp", gprs[RBP]);
    log_kv_u64("exc.r8",  gprs[R8]);
    log_kv_u64("exc.r9",  gprs[R9]);
    log_kv_u64("exc.r10", gprs[R10]);
    log_kv_u64("exc.r11", gprs[R11]);
    log_kv_u64("exc.r12", gprs[R12]);
    log_kv_u64("exc.r13", gprs[R13]);
    log_kv_u64("exc.r14", gprs[R14]);
    log_kv_u64("exc.r15", gprs[R15]);

    /* **走一遍页表。** 这是 AUDIT 的 P0 —— 它把"这个地址到底映没映射"
     * 从推演变成事实。走的是当前 CR3（也就是固件那张表），只读。 */
    {
        PAGING_WALK w;
        paging_walk(gTrapCr2, &w);
        log_kv_u64("pf.cr2.present", w.Present);
        log_kv_u64("pf.cr2.level",   w.Level);
        log_kv_u64("pf.cr2.entry",   w.Entry);
        log_kv_u64("pf.cr2.phys",    w.Phys);
        paging_walk(frame[1], &w);
        log_kv_u64("pf.rip.present", w.Present);
        log_kv_u64("pf.rip.level",   w.Level);
        log_kv_u64("pf.rip.entry",   w.Entry);
        log_kv_u64("pf.rip.phys",    w.Phys);
        paging_walk(0x28025A0ULL, &w);
        log_kv_u64("pf.gapi.present", w.Present);
        log_kv_u64("pf.gapi.entry",   w.Entry);
        log_kv_u64("pf.gapi.phys",    w.Phys);
    }

    con_puts("\r\n\r\n  *** CPU EXCEPTION ***\r\n");
    con_puts("      vector : "); con_u64(vec);
    con_puts("   "); con_puts(trap_name(vec)); con_puts("\r\n");
    con_puts("      errcode: "); con_hex(frame[0]); con_puts("\r\n");
    con_puts("      RIP    : "); con_hex(frame[1]); con_puts("\r\n");
    con_puts("      CS     : "); con_hex(frame[2]); con_puts("\r\n");
    con_puts("      RFLAGS : "); con_hex(frame[3]); con_puts("\r\n");
    if (vec == 14) {
        PAGING_WALK w;
        con_puts("      CR2    : "); con_hex(gTrapCr2);
        con_puts("   <- faulting address\r\n");
        con_puts("      CR3    : "); con_hex(cr3); con_puts("\r\n");
        /* 这几行才是关键：地址到底映没映射，由页表回答，不由推演回答 */
        paging_walk(gTrapCr2, &w);
        con_puts("      CR2 map: ");
        if (w.Present) { con_puts("PRESENT -> "); con_hex(w.Phys); }
        else           { con_puts("NOT PRESENT, level "); con_u64(w.Level); con_puts(", entry "); con_hex(w.Entry); }
        con_puts("\r\n");
        paging_walk(frame[1], &w);
        con_puts("      RIP map: ");
        if (w.Present) { con_puts("PRESENT -> "); con_hex(w.Phys); }
        else           { con_puts("NOT PRESENT, level "); con_u64(w.Level); con_puts(", entry "); con_hex(w.Entry); }
        con_puts("\r\n");
        paging_walk(0x28025A0ULL, &w);
        con_puts("      28025A0: ");
        if (w.Present) { con_puts("PRESENT -> "); con_hex(w.Phys); }
        else           { con_puts("NOT PRESENT, level "); con_u64(w.Level); }
        con_puts("\r\n");
    }
    con_puts("      (halted, not resuming)\r\n");

    for (;;) { __asm__ __volatile__("cli\n\thlt"); }
}

/* ------------------------------------------------------------------ 装 IDT */
static void set_gate(int vec, void (*fn)(void)) {
    UINT64 a = (UINT64)(UINTN)fn;
    gIdt[vec].off0 = (UINT16)(a & 0xFFFF);
    gIdt[vec].sel  = gCs;               /* 用当前的 CS —— UEFI 下是 0x38，不是 0x08 */
    gIdt[vec].ist  = 0;
    gIdt[vec].type = 0x8E;
    gIdt[vec].off1 = (UINT16)((a >> 16) & 0xFFFF);
    gIdt[vec].off2 = (UINT32)(a >> 32);
    gIdt[vec].zero = 0;
}

int trap_init(void) {
    TND_IDTR cur;
    __asm__ __volatile__("movw %%cs, %0" : "=r"(gCs));

    /* 把固件现有的 IDT 整张抄过来 —— 只覆盖我们在意的那几个。 */
    __asm__ __volatile__("sidt %0" : "=m"(cur));
    if (cur.base && cur.limit >= 256 * 16 - 1) {
        const UINT8 *src = (const UINT8 *)(UINTN)cur.base;
        UINT8 *dst = (UINT8 *)gIdt;
        for (UINTN i = 0; i < 256 * 16; i++) dst[i] = src[i];
        log_puts("[log] trap: copied the firmware IDT\r\n");
    } else {
        t_memzero(gIdt, sizeof(gIdt));
        log_puts("[log] trap: no usable firmware IDT, starting empty\r\n");
    }

    set_gate(0,  trap_de);
    set_gate(6,  trap_ud);
    set_gate(13, trap_gp);
    set_gate(14, trap_pf);
    set_gate(0x20, trap_irq0);   /* 定时器：必须自己接，见上面 trap_irq0 的注释 */

    /* **把 8259 的 IRQ 全屏蔽掉。**
     *
     * 原因：上面的做法是把固件的 IDT 整张抄过来，只替换我们关心的四个向量。
     * 其余 252 个（包括 0x20 定时器）仍然指向**固件的处理程序**。
     * 而 ExitBootServices 之后，固件已经把那块内存交还给我们了 ——
     * 定时器每打断一次，就执行一遍那块地址上的东西。
     *
     * 表现出来就是随机的寄存器损坏：
     *   - 被中断的指令流里 %rax 变成 29 / 31 这种毫无来由的值
     *   - 控制台偶尔丢一个字符
     *   - RIP 有时落在一条指令的中间
     * -d int 的日志里能看到 Servicing hardware INT=0x20 一直出现，
     * 而它的 IP 落在 0x1DCC11F0（固件的地址）。
     *
     * 退出启动服务之后我们不需要任何固件中断，全屏蔽最干净。 */
    __asm__ __volatile__(
        "movb $0xFF, %%al\n"
        "outb %%al, $0x21\n"     /* 主 PIC：IRQ0..7 全屏蔽 */
        "outb %%al, $0xA1\n"     /* 从 PIC：IRQ8..15 全屏蔽 */
        : : : "rax");
    log_kv("trap", "PIC IRQs masked (0x21=0xFF, 0xA1=0xFF)");

    gIdtr.limit = (UINT16)(sizeof(gIdt) - 1);
    gIdtr.base  = (UINT64)(UINTN)gIdt;
    __asm__ __volatile__("lidt %0" :: "m"(gIdtr));

    log_puts("[log] trap: IDT installed, CS="); log_hex(gCs); log_puts("\r\n");
    return 1;
}

/* 放在模块表**最前面** —— 任何东西出异常之前它得先装好 */
const TND_MODULE_DEF gModTrap = { "trap", "CPU exception handler (report + halt)", trap_init };