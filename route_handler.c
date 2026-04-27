/*
 * route_handler.c
 *
 * Implements the full HTTP layer of SegnalaCity:
 *   - HTML pages served to browser (login, register, citizen home,
 *     citizen submit, operator map)
 *   - REST-ish JSON API used by the JavaScript embedded in those pages
 *   - Form POST handlers (login, register, submit report, update status)
 *   - Cookie-based session management
 */

#include "route_handler.h"
#include "server_functions.h"
#include "user.h"
#include "session.h"
#include "report.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ══════════════════════════════════════════════════════════════════════
   SHARED HELPERS
   ══════════════════════════════════════════════════════════════════════ */

/* Converts a single hex char to its integer value. */
static inline int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

/*
 * URL-decodes src (in-place style) into dest (NUL-terminated, max-1 bytes).
 * Handles %XX sequences and '+' → space.
 * Blocks embedded NULs (%00) by writing '\0' and returning immediately.
 */
/*
 * URL-decodes one field value from src into dest (NUL-terminated, max-1 bytes).
 * Stops at '&' (next field), ' ', '\r', '\n' (end of query string / body line),
 * or buffer full.  Handles %XX sequences and '+' → space.
 * Blocks embedded NULs (%00) by stopping immediately.
 */
static void url_decode(const char *src, char *dest, size_t max) {
    size_t i = 0;
    while (*src && *src != '&' && *src != ' ' && *src != '\r' && *src != '\n'
           && i < max - 1) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char c = (char)((hex_val(src[1]) << 4) | hex_val(src[2]));
            if (c == '\0') break; /* block null-embedded strings */
            dest[i++] = c;
            src += 3;
        } else if (*src == '+') {
            dest[i++] = ' '; src++;
        } else {
            dest[i++] = *src++;
        }
    }
    dest[i] = '\0';
}

/*
 * Finds paramName in an URL-encoded string (query string or POST body).
 * paramName must end with '='. Writes the decoded value into dest.
 */
static void get_field(const char *src, const char *paramName, char *dest, size_t max) {
    dest[0] = '\0';
    if (!src || !paramName) return;
    size_t plen = strlen(paramName);
    const char *p = src;
    while ((p = strstr(p, paramName)) != NULL) {
        if (p == src || *(p - 1) == '&') {
            url_decode(p + plen, dest, max);
            return;
        }
        p++;
    }
}

/* Skips to the query string part of a URL (after '?'). */

/* Returns a pointer to the POST body (after \r\n\r\n), or NULL. */
static const char *post_body(const char *req) {
    const char *p = strstr(req, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

/*
 * Extracts the value of a named cookie from the Cookie: header.
 * Writes the value (URL-decoded) into dest.
 */
static void parse_cookie(const char *req, const char *name, char *dest, size_t max) {
    dest[0] = '\0';
    const char *h = strstr(req, "Cookie:");
    if (!h) return;
    h += 7;
    const char *eol = strchr(h, '\n');
    size_t nlen = strlen(name);

    while (h && (!eol || h < eol)) {
        while (*h == ' ') h++;
        if (strncmp(h, name, nlen) == 0 && h[nlen] == '=') {
            h += nlen + 1;
            url_decode(h, dest, max);
            return;
        }
        h = strchr(h, ';');
        if (h) h++;
    }
}

/*
 * Verifies the session cookie from the request and, on success, fills *u.
 * Returns true if the session is valid.
 */
static bool get_session_user(const char *req, User *u) {
    char token[TOKEN_HEX_LEN + 2];
    parse_cookie(req, SESSION_COOKIE_NAME, token, sizeof(token));
    if (!token[0]) return false;
    uint64_t userId;
    if (!session_verify(g_sessions, token, &userId)) return false;
    return user_get_by_id(userId, u);
}

/* Simple HTML entity escaping for user-controlled strings in HTML output. */
static void html_escape(const char *src, char *dest, size_t max) {
    size_t i = 0;
    while (*src && i + 7 < max) {  /* 7 = max entity len (6) + NUL */
        switch (*src) {
            case '<':  memcpy(dest + i, "&lt;",   4); i += 4; break;
            case '>':  memcpy(dest + i, "&gt;",   4); i += 4; break;
            case '&':  memcpy(dest + i, "&amp;",  5); i += 5; break;
            case '"':  memcpy(dest + i, "&quot;", 6); i += 6; break;
            case '\'': memcpy(dest + i, "&#39;",  5); i += 5; break;
            default:   dest[i++] = *src; break;
        }
        src++;
    }
    dest[i] = '\0';
}

/* ══════════════════════════════════════════════════════════════════════
   COMMON PAGE CHROME (header + footer shared by all HTML pages)
   ══════════════════════════════════════════════════════════════════════ */

/* Shared CSS embedded once per page via the common <head>. */
static const char CSS[] =
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:system-ui,sans-serif;background:#f0f4f8;color:#1f2937}"
"a{color:#2563eb;text-decoration:none}"
"a:hover{text-decoration:underline}"
".card{background:#fff;border-radius:12px;box-shadow:0 2px 16px rgba(0,0,0,.08);padding:2rem}"
".btn{display:inline-block;padding:.5rem 1.2rem;border-radius:8px;border:none;"
"font-size:.9rem;font-weight:600;cursor:pointer;transition:opacity .15s}"
".btn-primary{background:#2563eb;color:#fff}"
".btn-primary:hover{opacity:.85}"
".btn-warning{background:#d97706;color:#fff}"
".btn-warning:hover{opacity:.85}"
".btn-success{background:#059669;color:#fff}"
".btn-success:hover{opacity:.85}"
".btn-sm{padding:.3rem .8rem;font-size:.8rem}"
".navbar{background:#1e40af;color:#fff;padding:.8rem 1.5rem;"
"display:flex;align-items:center;justify-content:space-between;gap:1rem}"
".navbar h1{font-size:1.1rem;font-weight:700}"
".navbar .nav-right{display:flex;gap:1rem;align-items:center;font-size:.875rem}"
".navbar a{color:#bfdbfe}"
"input,select,textarea{width:100%;padding:.55rem .85rem;border:1px solid #d1d5db;"
"border-radius:8px;font-size:.95rem;margin-bottom:.9rem;outline:none;"
"font-family:inherit;transition:border .2s}"
"input:focus,select:focus,textarea:focus{border-color:#2563eb}"
"label{display:block;font-size:.85rem;font-weight:500;color:#374151;margin-bottom:.25rem}"
".form-group{margin-bottom:.6rem}"
".alert{padding:.65rem 1rem;border-radius:8px;margin-bottom:1rem;font-size:.875rem}"
".alert-err{background:#fee2e2;border:1px solid #fca5a5;color:#991b1b}"
".alert-ok{background:#d1fae5;border:1px solid #6ee7b7;color:#065f46}"
"table{width:100%;border-collapse:collapse;font-size:.875rem}"
"th,td{padding:.55rem .75rem;text-align:left;border-bottom:1px solid #e5e7eb}"
"th{background:#f9fafb;font-weight:600;color:#374151}"
"tr:hover td{background:#f3f4f6}"
".badge{display:inline-block;padding:.15rem .55rem;border-radius:999px;font-size:.75rem;font-weight:600}"
".badge-0{background:#dbeafe;color:#1d4ed8}"    /* ACTIVE      */
".badge-1{background:#fef3c7;color:#92400e}"    /* IN PROGRESS */
".badge-2{background:#d1fae5;color:#065f46}"    /* RESOLVED    */
".stat-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:1rem;margin:1rem 0}"
".stat-card{background:#fff;border-radius:10px;padding:1.2rem;text-align:center;"
"box-shadow:0 1px 8px rgba(0,0,0,.07)}"
".stat-card .val{font-size:2rem;font-weight:700;color:#2563eb}"
".stat-card .lbl{font-size:.8rem;color:#6b7280;margin-top:.25rem}";

/*
 * Writes the standard <head> block.  pageTitle is the <title> content.
 * Returns number of bytes written (snprintf-style).
 */
static int write_head(char *buf, size_t max, const char *pageTitle) {
    return snprintf(buf, max,
        "<!DOCTYPE html><html lang='it'>"
        "<head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>%s - SegnalaCity</title>"
        "<style>%s</style></head><body>",
        pageTitle, CSS);
}

/* Writes the top navigation bar. username may be NULL for guest pages. */
static int write_navbar(char *buf, size_t max, const char *username, bool isOp) {
    char esc[64] = "";
    if (username) html_escape(username, esc, sizeof(esc));
    return snprintf(buf, max,
        "<nav class='navbar'>"
        "<h1>&#128205; SegnalaCity</h1>"
        "<div class='nav-right'>"
        "%s%s"
        "<a href='/logout'>Esci</a>"
        "</div></nav>",
        username ? esc : "",
        isOp ? " &nbsp;(Operatore)" : "");
}

/* ══════════════════════════════════════════════════════════════════════
   PAGE GENERATORS
   ══════════════════════════════════════════════════════════════════════ */

static int page_login(char *resp, size_t max, const char *errMsg) {
    char head[8192]; write_head(head, sizeof(head), "Accedi");
    return snprintf(resp, max,
        "%s"
        "<div style='display:flex;align-items:center;justify-content:center;min-height:100vh'>"
        "<div class='card' style='width:100%%;max-width:400px'>"
        "<div style='text-align:center;font-size:2.5rem;margin-bottom:.5rem'>&#128205;</div>"
        "<h2 style='text-align:center;color:#2563eb;margin-bottom:1.5rem'>SegnalaCity</h2>"
        "%s"
        "<form method='POST' action='/login'>"
        "<div class='form-group'><label>Username</label>"
        "<input name='username' required autocomplete='username' placeholder='Il tuo username'></div>"
        "<div class='form-group'><label>Password</label>"
        "<input name='password' type='password' required placeholder='La tua password'></div>"
        "<button class='btn btn-primary' style='width:100%%' type='submit'>Accedi</button>"
        "</form>"
        "<p style='text-align:center;margin-top:1rem;font-size:.875rem;color:#6b7280'>"
        "Non hai un account? <a href='/register'>Registrati</a></p>"
        "</div></div></body></html>",
        head,
        errMsg && errMsg[0] ? errMsg : "");
}

static int page_register(char *resp, size_t max, const char *errMsg) {
    char head[8192]; write_head(head, sizeof(head), "Registrati");
    return snprintf(resp, max,
        "%s"
        "<div style='display:flex;align-items:center;justify-content:center;min-height:100vh'>"
        "<div class='card' style='width:100%%;max-width:440px'>"
        "<div style='text-align:center;font-size:2.5rem;margin-bottom:.5rem'>&#128205;</div>"
        "<h2 style='text-align:center;color:#2563eb;margin-bottom:1.5rem'>Crea un account</h2>"
        "%s"
        "<form method='POST' action='/register'>"
        "<div class='form-group'><label>Username</label>"
        "<input name='username' required placeholder='Scegli un username' maxlength='30'></div>"
        "<div class='form-group'><label>Password</label>"
        "<input name='password' type='password' required placeholder='Almeno 6 caratteri'></div>"
        "<div class='form-group'><label>Città</label>"
        "<input name='city' required placeholder='Es: Roma' maxlength='30'></div>"
        "<div class='form-group'><label>Ruolo</label>"
        "<select name='role'>"
        "<option value='0'>Cittadino</option>"
        "<option value='1'>Operatore municipale</option>"
        "</select></div>"
        "<button class='btn btn-primary' style='width:100%%' type='submit'>Registrati</button>"
        "</form>"
        "<p style='text-align:center;margin-top:1rem;font-size:.875rem;color:#6b7280'>"
        "Hai già un account? <a href='/'>Accedi</a></p>"
        "</div></div></body></html>",
        head,
        errMsg && errMsg[0] ? errMsg : "");
}

/* ── Citizen home ─────────────────────────────────────────────────────── */

static int page_citizen_home(char *resp, size_t max, const User *u) {
    char head[8192];    write_head(head, sizeof(head), "Home");
    char nav[512];      write_navbar(nav, sizeof(nav), u->username, false);
    char esc_user[64];  html_escape(u->username, esc_user, sizeof(esc_user));

    return snprintf(resp, max,
        "%s%s"
        "<div style='max-width:960px;margin:2rem auto;padding:0 1rem'>"

        /* Welcome + quick actions */
        "<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:1.5rem'>"
        "<h2>Ciao, %s &#128075;</h2>"
        "<a href='/submit' class='btn btn-primary'>&#43; Nuova segnalazione</a>"
        "</div>"

        /* Stats row */
        "<div class='stat-grid' id='stats-grid'>"
        "<div class='stat-card'><div class='val' id='s-active'>-</div><div class='lbl'>Le mie segnalazioni attive</div></div>"
        "<div class='stat-card'><div class='val' id='s-archived'>-</div><div class='lbl'>Le mie archiviate</div></div>"
        "<div class='stat-card'><div class='val' id='s-uptime'>-</div><div class='lbl'>Uptime server (s)</div></div>"
        "</div>"

        /* Active reports table */
        "<div class='card' style='margin-bottom:1.5rem'>"
        "<h3 style='margin-bottom:1rem'>&#128204; Segnalazioni attive</h3>"
        "<div id='active-table'><p style='color:#9ca3af'>Caricamento…</p></div>"
        "</div>"

        /* Archived reports table */
        "<div class='card'>"
        "<h3 style='margin-bottom:1rem'>&#128190; Segnalazioni archiviate</h3>"
        "<div id='archived-table'><p style='color:#9ca3af'>Caricamento…</p></div>"
        "</div>"
        "</div>"

        "<script>"
        "var STATUS=['Attivo','In lavorazione','Risolto'];"

        /* HTML-escape helper to prevent stored XSS from report fields */
        "function esc(s){"
        "return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;')"
        ".replace(/>/g,'&gt;').replace(/\"/g,'&quot;').replace(/'/g,'&#39;');}"

        "function renderTable(reports,containerId){"
        "var c=document.getElementById(containerId);"
        "if(!reports||!reports.length){c.innerHTML='<p style=\"color:#9ca3af\">Nessuna segnalazione.</p>';return;}"
        "var h='<table><thead><tr><th>ID</th><th>Categoria</th><th>Descrizione</th><th>Città</th><th>Stato</th><th>Data</th></tr></thead><tbody>';"
        "reports.forEach(function(r){"
        "var d=new Date(r.created_at*1000).toLocaleDateString('it-IT');"
        "h+='<tr><td>#'+r.id+'</td><td>'+esc(r.category)+'</td><td>'+esc(r.description)+'</td>"
        "<td>'+esc(r.city)+'</td>"
        "<td><span class=\"badge badge-'+r.status+'\">'+STATUS[r.status]+'</span></td>"
        "<td>'+d+'</td></tr>';"
        "});"
        "h+='</tbody></table>';"
        "c.innerHTML=h;"
        "}"

        "fetch('/api/reports/active').then(function(r){return r.json();}).then(function(d){"
        "renderTable(d,'active-table');"
        "document.getElementById('s-active').textContent=Array.isArray(d)?d.length:'-';"
        "}).catch(function(){"
        "document.getElementById('active-table').innerHTML='<p style=\"color:#ef4444\">Errore di caricamento.</p>';"
        "});"

        "fetch('/api/reports/archived').then(function(r){return r.json();}).then(function(d){"
        "renderTable(d,'archived-table');"
        "document.getElementById('s-archived').textContent=Array.isArray(d)?d.length:'-';"
        "}).catch(function(){"
        "document.getElementById('archived-table').innerHTML='<p style=\"color:#ef4444\">Errore di caricamento.</p>';"
        "});"

        "fetch('/api/stats').then(function(r){return r.json();}).then(function(d){"
        "document.getElementById('s-uptime').textContent=d.uptime||'-';"
        "}).catch(function(){});"
        "</script>"
        "</body></html>",
        head, nav, esc_user);
}

/* ── Citizen submit form ─────────────────────────────────────────────── */

static int page_submit(char *resp, size_t max, const User *u, const char *errMsg) {
    char head[8192]; write_head(head, sizeof(head), "Nuova segnalazione");
    char nav[512];   write_navbar(nav, sizeof(nav), u->username, false);
    char esc_city[64]; html_escape(u->city, esc_city, sizeof(esc_city));

    return snprintf(resp, max,
        "%s%s"
        "<div style='max-width:640px;margin:2rem auto;padding:0 1rem'>"
        "<h2 style='margin-bottom:1.5rem'>&#128204; Nuova segnalazione</h2>"
        "%s"
        "<div class='card'>"
        "<form method='POST' action='/submit'>"
        "<div class='form-group'><label>Categoria</label>"
        "<select name='category'>"
        "<option>Buche stradali</option>"
        "<option>Rifiuti abbandonati</option>"
        "<option>Illuminazione</option>"
        "<option>Vandalismi</option>"
        "<option>Verde pubblico</option>"
        "<option>Altro</option>"
        "</select></div>"
        "<div class='form-group'><label>Descrizione</label>"
        "<textarea name='description' rows='4' required placeholder='Descrivi il problema…' maxlength='127'></textarea></div>"
        "<div style='display:grid;grid-template-columns:1fr 1fr;gap:1rem'>"
        "<div class='form-group'><label>Latitudine</label>"
        "<input name='lat' type='number' step='any' placeholder='Es: 41.902782'></div>"
        "<div class='form-group'><label>Longitudine</label>"
        "<input name='lon' type='number' step='any' placeholder='Es: 12.496366'></div>"
        "</div>"
        "<p style='font-size:.8rem;color:#6b7280;margin-bottom:.9rem'>"
        "Clicca sulla mappa per selezionare le coordinate automaticamente.</p>"

        /* Embedded mini Leaflet map for coordinate picking */
        "<div id='pick-map' style='height:240px;border-radius:8px;margin-bottom:1rem'></div>"
        "<link rel='stylesheet' href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css'>"
        "<script src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js'></script>"
        "<script>"
        "var pm=L.map('pick-map').setView([41.9,12.5],5);"
        "L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',"
        "{attribution:'&copy; OpenStreetMap contributors'}).addTo(pm);"
        "var mk=null;"
        "pm.on('click',function(e){"
        "if(mk)pm.removeLayer(mk);"
        "mk=L.marker([e.latlng.lat,e.latlng.lng]).addTo(pm);"
        "document.querySelector('[name=lat]').value=e.latlng.lat.toFixed(6);"
        "document.querySelector('[name=lon]').value=e.latlng.lng.toFixed(6);"
        "});"
        "</script>"

        "<button class='btn btn-primary' style='width:100%%' type='submit'>Invia segnalazione</button>"
        "</form></div>"
        "<p style='margin-top:1rem;font-size:.875rem'><a href='/home'>&larr; Torna alla home</a></p>"
        "</div></body></html>",
        head, nav,
        errMsg && errMsg[0] ? errMsg : "");
}

/* ── Operator map ──────────────────────────────────────────────────────
 *
 * Full-page Leaflet map with a sidebar listing active reports.
 * All data is loaded via /api/reports/active (server-side filtered to
 * the operator's city).  Status updates go to /api/report/status.
 * ─────────────────────────────────────────────────────────────────── */

static int page_operator_map(char *resp, size_t max, const User *u) {
    char esc_user[64]; html_escape(u->username, esc_user, sizeof(esc_user));
    char esc_city[64]; html_escape(u->city,     esc_city, sizeof(esc_city));

    return snprintf(resp, max,
        "<!DOCTYPE html><html lang='it'>"
        "<head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Dashboard Operatore - %s</title>"
        "<link rel='stylesheet' href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css'>"
        "<style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{font-family:system-ui,sans-serif;display:flex;flex-direction:column;height:100vh;overflow:hidden}"
        ".topbar{background:#1e40af;color:#fff;padding:.6rem 1.2rem;"
        "display:flex;align-items:center;justify-content:space-between;flex-shrink:0}"
        ".topbar h1{font-size:1rem;font-weight:700}"
        ".topbar a{color:#bfdbfe;font-size:.85rem;text-decoration:none}"
        ".main{display:flex;flex:1;overflow:hidden}"
        "#sidebar{width:320px;overflow-y:auto;border-right:1px solid #e5e7eb;background:#f9fafb;flex-shrink:0}"
        ".sb-header{padding:.9rem 1rem;background:#fff;border-bottom:1px solid #e5e7eb;font-weight:600;font-size:.9rem}"
        ".report-item{padding:.75rem 1rem;border-bottom:1px solid #f3f4f6;cursor:pointer;transition:background .15s}"
        ".report-item:hover{background:#eff6ff}"
        ".report-item .cat{font-weight:600;font-size:.875rem;color:#1e40af}"
        ".report-item .desc{font-size:.8rem;color:#4b5563;margin:.15rem 0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
        ".report-item .meta{font-size:.75rem;color:#9ca3af}"
        ".badge{display:inline-block;padding:.1rem .45rem;border-radius:999px;font-size:.72rem;font-weight:600}"
        ".badge-0{background:#dbeafe;color:#1d4ed8}"
        ".badge-1{background:#fef3c7;color:#92400e}"
        ".badge-2{background:#d1fae5;color:#065f46}"
        "#map{flex:1}"
        ".popup-actions{margin-top:.5rem;display:flex;gap:.4rem;flex-wrap:wrap}"
        ".popup-actions button{padding:.3rem .7rem;border:none;border-radius:6px;"
        "font-size:.78rem;font-weight:600;cursor:pointer}"
        ".btn-takeon{background:#d97706;color:#fff}"
        ".btn-resolve{background:#059669;color:#fff}"
        ".loading{padding:1rem;text-align:center;color:#9ca3af;font-size:.875rem}"
        "</style></head><body>"

        /* Top bar */
        "<div class='topbar'>"
        "<h1>&#128205; SegnalaCity &mdash; Operatore: %s &nbsp;|&nbsp; Città: %s</h1>"
        "<a href='/logout'>Esci</a>"
        "</div>"

        "<div class='main'>"
        /* Sidebar */
        "<div id='sidebar'>"
        "<div class='sb-header'>Segnalazioni attive</div>"
        "<div id='sb-list'><div class='loading'>Caricamento…</div></div>"
        "</div>"
        /* Map */
        "<div id='map'></div>"
        "</div>"

        "<script src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js'></script>"
        "<script>"

        /* HTML-escape helper to prevent stored XSS from report fields */
        "function esc(s){"
        "return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;')"
        ".replace(/>/g,'&gt;').replace(/\"/g,'&quot;').replace(/'/g,'&#39;');}"

        /* Initialise Leaflet */
        "var map=L.map('map').setView([41.9,12.5],6);"
        "L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',"
        "{attribution:'&copy; <a href=\"https://www.openstreetmap.org/copyright\">OpenStreetMap</a>'}).addTo(map);"

        "var STATUS=['Attivo','In lavorazione','Risolto'];"
        "var markers={};"

        /* Update report status via POST — shows alert on server-side refusal */
        "function setStatus(reportId,status){"
        "fetch('/api/report/status',{"
        "method:'POST',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:'report_id='+reportId+'&status='+status"
        "}).then(function(r){"
        "if(r.ok){loadReports();}"
        "else{r.json().then(function(e){alert('Errore: '+(e.error||r.status));}).catch(function(){alert('Errore '+r.status);});}"
        "}).catch(function(){alert('Errore di rete.');});"
        "}"

        /* Build popup HTML for a report — all fields escaped to prevent XSS */
        "function makePopup(r){"
        "var d=new Date(r.created_at*1000).toLocaleDateString('it-IT');"
        "var h='<b>'+esc(r.category)+'</b><br>'+esc(r.description)+'<br>"
        "<small style=\"color:#6b7280\">'+esc(r.city)+' &bull; '+d+'</small><br>"
        "<b>Stato:</b> <span class=\"badge badge-'+r.status+'\">'+STATUS[r.status]+'</span>"
        "<div class=\"popup-actions\">';"
        "if(r.status===0)h+='<button class=\"btn-takeon\" onclick=\"setStatus('+r.id+',1)\">Prendi in carico</button>';"
        "if(r.status===1)h+='<button class=\"btn-resolve\" onclick=\"setStatus('+r.id+',2)\">Segna risolto</button>';"
        "h+='</div>';"
        "return h;"
        "}"

        /* Fetch and render all active reports */
        "function loadReports(){"
        "fetch('/api/reports/active').then(function(r){return r.json();}).then(function(reports){"

        /* Clear old markers */
        "for(var id in markers){map.removeLayer(markers[id]);} markers={};"

        /* Rebuild sidebar list */
        "var list=document.getElementById('sb-list');"
        "if(!reports.length){list.innerHTML='<div class=\"loading\">Nessuna segnalazione attiva.</div>';return;}"
        "list.innerHTML='';"
        "var bounds=[];"

        "reports.forEach(function(r){"
        /* Sidebar item — fields escaped to prevent stored XSS */
        "var item=document.createElement('div');"
        "item.className='report-item';"
        "item.innerHTML='<div class=\"cat\">'+esc(r.category)+"
        "' <span class=\"badge badge-'+r.status+'\">'+STATUS[r.status]+'</span></div>"
        "<div class=\"desc\">'+esc(r.description)+'</div>"
        "<div class=\"meta\">#'+r.id+' &bull; '+new Date(r.created_at*1000).toLocaleDateString('it-IT')+'</div>';"

        /* Map marker (only if valid coords) */
        "if(r.lat!==0||r.lon!==0){"
        "var m=L.marker([r.lat,r.lon]).addTo(map);"
        "m.bindPopup(makePopup(r),{minWidth:220});"
        "markers[r.id]=m;"
        "bounds.push([r.lat,r.lon]);"
        "item.onclick=function(){m.openPopup();map.setView([r.lat,r.lon],15);};"
        "}"
        "list.appendChild(item);"
        "});"

        /* Fit map to markers */
        "if(bounds.length)map.fitBounds(bounds,{padding:[30,30]});"
        "}).catch(function(){document.getElementById('sb-list').innerHTML="
        "'<div class=\"loading\">Errore di caricamento.</div>';});"
        "}"

        "loadReports();"
        "setInterval(loadReports,30000);" /* auto-refresh every 30 s */
        "</script>"
        "</body></html>",
        esc_city, esc_user, esc_city);
}

/* ══════════════════════════════════════════════════════════════════════
   ROUTE HANDLERS
   ══════════════════════════════════════════════════════════════════════ */

/* Helper: redirect with optional cookie */
static int redirect(RouteExtra *extra, const char *url,const char *cookie) {
    snprintf(extra->location, sizeof(extra->location), "%s", url);
    if (cookie)
        snprintf(extra->set_cookie, sizeof(extra->set_cookie), "%s", cookie);
    return 302;
}

/* GET / — login page (or redirect to /home if already logged in) */
static int route_get_root(const char *req,
                           char *resp, size_t max, RouteExtra *extra) {
    User u = {0};
    if (get_session_user(req, &u))
        return redirect(extra, "/home", NULL);
    page_login(resp, max, "");
    return 200;
}

/* GET /register */
static int route_get_register(const char *req,
                               char *resp, size_t max, RouteExtra *extra) {
 (void)req; (void)extra;
    page_register(resp, max, "");
    return 200;
}

/* GET /home — citizen dashboard or operator map based on role */
static int route_get_home(const char *req,
                          char *resp, size_t max, RouteExtra *extra) {
    User u = {0};
    if (!get_session_user(req, &u))
        return redirect(extra, "/", NULL);

    if (user_is_operator(&u))
        page_operator_map(resp, max, &u);
    else
        page_citizen_home(resp, max, &u);
    return 200;
}

/* GET /submit — report submission form */
static int route_get_submit(const char *req,
                             char *resp, size_t max, RouteExtra *extra) {
    User u = {0};
    if (!get_session_user(req, &u))
        return redirect(extra, "/", NULL);
    if (user_is_operator(&u))
        return redirect(extra, "/home", NULL);
    page_submit(resp, max, &u, "");
    return 200;
}

/* POST /login */
static int route_post_login(const char *req,
                             char *resp, size_t max, RouteExtra *extra) {
    const char *body = post_body(req);
    char username[USERNAME_LEN] = {0};
    char password[128]          = {0};
    get_field(body, "username=", username, sizeof(username));
    get_field(body, "password=", password, sizeof(password));

    User u = {0};
    if (!user_authenticate(username, password, &u)) {
        char alert[256];
        snprintf(alert, sizeof(alert),
                 "<div class='alert alert-err'>Username o password errati.</div>");
        page_login(resp, max, alert);
        return 200;
    }

    char token[TOKEN_HEX_LEN + 2];
    if (!session_create(g_sessions, u.userId, token)) {
        page_login(resp, max,
                   "<div class='alert alert-err'>Errore interno. Riprova.</div>");
        return 200;
    }

    char cookie[COOKIE_MAX];
    snprintf(cookie, sizeof(cookie),
             "%s=%s; Path=/; HttpOnly; Max-Age=%d; SameSite=Lax",
             SESSION_COOKIE_NAME, token, SESSION_MAX_AGE);
    return redirect(extra, "/home", cookie);
}

/* POST /register */
static int route_post_register(const char *req,
                                char *resp, size_t max, RouteExtra *extra) {
    const char *body = post_body(req);
    char username[USERNAME_LEN] = {0};
    char password[128]          = {0};
    char city[CITY_LEN]         = {0};
    char role_str[4]            = {0};
    get_field(body, "username=", username, sizeof(username));
    get_field(body, "password=", password, sizeof(password));
    get_field(body, "city=",     city,     sizeof(city));
    get_field(body, "role=",     role_str, sizeof(role_str));

    if (!username[0] || !password[0] || !city[0]) {
        page_register(resp, max,
                      "<div class='alert alert-err'>Tutti i campi sono obbligatori.</div>");
        return 200;
    }
    if (strlen(password) < 6) {
        page_register(resp, max,
                      "<div class='alert alert-err'>La password deve avere almeno 6 caratteri.</div>");
        return 200;
    }

    UserRole role = (role_str[0] == '1') ? ROLE_OPERATOR : ROLE_CITIZEN;
    if (user_register(username, password, city, role) != 0) {
        page_register(resp, max,
                      "<div class='alert alert-err'>Username già in uso. Scegline un altro.</div>");
        return 200;
    }
    return redirect(extra, "/", NULL);
}

/* GET /logout */
static int route_get_logout(const char *req,
                             char *resp, size_t max, RouteExtra *extra) {
    (void)resp; (void)max;
    char token[TOKEN_HEX_LEN + 2];
    parse_cookie(req, SESSION_COOKIE_NAME, token, sizeof(token));
    if (token[0]) session_destroy(g_sessions, token);
    /* Expire the cookie in the browser */
    char cookie[COOKIE_MAX];
    snprintf(cookie, sizeof(cookie),
             "%s=; Path=/; HttpOnly; Max-Age=0; SameSite=Lax", SESSION_COOKIE_NAME);
    return redirect(extra, "/", cookie);
}

/* POST /submit — citizen submits a new report */
static int route_post_submit(const char *req,
                              char *resp, size_t max, RouteExtra *extra) {
    User u = {0};
    if (!get_session_user(req, &u))
        return redirect(extra, "/", NULL);
    if (user_is_operator(&u))
        return redirect(extra, "/home", NULL);

    const char *body = post_body(req);
    char category[CAT_LEN]  = {0};
    char desc[DESC_LEN]     = {0};
    char lat_s[32]          = {0};
    char lon_s[32]          = {0};
    get_field(body, "category=",    category, sizeof(category));
    get_field(body, "description=", desc,     sizeof(desc));
    get_field(body, "lat=",         lat_s,    sizeof(lat_s));
    get_field(body, "lon=",         lon_s,    sizeof(lon_s));

    if (!desc[0]) {
        page_submit(resp, max, &u,
                    "<div class='alert alert-err'>La descrizione è obbligatoria.</div>");
        return 200;
    }

    double lat = lat_s[0] ? atof(lat_s) : 0.0;
    double lon = lon_s[0] ? atof(lon_s) : 0.0;

    uint64_t rid = report_insert(u.userId, lat, lon,
                                 u.city, category[0] ? category : "Altro", desc);
    if (rid == 0) {
        page_submit(resp, max, &u,
                    "<div class='alert alert-err'>Errore interno. Riprova.</div>");
        return 200;
    }
    return redirect(extra, "/home", NULL);
}

/* ── JSON API handlers ───────────────────────────────────────────────── */

/* GET /api/reports/active */
static int route_api_reports_active(const char *req,
                                    char *resp, size_t max, RouteExtra *extra) {
    (void)extra;
    User u = {0};
    if (!get_session_user(req, &u)) {
        snprintf(resp, max, "{\"error\":\"unauthorized\"}");
        return 401;
    }
    char *json = report_get_active_json(u.userId, u.city, user_is_operator(&u));
    if (!json) { snprintf(resp, max, "[]"); return 500; }
    size_t jlen = strlen(json);
    if (jlen >= max) {
        free(json);
        snprintf(resp, max, "{\"error\":\"response too large\"}");
        return 500;
    }
    memcpy(resp, json, jlen + 1);
    free(json);
    return 200;
}

/* GET /api/reports/archived */
static int route_api_reports_archived(const char *req,
                                      char *resp, size_t max, RouteExtra *extra) {
    (void)extra;
    User u = {0};
    if (!get_session_user(req, &u)) {
        snprintf(resp, max, "{\"error\":\"unauthorized\"}");
        return 401;
    }
    char *json = report_get_archived_json(u.userId, u.city, user_is_operator(&u));
    if (!json) { snprintf(resp, max, "[]"); return 500; }
    size_t jlen = strlen(json);
    if (jlen >= max) {
        free(json);
        snprintf(resp, max, "{\"error\":\"response too large\"}");
        return 500;
    }
    memcpy(resp, json, jlen + 1);
    free(json);
    return 200;
}

/* GET /api/stats */
static int route_api_stats(const char *req,
                           char *resp, size_t max, RouteExtra *extra) {
    (void)req; (void)extra;
    extern unsigned long g_stat_requests, g_stat_connections;
    extern time_t        g_stat_start;
    time_t uptime = time(NULL) - g_stat_start;
    snprintf(resp, max,
        "{\"uptime\":%ld,\"active_reports\":%d,"
        "\"requests\":%lu,\"connections\":%lu}",
        (long)uptime, report_count_active(),
        g_stat_requests, g_stat_connections);
    return 200;
}

/* POST /api/report/status — operator updates a report's status */
static int route_api_report_status(const char *req,
                                   char *resp, size_t max, RouteExtra *extra) {
    (void)extra;
    User u = {0};
    if (!get_session_user(req, &u) || !user_is_operator(&u)) {
        snprintf(resp, max, "{\"error\":\"forbidden\"}");
        return 403;
    }

    const char *body = post_body(req);
    char rid_s[24]  = {0};
    char stat_s[4]  = {0};
    get_field(body, "report_id=", rid_s,  sizeof(rid_s));
    get_field(body, "status=",    stat_s, sizeof(stat_s));

    if (!rid_s[0] || !stat_s[0]) {
        snprintf(resp, max, "{\"error\":\"missing params\"}");
        return 400;
    }

    uint64_t reportId  = (uint64_t)strtoull(rid_s, NULL, 10);
    int      newStatus = atoi(stat_s);

    /* Fetch report to enforce city-level authorisation */
    ActiveReport r;
    if (!report_get_by_id(reportId, &r)) {
        snprintf(resp, max, "{\"error\":\"not found\"}");
        return 404;
    }
    /* An operator may only manage reports belonging to their own city */
    if (strncmp(r.city, u.city, CITY_LEN) != 0) {
        snprintf(resp, max, "{\"error\":\"forbidden\"}");
        return 403;
    }

    if (newStatus == STATUS_RESOLVED) {
        int rc = report_resolve(reportId, u.userId);
        if (rc == 0) {
            snprintf(resp, max, "{\"error\":\"not authorized or already resolved\"}");
            return 403;
        }
        if (rc < 0) {
            snprintf(resp, max, "{\"error\":\"db error\"}");
            return 500;
        }
    } else if (newStatus == STATUS_IN_PROGRESS) {
        int rc = report_assign(reportId, u.userId);
        if (rc == 0) {
            snprintf(resp, max, "{\"error\":\"report already taken\"}");
            return 409;
        }
        if (rc < 0) {
            snprintf(resp, max, "{\"error\":\"db error\"}");
            return 500;
        }
    } else {
        snprintf(resp, max, "{\"error\":\"invalid status\"}");
        return 400;
    }

    snprintf(resp, max, "{\"ok\":true}");
    return 200;
}

/* ══════════════════════════════════════════════════════════════════════
   DISPATCH TABLE  +  handle_request
   ══════════════════════════════════════════════════════════════════════ */

static Route routes[] = {
    /* method   path                        handler */
    {"GET",  "/",                       route_get_root             },
    {"GET",  "/home",                   route_get_home             },
    {"GET",  "/register",               route_get_register         },
    {"GET",  "/submit",                 route_get_submit           },
    {"GET",  "/logout",                 route_get_logout           },
    {"POST", "/login",                  route_post_login           },
    {"POST", "/register",               route_post_register        },
    {"POST", "/submit",                 route_post_submit          },
    {"GET",  "/api/reports/active",     route_api_reports_active   },
    {"GET",  "/api/reports/archived",   route_api_reports_archived },
    {"GET",  "/api/stats",              route_api_stats            },
    {"POST", "/api/report/status",      route_api_report_status    },
};
static const size_t NUM_ROUTES = sizeof(routes) / sizeof(routes[0]);

/* Extracts method and URL path from the first request line into out buffers. */
static void parse_request_line(const char *req,
                                char *method, size_t mmax,
                                char *path,   size_t pmax) {
    method[0] = path[0] = '\0';
    /* method */
    size_t i = 0;
    while (*req && *req != ' ' && i < mmax - 1) method[i++] = *req++;
    method[i] = '\0';
    while (*req == ' ') req++;
    /* path (stop at '?' or ' ' or end) */
    i = 0;
    while (*req && *req != ' ' && *req != '?' && *req != '\r' && *req != '\n' && i < pmax - 1)
        path[i++] = *req++;
    path[i] = '\0';
}

int handle_request(const char *req,
                   char *resp, size_t respMax,
                   RouteExtra *extra, int *keepAlive) {

    resp[0] = '\0';
    memset(extra, 0, sizeof(*extra));
    *keepAlive = (strstr(req, "Connection: keep-alive") != NULL) ? 1 : 0;

    char method[8]  = {0};
    char path[URL_BUFFER_SIZE] = {0};
    parse_request_line(req, method, sizeof(method), path, sizeof(path));

    if (!method[0]) {
        snprintf(resp, respMax, "<h1>400 Bad Request</h1>");
        return 400;
    }

    /*
     * Two-pass dispatch:
     *   1. Look for an exact (path + method) match → call handler.
     *   2. If the path exists but with a different method → 405.
     *   3. Path not found at all → 404.
     * This avoids returning 405 as soon as the first path match is found
     * with a wrong method (e.g. GET /register found before POST /register).
     */
    int path_matched = 0;
    for (size_t i = 0; i < NUM_ROUTES; i++) {
        if (strcmp(path, routes[i].path) != 0) continue;
        path_matched = 1;
        if (strcmp(routes[i].method, method) != 0) continue;
        return routes[i].handler(req, resp, respMax, extra);
    }

    if (path_matched) {
        snprintf(resp, respMax, "<h1>405 Method Not Allowed</h1>");
        return 405;
    }

    snprintf(resp, respMax,
             "<h1>404 Not Found</h1><p>La pagina <code>%s</code> non esiste.</p>", path);
    return 404;
}