"""Client Python per segvault-server (solo libreria standard, Python 3.8+).

    from segvault import SegVault
    db = SegVault("http://127.0.0.1:8484", database="shop")
    for p in db.all("SELECT * FROM prodotti WHERE prezzo < ?", [10]):
        print(p["nome"], p["prezzo"])
"""
import json
import urllib.error
import urllib.parse
import urllib.request


class SegVaultError(Exception):
    def __init__(self, message, status=None, statement=None, line=None, results=None):
        super().__init__(message)
        self.status = status          # codice HTTP
        self.statement = statement    # indice dello statement fallito (0 = il primo)
        self.line = line              # riga nel testo SQL
        self.results = results or []  # risultati degli statement eseguiti prima dell'errore


class SegVault:
    def __init__(self, url="http://127.0.0.1:8484", database=None, token=None, timeout=30):
        self.url = url.rstrip("/")
        self.database = database
        self.token = token
        self.timeout = timeout

    def _call(self, method, path, body=None):
        headers = {}
        data = None
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        req = urllib.request.Request(self.url + "/api/v1" + path, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as res:
                status, payload = res.status, json.load(res)
        except urllib.error.HTTPError as e:      # 4xx / 5xx: il corpo contiene l'errore
            status = e.code
            try:
                payload = json.load(e)
            except ValueError:
                payload = {"ok": False, "error": {"message": "HTTP %d" % e.code}}
        if not payload.get("ok"):
            err = payload.get("error", {})
            raise SegVaultError(err.get("message", "HTTP %d" % status), status,
                                err.get("statement"), err.get("line"), payload.get("results"))
        return payload

    def query(self, sql, params=(), transaction=False, rows="arrays", database=None):
        """Uno o più statement separati da ';': ritorna la lista dei risultati, uno per statement."""
        return self._call("POST", "/query", {
            "sql": sql, "params": list(params), "transaction": transaction,
            "rows": rows, "database": database or self.database,
        })["results"]

    def all(self, sql, params=(), **kw):
        """Righe dell'ultimo statement come dizionari {colonna: valore}."""
        results = self.query(sql, params, rows="objects", **kw)
        return results[-1].get("rows", []) if results else []

    def get(self, sql, params=(), **kw):
        rows = self.all(sql, params, **kw)
        return rows[0] if rows else None

    def run(self, sql, params=(), **kw):
        """INSERT / UPDATE / DELETE: righe modificate dall'ultimo statement."""
        results = self.query(sql, params, **kw)
        return results[-1].get("rows_affected", 0) if results else 0

    def transaction(self, sql, params=(), **kw):
        return self.query(sql, params, transaction=True, **kw)

    def health(self):
        return self._call("GET", "/health")

    def databases(self):
        return self._call("GET", "/databases")["databases"]

    def tables(self, database=None):
        return self._call("GET", "/databases/%s/tables" % urllib.parse.quote(database or self.database))

    def describe(self, table, database=None):
        db = urllib.parse.quote(database or self.database)
        return self._call("GET", "/databases/%s/tables/%s" % (db, urllib.parse.quote(table)))
