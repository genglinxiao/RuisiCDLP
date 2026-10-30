@echo off
setlocal EnableExtensions DisableDelayedExpansion

net session >nul 2>&1
if errorlevel 1 (
  echo This command must be run as Administrator.
  exit /b 5
)

if "%~1"=="" goto usage

set "SERVER="
if exist "%ProgramFiles%\Ruisi USB DLP Legacy Server\UsbDlp.LegacyServer.exe" set "SERVER=%ProgramFiles%\Ruisi USB DLP Legacy Server\UsbDlp.LegacyServer.exe"
if not defined SERVER if defined ProgramFiles(x86) if exist "%ProgramFiles(x86)%\Ruisi USB DLP Legacy Server\UsbDlp.LegacyServer.exe" set "SERVER=%ProgramFiles(x86)%\Ruisi USB DLP Legacy Server\UsbDlp.LegacyServer.exe"
if not defined SERVER (
  echo Ruisi USB DLP Legacy Server is not installed.
  exit /b 2
)

rem The CLI updates encrypted persisted state. Restarting the service makes the change live.
sc.exe stop UsbDlp.LegacyServer >nul 2>&1
ping.exe -n 4 127.0.0.1 >nul

"%SERVER%" %*
set "COMMAND_RC=%ERRORLEVEL%"

for /l %%N in (1,1,15) do (
  sc.exe start UsbDlp.LegacyServer >nul 2>&1
  if not errorlevel 1 goto service_started
  ping.exe -n 2 127.0.0.1 >nul
)
echo Server command completed, but the service could not restart.
exit /b 6

:service_started
exit /b %COMMAND_RC%

:usage
echo Usage:
echo   admin-server.cmd --requests
echo   admin-server.cmd --workstations [search]
echo   admin-server.cmd --approve REQUEST_ID [DAYS]
echo   admin-server.cmd --deny REQUEST_ID
echo   admin-server.cmd --revoke REQUEST_ID
exit /b 1
