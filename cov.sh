#!/usr/bin/env bash
# =============================================================================
# cov.sh — Build, test e report HTML di copertura per SegnalaCity
#
# Utilizzo:
#   ./cov.sh           # esegue: build → avvio server → test → report
#   ./cov.sh clean     # rimuove tutti gli artefatti di coverage
#
# Requisiti: gcc, lcov, genhtml, curl
#
# Lifecycle report testato:
#   STATUS_ACTIVE (0) → [admin assign]
#   STATUS_ASSIGNED (1) → [operator accept]  STATUS_IN_PROGRESS (2) → [operator resolve]
#                       ↘ [operator reject] → STATUS_ACTIVE (0)      STATUS_RESOLVED (3)
#                                                                            ↓
#                                                               [citizen feedback 1-5★]
# =============================================================================

set -euo pipefail

# ── Configurazione ────────────────────────────────────────────────────────────

TARGET="segnalacity_cov"
PORT=8080                       # porta separata per non collidere con istanze in esecuzione
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

SRCS="connection_handler.c  http_utils.c server.c rate_limiter.c report.c route_handler.c template.c \
      route_api.c route_pages.c route_helpers.c hash_table.c db.c user.c \
      session.c slab_allocator.c client_manager.c geo.c picohttpparser.c"

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
# Come check, ma salva anche il corpo della risposta in CURL_BODY / CURL_STATUS.
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
    rm -f "${JAR_CITIZEN:-}" "${JAR_OPERATOR:-}" "${JAR_OPERATOR2:-}" "${JAR_ADMIN:-}"
}
trap cleanup EXIT

# Cookie jar temporanei per la gestione delle sessioni.
# Due operatori separati per testare guard cross-operatore.
JAR_CITIZEN="/tmp/cov_citizen_$$.jar"
JAR_OPERATOR="/tmp/cov_operator_$$.jar"
JAR_OPERATOR2="/tmp/cov_operator2_$$.jar"
JAR_ADMIN="/tmp/cov_admin_$$.jar"
rm -f "${JAR_CITIZEN}" "${JAR_OPERATOR}" "${JAR_OPERATOR2}" "${JAR_ADMIN}"

# ── Step 3: Suite di test HTTP ────────────────────────────────────────────────

echo -e "\n${BOLD}━━━ Step 3/4: Suite di test HTTP ━━━${RESET}"

# ── 3.1 Route pubbliche / non autenticate ─────────────────────────────────────
echo -e "\n  ${BOLD}§ 3.1 Route pubbliche${RESET}"

check "GET /"                  200 "${BASE}/"
check "GET /register"          200 "${BASE}/register"
check "GET /static/common.css" 200 "${BASE}/static/common.css"
check "GET /api/cities"        200 "${BASE}/api/cities"
check "GET /nonexistent"       404 "${BASE}/nonexistent"
check "POST / → 405"           405 -X POST "${BASE}/"

# ── 3.2 Login con credenziali errate ─────────────────────────────────────────
echo -e "\n  ${BOLD}§ 3.2 Autenticazione — credenziali errate${RESET}"

check "POST /login bad creds"      200 -X POST "${BASE}/login" --data "username=nobody&password=wrong"
check "POST /login username vuoto" 200 -X POST "${BASE}/login" --data "username=&password=secret"
check "POST /login password vuota" 200 -X POST "${BASE}/login" --data "username=test_citizen&password="

# ── 3.3 Registrazione utenti ─────────────────────────────────────────────────
echo -e "\n  ${BOLD}§ 3.3 Registrazione${RESET}"

CITY="Roma"

check "POST /register cittadino"              302 -X POST "${BASE}/register" \
    --data "username=test_citizen&password=secret123&city=${CITY}&role=0"
check "POST /register operatore"              302 -X POST "${BASE}/register" \
    --data "username=test_operator&password=secret123&city=${CITY}&role=1"
check "POST /register operatore2"             302 -X POST "${BASE}/register" \
    --data "username=test_operator2&password=secret123&city=${CITY}&role=1"
check "POST /register admin"                  302 -X POST "${BASE}/register" \
    --data "username=test_admin&password=secret123&city=${CITY}&role=2"
check "POST /register dup username"           200 -X POST "${BASE}/register" \
    --data "username=test_citizen&password=secret123&city=${CITY}&role=0"
check "POST /register campi vuoti"            200 -X POST "${BASE}/register" \
    --data "username=&password=&city="
check "POST /register pwd corta"              200 -X POST "${BASE}/register" \
    --data "username=tmpuser&password=ab&city=${CITY}&role=0"
check "POST /register secondo admin (dup)"    200 -X POST "${BASE}/register" \
    --data "username=test_admin2&password=secret123&city=${CITY}&role=2"
check "POST /register comune non valido"      200 -X POST "${BASE}/register" \
    --data "username=notown&password=secret123&city=CittaInesistente&role=0"

# ── 3.4 Login — acquisizione sessioni ─────────────────────────────────────────
echo -e "\n  ${BOLD}§ 3.4 Login — acquisizione sessioni${RESET}"

check "POST /login cittadino"  302 -c "${JAR_CITIZEN}"   -X POST "${BASE}/login" \
    --data "username=test_citizen&password=secret123"
check "POST /login operatore"  302 -c "${JAR_OPERATOR}"  -X POST "${BASE}/login" \
    --data "username=test_operator&password=secret123"
check "POST /login operatore2" 302 -c "${JAR_OPERATOR2}" -X POST "${BASE}/login" \
    --data "username=test_operator2&password=secret123"
check "POST /login admin"      302 -c "${JAR_ADMIN}"     -X POST "${BASE}/login" \
    --data "username=test_admin&password=secret123"

# ── 3.5 Route pagine — autenticate ────────────────────────────────────────────
echo -e "\n  ${BOLD}§ 3.5 Route pagine — autenticate${RESET}"

check "GET /home cittadino"              200 -b "${JAR_CITIZEN}"   "${BASE}/home"
check "GET /home operatore"              200 -b "${JAR_OPERATOR}"  "${BASE}/home"
check "GET /home admin"                  200 -b "${JAR_ADMIN}"     "${BASE}/home"
check "GET /submit cittadino"            200 -b "${JAR_CITIZEN}"   "${BASE}/submit"
check "GET /submit operatore → redirect" 302 -b "${JAR_OPERATOR}"  "${BASE}/submit"
check "GET /submit admin → redirect"     302 -b "${JAR_ADMIN}"     "${BASE}/submit"
check "GET /home non auth → redirect"    302 "${BASE}/home"
check "GET /submit non auth → redirect"  302 "${BASE}/submit"
check "GET / cittadino → /home"          302 -b "${JAR_CITIZEN}"   "${BASE}/"

# ── 3.6 Invio segnalazioni (POST /submit) ─────────────────────────────────────
echo -e "\n  ${BOLD}§ 3.6 Invio segnalazioni${RESET}"

# Tre report per coprire percorsi diversi:
#   RPT_FULL  → percorso completo: assign → accept → resolve → feedback
#   RPT_REJ   → percorso reject:   assign → reject → re-assign → accept → resolve
#   RPT_OP2   → assegnato a operatore2 per test cross-operatore

check "POST /submit RPT_FULL (Strade)"     302 -b "${JAR_CITIZEN}" -X POST "${BASE}/submit" \
    --data "category=Strade&description=Buca+profonda+in+via+Roma&lat=41.9&lon=12.5"
check "POST /submit RPT_REJ (Verde)"       302 -b "${JAR_CITIZEN}" -X POST "${BASE}/submit" \
    --data "category=Verde&description=Erba+alta+nel+parco&lat=41.91&lon=12.51"
check "POST /submit RPT_OP2 (Illuminaz.)"  302 -b "${JAR_CITIZEN}" -X POST "${BASE}/submit" \
    --data "category=Illuminazione&description=Lampione+rotto&lat=41.92&lon=12.52"

# Casi di errore submit
check "POST /submit no descrizione"            200 -b "${JAR_CITIZEN}" -X POST "${BASE}/submit" \
    --data "category=Strade&description=&lat=41.9&lon=12.5"
check "POST /submit coordinate fuori bbox"     200 -b "${JAR_CITIZEN}" -X POST "${BASE}/submit" \
    --data "category=Strade&description=Test&lat=0.0&lon=0.0"
check "POST /submit come operatore → redirect" 302 -b "${JAR_OPERATOR}" -X POST "${BASE}/submit" \
    --data "category=Strade&description=Test&lat=41.9&lon=12.5"
check "POST /submit come admin → redirect"     302 -b "${JAR_ADMIN}" -X POST "${BASE}/submit" \
    --data "category=Strade&description=Test&lat=41.9&lon=12.5"

# ── 3.7 API — lettura report ──────────────────────────────────────────────────
echo -e "\n  ${BOLD}§ 3.7 API — /api/reports (lettura)${RESET}"

check "GET /api/reports/active  cittadino"       200 -b "${JAR_CITIZEN}"   "${BASE}/api/reports/active"
check "GET /api/reports/active  operatore"       200 -b "${JAR_OPERATOR}"  "${BASE}/api/reports/active"
check "GET /api/reports/active  operatore2"      200 -b "${JAR_OPERATOR2}" "${BASE}/api/reports/active"
check "GET /api/reports/archived cittadino"      200 -b "${JAR_CITIZEN}"   "${BASE}/api/reports/archived"
check "GET /api/reports/archived operatore"      200 -b "${JAR_OPERATOR}"  "${BASE}/api/reports/archived"
check "GET /api/reports/all  admin"              200 -b "${JAR_ADMIN}"     "${BASE}/api/reports/all"
check "GET /api/reports/active non auth → 401"   401 "${BASE}/api/reports/active"
check "GET /api/reports/archived non auth → 401" 401 "${BASE}/api/reports/archived"
check "GET /api/reports/all non-admin → 403"     403 -b "${JAR_CITIZEN}"  "${BASE}/api/reports/all"
check "GET /api/reports/all operatore → 403"     403 -b "${JAR_OPERATOR}" "${BASE}/api/reports/all"

# Recupera gli ID dei 3 report.
# I report sono ordinati per created_at DESC → il più recente arriva prima.
# Usiamo sed -n '<N>p' per estrarre il 1°, 2° e 3° "id" dalla risposta JSON.
request "fetch report IDs (citizen active)" 200 -b "${JAR_CITIZEN}" "${BASE}/api/reports/active"
RPT_OP2=$(echo  "${CURL_BODY}" | grep -oP '"id":\K[0-9]+' | sed -n '1p' || echo "")   # Illuminazione (newest)
RPT_REJ=$(echo  "${CURL_BODY}" | grep -oP '"id":\K[0-9]+' | sed -n '2p' || echo "")   # Verde
RPT_FULL=$(echo "${CURL_BODY}" | grep -oP '"id":\K[0-9]+' | sed -n '3p' || echo "")   # Strade (oldest)

if [[ -z "${RPT_FULL}" || -z "${RPT_REJ}" || -z "${RPT_OP2}" ]]; then
    warn "  Non tutti i 3 report trovati (FULL=${RPT_FULL} REJ=${RPT_REJ} OP2=${RPT_OP2})"
    warn "  Alcuni test saranno saltati."
else
    ok "  IDs — RPT_FULL=${RPT_FULL}, RPT_REJ=${RPT_REJ}, RPT_OP2=${RPT_OP2}"
fi

# ── 3.8 API — /api/operators e /api/admin/assign ──────────────────────────────
echo -e "\n  ${BOLD}§ 3.8 API — /api/operators e /api/admin/assign${RESET}"

check "GET /api/operators admin"           200 -b "${JAR_ADMIN}"    "${BASE}/api/operators"
check "GET /api/operators non-admin → 403" 403 -b "${JAR_CITIZEN}"  "${BASE}/api/operators"
check "GET /api/operators operatore → 403" 403 -b "${JAR_OPERATOR}" "${BASE}/api/operators"

# Recupera gli ID degli operatori dalla lista admin.
request "fetch lista operatori" 200 -b "${JAR_ADMIN}" "${BASE}/api/operators"
OP1_ID=$(echo "${CURL_BODY}" | grep -oP '"id":\K[0-9]+' | sed -n '1p' || echo "")
OP2_ID=$(echo "${CURL_BODY}" | grep -oP '"id":\K[0-9]+' | sed -n '2p' || echo "")

if [[ -n "${OP1_ID}" && -n "${OP2_ID}" ]]; then
    ok "  Operatori — op1=${OP1_ID}, op2=${OP2_ID}"
fi

# ── Assegnamenti principali ───────────────────────────────────────────────────
if [[ -n "${RPT_FULL}" && -n "${OP1_ID}" ]]; then
    check "admin/assign RPT_FULL → op1 ok"          200 -b "${JAR_ADMIN}" -X POST "${BASE}/api/admin/assign" \
        --data "report_id=${RPT_FULL}&operator_id=${OP1_ID}"
fi
if [[ -n "${RPT_REJ}" && -n "${OP1_ID}" ]]; then
    check "admin/assign RPT_REJ → op1 ok"           200 -b "${JAR_ADMIN}" -X POST "${BASE}/api/admin/assign" \
        --data "report_id=${RPT_REJ}&operator_id=${OP1_ID}"
fi
if [[ -n "${RPT_OP2}" && -n "${OP2_ID}" ]]; then
    check "admin/assign RPT_OP2 → op2 ok"           200 -b "${JAR_ADMIN}" -X POST "${BASE}/api/admin/assign" \
        --data "report_id=${RPT_OP2}&operator_id=${OP2_ID}"
fi

# ── Edge case assegnamento admin (report in stato ASSIGNED: force-reassign) ───
# Un report ASSIGNED (1) può essere riassegnato dall'admin; IN_PROGRESS/RESOLVED → 409.
if [[ -n "${RPT_FULL}" && -n "${OP1_ID}" ]]; then
    check "admin/assign ASSIGNED → stessa op (force-reassign) ok" 200 \
        -b "${JAR_ADMIN}" -X POST "${BASE}/api/admin/assign" \
        --data "report_id=${RPT_FULL}&operator_id=${OP1_ID}"
fi

# ── Errori parametri e permessi ───────────────────────────────────────────────
if [[ -n "${RPT_FULL}" && -n "${OP1_ID}" ]]; then
    check "admin/assign parametri mancanti → 400"    400 -b "${JAR_ADMIN}" -X POST "${BASE}/api/admin/assign" \
        --data "report_id=${RPT_FULL}"
    check "admin/assign operatore non valido → 400"  400 -b "${JAR_ADMIN}" -X POST "${BASE}/api/admin/assign" \
        --data "report_id=${RPT_FULL}&operator_id=999999"
    check "admin/assign report inesistente → 404"    404 -b "${JAR_ADMIN}" -X POST "${BASE}/api/admin/assign" \
        --data "report_id=999999&operator_id=${OP1_ID}"
fi
check "admin/assign come cittadino → 403"            403 -b "${JAR_CITIZEN}"  -X POST "${BASE}/api/admin/assign" \
    --data "report_id=1&operator_id=1"
check "admin/assign come operatore → 403"            403 -b "${JAR_OPERATOR}" -X POST "${BASE}/api/admin/assign" \
    --data "report_id=1&operator_id=1"

# Stato corrente:
#   RPT_FULL → ASSIGNED (1) a op1
#   RPT_REJ  → ASSIGNED (1) a op1
#   RPT_OP2  → ASSIGNED (1) a op2

# ── 3.9 API — /api/report/respond (accept / reject) ──────────────────────────
echo -e "\n  ${BOLD}§ 3.9 API — /api/report/respond (accept/reject) [NUOVO]${RESET}"

# ── Errori pre-validazione ────────────────────────────────────────────────────
if [[ -n "${RPT_FULL}" ]]; then
    check "respond: body vuoto → 400"               400 -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/respond" \
        --data ""
    check "respond: report_id mancante → 400"       400 -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/respond" \
        --data "action=accept"
    check "respond: action mancante → 400"          400 -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/respond" \
        --data "report_id=${RPT_FULL}"
    check "respond: action non valida → 400"        400 -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/respond" \
        --data "report_id=${RPT_FULL}&action=approve"
    check "respond: report inesistente → 404"       404 -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/respond" \
        --data "report_id=999999&action=accept"
    # Solo operatori possono rispondere
    check "respond: cittadino → 403"                403 -b "${JAR_CITIZEN}"  -X POST "${BASE}/api/report/respond" \
        --data "report_id=${RPT_FULL}&action=accept"
    check "respond: admin → 403"                    403 -b "${JAR_ADMIN}"    -X POST "${BASE}/api/report/respond" \
        --data "report_id=${RPT_FULL}&action=accept"
    # Non autenticato
    check "respond: non auth → 403"                 403 -X POST "${BASE}/api/report/respond" \
        --data "report_id=${RPT_FULL}&action=accept"
fi

# ── Guard cross-operatore: op2 prova ad accettare un report assegnato a op1 ───
if [[ -n "${RPT_FULL}" ]]; then
    check "respond: op2 accept RPT_FULL (non suo) → 409" 409 \
        -b "${JAR_OPERATOR2}" -X POST "${BASE}/api/report/respond" \
        --data "report_id=${RPT_FULL}&action=accept"
    check "respond: op2 reject RPT_FULL (non suo) → 409" 409 \
        -b "${JAR_OPERATOR2}" -X POST "${BASE}/api/report/respond" \
        --data "report_id=${RPT_FULL}&action=reject"
fi

# ── Percorso ACCEPT — RPT_FULL ────────────────────────────────────────────────
if [[ -n "${RPT_FULL}" ]]; then
    # op1 accetta RPT_FULL: ASSIGNED → IN_PROGRESS
    check "respond: op1 accept RPT_FULL → 200"       200 -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/respond" \
        --data "report_id=${RPT_FULL}&action=accept"
    # Secondo accept sullo stesso report (non più ASSIGNED): guard fallisce → 409
    check "respond: op1 accept RPT_FULL doppio → 409" 409 -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/respond" \
        --data "report_id=${RPT_FULL}&action=accept"
    # Reject su un report IN_PROGRESS → 409 (guard vuole status=1)
    check "respond: op1 reject RPT_FULL (IN_PROGRESS) → 409" 409 \
        -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/respond" \
        --data "report_id=${RPT_FULL}&action=reject"
fi

# ── Percorso REJECT — RPT_REJ ────────────────────────────────────────────────
if [[ -n "${RPT_REJ}" ]]; then
    # op1 rifiuta RPT_REJ: ASSIGNED → ACTIVE (rimuove assegnazione)
    check "respond: op1 reject RPT_REJ → 200"        200 -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/respond" \
        --data "report_id=${RPT_REJ}&action=reject"
    # Secondo reject sullo stesso report (ora ACTIVE, non ASSIGNED a op1) → 409
    check "respond: op1 reject RPT_REJ doppio → 409"  409 -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/respond" \
        --data "report_id=${RPT_REJ}&action=reject"
    # Accept su un report ora ACTIVE (non assegnato a nessuno) → 409
    check "respond: op1 accept RPT_REJ (ACTIVE) → 409" 409 -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/respond" \
        --data "report_id=${RPT_REJ}&action=accept"
fi

# ── Accept — RPT_OP2 (op2) ───────────────────────────────────────────────────
if [[ -n "${RPT_OP2}" ]]; then
    check "respond: op2 accept RPT_OP2 → 200"        200 -b "${JAR_OPERATOR2}" -X POST "${BASE}/api/report/respond" \
        --data "report_id=${RPT_OP2}&action=accept"
fi

# Stato corrente:
#   RPT_FULL → IN_PROGRESS (2) a op1
#   RPT_REJ  → ACTIVE (0) non assegnato
#   RPT_OP2  → IN_PROGRESS (2) a op2

# ── Verifica vista operatore dopo accept ──────────────────────────────────────
check "op1 active dopo accept (RPT_FULL visibile)" 200 -b "${JAR_OPERATOR}"  "${BASE}/api/reports/active"
check "op2 active dopo accept (RPT_OP2 visibile)"  200 -b "${JAR_OPERATOR2}" "${BASE}/api/reports/active"

# ── 3.10 API — /api/report/status (solo status=3 RESOLVED) ───────────────────
echo -e "\n  ${BOLD}§ 3.10 API — /api/report/status (resolve) [AGGIORNATO]${RESET}"
echo -e "     ${YELLOW}Nota: status=1 e status=2 non sono più validi per questo endpoint.${RESET}"
echo -e "     ${YELLOW}      Accept/reject usano ora /api/report/respond.${RESET}"

# ── Valori di status non più validi (restituiscono 400) ──────────────────────
if [[ -n "${RPT_FULL}" ]]; then
    check "status=1 → 400 (ora non valido)"         400 -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/status" \
        --data "report_id=${RPT_FULL}&status=1"
    check "status=2 → 400 (ora non valido)"         400 -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/status" \
        --data "report_id=${RPT_FULL}&status=2"
    check "status=99 → 400 (non valido)"            400 -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/status" \
        --data "report_id=${RPT_FULL}&status=99"
    check "status=0 → 400 (non valido)"             400 -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/status" \
        --data "report_id=${RPT_FULL}&status=0"
    # Parametri mancanti / report inesistente
    check "parametri mancanti → 400"                400 -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/status" \
        --data "report_id=${RPT_FULL}"
    check "report inesistente → 404"                404 -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/status" \
        --data "report_id=999999&status=3"
fi

# ── Solo operatori possono usare questo endpoint ──────────────────────────────
if [[ -n "${RPT_FULL}" ]]; then
    check "cittadino resolve → 403"                 403 -b "${JAR_CITIZEN}"  -X POST "${BASE}/api/report/status" \
        --data "report_id=${RPT_FULL}&status=3"
    check "admin resolve → 403"                     403 -b "${JAR_ADMIN}"    -X POST "${BASE}/api/report/status" \
        --data "report_id=${RPT_FULL}&status=3"
    check "non auth resolve → 403"                  403 -X POST "${BASE}/api/report/status" \
        --data "report_id=${RPT_FULL}&status=3"
fi

# ── Guard di stato: resolve su report non IN_PROGRESS fallisce ───────────────
if [[ -n "${RPT_REJ}" ]]; then
    # RPT_REJ è ACTIVE (0) → resolve fallisce (non in progress, non assegnato a op1)
    check "resolve RPT_REJ ACTIVE → 403"            403 -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/status" \
        --data "report_id=${RPT_REJ}&status=3"
fi

# ── Guard cross-operatore: op1 prova a risolvere il report di op2 ─────────────
if [[ -n "${RPT_OP2}" ]]; then
    check "op1 resolve RPT_OP2 (di op2) → 403"     403 -b "${JAR_OPERATOR}"  -X POST "${BASE}/api/report/status" \
        --data "report_id=${RPT_OP2}&status=3"
    # op2 risolve RPT_OP2 → 200
    check "op2 resolve RPT_OP2 → 200"              200 -b "${JAR_OPERATOR2}" -X POST "${BASE}/api/report/status" \
        --data "report_id=${RPT_OP2}&status=3"
    # Secondo resolve su report già RESOLVED → 403 (guard fallisce)
    check "op2 resolve RPT_OP2 doppio → 403"        403 -b "${JAR_OPERATOR2}" -X POST "${BASE}/api/report/status" \
        --data "report_id=${RPT_OP2}&status=3"
fi

# ── op1 risolve RPT_FULL ──────────────────────────────────────────────────────
if [[ -n "${RPT_FULL}" ]]; then
    check "op1 resolve RPT_FULL → 200"              200 -b "${JAR_OPERATOR}"  -X POST "${BASE}/api/report/status" \
        --data "report_id=${RPT_FULL}&status=3"
    # Resolve su report già RESOLVED → 403 (guard fallisce)
    check "op1 resolve RPT_FULL doppio → 403"       403 -b "${JAR_OPERATOR}"  -X POST "${BASE}/api/report/status" \
        --data "report_id=${RPT_FULL}&status=3"
fi

# ── Admin tenta di assegnare un report IN_PROGRESS → 409 ─────────────────────
# (Nota: ASSIGNED è force-reassignable; solo IN_PROGRESS/RESOLVED → 409)
if [[ -n "${RPT_OP2}" && -n "${OP1_ID}" ]]; then
    check "admin/assign RPT_OP2 RESOLVED → 409"    409 -b "${JAR_ADMIN}" -X POST "${BASE}/api/admin/assign" \
        --data "report_id=${RPT_OP2}&operator_id=${OP1_ID}"
fi

# ── Percorso reject/re-assign/accept/resolve per RPT_REJ ─────────────────────
if [[ -n "${RPT_REJ}" && -n "${OP1_ID}" ]]; then
    check "admin re-assign RPT_REJ (dopo reject) → 200" 200 -b "${JAR_ADMIN}" -X POST "${BASE}/api/admin/assign" \
        --data "report_id=${RPT_REJ}&operator_id=${OP1_ID}"
    check "op1 accept RPT_REJ (re-assigned) → 200"      200 -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/respond" \
        --data "report_id=${RPT_REJ}&action=accept"
    check "op1 resolve RPT_REJ → 200"                   200 -b "${JAR_OPERATOR}"  -X POST "${BASE}/api/report/status" \
        --data "report_id=${RPT_REJ}&status=3"
fi

# Stato corrente:
#   RPT_FULL → RESOLVED (3) a op1
#   RPT_REJ  → RESOLVED (3) a op1
#   RPT_OP2  → RESOLVED (3) a op2

# ── 3.11 Viste operatore/cittadino dopo risoluzione ───────────────────────────
echo -e "\n  ${BOLD}§ 3.11 Viste operative post-risoluzione${RESET}"

check "op1 active dopo resolve (dovrebbe essere vuoto)"      200 -b "${JAR_OPERATOR}"  "${BASE}/api/reports/active"
check "op1 archived (RPT_FULL + RPT_REJ visibili)"           200 -b "${JAR_OPERATOR}"  "${BASE}/api/reports/archived"
check "op2 active dopo resolve (dovrebbe essere vuoto)"      200 -b "${JAR_OPERATOR2}" "${BASE}/api/reports/active"
check "op2 archived (RPT_OP2 visibile)"                      200 -b "${JAR_OPERATOR2}" "${BASE}/api/reports/archived"
check "cittadino active (report non ancora risolti)"         200 -b "${JAR_CITIZEN}"   "${BASE}/api/reports/active"
check "cittadino archived (tutti i report risolti)"          200 -b "${JAR_CITIZEN}"   "${BASE}/api/reports/archived"
check "admin /api/reports/all post-resolve"                  200 -b "${JAR_ADMIN}"     "${BASE}/api/reports/all"

# ── 3.12 API — /api/report/feedback (valutazione cittadino) ──────────────────
echo -e "\n  ${BOLD}§ 3.12 API — /api/report/feedback (1-5★) [NUOVO]${RESET}"

if [[ -n "${RPT_FULL}" ]]; then
    # ── Solo i cittadini possono dare feedback ────────────────────────────────
    check "feedback: operatore → 403"               403 -b "${JAR_OPERATOR}" -X POST "${BASE}/api/report/feedback" \
        --data "report_id=${RPT_FULL}&stars=5"
    check "feedback: admin → 403"                   403 -b "${JAR_ADMIN}"    -X POST "${BASE}/api/report/feedback" \
        --data "report_id=${RPT_FULL}&stars=5"
    check "feedback: non auth → 403"                403 -X POST "${BASE}/api/report/feedback" \
        --data "report_id=${RPT_FULL}&stars=5"

    # ── Validazione stelle ────────────────────────────────────────────────────
    check "feedback: stars=0 → 400"                 400 -b "${JAR_CITIZEN}"  -X POST "${BASE}/api/report/feedback" \
        --data "report_id=${RPT_FULL}&stars=0"
    check "feedback: stars=6 → 400"                 400 -b "${JAR_CITIZEN}"  -X POST "${BASE}/api/report/feedback" \
        --data "report_id=${RPT_FULL}&stars=6"
    check "feedback: stars=-1 → 400"               400 -b "${JAR_CITIZEN}"  -X POST "${BASE}/api/report/feedback" \
        --data "report_id=${RPT_FULL}&stars=-1"

    # ── Parametri mancanti ────────────────────────────────────────────────────
    check "feedback: solo report_id → 400"          400 -b "${JAR_CITIZEN}"  -X POST "${BASE}/api/report/feedback" \
        --data "report_id=${RPT_FULL}"
    check "feedback: solo stars → 400"              400 -b "${JAR_CITIZEN}"  -X POST "${BASE}/api/report/feedback" \
        --data "stars=5"
    check "feedback: body vuoto → 400"              400 -b "${JAR_CITIZEN}"  -X POST "${BASE}/api/report/feedback" \
        --data ""

    # ── Report inesistente ─────────────────────────────────────────────────────
    check "feedback: report inesistente → 409"      409 -b "${JAR_CITIZEN}"  -X POST "${BASE}/api/report/feedback" \
        --data "report_id=999999&stars=4"

    # ── Feedback valido: cittadino dà 5 stelle a RPT_FULL ────────────────────
    check "feedback: 5★ su RPT_FULL → 200"          200 -b "${JAR_CITIZEN}"  -X POST "${BASE}/api/report/feedback" \
        --data "report_id=${RPT_FULL}&stars=5"
    # Secondo feedback: guard (feedback IS NULL) fallisce → 409
    check "feedback: secondo feedback → 409"         409 -b "${JAR_CITIZEN}"  -X POST "${BASE}/api/report/feedback" \
        --data "report_id=${RPT_FULL}&stars=3"
fi

if [[ -n "${RPT_REJ}" ]]; then
    # Feedback 1 stella su RPT_REJ → 200
    check "feedback: 1★ su RPT_REJ → 200"          200 -b "${JAR_CITIZEN}"  -X POST "${BASE}/api/report/feedback" \
        --data "report_id=${RPT_REJ}&stars=1"
fi

if [[ -n "${RPT_OP2}" ]]; then
    # Feedback 3 stelle su RPT_OP2 → 200
    check "feedback: 3★ su RPT_OP2 → 200"          200 -b "${JAR_CITIZEN}"  -X POST "${BASE}/api/report/feedback" \
        --data "report_id=${RPT_OP2}&stars=3"
fi

# ── 3.13 Method not allowed ────────────────────────────────────────────────────
echo -e "\n  ${BOLD}§ 3.13 Method not allowed${RESET}"

check "DELETE /home → 405"                  405 -X DELETE "${BASE}/home"
check "PUT /register → 405"                 405 -X PUT    "${BASE}/register"
check "PATCH /login → 405"                  405 -X PATCH  "${BASE}/login"
check "GET /api/report/status → 405"        405 -X GET    "${BASE}/api/report/status"
check "GET /api/report/respond → 405"       405 -X GET    "${BASE}/api/report/respond"
check "GET /api/report/feedback → 405"      405 -X GET    "${BASE}/api/report/feedback"
check "GET /api/admin/assign → 405"         405 -X GET    "${BASE}/api/admin/assign"
check "DELETE /api/reports/active → 405"    405 -X DELETE "${BASE}/api/reports/active"

# ── 3.14 Logout ───────────────────────────────────────────────────────────────
echo -e "\n  ${BOLD}§ 3.14 Logout${RESET}"

check "GET /logout cittadino"             302 -b "${JAR_CITIZEN}"   "${BASE}/logout"
check "GET /logout operatore"             302 -b "${JAR_OPERATOR}"  "${BASE}/logout"
check "GET /logout operatore2"            302 -b "${JAR_OPERATOR2}" "${BASE}/logout"
check "GET /logout admin"                 302 -b "${JAR_ADMIN}"     "${BASE}/logout"
check "GET /home dopo logout → redirect"  302 -b "${JAR_CITIZEN}"   "${BASE}/home"
check "GET /api/reports/active dopo logout → 401" 401 -b "${JAR_CITIZEN}" "${BASE}/api/reports/active"

# ── Arresto server per flush dei .gcda ────────────────────────────────────────
echo -e "\n  ${BOLD}§ Arresto server (flush .gcda)${RESET}"
kill -SIGINT "${SERVER_PID}" 2>/dev/null || true
wait "${SERVER_PID}" 2>/dev/null || true
SERVER_PID=0
ok "Server arrestato — dati coverage salvati su disco."
trap - EXIT
rm -f "${JAR_CITIZEN}" "${JAR_OPERATOR}" "${JAR_OPERATOR2}" "${JAR_ADMIN}"

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