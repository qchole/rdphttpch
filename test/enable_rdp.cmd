@echo off
rem Enable RDP for the P4 loopback test (run as administrator).
rem Loopback mstsc connections do not traverse the firewall, so no
rem inbound rule is added.  disable_rdp.cmd reverts this change.
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Terminal Server" /v fDenyTSConnections /t REG_DWORD /d 0 /f
net start TermService 2>nul
sc query TermService | find "STATE"
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Terminal Server" /v fDenyTSConnections
echo done> "%~dp0rdp_enabled.marker"
