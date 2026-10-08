# TNDOS-SysCore
TNDOS — a DOS defined from scratch.

---

## TNDDOS / 2NDDOS
**This is not "porting DOS to UEFI". It is making a DOS for the UEFI era.**

Keep DOS's interaction model (simple, direct, command-line first, `C:\` drive letters).
Drop DOS's memory model and its historical baggage (16-bit, BIOS, 640 KB, the DOS ABI).

- Platform: x86-64 + UEFI-only (ia32 / armv7 / armv8 / RISC-V / LoongArch are reserved, not implemented)
- No CSM, no legacy BIOS, no 16-bit real mode
- Executable formats: **PE as a stopgap**, **TNX v1**
- UEFI is still the platform we live on (file system, disks, keyboard are all borrowed from it), and we **do not cut that off early**

The item-by-item feature list is in [TNDOS-目标清单.txt](TNDOS-目标清单.txt) (Chinese)
and the version plan in [docs/ROADMAP.md](docs/ROADMAP.md) (Chinese).

### Repositories

| Repository | Contents |
|---|---|
| **TNDOS-SysCore** (this one) | The system itself: boot loader, kernel, shell, sample drivers |
| **TNDOS-SDK** | Headers / runtime / linker script / samples for writing TNX programs and drivers |
| **TNDOS-SysAPP** | Commands shipped with the system (TREE / FIND / FC / ATTRIB / MORE / EDIT …) |
| **TNDOS-ToolsKit** | Host tools: tnxpack (ELF64 to TNX), tnxdump (inspector), mkfat (FAT16 image) |
| **WebSrv** | Project website and version index |

The TNX format spec is [docs/TNX-SPEC.md](docs/TNX-SPEC.md) (frozen at v1.0).

Bug reports welcome.

---

## Read this before you try it

**These five are "this will waste your afternoon" notes, not a disclaimer.**

### 1. Machines without EFI — not hopeless, but a real pain

**Two hard limits:**

- A machine without EFI support **will not boot it**
- There is also **no CSM fallback** on x86 — TNDDOS needs real UEFI, and CSM gives you BIOS

On **x86**, you need a compatibility layer such as
[OpenCore](https://github.com/acidanthera/OpenCorePkg) (its OpenDuet branch keeps the
DUET components alive) to emulate UEFI in software.
**Not recommended unless you enjoy this sort of thing.**

If you have Hackintosh experience you can probably go straight at it —
**but we have not tested that path.** What we actually verified was
**QEMU + SeaBIOS + TianoCore DUET**:
<https://gitlab.com/77-0/tianocore_uefi_duet_installer/>

On **other architectures** without UEFI support you could try porting UEFI firmware
yourself. **Also not recommended unless you enjoy this sort of thing.**

### 2. Writing to real disks — read this one

The default `autoexec.bat` in this repo **writes to the real ESP on every boot**:

```
md TMP  /  copy HELLO.TXT TMP\COPY.TXT  /  del  /  rd TMP
```

The blast radius is small (it creates a temporary directory under `\EFI\TNDOS\`
and removes it again), but **it is a real write**.

**Comment those lines out before booting on real hardware.** This is second on the list
because it is the one most likely to bite when nothing else would.

### 3. Secure Boot

With Secure Boot enabled the unsigned `BOOTX64.EFI` **is rejected outright** —
the symptom is "nothing happened". Turn it off in firmware setup, or trust the binary yourself.

### 4. One program at a time, no memory isolation

Programs run in **ring 0**, physical addresses are virtual addresses, and there is no paging
and no protection.

**A crashing program takes the system down, and it does not fail gracefully.** That is not a
bug, it is the current design — real DOS was the same, and dropping DOS's memory model is
exactly what this project intends to do later.

### 5. Only ever verified on QEMU + OVMF

Real hardware is in a "**it works but has never been systematically tested**" state,
which is not the same as "supported".

It did boot on a real laptop once, and that was **safe** — but it was safe because the
feature surface was small at the time, not because we had any protection in place.

---

## Boot chain

```
UEFI Firmware
  -> \EFI\BOOT\BOOTX64.EFI     initialiser (roughly DOS's PBR, transient)
  -> \EFI\TNDOS\kernel.efi     kernel   (roughly IO.SYS)
  -> \EFI\TNDOS\efidos.sys     system config (installed, loaded first)
  -> \EFI\TNDOS\config.sys     user config
  -> Shell
  -> \EFI\TNDOS\autoexec.bat   boot script (runs last)
```

`efidos.sys` and `config.sys` share a syntax but differ in load order: the former is
system-level and written at install time, the latter belongs entirely to the user, so user
settings can override system defaults.

Typing `EXIT` in the shell returns from kernel.efi; the initialiser then cleans up and
exits — that is the measured meaning of "transient PBR".

---

## Layout

```
src/include/efi.h      hand-written UEFI definitions (offsets checked one by one against UEFI 2.10)
src/include/tnd.h      TNDDOS internal interfaces
src/include/tnx.h      TNX format definitions (shared by kernel and tools)
src/include/tnd_api.h  the kernel API contract available to TNX programs
src/lib/               log(serial) util file(ESP I/O) guid(globals) uni(Unicode)
src/lib/con_uefi.c     console backend: UEFI ConOut
src/lib/con_fb.c       console backend: framebuffer (writes its own pixels)
src/lib/fontvga.c      embedded 8x12 CP437 bitmap font (extracted from vgaoem.fon)
src/boot/bootx64.c     boot initialiser
src/kernel/kernel.c    kernel entry and boot orchestration
src/kernel/module.c    kernel module framework (register / ordered init / status report)
src/kernel/vfs.c       M2 virtual file system, DOS path semantics, drive mapping
src/kernel/pmm.c       M3 physical memory manager (bitmap)
src/kernel/heap.c      M3 kernel heap, kmalloc / kfree
src/kernel/drv.c       driver loader (DEVICE= -> LoadImage + StartImage)
src/kernel/tnx.c       TNX loader (validate / copy / call entry)
src/kernel/api.c       TNX program API table (v2.4) and handle table
src/kernel/conf.c      efidos.sys / config.sys parsing, environment variables
src/kernel/shell.c     shell, commands, autoexec.bat batch processing
src/drv/               driver SDK and sample drivers (VGA.EFI / KBD.EFI)
boot/                  boot-time config files deployed with the sources
tools/build-run.ps1    one-shot build + boot verification
build/                 build output (build/esp is the tree, build/esp.img the disk image)
```

**The TNX sample program, minimal runtime and linker script are not in this repository** —
they live in **TNDOS-SDK** and are taken directly from there at build time. `tnxpack`,
`tnxdump` and `mkfat` likewise live in **TNDOS-ToolsKit**.

(There used to be a copy under `src/tnx/` that silently drifted from the SDK — editing the
SDK did not affect the build and nothing warned you. A mirror that drifts is worse than an
explicit dependency, so the copy is gone and the build now depends on the SDK directly.)

---

## Build and verify

```powershell
\.\tools\build-run.ps1                       # compile -> build ESP -> make FAT16 image -> boot in QEMU/OVMF
\.\tools\build-run.ps1 -NoRun                # compile only
\.\tools\build-run.ps1 -Seconds 60 -Keys 'h,e,l,p,ret'   # inject keystrokes via the QEMU monitor
\.\tools\build-run.ps1 -Fat vvfat            # fall back to QEMU vvfat (writes crash it; for comparison only)
```

### Toolchain: everything goes through environment variables

The scripts **hard-code no machine-specific paths**. If a value is missing they probe
automatically (PATH, and QEMU's own `share\` directory), and if that fails they error out
and print a ready-to-paste `setx` command.

| Variable | Points at | Required |
|---|---|---|
| `TNDDOS_LLVM_BIN` | the **directory** containing `clang.exe` | yes (skippable if clang is on PATH) |
| `TNDDOS_QEMU` | full path to `qemu-system-x86_64.exe` | yes (skippable if qemu is on PATH) |
| `TNDDOS_OVMF_CODE` | OVMF code firmware `*.fd` | no, defaults to QEMU's `share\` |
| `TNDDOS_OVMF_VARS` | OVMF variable store template `*.fd` | no, same |

```powershell
# In practice you only set these two — OVMF is found in QEMU's own share directory
setx TNDDOS_LLVM_BIN "D:\LLVM\bin"
setx TNDDOS_QEMU     "D:\qemu\qemu-system-x86_64.exe"

# setx only affects new processes, so open a new terminal. Then confirm what was resolved:
\.\tools\build-run.ps1 -ShowEnv
```

`-ShowEnv` prints where all four paths actually landed without compiling or booting —
use it when tracking down environment problems.

Verification is **headless boot plus serial log**. `build\serial.log` contains both the
`[log]` diagnostic lines and the user-visible ConOut output (OVMF's console splitter sends
ConOut to the serial port as well).

### Why we build our own FAT16 image

QEMU's `-drive file=fat:rw:<dir>` (vvfat) reads fine but **kills QEMU outright on write-back**:

```
ERROR: block/vvfat.c:2429: commit_direntries: assertion failed: (mapping)
```

TNDDOS is a DOS; it cannot be tested against a file system you cannot write to. So
`tools/mkfat.ps1` builds a real FAT16 volume: MBR + one partition (type 0xEF) + two FATs +
a fixed root directory + a data area, 8.3 short names only (everything on our ESP is 8.3,
which removes the whole long-filename problem area).

---

## Further reading

All documents below are in Chinese.

| Document | Contents |
|---|---|
| [docs/STATUS.md](docs/STATUS.md) | Milestone progress and runtime self-checks |
| [docs/PITFALLS.md](docs/PITFALLS.md) | Bugs actually hit during development (all "compiled fine, only showed up at runtime") |
| [docs/DRIVERS.md](docs/DRIVERS.md) | Driver model (`DEVICE=`) semantics and conventions |
| [docs/CONSOLE.md](docs/CONSOLE.md) | Console character-set policy and the right path to localisation |
| [docs/TNX-SPEC.md](docs/TNX-SPEC.md) | **The TNX executable format specification** (v1.0, frozen) |
| [docs/TNX-DESIGN.md](docs/TNX-DESIGN.md) | TNX design trade-offs, v1 limits and anti-goals |
| [docs/TODO.md](docs/TODO.md) | Known issues, TODOs, what comes next |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Version roadmap |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Commit and code conventions |
| [CONTRIBUTORS.md](CONTRIBUTORS.md) | Contributors |
| [TNDOS-目标清单.txt](TNDOS-目标清单.txt) | Feature list (implemented / not, item by item) |

## Licence

See [LICENSE](LICENSE).
