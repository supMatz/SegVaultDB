SegVault - local development server and database
=================================================

Start
  Windows : segvault.exe (window)   or  start-server.cmd (no window)
  Linux   : ./segvault               or  ./start-server.sh

What runs
  http://localhost:8484            your sites: every folder of "www" is a site,
                                   www/blog -> http://blog.localhost:8484 (Laravel: its public/)
  https://localhost:8443           the same over HTTPS + HTTP/2 (run trust-certificate.cmd /
                                   segvault-server --trust-ca once to remove browser warnings)
  127.0.0.1:3306                   MySQL protocol: PHP (mysqli, PDO), Node, Python, Java, the mysql client
                                   user root, empty password on this computer
  127.0.0.1:1025                   mail catcher: mail sent by your apps is kept and shown at
                                   http://localhost:8484/.segvault/mail/
  http://localhost:8484/.segvault  dashboard: services and sites

PHP
  Windows : run get-php.cmd once (downloads PHP into the php folder; extensions are turned on by
            themselves). Any other PHP in a php folder next to segvault.exe works too.
  Linux   : install php-cgi with the package manager.

Dev servers (Vite, Next.js, Django, Rails, ...)
  segvault-server --site app=http://127.0.0.1:5173   ->  http://app.localhost:8484 (WebSockets too)

Data
  The "data" folder holds the databases, users, certificates and caught mail. Back it up by copying
  it while SegVault is stopped, or with BACKUP TO 'folder' from SQL.

Documentation: docs/API.md in the source repository.
