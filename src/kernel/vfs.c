/* ============================================================================
 * TNDDOS -- virtual file system layer
 *
 * Underneath there is exactly one provider for now: the UEFI Simple File System
 * Protocol (FAT). What this layer does is translate DOS path semantics into
 * UEFI ones:
 *
 *   C:\EFI\TNDOS\KERNEL.EFI   ->  \EFI\TNDOS\KERNEL.EFI
 *   .\FOO.TXT                 ->  <current dir>\FOO.TXT
 *   ..\BAR                    ->  parent\BAR
 *   D:FOO                     ->  explicit drive (only C: is mounted today)
 *
 * Per the plan, this layer will later sit on TNDDOS's own FAT32 driver with the
 * interface unchanged.
 * ==========================================================================*/
#include "tnd.h"

#define TND_MAX_DRIVES 4
#define TND_MAX_COMPS  32

typedef struct {
    int   used;
    char  letter;
    char  label[16];
    EFI_FILE_PROTOCOL *root;
} TND_DRIVE;

static TND_DRIVE gDrives[TND_MAX_DRIVES];
static int  gDriveCount = 0;
static char gCwd[TND_MAX_PATH] = "\\";
static char gCurDrive = 'C';

const TND_MODULE_DEF gModVfs = { "vfs", "virtual file system / DOS path + drive mapping", vfs_init };

/* thin alias so the rest of the code keeps reading naturally */
const char *vfs_efi_error(EFI_STATUS s) { return t_status_str(s); }

int vfs_init(void) {
    if (!gEnv.Root) { log_kv("vfs", "no root handle, mount failed"); return 0; }
    gDrives[0].used = 1;
    gDrives[0].letter = 'C';
    t_strcpy(gDrives[0].label, "TNDDOS");
    gDrives[0].root = gEnv.Root;
    gDriveCount = 1;
    gCwd[0] = '\\'; gCwd[1] = 0;
    gCurDrive = 'C';
    log_puts("[log] vfs: mounted C: -> ESP root\r\n");
    return 1;
}

static TND_DRIVE *drive_of(char letter) {
    for (int i = 0; i < gDriveCount; i++)
        if (gDrives[i].used && gDrives[i].letter == letter) return &gDrives[i];
    return 0;
}

const char *vfs_cwd(void) { return gCwd; }
char vfs_drive(void) { return gCurDrive; }
void vfs_set_drive(char letter) {
    letter = (char)t_toupper((unsigned char)letter);
    if (drive_of(letter)) gCurDrive = letter;
}

static void path_normalize(const char *in, char *out, UINTN cap) {
    char comps[TND_MAX_COMPS][64];
    int n = 0;
    const char *p = in;

    while (*p && n < TND_MAX_COMPS) {
        while (*p == '\\' || *p == '/') p++;
        if (!*p) break;
        char c[64]; UINTN k = 0;
        while (*p && *p != '\\' && *p != '/' && k + 1 < sizeof(c)) c[k++] = *p++;
        c[k] = 0;
        while (*p && *p != '\\' && *p != '/') p++;
        if (!t_strcmp(c, ".")) continue;
        if (!t_strcmp(c, "..")) { if (n > 0) n--; continue; }
        t_strncpy(comps[n], c, sizeof(comps[0]));
        n++;
    }

    UINTN o = 0;
    if (o + 1 < cap) out[o++] = '\\';
    for (int i = 0; i < n; i++) {
        UINTN l = t_strlen(comps[i]);
        if (o + l + 2 >= cap) break;
        for (UINTN k = 0; k < l; k++) out[o++] = comps[i][k];
        if (i + 1 < n) out[o++] = '\\';
    }
    out[o] = 0;
    if (o == 0) { out[0] = '\\'; out[1] = 0; }
}

EFI_STATUS vfs_resolve(const char *dos, char *out, UINTN cap) {
    char base[TND_MAX_PATH];
    char letter = gCurDrive;

    while (*dos == ' ') dos++;
    if (dos[0] && dos[1] == ':') {
        letter = (char)t_toupper((unsigned char)dos[0]);
        dos += 2;
    }
    if (!drive_of(letter)) return EFI_NOT_FOUND;

    if (dos[0] == '\\' || dos[0] == '/') {
        t_strncpy(base, dos, sizeof(base));
    } else {
        t_strncpy(base, gCwd, sizeof(base));
        UINTN l = t_strlen(base);
        if (l && base[l - 1] != '\\' && l + 1 < sizeof(base)) { base[l] = '\\'; base[l + 1] = 0; }
        t_strncpy(base + t_strlen(base), dos, sizeof(base) - t_strlen(base));
    }

    path_normalize(base, out, cap);
    return EFI_SUCCESS;
}

EFI_STATUS vfs_open(const char *dos, int write, EFI_FILE_PROTOCOL **out) {
    char path[TND_MAX_PATH];
    CHAR16 wname[TND_MAX_PATH + 4];
    EFI_STATUS s;
    TND_DRIVE *d;

    if (out) *out = 0;
    s = vfs_resolve(dos, path, sizeof(path));
    if (EFI_ERROR(s)) return s;
    if (!t_strcmp(path, "\\")) return EFI_ACCESS_DENIED;   /* the root itself: use vfs_dir */

    d = drive_of(gCurDrive);
    if (!d || !d->root) return EFI_NOT_FOUND;

    t_ascii_to_u16(path, wname, TND_MAX_PATH + 4);
    s = d->root->Open(d->root, out, wname,
                      write ? (EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE) : EFI_FILE_MODE_READ, 0);
    return s;
}

/* 带 CREATE 的打开。给 TNX 程序的 TND_O_CREATE 用。 */
EFI_STATUS vfs_create(const char *dos, EFI_FILE_PROTOCOL **out) {
    char path[TND_MAX_PATH];
    CHAR16 wname[TND_MAX_PATH + 4];
    EFI_STATUS s;
    TND_DRIVE *d = drive_of(gCurDrive);

    if (out) *out = 0;
    if (!d || !d->root) return EFI_NOT_FOUND;
    s = vfs_resolve(dos, path, sizeof(path));
    if (EFI_ERROR(s)) return s;
    if (!t_strcmp(path, "\\")) return EFI_ACCESS_DENIED;

    t_ascii_to_u16(path, wname, TND_MAX_PATH + 4);
    return d->root->Open(d->root, out, wname,
                         EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
}

/* 截断成 0 字节。UEFI 没有 truncate，靠 SetInfo 把 FileSize 写成 0。 */
EFI_STATUS vfs_truncate(EFI_FILE_PROTOCOL *f) {
    static UINT8 info[512];
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)info;
    UINTN sz = sizeof(info);
    EFI_STATUS s;
    if (!f) return EFI_INVALID_PARAMETER;
    s = f->GetInfo(f, &gEfiFileInfoGuid, &sz, info);
    if (EFI_ERROR(s)) return s;
    fi->FileSize = 0;
    /* SetInfo 的参数不是 const（GetInfo 是），要显式转一下 */
    s = f->SetInfo(f, (EFI_GUID *)&gEfiFileInfoGuid, sz, info);
    if (!EFI_ERROR(s)) f->SetPosition(f, 0);
    return s;
}

int vfs_exists(const char *dos) {
    EFI_FILE_PROTOCOL *f = 0;
    if (EFI_ERROR(vfs_open(dos, 0, &f)) || !f) return 0;
    f->Close(f);
    return 1;
}

EFI_STATUS vfs_read_all(const char *dos, char *buf, UINTN cap, UINTN *len) {
    EFI_FILE_PROTOCOL *f = 0;
    EFI_STATUS s;
    UINTN total = 0;

    s = vfs_open(dos, 0, &f);
    if (EFI_ERROR(s) || !f) return s;
    for (;;) {
        UINTN want = 512;
        if (total + want >= cap) { want = cap - total - 1; if (!want) break; }
        s = f->Read(f, &want, buf + total);
        if (EFI_ERROR(s) || want == 0) break;
        total += want;
    }
    f->Close(f);
    buf[total] = 0;
    if (len) *len = total;
    if (total + 1 >= cap) {
        log_puts("[log] vfs_read_all: buffer full, file may be truncated: "); log_puts(dos); log_puts("\r\n");
        return EFI_BUFFER_TOO_SMALL;
    }
    return EFI_SUCCESS;
}

/* ---------------------------------------------------------------- DIR */
int vfs_dir(const char *dos) {
    char path[TND_MAX_PATH];
    EFI_FILE_PROTOCOL *dir = 0, *target = 0;
    TND_DRIVE *d = drive_of(gCurDrive);
    void *buf = 0;
    UINTN isz = 0;
    EFI_STATUS s;
    UINT64 files = 0, dirs = 0, bytes = 0;
    int closedDir = 0;
    (void)dir;

    if (!d) return 0;
    s = vfs_resolve(dos, path, sizeof(path));
    if (EFI_ERROR(s)) { con_puts("  invalid path\r\n"); return 0; }

    if (!t_strcmp(path, "\\")) {
        target = d->root;
    } else {
        s = vfs_open(dos, 0, &dir);
        if (EFI_ERROR(s) || !dir) {
            con_puts("  cannot open: "); con_puts(path); con_puts("  ("); con_puts(vfs_efi_error(s)); con_puts(")\r\n");
            return 0;
        }
        target = dir; closedDir = 1;
    }

    isz = 0;
    s = target->GetInfo(target, &gEfiFileInfoGuid, &isz, 0);
    if (s != EFI_BUFFER_TOO_SMALL && EFI_ERROR(s)) isz = 4096;
    isz += 256;
    if (EFI_ERROR(gEnv.BS->AllocatePool(EfiLoaderData, isz, &buf)) || !buf) {
        con_puts("  out of memory\r\n");
        if (closedDir) dir->Close(dir);
        return 0;
    }

    con_puts("\r\n Volume "); con_putc(gCurDrive); con_puts(":  Directory of ");
    con_puts(path); con_puts("\r\n\r\n");

    /* The read position MUST be rewound before enumerating.
     * EDK2's FAT driver leaves the position at EOF after a full enumeration, so
     * the next Read returns length 0 -- which showed up as "second DIR of the
     * volume root comes back empty". Non-root directories open a fresh handle
     * every time, so only the reused volume root handle was affected. */
    if (target->SetPosition) target->SetPosition(target, 0);

    for (;;) {
        UINTN sz = isz;
        s = target->Read(target, &sz, buf);
        if (EFI_ERROR(s) || sz == 0) break;
        {
            EFI_FILE_INFO *fi = (EFI_FILE_INFO *)buf;
            char name[TND_MAX_PATH];
            UINTN k = 0;
            while (fi->FileName[k] && k + 1 < sizeof(name)) { name[k] = (char)(fi->FileName[k] & 0xFF); k++; }
            name[k] = 0;
            if (!name[0]) continue;
            /* classic DOS DIR does not list . or .. */
            if (!t_strcmp(name, ".") || !t_strcmp(name, "..")) continue;

            if (fi->Attribute & EFI_FILE_DIRECTORY) {
                dirs++;
                con_puts("   <DIR>          ");
                t_upper(name);
                con_puts(name); con_puts("\r\n");
            } else {
                files++; bytes += fi->FileSize;
                con_puts("   ");
                char nb[24]; t_utoa(fi->FileSize, nb);
                for (UINTN i = t_strlen(nb); i < 10; i++) con_puts(" ");
                con_puts(nb); con_puts("  ");
                if (fi->ModificationTime.Year > 1970) {
                    char yb[8]; t_utoa(fi->ModificationTime.Year, yb);
                    con_puts(yb); con_puts("-");
                    if (fi->ModificationTime.Month < 10) con_puts("0");
                    con_u64(fi->ModificationTime.Month); con_puts("-");
                    if (fi->ModificationTime.Day < 10) con_puts("0");
                    con_u64(fi->ModificationTime.Day); con_puts(" ");
                    if (fi->ModificationTime.Hour < 10) con_puts("0");
                    con_u64(fi->ModificationTime.Hour); con_puts(":");
                    if (fi->ModificationTime.Minute < 10) con_puts("0");
                    con_u64(fi->ModificationTime.Minute); con_puts("  ");
                } else {
                    con_puts("                   ");
                }
                t_upper(name);
                con_puts(name); con_puts("\r\n");
            }
        }
    }

    con_puts("\r\n   "); con_u64(files); con_puts(" file(s)   ");
    con_u64(bytes); con_puts(" bytes     "); con_u64(dirs); con_puts(" dir(s)\r\n");

    gEnv.BS->FreePool(buf);
    if (closedDir) dir->Close(dir);
    log_kv_u64("dir.files", files);
    log_kv_u64("dir.dirs", dirs);
    return 1;
}

int vfs_type(const char *dos) {
    static char buf[TND_FILE_CAP];
    UINTN len = 0;
    EFI_STATUS s = vfs_read_all(dos, buf, TND_FILE_CAP, &len);
    if (EFI_ERROR(s)) {
        con_puts("  cannot read "); con_puts(dos); con_puts("  ("); con_puts(vfs_efi_error(s)); con_puts(")\r\n");
        return 0;
    }
    con_puts(buf);
    if (len && buf[len - 1] != '\n') con_puts("\r\n");
    log_kv_u64("type.bytes", len);
    return 1;
}

int vfs_chdir(const char *dos) {
    char path[TND_MAX_PATH];
    TND_DRIVE *d;
    EFI_STATUS s;

    while (*dos == ' ') dos++;
    if (dos[0] && dos[1] == ':') {
        char letter = (char)t_toupper((unsigned char)dos[0]);
        if (!drive_of(letter)) { con_puts("  invalid drive\r\n"); return 0; }
        gCurDrive = letter;
        d = drive_of(letter);
        if (!dos[2]) { gCwd[0] = '\\'; gCwd[1] = 0; return 1; }
    }
    d = drive_of(gCurDrive);
    if (!d) return 0;

    s = vfs_resolve(dos, path, sizeof(path));
    if (EFI_ERROR(s)) { con_puts("  invalid path\r\n"); return 0; }

    if (!t_strcmp(path, "\\")) { t_strncpy(gCwd, "\\", TND_MAX_PATH); return 1; }

    {
        EFI_FILE_PROTOCOL *f = 0;
        CHAR16 wname[TND_MAX_PATH + 4];
        t_ascii_to_u16(path, wname, TND_MAX_PATH + 4);
        s = d->root->Open(d->root, &f, wname, EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(s) || !f) {
            con_puts("  no such directory: "); con_puts(path); con_puts("\r\n");
            return 0;
        }
        /* make sure it really is a directory */
        {
            static UINT8 info[256];
            UINTN sz = sizeof(info);
            EFI_STATUS gs = f->GetInfo(f, &gEfiFileInfoGuid, &sz, info);
            EFI_FILE_INFO *fi = (EFI_FILE_INFO *)info;
            if (EFI_ERROR(gs) || !(fi->Attribute & EFI_FILE_DIRECTORY)) {
                con_puts("  not a directory: "); con_puts(path); con_puts("\r\n");
                f->Close(f);
                return 0;
            }
        }
        f->Close(f);
    }

    t_strncpy(gCwd, path, TND_MAX_PATH);
    log_puts("[log] cd -> "); log_puts(gCwd); log_puts("\r\n");
    return 1;
}

/* ------------------------------------------------- directories / files */
static EFI_STATUS create_dir(const char *dos) {
    char path[TND_MAX_PATH];
    CHAR16 wname[TND_MAX_PATH + 4];
    EFI_FILE_PROTOCOL *f = 0;
    EFI_STATUS s;
    TND_DRIVE *d = drive_of(gCurDrive);
    if (!d) return EFI_NOT_FOUND;
    s = vfs_resolve(dos, path, sizeof(path));
    if (EFI_ERROR(s)) return s;
    t_ascii_to_u16(path, wname, TND_MAX_PATH + 4);
    s = d->root->Open(d->root, &f, wname,
                      EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                      EFI_FILE_DIRECTORY);
    if (!EFI_ERROR(s) && f) f->Close(f);
    return s;
}

int vfs_mkdir(const char *dos) {
    EFI_STATUS s = create_dir(dos);
    if (EFI_ERROR(s)) { con_puts("  mkdir failed: "); con_puts(vfs_efi_error(s)); con_puts("\r\n"); return 0; }
    con_puts("  created directory "); con_puts(dos); con_puts("\r\n");
    return 1;
}

int vfs_rmdir(const char *dos) {
    EFI_FILE_PROTOCOL *f = 0;
    EFI_STATUS s = vfs_open(dos, 1, &f);
    if (EFI_ERROR(s) || !f) { con_puts("  cannot open: "); con_puts(vfs_efi_error(s)); con_puts("\r\n"); return 0; }
    s = f->Delete(f);
    if (EFI_ERROR(s)) { con_puts("  delete failed: "); con_puts(vfs_efi_error(s)); con_puts("\r\n"); f->Close(f); return 0; }
    con_puts("  removed directory "); con_puts(dos); con_puts("\r\n");
    return 1;
}

int vfs_unlink(const char *dos) {
    EFI_FILE_PROTOCOL *f = 0;
    EFI_STATUS s = vfs_open(dos, 1, &f);
    if (EFI_ERROR(s) || !f) { con_puts("  cannot open: "); con_puts(vfs_efi_error(s)); con_puts("\r\n"); return 0; }
    s = f->Delete(f);
    if (EFI_ERROR(s)) { con_puts("  delete failed: "); con_puts(vfs_efi_error(s)); con_puts("\r\n"); f->Close(f); return 0; }
    con_puts("  deleted "); con_puts(dos); con_puts("\r\n");
    return 1;
}

int vfs_copy(const char *a, const char *b) {
    EFI_FILE_PROTOCOL *f = 0;
    EFI_STATUS s;
    void *buf = 0;
    UINTN got = 0, total = 0;

    s = vfs_open(a, 0, &f);
    if (EFI_ERROR(s) || !f) { con_puts("  cannot open source: "); con_puts(vfs_efi_error(s)); con_puts("\r\n"); return 0; }

    if (EFI_ERROR(gEnv.BS->AllocatePool(EfiLoaderData, TND_BLOB_CAP, &buf)) || !buf) {
        f->Close(f); con_puts("  out of memory\r\n"); return 0;
    }
    for (;;) {
        UINTN want = 1024;
        s = f->Read(f, &want, (UINT8 *)buf + total);
        if (EFI_ERROR(s) || want == 0) break;
        total += want;
        if (total + 1024 > TND_BLOB_CAP) break;
    }
    f->Close(f);

    {
        char path[TND_MAX_PATH];
        CHAR16 wname[TND_MAX_PATH + 4];
        TND_DRIVE *d = drive_of(gCurDrive);
        EFI_FILE_PROTOCOL *o = 0;
        if (!d) { gEnv.BS->FreePool(buf); return 0; }
        if (EFI_ERROR(vfs_resolve(b, path, sizeof(path)))) { gEnv.BS->FreePool(buf); return 0; }
        t_ascii_to_u16(path, wname, TND_MAX_PATH + 4);
        s = d->root->Open(d->root, &o, wname,
                          EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
        if (EFI_ERROR(s) || !o) {
            con_puts("  cannot open target: "); con_puts(vfs_efi_error(s)); con_puts("\r\n");
            gEnv.BS->FreePool(buf); return 0;
        }
        got = total;
        s = o->Write(o, &got, buf);
        o->Close(o);
        if (EFI_ERROR(s) || got != total) {
            con_puts("  write failed: "); con_puts(vfs_efi_error(s)); con_puts("\r\n");
            gEnv.BS->FreePool(buf); return 0;
        }
    }
    gEnv.BS->FreePool(buf);
    con_puts("  copied "); con_puts(a); con_puts(" -> "); con_puts(b);
    con_puts("  ("); con_u64(total); con_puts(" bytes)\r\n");
    log_kv_u64("copy.bytes", total);
    return 1;
}

int vfs_rename(const char *a, const char *b) {
    /* UEFI has no rename, so it is copy-then-delete-source */
    if (!vfs_copy(a, b)) return 0;
    if (!vfs_unlink(a)) { con_puts("  !! copied, but the source could not be deleted\r\n"); return 0; }
    return 1;
}

void vfs_report(void) {
    for (int i = 0; i < gDriveCount; i++) {
        if (!gDrives[i].used) continue;
        con_puts("  ");
        con_putc(gDrives[i].letter);
        con_puts(":  ");
        con_puts(gDrives[i].label);
        con_puts("   (UEFI Simple File System / FAT)\r\n");
    }
    con_puts("  current directory: ");
    con_putc(gCurDrive);
    con_puts(":");
    con_puts(gCwd);
    con_puts("\r\n");
}
