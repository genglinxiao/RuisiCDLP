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
set "CONFIG=%ROOT%config\deployment.ini"
if not defined MSI (
  echo Missing RuisiUsbDlpAgent profile MSI in %KIT%
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
if not exist "%CONFIG%" (
  echo Missing %CONFIG%
  exit /b 2
)

set "SERVERURL="
set "DEBUGLOG=0"
for /f "usebackq tokens=1,* delims==" %%A in ("%CONFIG%") do if /i "%%A"=="ServerUrl" set "SERVERURL=%%B"
for /f "usebackq tokens=1,* delims==" %%A in ("%CONFIG%") do if /i "%%A"=="DebugLog" set "DEBUGLOG=%%B"
if not defined SERVERURL (
  echo deployment.ini does not define ServerUrl.
  exit /b 3
)
if not "%DEBUGLOG%"=="1" set "DEBUGLOG=0"

findstr /r /c:"................................" "%TOKEN%" >nul
if errorlevel 1 (
  echo enrollment-token.txt must contain at least 32 characters.
  exit /b 3
)

msiexec.exe /i "%MSI%" /qn /norestart
if errorlevel 1 (
  echo Agent MSI installation failed.
  exit /b 4
)

rem A same-version repair may leave the service running with its previous configuration.
sc.exe stop UsbDlp.LegacyAgent >nul 2>&1

if exist "%ProgramFiles(x86)%" (
  if exist "%ProgramFiles%\Ruisi USB DLP Legacy\UsbDlp.LegacyAgent.exe" (
    set "AGENT=%ProgramFiles%\Ruisi USB DLP Legacy\UsbDlp.LegacyAgent.exe"
  ) else (
    set "AGENT=%ProgramFiles(x86)%\Ruisi USB DLP Legacy\UsbDlp.LegacyAgent.exe"
  )
) else (
  set "AGENT=%ProgramFiles%\Ruisi USB DLP Legacy\UsbDlp.LegacyAgent.exe"
)

echo Configuring the agent data directory with Unicode Windows APIs.
"%AGENT%" --configure "%TOKEN%" "%SERVERURL%" "%DEBUGLOG%"
if errorlevel 1 (
  echo Could not configure the agent data directory shown above.
  echo Windows XP requires this directory to be on an NTFS volume.
  exit /b 5
)

for /l %%N in (1,1,15) do (
  sc.exe start UsbDlp.LegacyAgent >nul 2>&1
  if not errorlevel 1 goto agent_started
  ping.exe -n 2 127.0.0.1 >nul
)
echo Agent service could not restart. Check legacy-agent.log in the data directory printed above.
exit /b 6

:agent_started
echo Ruisi USB DLP Legacy Agent installed in live default-deny mode.
exit /b 0
