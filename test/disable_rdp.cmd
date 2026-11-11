@echo off
rem Revert the RDP enablement done by enable_rdp.cmd (admin).
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Terminal Server" /v fDenyTSConnections /t REG_DWORD /d 1 /f
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Terminal Server" /v fDenyTSConnections
echo done> "%~dp0rdp_disabled.marker"
