@echo off
rem Scarica PHP per Windows (Non Thread Safe, x64) nella cartella php accanto a SegVault.
rem Usa curl e tar, presenti in Windows 10 e 11.
setlocal
cd /d "%~dp0"
if exist php\php-cgi.exe (
  echo PHP is already in the php folder.
  goto :eof
)
for %%V in (8.5 8.4 8.3) do (
  echo Downloading PHP %%V ...
  curl -fL --progress-bar -o php.zip https://windows.php.net/downloads/releases/latest/php-%%V-nts-Win32-vs17-x64-latest.zip && goto :unzip
)
echo.
echo Could not download PHP. Get the "Non Thread Safe" x64 zip from https://windows.php.net/download/
echo and extract it into the php folder next to segvault.exe.
exit /b 1
:unzip
if not exist php mkdir php
tar -xf php.zip -C php
del php.zip
echo.
echo PHP is in the php folder. Start SegVault: it finds PHP by itself and turns on the usual
echo extensions (mysqli, pdo_mysql, openssl, mbstring, curl, gd, intl, zip). HTTPS uses the
echo OpenSSL that comes with PHP.
