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
 * **必须是 naked**：不然编译器会在入口插 prologue，rsp 就不是 CPU 压好的那个了，
 * frame 的偏移全错。
 *
 * 参数按 MS ABI 放：第一个在 rcx（向量号），第二个在 rdx（frame 指针）。
 *
 * **必须用 call 不能用 jmp。** MS ABI 要求调用方预留 32 字节 shadow space、
 * 而且入口 16 字节对齐；硬件异常入口这两条都不保证。jmp 进去之后，
 * 被调函数一旦 spill 到 shadow space，就会踩到异常帧。 */
/* 有出错码的那几个：#PF 栈上是 [errcode][RIP][CS][RFLAGS]。
 *
 * **注意：同特权级的 #PF 不压 RSP/SS。** 那是跨特权级切换才有的。
 * 所以 frame[4]/frame[5] 是栈上的任意内容，不能当 RSP/SS 解释 ——
 * 这一点原来记错了，外面评审指出来的。 */
#define TRAP_STUB_ERR(name, vec)                              \
    __attribute__((naked)) void name(void) {                  \
        __asm__ __volatile__("movq %cr2, %rax\n"            \
                             "movq %rax, gTrapCr2(%rip)\n"   \
                             "movq %rsp, %rdx\n"             \
                             "movl $" #vec ", %ecx\n"       \
                             "andq $-16, %rsp\n"             \
                             "subq $0x28, %rsp\n"            \
                             "call trap_common\n"            \
                             "hlt\n");                        \
    }

/* 不压出错码的那几个：自己补一个 0，让帧布局统一。
 * **补完之后才能取 frame 指针**，否则会指到补的那个 0 前面去。 */
#define TRAP_STUB_NOERR(name, vec)                            \
    __attribute__((naked)) void name(void) {                  \
        __asm__ __volatile__("movq %cr2, %rax\n"            \
                             "movq %rax, gTrapCr2(%rip)\n"   \
                             "pushq $0\n"                     \
                             "movq %rsp, %rdx\n"             \
                             "movl $" #vec ", %ecx\n"       \
                             "andq $-16, %rsp\n"             \
                             "subq $0x28, %rsp\n"            \
                             "call trap_common\n"            \
                             "hlt\n");                        \
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
void trap_common(UINT64 vec, UINT64 *frame) {
    UINT64 cr3 = 0;
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

    gIdtr.limit = (UINT16)(sizeof(gIdt) - 1);
    gIdtr.base  = (UINT64)(UINTN)gIdt;
    __asm__ __volatile__("lidt %0" :: "m"(gIdtr));

    log_puts("[log] trap: IDT installed, CS="); log_hex(gCs); log_puts("\r\n");
    return 1;
}

/* 放在模块表**最前面** —— 任何东西出异常之前它得先装好 */
const TND_MODULE_DEF gModTrap = { "trap", "CPU exception handler (report + halt)", trap_init };
