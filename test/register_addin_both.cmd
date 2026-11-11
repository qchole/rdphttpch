@echo off
rem Register RDPCH mstsc addin in BOTH hives and BOTH value forms,
rem to isolate why mstsc does not load it from HKCU DllName.
rem (elevated - same user, so HKCU below is still the running user's)

set DLL=%LOCALAPPDATA%\rdpch\rdpchcli.dll

echo --- HKCU: add default (unnamed) value ---
reg add "HKCU\Software\Microsoft\Terminal Server Client\Default\Addins\RDPCH" /ve /t REG_SZ /d "%DLL%" /f

echo --- HKLM: create key with DllName + default value ---
reg add "HKLM\SOFTWARE\Microsoft\Terminal Server Client\Default\Addins\RDPCH" /v DllName /t REG_SZ /d "%DLL%" /f
reg add "HKLM\SOFTWARE\Microsoft\Terminal Server Client\Default\Addins\RDPCH" /ve /t REG_SZ /d "%DLL%" /f

echo.
echo --- verify ---
reg query "HKCU\Software\Microsoft\Terminal Server Client\Default\Addins\RDPCH"
reg query "HKLM\SOFTWARE\Microsoft\Terminal Server Client\Default\Addins\RDPCH"

echo done> "%~dp0rdp_reg_both.marker"
echo.
echo ALL DONE - you can close this window.
pause
