@echo off
rem Build rdpch (x64 all targets + x86 client DLL) using CMake's VS generator.
rem Locates CMake on PATH first, else via vswhere inside any VS install.
setlocal enabledelayedexpansion
cd /d "%~dp0.."

set "CMAKE="
where cmake >nul 2>nul && set "CMAKE=cmake"

if not defined CMAKE (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -property installationPath`) do (
            if exist "%%i\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
                set "CMAKE=%%i\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            )
        )
    )
)
if not defined CMAKE (
    echo [rdpch] cmake not found on PATH or inside Visual Studio 1>&2
    exit /b 1
)
echo [rdpch] using cmake: %CMAKE%

"%CMAKE%" -S . -B build\x64 -A x64
if errorlevel 1 exit /b 1
"%CMAKE%" --build build\x64 --config Release
if errorlevel 1 exit /b 1

"%CMAKE%" -S . -B build\x86 -A Win32
if errorlevel 1 exit /b 1
"%CMAKE%" --build build\x86 --config Release --target rdpchcli
if errorlevel 1 exit /b 1

rem unit tests (x64)
build\x64\Release\test_protocol.exe
if errorlevel 1 exit /b 1
build\x64\Release\test_http_rewrite.exe
if errorlevel 1 exit /b 1
build\x64\Release\test_conf.exe
if errorlevel 1 exit /b 1
build\x64\Release\test_vc.exe
if errorlevel 1 exit /b 1
build\x64\Release\test_backpressure.exe
if errorlevel 1 exit /b 1

rem stage artifacts
if not exist dist\x64 mkdir dist\x64
if not exist dist\x86 mkdir dist\x86
copy /y build\x64\Release\rdpchsrv.exe      dist\x64\ >nul
copy /y build\x64\Release\rdpchcli.dll      dist\x64\ >nul
copy /y build\x64\Release\rdpchclihost.exe  dist\x64\ >nul
copy /y build\x86\Release\rdpchcli.dll      dist\x86\ >nul

echo [rdpch] build OK - dist\x64 (srv, cli dll, clihost), dist\x86 (cli dll)
exit /b 0
