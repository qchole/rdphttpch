@echo off
rem Elevated: capture procmon trace of an mstsc launch + login, to see
rem whether mstsc reads the Addins registry key at all.
title rdpch procmon capture

echo killing current mstsc (test session)...
taskkill /im mstsc.exe /f >nul 2>&1

if exist D:\temp\pm.pml del D:\temp\pm.pml
if exist D:\temp\pm.csv del D:\temp\pm.csv
if exist "%~dp0pm_done.marker" del "%~dp0pm_done.marker"

echo starting procmon (200s capture window)...
D:\temp\procmon.exe /AcceptEula /Quiet /Minimized /BackingFile D:\temp\pm.pml

ping -n 11 127.0.0.1 >nul
echo launching mstsc...
set RDPCH_LOG=D:\temp\rdpch_dll.log
start "" "C:\Windows\System32\mstsc.exe" "D:\newProjects\rdpch\test\connect_223.rdp"

echo.
echo   *** NOW: log in to 10.10.4.223 in the mstsc window (you have ~3 min) ***
echo.

ping -n 201 127.0.0.1 >nul
echo stopping procmon...
D:\temp\procmon.exe /Terminate
ping -n 8 127.0.0.1 >nul

echo exporting csv (may take a while)...
D:\temp\procmon.exe /OpenLog D:\temp\pm.pml /SaveAs D:\temp\pm.csv /Quiet

echo done> "%~dp0pm_done.marker"
echo.
echo CAPTURE DONE - you can close this window.
pause
