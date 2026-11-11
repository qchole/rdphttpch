@echo off
setlocal enableextensions
rem Remove the mstsc add-in registration and the copied DLL.

set KEY=HKCU\Software\Microsoft\Terminal Server Client\Default\Addins\RDPCH

reg delete "%KEY%" /f >nul 2>&1

if exist "%LOCALAPPDATA%\rdpch\rdpchcli.dll" del /q "%LOCALAPPDATA%\rdpch\rdpchcli.dll"

echo mstsc add-in uninstalled (a running mstsc keeps the old DLL loaded
echo until it exits).
endlocal
exit /b 0
