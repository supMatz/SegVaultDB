<?php
// Sito d'esempio di SegVault: ogni cartella di www è un sito (http://NOME.localhost:8484).
$db = null; $err = ''; $count = null;
try {
    $db = new PDO('mysql:host=127.0.0.1;port=3306', 'root', '');
    $db->exec('CREATE DATABASE IF NOT EXISTS hello');
    $db->exec('USE hello');
    $db->exec('CREATE TABLE IF NOT EXISTS visits (id INT AUTO_INCREMENT PRIMARY KEY, at DATETIME DEFAULT CURRENT_TIMESTAMP)');
    $db->exec('INSERT INTO visits () VALUES ()');
    $count = $db->query('SELECT COUNT(*) FROM visits')->fetchColumn();
} catch (Throwable $e) { $err = $e->getMessage(); $db = null; }
?><!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Hello from SegVault</title><style>body{font:16px/1.5 system-ui,sans-serif;max-width:640px;margin:40px auto;padding:0 16px}
code{background:#eee;padding:1px 5px;border-radius:4px}@media(prefers-color-scheme:dark){body{background:#111;color:#ddd}code{background:#333}}</style></head>
<body><h1>It works</h1>
<p>PHP <?= PHP_VERSION ?> on <?= htmlspecialchars($_SERVER['SERVER_SOFTWARE']) ?><?= ($_SERVER['HTTPS'] ?? '') === 'on' ? ' (HTTPS)' : '' ?>.</p>
<?php if ($db): ?><p>Database: this page has been opened <b><?= (int)$count ?></b> times (table <code>hello.visits</code>).</p>
<?php else: ?><p>Database not reachable: <?= htmlspecialchars($err) ?>. Turn on the MySQL protocol (port 3306) in SegVault.</p><?php endif ?>
<p>Make a folder in <code>www</code> and it becomes <code>http://folder.localhost:8484</code>. All sites, the mail catcher and the
services: <a href="http://localhost:8484/.segvault/">localhost:8484/.segvault</a>.</p></body></html>
