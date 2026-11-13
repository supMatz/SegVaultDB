// Client TypeScript per segvault-server (browser, Node.js 18+, Deno, Bun: usa solo fetch).
//
//   import { SegVault } from "./segvault";
//   interface Prodotto { id: number; nome: string; prezzo: number }
//   const db = new SegVault("http://127.0.0.1:8484", { database: "shop" });
//   const economici = await db.all<Prodotto>("SELECT * FROM prodotti WHERE prezzo < ?", [10]);

export type SqlValue = string | number | boolean | null;

export interface Column { name: string; type: string }

export interface StatementResult<Row = SqlValue[] | Record<string, SqlValue>> {
  columns?: Column[];        // presenti per le SELECT
  rows?: Row[];
  row_count?: number;
  rows_affected?: number;    // presenti per INSERT / UPDATE / DELETE / DDL
  time_ms: number;
}

export interface QueryOptions {
  transaction?: boolean;     // tutti gli statement in una transazione (rollback se uno fallisce)
  rows?: "arrays" | "objects";
  database?: string;
}

export class SegVaultError extends Error {
  status?: number;            // codice HTTP
  statement?: number;         // indice dello statement fallito (0 = il primo)
  line?: number;              // riga nel testo SQL
  results: StatementResult[]; // risultati degli statement eseguiti prima dell'errore

  constructor(message: string, status?: number, statement?: number, line?: number, results: StatementResult[] = []) {
    super(message);
    this.name = "SegVaultError";
    this.status = status;
    this.statement = statement;
    this.line = line;
    this.results = results;
  }
}

export class SegVault {
  private url: string;
  private options: { database?: string; token?: string; prefix?: string };
  private prefix: string;

  // prefix: dove sta l'API (default "/api/v1"; quello di --api-prefix se il server lo cambia)
  constructor(url = "", options: { database?: string; token?: string; prefix?: string } = {}) {
    this.url = url.replace(/\/+$/, "");
    this.prefix = (options.prefix ?? "/api/v1").replace(/\/+$/, "");
    this.options = options;
  }

  private async call<T>(method: string, path: string, body?: unknown): Promise<T> {
    const headers: Record<string, string> = {};
    if (body !== undefined) headers["Content-Type"] = "application/json";
    if (this.options.token) headers["Authorization"] = `Bearer ${this.options.token}`;
    const res = await fetch(`${this.url}${this.prefix}${path}`, {
      method, headers, body: body === undefined ? undefined : JSON.stringify(body),
    });
    let data: any;
    try { data = await res.json(); } catch { data = { ok: false, error: { message: `HTTP ${res.status}` } }; }
    if (!data.ok) {
      const e = data.error ?? {};
      throw new SegVaultError(e.message ?? `HTTP ${res.status}`, res.status, e.statement, e.line, data.results ?? []);
    }
    return data as T;
  }

  /** Uno o più statement separati da ';': un risultato per statement. */
  async query(sql: string, params: SqlValue[] = [], opts: QueryOptions = {}): Promise<StatementResult[]> {
    const data = await this.call<{ results: StatementResult[] }>("POST", "/query", {
      sql, params, transaction: opts.transaction ?? false, rows: opts.rows ?? "arrays",
      database: opts.database ?? this.options.database,
    });
    return data.results;
  }

  /** Righe dell'ultimo statement come oggetti tipizzati. */
  async all<T = Record<string, SqlValue>>(sql: string, params: SqlValue[] = [], opts: QueryOptions = {}): Promise<T[]> {
    const results = await this.query(sql, params, { ...opts, rows: "objects" });
    return (results.at(-1)?.rows ?? []) as T[];
  }

  async get<T = Record<string, SqlValue>>(sql: string, params: SqlValue[] = [], opts: QueryOptions = {}): Promise<T | null> {
    return (await this.all<T>(sql, params, opts))[0] ?? null;
  }

  /** INSERT / UPDATE / DELETE: righe modificate dall'ultimo statement. */
  async run(sql: string, params: SqlValue[] = [], opts: QueryOptions = {}): Promise<number> {
    return (await this.query(sql, params, opts)).at(-1)?.rows_affected ?? 0;
  }

  transaction(sql: string, params: SqlValue[] = [], opts: QueryOptions = {}) {
    return this.query(sql, params, { ...opts, transaction: true });
  }

  /** Endpoint salvato (tabella api_endpoints): righe dell'ultimo statement come oggetti tipizzati. */
  async endpoint<T = Record<string, SqlValue>>(name: string, params: Record<string, SqlValue> = {},
                                               database = this.options.database): Promise<T[]> {
    const q = database ? `?database=${encodeURIComponent(database)}` : "";
    const data = await this.call<{ results: StatementResult[] }>("POST", `/endpoints/${encodeURIComponent(name)}${q}`, params);
    return (data.results.at(-1)?.rows ?? []) as T[];
  }

  health() { return this.call<{ ok: true; version: string }>("GET", "/health"); }

  async databases(): Promise<string[]> {
    return (await this.call<{ databases: string[] }>("GET", "/databases")).databases;
  }

  tables(db = this.options.database ?? "") {
    return this.call<{ tables: string[]; views: string[] }>("GET", `/databases/${encodeURIComponent(db)}/tables`);
  }

  describe(table: string, db = this.options.database ?? "") {
    return this.call<{ columns: { name: string; type: string; nullable: boolean; primary_key: boolean;
                                  auto_increment: boolean; default: string | null }[]; create: string }>(
      "GET", `/databases/${encodeURIComponent(db)}/tables/${encodeURIComponent(table)}`);
  }
}
