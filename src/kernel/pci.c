/* ============================================================================
 * TNDDOS —— PCI 枚举（走 ECAM）
 *
 * **为什么不用 0xCF8/0xCFC：内核是 MS ABI，碰不到端口。**
 * clang 在这个配置下既不提供 __inbyte，也不让写内联汇编（见
 * docs/DRIVER-SDK.md 第 8 节）。这不是偷懒，是 ABI 逼出来的。
 *
 * ECAM（Enhanced Configuration Access Mechanism）把配置空间**映射到内存**，
 * 于是全是普通读写。基址在 ACPI 的 MCFG 表里：
 *
 *     系统配置表 -> RSDP -> XSDT -> MCFG -> ECAM 基址
 *
 * 这几步也全是读内存。**整条路上没有一条端口指令。**
 * ==========================================================================*/
#include "tnd.h"

/* ------------------------------------------------------------ ACPI 结构
 * 只放这里用得上的那几个。字段名按规范，方便对照。 */
typedef struct {
    char     Signature[8];      /* "RSD PTR " */
    UINT8    Checksum;
    char     OemId[6];
    UINT8    Revision;
    UINT32   RsdtAddress;
    UINT32   Length;            /* ACPI 2.0+ 才有 */
    UINT64   XsdtAddress;
    UINT8    ExtendedChecksum;
    UINT8    Reserved[3];
} ACPI_RSDP;

typedef struct {
    char     Signature[4];
    UINT32   Length;
    UINT8    Revision;
    UINT8    Checksum;
    char     OemId[6];
    char     OemTableId[8];
    UINT32   OemRevision;
    UINT32   CreatorId;
    UINT32   CreatorRevision;
} ACPI_SDT;

typedef struct {
    UINT64   BaseAddress;       /* ECAM 起始物理地址 */
    UINT16   PciSegment;
    UINT8    StartBus;
    UINT8    EndBus;
    UINT32   Reserved;
} ACPI_MCFG_ENTRY;

typedef struct { UINT8 a[16]; } TND_GUID_BYTES;   /* 只用来比对，不用解构 */

/* ACPI 2.0 表的 GUID：8868E871-E4F1-11D3-BC22-0080C73C8881 */
static const UINT8 kAcpi20Guid[16] = {
    0x71, 0xE8, 0x68, 0x88, 0xF1, 0xE4, 0xD3, 0x11,
    0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81
};

/* ------------------------------------------------------------------ 状态 */
static UINT64 gEcamBase = 0;
static UINT8  gEcamBusStart = 0, gEcamBusEnd = 0;
static int    gEcamOk = 0;

int      pci_ready(void)     { return gEcamOk; }
UINT64   pci_ecam_base(void) { return gEcamBase; }

/* ECAM 布局：每（总线, 设备, 功能）占 4KB，设备占 32KB（8 个功能） */
static volatile UINT32 *ecam(UINT8 bus, UINT8 dev, UINT8 fn, UINT8 off) {
    UINT64 a = gEcamBase
             + ((UINT64)(bus - gEcamBusStart) << 20)
             + ((UINT64)dev << 15)
             + ((UINT64)fn  << 12)
             + (off & 0xFC);
    return (volatile UINT32 *)(UINTN)a;
}

UINT32 pci_read32(UINT8 bus, UINT8 dev, UINT8 fn, UINT8 off) {
    if (!gEcamOk) return 0xFFFFFFFFu;
    return *ecam(bus, dev, fn, off);
}

static UINT16 pci_vendor(UINT8 bus, UINT8 dev, UINT8 fn) {
    return (UINT16)(pci_read32(bus, dev, fn, 0x00) & 0xFFFFu);
}

/* ---------------------------------------------------------------- 找 MCFG */

static int sig4(const char *a, const char *b) {
    for (int i = 0; i < 4; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static void take_mcfg(const ACPI_SDT *sdt) {
    /* MCFG = 头 + 8 字节保留 + 若干 16 字节项 */
    const UINT8 *p = (const UINT8 *)sdt + sizeof(ACPI_SDT) + 8;
    const UINT8 *end = (const UINT8 *)sdt + sdt->Length;
    int n = 0;

    while (p + sizeof(ACPI_MCFG_ENTRY) <= end) {
        const ACPI_MCFG_ENTRY *e = (const ACPI_MCFG_ENTRY *)p;
        if (e->BaseAddress) {
            gEcamBase = e->BaseAddress;
            gEcamBusStart = e->StartBus;
            gEcamBusEnd = e->EndBus;
            gEcamOk = 1;
            log_puts("[log] pci: ECAM at "); log_hex(gEcamBase);
            log_puts("  bus "); log_u64(e->StartBus);
            log_puts(".."); log_u64(e->EndBus); log_puts("\r\n");
            n++;
        }
        p += sizeof(ACPI_MCFG_ENTRY);
    }
    if (!n) log_puts("[log] pci: MCFG has no usable entry\r\n");
}

static void walk_xsdt(const ACPI_RSDP *rsdp) {
    const ACPI_SDT *xsdt = (const ACPI_SDT *)(UINTN)rsdp->XsdtAddress;
    UINTN n, i;

    if (!rsdp->XsdtAddress) { log_puts("[log] pci: RSDP has no XSDT\r\n"); return; }
    if (!sig4(xsdt->Signature, "XSDT")) { log_puts("[log] pci: bad XSDT signature\r\n"); return; }

    n = (xsdt->Length - sizeof(ACPI_SDT)) / 8;
    for (i = 0; i < n; i++) {
        const UINT64 *ent = (const UINT64 *)((const UINT8 *)xsdt + sizeof(ACPI_SDT));
        const ACPI_SDT *t = (const ACPI_SDT *)(UINTN)ent[i];
        if (!t) continue;
        if (sig4(t->Signature, "MCFG")) { take_mcfg(t); return; }
    }
    log_puts("[log] pci: no MCFG in XSDT\r\n");
}

int pci_init(void) {
    EFI_CONFIGURATION_TABLE *ct;
    UINTN n, i;

    gEcamOk = 0; gEcamBase = 0;

    if (!gEnv.ST || !gEnv.ST->ConfigurationTable) {
        log_puts("[log] pci: no configuration table\r\n"); return 0;
    }
    ct = (EFI_CONFIGURATION_TABLE *)gEnv.ST->ConfigurationTable;
    n  = gEnv.ST->NumberOfTableEntries;

    for (i = 0; i < n; i++) {
        const UINT8 *g = (const UINT8 *)&ct[i].VendorGuid;
        int same = 1;
        for (int k = 0; k < 16; k++) if (g[k] != kAcpi20Guid[k]) { same = 0; break; }
        if (!same) continue;
        walk_xsdt((const ACPI_RSDP *)ct[i].VendorTable);
        break;
    }

    if (!gEcamOk) {
        log_puts("[log] pci: no ECAM -- PCI enumeration unavailable\r\n");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ 扫描
 * 只做**发现的活**：把设备变成 TND_DEVICE 交给驱动模型去认领。
 * 类代码、BAR、中断这些东西先只记录，不解释 —— 解释是各个驱动自己的事。 */

#define TND_MAX_PCI 48
/* ------------------------------------------------------------------ v2.6
 * **把一个 PCI 设备从固件的驱动手里摘下来。**
 *
 * Boot Services 还活着的时候，固件自己的驱动仍然拥有设备 —— OVMF 的
 * Ps2KeyboardDxe 还在轮询 8042，XhciDxe 还在用 USB 端口。我们插进去之后
 * 两边抢同一个硬件，症状千奇百怪（键盘只剩最后一个字符、USB 完成事件被
 * 顶掉、控制器转不出去）。
 *
 * UEFI 的标准做法是 DisconnectController。内核有 gBS，驱动（TNX）拿不到
 * 固件服务，所以只能由内核代做。
 *
 * **只对调用者自己那个设备做。** GOP 挂在 00:01.0 上，摘了它屏幕就黑。 */
int pci_disown(int bus, int dev, int fun) {
    EFI_HANDLE *buf = 0;
    UINTN count = 0, i;
    int done = 0;

    if (!gEnv.BS || !gEnv.BS->LocateHandleBuffer || !gEnv.BS->DisconnectController)
        return -1;

    if (EFI_ERROR(gEnv.BS->LocateHandleBuffer(2 /* ByProtocol */,
                                              (EFI_GUID *)&gEfiPciIoProtocolGuid,
                                              0, &count, &buf)) || !buf)
        return -1;

    for (i = 0; i < count; i++) {
        EFI_PCI_IO_PROTOCOL *io = 0;
        UINTN seg = 0, b = 0, d = 0, f = 0;

        if (EFI_ERROR(gEnv.BS->HandleProtocol(buf[i], &gEfiPciIoProtocolGuid, (VOID **)&io)) || !io)
            continue;
        if (!io->GetLocation) continue;
        if (EFI_ERROR(io->GetLocation(io, &seg, &b, &d, &f))) continue;

        if ((int)b == bus && (int)d == dev && (int)f == fun) {
            if (!EFI_ERROR(gEnv.BS->DisconnectController(buf[i], 0, 0))) done++;
        }
    }

    if (gEnv.BS->FreePool) gEnv.BS->FreePool(buf);
    return done;
}

static TND_DEVICE gPciDev[TND_MAX_PCI];
static int gPciCount = 0;

int pci_device_count(void) { return gPciCount; }
const TND_DEVICE *pci_device(int i) {
    return (i >= 0 && i < gPciCount) ? &gPciDev[i] : 0;
}

/* 扫**一条**总线。pci_scan 对 bus 0 和每个 VMD 的总线段各调一次。 */
static void scan_bus(UINT8 only) {
    UINT8 bus, dev, fn;

    if (gPciCount >= TND_MAX_PCI) return;

    /* **只扫这一条，不盲扫 0..255。**
     *
     * MCFG 说这段 ECAM 覆盖 bus 0..255，但**没实现的总线在某些机器上会
     * 别名回 bus 0** —— QEMU 就是这样。盲扫的后果是同一批设备被数 8 遍
     * （实测：6 个设备变成 48 个），而且看起来还挺像真的。
     *
     * 真实的多总线只能从 **PCI 桥的 secondary bus 号**走 —— 那是下一步。
     * 先把 bus 0 做对：PC 上的设备本来就几乎都在 bus 0。 */
    for (bus = only; bus == only; bus++) {
        for (dev = 0; dev < 32; dev++) {
            UINT16 v = pci_vendor(bus, dev, 0);
            UINT8  nfn = 1;

            if (v == 0xFFFF || v == 0) continue;      /* 这个位置没人 */

            /* 多功能位（头类型 bit7）决定要不要看 fn 1..7 */
            if (pci_read32(bus, dev, 0, 0x0C) & 0x00800000u) nfn = 8;

            for (fn = 0; fn < nfn; fn++) {
                UINT32 id, cls, bar0;
                TND_DEVICE *d;

                if (fn && pci_vendor(bus, dev, fn) == 0xFFFF) continue;
                if (gPciCount >= TND_MAX_PCI) return;

                id   = pci_read32(bus, dev, fn, 0x00);
                cls  = pci_read32(bus, dev, fn, 0x08);
                bar0 = pci_read32(bus, dev, fn, 0x10);

                d = &gPciDev[gPciCount];
                t_memzero(d, sizeof(*d));
                d->StructSize = sizeof(TND_DEVICE);
                /* **按 class code 定类型** —— 驱动就是靠这个自称的。
                 * 原来一律标 BUS 是个占位，结果显卡驱动没法说「我要显示设备」。
                 * class 的高字节是基类：01=存储 03=显示 0C=串行总线。 */
                switch ((cls >> 24) & 0xFF) {
                case 0x01: d->Type = TND_DRV_BLOCK;   break;   /* 大容量存储：AHCI/NVMe */
                case 0x03: d->Type = TND_DRV_DISPLAY; break;   /* 显示：显卡 */
                case 0x0C: d->Type = TND_DRV_BUS;     break;   /* 串行总线：USB */
                default:   d->Type = TND_DRV_BUS;     break;
                }
                d->Class  = cls >> 8;           /* 去掉 revision，留 class+subclass+progif */
                d->Unit   = (UINT32)gPciCount;
                d->Vendor = (UINT32)(id & 0xFFFF);
    d->Bus = bus; d->Dev = dev; d->Fun = fn;   /* v2.6：给 tnd_pci_own 用 */
                d->Device = (UINT32)(id >> 16);
                d->Base   = bar0 & 0xFFFFFFF0u;  /* 只取地址，先不管 I/O 空间标记 */
                d->Size   = 0;
                {
                    /* 六个 BAR。**64 位 BAR 占两个槽**（低 4 位里的 type 位
                     * 是 0b10），高半必须一起读进来，否则地址只有低 32 位。 */
                    for (int b = 0; b < 6; b++) {
                        UINT32 v = pci_read32(bus, dev, fn, (UINT8)(0x10 + b * 4));
                        UINT8  t = (UINT8)((v >> 1) & 0x3);
                        d->Bar[b] = (UINT64)(v & 0xFFFFFFF0u);
                        if (t == 0x2 && b < 5) {
                            UINT32 hi = pci_read32(bus, dev, fn, (UINT8)(0x10 + (b + 1) * 4));
                            d->Bar[b] |= ((UINT64)hi << 32);
                            b++;            /* 高半已经吃掉，跳过 */
                        }
                    }
                }
                d->Irq    = 0;                   /* 没有 IDT，中断先留空 */

                /* 名字做成 BDF 加类码 —— 一眼能对上 lspci 的输出 */
                {
                    const char *hx = "0123456789ABCDEF";
                    d->Name[0]  = hx[(bus >> 4) & 0xF]; d->Name[1]  = hx[bus & 0xF];
                    d->Name[2]  = ':';
                    d->Name[3]  = hx[(dev >> 4) & 0xF]; d->Name[4]  = hx[dev & 0xF];
                    d->Name[5]  = '.';
                    d->Name[6]  = hx[fn & 0xF];
                    d->Name[7]  = ' ';
                    d->Name[8]  = hx[(cls >> 20) & 0xF];
                    d->Name[9]  = hx[(cls >> 16) & 0xF];
                    d->Name[10] = 0;
                }

                gPciCount++;
            }
        }
    }

}

/* ============================================================================
 * 枚举入口
 *
 * **VMD（Volume Management Device）是这版新加的，理由是实测。**
 *
 * 真机上：笔记本自己组了硬盘阵列（Intel RST/VMD），exitboot 之后
 * "no block device at all"，而且屏幕上**连一句 [nvme] 都没有** —— 因为
 * NVMe 控制器根本不在 bus 0 上，VMD 把它藏在自己那段总线后面。
 *
 * 但不能盲扫 0..255（见 scan_bus 上面那段：未实现的总线会别名回 bus 0）。
 * 正确做法是**问 VMD 它占哪段总线** —— 配置空间 0x40 的 VMCAP：
 *     bits 15:8   总线号起点
 *     bits 23:16  总线数量
 * 然后只扫那一段，既找得到设备，又碰不到未实现的总线。
 * ==========================================================================*/
void pci_scan(void) {
    int i, n;

    gPciCount = 0;
    if (!gEcamOk) return;

    scan_bus(gEcamBusStart);

    /* 只对 bus 0 上找到的设备发问 —— VMD 一定在 bus 0。 */
    n = gPciCount;
    for (i = 0; i < n; i++) {
        /* class 0x01 存储 / subclass 0x04 RAID：这就是 VMD（或类似的
         * 卷管理控制器）。PCI 规范里它叫 "RAID bus controller"。 */
        if ((UINT32)gPciDev[i].Class != 0x010400u) continue;
        {
            UINT8  vb = (UINT8)gPciDev[i].Bus;
            UINT8  vd = (UINT8)gPciDev[i].Dev;
            UINT8  vf = (UINT8)gPciDev[i].Fun;
            UINT32 cap  = pci_read32(vb, vd, vf, 0x40);
            UINT8  base = (UINT8)((cap >> 8) & 0xFF);
            UINT8  cnt  = (UINT8)((cap >> 16) & 0xFF);
            UINT8  b;

            if (cnt == 0 || cnt > 32) cnt = 1;
            log_puts("[log] pci: VMD "); log_hex((UINT64)((vd << 3) | vf));
            log_puts(" covers buses "); log_u64((UINT64)base);
            log_puts(".."); log_u64((UINT64)(base + cnt - 1));
            log_puts(" -- scanning them\r\n");

            for (b = base; b != (UINT8)(base + cnt); b++) scan_bus(b);
        }
    }

    log_puts("[log] pci: found "); log_u64((UINT64)gPciCount); log_puts(" device(s)\r\n");
}

/* ------------------------------------------------------------------ 模块 */
static int pci_module_init(void) {
    /* **没有 ECAM 不算致命** —— 只是 PCI 枚举不可用。
     * 真实硬件上 ACPI 一定在；虚拟机上也该在。不在就得查，所以打日志。 */
    if (!pci_init()) return 0;
    pci_scan();
    return 1;
}

const TND_MODULE_DEF gModPci = { "pci", "PCI enumeration via ACPI MCFG / ECAM", pci_module_init };

void pci_report(void) {
    con_puts("\r\n  PCI: ECAM ");
    if (!gEcamOk) { con_puts("(unavailable)\r\n"); return; }
    con_hex(gEcamBase);
    con_puts("   bus "); con_u64(gEcamBusStart); con_puts(".."); con_u64(gEcamBusEnd);
    con_puts("   "); con_u64((UINT64)gPciCount); con_puts(" device(s)\r\n");
    for (int i = 0; i < gPciCount; i++) {
        const TND_DEVICE *d = &gPciDev[i];
        con_puts("    "); con_puts(d->Name);
        con_puts("   " ); con_hex(((UINT64)d->Vendor << 16) | (UINT64)d->Device);
        con_puts("   class="); con_hex((UINT64)d->Class);
        con_puts("   bar0="); con_hex(d->Base);
        con_puts("\r\n");
    }
}
