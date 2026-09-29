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
echo   --- TNX: just type the program name, like DOS ---
HELLO.TNX
echo.
echo   --- extension is optional ---
HELLO
echo.
echo   --- TNX <file> inspects, does not run ---
tnx HELLO.TNX
echo.
echo   --- unknown program name ---
NOPE.TNX
echo.
echo   --- Memory self-test ---
memtest
echo.
echo   ------------------------------------------
echo   Boot complete. Type HELP for commands.
echo.
