# Segnalazioni Urbane — *Segnalacity*

> Piattaforma web per la gestione civica di segnalazioni urbane, scritta interamente in C.  
> Cittadini, operatori e amministratori comunali interagiscono su una mappa interattiva per seguire l'intero ciclo di vita di una segnalazione.

---

## Panoramica

**Segnalacity** è un server HTTP scritto in C puro che espone un'applicazione multi-ruolo per la gestione di problemi urbani (buche, lampioni rotti, degrado, ecc.). Il server usa **libuv** per l'I/O asincrono non-bloccante, **SQLite** come database embedded, **cJSON** per la serializzazione JSON, e un motore di template HTML custom.

```
                         ┌────────────────────────┐
                         │      libuv event loop   │
                         └──────────┬─────────────┘
                                    │
             ┌──────────────────────┼──────────────────────┐
             │                      │                       │
      accept_connection()   handle_connection()    rate_limiter_check()
             │                      │
      client_manager         picohttpparser
      (slab alloc)                  │
                             handle_request()
                                    │
                    ┌───────────────┼───────────────┐
                    │               │               │
              route_pages       route_api      route_helpers
             (HTML views)     (JSON API)      (static assets)
                    │               │
               template.c        db.c / user.c / report.c / session.c
                                    │
                               segnalacity.db (SQLite)
```

---

## Ruoli e flusso applicativo

| Ruolo | Cosa può fare |
|---|---|
| **Cittadino** | Registrarsi, accedere, inviare una segnalazione su mappa, consultare le proprie segnalazioni attive e archiviate, lasciare feedback sulle risolte |
| **Operatore** | Visualizzare le segnalazioni assegnategli, accettare o rifiutare un'assegnazione, marcarle come risolte |
| **Admin** | Vedere tutte le segnalazioni della propria città, assegnare segnalazioni agli operatori |

### Ciclo di vita di una segnalazione

```
STATUS_ACTIVE  ──(admin assegna)──▶  STATUS_ASSIGNED
                                           │
                           ┌───────────────┴───────────────┐
                    (operatore accetta)             (operatore rifiuta)
                           │                               │
                  STATUS_IN_PROGRESS              STATUS_ACTIVE (riassegnabile)
                           │
                   (operatore risolve)
                           │
                    STATUS_RESOLVED
                           │
                  (cittadino lascia feedback)
```

---

## Struttura del progetto

```
segnalacity/
├── server.c / server.h            # Entry point, event loop libuv
├── connection_handler.c / .h      # Gestione ciclo di vita connessione TCP
├── client_manager.c / .h          # Slab allocator per ClientCtx
├── route_handler.c / .h           # Dispatch table (method, path) → handler
├── route_pages.c / .h             # Handler HTML (login, home, submit, mappa)
├── route_api.c / .h               # Handler JSON REST API
├── route_helpers.c / .h           # Asset statici (CSS)
├── http_types.h                   # Struct HttpRequest / HttpResponse
├── http_utils.c / .h              # Parsing URL, query string, cookie
├── picohttpparser.c / .h          # Parser HTTP/1.1 embedded (pico)
├── template.c / .h                # Motore di template (sostituzione variabili)
├── db.c / .h                      # Init e accesso SQLite
├── user.c / .h                    # Registrazione, autenticazione, ruoli
├── session.c / .h                 # Sessioni in-memory (cookie sid)
├── report.c / .h                  # CRUD segnalazioni + serializzazione JSON
├── geo.c / .h                     # Lookup geometrie comunali (GeoJSON ISTAT)
├── hash_table.c / .h              # Hash table generica con chaining e auto-resize
├── slab_allocator.c / .h          # Allocatore a slab per ClientCtx
├── rate_limiter.c / .h            # Rate limiting per-IP (sliding window)
├── config.h                       # Costanti globali e include centrali
├── Makefile
├── data/
│   ├── comuni.geojson             # Geometrie ISTAT di tutti i comuni italiani
│   └── cities.json                # Array di nomi comuni (generato da geo_init)
└── templates/
    ├── login.html
    ├── register.html
    ├── submit.html
    ├── citizen_home.html          # Mappa interattiva cittadino
    ├── operator_map.html          # Mappa interattiva operatore
    ├── admin_map.html             # Dashboard mappa admin
    └── common.css
```

---

## Componenti principali

### Server & Event Loop (libuv)

Il server usa **libuv** per multiplexare migliaia di connessioni simultanee su un singolo thread. L'I/O è completamente non-bloccante; ogni `ClientCtx` vive in un pool slab per minimizzare le allocazioni heap sotto carico elevato.

### Database (SQLite + cJSON)

La persistenza è affidata a **SQLite** (`segnalacity.db`). Le query sono gestite in `db.c`, `user.c` e `report.c`. I risultati vengono serializzati in JSON con **cJSON** e restituiti direttamente ai client.

### Autenticazione e sessioni

Le password vengono salvate come hash DJB2 con salt casuale a 16 byte (hex). Le sessioni sono gestite in-memory tramite la hash table interna; ogni sessione è identificata da un token casuale a 32 caratteri hex memorizzato nel cookie `sid` (durata 24 ore).

### Geolocalizzazione

Al boot, `geo_init()` carica il GeoJSON ISTAT dei comuni italiani, calcola il bounding box e il centroide per ogni municipio e li indicizza nella hash table. Le mappe HTML vengono centrate automaticamente sulla città dell'utente loggato.

### Hash Table generica

Implementazione custom con:
- Chiavi e valori come buffer binari (`void *` + `size_t`)
- Chaining per la risoluzione delle collisioni
- Auto-resize al prossimo primo ≥ 2× la capacità quando il load factor raggiunge 1
- Seed per-istanza da `/dev/urandom` contro hash-flooding

### Rate Limiter

Sliding window per-IP con due contatori (finestra corrente e precedente):

```
estimated_rate = countPrev × (1 − elapsed / window) + countCurr
```

Se la stima supera `RATE_LIMIT_RPS` (default 100 req/s), il server risponde con `429 Too Many Requests`. La tabella si ricicla automaticamente oltre le 10 000 entry.

---

## API HTTP

### Pagine (HTML)

| Metodo | Path | Descrizione |
|---|---|---|
| `GET` | `/` | Redirect al login |
| `GET` | `/home` | Dashboard utente (varia per ruolo) |
| `GET` | `/register` | Pagina registrazione |
| `GET` | `/submit` | Pagina invio segnalazione |
| `GET` | `/logout` | Logout e invalidazione sessione |
| `POST` | `/login` | Autenticazione |
| `POST` | `/register` | Creazione account |
| `POST` | `/submit` | Invio nuova segnalazione |
| `GET` | `/static/common.css` | Foglio di stile condiviso |

### REST API (JSON)

| Metodo | Path | Accesso | Descrizione |
|---|---|---|---|
| `GET` | `/api/cities` | Pubblico | Lista di tutti i comuni disponibili |
| `GET` | `/api/reports/active` | Autenticato | Segnalazioni attive dell'utente |
| `GET` | `/api/reports/archived` | Autenticato | Segnalazioni risolte dell'utente |
| `GET` | `/api/reports/all` | Admin | Tutte le segnalazioni della città |
| `GET` | `/api/operators` | Admin | Lista operatori della città |
| `POST` | `/api/report/status` | Operatore | Marca una segnalazione come risolta |
| `POST` | `/api/report/respond` | Operatore | Accetta o rifiuta un'assegnazione |
| `POST` | `/api/report/feedback` | Cittadino | Lascia un feedback (1–5 stelle) |
| `POST` | `/api/admin/assign` | Admin | Assegna una segnalazione a un operatore |

---

## Build

### Dipendenze

- **libuv** ≥ 1.x
- **SQLite 3** (`libsqlite3-dev`)
- **cJSON** (`libcjson-dev`)
- **GCC** con supporto a `-fsanitize=address`

Su Ubuntu/Debian:

```bash
sudo apt install libsqlite3-dev libcjson-dev libuv1-dev gcc
```

### Compilazione

```bash
make
```

Il Makefile compila con `-fsanitize=address -Wall -Wextra -O2`. Per rimuovere il binario:

```bash
make clean
```

Per ripristinare anche il database e le sessioni:

```bash
make cleandb
```

---

## Avvio

```bash
./segnalacity
```

Il server ascolta su **porta 8080** (configurabile via `PORT` in `config.h` o come macro al momento della compilazione):

```bash
make CFLAGS="-DPORT=9090 -O2 -Wall -Wextra"
```

Al primo avvio, `geo_init()` carica `data/comuni.geojson` e genera `data/cities.json`. L'operazione può richiedere alcuni secondi la prima volta.

---

## Configurazione

Tutte le costanti si trovano in `config.h`:

| Costante | Valore predefinito | Descrizione |
|---|---|---|
| `PORT` | `8080` | Porta di ascolto |
| `APP_DB_PATH` | `segnalacity.db` | Percorso del database SQLite |
| `KEEPALIVE_TIMEOUT` | `10` | Timeout connessione idle (secondi) |
| `MAX_EVENTS` | `4096` | Max eventi per iterazione libuv |
| `MAX_CLIENTS` | `16384` | Limite massimo connessioni simultanee |
| `LISTEN_BACKLOG` | `65535` | TCP listen backlog |
| `RATE_LIMIT_RPS` | `100` | Max richieste/secondo per IP |
| `CLIENT_BUFFER_SIZE` | `8 KB` | Buffer di ricezione per connessione |
| `RESPONSE_BUFFER_SIZE` | `256 KB` | Buffer di risposta per richiesta |
| `SESSION_MAX_AGE` | `86400` | Durata massima sessione (24 ore) |
| `GEO_JSON_PATH` | `data/comuni.geojson` | GeoJSON ISTAT dei comuni |
| `CITIES_JSON_PATH` | `data/cities.json` | JSON delle città (generato) |

---

## Documentazione

Il progetto è documentato con commenti in stile Doxygen. Per generare la documentazione HTML:

```bash
doxygen Doxyfile
```

---

## Note tecniche

- Il parsing HTTP è delegato a **picohttpparser**, un parser zero-copy embedded nel progetto.
- Il motore di template (`template.c`) sostituisce variabili del tipo `{{NOME}}` all'interno dei file HTML, senza dipendenze esterne.
- Le geometrie comunali provengono dal dataset **ISTAT** in formato GeoJSON; il bounding box è una approssimazione dell'area reale del comune.
- Il rate limiter è disabilitato in modalità debug (`DEBUG_RATE_LIMIT 0`); impostarlo a `1` in `rate_limiter.h` per abilitarlo.