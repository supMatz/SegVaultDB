# SegVault: eseguibili pronti

Basta scaricare **un solo file** (i font sono già dentro). La cartella dei dati viene creata
accanto, al primo avvio (`./data`, oppure quella indicata sulla riga di comando).

| Sistema | App desktop (editor SQL + server) | Solo server (HTTP/JSON e protocollo MySQL) |
|---|---|---|
| Windows 10 / 11 (64 bit) | [`segvault.exe`](windows-x86_64/segvault.exe) | [`segvault-server.exe`](windows-x86_64/segvault-server.exe) |
| Linux x86-64 | [`segvault`](linux-x86_64/segvault) | [`segvault-server`](linux-x86_64/segvault-server) |

- **Windows:** doppio clic su `segvault.exe` (se SmartScreen avvisa: *Ulteriori informazioni → Esegui
  comunque*: l'eseguibile non è firmato).
- **Linux:** `chmod +x segvault && ./segvault`. L'app desktop usa le librerie grafiche del sistema
  (X11, Xft, fontconfig: presenti in ogni desktop) e glibc 2.38 o più recente (Ubuntu 24.04+,
  Fedora 39+, Debian 13, Arch); su sistemi più vecchi si compila con `make`. Il server è statico e
  gira su qualunque Linux a 64 bit.

Esempi:

```bash
./segvault                                   # sceglie editor o server
./segvault --editor dati/                    # editor SQL, dati nella cartella dati/
./segvault-server --mysql-port 3306 dati/    # server: API su :8484 e protocollo MySQL su :3306
```

Gli eseguibili si rigenerano dai sorgenti con `make dist` (vedi il [README](../README.md)).
