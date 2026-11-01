@echo off
rem Klonoa: Empire of Dreams -- Dreamcast port: build a CDI from your own GBA ROM.
rem
rem Drag your Klonoa: Empire of Dreams (USA) ROM onto this file, or run:
rem     build-cdi.bat "C:\path\to\your rom.gba"
rem
rem The build itself runs in WSL (Windows Subsystem for Linux), with KallistiOS, its sh-elf
rem toolchain and mkdcdisc installed inside your WSL distribution: this file only hands the ROM to
rem build-cdi.sh there. Native Windows builds are not supported. See README.md, "Building a CDI".
setlocal EnableExtensions DisableDelayedExpansion

if "%~1"=="" (
    echo build-cdi: drag your Klonoa: Empire of Dreams ^(USA^) GBA ROM onto this file,
    echo or run:  build-cdi.bat "C:\path\to\your rom.gba"
    set RC=2
    goto :done
)
if not "%~2"=="" (
    echo build-cdi: error: drop one ROM file at a time.
    set RC=2
    goto :done
)
if not exist "%~f1" (
    echo build-cdi: error: no such file: "%~f1"
    set RC=2
    goto :done
)
if exist "%~f1\*" (
    echo build-cdi: error: "%~f1" is a folder, not a ROM file.
    set RC=2
    goto :done
)

rem WSL: wsl.exe must be on PATH (it is installed in System32) and have a Linux distribution.
set "WSL_EXE="
for %%W in (wsl.exe) do set "WSL_EXE=%%~$PATH:W"
if not defined WSL_EXE (
    echo build-cdi: error: WSL ^(Windows Subsystem for Linux^) was not found.
    echo   The Dreamcast toolchain runs under Linux. Install WSL ^("wsl --install" in an
    echo   administrator PowerShell^), then install KallistiOS, its toolchain and mkdcdisc inside
    echo   it as described in README.md, "Building a CDI".
    set RC=4
    goto :done
)
wsl.exe -e true >nul 2>nul
if errorlevel 1 (
    echo build-cdi: error: WSL is installed but no Linux distribution answers.
    echo   Install one ^("wsl --install -d Ubuntu"^), open it once to finish its setup, then
    echo   install KallistiOS, its toolchain and mkdcdisc inside it ^(README.md, "Building a CDI"^).
    set RC=4
    goto :done
)

rem Windows paths to WSL paths (wslpath handles spaces, drive letters and \\wsl$ shares).
set "ROM_WIN=%~f1"
set "SCRIPT_WIN=%~dp0build-cdi.sh"
set "ROM_WSL="
set "SCRIPT_WSL="
for /f "usebackq delims=" %%P in (`wsl.exe wslpath -a -u "%ROM_WIN%"`) do set "ROM_WSL=%%P"
for /f "usebackq delims=" %%P in (`wsl.exe wslpath -a -u "%SCRIPT_WIN%"`) do set "SCRIPT_WSL=%%P"
if not defined ROM_WSL goto :nowsl
if not defined SCRIPT_WSL goto :nowsl

echo build-cdi: running build-cdi.sh in WSL
wsl.exe bash "%SCRIPT_WSL%" "%ROM_WSL%"
set RC=%ERRORLEVEL%
goto :done

:nowsl
echo build-cdi: error: WSL could not translate the paths ^(wslpath^). Is the ROM on a drive WSL
echo   can see?
set RC=4

:done
echo.
if "%RC%"=="0" (echo build-cdi: finished.) else (echo build-cdi: failed with exit status %RC%.)
rem Keep the window open when started by drag and drop; set BUILD_CDI_NOPAUSE=1 to skip.
if not defined BUILD_CDI_NOPAUSE pause
exit /b %RC%
