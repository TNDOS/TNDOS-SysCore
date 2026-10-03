@echo off
REM ==========================================================================
REM TNDDOS boot script -- autoexec.bat
REM Executed by the Shell as the last step of the boot chain.
REM ==========================================================================
echo.
echo   TNDDOS autoexec.bat starting
echo   ------------------------------------------
ver
mem
echo.
echo   --- File system ---
dir
cd \EFI\TNDOS
echo.
echo   --- Relative path test (cd .. then back) ---
cd ..
cd TNDOS
echo.
echo   --- TYPE demo ---
type HELLO.TXT
echo.
echo   --- Write ops (mkdir / copy / list / cleanup) ---
md TMP
copy HELLO.TXT TMP\COPY.TXT
dir TMP
del TMP\COPY.TXT
rd TMP
echo.
echo   --- TNX program: API v2 self-test ---
HELLO.TNX
echo.
echo   --- same program, with an argument (argv) ---
HELLO.TNX /verbose
echo.
echo   --- extension is optional ---
HELLO
echo.
echo   --- TNX <file> inspects, does not run ---
tnx HELLO.TNX
echo.
echo   --- external commands (TNDOS-Commands, TNX programs) ---
TREE
echo.
echo   --- EXECOM: typing a .EXE gets you a diagnosis, not an error ---
PEDEMO.EXE
DOSDEMO.EXE
echo.
echo   --- EXECOM also works without the extension ---
PEDEMO
echo.
echo   --- FIND / ATTRIB / FC ---
FIND DOS TNDOS.TXT
ATTRIB *.TNX
echo.
echo   --- MORE (paged) ---
MORE TNDOS.TXT
echo.
echo   --- unknown program name ---
NOPE.TNX
echo.
echo   --- TREE (nested) ---
TREE
echo.
echo   --- Memory self-test ---
memtest
echo.
echo   ------------------------------------------
echo   Boot complete. Type HELP for commands.
echo.
echo   --- DIR with colour coding (dirs cyan, executables green) ---
DIR
echo.
echo   EDIT is interactive, so a batch file must not start it.
echo   Run it by hand:   EDIT TNDOS.TXT      F2 saves, ESC quits
echo.
echo   --- console backends ---
CONSOLE
echo.
echo   --- switching to the framebuffer console ---
CONSOLE fb
echo   If you can read this, TNDDOS is drawing the pixels itself.
echo   Type CONSOLE uefi to switch back.
DIR
echo.
echo   --- scale: 8x12 is too small on a 1280x800 panel ---
SF
echo   --- now at 200% (16x24 cells, a DOS-like grid) ---
SF 2
DIR
echo.
