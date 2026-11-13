@echo off
rem Tutto insieme senza finestra grafica: sito (cartella www), PHP, MySQL 3306, posta 1025, HTTPS 8443.
cd /d "%~dp0"
if not exist php\php-cgi.exe (
  echo PHP not found: run get-php.cmd first ^(or install PHP and add it to the PATH^).
  segvault-server.exe --static www --mysql-port 3306 --mail-port 1025 --https-port 8443 data
) else (
  segvault-server.exe --static www --php auto --mysql-port 3306 --mail-port 1025 --https-port 8443 data
)
pause
