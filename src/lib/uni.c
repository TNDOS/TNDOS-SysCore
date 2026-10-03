/* ============================================================================
 * TNDDOS -- Unicode 基础：解码与字宽
 *
 * 为什么单独一个文件：控制台要正确排版就必须知道"这个字符占几格"，
 * 而这和字体没关系 —— 字体只决定"画成什么样"。
 * 先把布局这一半做对，字形那一半（CJK 点阵）后面接上来即可，
 * **接口不用再动**。
 * ==========================================================================*/
#include "tnd.h"

/* UTF-8 解码一个码点。
 * 返回消耗的字节数（1..4）；非法序列返回 1 并给出 U+FFFD。
 *
 * **不做容错性修复**：多字节序列只要有一个续字节不对就整个判非法，
 * 而不是"尽可能多地解出来"。后者会让坏数据解码成一串看似合理的字符，
 * 反而更难查。 */
int t_utf8_decode(const char *s, UINT32 *outCp) {
    const UINT8 *p = (const UINT8 *)s;
    UINT8 c;
    UINT32 cp;

    if (!s) { if (outCp) *outCp = 0; return 0; }
    c = p[0];

    if (c < 0x80) { if (outCp) *outCp = c; return 1; }

    if ((c & 0xE0) == 0xC0) {
        if ((p[1] & 0xC0) != 0x80) goto bad;
        cp = ((UINT32)(c & 0x1F) << 6) | (UINT32)(p[1] & 0x3F);
        if (cp < 0x80) goto bad;                  /* 过长编码：不接受 */
        if (outCp) *outCp = cp;
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) goto bad;
        cp = ((UINT32)(c & 0x0F) << 12) | ((UINT32)(p[1] & 0x3F) << 6) | (UINT32)(p[2] & 0x3F);
        if (cp < 0x800) goto bad;
        if (outCp) *outCp = cp;
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) goto bad;
        cp = ((UINT32)(c & 0x07) << 18) | ((UINT32)(p[1] & 0x3F) << 12)
           | ((UINT32)(p[2] & 0x3F) << 6)  | (UINT32)(p[3] & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF) goto bad;
        if (outCp) *outCp = cp;
        return 4;
    }

bad:
    if (outCp) *outCp = 0xFFFD;   /* U+FFFD REPLACEMENT CHARACTER */
    return 1;
}

/* 一个码点占几个字符格。
 *   0 = 组合字符（叠在前一个字符上）
 *   1 = 半角
 *   2 = 全角 / 东亚宽字符
 *
 * 这是"东亚宽度"（East Asian Width）的实用子集，不是完整的 UAX #11 表 ——
 * 完整表有两千多项，而控制台真正会遇到的就是下面这些区间。
 * 表错了的症状是"中文排版错位一格"，很好发现，所以这里宁可小而准。 */
int t_char_width(UINT32 cp) {
    if (cp == 0) return 0;

    /* --- 零宽 --- */
    if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF) return 0;

    /* --- 组合记号（占 0 格）--- */
    if ((cp >= 0x0300  && cp <= 0x036F) ||    /* 组合附加符号 */
        (cp >= 0x0483  && cp <= 0x0489) ||
        (cp >= 0x0591  && cp <= 0x05BD) ||
        (cp >= 0x0610  && cp <= 0x061A) ||
        (cp >= 0x064B  && cp <= 0x065F) ||
        (cp >= 0x0E31  && cp <= 0x0E3A) ||
        (cp >= 0x0E47  && cp <= 0x0E4E) ||
        (cp >= 0x1AB0  && cp <= 0x1AFF) ||
        (cp >= 0x1DC0  && cp <= 0x1DFF) ||
        (cp >= 0x20D0  && cp <= 0x20FF) ||
        (cp >= 0xFE20  && cp <= 0xFE2F))
        return 0;

    /* --- 东亚宽 / 全角（占 2 格）--- */
    if ((cp >= 0x1100  && cp <= 0x115F) ||    /* 谚文字母 */
        (cp >= 0x2E80  && cp <= 0x303E) ||    /* CJK 部首补充、康熙部首、CJK 符号 */
        (cp >= 0x3041  && cp <= 0x33FF) ||    /* 平假名/片假名/注音/CJK 兼容 */
        (cp >= 0x3400  && cp <= 0x4DBF) ||    /* CJK 扩展 A */
        (cp >= 0x4E00  && cp <= 0x9FFF) ||    /* CJK 基本区（汉字主体）*/
        (cp >= 0xA000  && cp <= 0xA4CF) ||    /* 彝文 */
        (cp >= 0xAC00  && cp <= 0xD7A3) ||    /* 谚文音节 */
        (cp >= 0xF900  && cp <= 0xFAFF) ||    /* CJK 兼容表意 */
        (cp >= 0xFE10  && cp <= 0xFE19) ||    /* 竖排标点 */
        (cp >= 0xFE30  && cp <= 0xFE6F) ||    /* CJK 兼容形式 */
        (cp >= 0xFF00  && cp <= 0xFF60) ||    /* 全角 ASCII */
        (cp >= 0xFFE0  && cp <= 0xFFE6) ||    /* 全角符号 */
        (cp >= 0x1F300 && cp <= 0x1F64F) ||   /* 表情符号 */
        (cp >= 0x1F900 && cp <= 0x1F9FF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD))     /* CJK 扩展 B 及以后 */
        return 2;

    return 1;
}
