@echo off
rem Create a dedicated local test account for the RDP loopback test
rem (run as administrator).  A DIFFERENT account avoids the
rem "cannot connect to another console session" conflict.
net user rdp_test Rdpch#2026 /add /fullname:"rdpch test" /passwordchg:no
net localgroup Administrators rdp_test /add
net localgroup "Remote Desktop Users" rdp_test /add 2>nul
net user rdp_test
echo done> "%~dp0rdp_account.marker"
