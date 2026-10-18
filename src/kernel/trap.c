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

/* ------------------------------------------------------------------ 桩
 * **必须是 naked**：不然编译器会在入口插 prologue，rsp 就不是 CPU 压好的那个了，
 * frame 的偏移全错。
 *
 * 参数按 MS ABI 放：第一个在 rcx（向量号），第二个在 rdx（frame 指针）。
 * 用 jmp 不用 call —— 处理器调的入口不需要返回地址。
 *
 * 有的异常 CPU 会压一个出错码、有的不压。不压的那几个自己补一个 0，
 * 让 frame 的布局统一：frame[0]=出错码 frame[1]=RIP frame[2]=CS frame[3]=RFLAGS。 */
#define TRAP_STUB_ERR(name, vec)                              \
    __attribute__((naked)) void name(void) {                  \
        __asm__ __volatile__("movq %rsp, %rdx\n"              \
                             "movl $" #vec ", %ecx\n"          \
                             "jmp trap_common\n");            \
    }

#define TRAP_STUB_NOERR(name, vec)                            \
    __attribute__((naked)) void name(void) {                  \
        __asm__ __volatile__("pushq $0\n"                    \
                             "movq %rsp, %rdx\n"              \
                             "movl $" #vec ", %ecx\n"          \
                             "jmp trap_common\n");            \
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
    UINT64 cr2 = 0;
    UINT64 rsp = 0, rbp = 0;
    __asm__ __volatile__("movq %%cr2, %0" : "=r"(cr2));
    /* **rsp / rbp 必须一起记。**
     * 只记 cr2 的话，就只能反推 rbp，而反推依赖"故障指令是 rbp-0x44"这个假设 ——
     * 那次我就靠这个假设走错了方向（还把 cr2 抄成了 17 位）。 */
    __asm__ __volatile__("movq %%rsp, %0" : "=r"(rsp));
    __asm__ __volatile__("movq %%rbp, %0" : "=r"(rbp));

    /* 先写日志 —— 控制台可能已经不可靠了，日志更稳 */
    log_puts("[log] *** CPU EXCEPTION \"");
    log_puts(trap_name(vec));
    log_puts("\" ***\r\n");
    log_kv_u64("exc.vector",  vec);
    log_kv_u64("exc.errcode", frame[0]);
    log_kv_u64("exc.rip",     frame[1]);
    log_kv_u64("exc.cs",      frame[2]);
    log_kv_u64("exc.rflags",  frame[3]);
    log_kv_u64("exc.cr2",     cr2);
    log_kv_u64("exc.rsp",     rsp);
    log_kv_u64("exc.rbp",     rbp);

    con_puts("\r\n\r\n  *** CPU EXCEPTION ***\r\n");
    con_puts("      vector : "); con_u64(vec);
    con_puts("   "); con_puts(trap_name(vec)); con_puts("\r\n");
    con_puts("      errcode: "); con_hex(frame[0]); con_puts("\r\n");
    con_puts("      RIP    : "); con_hex(frame[1]); con_puts("\r\n");
    con_puts("      CS     : "); con_hex(frame[2]); con_puts("\r\n");
    con_puts("      RFLAGS : "); con_hex(frame[3]); con_puts("\r\n");
    if (vec == 14) {
        con_puts("      CR2    : "); con_hex(cr2);
        con_puts("   <- faulting address\r\n");
        con_puts("      RSP    : "); con_hex(rsp); con_puts("\r\n");
        con_puts("      RBP    : "); con_hex(rbp); con_puts("\r\n");
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
