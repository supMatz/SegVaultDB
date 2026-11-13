@echo off
rem Rende fidata la CA locale di SegVault (https://NOME.localhost:8443 senza avvisi in Chrome ed Edge).
cd /d "%~dp0"
segvault-server.exe --trust-ca data
pause
