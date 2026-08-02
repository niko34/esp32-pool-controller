#include "web_routes_data.h"
#include "web_helpers.h"
#include "auth.h"
#include "config.h"
#include "constants.h"
#include "sensors.h"
#include "filtration.h"
#include "measure_validity.h"
#include "pump_controller.h"
#include "web_routes_control.h"
#include "logger.h"
#include "logs_page.h"
#include "logs_export_chunk.h"
#include "history.h"
#include "history_export_chunk.h"
#include "json_compat.h"
#include <AsyncJson.h>
#include <time.h>
#include <memory>
#include <stdarg.h>  // va_list (historyCarryLoadf)

// Signal réseau secondaire (v2.9.2, défini dans main.cpp) : /data est polled toutes
// les ~30 s par l'appli web → une requête servie prouve la pile HTTP vivante et
// rafraîchit le watchdog de connectivité (jamais de reboot pendant une session active).
void notifyNetworkActivity();

namespace {
bool isTimeValid(time_t t) {
  return t >= kMinValidEpoch;
}
}  // namespace

static void handleGetData(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);
  notifyNetworkActivity();
  // Buffer statique pour éviter la fragmentation du heap
  // Taille estimée : ~16 champs × 30 bytes + overhead (feature-020 ajoute 3 champs)
  // feature-025 : +14 champs filtre pH/ORP → bump du buffer.
  // feature-061 : +3 champs validité mesures → bump du buffer.
  StaticJson<1280> doc;

  // feature-025 : la "valeur courante" (champs ph/orp) reflète la valeur FILTRÉE
  // (médiane + EMA), alignée sur le WS. Le brut reste exposé séparément (phRaw/orpRaw).
  // Fallback sur le brut si le filtre n'est pas encore amorcé (warmup).
  float orpRaw = sensors.getOrpRaw();
  float phRaw  = sensors.getPhRaw();
  float orpMed = sensors.getOrpMedian();
  float phMed  = sensors.getPhMedian();
  float orpFil = sensors.getOrpFiltered();
  float phFil  = sensors.getPhFiltered();
  float orpVal = !isnan(orpFil) ? orpFil : orpRaw;
  float phVal  = !isnan(phFil)  ? phFil  : phRaw;

  // ORP (valeur courante = filtrée)
  if (!isnan(orpVal)) {
    doc["orp"] = round(orpVal);
  } else {
    doc["orp"] = nullptr;
  }

  // pH (feature-021 : 3 décimales fiables EZO — alignement WS / MQTT / REST).
  if (!isnan(phVal)) {
    doc["ph"] = round(phVal * 1000.0f) / 1000.0f;
  } else {
    doc["ph"] = nullptr;
  }

  // feature-025 : chaîne de filtrage exposée pour la chip d'état + ligne brute UI.
  if (!isnan(phRaw)) doc["phRaw"] = round(phRaw * 1000.0f) / 1000.0f; else doc["phRaw"] = nullptr;
  if (!isnan(phMed)) doc["phMedian"] = round(phMed * 1000.0f) / 1000.0f; else doc["phMedian"] = nullptr;
  if (!isnan(phFil)) doc["phFiltered"] = round(phFil * 1000.0f) / 1000.0f; else doc["phFiltered"] = nullptr;
  doc["phFilterReady"]    = sensors.isPhFilterReady();
  doc["phFilterUnstable"] = sensors.isPhFilterUnstable();
  doc["phRejectedCount"]  = sensors.getPhRejectedCount();
  if (!isnan(orpRaw)) doc["orpRaw"] = round(orpRaw); else doc["orpRaw"] = nullptr;
  if (!isnan(orpMed)) doc["orpMedian"] = round(orpMed); else doc["orpMedian"] = nullptr;
  if (!isnan(orpFil)) doc["orpFiltered"] = round(orpFil); else doc["orpFiltered"] = nullptr;
  doc["orpFilterReady"]    = sensors.isOrpFilterReady();
  doc["orpFilterUnstable"] = sensors.isOrpFilterUnstable();
  doc["orpRejectedCount"]  = sensors.getOrpRejectedCount();

  // feature-034 : points de calibration EZO (déjà publiés en WS, mais ABSENTS de /data —
  // nécessaires au polling de calibration qui interroge /data activement, Safari mettant
  // le WebSocket en pause sur un onglet non focalisé).
  doc["phCalPoints"]  = sensors.getPhCalibrationPointsCached();
  doc["orpCalPoints"] = sensors.getOrpCalibrationPointsCached();

  // Température (offset utilisateur appliqué dans getTemperature())
  if (!isnan(sensors.getTemperature())) {
    doc["temperature"] = sensors.getTemperature();
  } else {
    doc["temperature"] = nullptr;
  }

  // feature-020 : T° circuit (2ᵉ sonde DS18B20) + indicateurs identification
  float tCircuit = sensors.getCircuitTemperature();
  if (!isnan(tCircuit)) {
    doc["temperature_circuit"] = round(tCircuit * 10.0f) / 10.0f;
  } else {
    doc["temperature_circuit"] = nullptr;
  }
  doc["sondes_identified"] = sensors.areSondesIdentified();
  doc["sondes_detected"]   = sensors.getDetectedSondeCount();

  doc["filtration_running"] = filtration.isRunning();

  // feature-061 : validité des mesures pH/ORP — lue depuis le CACHE de
  // measure_validity (snapshot cohérent sous spinlock). NE JAMAIS appeler
  // filtration.resolveWaterPresence() ici : callback en contexte async_tcp.
  // NB : les champs live ph/orp/phRaw/... restent publiés tels quels (page
  // calibration + onglet Avancé) — l'UI arbitre l'affichage via measures_valid.
  {
    bool wPresent = false, mValid = false;
    uint32_t mRemainS = 0;
    measureValidity.getSnapshot(wPresent, mValid, mRemainS);
    doc["water_present"] = wPresent;
    doc["measures_valid"] = mValid;
    // Nom distinct de stabilization_remaining_s (WS = stabilisation post-
    // calibration des pompes) : ici purge de la chambre de mesure.
    doc["measures_stab_remaining_s"] = mRemainS;
  }
  doc["ph_dosing"] = PumpController.isPhDosing();
  doc["orp_dosing"] = PumpController.isOrpDosing();
  doc["ph_daily_ml"] = safetyLimits.dailyPhInjectedMl;
  doc["orp_daily_ml"] = safetyLimits.dailyOrpInjectedMl;
  doc["ph_limit_reached"] = safetyLimits.phLimitReached;
  doc["orp_limit_reached"] = safetyLimits.orpLimitReached;
  doc["ph_inject_remaining_s"]  = manualInjectRemainingS(manualInjectPh);
  doc["orp_inject_remaining_s"] = manualInjectRemainingS(manualInjectOrp);

  doc["ph_tracking_enabled"]  = productCfg.phTrackingEnabled;
  doc["ph_remaining_ml"]      = max(0.0f, productCfg.phContainerVolumeMl  - productCfg.phTotalInjectedMl);
  doc["ph_alert_threshold_ml"]= productCfg.phAlertThresholdMl;
  doc["orp_tracking_enabled"] = productCfg.orpTrackingEnabled;
  doc["orp_remaining_ml"]     = max(0.0f, productCfg.orpContainerVolumeMl - productCfg.orpTotalInjectedMl);
  doc["orp_alert_threshold_ml"]= productCfg.orpAlertThresholdMl;

  time_t nowEpoch = time(nullptr);
  doc["time_synced"] = isTimeValid(nowEpoch);

  sendJsonResponse(request, doc);
}

// État partagé de l'export chunked /download-logs (capturé en shared_ptr par le
// callback — détruit à la fin de la réponse OU sur déconnexion client, ce qui
// ferme le File via son destructeur).
struct LogsDownloadState {
  enum Phase : uint8_t { kHeader, kFileOpen, kFileData, kFileEnd, kRamHeader, kRamEntries, kDone };
  Phase phase = kHeader;
  File file;                 // /system.log (partition history via systemLogger.getPersistenceFs())
  size_t ramIdx = 0;         // Index chronologique dans le buffer RAM du logger
  bool hasAbsoluteTime = false;
  long bootEpoch = 0;        // Epoch de démarrage (reconstruction des timestamps absolus)
  time_t nowEpoch = 0;
  unsigned long nowMs = 0;
  LogLineCarry<kLogExportLineMaxBytes> carry;  // Reliquat de ligne entre deux chunks
};

// Charge une chaîne C (marqueurs de section) dans le carry.
static void logsCarryLoad(LogLineCarry<kLogExportLineMaxBytes>& c, const char* s) {
  size_t n = strlen(s);
  if (n > sizeof(c.line)) n = sizeof(c.line);
  memcpy(c.line, s, n);
  c.len = n;
  c.pos = 0;
}

// Formate le timestamp d'une entrée RAM (absolu si l'heure est synchronisée,
// sinon +HH:MM:SS depuis le boot).
static void logsFormatTime(const LogsDownloadState& st, unsigned long entryMs,
                           char* timeBuf, size_t timeBufLen) {
  if (st.hasAbsoluteTime) {
    time_t entryEpoch = st.bootEpoch + (long)(entryMs / 1000);
    struct tm t;
    localtime_r(&entryEpoch, &t);
    strftime(timeBuf, timeBufLen, "%Y-%m-%d %H:%M:%S", &t);
  } else {
    unsigned long totalSec = entryMs / 1000;
    snprintf(timeBuf, timeBufLen, "+%02lu:%02lu:%02lu",
             totalSec / 3600, (totalSec % 3600) / 60, totalSec % 60);
  }
}

static void handleDownloadLogs(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  // bug-crash-getlogs-badalloc : réponse CHUNKED — le callback remplit le buffer
  // TCP borné fourni par AsyncWebServer, jamais de cbuf contigu proportionnel à
  // la taille de l'export (l'ancien AsyncResponseStream accumulait tout : resize()
  // = new[] throwant → std::bad_alloc → abort). Aucune garde heap nécessaire.
  auto st = std::make_shared<LogsDownloadState>();
  st->nowEpoch = time(nullptr);
  st->nowMs = millis();
  st->hasAbsoluteTime = (st->nowEpoch > 1609459200L);
  st->bootEpoch = st->hasAbsoluteTime ? (long)(st->nowEpoch - st->nowMs / 1000) : 0;

  AsyncWebServerResponse* res = request->beginChunkedResponse(
    "text/plain; charset=utf-8",
    [st](uint8_t* buf, size_t maxLen, size_t /*index*/) -> size_t {
      // Machine à états : chaque appel émet au plus maxLen octets (< 50 ms :
      // écritures mémoire + lectures LittleFS par blocs). Retourne 0 UNIQUEMENT
      // quand tout est émis (0 = fin de réponse pour AsyncWebServer).
      size_t total = 0;
      while (total < maxLen && st->phase != LogsDownloadState::kDone) {
        // 1) Vider d'abord le reliquat de ligne en cours (garantie de progression)
        if (st->carry.pos < st->carry.len) {
          total += emitCarry(st->carry, buf + total, maxLen - total);
          continue;  // re-tester la place restante
        }
        // 2) Carry épuisé : produire le contenu suivant selon la phase
        switch (st->phase) {
          case LogsDownloadState::kHeader: {
            char exportTime[32];
            if (st->hasAbsoluteTime) {
              struct tm t;
              localtime_r(&st->nowEpoch, &t);
              strftime(exportTime, sizeof(exportTime), "%Y-%m-%d %H:%M:%S", &t);
            } else {
              snprintf(exportTime, sizeof(exportTime), "boot+%lus (heure non sync.)",
                       st->nowMs / 1000);
            }
            int n = snprintf(st->carry.line, sizeof(st->carry.line),
                             "# Pool Controller — Journal système\n"
                             "# Exporté le : %s\n"
                             "# Format: [YYYY-MM-DD HH:MM:SS] NIVEAU : message\n",
                             exportTime);
            st->carry.len = (n > 0) ? ((size_t)n < sizeof(st->carry.line)
                                           ? (size_t)n : sizeof(st->carry.line) - 1)
                                    : 0;
            st->carry.pos = 0;
            st->phase = LogsDownloadState::kFileOpen;
            break;
          }
          case LogsDownloadState::kFileOpen: {
            // Ouverture paresseuse du journal persistant (boots précédents)
            fs::FS* pfs = systemLogger.getPersistenceFs();
            if (pfs) st->file = pfs->open("/system.log", "r");
            if (st->file && st->file.size() > 0) {
              logsCarryLoad(st->carry, "\n# === Historique persistant ===\n");
              st->phase = LogsDownloadState::kFileData;
            } else {
              if (st->file) st->file.close();
              st->phase = LogsDownloadState::kRamHeader;
            }
            break;
          }
          case LogsDownloadState::kFileData: {
            // Copie par blocs directement dans le buffer TCP (hors mutex logger)
            size_t n = st->file.available()
                           ? st->file.read(buf + total, maxLen - total) : 0;
            if (n > 0) {
              total += n;
            } else {
              st->file.close();
              st->phase = LogsDownloadState::kFileEnd;
            }
            break;
          }
          case LogsDownloadState::kFileEnd:
            logsCarryLoad(st->carry, "# === Fin historique persistant ===\n");
            st->phase = LogsDownloadState::kRamHeader;
            break;
          case LogsDownloadState::kRamHeader:
            logsCarryLoad(st->carry, "\n# === Session courante ===\n");
            st->phase = LogsDownloadState::kRamEntries;
            break;
          case LogsDownloadState::kRamEntries: {
            // Une entrée RAM par itération, copiée sous mutex (une seule String).
            // false = fin du buffer OU timeout mutex (export best-effort) ; le
            // ring peut tourner entre deux chunks (doublon/saut tolérés).
            LogEntry entry;
            if (systemLogger.getLogEntryByIndex(st->ramIdx, entry)) {
              st->ramIdx++;
              char timeBuf[24];
              logsFormatTime(*st, entry.timestamp, timeBuf, sizeof(timeBuf));
              int n = snprintf(st->carry.line, sizeof(st->carry.line),
                               "[%s] %s : %s\n", timeBuf,
                               systemLogger.getLevelString(entry.level).c_str(),
                               entry.message.c_str());
              if (n < 0) {
                st->carry.len = 0;
              } else if ((size_t)n >= sizeof(st->carry.line)) {
                // Ligne trop longue : tronquée avec « … » (3 octets UTF-8) + \n.
                // Une séquence UTF-8 du message peut être coupée — cosmétique.
                size_t len = sizeof(st->carry.line) - 1;
                memcpy(&st->carry.line[len - 4], "\xE2\x80\xA6\n", 4);
                st->carry.len = len;
              } else {
                st->carry.len = (size_t)n;
              }
              st->carry.pos = 0;
            } else {
              st->phase = LogsDownloadState::kDone;
            }
            break;
          }
          default:
            st->phase = LogsDownloadState::kDone;
            break;
        }
      }
      return total;
    });
  res->addHeader("Content-Disposition", "attachment; filename=\"pool_logs.txt\"");
  res->addHeader("Cache-Control", "no-cache");
  request->send(res);
}

static void handleGetLogs(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  // Support paramètre optionnel ?since=TIMESTAMP pour récupération incrémentale
  unsigned long sinceTimestamp = 0;
  if (request->hasParam("since")) {
    String sinceParam = request->getParam("since")->value();
    sinceTimestamp = sinceParam.toInt();
  }

  // bug-crash-getlogs-badalloc : page BORNÉE construite en JsonDocument (une seule
  // passe sous le mutex logger), puis réponse pré-dimensionnée. Contrat réel du
  // cbuf d'AsyncResponseStream : resize() = new char[] THROWANT (2 coredumps le
  // prouvent — le commentaire v2.8.2 « realloc, jamais d'abort » était faux),
  // d'où pagination (~4 Ko max) + measureJson + garde heap AVANT toute allocation.
  // Le client boucle avec ?since=<dernier timestamp> tant que truncated == true.
  JsonDocument doc;
  doc["uptime_ms"] = millis();
  JsonArray arr = doc["logs"].to<JsonArray>();

  bool truncated = false;
  size_t included = 0;
  unsigned long lastTs = 0;
  systemLogger.forEachRecentLog(kMaxLogEntries, sinceTimestamp,
    [&arr, &truncated, &included, &lastTs](const LogEntry& entry) -> bool {
      LogsPageDecision d = logsPageAdmit(included, entry.timestamp, lastTs,
                                         kLogsMaxEntriesPerPage, kLogsPageHardCap);
      if (d.include) {
        JsonObject o = arr.add<JsonObject>();
        o["timestamp"] = entry.timestamp;
        o["level"] = systemLogger.getLevelString(entry.level);
        // serializeJson échappe le message (l'échappement manuel a été supprimé)
        o["message"] = entry.message;
        included++;
        lastTs = entry.timestamp;
      }
      if (d.stop) {
        truncated = true;  // il reste des entrées au-delà de cette page
        return false;      // arrêt anticipé de l'itération
      }
      return true;
    });
  doc["truncated"] = truncated;

  // ORDRE IMPÉRATIF : measureJson → garde heap → beginResponseStream pré-dimensionné
  // → serializeJson → send. Ne RIEN écrire d'autre dans le stream.
  size_t sz = measureJson(doc);
  if (sz + kHttpHeapMarginBytes > ESP.getMaxAllocHeap()) {
    request->send(503, "application/json", "{\"error\":\"heap fragmenté, réessayez\"}");
    return;
  }
  AsyncResponseStream* res = request->beginResponseStream("application/json", sz);
  serializeJson(doc, *res);
  request->send(res);
}

// État partagé de l'export chunked /get-history (bug-heap-toctou A3, patron
// LogsDownloadState) : capturé en shared_ptr par le callback, détruit à la fin de
// la réponse OU sur déconnexion client. Contrat JSON INCHANGÉ octet pour octet :
// {"range":"…","count":0,"history":[…],"count":N} — la clé "count" est émise DEUX
// fois (0 dans l'en-tête, valeur réelle dans le pied), comme l'ancienne route.
struct HistoryDownloadState {
  enum Phase : uint8_t { kHeader, kPoints, kFooter, kDone };
  Phase phase = kHeader;
  size_t idx = 0;             // Index chronologique dans memoryBuffer (vecteur trié unique)
  unsigned long cutoff = 0;   // Borne basse ?range= (0 = tout) — historyRangeCutoff
  unsigned long since = 0;    // Borne basse ?since= (0 = désactivé)
  size_t count = 0;           // Points réellement émis (pied de réponse)
  bool first = true;          // Virgule de séparation entre points
  uint16_t retries = 0;       // Timeouts mutex consécutifs (borné kHistoryChunkMaxRetries)
  String range;               // Écho du paramètre ?range= (en-tête)
  LogLineCarry<kHistoryPointJsonMaxBytes> carry;  // Reliquat en-tête/point/pied entre deux chunks
};

// Charge une chaîne formatée bornée dans le carry (en-tête / pied de réponse).
static void historyCarryLoadf(LogLineCarry<kHistoryPointJsonMaxBytes>& c,
                              const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(c.line, sizeof(c.line), fmt, args);
  va_end(args);
  c.len = (n > 0) ? ((size_t)n < sizeof(c.line) ? (size_t)n : sizeof(c.line) - 1) : 0;
  c.pos = 0;
}

static void handleGetHistory(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  // Support paramètre optionnel ?range=24h|3d|7d|30d|all
  String rangeParam = "all";
  if (request->hasParam("range")) {
    rangeParam = request->getParam("range")->value();
  }
  // Revue 2.14.0 : seul le TOKEN RECONNU est échoé dans l'en-tête JSON (jamais
  // la valeur brute — une valeur longue ou avec guillemet malformerait le JSON
  // dans le carry borné). Valeur inconnue → traitée et échoée comme "all".
  const char* range = "all";
  int hours = 0;  // 0 = "all" (aucun filtre temporel)
  if (rangeParam == "24h")      { range = "24h"; hours = 24; }
  else if (rangeParam == "3d")  { range = "3d";  hours = 24 * 3; }
  else if (rangeParam == "7d")  { range = "7d";  hours = 24 * 7; }
  else if (rangeParam == "30d") { range = "30d"; hours = 24 * 30; }

  // bug-heap-toctou A3 : réponse CHUNKED zéro-buffer (patron /download-logs,
  // ADR-0032 mécanisme b) — supprime la String contiguë ~40 Ko épinglée pendant
  // tout le transfert (générateur de fond de la fragmentation, G3). Le filtre
  // range/since est appliqué PAR POINT dans le callback ; chaque point est copié
  // sous mutex COURT (history.getPointByIndex, 25 ms) — le mutex n'est JAMAIS
  // tenu entre deux chunks. NOTE : migrateLegacyHistory n'est plus appelé sur ce
  // chemin (l'ancien getLastHours le faisait) — couvert par update()/load/import.
  // Epoch : time(nullptr) si valide, sinon 0 → cutoff 0 = tout renvoyer (l'estimation
  // lastKnownEpoch de getLastHours n'est pas accessible ici ; superset toléré).
  auto st = std::make_shared<HistoryDownloadState>();
  st->range = range;
  time_t nowT = time(nullptr);
  st->cutoff = historyRangeCutoff(hours, isTimeValid(nowT) ? (unsigned long)nowT : 0);
  if (request->hasParam("since")) {
    st->since = request->getParam("since")->value().toInt();
  }

  AsyncWebServerResponse* res = request->beginChunkedResponse(
    "application/json",
    [st](uint8_t* buf, size_t maxLen, size_t /*index*/) -> size_t {
      // Machine à états : chaque appel émet au plus maxLen octets. Retourne 0
      // UNIQUEMENT en phase kDone (0 = fin de réponse pour AsyncWebServer) ;
      // timeout mutex sans octet émis → RESPONSE_TRY_AGAIN (réessai différé).
      size_t total = 0;
      while (total < maxLen && st->phase != HistoryDownloadState::kDone) {
        // 1) Vider d'abord le reliquat en cours (garantie de progression)
        if (st->carry.pos < st->carry.len) {
          total += emitCarry(st->carry, buf + total, maxLen - total);
          continue;  // re-tester la place restante
        }
        // 2) Carry épuisé : produire le contenu suivant selon la phase
        switch (st->phase) {
          case HistoryDownloadState::kHeader:
            historyCarryLoadf(st->carry, "{\"range\":\"%s\",\"count\":0,\"history\":[",
                              st->range.c_str());
            st->phase = HistoryDownloadState::kPoints;
            break;
          case HistoryDownloadState::kPoints: {
            DataPoint p;
            bool timedOut = false;
            if (history.getPointByIndex(st->idx, p, timedOut)) {
              st->idx++;
              st->retries = 0;
              // Filtres appliqués par point (identiques à l'ancienne route :
              // getLastHours filtrait timestamp >= cutoff, puis since exclusif).
              if (p.timestamp < st->cutoff) break;
              if (st->since > 0 && p.timestamp <= st->since) break;
              size_t n = formatHistoryPointJson(
                  st->carry.line, sizeof(st->carry.line), p.timestamp, p.ph, p.orp,
                  p.temperature, p.filtrationActive, p.phDosing || p.orpDosing,
                  static_cast<uint8_t>(p.granularity), st->first);
              if (n > 0) {
                st->carry.len = n;
                st->carry.pos = 0;
                st->first = false;
                st->count++;
              }
              // n == 0 : point plus long que kHistoryPointJsonMaxBytes (impossible
              // sur le format réel, ~120 o max) → sauté, jamais de JSON partiel.
              break;
            }
            if (timedOut) {
              // Mutex history occupé (consolidation/save en cours). Borné : au-delà
              // de kHistoryChunkMaxRetries, clôture PROPRE (pied émis → JSON valide).
              if (++st->retries >= kHistoryChunkMaxRetries) {
                st->phase = HistoryDownloadState::kFooter;
                break;
              }
              if (total > 0) return total;   // émettre le progrès déjà accumulé
              return RESPONSE_TRY_AGAIN;     // rien à émettre : réessai différé
            }
            st->phase = HistoryDownloadState::kFooter;  // fin du buffer
            break;
          }
          case HistoryDownloadState::kFooter:
            historyCarryLoadf(st->carry, "],\"count\":%u}", (unsigned)st->count);
            st->phase = HistoryDownloadState::kDone;
            break;
          default:
            st->phase = HistoryDownloadState::kDone;
            break;
        }
      }
      return total;
    });
  // Aucun header additionnel : l'ancienne route (sendRawJsonResponse) n'en posait
  // pas — seul le Transfer-Encoding change (chunked, transparent pour fetch()).
  request->send(res);
}

void setupDataRoutes(AsyncWebServer* server) {
  server->on("/data", HTTP_GET, handleGetData);
  server->on("/get-logs", HTTP_GET, handleGetLogs);
  server->on("/download-logs", HTTP_GET, handleDownloadLogs);
  server->on("/logs", HTTP_DELETE, [](AsyncWebServerRequest* request) {
    REQUIRE_AUTH(request, RouteProtection::WRITE);
    systemLogger.clearAll();
    systemLogger.info("Logs effacés (RAM + fichier persistant)");
    JsonDocument doc;
    doc["success"] = true;
    sendJsonResponse(request, doc);
  });
  server->on("/get-history", HTTP_GET, handleGetHistory);

  auto* importHandler = new AsyncCallbackJsonWebHandler(
    "/history/import",
    [](AsyncWebServerRequest* request, JsonVariant& json) {
      REQUIRE_AUTH(request, RouteProtection::WRITE);

      JsonObject root = json.as<JsonObject>();
      if (!root["history"].is<JsonArray>()) {
        sendErrorResponse(request, 400, "Format invalide: champ history manquant");
        return;
      }

      JsonArray items = root["history"].as<JsonArray>();
      if (items.isNull() || items.size() == 0) {
        sendErrorResponse(request, 400, "Historique vide");
        return;
      }

      std::vector<DataPoint> imported;
      imported.reserve(items.size());

      for (JsonObject item : items) {
        if (!item["timestamp"].is<unsigned long>()) {
          continue;
        }
        unsigned long ts = item["timestamp"] | 0;
        if (ts == 0) {
          continue;
        }

        DataPoint dp;
        dp.timestamp = ts;
        dp.ph = item["ph"].isNull() ? NAN : item["ph"].as<float>();
        dp.orp = item["orp"].isNull() ? NAN : item["orp"].as<float>();
        dp.temperature = item["temperature"].isNull() ? NAN : item["temperature"].as<float>();
        dp.filtrationActive = item["filtration"] | false;
        dp.phDosing = item["dosing"] | false;
        dp.orpDosing = false;
        uint8_t granularity = item["granularity"] | 0;
        dp.granularity = granularity <= DAILY ? static_cast<Granularity>(granularity) : RAW;

        imported.push_back(dp);
      }

      if (imported.empty()) {
        sendErrorResponse(request, 400, "Aucune donnée valide à importer");
        return;
      }

      if (!history.importData(imported)) {
        sendErrorResponse(request, 500, "Impossible d'importer l'historique");
        return;
      }

      JsonDocument doc;
      doc["status"] = "success";
      doc["count"] = imported.size();
      sendJsonResponse(request, doc);
    });
  importHandler->setMethod(HTTP_POST);
  server->addHandler(importHandler);

  server->on("/history/clear", HTTP_POST, [](AsyncWebServerRequest* request) {
    REQUIRE_AUTH(request, RouteProtection::WRITE);
    // feature-027 : clearHistory() peut échouer si le mutex historique est occupé
    if (!history.clearHistory()) {
      sendErrorResponse(request, 503, "Historique occupé — réessayer");
      return;
    }

    JsonDocument doc;
    doc["status"] = "success";
    sendJsonResponse(request, doc);
  });
}
