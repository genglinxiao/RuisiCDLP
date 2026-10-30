@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "ROOT=C:\RuisiUsbDlpLab"
set "LOG=%ROOT%\xp-lab-test.log"
set "RESULT=%ROOT%\xp-lab-result.txt"

>"%LOG%" echo Ruisi USB DLP XP lab test started %DATE% %TIME%
>"%RESULT%" echo status=running

echo ==== Network before configuration ====>>"%LOG%"
ipconfig.exe /all >>"%LOG%" 2>&1
netsh interface ip show config >>"%LOG%" 2>&1
netsh interface ip set address name="Local Area Connection" static 192.168.77.10 255.255.255.0 none >>"%LOG%" 2>&1
set "NETWORK_RC=%ERRORLEVEL%"
echo guest_network_config_exit=%NETWORK_RC% >>"%RESULT%"
netsh firewall add portopening protocol=TCP port=5080 name="Ruisi USB DLP Lab" mode=ENABLE scope=SUBNET >>"%LOG%" 2>&1
set "FIREWALL_RC=%ERRORLEVEL%"
echo guest_firewall_config_exit=%FIREWALL_RC% >>"%RESULT%"
echo ==== Network after configuration ====>>"%LOG%"
ipconfig.exe /all >>"%LOG%" 2>&1

call "%ROOT%\server\install-server.cmd" >>"%LOG%" 2>&1
set "SERVER_RC=%ERRORLEVEL%"
echo server_install_exit=%SERVER_RC% >>"%RESULT%"

ping.exe -n 4 127.0.0.1 >nul
call "%ROOT%\client\install-client.cmd" >>"%LOG%" 2>&1
set "CLIENT_RC=%ERRORLEVEL%"
echo client_install_exit=%CLIENT_RC% >>"%RESULT%"

"%ROOT%\tests\usb_dlp_legacy_policy_tests.exe" >>"%LOG%" 2>&1
set "POLICY_RC=%ERRORLEVEL%"
echo policy_test_exit=%POLICY_RC% >>"%RESULT%"
"%ROOT%\tests\usb_dlp_legacy_store_tests.exe" >>"%LOG%" 2>&1
set "STORE_RC=%ERRORLEVEL%"
echo secure_store_test_exit=%STORE_RC% >>"%RESULT%"

ping.exe -n 16 127.0.0.1 >nul
sc.exe query UsbDlp.LegacyServer >>"%LOG%" 2>&1
set "SERVER_SERVICE_RC=%ERRORLEVEL%"
echo server_service_query_exit=%SERVER_SERVICE_RC% >>"%RESULT%"
sc.exe query UsbDlp.LegacyAgent >>"%LOG%" 2>&1
set "AGENT_SERVICE_RC=%ERRORLEVEL%"
echo agent_service_query_exit=%AGENT_SERVICE_RC% >>"%RESULT%"
"%ProgramFiles%\Ruisi USB DLP Legacy Server\UsbDlp.LegacyServer.exe" --workstations >>"%LOG%" 2>&1
set "SERVER_QUERY_RC=%ERRORLEVEL%"
echo server_workstations_exit=%SERVER_QUERY_RC% >>"%RESULT%"

set "OUTCOME=pass"
if not "%NETWORK_RC%"=="0" set "OUTCOME=fail"
if not "%SERVER_RC%"=="0" set "OUTCOME=fail"
if not "%CLIENT_RC%"=="0" set "OUTCOME=fail"
if not "%POLICY_RC%"=="0" set "OUTCOME=fail"
if not "%STORE_RC%"=="0" set "OUTCOME=fail"
if not "%SERVER_SERVICE_RC%"=="0" set "OUTCOME=fail"
if not "%AGENT_SERVICE_RC%"=="0" set "OUTCOME=fail"
if not "%SERVER_QUERY_RC%"=="0" set "OUTCOME=fail"
echo outcome=%OUTCOME% >>"%RESULT%"
echo status=complete >>"%RESULT%"
echo Ruisi USB DLP XP lab test completed %DATE% %TIME%>>"%LOG%"
exit /b 0
