@echo off
REM ---------------------------------------------------------------------------
REM SkyrimTogetherClaude build wrapper.
REM
REM Two things on this machine break a bare `xmake` invocation:
REM
REM 1. TOOLCHAIN. vswhere reports two Visual Studio installs. VS 18 Community is
REM    newer, so auto-detection picks it -- but its C++ workload has no headers
REM    (VC\Tools\MSVC\14.51.36231\include does not exist) and its vcvars64.bat
REM    sets INCLUDE/LIB empty. Builds fail with:
REM        fatal error C1083: Cannot open include file: 'stdarg.h'
REM    We force the complete VS 2022 BuildTools toolset (MSVC 14.44.35207,
REM    Windows SDK 10.0.26100.0) instead.
REM
REM 2. XMAKE VERSION. CI pins xmake 2.9.8 (.github/workflows/windows.yml) and
REM    the project declares set_xmakever("2.8.5"). winget only carries 3.x, a
REM    major version with breaking changes, so 2.9.8 is installed side-by-side
REM    at a fixed path and invoked absolutely. Local and CI must agree.
REM
REM Usage:  Tools\build-env.cmd f --arch=x64 --mode=releasedbg --yes
REM         Tools\build-env.cmd -y
REM         Tools\build-env.cmd install -o distrib
REM ---------------------------------------------------------------------------

setlocal

REM Git Bash exports NoDefaultCurrentDirectoryInExePath=1, and it survives into
REM cmd.exe. That switches off cmd's normal "search the current directory first"
REM behaviour, which DirectXTK's shader step depends on -- its custom build rule
REM invokes CompileShaders.cmd by bare name, so the build dies with:
REM     'CompileShaders.cmd' is not recognized as an internal or external command
REM     error MSB8066: Custom build for '...CompileShaders.cmd' exited with code 1
REM CI never hits this because it runs from PowerShell, where the variable is
REM unset. Clear it so local builds behave like CI.
set "NoDefaultCurrentDirectoryInExePath="

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "XMAKE=C:\Users\Bryce\Tools\xmake-2.9.8\xmake.exe"

REM NOTE: the paths below are echoed *quoted* on purpose. cmd expands %VAR% when
REM it parses an IF block, before evaluating the condition, so an unquoted
REM "C:\Program Files (x86)\..." closes the block at "(x86)" and the script dies
REM with "\Microsoft was unexpected at this time." -- even when the file exists.
if not exist "%VCVARS%" (
    echo ERROR: VS 2022 BuildTools not found at:
    echo   "%VCVARS%"
    echo Install the "Desktop development with C++" workload for VS 2022 BuildTools.
    exit /b 1
)

if not exist "%XMAKE%" (
    echo ERROR: xmake 2.9.8 not found at:
    echo   "%XMAKE%"
    echo Install it from:
    echo   https://github.com/xmake-io/xmake/releases/download/v2.9.8/xmake-v2.9.8.win64.exe
    echo using:  installer.exe /S /D=C:\Users\Bryce\Tools\xmake-2.9.8
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 (
    echo ERROR: vcvars64.bat failed.
    exit /b 1
)

REM Sanity check: a complete toolset always puts stdarg.h on INCLUDE. Use
REM `defined` rather than comparing "%INCLUDE%" -- the value contains "(x86)",
REM whose ")" would close the IF block early and break the parser.
if not defined INCLUDE (
    echo ERROR: INCLUDE is empty after vcvars64.bat -- this toolset has no headers.
    exit /b 1
)

cd /d "%~dp0.."

set XMAKE_ROOT=y
"%XMAKE%" %*
exit /b %errorlevel%
