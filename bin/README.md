# SegVault: eseguibili pronti

Basta scaricare **un solo file** (i font sono già dentro). La cartella dei dati viene creata
accanto, al primo avvio (`./data`, oppure quella indicata sulla riga di comando).

| Sistema | App desktop (editor SQL + server) | Solo server (HTTP/JSON e protocollo MySQL) |
|---|---|---|
| Windows 10 / 11 (64 bit) | [`segvault.exe`](https://github.com/supMatz/SegVaultDB/releases/latest/download/segvault.exe) | [`segvault-server.exe`](https://github.com/supMatz/SegVaultDB/releases/latest/download/segvault-server.exe) |
| Linux x86-64 | [`segvault`](https://github.com/supMatz/SegVaultDB/releases/latest/download/segvault) | [`segvault-server`](https://github.com/supMatz/SegVaultDB/releases/latest/download/segvault-server) (o [`segvault-server-static`](https://github.com/supMatz/SegVaultDB/releases/latest/download/segvault-server-static), senza HTTPS) |

I file sono allegati a ogni [release](https://github.com/supMatz/SegVaultDB/releases) (i link sopra
puntano all'ultima); il repository non contiene eseguibili. Pacchetti completi (eseguibili, script di
avvio, sito d'esempio, `get-php.cmd` per Windows): `SegVault-VERSIONE-windows-x86_64.zip` e
`SegVault-VERSIONE-linux-x86_64.tar.gz`. Ogni release allega anche `SHA256SUMS`: nella cartella dei
file scaricati, `sha256sum -c --ignore-missing SHA256SUMS` li verifica.

- **Windows:** doppio clic su `segvault.exe` (se SmartScreen avvisa: *Ulteriori informazioni → Esegui
  comunque*: l'eseguibile non è firmato).
- **Linux:** `chmod +x segvault && ./segvault`. L'app desktop usa le librerie grafiche del sistema
  (X11, Xft, fontconfig: presenti in ogni desktop) e glibc 2.38 o più recente (Ubuntu 24.04+,
  Fedora 39+, Debian 13, Arch); su sistemi più vecchi si compila con `make`. Anche `segvault-server`
  usa le librerie del sistema (carica OpenSSL all'avvio per HTTPS); `segvault-server-static` gira su
  qualunque Linux a 64 bit, senza HTTPS.
- **HTTPS** usa OpenSSL 3 o successivo: su Linux c'è già; su Windows bastano `libssl-3-x64.dll` e
  `libcrypto-3-x64.dll` accanto all'eseguibile o nella cartella `php/` (sono nello zip di PHP).

Esempi:

```bash
./segvault                                   # sceglie editor o server
./segvault --editor dati/                    # editor SQL, dati nella cartella dati/
./segvault-server --mysql-port 3306 dati/    # server: API su :8484 e protocollo MySQL su :3306
./segvault-server --static www --php auto --mysql-port 3306 --mail-port 1025 --https-port 8443 dati/
                                             # ambiente di sviluppo completo: siti NOME.localhost, PHP, posta, HTTPS
```

Gli eseguibili si rigenerano dai sorgenti con `make dist`, che li scrive in `dist/` con `SHA256SUMS`
(vedi il [README](../README.md)); il workflow `.github/workflows/release.yml` li allega alla release
quando si pubblica un tag `vX.Y.Z`.
