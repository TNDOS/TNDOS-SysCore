/* ============================================================================
 * TNX 链接脚本
 *
 * 只有一件事要保证：**代码和数据链接在固定基址上**，这样加载器就不需要重定位。
 * 基址必须和内核的 TNX_IMAGE_BASE 一致。
 *
 * /DISCARD/ 那段不是可选的：clang/lld 默认会吐 .eh_frame / .comment / .note，
 * 而 TNX 的非目标里明写了"不要异常元数据、不要调试信息"。留着它们
 * 既浪费空间，也是在给"慢慢长成 PE"开门。
 * ==========================================================================*/
ENTRY(tnx_entry)

SECTIONS
{
    . = 0x0000000001000000;

    .text : ALIGN(16) {
        *(.text.entry)
        *(.text*)
    }

    .rodata : ALIGN(16) {
        *(.rodata*)
    }

    .data : ALIGN(16) {
        *(.data*)
    }

    .bss : ALIGN(16) {
        *(.bss*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.eh_frame*)
        *(.comment)
        *(.note*)
        *(.llvm_addrsig)
        *(.rela*)
        *(.rel*)
    }
}
