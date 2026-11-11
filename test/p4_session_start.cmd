@echo off
rem Run this INSIDE the RDP session (inside the mstsc window).
rem Refuses to start on the console desktop - there is no RDP channel there.
title rdpchsrv (RDP session)

if /i "%SESSIONNAME%"=="Console" (
    echo.
    echo  ERROR: you are on the CONSOLE desktop, not inside an RDP session.
    echo  Connect with mstsc first, then run this script inside that window.
    echo  ^(session name: %SESSIONNAME%^)
    echo.
    pause
    exit /b 1
)

echo session: %SESSIONNAME%
set RDPCH_LOG=D:\temp\rdpch_p4_srv.log
echo starting rdpchsrv inside this RDP session...
echo log file: %RDPCH_LOG%
D:\newProjects\rdpch\dist\x64\rdpchsrv.exe --port 18080
echo.
echo rdpchsrv exited (code %errorlevel%)
pause
