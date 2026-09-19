#include "history.h"
#include "history_logic.h"
#include "history_import_logic.h"  // bug-101 : regles pures de l'import par lots (ADR-0017)
#include "history_guard_logic.h"
#include "history_persist_format.h"  // feature-131 : format pur du JSON persiste (ADR-0017)
#include "fs_guard.h"          // bug-100 : point d'admission unique des fopen internes (ADR-0070)
#include "heap_metrics.h"      // feature-131 (F1) : sonde maxBloc8 PENDANT l'ecriture
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
// feature-131 : fichier temporaire de l'écriture atomique (patron du journal,
// ADR-0076) — écrit intégralement PUIS renommé par-dessus historyFilePath.
const char* historyTmpFilePath = "/history.json.tmp";
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

// ---------------------------------------------------------------------------
// feature-131 (v2.53.0) — les DEUX puits de HistoryManager::emitPersistedJson()
// ---------------------------------------------------------------------------
// « Un seul émetteur, deux puits » : emitPersistedJson() ne connaît QUE le
// contrat HistoryByteSink (history.h). Le choix du puits — mesurer sans effet
// de bord, ou écrire réellement — se fait entièrement côté appelant
// (saveToFile()), ce qui rend une divergence entre passe 1 (mesure) et passe 2
// (écriture) impossible par construction : c'est EXACTEMENT le même code de
// formatage qui alimente les deux.

// Puits COMPTEUR (passe 1) : n'écrit rien, ne fait aucun accès FS — juste
// additionner la taille des tronçons reçus dans le size_t pointé par `ctx`.
size_t historyCounterSink(void* ctx, const char* p, size_t n) {
  (void)p;
  *static_cast<size_t*>(ctx) += n;
  return n;
}

// Puits FICHIER (passe 2) : accumule les octets dans un tampon de PILE
// (kHistoryWriteChunkBytes) et ne touche au fichier qu'au remplissage du
// tampon — un f.write() toutes les ~4 points plutôt qu'un par point. `ok`
// passe à false au premier write() LittleFS court et le puits cesse alors
// d'accepter des octets (fileSinkWrite renvoie moins que demandé) : c'est ce
// qui fait chuter la somme renvoyée par emitPersistedJson() sous jsonSize,
// déclenchant la détection d'écriture partielle de saveToFile() (B6).
//
// F1 : échantillonne heapLargestFreeBlock8Bit() tous les ~64 appels (proxy de
// « tous les 64 points » — le préfixe/suffixe décalent le compte d'au plus 2,
// sans conséquence sur une cadence de 64) et garde le MINIMUM observé PENDANT
// l'écriture — c'est la mesure « pendant, pas autour » exigée par la spec.
struct HistoryFileWriteSink {
  File* f;
  char buf[kHistoryWriteChunkBytes];
  size_t used = 0;
  bool ok = true;
  uint32_t callCount = 0;
  uint32_t minMaxAlloc8DuringWrite = UINT32_MAX;

  // Vide le tampon vers le fichier. `ok` reste sticky : un appel après échec
  // est un no-op qui renvoie false sans re-tenter d'écrire.
  bool flush() {
    if (!ok) return false;
    if (used == 0) return true;
    const size_t expected = used;
    const size_t w = f->write(reinterpret_cast<const uint8_t*>(buf), expected);
    used = 0;
    if (w != expected) ok = false;
    return ok;
  }
};

size_t historyFileSink(void* ctx, const char* p, size_t n) {
  HistoryFileWriteSink* sink = static_cast<HistoryFileWriteSink*>(ctx);
  if (!sink->ok) return 0;  // échec déjà survenu : on cesse d'accepter des octets

  if ((++sink->callCount % 64) == 0) {
    const uint32_t maxAlloc8 = heapLargestFreeBlock8Bit();
    if (maxAlloc8 < sink->minMaxAlloc8DuringWrite) sink->minMaxAlloc8DuringWrite = maxAlloc8;
  }

  size_t total = 0;
  while (n > 0) {
    const size_t space = sizeof(sink->buf) - sink->used;
    const size_t chunk = (n < space) ? n : space;
    memcpy(sink->buf + sink->used, p, chunk);
    sink->used += chunk;
    p += chunk;
    n -= chunk;
    total += chunk;
    if (sink->used == sizeof(sink->buf) && !sink->flush()) {
      break;  // écriture partielle détectée pendant l'accumulation : on s'arrête ici
    }
  }
  return total;
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

  // feature-131 (C2) : réconciliation du .tmp d'écriture atomique — APRÈS
  // setPersistenceFs() (le journal doit déjà pouvoir persister le WARNING
  // ci-dessous) et AVANT tout chargement.
  reconcileHistoryTmp();

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
  // bug-098 : les deux voies dans le masque — elles ne se confondent plus.
  point.doseMask = (uint8_t)((PumpController.isPhDosing()  ? kHistDosePh  : 0) |
                             (PumpController.isOrpDosing() ? kHistDoseOrp : 0));
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

void HistoryManager::reconcileHistoryTmp() {
  if (!historyEnabled) return;
  // Effacement INCONDITIONNEL de l'orphelin — `remove()` SEUL, ni `open()` ni
  // `exists()` (0 site fopen, cf. history.h pour la justification complète de
  // la différence avec Logger::reconcileLogTmp()/ADR-0076). `remove()` d'un
  // fichier absent échoue silencieusement côté LittleFS : appelable sans
  // condition, à chaque démarrage, qu'un `.tmp` existe ou non.
  const bool hadOrphan = historyStore->remove(historyTmpFilePath);
  if (hadOrphan) {
    // WARNING SANS THROTTLE (≤ 1/démarrage par construction) : c'est la
    // preuve DATÉE d'une coupure d'alimentation pendant une sauvegarde
    // précédente. JAMAIS error()/critical() : cet appel a lieu avant tout
    // chargement, un flush synchrone rouvrirait le journal ici même.
    systemLogger.warning("Fichier temporaire d'historique orphelin efface au demarrage — "
                         "coupure pendant une sauvegarde precedente, l'historique courant "
                         "(/history.json) est conserve tel quel");
  }
}

size_t HistoryManager::emitPersistedJson(HistoryByteSink sink, void* ctx) {
  // `_mutex` DÉJÀ TENU par l'appelant (saveToFile(), lui-même appelé sous
  // mutex depuis consolidateData()/importData()/appendData()) — aucun verrou
  // n'est pris ici.
  size_t total = 0;
  total += sink(ctx, kHistoryPersistedPrefix, sizeof(kHistoryPersistedPrefix) - 1);

  bool first = true;
  size_t pointIndex = 0;
  for (const auto& point : memoryBuffer) {
    char buf[kHistoryPersistPointMaxBytes];
    const size_t n = formatPersistedHistoryPoint(
        buf, sizeof(buf), first, point.timestamp, point.ph, point.orp,
        point.temperature, point.phFiltered, point.orpFiltered,
        point.filtrationActive, point.doseMask,
        static_cast<uint8_t>(point.granularity));
    // n == 0 : le point ne tient pas dans kHistoryPersistPointMaxBytes — SAUTÉ,
    // identiquement dans la passe de mesure et la passe d'écriture (même code,
    // même buffer, même seuil) : jamais de JSON partiel, toujours cohérent.
    if (n > 0) {
      total += sink(ctx, buf, n);
      first = false;
    }

    if ((++pointIndex % 64) == 0) esp_task_wdt_reset();  // patron history.cpp:164
  }

  total += sink(ctx, kHistoryPersistedSuffix, sizeof(kHistoryPersistedSuffix) - 1);
  return total;
}

void HistoryManager::saveToFile() {
  if (!historyEnabled) return;

  // bug-100 / ADR-0070 (§d) — ADMISSION MÉMOIRE UNIQUE EN TÊTE, classe A
  // (8 192). feature-131 : cette fonction FUSIONNE les deux admissions
  // historiques (avant/après measureJson) en UNE SEULE.
  //
  // ⚠️ CE QUE DEVIENT « LE SITE EXACT DU COREDUMP DU 05/09 » : l'écart entre
  // les deux anciennes admissions existait pour survivre au pic du
  // `JsonDocument` + ses `String` de sérialisation (« des dizaines de
  // kilooctets », commentaire d'origine) construit ENTRE elles. Revue
  // code-reviewer (feature-131) : ce pic était fait de jusqu'à 1 695 mallocs
  // transitoires (339 points × 5 champs flottants) de 44 o CHACUN — et non
  // 314 o : `point.ph/orp/temperature/phFiltered/orpFiltered` sont des
  // `float` (history.h), donc `String(point.ph, 2)` résolvait vers la
  // surcharge à correspondance EXACTE `String(float, n)`
  // (`malloc(decimalPlaces + 42)`, WString.cpp), jamais `String(double, n)`
  // (`malloc(decimalPlaces + 312)`). Un chiffre faux ici finirait recopié
  // dans un ADR (ADR-0073) — d'où la précision.
  //
  // Ce pic n'existe plus : la mesure (passe 1, puits compteur) et l'écriture
  // (passe 2, puits fichier) n'allouent qu'un point à la fois
  // (kHistoryPersistPointMaxBytes = 128 o, sur PILE) plus un tampon
  // d'accumulation de kHistoryWriteChunkBytes (512 o, sur PILE lui aussi).
  // AUCUNE ALLOCATION DE TAS PROPORTIONNELLE AU VOLUME DE POINTS — nuance
  // volontaire : le `rename()` final fait DEUX `malloc()` internes
  // (`VFSImpl::rename()`, vfs_api.cpp) et les `systemLogger.debug/warning`
  // de cette fonction allouent chacun une `String` — mais ce sont des coûts
  // CONSTANTS, indépendants du nombre de points, à des lieues du pic
  // supprimé. Le site du coredump n'a donc plus de raison d'exister : une
  // seule mesure, prise avant tout effet de bord, suffit.
  //
  // La garde va là où la fonction peut encore NE RIEN FAIRE : classe A, et non
  // B, car ce chemin est atteignable depuis POST /history/import
  // (→ importData()/appendData() → saveToFile()) sur la tâche async_tcp, donc
  // avec des pbuf lwip en vol.
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

  // Passe 1 — MESURE, aucun effet de bord FS : le puits compteur additionne la
  // taille des tronçons émis par le MÊME code de formatage que la passe 2.
  size_t jsonSize = 0;
  emitPersistedJson(historyCounterSink, &jsonSize);

  // Garde d'espace (A2, feature-131 : plus de crédit de l'ancien fichier — cf.
  // historySaveAdmit()/kHistorySaveSpaceMarginBytes, constants.h). Refuser
  // AVANT de toucher au FS — mieux vaut un historique périmé de 5 min qu'un
  // fichier tronqué. Retentative naturelle au prochain cycle SAVE_INTERVAL.
  fs::LittleFSFS* lfs = (historyStore == &historyFs) ? &historyFs : &LittleFS;
  const size_t freeBytes = lfs->totalBytes() - lfs->usedBytes();
  if (!historySaveAdmit(freeBytes, jsonSize, kHistorySaveSpaceMarginBytes)) {
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

  // Écriture ATOMIQUE (feature-131, décision utilisateur 2026-09-19) : .tmp
  // PUIS rename, patron du journal (ADR-0076). L'ancien /history.json N'EST
  // PAS supprimé avant l'écriture — il coexiste avec le .tmp jusqu'au rename
  // final, qui est la SEULE opération qui le remplace. Une coupure pendant
  // l'écriture laisse donc soit l'ancien intact (rename jamais atteint), soit
  // un .tmp orphelin sans effet sur /history.json — jamais un fichier
  // tronqué en place. reconcileHistoryTmp() efface l'orphelin au démarrage
  // suivant (cf. history.h).
  File f = openIfHeapAllows(historyStore, historyTmpFilePath, "w", kFsGuardAlreadyAdmitted);
  if (!f) {
    static unsigned long sWarnOpenMs = 0;
    unsigned long nowMs = millis();
    if (sWarnOpenMs == 0 || nowMs - sWarnOpenMs >= kHistorySaveWarnThrottleMs) {
      systemLogger.warning("Impossible d'ouvrir /history.json.tmp en écriture — save sauté");
      sWarnOpenMs = nowMs;
    }
    recordHistorySaveFailureNvs(_lastOkEpoch);
    return;
  }

  // Passe 2 — ÉCRITURE réelle, MÊME code de formatage que la passe 1 (même
  // boucle, même drapeau `first`, même prédicat RAW) : divergence impossible
  // par construction.
  HistoryFileWriteSink sink{&f};
  const size_t written = emitPersistedJson(historyFileSink, &sink);
  const bool flushOk = sink.flush();  // vide le reliquat du tampon d'accumulation
  f.close();

  // F1 : minimum de maxBloc8 observé PENDANT la passe 2 — la mesure « pendant,
  // pas autour » exigée par la spec. DEBUG : gratuit en production, coûte une
  // ligne uniquement quand le niveau de log l'affiche. Conservé (et non
  // retiré) : c'est le seul moyen de vérifier a posteriori, sur le terrain,
  // que le pic transitoire visé par ce lot est bien resté au plancher, sans
  // ajouter de sonde permanente au chemin critique.
  if (sink.minMaxAlloc8DuringWrite != UINT32_MAX) {
    // Revue code-reviewer (feature-131) : `char[]` + surcharge `const char*`,
    // pas de `String` — les arguments d'un `+=` de `String` seraient
    // construits même si le niveau DEBUG est filtré, ce qui contredirait
    // l'objet même de ce lot sur un chemin qui se répète toutes les 5 min.
    char ligne[64];
    snprintf(ligne, sizeof(ligne),
             "Sauvegarde historique : maxBloc8 min pendant l'écriture = %u o",
             (unsigned)sink.minMaxAlloc8DuringWrite);
    systemLogger.debug(ligne);
  }

  // Détection d'écriture partielle (B6) : la somme des retours du puits
  // fichier doit être EXACTEMENT jsonSize (mesuré passe 1), ET le tampon
  // d'accumulation doit s'être vidé sans erreur. Échec → le .tmp est
  // supprimé, et — c'est le gain net de l'atomicité — l'ANCIEN /history.json
  // RESTE EN PLACE, intact : aucune fenêtre où le fichier utilisé par le
  // reste du firmware est tronqué.
  if (!flushOk || written != jsonSize) {
    historyStore->remove(historyTmpFilePath);
    static unsigned long sWarnPartialMs = 0;
    unsigned long nowMs = millis();
    if (sWarnPartialMs == 0 || nowMs - sWarnPartialMs >= kHistorySaveWarnThrottleMs) {
      systemLogger.warning("Écriture partielle de l'historique (" + String(written) + "/" +
                           String(jsonSize) + " octets) — fichier temporaire supprimé, "
                           "l'historique precedent est conserve, nouvelle tentative dans 5 min");
      sWarnPartialMs = nowMs;
    }
    recordHistorySaveFailureNvs(_lastOkEpoch);
    return;
  }

  // Bascule atomique : SEULE opération qui remplace /history.json. Échec du
  // rename → PAS de repli remove(dest)+rename : l'atomicité vient du rename
  // lui-même, un repli manuel rouvrirait exactement la fenêtre qu'on ferme
  // (ancien supprimé, nouveau pas encore en place). Le .tmp reste alors sur
  // le FS ; le prochain cycle SAVE_INTERVAL le réécrira (open("w") tronque),
  // et reconcileHistoryTmp() l'effacerait de toute façon à un redémarrage.
  if (!historyStore->rename(historyTmpFilePath, historyFilePath)) {
    static unsigned long sWarnRenameMs = 0;
    unsigned long nowMs = millis();
    if (sWarnRenameMs == 0 || nowMs - sWarnRenameMs >= kHistorySaveWarnThrottleMs) {
      systemLogger.warning("Échec du renommage de l'historique (.tmp → .json) — "
                           "l'historique precedent est conserve, nouvelle tentative dans 5 min");
      sWarnRenameMs = nowMs;
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

  systemLogger.debug("Historique sauvegardé (" + String(memoryBuffer.size()) + " points)");
}

void HistoryManager::loadFromFile() {
  if (!historyEnabled) return;

  // bug-100 / ADR-0070 — CLASSE B (5 120), tripwire de boot.
  // feature-131 (E2) : seuil INCHANGÉ malgré la réécriture en flux. Ce seuil
  // encode l'ABSENCE DE CONTENTION TCP au boot (ordre de setup(), cf.
  // history.h/acces-fichiers.md), pas la taille du pic à traverser — et le pic
  // résiduel de loadFromFile() (le vecteur `memoryBuffer`, kHistoryLoadReservePoints
  // × sizeof(DataPoint) = 9 492 o EN UNE FOIS, cf. constants.h) reste supérieur
  // au pic qu'un point de flux, lui, a fait disparaître. Rien ici ne justifie
  // de toucher au 7ᵉ barreau de l'échelle ADR-0058.
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

  // feature-131 (v2.53.0) : chargement EN FLUX — idiome ArduinoJson 7 « flux de
  // documents » (deserializeJson successifs sur le MÊME File, chacun consomme
  // exactement UNE valeur JSON et laisse le curseur juste après). Le
  // `deserializeJson(doc, f)` GLOBAL sur tout le fichier a disparu : on ne
  // matérialise plus jamais qu'UN point à la fois.
  memoryBuffer.clear();
  legacyHistoryPending = false;
  legacyMaxTimestamp = 0;
  // D2 : réservation UNIQUE — sans elle, l'escalade de capacité du vecteur
  // (cf. kHistoryLoadReservePoints, constants.h) redemande jusqu'à 14 336 o
  // contigus au lieu de 9 492 o en une fois.
  memoryBuffer.reserve(kHistoryLoadReservePoints);

  // Préfixe fixe `{"data":[` consommé OCTET PAR OCTET — c'est le même contrat
  // que kHistoryPersistedPrefix (history_persist_format.h), écrit par
  // emitPersistedJson(). Un écart ici signifie un fichier étranger ou
  // corrompu dès l'en-tête : rien à récupérer (le tableau lui-même n'a pas
  // commencé), on s'arrête SANS toucher au fichier.
  //
  // ⚠️ LIMITE ASSUMÉE (revue code-reviewer, feature-131) : cette comparaison
  // est un match EXACT, octet à octet — aucune tolérance aux espaces ni à un
  // réordonnancement des clés (`{"data": [`, ou un hypothétique
  // `{"other":1,"data":[`, seraient rejetés EN BLOC, 0 point chargé, simple
  // WARNING). Le « meilleur effort » de D4 ne couvre PAS l'en-tête, seulement
  // le contenu du tableau. Ce n'est pas une régression : le firmware est
  // l'UNIQUE écrivain de /history.json (vérifié par grep — aucun autre
  // producteur), et emitPersistedJson() écrit toujours ce préfixe à
  // l'identique, sans espace ni réordonnancement possible.
  bool prefixOk = true;
  for (size_t i = 0; i < sizeof(kHistoryPersistedPrefix) - 1; ++i) {
    if (f.read() != static_cast<int>(static_cast<unsigned char>(kHistoryPersistedPrefix[i]))) {
      prefixOk = false;
      break;
    }
  }
  if (!prefixOk) {
    f.close();
    // WARNING et non ERROR (D4) : un ERROR flusherait le journal en synchrone
    // → fopen() → l'abort() que ce lot évite précisément.
    systemLogger.warning("Historique illisible (en-tête invalide) — aucun point chargé");
    return;
  }

  // D4 : MEILLEUR EFFORT, BRUYANT — on garde les points déjà complets, on
  // s'arrête à la première erreur (JSON malformé, fichier tronqué), et on
  // WARN (jamais ERROR) au lieu d'abandonner tout ce qui a déjà été lu.
  // Un point n'entre dans memoryBuffer QUE si deserializeJson() l'a validé
  // COMPLET (aucune erreur de syntaxe) — jamais un objet partiel.
  bool truncated = false;
  size_t parsedCount = 0;
  for (;;) {
    const int next = f.peek();
    if (next == ']') { f.read(); break; }  // fin normale du tableau
    if (next < 0) { truncated = true; break; }  // EOF avant le ']' attendu

    // Revue code-reviewer (feature-131) : PLAFOND DUR — sans lui, un fichier
    // corrompu ou porteur de doublons RAW ferait croître memoryBuffer sans
    // limite au boot (régression du passage en flux : l'ancien code était
    // borné implicitement par l'échec d'allocation du JsonDocument global).
    // « Meilleur effort bruyant » identique aux autres troncatures : les
    // points déjà lus sont gardés, WARNING (jamais ERROR).
    if (parsedCount >= kHistoryLoadMaxPoints) {
      truncated = true;
      break;
    }

    JsonDocument doc;
    if (deserializeJson(doc, f)) {
      truncated = true;  // objet syntaxiquement incomplet/invalide
      break;
    }
    JsonObject point = doc.as<JsonObject>();

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
    // bug-098 — DISCRIMINANT DE FORMAT, sans cle de version ni numero de
    // schema : `is<bool>()` separe EXACTEMENT les deux ecritures possibles.
    //   `true`  (<= 2.46) → is<bool>()  : dosage, voie INDETERMINEE ;
    //   `3`     (>= 2.47) → is<int>()   : masque de voies ;
    //   absent            → ni l'un ni l'autre : aucun dosage.
    // LIMITE ASSUMEE : un `"d"` en FLOTTANT (`3.0`) ou en entier 64 bits hors
    // plage `int` ne satisfait NI is<bool>() NI is<int>() — il retombe donc sur
    // 0, « aucun dosage », c'est-a-dire le mensonge favorable. Ni le firmware ni
    // l'export ne produisent ces formes : il faut un fichier edite a la main ou
    // un export etranger. Non traite pour ne pas elargir le discriminant a des
    // types qui rendraient la separation des deux formats moins nette.
    // Le masque conserve kHistDoseUnknownChannel : sans lui, le premier
    // saveToFile/loadFromFile qui suit l'OTA retransformerait « voie
    // indeterminee » en « aucun dosage » — le mensonge favorable reviendrait
    // par la porte de derriere, cinq minutes apres la mise a jour.
    JsonVariant dosingVar = point["d"];
    if (dosingVar.is<bool>()) {
      dp.doseMask = dosingVar.as<bool>() ? kHistDoseUnknownChannel : 0;
    } else if (dosingVar.is<int>()) {
      dp.doseMask = (uint8_t)(dosingVar.as<unsigned int>() &
                              (kHistDosePh | kHistDoseOrp | kHistDoseUnknownChannel));
    } else {
      dp.doseMask = 0;
    }
    dp.granularity = static_cast<Granularity>(point["g"] | 0);
    memoryBuffer.push_back(dp);

    if ((++parsedCount % 64) == 0) esp_task_wdt_reset();

    const int sep = f.peek();
    if (sep == ',') { f.read(); continue; }
    if (sep == ']') { f.read(); break; }
    truncated = true;  // séparateur inattendu (virgule ou crochet manquant)
    break;
  }

  // Accolade fermante du document (`}` de `{"data":[...]}`). Un écart ici ne
  // remet PAS en cause les points déjà chargés (idiome D4) — seule la trace
  // change.
  if (!truncated && f.read() != '}') truncated = true;

  f.close();

  if (truncated) {
    systemLogger.warning("Historique tronqué ou corrompu — " + String((unsigned)parsedCount) +
                         " point(s) récupéré(s), le reste est perdu");
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

// bug-101 — BORNAGE DU MODE « AJOUTER ».
// Voir history.h pour la raison d'un troisième site de bornage (les deux
// existants, recordDataPoint pour RAW et le pas 4 de consolidateData pour les
// agrégats, restent INCHANGÉS : ce lot ne les touche pas).
// Pré-requis : mutex pris par l'appelant, buffer TRIÉ par timestamp — l'ordre du
// vecteur EST l'ordre chronologique, donc « les premiers rencontrés » = « les
// plus anciens ».
void HistoryManager::trimToCaps() {
  size_t raw = 0, hourly = 0, daily = 0;
  for (const auto& p : memoryBuffer) {
    if (p.granularity == RAW) raw++;
    else if (p.granularity == HOURLY) hourly++;
    else if (p.granularity == DAILY) daily++;
  }

  const HistoryImportTrimPlan plan = historyImportTrimPlan(raw, hourly, daily);
  if (plan.empty()) return;

  // UN SEUL passage erase-remove pour les trois granularités (O(n) au total) :
  // std::remove_if applique le prédicat exactement une fois par élément, dans
  // l'ordre — c'est ce que suppose déjà le comptage du pas 4 de consolidateData.
  size_t doneRaw = 0, doneHourly = 0, doneDaily = 0;
  memoryBuffer.erase(
    std::remove_if(memoryBuffer.begin(), memoryBuffer.end(),
      [&plan, &doneRaw, &doneHourly, &doneDaily](const DataPoint& p) {
        if (p.granularity == RAW && doneRaw < plan.rawToRemove) {
          doneRaw++;
          return true;
        }
        if (p.granularity == HOURLY && doneHourly < plan.hourlyToRemove) {
          doneHourly++;
          return true;
        }
        if (p.granularity == DAILY && doneDaily < plan.dailyToRemove) {
          doneDaily++;
          return true;
        }
        return false;
      }),
    memoryBuffer.end()
  );
}

bool HistoryManager::appendData(const std::vector<DataPoint>& pts,
                                size_t* outTotal, size_t* outDropped) {
  // Sorties initialisées AVANT tout retour d'échec : l'appelant sérialise
  // `total`/`dropped` quel que soit le verdict, il ne doit jamais lire une
  // valeur non écrite.
  if (outTotal != nullptr) *outTotal = 0;
  if (outDropped != nullptr) *outDropped = 0;

  if (!historyEnabled) return false;
  if (pts.empty()) return false;
  if (_mutex == nullptr) return false;

  // UNE seule prise, comme importData : la séquence fusion → tri → bornage →
  // sauvegarde est atomique. Sans cela, le recordDataPoint() de la boucle
  // pourrait s'intercaler entre la fusion et la sauvegarde et rendre le fichier
  // incohérent avec ce qu'on vient de répondre au client.
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(kHistoryMutexTimeoutMs)) != pdTRUE) {
    static unsigned long sWarnAppendMs = 0;
    warnHistoryMutexTimeout(sWarnAppendMs, "appendData");
    return false;
  }

  // FUSION EN PLACE — recherche linéaire dans memoryBuffer, ZÉRO tampon
  // supplémentaire. Coût : |pts| × |buffer|, soit au pire ~45 × 339 ≈ 15 000
  // comparaisons de deux scalaires (le plafond de corps borne |pts|). C'est
  // délibérément le compromis inverse d'une std::map ou d'un vecteur trié
  // auxiliaire : sur ce chemin, la MÉMOIRE est la ressource rare, pas le CPU.
  size_t overwritten = 0;
  size_t created = 0;
  for (const auto& in : pts) {
    bool found = false;
    // TOUTES les occurrences du créneau sont écrasées, pas seulement la
    // première : un buffer chargé depuis un fichier écrit par une version
    // ≤ 2.18.0 peut porter des doublons d'agrégats. N'en écraser qu'un
    // laisserait l'autre périmé, que dedupAggregatedPoints() moyennerait
    // ensuite avec le neuf — l'idempotence serait perdue. Écraser les deux la
    // conserve (historyMergeMeanFinite(x, x) == x).
    for (auto& live : memoryBuffer) {
      if (historyImportSameSlot((uint32_t)in.timestamp, (uint8_t)in.granularity,
                                (uint32_t)live.timestamp, (uint8_t)live.granularity)) {
        live = in;  // ÉCRASEMENT, cf. history_import_logic.h
        found = true;
      }
    }
    if (found) {
      overwritten++;
    } else {
      // push_back APRÈS la boucle interne : aucune invalidation d'itérateur.
      // Le point devient visible pour les points SUIVANTS du même lot — voulu :
      // un lot qui porte deux fois le même créneau reste idempotent.
      memoryBuffer.push_back(in);
      created++;
    }
  }

  std::sort(memoryBuffer.begin(), memoryBuffer.end(),
    [](const DataPoint& a, const DataPoint& b) {
      return a.timestamp < b.timestamp;
    });

  // dedupAggregatedPoints() N'EST PAS APPELÉ ICI, et c'est un choix :
  // il MOYENNE (historyMergeMeanFinite) les agrégats du même créneau, ce qui
  // contredirait la règle d'écrasement qu'on vient d'appliquer — le point
  // restauré serait re-moyenné avec le vivant qu'il devait remplacer. La fusion
  // en place ci-dessus garantit déjà l'unicité de (timestamp, granularité) pour
  // tout ce que ce lot a touché ; le reste est du ressort de consolidateData().
  const size_t beforeTrim = memoryBuffer.size();
  trimToCaps();
  const size_t dropped = beforeTrim - memoryBuffer.size();

  // OBLIGATOIRE, repris à l'identique d'importData : sans ces deux lignes, un
  // import partiel laisserait la migration d'historique hérité se réveiller au
  // prochain consolidateData() et réécrire par-dessus ce qu'on vient d'ajouter.
  legacyHistoryPending = false;
  legacyMaxTimestamp = 0;
  lastSave = millis();
  lastRecord = millis();
  saveToFile();
  const size_t count = memoryBuffer.size();
  xSemaphoreGive(_mutex);

  if (outTotal != nullptr) *outTotal = count;
  if (outDropped != nullptr) *outDropped = dropped;

  // Journalisé APRÈS la libération du mutex (idiome d'importData) : un WARNING
  // ou pire déclencherait un flush synchrone du journal, donc un fopen, sous le
  // mutex historique.
  systemLogger.info("Historique : lot ajouté (" + String((unsigned)created) +
                    " nouveaux, " + String((unsigned)overwritten) + " écrasés, " +
                    String((unsigned)dropped) + " évincés, total " +
                    String((unsigned)count) + ")");
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
    // bug-098 : masque nul = aucun dosage dans le creneau (l'union ci-dessous
    // le remplit voie par voie).
    avgPoint.doseMask = 0;
    avgPoint.granularity = HOURLY;

    int validCount = 0;
    int filtrationCount = 0;
    uint8_t doseMaskUnion = 0;

    for (const auto& p : group.second) {
      if (!isnan(p.ph)) {
        avgPoint.ph += p.ph;
        validCount++;
      }
      if (!isnan(p.orp)) avgPoint.orp += p.orp;
      if (!isnan(p.temperature)) avgPoint.temperature += p.temperature;
      if (p.filtrationActive) filtrationCount++;
      // bug-098 : « au moins un » BIT A BIT — kHistDoseUnknownChannel d'un
      // point herite se propage dans l'agregat au lieu de disparaitre.
      doseMaskUnion = (uint8_t)(doseMaskUnion | p.doseMask);
    }

    if (validCount > 0) {
      avgPoint.ph = finalizeMean(avgPoint.ph, validCount);
      avgPoint.orp = finalizeMean(avgPoint.orp, validCount);
      avgPoint.temperature = finalizeMean(avgPoint.temperature, validCount);
      avgPoint.filtrationActive = isMajority(filtrationCount, (int)group.second.size());
      avgPoint.doseMask = doseMaskUnion;

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
    // bug-098 : masque nul = aucun dosage dans le creneau (l'union ci-dessous
    // le remplit voie par voie).
    avgPoint.doseMask = 0;
    avgPoint.granularity = DAILY;

    int validCount = 0;
    int filtrationCount = 0;
    uint8_t doseMaskUnion = 0;

    for (const auto& p : group.second) {
      if (!isnan(p.ph)) {
        avgPoint.ph += p.ph;
        validCount++;
      }
      if (!isnan(p.orp)) avgPoint.orp += p.orp;
      if (!isnan(p.temperature)) avgPoint.temperature += p.temperature;
      if (p.filtrationActive) filtrationCount++;
      // bug-098 : « au moins un » BIT A BIT — kHistDoseUnknownChannel d'un
      // point herite se propage dans l'agregat au lieu de disparaitre.
      doseMaskUnion = (uint8_t)(doseMaskUnion | p.doseMask);
    }

    if (validCount > 0) {
      avgPoint.ph = finalizeMean(avgPoint.ph, validCount);
      avgPoint.orp = finalizeMean(avgPoint.orp, validCount);
      avgPoint.temperature = finalizeMean(avgPoint.temperature, validCount);
      avgPoint.filtrationActive = isMajority(filtrationCount, (int)group.second.size());
      avgPoint.doseMask = doseMaskUnion;

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
      // bug-098 : union des masques — le OU bit a bit EST le « au moins un »
      // d'avant, applique voie par voie (et non toutes voies confondues).
      dup->doseMask = (uint8_t)(dup->doseMask | p.doseMask);
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
  // feature-131 (C3) : le .tmp de l'écriture atomique aussi — sinon un
  // effacement suivi d'un redémarrage avant la prochaine sauvegarde laisserait
  // reconcileHistoryTmp() effacer un .tmp qui n'a plus lieu d'être (harmless,
  // mais autant ne pas laisser de déchet).
  historyStore->remove(historyTmpFilePath);
  systemLogger.warning("Historique effacé");
  return true;
}
