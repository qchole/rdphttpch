@echo off
setlocal enableextensions
rem Install the mstsc add-in DLL (client half).
rem   install-client.cmd        -> x64 (default, matches 64-bit mstsc)
rem   install-client.cmd /x86   -> x86 (for 32-bit mstsc)
rem The Addins key is shared by both bitnesses: installing one
rem registration replaces the other.

set ARCH=x64
if /i "%~1"=="/x86" set ARCH=x86

set SRC=%~dp0..\dist\%ARCH%\rdpchcli.dll
if not exist "%SRC%" (
    echo error: %SRC% not found - run scripts\build.cmd first
    exit /b 1
)

set DSTDIR=%LOCALAPPDATA%\rdpch
if not exist "%DSTDIR%" mkdir "%DSTDIR%" || exit /b 1

copy /y "%SRC%" "%DSTDIR%\rdpchcli.dll" >nul || exit /b 1

set KEY=HKCU\Software\Microsoft\Terminal Server Client\Default\Addins\RDPCH
rem "Name" is what recent mstsc builds (26100+, verified with procmon)
rem actually query; "DllName" is the documented value for older builds.
rem Register both so one install works everywhere.
reg add "%KEY%" /v Name /t REG_SZ /d "%DSTDIR%\rdpchcli.dll" /f || exit /b 1
reg add "%KEY%" /v DllName /t REG_SZ /d "%DSTDIR%\rdpchcli.dll" /f || exit /b 1

echo installed %ARCH% DLL for mstsc:
reg query "%KEY%"
echo note: start a NEW mstsc connection to load the add-in.
endlocal
exit /b 0
