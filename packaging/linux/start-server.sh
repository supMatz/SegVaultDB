#!/bin/sh
# Tutto insieme senza finestra grafica: sito (cartella www), PHP, MySQL 3306, posta 1025, HTTPS 8443.
cd "$(dirname "$0")"
PHP=""
command -v php-cgi >/dev/null 2>&1 || [ -x php/php-cgi ] && PHP="--php auto"
[ -z "$PHP" ] && echo "php-cgi not found: install it (Debian/Ubuntu: sudo apt install php-cgi php-mysql; Fedora: sudo dnf install php-cli php-mysqlnd; Arch: sudo pacman -S php-cgi)"
exec ./segvault-server --static www $PHP --mysql-port 3306 --mail-port 1025 --https-port 8443 data
