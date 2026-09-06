#include "history.h"
#include "history_logic.h"
#include "history_guard_logic.h"
#include "fs_guard.h"          // bug-100 : point d'admission unique des fopen internes (ADR-0070)
#include "constants.h"
#include "logger.h"
#include "sensors.h"
#include "filtration.h"
#include "measure_validity.h"
#include "pump_controller.h"
#include <ArduinoJson.h>
#include <map>
#include <algorithm>
#include <time.h>
#include <Preferences.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_rom_crc.h>
#include <atomic>

HistoryManager history;

namespace {
// bug-100 : refus d'admission MÉMOIRE de l'historique (famine), distinct du
// compteur NVS de refus d'admission d'ESPACE (historySaveAdmit / « partition
// pleine »). Deux pannes différentes, deux compteurs. `relaxed` : saveToFile()
// s'exécute depuis loopTask ET depuis async_tcp (POST /history/import).
std::atomic<uint32_t> sHistoryFopenDeferrals{0};

// Fragment de libellé commun aux DEUX refus mémoire de saveToFile() : même
// forme, même seuil affiché, donc écart lisible dans les deux cas. Et si le
// refus a été servi par le CACHE (asymétrie ADR-0070 §e, aucune mesure prise),
// on le dit au lieu d'afficher un maxBloc8 périmé comme s'il datait de l'instant.
// Une seule String temporaire, construite uniquement sur le chemin de refus
// throttlé (au plus une fois par heure) — jamais dans la loop nominale.
String fopenRefusalDetail() {
  const String seuil = String((unsigned)kStaticFileMinAllocBytes);
  if (!fsGuardLastRefusalMeasured()) {
    return "maxBloc8 non mesure (refus en cache) < " + seuil + " o";
  }
  return "maxBloc8=" + String(fsGuardLastMaxAlloc8()) + " o < " + seuil + " o";
}

fs::LittleFSFS historyFs;
fs::FS* historyStore = &LittleFS;
const char* historyFilePath = "/history.json";
unsigned long lastKnownEpoch = 0;
bool warnedUnsynced = false;
bool warnedEstimated = false;

bool isTimeValid(time_t t) {
  return t >= kMinValidEpoch;
}

void loadClockPrefs() {
  Preferences prefs;
  if (prefs.begin("clock", true)) {
    lastKnownEpoch = prefs.getULong("epoch", 0);
    prefs.end();
  }
}

void saveClockPrefs(unsigned long epoch) {
  Preferences prefs;
  if (prefs.begin("clock", false)) {
    prefs.putULong("epoch", epoch);
    prefs.end();
  }
}

// feature-027 : warn throttlé (max 1/min par site — lastWarnMs = statique locale du site)
// sur timeout de prise du mutex historique.
void warnHistoryMutexTimeout(unsigned long& lastWarnMs, const char* site) {
  unsigned long nowMs = millis();
  if (lastWarnMs == 0 || nowMs - lastWarnMs >= kMutexTimeoutWarnThrottleMs) {
    systemLogger.warning(String("[History] ") + site + ": timeout mutex — opération sautée");
    lastWarnMs = nowMs;
  }
}

// feature-059 : mappe esp_reset_reason() (ESP-IDF, impur) vers l'enum pur du
// disjoncteur de boot-loop (history_guard_logic, testable en natif).
HistoryResetKind mapResetReason(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return HistoryResetKind::PowerOn;
    case ESP_RST_SW:        return HistoryResetKind::Software;
    case ESP_RST_EXT:       return HistoryResetKind::External;
    case ESP_RST_BROWNOUT:  return HistoryResetKind::Brownout;
    case ESP_RST_DEEPSLEEP: return HistoryResetKind::DeepSleep;
    case ESP_RST_PANIC:     return HistoryResetKind::Panic;
    case ESP_RST_INT_WDT:   return HistoryResetKind::IntWdt;
    case ESP_RST_TASK_WDT:  return HistoryResetKind::TaskWdt;
    case ESP_RST_WDT:       return HistoryResetKind::OtherWdt;
    default:                return HistoryResetKind::Other;
  }
}

// feature-059 : lit le compteur de crashs d'init (drapeau du disjoncteur) en NVS.
// NVS indisponible (ou namespace absent au 1er boot) → 0 = pas de drapeau,
// dégradation silencieuse vers le comportement historique.
uint8_t readHistoryCrashCount() {
  Preferences prefs;
  uint8_t value = 0;
  if (prefs.begin("hist_meta", true)) {
    value = prefs.getUChar(kHistoryCrashCountNvsKey, 0);
    prefs.end();
  }
  return value;
}

// feature-059 : écrit le compteur de crashs d'init en NVS (pose : valeur > 0,
// levée : 0). Échec NVS → silencieux, jamais de blocage du boot.
void writeHistoryCrashCount(uint8_t value) {
  Preferences prefs;
  if (prefs.begin("hist_meta", false)) {
    prefs.putUChar(kHistoryCrashCountNvsKey, value);
    prefs.end();
  }
}

// bug-nospc-partition-history (A1) : préservation de preuve AVANT l'effacement
// du disjoncteur. Dump hex des blocs 0-1 de la partition (8 Ko = superblocks +
// métadonnées littlefs) sur Serial + empreinte courte (CRC32 par bloc + 16
// premiers octets) en NVS hist_meta. Discrimine H1 (chaîne de métadonnées
// pleine) vs H2 (superblock CRC-valide insensé) vs H3 (paire déchirée).
// DÉROGATION systemLogger → Serial.printf direct : la persistance des logs
// n'est PAS encore active à ce stade du boot, et il est interdit d'écrire sur
// le FS en cours d'autopsie (un ERROR déclencherait un flush disque).
// AUCUNE écriture sur la partition history ici (lecture seule + NVS).
// Durée ≈ 2,4 s à 115200 bauds → esp_task_wdt_reset() toutes les 64 lignes
// (WDT actif depuis main.cpp).
void dumpHistoryPartitionEvidence(const esp_partition_t* part) {
  constexpr size_t kDumpBytes    = 8192;  // blocs 0-1 littlefs (2 × 4096 o)
  constexpr size_t kBlockBytes   = 4096;
  constexpr size_t kChunkBytes   = 256;   // tampon de lecture SUR PILE
  constexpr size_t kBytesPerLine = 32;
  constexpr size_t kHeadBytes    = 16;    // empreinte courte NVS (début bloc 0)

  uint8_t chunk[kChunkBytes];
  uint8_t head[kHeadBytes] = {0};
  uint32_t crc0 = 0;
  uint32_t crc1 = 0;
  size_t lineCount = 0;

  Serial.printf("[HISTDUMP] BEGIN size=%u\n", (unsigned)kDumpBytes);
  for (size_t offset = 0; offset < kDumpBytes; offset += kChunkBytes) {
    if (esp_partition_read(part, offset, chunk, kChunkBytes) != ESP_OK) {
      Serial.printf("[HISTDUMP] ERREUR lecture offset 0x%04X\n", (unsigned)offset);
      break;
    }
    if (offset == 0) memcpy(head, chunk, kHeadBytes);
    // CRC32 ROM accumulé par bloc de 4096 o pendant la lecture
    if (offset < kBlockBytes) crc0 = esp_rom_crc32_le(crc0, chunk, kChunkBytes);
    else                      crc1 = esp_rom_crc32_le(crc1, chunk, kChunkBytes);

    for (size_t l = 0; l < kChunkBytes; l += kBytesPerLine) {
      char hex[kBytesPerLine * 3 + 1];
      for (size_t i = 0; i < kBytesPerLine; i++) {
        snprintf(&hex[i * 3], 4, "%02X ", chunk[l + i]);
      }
      Serial.printf("[HISTDUMP] 0x%04X: %s\n", (unsigned)(offset + l), hex);
      if ((++lineCount % 64) == 0) esp_task_wdt_reset();
    }
  }
  Serial.printf("[HISTDUMP] END crc0=0x%08X crc1=0x%08X\n", crc0, crc1);

  // Empreinte courte en NVS : récupérable après reboot même sans capture Serial.
  // Échec NVS silencieux — la preuve principale est déjà partie sur Serial.
  Preferences prefs;
  if (prefs.begin("hist_meta", false)) {
    prefs.putUInt(kHistoryDumpCrc0NvsKey, crc0);
    prefs.putUInt(kHistoryDumpCrc1NvsKey, crc1);
    prefs.putBytes(kHistoryDumpHeadNvsKey, head, kHeadBytes);
    prefs.end();
  }
}

// bug-nospc (A2/A3) : trace NVS d'un save refusé ou échoué — compteur cumulé
// nospc_cnt + epoch RAM du dernier save réussi (last_ok). Écrit UNIQUEMENT sur
// échec, jamais à chaque save réussi (usure NVS). Échec NVS silencieux.
void recordHistorySaveFailureNvs(unsigned long lastOkEpoch) {
  Preferences prefs;
  if (prefs.begin("hist_meta", false)) {
    prefs.putUInt(kHistoryNospcCountNvsKey, prefs.getUInt(kHistoryNospcCountNvsKey, 0) + 1);
    prefs.putUInt(kHistoryLastOkEpochNvsKey, (uint32_t)lastOkEpoch);
    prefs.end();
  }
}

unsigned long getCurrentEpoch(bool* synced, bool* estimated) {
  time_t nowEpoch = time(nullptr);
  if (isTimeValid(nowEpoch)) {
    if (synced) *synced = true;
    if (estimated) *estimated = false;
    lastKnownEpoch = static_cast<unsigned long>(nowEpoch);
    saveClockPrefs(lastKnownEpoch);
    return lastKnownEpoch;
  }

  if (synced) *synced = false;
  if (lastKnownEpoch > 0) {
    if (estimated) *estimated = true;
    unsigned long sinceBoot = millis() / kMillisToSeconds;
    return lastKnownEpoch + sinceBoot;
  }

  if (estimated) *estimated = false;
  return 0;
}
}  // namespace

void HistoryManager::begin() {
  _mutex = xSemaphoreCreateMutex();

  // feature-059 : disjoncteur de boot-loop de l'init history.
  // Une corruption littlefs peut provoquer une exception CPU non récupérable
  // (IntegerDivideByZero dans lfs_alloc) à la 1ʳᵉ écriture → boot-loop avant la
  // montée du WiFi. Détection inter-boot : compteur crash_cnt (NVS hist_meta)
  // posé juste avant le montage, remis à 0 sur TOUTES les sorties de begin().
  // Si le boot précédent a crashé pendant cette fenêtre kHistoryInitCrashThreshold
  // fois consécutives → effacement de la partition avant remontage. Indépendant
  // du budget kAutoHealMaxReboots et du mode maintenance (agit avant, en amont).
  uint8_t guardCountToWrite = 1;  // Proceed / ErasePartition → réarmement à 1
  {
    HistoryGuardInputs guardIn;
    guardIn.crashCount = readHistoryCrashCount();
    guardIn.resetWasCrash = isCrashResetKind(mapResetReason(esp_reset_reason()));
    guardIn.eraseThreshold = kHistoryInitCrashThreshold;

    switch (evaluateHistoryGuard(guardIn)) {
      case HistoryGuardDecision::ErasePartition: {
        // Seuil atteint : effacer la partition AVANT montage (le montage qui
        // suit reformatera proprement). La persistance des logs n'est pas
        // encore active ici → le CRIT part en RAM/Serial, c'est suffisant.
        const esp_partition_t* histPart = esp_partition_find_first(
          ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "history");
        if (histPart) {
          // bug-nospc (A1) : préserver la preuve AVANT destruction (discrimination H1/H2/H3).
          dumpHistoryPartitionEvidence(histPart);
          if (esp_partition_erase_range(histPart, 0, histPart->size) == ESP_OK) {
            _partitionAutoErased = true;
            systemLogger.critical(String("Boot-loop détecté : ") + guardIn.crashCount +
                                  " crashs consécutifs pendant l'init history — partition effacée automatiquement, l'historique repart de zéro");
          } else {
            systemLogger.critical(String("Boot-loop détecté : ") + guardIn.crashCount +
                                  " crashs consécutifs pendant l'init history — échec de l'effacement de la partition");
          }
        }
        break;
      }
      case HistoryGuardDecision::CountCrash:
        // Crash pendant l'init précédente, sous le seuil : on compte et on retente.
        guardCountToWrite = guardIn.crashCount + 1;
        systemLogger.warning(String("Crash détecté pendant l'init history précédente (") +
                             guardIn.crashCount + "/" + kHistoryInitCrashThreshold +
                             ") — nouvelle tentative de montage");
        break;
      case HistoryGuardDecision::Proceed:
      default:
        // Pas de drapeau, ou reset propre (POWERON/SW/EXT/...) → réarmement silencieux.
        break;
    }
  }

  // Effacer la partition si sa taille a changé depuis le dernier boot (ex: partition réduite).
  // LittleFS monte sans erreur sur un filesystem corrompu puis plante (division par zéro)
  // à la première écriture — on prévient ça en effaçant avant montage.
  {
    const esp_partition_t* histPart = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "history");
    if (histPart) {
      Preferences histPrefs;
      uint32_t storedSize = 0;
      if (histPrefs.begin("hist_meta", true)) {
        storedSize = histPrefs.getUInt("part_sz", 0);
        histPrefs.end();
      }
      if (storedSize != histPart->size) {
        if (storedSize != 0) {
          Serial.printf("[HISTORY] Partition redimensionnée (%u→%u o) — effacement LittleFS\n",
                        storedSize, histPart->size);
        }
        esp_partition_erase_range(histPart, 0, histPart->size);
        if (histPrefs.begin("hist_meta", false)) {
          histPrefs.putUInt("part_sz", (uint32_t)histPart->size);
          histPrefs.end();
        }
      }
    }
  }

  // feature-059 : pose du drapeau « init history en cours » juste avant le
  // montage. La fenêtre de détection couvre montage + .fscheck +
  // setPersistenceFs + loadFromFile ; le drapeau est levé (crash_cnt=0) sur
  // toutes les sorties de begin(). Un panic ailleurs dans le firmware ne peut
  // donc jamais déclencher l'effacement.
  writeHistoryCrashCount(guardCountToWrite);

  if (historyFs.begin(true, "/history", 5, "history")) {
    historyStore = &historyFs;
    historyFilePath = "/history.json";
    systemLogger.info("Partition historique dédiée montée");

    // Validation post-montage (défense en profondeur).
    // LittleFS peut monter un filesystem corrompu sans erreur puis crasher
    // (IntegerDivideByZero) à la 1ʳᵉ écriture — observé sur PCB v1 après
    // longue inactivité ESP32. On force un test write/read avant tout autre
    // accès. Si le test échoue → erase + remount propre.
    // bug-100 / ADR-0070 — CLASSE B, 7e barreau (5 120). Sous le seuil, on ne
    // REPORTE pas : au boot, « plus tard » n'existe pas. On arme le TRIPWIRE
    // (historyEnabled = false pour ce boot) au lieu de laisser fopen aborter.
    // L'absence de contention TCP est prouvée par l'ordre de setup() :
    // history.begin() (main.cpp:270) precede mqttManager.begin() (300) et
    // webServer.begin() (316).
    if (!fsGuardAdmit(kInternalFopenMinAllocBytes)) {
      // Le tripwire compte SON PROPRE refus : c'est la dégradation la plus
      // sévère du lot, elle doit faire bouger `hdef`. La ligne de journal, elle,
      // n'est durable que de façon BORNÉE — `_persistBuffer` plafonne à 100
      // entrées en éviction FIFO (logger.cpp:69-70), donc sous famine prolongée
      // le WARN de boot sort par le haut. Le compteur, lui, ne sort pas : il
      // part dans le payload RETENU <base>/diagnostic et survit au reboot.
      sHistoryFopenDeferrals.fetch_add(1, std::memory_order_relaxed);
      // ⚠️ L'OBSERVABILITÉ NE DOIT PAS DÉPENDRE DU SOUS-SYSTÈME QU'ELLE GARDE.
      // Le montage a réussi : ce n'est pas la partition qui manque, c'est la
      // mémoire. On arme donc la persistance du JOURNAL avant de sortir, sans
      // quoi le tripwire ferait perdre la trace de l'épisode même qu'il vient
      // de détecter — on ne perdrait pas seulement l'historique, mais le
      // journal persistant du boot ENTIER. setPersistenceFs() est lui-même
      // gardé (classe A) : sous le seuil il ne fera qu'échouer proprement.
      systemLogger.setPersistenceFs(&historyFs);
      systemLogger.warning("Memoire insuffisante pour verifier la partition history — "
                           "historique persistant desactive pour ce demarrage "
                           "(journal persistant conserve)");
      historyEnabled = false;
      writeHistoryCrashCount(0);  // feature-059 : sortie propre → levée du drapeau
      return;
    }

    bool fsHealthy = false;
    {
      File f = openIfHeapAllows(&historyFs, "/.fscheck", "w", kFsGuardAlreadyAdmitted);
      if (f) {
        if (f.print("ok") == 2) {
          f.close();
          File r = openIfHeapAllows(&historyFs, "/.fscheck", "r", kFsGuardAlreadyAdmitted);
          if (r) {
            String s = r.readString();
            r.close();
            if (s == "ok") fsHealthy = true;
          }
          historyFs.remove("/.fscheck");
        } else {
          f.close();
        }
      }
    }

    if (!fsHealthy) {
      systemLogger.warning("Partition history corrompue détectée — reformatage automatique");
      historyFs.end();
      const esp_partition_t* histPart = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "history");
      if (histPart) {
        esp_partition_erase_range(histPart, 0, histPart->size);
      }
      if (!historyFs.begin(true, "/history", 5, "history")) {
        systemLogger.error("Échec remontage partition history après reformatage");
        historyEnabled = false;
        writeHistoryCrashCount(0);  // feature-059 : sortie propre → levée du drapeau
        return;
      }
      systemLogger.info("Partition history reformatée et remontée");
    }

    systemLogger.setPersistenceFs(&historyFs);
  }
  else {
    systemLogger.error("Partition historique absente ou échec montage — logs persistants et historique désactivés");
    Serial.println("[HISTORY] ERREUR CRITIQUE: partition 'history' introuvable ou corrompue");
    historyEnabled = false;
    writeHistoryCrashCount(0);  // feature-059 : sortie propre → levée du drapeau
    return;
  }

  loadClockPrefs();
  loadFromFile();

  // feature-059 : init complète sans crash → levée du drapeau (crash_cnt=0).
  writeHistoryCrashCount(0);
  systemLogger.info("Gestionnaire d'historique initialisé");
}

uint32_t historyFopenDeferralCount() {
  return sHistoryFopenDeferrals.load(std::memory_order_relaxed);
}

void HistoryManager::update() {
  if (!historyEnabled) return;
  unsigned long now = millis();

  // Enregistrer un point toutes les 5 minutes
  if (now - lastRecord >= RECORD_INTERVAL) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(kHistoryMutexTimeoutMs)) == pdTRUE) {
      recordDataPoint();
      xSemaphoreGive(_mutex);
      lastRecord = now;
    } else {
      // feature-027 : point sauté, lastRecord NON avancé → retry au tour suivant
      static unsigned long sWarnRecordMs = 0;
      warnHistoryMutexTimeout(sWarnRecordMs, "recordDataPoint");
    }
  }

  // Consolidation + sauvegarde toutes les 5 min (consolidateData appelle saveToFile en interne)
  if (now - lastSave >= SAVE_INTERVAL) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(kHistoryMutexTimeoutMs)) == pdTRUE) {
      consolidateData();
      xSemaphoreGive(_mutex);
      lastSave = now;
    } else {
      // feature-027 : consolidation sautée, lastSave NON avancé → retry au tour suivant
      static unsigned long sWarnSaveMs = 0;
      warnHistoryMutexTimeout(sWarnSaveMs, "consolidateData");
    }
  }
}

void HistoryManager::recordDataPoint() {
  if (!historyEnabled) return;
  bool synced = false;
  bool estimated = false;
  unsigned long nowEpoch = getCurrentEpoch(&synced, &estimated);
  if (nowEpoch == 0) {
    // Pas d'heure disponible : enregistrer avec uptime comme timestamp provisoire.
    // Sera corrigé a posteriori lors de la première sync NTP (_applyPreNtpCorrection).
    nowEpoch = millis() / kMillisToSeconds;
    _preNtpPending = true;
    if (!warnedUnsynced) {
      systemLogger.warning("Horloge non synchronisée — timestamp provisoire (uptime), correction à la sync NTP");
      warnedUnsynced = true;
    }
  } else {
    if (synced) {
      migrateLegacyHistory(nowEpoch);
      if (_preNtpPending) {
        _applyPreNtpCorrection(nowEpoch, millis() / kMillisToSeconds);
      }
    }
    if (!synced && estimated && !warnedEstimated) {
      systemLogger.warning("Horloge non synchronisée, historique estimé depuis la dernière heure connue");
      warnedEstimated = true;
    }
  }

  DataPoint point;
  point.timestamp = nowEpoch;
  // feature-067 : getPh() renvoie le pH CORRIGÉ (brut + offset) — c'est la valeur
  // "vraie" du bassin, donc celle qui doit être historisée.
  point.ph = sensors.getPh();
  point.orp = sensors.getOrp();
  point.temperature = sensors.getTemperature();
  // feature-095 : la valeur FILTRÉE, celle sur laquelle la régulation décide
  // (mêmes accesseurs que /data). NaN tant que le filtre n'est pas amorcé —
  // AUCUN repli sur le brut : « pas de filtrée » doit rester observable.
  point.phFiltered = sensors.getPhFiltered();
  point.orpFiltered = sensors.getOrpFiltered();
  point.filtrationActive = filtration.isRunning();
  point.phDosing = PumpController.isPhDosing();
  point.orpDosing = PumpController.isOrpDosing();
  point.granularity = RAW;

  // feature-061 (it. 2) : filtration arrêtée ou purge post-reprise en cours →
  // les mesures pH/ORP/température ne représentent pas l'eau du bassin (la
  // sonde en canalisation mesure l'ambiante du local à l'arrêt) : exclues du
  // point (NaN → "null" à la sérialisation, ignorées par l'agrégation → trous
  // dans les courbes). État filtration et flags dosage INCHANGÉS.
  if (!measureValidity.measuresValid()) {
    point.ph = NAN;
    point.orp = NAN;
    point.temperature = NAN;
    // feature-095 : la filtrée subit le MÊME sort que le brut — une mesure non
    // représentative filtrée reste une mesure non représentative.
    point.phFiltered = NAN;
    point.orpFiltered = NAN;
  }

  memoryBuffer.push_back(point);

  // Limiter le nombre de points RAW
  size_t rawCount = 0;
  for (const auto& p : memoryBuffer) {
    if (p.granularity == RAW) rawCount++;
  }

  if (rawCount > MAX_RAW_POINTS) {
    // Supprimer les points RAW en excès (les plus anciens)
    // Utilise erase-remove idiom pour éviter O(n²)
    size_t toRemove = rawCount - MAX_RAW_POINTS;
    size_t removed = 0;
    memoryBuffer.erase(
      std::remove_if(memoryBuffer.begin(), memoryBuffer.end(),
        [&removed, toRemove](const DataPoint& p) {
          if (removed < toRemove && p.granularity == RAW) {
            removed++;
            return true;
          }
          return false;
        }),
      memoryBuffer.end()
    );
  }
}

void HistoryManager::saveToFile() {
  if (!historyEnabled) return;

  // bug-100 / ADR-0070 (§d) — ADMISSION MÉMOIRE EN TÊTE, classe A (8 192).
  // La garde va là où la fonction peut encore NE RIEN FAIRE, jamais au point
  // d'ouverture, pour deux raisons distinctes et toutes deux dirimantes :
  //  1. saveToFile() n'est PAS atomique — le remove() precede l'open("w") : une
  //     garde sur l'ouverture echangerait l'abort() contre la DESTRUCTION du
  //     fichier d'historique ;
  //  2. historySaveAdmit() consomme `oldSize`, une taille obtenue PAR le fopen
  //     de lecture plus bas : garder ce fopen-là le laisserait s'executer nu.
  // Classe A et non B : ce chemin est atteignable depuis POST /history/import
  // (→ importData() → saveToFile()) sur la tâche async_tcp, donc avec des pbuf
  // lwip en vol.
  // Le libellé, le compteur et le champ de diagnostic sont DISTINCTS de ceux de
  // historySaveAdmit() : « partition pleine » et « famine mémoire » sont deux
  // pannes différentes, elles doivent rester discernables dans le journal.
  if (!fsGuardAdmit(kStaticFileMinAllocBytes)) {
    sHistoryFopenDeferrals.fetch_add(1, std::memory_order_relaxed);
    // WARNING throttlé (jamais ERROR : un ERROR flusherait le disque en
    // synchrone → fopen → l'abort() prévenu). Retentative naturelle au prochain
    // cycle SAVE_INTERVAL, dans 5 min.
    static unsigned long sWarnHeapMs = 0;
    unsigned long nowHeapMs = millis();
    if (sWarnHeapMs == 0 || nowHeapMs - sWarnHeapMs >= kHistorySaveWarnThrottleMs) {
      systemLogger.warning("Famine memoire : sauvegarde de l'historique sautee (" +
                           fopenRefusalDetail() + ") — nouvelle tentative dans 5 min");
      sWarnHeapMs = nowHeapMs;
    }
    return;
  }

  // bug-nospc-partition-history (A2+A3) : construire et MESURER le JSON avant
  // de toucher au FS — l'ancien code ouvrait "w" d'entrée, ce qui tronquait
  // l'ancien fichier avant toute vérification d'espace.
  JsonDocument doc;
  JsonArray data = doc["data"].to<JsonArray>();

  // Sauvegarder tous les points (déjà optimisés par consolidation)
  for (const auto& point : memoryBuffer) {
    JsonObject obj = data.add<JsonObject>();
    obj["t"] = point.timestamp;
    if (isfinite(point.ph))          obj["p"] = serialized(String(point.ph, 2));
    if (isfinite(point.orp))         obj["o"] = serialized(String(point.orp, 1));
    if (isfinite(point.temperature)) obj["T"] = serialized(String(point.temperature, 1));
    // feature-095 : la filtrée n'est persistée QUE sur les points RAW (5 min,
    // ~8 h de profondeur). Cette garde est LE rempart du budget de la partition
    // history (64 Ko) : toutes granularités confondues coûteraient ~7,1 Ko et
    // porteraient l'occupation à 16 blocs sur 16 — historySaveAdmit() refuserait
    // alors la sauvegarde et l'historique cesserait SILENCIEUSEMENT d'être
    // persisté. En RAW seul : +2 016 o, profondeur horaire (7 j) intacte. La
    // filtrée étant un signal passe-bas, sa moyenne horaire coïnciderait de
    // toute façon avec celle du brut : série redondante, coût réel.
    if (point.granularity == RAW) {
      if (isfinite(point.phFiltered))  obj["pf"] = serialized(String(point.phFiltered, 2));
      if (isfinite(point.orpFiltered)) obj["of"] = serialized(String(point.orpFiltered, 1));
    }
    obj["f"] = point.filtrationActive;
    obj["d"] = point.phDosing || point.orpDosing;
    obj["g"] = static_cast<uint8_t>(point.granularity);
  }
  const size_t jsonSize = measureJson(doc);

  // bug-100 / revue — SECONDE ADMISSION, et c'est la décisive.
  // La première (tête de fonction) est exigée par ADR-0070 §d : la garde va là
  // où la fonction peut encore ne rien faire. Mais entre elle et l'ouverture
  // ci-dessous — LE SITE EXACT DU COREDUMP DU 05/09 — on vient de construire le
  // JsonDocument et ses String de sérialisation, soit des dizaines de kilooctets.
  // La marge ×1,99 validée par la première mesure est donc DÉPENSÉE APRÈS
  // elle : autoriser sur cette seule mesure reviendrait à ouvrir sur une valeur
  // qui n'est plus vraie — exactement le mode de défaillance que l'asymétrie
  // mesure/décision existe pour interdire.
  // Placement : après measureJson, donc après le pic d'allocation, et toujours
  // AVANT le remove() plus bas — l'opération peut encore ne rien faire.
  // L'ADR exige une garde AVANT la lecture d'oldSize ; il n'en interdit pas une
  // seconde, et le coût est une mesure de plus toutes les 5 min.
  if (!fsGuardAdmit(kStaticFileMinAllocBytes)) {
    sHistoryFopenDeferrals.fetch_add(1, std::memory_order_relaxed);
    static unsigned long sWarnHeapPostJsonMs = 0;
    unsigned long nowPostJsonMs = millis();
    if (sWarnHeapPostJsonMs == 0 ||
        nowPostJsonMs - sWarnHeapPostJsonMs >= kHistorySaveWarnThrottleMs) {
      systemLogger.warning("Famine memoire apres serialisation : sauvegarde de "
                           "l'historique sautee (" + fopenRefusalDetail() +
                           ") — nouvelle tentative dans 5 min");
      sWarnHeapPostJsonMs = nowPostJsonMs;
    }
    return;
  }

  // Espace libre + taille de l'ancien fichier (0 si absent). historyStore
  // pointe toujours sur un fs::LittleFSFS (partition dédiée après begin(),
  // sinon le LittleFS principal) — totalBytes()/usedBytes() y sont exposés.
  fs::LittleFSFS* lfs = (historyStore == &historyFs) ? &historyFs : &LittleFS;
  const size_t freeBytes = lfs->totalBytes() - lfs->usedBytes();
  size_t oldSize = 0;
  {
    // Admission déjà prononcée JUSTE AU-DESSUS, après le pic d'allocation du
    // JSON (kFsGuardAlreadyAdmitted) : une seule mesure pour les deux
    // ouvertures de la sauvegarde, et elle est postérieure à la dépense.
    File old = openIfHeapAllows(historyStore, historyFilePath, "r", kFsGuardAlreadyAdmitted);
    if (old) {
      oldSize = old.size();
      old.close();
    }
  }

  // Garde d'espace (A2) : refuser AVANT de toucher à l'ancien fichier — mieux
  // vaut un historique périmé de 5 min qu'un fichier tronqué. Retentative
  // naturelle au prochain cycle SAVE_INTERVAL.
  if (!historySaveAdmit(freeBytes, oldSize, jsonSize, kHistorySaveSpaceMarginBytes)) {
    // A6 : WARNING (jamais ERROR) — un ERROR déclencherait un flush disque
    // immédiat sur le FS déjà plein toutes les 5 min (flush-storm).
    static unsigned long sWarnFullMs = 0;
    unsigned long nowMs = millis();
    if (sWarnFullMs == 0 || nowMs - sWarnFullMs >= kHistorySaveWarnThrottleMs) {
      systemLogger.warning("Partition history pleine : save sauté (libre=" + String(freeBytes) +
                           ", requis=" + String(jsonSize + kHistorySaveSpaceMarginBytes) +
                           ") — nouvelle tentative dans 5 min");
      sWarnFullMs = nowMs;
    }
    recordHistorySaveFailureNvs(_lastOkEpoch);
    return;
  }

  // Admis : remove AVANT réécriture — l'ancien et le nouveau fichier ne
  // coexistent plus, le pic copy-on-write est divisé par 2. Trade-off ADR-0033 :
  // un crash dans la fenêtre remove→write perd l'historique (borné par le
  // disjoncteur feature-059 et la cadence 5 min).
  historyStore->remove(historyFilePath);

  File f = openIfHeapAllows(historyStore, historyFilePath, "w", kFsGuardAlreadyAdmitted);
  if (!f) {
    static unsigned long sWarnOpenMs = 0;
    unsigned long nowMs = millis();
    if (sWarnOpenMs == 0 || nowMs - sWarnOpenMs >= kHistorySaveWarnThrottleMs) {
      systemLogger.warning("Impossible d'ouvrir /history.json en écriture — save sauté");
      sWarnOpenMs = nowMs;
    }
    recordHistorySaveFailureNvs(_lastOkEpoch);
    return;
  }

  const size_t written = serializeJson(doc, f);
  f.close();

  // Détection d'écriture partielle (A3) : le retour de serializeJson doit être
  // EXACTEMENT la taille mesurée (fs::File n'expose pas d'état d'erreur fiable
  // après un write LittleFS court — la comparaison suffit). Échec → jamais de
  // fichier partiel silencieux sur le FS.
  if (written != jsonSize) {
    historyStore->remove(historyFilePath);
    static unsigned long sWarnPartialMs = 0;
    unsigned long nowMs = millis();
    if (sWarnPartialMs == 0 || nowMs - sWarnPartialMs >= kHistorySaveWarnThrottleMs) {
      systemLogger.warning("Écriture partielle de l'historique (" + String(written) + "/" +
                           String(jsonSize) + " octets) — fichier supprimé, nouvelle tentative dans 5 min");
      sWarnPartialMs = nowMs;
    }
    recordHistorySaveFailureNvs(_lastOkEpoch);
    return;
  }

  // Succès : epoch RAM du dernier save OK (persisté en NVS uniquement sur échec).
  time_t nowEpoch = time(nullptr);
  _lastOkEpoch = isTimeValid(nowEpoch) ? static_cast<unsigned long>(nowEpoch) : 0;

  // Instrumentation (A5) : occupation FS, throttlée (même période que les warn).
  static unsigned long sInfoUsageMs = 0;
  unsigned long nowMs = millis();
  if (sInfoUsageMs == 0 || nowMs - sInfoUsageMs >= kHistorySaveWarnThrottleMs) {
    systemLogger.info("Partition history : " + String(lfs->usedBytes()) + "/" +
                      String(lfs->totalBytes()) + " octets utilisés");
    sInfoUsageMs = nowMs;
  }

  systemLogger.debug("Historique sauvegardé (" + String(data.size()) + " points)");
}

void HistoryManager::loadFromFile() {
  if (!historyEnabled) return;

  // bug-100 / ADR-0070 — CLASSE B (5 120), tripwire de boot.
  if (!fsGuardAdmit(kInternalFopenMinAllocBytes)) {
    sHistoryFopenDeferrals.fetch_add(1, std::memory_order_relaxed);
    systemLogger.warning("Memoire insuffisante pour charger l'historique — "
                         "historique persistant desactive pour ce demarrage");
    historyEnabled = false;
    return;
  }

  // L'exists() qui precedait a ete SUPPRIME : VFSImpl::exists() ne fait pas de
  // stat(), il construit un VFSFileImpl — c'est-a-dire un fopen complet, verrou
  // newlib compris. Il etait donc strictement redondant avec l'ouverture
  // ci-dessous, tout en doublant l'exposition a l'abort(). C'est le site
  // qu'aucun grep "open(" ne trouve (docs/subsystems/acces-fichiers.md).
  File f = openIfHeapAllows(historyStore, historyFilePath, "r", kFsGuardAlreadyAdmitted);
  if (!f) {
    // Libellé volontairement NON DISCRIMINANT : sans l'exists() supprimé
    // ci-dessus, « absent » et « illisible » ne se distinguent plus — et l'API
    // fs:: n'offre aucun stat() pour les départager sans PAYER une seconde
    // ouverture, c'est-à-dire sans réintroduire le site qu'on vient de fermer.
    // On dit donc ce que l'on sait, ni plus ni moins. INFO et jamais ERROR :
    // l'absence au premier boot est le cas nominal, et un ERROR flusherait le
    // disque en synchrone.
    systemLogger.info("Aucun historique exploitable (fichier absent ou illisible)");
    return;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, f);
  f.close();

  if (error) {
    systemLogger.error("Erreur parsing historique: " + String(error.c_str()));
    return;
  }

  JsonArray data = doc["data"];
  memoryBuffer.clear();
  legacyHistoryPending = false;
  legacyMaxTimestamp = 0;

  for (JsonObject point : data) {
    DataPoint dp;
    dp.timestamp = point["t"];
    dp.ph = point["p"] | NAN;
    dp.orp = point["o"] | NAN;
    dp.temperature = point["T"] | NAN;
    // feature-095 : tolérant à l'absence — un /history.json écrit par une
    // version ≤ 2.37 (ou un agrégat, jamais sérialisé avec la filtrée) se relit
    // sans perte, la filtrée valant simplement NaN.
    dp.phFiltered = point["pf"] | NAN;
    dp.orpFiltered = point["of"] | NAN;
    dp.filtrationActive = point["f"];
    dp.phDosing = point["d"];
    dp.orpDosing = false;
    dp.granularity = static_cast<Granularity>(point["g"] | 0);
    memoryBuffer.push_back(dp);
  }

  if (!memoryBuffer.empty()) {
    for (const auto& p : memoryBuffer) {
      if (p.timestamp > legacyMaxTimestamp) legacyMaxTimestamp = p.timestamp;
    }
    if (legacyMaxTimestamp > 0 && legacyMaxTimestamp < static_cast<unsigned long>(kMinValidEpoch)) {
      legacyHistoryPending = true;
      systemLogger.warning("Historique legacy détecté (timestamps uptime)");
      time_t nowEpoch = time(nullptr);
      if (isTimeValid(nowEpoch)) {
        migrateLegacyHistory(static_cast<unsigned long>(nowEpoch));
      }
    }
  }

  // bug-history-doublons-troncature (A2) : tri défensif (fichier normalement
  // déjà trié) puis fusion — assainit EN UNE FOIS les agrégats dupliqués
  // persistés par les versions ≤ 2.18.0 (jusqu'à ~12 doublons/heure).
  std::sort(memoryBuffer.begin(), memoryBuffer.end(),
    [](const DataPoint& a, const DataPoint& b) {
      return a.timestamp < b.timestamp;
    });
  dedupAggregatedPoints();

  // bug-points-futurs : purge des points horodatés dans le futur (impossibles
  // par construction — excursion d'horloge pendant une session de reboots ;
  // constaté en prod : agrégat journalier fantôme daté du jour courant).
  // Uniquement si l'horloge est disponible (RTC/NTP) — now==0 : on ne juge pas.
  {
    bool synced = false;
    bool estimated = false;
    unsigned long nowEpoch = getCurrentEpoch(&synced, &estimated);
    if (nowEpoch > 0) {
      size_t before = memoryBuffer.size();
      unsigned long horizon = nowEpoch + kHistoryFutureToleranceSeconds;
      memoryBuffer.erase(
        std::remove_if(memoryBuffer.begin(), memoryBuffer.end(),
          [horizon](const DataPoint& p) { return p.timestamp > horizon; }),
        memoryBuffer.end()
      );
      if (memoryBuffer.size() != before) {
        systemLogger.info("Historique : " + String((unsigned)(before - memoryBuffer.size())) +
                          " point(s) horodaté(s) dans le futur purgé(s)");
      }
    }
  }

  systemLogger.info("Historique chargé (" + String(memoryBuffer.size()) + " points)");
}

void HistoryManager::migrateLegacyHistory(unsigned long nowEpoch) {
  if (!legacyHistoryPending || legacyMaxTimestamp == 0) return;

  for (auto& p : memoryBuffer) {
    unsigned long delta = legacyMaxTimestamp - p.timestamp;
    p.timestamp = nowEpoch - delta;
  }

  legacyHistoryPending = false;
  legacyMaxTimestamp = 0;
  systemLogger.warning("Historique legacy converti en epoch");
  saveToFile();
}

void HistoryManager::_applyPreNtpCorrection(unsigned long ntpEpoch, unsigned long uptimeSec) {
  // Corrige les timestamps provisoires (uptime en secondes) enregistrés avant la sync NTP.
  // Formule : timestamp_réel = uptime_enregistré + (epoch_NTP - uptime_courant)
  // Seuls les points de ce boot sont touchés : timestamp < kMinValidEpoch ET <= uptimeSec.
  // Les points "legacy" de boots précédents ont des timestamps qui peuvent dépasser uptimeSec.
  if (ntpEpoch <= uptimeSec) return;  // sécurité : l'epoch doit être > uptime
  unsigned long bootOffset = ntpEpoch - uptimeSec;

  int count = 0;
  for (auto& p : memoryBuffer) {
    if (p.timestamp < static_cast<unsigned long>(kMinValidEpoch) && p.timestamp <= uptimeSec) {
      p.timestamp += bootOffset;
      count++;
    }
  }

  _preNtpPending = false;
  if (count > 0) {
    systemLogger.info("Historique pré-NTP: " + String(count) + " point(s) corrigé(s) après sync NTP");
  }
}

std::vector<DataPoint> HistoryManager::getLastHours(int hours) {
  std::vector<DataPoint> result;

  bool synced = false;
  bool estimated = false;
  unsigned long nowEpoch = getCurrentEpoch(&synced, &estimated);

  // feature-027 : timeout → vecteur vide (le client réessaiera)
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(kHistoryMutexTimeoutMs)) != pdTRUE) {
    static unsigned long sWarnGetHoursMs = 0;
    warnHistoryMutexTimeout(sWarnGetHoursMs, "getLastHours");
    return result;
  }

  if (memoryBuffer.empty()) {
    xSemaphoreGive(_mutex);
    return result;
  }

  if (nowEpoch == 0) {
    result = memoryBuffer;
    xSemaphoreGive(_mutex);
    return result;
  }
  if (synced) {
    migrateLegacyHistory(nowEpoch);
  }

  unsigned long now = nowEpoch;
  unsigned long rangeSeconds = hours * kSecondsPerHour;
  if (now < rangeSeconds) {
    result = memoryBuffer;
    xSemaphoreGive(_mutex);
    return result;
  }
  unsigned long cutoff = now - rangeSeconds;

  for (const auto& point : memoryBuffer) {
    if (point.timestamp >= cutoff) {
      result.push_back(point);
    }
  }

  xSemaphoreGive(_mutex);
  return result;
}

std::vector<DataPoint> HistoryManager::getLastDay() {
  return getLastHours(24);
}

std::vector<DataPoint> HistoryManager::getAllData() {
  // feature-027 : timeout → vecteur vide
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(kHistoryMutexTimeoutMs)) != pdTRUE) {
    static unsigned long sWarnGetAllMs = 0;
    warnHistoryMutexTimeout(sWarnGetAllMs, "getAllData");
    return std::vector<DataPoint>();
  }
  std::vector<DataPoint> copy = memoryBuffer;
  xSemaphoreGive(_mutex);
  return copy;
}

bool HistoryManager::snapshotFiltered(std::vector<DataPoint>& out,
                                      unsigned long cutoff, unsigned long since) {
  // bug-history-doublons-troncature (itération 2) : copie filtrée du buffer en
  // UNE prise de mutex — le callback chunked de /get-history streame ensuite
  // depuis cette copie sans jamais reprendre le mutex (l'accès point par point
  // sous timeout court produisait des réponses tronquées, démontré en prod).
  // Timeout kHistorySnapshotTimeoutMs (100 ms) : appelé depuis le handler HTTP
  // (tâche async_tcp) AVANT le début de la réponse — un refus rare vaut mieux
  // qu'un blocage de 2 s sur la consolidation.
  out.clear();
  if (_mutex == nullptr ||
      xSemaphoreTake(_mutex, pdMS_TO_TICKS(kHistorySnapshotTimeoutMs)) != pdTRUE) {
    return false;
  }
  out.reserve(memoryBuffer.size());
  for (const auto& p : memoryBuffer) {
    if (p.timestamp < cutoff) continue;
    if (since > 0 && p.timestamp <= since) continue;
    out.push_back(p);
  }
  xSemaphoreGive(_mutex);
  return true;
}

bool HistoryManager::importData(const std::vector<DataPoint>& dataPoints) {
  if (!historyEnabled) return false;
  if (dataPoints.empty()) return false;

  // feature-027 : timeout → return false AVANT toute modif (la route teste le retour)
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(kHistoryMutexTimeoutMs)) != pdTRUE) {
    static unsigned long sWarnImportMs = 0;
    warnHistoryMutexTimeout(sWarnImportMs, "importData");
    return false;
  }
  memoryBuffer = dataPoints;
  std::sort(memoryBuffer.begin(), memoryBuffer.end(),
    [](const DataPoint& a, const DataPoint& b) {
      return a.timestamp < b.timestamp;
    });
  // bug-history-doublons-troncature (A2) : un import peut porter des doublons
  // (export d'une version ≤ 2.18.0) — même fusion qu'au chargement.
  dedupAggregatedPoints();

  legacyHistoryPending = false;
  legacyMaxTimestamp = 0;
  lastSave = millis();
  lastRecord = millis();
  saveToFile();
  size_t count = memoryBuffer.size();
  xSemaphoreGive(_mutex);

  systemLogger.info("Historique importé (" + String(count) + " points)");
  return true;
}

void HistoryManager::consolidateData() {
  if (!historyEnabled) return;
  bool synced = false;
  bool estimated = false;
  unsigned long now = getCurrentEpoch(&synced, &estimated);
  if (now == 0) {
    if (!warnedUnsynced) {
      systemLogger.warning("Horloge non synchronisée, consolidation ignorée");
      warnedUnsynced = true;
    }
    return;
  }
  if (synced) {
    migrateLegacyHistory(now);
    if (_preNtpPending) {
      _applyPreNtpCorrection(now, millis() / kMillisToSeconds);
    }
  }
  // 1. Supprimer les données trop anciennes (> 90 jours) — future-safe
  // (bug-points-futurs) : un point horodaté dans le futur n'est PAS « ancien »
  // (le wrap uint32 le purgeait silencieusement).
  memoryBuffer.erase(
    std::remove_if(memoryBuffer.begin(), memoryBuffer.end(),
      [now](const DataPoint& p) {
        return historyIsExpired((uint32_t)now, (uint32_t)p.timestamp, (uint32_t)DAILY_MAX_AGE);
      }),
    memoryBuffer.end()
  );

  // 2. Convertir les points RAW > 6h en moyennes horaires
  std::vector<DataPoint> hourlyPoints;

  // Grouper les points RAW par heure
  std::map<unsigned long, std::vector<DataPoint>> hourlyGroups;

  for (const auto& point : memoryBuffer) {
    if (point.granularity == RAW) {
      // bug-history-doublons-troncature (A1) : ne consolider que les créneaux
      // COMPLETS (dernière seconde de l'heure > RAW_MAX_AGE). L'ancien prédicat
      // par point convertissait chaque heure en tranches successives au fil des
      // passages (jusqu'à 12 agrégats/heure au même timestamp).
      unsigned long hourTimestamp = bucketTimestamp((uint32_t)point.timestamp, kSecondsPerHour);
      if (historyBucketConsolidable((uint32_t)now, (uint32_t)hourTimestamp,
                                    (uint32_t)kSecondsPerHour, (uint32_t)RAW_MAX_AGE)) {
        hourlyGroups[hourTimestamp].push_back(point);
      }
    }
  }

  // Créer les moyennes horaires
  for (const auto& group : hourlyGroups) {
    if (group.second.empty()) continue;

    DataPoint avgPoint;
    avgPoint.timestamp = group.first;
    avgPoint.ph = 0;
    avgPoint.orp = 0;
    avgPoint.temperature = 0;
    // feature-095 : la filtrée n'est PAS agrégée — elle n'existe que sur les
    // points RAW (cf. saveToFile). Explicitement NaN pour ne dépendre d'aucun
    // état antérieur, et pour ne surtout pas toucher au `validCount` partagé
    // ci-dessous (invariant de caractérisation gelé).
    avgPoint.phFiltered = NAN;
    avgPoint.orpFiltered = NAN;
    avgPoint.filtrationActive = false;
    avgPoint.phDosing = false;
    avgPoint.orpDosing = false;
    avgPoint.granularity = HOURLY;

    int validCount = 0;
    int filtrationCount = 0;
    int phDosingCount = 0;
    int orpDosingCount = 0;

    for (const auto& p : group.second) {
      if (!isnan(p.ph)) {
        avgPoint.ph += p.ph;
        validCount++;
      }
      if (!isnan(p.orp)) avgPoint.orp += p.orp;
      if (!isnan(p.temperature)) avgPoint.temperature += p.temperature;
      if (p.filtrationActive) filtrationCount++;
      if (p.phDosing) phDosingCount++;
      if (p.orpDosing) orpDosingCount++;
    }

    if (validCount > 0) {
      avgPoint.ph = finalizeMean(avgPoint.ph, validCount);
      avgPoint.orp = finalizeMean(avgPoint.orp, validCount);
      avgPoint.temperature = finalizeMean(avgPoint.temperature, validCount);
      avgPoint.filtrationActive = isMajority(filtrationCount, (int)group.second.size());
      avgPoint.phDosing = anyTrue(phDosingCount);
      avgPoint.orpDosing = anyTrue(orpDosingCount);

      hourlyPoints.push_back(avgPoint);
    }
  }

  // Supprimer les points RAW qui ont été convertis en horaires — MÊME prédicat
  // que le groupement ci-dessus (créneau complet), sinon perte ou re-conversion.
  memoryBuffer.erase(
    std::remove_if(memoryBuffer.begin(), memoryBuffer.end(),
      [now](const DataPoint& p) {
        if (p.granularity != RAW) return false;
        unsigned long hourTs = bucketTimestamp((uint32_t)p.timestamp, kSecondsPerHour);
        return historyBucketConsolidable((uint32_t)now, (uint32_t)hourTs,
                                         (uint32_t)kSecondsPerHour, (uint32_t)RAW_MAX_AGE);
      }),
    memoryBuffer.end()
  );

  // Ajouter les nouveaux points horaires
  memoryBuffer.insert(memoryBuffer.end(), hourlyPoints.begin(), hourlyPoints.end());

  // 3. Convertir les points HOURLY > 15 jours en moyennes journalières
  std::vector<DataPoint> dailyPoints;
  std::map<unsigned long, std::vector<DataPoint>> dailyGroups;

  for (const auto& point : memoryBuffer) {
    if (point.granularity == HOURLY) {
      // bug-history-doublons-troncature (A1) : créneau JOUR complet uniquement
      // (même logique que RAW→HOURLY ci-dessus).
      unsigned long dayTimestamp = bucketTimestamp((uint32_t)point.timestamp, 86400UL);
      if (historyBucketConsolidable((uint32_t)now, (uint32_t)dayTimestamp,
                                    86400UL, (uint32_t)HOURLY_MAX_AGE)) {
        dailyGroups[dayTimestamp].push_back(point);
      }
    }
  }

  // Créer les moyennes journalières
  for (const auto& group : dailyGroups) {
    if (group.second.empty()) continue;

    DataPoint avgPoint;
    avgPoint.timestamp = group.first;
    avgPoint.ph = 0;
    avgPoint.orp = 0;
    avgPoint.temperature = 0;
    // feature-095 : idem HOURLY — aucune agrégation de la filtrée.
    avgPoint.phFiltered = NAN;
    avgPoint.orpFiltered = NAN;
    avgPoint.filtrationActive = false;
    avgPoint.phDosing = false;
    avgPoint.orpDosing = false;
    avgPoint.granularity = DAILY;

    int validCount = 0;
    int filtrationCount = 0;
    int phDosingCount = 0;
    int orpDosingCount = 0;

    for (const auto& p : group.second) {
      if (!isnan(p.ph)) {
        avgPoint.ph += p.ph;
        validCount++;
      }
      if (!isnan(p.orp)) avgPoint.orp += p.orp;
      if (!isnan(p.temperature)) avgPoint.temperature += p.temperature;
      if (p.filtrationActive) filtrationCount++;
      if (p.phDosing) phDosingCount++;
      if (p.orpDosing) orpDosingCount++;
    }

    if (validCount > 0) {
      avgPoint.ph = finalizeMean(avgPoint.ph, validCount);
      avgPoint.orp = finalizeMean(avgPoint.orp, validCount);
      avgPoint.temperature = finalizeMean(avgPoint.temperature, validCount);
      avgPoint.filtrationActive = isMajority(filtrationCount, (int)group.second.size());
      avgPoint.phDosing = anyTrue(phDosingCount);
      avgPoint.orpDosing = anyTrue(orpDosingCount);

      dailyPoints.push_back(avgPoint);
    }
  }

  // Supprimer les points HOURLY qui ont été convertis en journaliers — MÊME
  // prédicat que le groupement (créneau jour complet).
  memoryBuffer.erase(
    std::remove_if(memoryBuffer.begin(), memoryBuffer.end(),
      [now](const DataPoint& p) {
        if (p.granularity != HOURLY) return false;
        unsigned long dayTs = bucketTimestamp((uint32_t)p.timestamp, 86400UL);
        return historyBucketConsolidable((uint32_t)now, (uint32_t)dayTs,
                                         86400UL, (uint32_t)HOURLY_MAX_AGE);
      }),
    memoryBuffer.end()
  );

  // Ajouter les nouveaux points journaliers
  memoryBuffer.insert(memoryBuffer.end(), dailyPoints.begin(), dailyPoints.end());

  // 4. Limiter le nombre de points par granularité
  size_t hourlyCount = 0, dailyCount = 0;
  for (const auto& p : memoryBuffer) {
    if (p.granularity == HOURLY) hourlyCount++;
    if (p.granularity == DAILY) dailyCount++;
  }

  // Supprimer les points horaires en excès (les plus anciens)
  // Utilise erase-remove idiom pour éviter O(n²)
  if (hourlyCount > MAX_HOURLY_POINTS) {
    size_t toRemove = hourlyCount - MAX_HOURLY_POINTS;
    size_t removed = 0;
    memoryBuffer.erase(
      std::remove_if(memoryBuffer.begin(), memoryBuffer.end(),
        [&removed, toRemove](const DataPoint& p) {
          if (removed < toRemove && p.granularity == HOURLY) {
            removed++;
            return true;
          }
          return false;
        }),
      memoryBuffer.end()
    );
  }

  // Supprimer les points journaliers en excès (les plus anciens)
  // Utilise erase-remove idiom pour éviter O(n²)
  if (dailyCount > MAX_DAILY_POINTS) {
    size_t toRemove = dailyCount - MAX_DAILY_POINTS;
    size_t removed = 0;
    memoryBuffer.erase(
      std::remove_if(memoryBuffer.begin(), memoryBuffer.end(),
        [&removed, toRemove](const DataPoint& p) {
          if (removed < toRemove && p.granularity == DAILY) {
            removed++;
            return true;
          }
          return false;
        }),
      memoryBuffer.end()
    );
  }

  // Trier par timestamp
  std::sort(memoryBuffer.begin(), memoryBuffer.end(),
    [](const DataPoint& a, const DataPoint& b) {
      return a.timestamp < b.timestamp;
    });

  // bug-history-doublons-troncature (A2) : fusion des agrégats du même créneau
  // (ceinture-bretelles derrière A1 — et rattrapage d'un éventuel import).
  dedupAggregatedPoints();

  systemLogger.debug("Consolidation terminée: " + String(memoryBuffer.size()) + " points");
  saveToFile();
}

// Fusionne les agrégats adjacents de même (timestamp, granularité ≠ RAW).
// Buffer supposé trié par timestamp. Fusion équipondérée champ à champ
// (historyMergeMeanFinite), booléens en OR — approximation assumée pour les
// doublons hérités des versions ≤ 2.18.0 (effectifs d'origine non stockés).
void HistoryManager::dedupAggregatedPoints() {
  if (memoryBuffer.size() < 2) return;
  size_t before = memoryBuffer.size();
  std::vector<DataPoint> out;
  out.reserve(memoryBuffer.size());
  for (const auto& p : memoryBuffer) {
    // Recherche du doublon dans la PLAGE d'ex æquo (même timestamp) déjà émise :
    // le tri n'est que par timestamp (non stable), un point d'une autre
    // granularité peut s'intercaler entre deux doublons du même créneau.
    DataPoint* dup = nullptr;
    if (p.granularity != RAW) {
      for (size_t j = out.size(); j > 0 && out[j - 1].timestamp == p.timestamp; --j) {
        if (out[j - 1].granularity == p.granularity) {
          dup = &out[j - 1];
          break;
        }
      }
    }
    if (dup != nullptr) {
      dup->ph = historyMergeMeanFinite(dup->ph, p.ph);
      dup->orp = historyMergeMeanFinite(dup->orp, p.orp);
      dup->temperature = historyMergeMeanFinite(dup->temperature, p.temperature);
      // feature-095 : phFiltered/orpFiltered VOLONTAIREMENT non fusionnés — ce
      // chemin ne traite que des agrégats (granularity != RAW), qui ne portent
      // jamais de filtrée (NaN par initialiseur de membre, jamais sérialisée).
      // Ne rien écrire ici garantit qu'aucune filtrée n'est inventée.
      dup->filtrationActive = dup->filtrationActive || p.filtrationActive;
      dup->phDosing = dup->phDosing || p.phDosing;
      dup->orpDosing = dup->orpDosing || p.orpDosing;
      continue;
    }
    out.push_back(p);
  }
  if (out.size() != before) {
    systemLogger.info("Historique : " + String((unsigned)(before - out.size())) +
                      " agrégats dupliqués fusionnés");
    memoryBuffer.swap(out);
  }
}

bool HistoryManager::clearHistory() {
  if (!historyEnabled) return false;
  // feature-027 : timeout → return false SANS supprimer le fichier (RAM non vidée)
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(kHistoryMutexTimeoutMs)) != pdTRUE) {
    static unsigned long sWarnClearMs = 0;
    warnHistoryMutexTimeout(sWarnClearMs, "clearHistory");
    return false;
  }
  memoryBuffer.clear();
  xSemaphoreGive(_mutex);
  historyStore->remove(historyFilePath);
  systemLogger.warning("Historique effacé");
  return true;
}
