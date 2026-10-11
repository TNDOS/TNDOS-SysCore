/* ============================================================================
 * TNDDOS —— PS/2 键盘：**直接读 8042，不问固件**
 *
 * 这是「我们能自己碰硬件」的第一块证明。在此之前，键盘一直是固件的
 * SimpleTextInput（见 src/drv/kbd.c，那个文件甚至把这步写在注释里当 TODO）。
 *
 * **暂时编进内核**：驱动接口还没定，先让硬件能通。等它稳定了再抽成 TNX 驱动，
 * 那时候「SDK 该给驱动什么」自己就现形了 —— 这正是接着写驱动而不是先设计接口的原因。
 *
 * **没有 IDT，所以走轮询。** 8042 配置字节里键盘中断是关掉的（bit0=0）。
 * 这与驱动 SDK 草案第 2 节的取舍一致：留中断的位置，先不实现。
 * ==========================================================================*/
#include "tnd.h"

#define PS2_DATA   0x60
#define PS2_CMD    0x64   /* 写：命令；读：状态 */

/* 状态寄存器的位 */
#define ST_OUTB    0x01   /* 输出缓冲满 —— 有数据可读 */
#define ST_INPB    0x02   /* 输入缓冲满 —— 先别写 */

static UINT8 ps2_in(UINT16 p)          { return __inbyte(p); }
static void  ps2_out(UINT16 p, UINT8 v){ __outbyte(p, v); }

/* 等输入缓冲空。8042 很慢，每步都要等 —— 不等就会丢命令，而且症状很难查。 */
static int ps2_wait_write(void) {
    for (int i = 0; i < 100000; i++)
        if (!(ps2_in(PS2_CMD) & ST_INPB)) return 1;
    return 0;
}

/* 等输出缓冲满。timeout 是为了「没接键盘」时不至于卡死 —— 虚拟机尤其如此。 */
static int ps2_wait_read(void) {
    for (int i = 0; i < 100000; i++)
        if (ps2_in(PS2_CMD) & ST_OUTB) return 1;
    return 0;
}

static void ps2_cmd(UINT8 c) { if (ps2_wait_write()) ps2_out(PS2_CMD, c); }
static void ps2_data(UINT8 d){ if (ps2_wait_write()) ps2_out(PS2_DATA, d); }

/* 读一个字节；读不到返回 -1。**丢掉的字节也必须读出来**，否则缓冲一直是满的。 */
static int ps2_read_byte(void) {
    if (!(ps2_in(PS2_CMD) & ST_OUTB)) return -1;
    return (int)ps2_in(PS2_DATA);
}

/* ----------------------------------------------------------------- 状态 */

static int sInit = 0;      /* 自检过了吗 */
static int sShift = 0;     /* 左右 Shift 任一按下 */
static int sCtrl = 0;
static int sAlt = 0;
static int sCaps = 0;
static int sExt = 0;       /* 上一个字节是 0xE0 前缀 */

int ps2_ready(void) { return sInit; }
int ps2_shift(void) { return sShift; }
int ps2_ctrl(void)  { return sCtrl; }
int ps2_alt(void)   { return sAlt; }

/* ------------------------------------------------------- 扫描码 -> ASCII
 * Set 1，美式布局。低 7 位是按下，|0x80 是松开。
 *
 * **表里为 0 的位置不是「没有键」，是「这个键不产生字符」** —— 比如 Ctrl、F1。
 * 需要用它们的地方（比如方向键）在下面显式处理。 */
static const char kNormal[128] = {
    0,    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,    '\\','z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0,    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

static const char kShifted[128] = {
    0,    27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '\'', '~',
    0,    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0,    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

/* ------------------------------------------------------------- 初始化 */

/* 自检：让 8042 报一下自己是谁。**这一步失败就说明压根没有 8042**，
 * 而不是「键盘没插」—— 两者要分开，不然查错会查错方向。 */

int ps2_init(void) {
    UINT8 cfg = 0;
    int r;

    sInit = 0; sShift = 0; sCtrl = 0; sAlt = 0; sCaps = 0; sExt = 0;

    /* 1. 先把两个端口都关了，免得初始化期间它们往缓冲里塞东西 */
    ps2_cmd(0xAD);   /* 禁键盘 */
    ps2_cmd(0xA7);   /* 禁鼠标 */

    /* 2. 清空输出缓冲 —— 里面的东西多半是上一步留下的，不是按键 */
    for (int i = 0; i < 32; i++) {
        if (!(ps2_in(PS2_CMD) & ST_OUTB)) break;
        (void)ps2_in(PS2_DATA);
    }

    /* 3. 读配置字节 */
    ps2_cmd(0x20);
    if (!ps2_wait_read()) return 0;     /* 读不到 -> 没有 8042 */
    cfg = (UINT8)ps2_in(PS2_DATA);

    /* 4. 改配置：
     *      bit0 = 0  关键盘中断     <- 我们没有 IDT，只能轮询
     *      bit1 = 0  关鼠标中断
     *      bit4 = 1  开键盘时钟
     *      bit5 = 0  关鼠标时钟（鼠标还没做）
     *      bit6 = 0  不做扫描码翻译  <- 译码我们自己在表里做，更可控 */
    cfg &= (UINT8)~(0x01 | 0x02 | 0x20 | 0x40);
    cfg |= (UINT8)0x10;

    /* 5. 写回去 */
    ps2_cmd(0x60);
    ps2_data(cfg);

    /* 6. 使能键盘 */
    ps2_cmd(0xAE);

    /* 7. 自检：0xAA 应该回 0x55。回了才敢说「8042 在」 */
    ps2_cmd(0xAA);
    if (!ps2_wait_read()) return 0;
    r = ps2_in(PS2_DATA);
    if (r != 0x55) {
        /* 自检没过。**但要区分「没设备」和「自检失败」** —— 后者是真异常。 */
        log_kv_u64("ps2.selfTest", (UINT64)r);
        return 0;
    }

    sInit = 1;
    return 1;
}

/* ------------------------------------------------------------- 取键 */

/* 返回：>0 一个 ASCII；0 没按键；-1 是一个「没有字符产出」的键（方向键等）。
 *
 * **一次只处理一个字节。** 松开码、E0 前缀、修饰键状态都在这里消化掉，
 * 调用方只管拿字符。 */
int ps2_poll(void) {
    int b, sc;

    if (!sInit) return 0;

    b = ps2_read_byte();
    if (b < 0) return 0;

    if (b == 0xE0) { sExt = 1; return -1; }   /* 扩展前缀，下一个字节才是真的 */

    sc = b & 0x7F;

    /* 修饰键：按下和松开都要更新状态，且它们不产出字符 */
    if (!sExt) {
        switch (sc) {
        case 0x2A: case 0x36: sShift = !(b & 0x80); return -1;   /* L/R Shift */
        case 0x1D:            sCtrl  = !(b & 0x80); return -1;   /* L Ctrl    */
        case 0x38:            sAlt   = !(b & 0x80); return -1;   /* L Alt     */
        case 0x3A:            if (!(b & 0x80)) sCaps = !sCaps; return -1;
        default: break;
        }
    }

    sExt = 0;

    if (b & 0x80) return -1;      /* 松开码，除了修饰键之外不管 */
    if (sc >= 128) return -1;

    {
        /* CapsLock 只影响字母，不影响数字键上的符号 —— 这是 PC 的规矩 */
        int letters = (sc >= 0x10 && sc <= 0x19) || (sc >= 0x1E && sc <= 0x26) ||
                      (sc >= 0x2C && sc <= 0x32);
        int upper = sShift ^ (sCaps && letters);
        char c = upper ? kShifted[sc] : kNormal[sc];
        if (!c) return -1;
        return (int)(unsigned char)c;
    }
}

/* ------------------------------------------------------------- 自检命令 */

/* 敲键就回声，ESC 退出。
 *
 * **这不是个正经的输入通道** —— 它只是用来证明「我们能自己读键」。
 * 真正接进 shell 是下一步的事，那时才会碰到「驱动怎么把输入交给系统」。 */
void ps2_echo_test(void) {
    if (!sInit) {
        con_puts("  no PS/2 controller (self-test failed). see the log.\r\n");
        return;
    }
    con_puts("  reading the 8042 directly. type something, ESC to stop.\r\n");
    con_puts("  ");
    for (;;) {
        int c = ps2_poll();
        /* 枚举期间没有别的事可做，空转就是这里的等待方式 ——
         * 和 api_getkey 的循环是同一个道理，只是等的对象不同。 */
        if (c == 0) { gEnv.BS->Stall(500); continue; }
        if (c < 0) continue;
        if (c == 27) break;                       /* ESC */
        if (c == '\r') c = '\n';
        if (c == '\b') { con_puts("\b \b"); continue; }
        con_putc((char)c);
    }
    con_puts("\r\n  stopped.\r\n");
}
