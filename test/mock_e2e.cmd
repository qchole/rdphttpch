@echo off
setlocal enableextensions
rem Full-chain loopback test without RDP:
rem   browser -> rdpchsrv proxy (127.0.0.1:18080)
rem            -> mock TCP carrier (127.0.0.1:19001)
rem            -> rdpchclihost relay core -> real target site
rem Requires outbound internet access from this machine.

set SRV=..\dist\x64\rdpchsrv.exe
set CLI=..\dist\x64\rdpchclihost.exe
set CARRIER_PORT=19001
set PROXY_PORT=18080

if not exist "%SRV%" ( echo error: build first ^(scripts\build.cmd^) & exit /b 1 )
if not exist "%CLI%" ( echo error: build first ^(scripts\build.cmd^) & exit /b 1 )

set RDPCH_LOG=warn

echo [1/5] starting rdpchsrv (mock carrier :%CARRIER_PORT%, proxy :%PROXY_PORT%)
start "rdpchsrv" /min "%SRV%" --mock-listen %CARRIER_PORT% --port %PROXY_PORT%
ping -n 3 127.0.0.1 >nul

echo [2/5] starting rdpchclihost
start "rdpchclihost" /min "%CLI%" --connect 127.0.0.1:%CARRIER_PORT%
ping -n 3 127.0.0.1 >nul

echo [3/5] CONNECT-tunnel test (https://example.com)
curl -s -x http://127.0.0.1:%PROXY_PORT% https://example.com -o nul -w "https status: %%{http_code}\n" || goto fail

echo [4/5] absolute-URI test (http://example.com)
curl -s -x http://127.0.0.1:%PROXY_PORT% http://example.com -o nul -w "http  status: %%{http_code}\n" || goto fail

echo [5/5] keep-alive pipelined requests (two requests, one connection)
curl -s -x http://127.0.0.1:%PROXY_PORT% http://example.com http://example.com/ -o nul -o nul -w "two-req status: %%{http_code}\n" || goto fail

echo.
echo ALL MOCK E2E TESTS PASSED
goto cleanup

:fail
echo.
echo MOCK E2E TEST FAILED (exit %errorlevel%)

:cleanup
taskkill /im rdpchclihost.exe /f >nul 2>&1
taskkill /im rdpchsrv.exe /f >nul 2>&1
endlocal
