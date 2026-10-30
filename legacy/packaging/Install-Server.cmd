@echo off
setlocal EnableExtensions DisableDelayedExpansion

net session >nul 2>&1
if errorlevel 1 (
  echo This installer must be run as Administrator.
  exit /b 5
)

set "KIT=%~dp0"
set "ROOT=%KIT%..\"
set "MSI="
for %%F in ("%KIT%*.msi") do if not defined MSI set "MSI=%%~fF"
set "TOKEN=%ROOT%config\enrollment-token.txt"
set "PASSWORD=%ROOT%config\admin-password.txt"
set "CONFIG=%ROOT%config\deployment.ini"
if not defined MSI (
  echo Missing RuisiUsbDlpServer profile MSI in %KIT%
  exit /b 2
)
if not exist "%MSI%" (
  echo Missing %MSI%
  exit /b 2
)
if not exist "%TOKEN%" (
  echo Missing %TOKEN%
  exit /b 2
)
if not exist "%PASSWORD%" (
  echo Missing %PASSWORD%
  exit /b 2
)
if not exist "%CONFIG%" (
  echo Missing %CONFIG%
  exit /b 2
)

findstr /r /c:"................................" "%TOKEN%" >nul
if errorlevel 1 (
  echo enrollment-token.txt must contain at least 32 characters.
  exit /b 3
)
findstr /r /c:".............." "%PASSWORD%" >nul
if errorlevel 1 (
  echo admin-password.txt must contain at least 14 characters.
  exit /b 3
)

set "PORT=5080"
for /f "usebackq tokens=1,* delims==" %%A in ("%CONFIG%") do if /i "%%A"=="Port" set "PORT=%%B"
msiexec.exe /i "%MSI%" /qn /norestart
if errorlevel 1 (
  echo Server MSI installation failed.
  exit /b 4
)

rem A same-version repair may leave the service running with its previous configuration.
sc.exe stop UsbDlp.LegacyServer >nul 2>&1

if exist "%ProgramFiles(x86)%" (
  if exist "%ProgramFiles%\Ruisi USB DLP Legacy Server\UsbDlp.LegacyServer.exe" (
    set "SERVER=%ProgramFiles%\Ruisi USB DLP Legacy Server\UsbDlp.LegacyServer.exe"
  ) else (
    set "SERVER=%ProgramFiles(x86)%\Ruisi USB DLP Legacy Server\UsbDlp.LegacyServer.exe"
  )
) else (
  set "SERVER=%ProgramFiles%\Ruisi USB DLP Legacy Server\UsbDlp.LegacyServer.exe"
)

echo Configuring the server data directory with Unicode Windows APIs.
"%SERVER%" --configure "%TOKEN%" "%PASSWORD%" "%PORT%"
if errorlevel 1 (
  echo Could not configure the server data directory shown above.
  echo Windows XP requires this directory to be on an NTFS volume.
  exit /b 5
)

for /l %%N in (1,1,15) do (
  sc.exe start UsbDlp.LegacyServer >nul 2>&1
  if not errorlevel 1 goto server_started
  ping.exe -n 2 127.0.0.1 >nul
)
echo Server service could not restart.
echo Startup diagnostics:
"%SERVER%" --workstations
sc.exe query UsbDlp.LegacyServer
exit /b 6

:server_started
echo Ruisi USB DLP Legacy Server installed on HTTP port %PORT%.
echo This compatibility build permits HTTP only for an isolated test network.
exit /b 0
