#!/usr/bin/env bash
# =============================================================================
# coverage.sh — Build, test e report HTML di copertura per SegnalaCity
#
# Utilizzo:
#   ./coverage.sh           # esegue: build → avvio server → test → report
#   ./coverage.sh clean     # rimuove tutti gli artefatti di coverage
#
# Requisiti: gcc, lcov, genhtml, curl
# =============================================================================

set -euo pipefail

# ── Configurazione ────────────────────────────────────────────────────────────

TARGET="segnalacity_cov"
PORT=8080                           # porta separata per non collidere con un'istanza in esecuzione
BASE="http://127.0.0.1:${PORT}"
REPORT_DIR="coverage_html"
LCOV_INFO="coverage.info"
LCOV_FILTERED="coverage_filtered.info"
DB_FILE="segnalacity.db"
LOG_FILE="coverage_server.log"

CC="gcc"
CFLAGS="-fprofile-arcs -ftest-coverage -O0 -g -Wall -I. -I/usr/include/node -D_GNU_SOURCE"
CFLAGS="${CFLAGS} -DPORT=${PORT} -DAPP_DB_PATH='\"${DB_FILE}\"'"
LDFLAGS="-luv -lpthread -lsqlite3 -lcjson -ldl -lrt"

SRCS="http_utils.c server_functions.c report.c route_handler.c template.c \
      route_api.c route_pages.c route_helpers.c hash_table.c db.c user.c \
      session.c slab_allocator.c connection_manager.c geo.c picohttpparser.c"

# Colori ANSI
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'

# ── Helper functions ──────────────────────────────────────────────────────────

log()  { echo -e "${CYAN}[cov]${RESET} $*"; }
ok()   { echo -e "${GREEN}[ok ]${RESET} $*"; }
warn() { echo -e "${YELLOW}[wrn]${RESET} $*"; }
fail() { echo -e "${RED}[err]${RESET} $*"; }

# check <label> <expected_http_status> <curl_args…>
# Invia una richiesta curl e stampa PASS/FAIL in base allo status atteso.
check() {
    local label="$1" expected="$2"; shift 2
    local status
    status=$(curl -s -o /dev/null -w "%{http_code}" "$@") || true
    if [[ "$status" == "$expected" ]]; then
        ok "  [${label}] → HTTP ${status}"
    else
        warn "  [${label}] → HTTP ${status} (atteso ${expected})"
    fi
}

# request <label> <expected_http_status> <curl_args…>
# Come check, ma salva anche il corpo della risposta in CURL_BODY.
CURL_BODY=""; CURL_STATUS=""
request() {
    local label="$1" expected="$2"; shift 2
    local raw
    raw=$(curl -s -w "\n__STATUS__%{http_code}" "$@") || true
    CURL_STATUS="${raw##*__STATUS__}"
    CURL_BODY="${raw%__STATUS__*}"
    if [[ "$CURL_STATUS" == "$expected" ]]; then
        ok "  [${label}] → HTTP ${CURL_STATUS}"
    else
        warn "  [${label}] → HTTP ${CURL_STATUS} (atteso ${expected})"
    fi
}

# ── Pulizia ───────────────────────────────────────────────────────────────────

if [[ "${1:-}" == "clean" ]]; then
    log "Rimozione artefatti di coverage..."
    rm -f "${TARGET}" "${LCOV_INFO}" "${LCOV_FILTERED}" "${LOG_FILE}" "${DB_FILE}"
    rm -f ./*.gcda ./*.gcno
    rm -rf "${REPORT_DIR}"
    ok "Pulizia completata."
    exit 0
fi

# ── Step 1: Build ─────────────────────────────────────────────────────────────

echo -e "\n${BOLD}━━━ Step 1/4: Build con strumentazione coverage ━━━${RESET}"

# Rimuovi i .gcda obsoleti per azzerare i contatori ad ogni esecuzione.
rm -f ./*.gcda

log "Compilazione ${TARGET}..."
# shellcheck disable=SC2086
eval "${CC} ${CFLAGS} ${SRCS} ${LDFLAGS} -o ${TARGET}"
ok "Build completato."

# ── Step 2: Avvio server ──────────────────────────────────────────────────────

echo -e "\n${BOLD}━━━ Step 2/4: Avvio server strumentato ━━━${RESET}"

rm -f "${DB_FILE}"

if lsof -ti:"${PORT}" >/dev/null 2>&1; then
    fail "La porta ${PORT} è già in uso. Terminare il processo o cambiare PORT nello script."
    exit 1
fi

log "Avvio ${TARGET} sulla porta ${PORT}..."
./"${TARGET}" >"${LOG_FILE}" 2>&1 &
SERVER_PID=$!

# Attendi che il server accetti connessioni (massimo 5 secondi).
for i in $(seq 1 20); do
    if curl -sf "${BASE}/" -o /dev/null 2>/dev/null; then
        ok "Server pronto (pid=${SERVER_PID})."
        break
    fi
    sleep 0.25
    if [[ $i -eq 20 ]]; then
        fail "Il server non si è avviato in tempo. Log:"
        cat "${LOG_FILE}"
        exit 1
    fi
done

# Garantisce l'arresto del server all'uscita (normale o per errore).
cleanup() {
    if [[ "${SERVER_PID}" -ne 0 ]]; then
        log "Arresto server (pid=${SERVER_PID})..."
        kill -SIGINT "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
        ok "Server arrestato."
    fi
    rm -f "${JAR_CITIZEN:-}" "${JAR_OPERATOR:-}" "${JAR_ADMIN:-}"
}
trap cleanup EXIT

# Cookie jar temporanei per la gestione delle sessioni.
JAR_CITIZEN="/tmp/cov_citizen_$$.jar"
JAR_OPERATOR="/tmp/cov_operator_$$.jar"
JAR_ADMIN="/tmp/cov_admin_$$.jar"
rm -f "${JAR_CITIZEN}" "${JAR_OPERATOR}" "${JAR_ADMIN}"

# ── Step 3: Suite di test HTTP ────────────────────────────────────────────────

echo -e "\n${BOLD}━━━ Step 3/4: Suite di test HTTP ━━━${RESET}"

# ── 3.1 Route pubbliche / non autenticate ─────────────────────────────────────
echo -e "\n  ${BOLD}§ Route pubbliche${RESET}"

check "GET /"                  200  "${BASE}/"
check "GET /register"          200  "${BASE}/register"
check "GET /static/common.css" 200  "${BASE}/static/common.css"
check "GET /api/cities"        200  "${BASE}/api/cities"
check "GET /nonexistent"       404  "${BASE}/nonexistent"
check "POST / → 405"           405  -X POST "${BASE}/"

# ── 3.2 Login con credenziali errate ─────────────────────────────────────────
echo -e "\n  ${BOLD}§ Autenticazione — credenziali errate${RESET}"

check "POST /login bad creds"  200  -X POST "${BASE}/login" \
    --data "username=nobody&password=wrong"

# ── 3.3 Registrazione utenti ─────────────────────────────────────────────────
echo -e "\n  ${BOLD}§ Registrazione${RESET}"

CITY="Roma"

check "POST /register cittadino"  302 -X POST "${BASE}/register" \
    --data "username=test_citizen&password=secret123&city=${CITY}&role=0"

check "POST /register operatore"  302 -X POST "${BASE}/register" \
    --data "username=test_operator&password=secret123&city=${CITY}&role=1"

check "POST /register admin"      302 -X POST "${BASE}/register" \
    --data "username=test_admin&password=secret123&city=${CITY}&role=2"

check "POST /register dup username" 200 -X POST "${BASE}/register" \
    --data "username=test_citizen&password=secret123&city=${CITY}&role=0"

check "POST /register campi vuoti"  200 -X POST "${BASE}/register" \
    --data "username=&password=&city="

check "POST /register pwd corta"    200 -X POST "${BASE}/register" \
    --data "username=tmpuser&password=ab&city=${CITY}&role=0"

check "POST /register secondo admin (dup)" 200 -X POST "${BASE}/register" \
    --data "username=test_admin2&password=secret123&city=${CITY}&role=2"

check "POST /register comune non valido" 200 -X POST "${BASE}/register" \
    --data "username=notown&password=secret123&city=CittaInesistente&role=0"

# ── 3.4 Login — acquisizione sessioni ─────────────────────────────────────────
echo -e "\n  ${BOLD}§ Login — acquisizione sessioni${RESET}"

check "POST /login cittadino"  302 -c "${JAR_CITIZEN}"  -X POST "${BASE}/login" \
    --data "username=test_citizen&password=secret123"

check "POST /login operatore"  302 -c "${JAR_OPERATOR}" -X POST "${BASE}/login" \
    --data "username=test_operator&password=secret123"

check "POST /login admin"      302 -c "${JAR_ADMIN}"    -X POST "${BASE}/login" \
    --data "username=test_admin&password=secret123"

# ── 3.5 Route pagine — autenticate ────────────────────────────────────────────
echo -e "\n  ${BOLD}§ Route pagine — autenticate${RESET}"

check "GET /home cittadino"      200 -b "${JAR_CITIZEN}"  "${BASE}/home"
check "GET /home operatore"      200 -b "${JAR_OPERATOR}" "${BASE}/home"
check "GET /home admin"          200 -b "${JAR_ADMIN}"    "${BASE}/home"
check "GET /submit cittadino"    200 -b "${JAR_CITIZEN}"  "${BASE}/submit"
check "GET /submit operatore → redirect" 302 -b "${JAR_OPERATOR}" "${BASE}/submit"
check "GET /submit admin → redirect"     302 -b "${JAR_ADMIN}"    "${BASE}/submit"
check "GET /home non auth → redirect"    302  "${BASE}/home"
check "GET /submit non auth → redirect"  302  "${BASE}/submit"
check "GET / cittadino → /home"  302 -b "${JAR_CITIZEN}" "${BASE}/"

# ── 3.6 Invio segnalazioni (POST /submit) ─────────────────────────────────────
echo -e "\n  ${BOLD}§ Invio segnalazioni${RESET}"

check "POST /submit ok"            302 -b "${JAR_CITIZEN}" -X POST "${BASE}/submit" \
    --data "category=Strade&description=Buca+profonda+in+via+Roma&lat=41.9&lon=12.5"

check "POST /submit no descrizione" 200 -b "${JAR_CITIZEN}" -X POST "${BASE}/submit" \
    --data "category=Strade&description=&lat=41.9&lon=12.5"

check "POST /submit coordinate fuori bounding box" 200 -b "${JAR_CITIZEN}" -X POST "${BASE}/submit" \
    --data "category=Strade&description=Test&lat=0.0&lon=0.0"

check "POST /submit come operatore → redirect" 302 -b "${JAR_OPERATOR}" -X POST "${BASE}/submit" \
    --data "category=Strade&description=Test&lat=41.9&lon=12.5"

# Seconda segnalazione per i test successivi
check "POST /submit #2"            302 -b "${JAR_CITIZEN}" -X POST "${BASE}/submit" \
    --data "category=Verde&description=Erba+alta&lat=41.9&lon=12.5"

# ── 3.7 API — report ──────────────────────────────────────────────────────────
echo -e "\n  ${BOLD}§ API — /api/reports${RESET}"

check "GET /api/reports/active  cittadino"    200 -b "${JAR_CITIZEN}"  "${BASE}/api/reports/active"
check "GET /api/reports/active  operatore"    200 -b "${JAR_OPERATOR}" "${BASE}/api/reports/active"
check "GET /api/reports/archived cittadino"   200 -b "${JAR_CITIZEN}"  "${BASE}/api/reports/archived"
check "GET /api/reports/archived operatore"   200 -b "${JAR_OPERATOR}" "${BASE}/api/reports/archived"
check "GET /api/reports/all  admin"           200 -b "${JAR_ADMIN}"    "${BASE}/api/reports/all"
check "GET /api/reports/active non auth → 401" 401  "${BASE}/api/reports/active"
check "GET /api/reports/all non-admin → 403"  403 -b "${JAR_CITIZEN}" "${BASE}/api/reports/all"

# ── 3.8 API — transizioni di stato dei report ─────────────────────────────────
echo -e "\n  ${BOLD}§ API — /api/report/status${RESET}"

request "fetch segnalazioni attive" 200 -b "${JAR_CITIZEN}" "${BASE}/api/reports/active"
REPORT_ID=$(echo "${CURL_BODY}" | grep -oP '"id":\K[0-9]+' | head -1 || echo "")

if [[ -n "${REPORT_ID}" ]]; then
    ok "  Trovato report id=${REPORT_ID}"

    check "assign → 200"                 200 \
        -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/status" \
        --data "report_id=${REPORT_ID}&status=1"

    check "assign come cittadino → 403"  403 \
        -b "${JAR_CITIZEN}" -X POST "${BASE}/api/report/status" \
        --data "report_id=${REPORT_ID}&status=1"

    check "assegna di nuovo (già preso → 409)" 409 \
        -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/status" \
        --data "report_id=${REPORT_ID}&status=1"

    check "risolvi → 200"               200 \
        -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/status" \
        --data "report_id=${REPORT_ID}&status=2"

    check "status non valido → 400"     400 \
        -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/status" \
        --data "report_id=${REPORT_ID}&status=99"

    check "parametri mancanti → 400"    400 \
        -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/status" \
        --data "report_id=${REPORT_ID}"

    check "report inesistente → 404"    404 \
        -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/status" \
        --data "report_id=999999&status=1"
else
    warn "  Nessun report trovato; test di transizione saltati."
fi

# ── 3.9 API — operatori e assegnazione admin ───────────────────────────────────
echo -e "\n  ${BOLD}§ API — /api/operators e /api/admin/assign${RESET}"

check "GET /api/operators admin"         200 -b "${JAR_ADMIN}"    "${BASE}/api/operators"
check "GET /api/operators non-admin 403" 403 -b "${JAR_CITIZEN}"  "${BASE}/api/operators"

request "fetch lista operatori" 200 -b "${JAR_ADMIN}" "${BASE}/api/operators"
OPERATOR_ID=$(echo "${CURL_BODY}" | grep -oP '"id":\K[0-9]+' | head -1 || echo "")

request "fetch segnalazioni per admin-assign" 200 -b "${JAR_CITIZEN}" "${BASE}/api/reports/active"
REPORT_ID2=$(echo "${CURL_BODY}" | grep -oP '"id":\K[0-9]+' | head -1 || echo "")

if [[ -n "${OPERATOR_ID}" && -n "${REPORT_ID2}" ]]; then
    ok "  operatore id=${OPERATOR_ID}, report id=${REPORT_ID2}"

    check "admin/assign ok → 200"        200 \
        -b "${JAR_ADMIN}" -X POST "${BASE}/api/admin/assign" \
        --data "report_id=${REPORT_ID2}&operator_id=${OPERATOR_ID}"

    check "admin/assign già preso → 409" 409 \
        -b "${JAR_ADMIN}" -X POST "${BASE}/api/admin/assign" \
        --data "report_id=${REPORT_ID2}&operator_id=${OPERATOR_ID}"

    check "admin/assign report inesistente → 404" 404 \
        -b "${JAR_ADMIN}" -X POST "${BASE}/api/admin/assign" \
        --data "report_id=999999&operator_id=${OPERATOR_ID}"

    check "admin/assign operatore non valido → 400" 400 \
        -b "${JAR_ADMIN}" -X POST "${BASE}/api/admin/assign" \
        --data "report_id=${REPORT_ID2}&operator_id=999999"

    check "admin/assign parametri mancanti → 400" 400 \
        -b "${JAR_ADMIN}" -X POST "${BASE}/api/admin/assign" \
        --data "report_id=${REPORT_ID2}"
else
    warn "  Salto admin-assign: operatore o report non trovato."
fi

check "admin/assign come cittadino → 403" 403 \
    -b "${JAR_CITIZEN}" -X POST "${BASE}/api/admin/assign" \
    --data "report_id=1&operator_id=1"

# ── 3.10 Logout ───────────────────────────────────────────────────────────────
echo -e "\n  ${BOLD}§ Logout${RESET}"

check "GET /logout cittadino"  302 -b "${JAR_CITIZEN}"  "${BASE}/logout"
check "GET /logout operatore"  302 -b "${JAR_OPERATOR}" "${BASE}/logout"
check "GET /logout admin"      302 -b "${JAR_ADMIN}"    "${BASE}/logout"
check "GET /home dopo logout → redirect" 302 -b "${JAR_CITIZEN}" "${BASE}/home"

# ── 3.11 Method not allowed ────────────────────────────────────────────────────
echo -e "\n  ${BOLD}§ Method not allowed${RESET}"

check "DELETE /home → 405"    405 -X DELETE "${BASE}/home"
check "PUT /register → 405"   405 -X PUT    "${BASE}/register"

# ── Arresto server per flush dei .gcda ────────────────────────────────────────
echo -e "\n  ${BOLD}§ Arresto server (flush .gcda)${RESET}"
kill -SIGINT "${SERVER_PID}" 2>/dev/null || true
wait "${SERVER_PID}" 2>/dev/null || true
SERVER_PID=0
ok "Server arrestato — dati coverage salvati su disco."
trap - EXIT
rm -f "${JAR_CITIZEN}" "${JAR_OPERATOR}" "${JAR_ADMIN}"

# ── Step 4: Generazione report HTML ───────────────────────────────────────────

echo -e "\n${BOLD}━━━ Step 4/4: Generazione report HTML di copertura ━━━${RESET}"

log "Raccolta dati con lcov..."
lcov \
    --capture \
    --directory . \
    --output-file "${LCOV_INFO}" \
    --rc branch_coverage=1 \
    --ignore-errors mismatch \
    --quiet

log "Filtro dei sorgenti di sistema e librerie terze..."
lcov \
    --remove "${LCOV_INFO}" \
    '/usr/*' \
    '*/picohttpparser.*' \
    --output-file "${LCOV_FILTERED}" \
    --rc branch_coverage=1 \
    --ignore-errors unused \
    --quiet

log "Generazione HTML in ${REPORT_DIR}/..."
genhtml \
    "${LCOV_FILTERED}" \
    --output-directory "${REPORT_DIR}" \
    --title "SegnalaCity — Coverage Report" \
    --legend \
    --branch-coverage \
    --rc branch_coverage=1 \
    --quiet

# ── Riepilogo a console ───────────────────────────────────────────────────────

echo -e "\n${BOLD}━━━ Riepilogo Coverage ━━━${RESET}"
lcov --summary "${LCOV_FILTERED}" --rc branch_coverage=1 2>&1 | \
    grep -E 'lines|functions|branches' | \
    while IFS= read -r line; do
        pct=$(echo "$line" | grep -oP '[0-9]+\.[0-9]+(?=%)' | head -1 || echo "0")
        if awk "BEGIN{exit !($pct >= 80)}"; then
            echo -e "  ${GREEN}${line}${RESET}"
        elif awk "BEGIN{exit !($pct >= 50)}"; then
            echo -e "  ${YELLOW}${line}${RESET}"
        else
            echo -e "  ${RED}${line}${RESET}"
        fi
    done

echo ""
ok "Report HTML pronto: ${REPORT_DIR}/index.html"
echo -e "  ${CYAN}Apri con:${RESET}  xdg-open ${REPORT_DIR}/index.html"
echo ""