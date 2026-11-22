@echo off
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio Installer not found.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    for /d %%d in ("%ProgramFiles(x86)%\Microsoft Visual Studio\*") do (
        if exist "%%d\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VSPATH=%%d\BuildTools"
    )
)
if not defined VSPATH (
    echo Visual Studio installation not found.
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

if not exist build mkdir build
set "PYTHON=python"
where python >nul 2>nul
if errorlevel 1 set "PYTHON=py -3"
%PYTHON% tools\gen_version.py build\finte_version.h
if errorlevel 1 exit /b 1

set "CLANGCL=%ProgramFiles%\LLVM\bin\clang-cl.exe"
if not exist "%CLANGCL%" set "CLANGCL=clang-cl"

"%CLANGCL%" /nologo /std:c11 /O2 /W3 /Isrc\include /Ibuild ^
    src\core\util.c src\core\object.c src\core\cap.c src\core\arena.c src\core\channel.c ^
    src\core\flow.c src\core\sched.c src\core\verb.c src\core\compat_linux.c ^
    src\core\demo.c src\core\guest_linux.c src\core\selftest.c src\core\initrd.c ^
    src\core\disk.c src\core\lfc1.c src\core\ext4.c src\core\ntfs.c src\core\file.c src\core\lock.c ^
    src\core\cas.c src\core\ansi.c src\core\watchdog.c src\core\bootinfo.c src\core\pmm.c ^
    src\core\process.c ^
    src\host\hal_host.c src\host\blk_host.c src\host\process_host.c ^
    src\host\main.c src\host\bench.c src\tests\test_main.c ^
    /Fe:build\finte.exe
if errorlevel 1 exit /b 1
echo Built build\finte.exe
