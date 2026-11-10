// Client JavaScript per segvault-server (browser, Node.js 18+, Deno, Bun: usa solo fetch).
//
//   import { SegVault } from "./segvault.js";
//   const db = new SegVault("http://127.0.0.1:8484", { database: "shop", token: "..." });
//   const rows = await db.all("SELECT * FROM prodotti WHERE prezzo < ?", [10]);
//
// Versione TypeScript con i tipi: segvault.ts (stessa API).

export class SegVaultError extends Error {
  constructor(message, details = {}) {
    super(message);
    this.name = "SegVaultError";
    this.status = details.status;          // codice HTTP
    this.statement = details.statement;    // indice dello statement fallito (0 = il primo)
    this.line = details.line;              // riga nel testo SQL
    this.results = details.results || [];  // risultati degli statement eseguiti prima dell'errore
  }
}

export class SegVault {
  /**
   * @param {string} url       indirizzo del server, es. "http://127.0.0.1:8484" ("" = stessa origine della pagina)
   * @param {{database?: string, token?: string}} [options]
   */
  constructor(url = "", options = {}) {
    this.url = url.replace(/\/+$/, "");
    this.database = options.database;
    this.token = options.token;
  }

  async #call(method, path, body) {
    const headers = {};
    if (body !== undefined) headers["Content-Type"] = "application/json";
    if (this.token) headers["Authorization"] = `Bearer ${this.token}`;
    const res = await fetch(`${this.url}/api/v1${path}`, {
      method, headers, body: body === undefined ? undefined : JSON.stringify(body),
    });
    let data;
    try { data = await res.json(); } catch { data = { ok: false, error: { message: `HTTP ${res.status}` } }; }
    if (!data.ok) {
      const e = data.error || {};
      throw new SegVaultError(e.message || `HTTP ${res.status}`,
        { status: res.status, statement: e.statement, line: e.line, results: data.results });
    }
    return data;
  }

  /** Esegue uno o più statement (separati da ;). Ritorna l'array dei risultati, uno per statement. */
  async query(sql, params = [], { transaction = false, rows = "arrays", database = this.database } = {}) {
    const data = await this.#call("POST", "/query", { sql, params, transaction, rows, database });
    return data.results;
  }

  /** Righe come oggetti { colonna: valore } dell'ultimo statement (di solito una SELECT). */
  async all(sql, params = [], options = {}) {
    const results = await this.query(sql, params, { ...options, rows: "objects" });
    return results.length ? (results[results.length - 1].rows || []) : [];
  }

  /** Prima riga (o null). */
  async get(sql, params = [], options = {}) {
    return (await this.all(sql, params, options))[0] ?? null;
  }

  /** INSERT / UPDATE / DELETE: numero di righe modificate dall'ultimo statement. */
  async run(sql, params = [], options = {}) {
    const results = await this.query(sql, params, options);
    return results.length ? (results[results.length - 1].rows_affected ?? 0) : 0;
  }

  /** Più statement in un'unica transazione: o tutti o nessuno. */
  async transaction(sql, params = [], options = {}) {
    return this.query(sql, params, { ...options, transaction: true });
  }

  /** Endpoint salvato (tabella api_endpoints): righe dell'ultimo statement come oggetti. */
  async endpoint(name, params = {}, { database = this.database } = {}) {
    const q = database ? `?database=${encodeURIComponent(database)}` : "";
    const data = await this.#call("POST", `/endpoints/${encodeURIComponent(name)}${q}`, params);
    return data.results.length ? (data.results[data.results.length - 1].rows || []) : [];
  }

  health()                 { return this.#call("GET", "/health"); }
  async databases()        { return (await this.#call("GET", "/databases")).databases; }
  async tables(db = this.database)        { return this.#call("GET", `/databases/${encodeURIComponent(db)}/tables`); }
  async describe(table, db = this.database) {
    return this.#call("GET", `/databases/${encodeURIComponent(db)}/tables/${encodeURIComponent(table)}`);
  }
}
