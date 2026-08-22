#include <Arduino.h>
#include <new>       // std::set_new_handler
#include <atomic>
#include <stdio.h>   // snprintf (ligne de WARN construite sur la PILE, aucune String)
#include <string.h>  // memcpy (copie de la zone RTC vers un tampon d'octets)
#include <esp_heap_caps.h>  // heap_caps_register_failed_alloc_callback, heap_caps_check_integrity_all
#include <esp_task_wdt.h>   // esp_task_wdt_reset (après la section critique du contrôle d'intégrité)
#include <esp_timer.h>      // esp_timer_get_time (mesure de durée du contrôle d'intégrité)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>  // xTaskGetTickCount (horloge SANS verrou pour le chemin d'échec)
#include "oom_guard.h"
#include "oom_guard_logic.h"
#include "constants.h"
#include "heap_metrics.h"  // heapFree8Bit / heapLargestFreeBlock8Bit (JAMAIS depuis les chemins d'échec)
#include "logger.h"

// =============================================================================
// État partagé handler/callback ↔ loop (bug-oom-accept-http-durcissement)
// =============================================================================
// Le new_handler peut être invoqué depuis N'IMPORTE QUELLE tâche (async_tcp,
// mqttTask, loopTask...) et le callback heap_caps depuis l'INTÉRIEUR de
// l'allocateur : tout l'état partagé est atomique, aucun mutex, aucune alloc,
// aucun systemLogger dans ces chemins (réentrance allocateur interdite).
// seq_cst par défaut : coût négligeable (chemins froids), raisonnement simple.

static std::atomic<void*>    sOomReserve{nullptr};            // Réserve d'urgence (nullptr = consommée/absente)
static std::atomic<uint32_t> sOomReserveBytes{0};             // Taille du palier armé (0 = aucune réserve)
static std::atomic<uint8_t>  sOomState{kOomStateDepleted};    // Machine à états (oom_guard_logic.h)
static std::atomic<uint32_t> sOomReleaseCount{0};             // Épisodes de libération de la réserve
static std::atomic<uint32_t> sOomFailedAllocCount{0};         // Échecs heap_caps cumulés (diagnostic)
static std::atomic<uint32_t> sOomFailedAllocLastSize{0};      // Taille demandée du dernier échec

// -----------------------------------------------------------------------------
// Miroir d'inscription du new_handler (v2.20.2, bloc A)
// -----------------------------------------------------------------------------
// La libstdc++ n'offre aucun moyen de LIRE le handler courant sans l'écraser
// (std::get_new_handler existe en C++11 mais la ré-inscription doit rester une
// TRANSITION observable pour ne journaliser qu'une fois). On maintient donc un
// miroir atomique : false dès que oomNewHandler se désinscrit, true juste après
// chaque std::set_new_handler(oomNewHandler).
static std::atomic<bool> sHandlerRegistered{false};

// -----------------------------------------------------------------------------
// Attribution du fragmenteur (v2.20.2, bloc D)
// -----------------------------------------------------------------------------
// L'IDF passe à heap_caps_register_failed_alloc_callback un `function_name` qui
// est un LITTÉRAL de .rodata (immortel, jamais libéré) : on stocke le POINTEUR,
// pas une copie. Décision D-2 de l'architecte : un strncpy coûterait 24 o de
// copie sous le verrou du tas et autoriserait une lecture déchirée depuis la
// loop, pour aucun gain. Drainé et journalisé UNIQUEMENT par oomGuardLoopTick().
static std::atomic<const char*> sOomFailedAllocLastFn{nullptr};  // Appelant du dernier échec (.rodata)
static std::atomic<uint32_t>    sOomFailedAllocLastCaps{0};      // Masque heap_caps du dernier échec

// Drapeau « allocation auto-infligée en cours » (v2.20.2, bloc E1) : posé autour
// du SEUL malloc(target) du réarmement. ~100 % des 1025 échecs comptés le
// 15/08 étaient ce malloc-là. Un store atomique est la seule écriture autorisée
// dans le hook, et c'est la loop qui pose/retire le drapeau (jamais le hook).
// LIMITE ASSUMÉE : le drapeau est global, pas par tâche — un échec RÉEL survenu
// sur l'autre cœur pendant la fenêtre (quelques microsecondes toutes les 10 s)
// ne serait pas comptabilisé. Compromis très favorable face au bruit supprimé.
static std::atomic<bool> sSelfAllocInFlight{false};

// =============================================================================
// Post-mortem d'allocation en RAM RTC noinit (bug-parachute-oom-jamais-rearme)
// =============================================================================
// TROISIÈME zone RTC noinit du projet, magic PROPRE (0xF00DFA12 depuis v2.19.0,
// cf. kOomPostMortemMagic) — distinct de l'anneau heap (0xB10C4EAA, heap_ring.cpp)
// et de l'auto-guérison (0xA5C0FFEE, main.cpp). Budget RAM RTC noinit total :
// auto-heal 9 o + anneau 1032 o + post-mortem 32 o = 1073 o sur 8192 disponibles.
//
// UNE seule zone, UN seul dump, UN seul budget : le drapeau « réserve consommée »
// demandé par la spec EST l'octet `flags` de cette zone (pas une variable RTC
// séparée).
//
// La zone est vue comme 8 mots de 32 bits ALIGNÉS : les chemins d'échec
// (oomFailedAllocHook, oomNewHandler) n'y font que des STORES BRUTS ALIGNÉS.
// INTERDICTION ABSOLUE dans ces deux chemins (le verrou du composant heap est
// déjà détenu par l'allocateur quand le callback est appelé → tout appel
// réentrant est un deadlock) : heap_caps_*, ESP.get*Heap(), tout verrou, toute
// allocation, tout log. Correspondance mot ↔ champs, IDENTIQUE au format pur
// (little-endian, cf. oom_guard_logic.h) :
//   w0 = magic | w1 = failCount | w2 = firstSize | w3 = lastSize | w4 = lastCaps
//   w5 = firstUptimeS | (lastUptimeS << 16)
//   w6 = (ringFree >> 8) | ((ringMaxB >> 6) << 16)
//   w7 = ringUptimeS | (flags << 16) | (releaseCount << 24)
//
// PRÉCISION IMPORTANTE (revue v2.18.8) : les mots w1 (failCount) et w5
// (uptimes) sont modifiés en LECTURE-MODIFICATION-ÉCRITURE, pas en simple store.
// Aucun verrou n'étant autorisé ici, deux échecs simultanés sur les DEUX cœurs
// peuvent perdre un incrément. Assumé : le post-mortem est une trace INDICATIVE
// destinée à orienter un diagnostic, jamais un compteur exact.
RTC_NOINIT_ATTR static uint32_t sOomPmWords[kOomPostMortemBytes / 4];

// Instantané de la zone du boot PRÉCÉDENT, pris par initOomGuard() AVANT
// d'enregistrer les chemins d'échec (revue v2.18.8) : sans lui, un échec
// d'allocation survenant pendant la suite du setup viendrait polluer la zone
// avant qu'elle ait été lue. RAM ordinaire (.bss) : sa durée de vie utile
// s'arrête au dump, quelques centaines de lignes plus loin dans setup().
// LIMITE ASSUMÉE : l'instantané vit en .bss et ne survit donc PAS à un reset.
// Un crash survenant entre initOomGuard() et le dump fait perdre le post-mortem
// du boot n-2 (celui du boot n-1 est intact). Compromis accepté : la fenêtre est
// de quelques centaines de millisecondes de setup.
static uint8_t sOomPmSnapshot[kOomPostMortemBytes];

// Miroir RAM du dernier échantillon de l'anneau heap (valeurs DÉJÀ mesurées à la
// cadence de 2 s) : la seule source « free / maxBloc » du chemin d'échec, qui ne
// mesure donc jamais le tas lui-même. Pré-encodé au format du mot w6 pour que le
// chemin d'échec n'ait qu'un store à faire.
static std::atomic<uint32_t> sHeapSampleW6{0};        // (free >> 8) | ((maxB >> 6) << 16)
static std::atomic<uint32_t> sHeapSampleUptimeS{0};   // uptime (s) de cet échantillon

// Miroir RAM des drapeaux (octet 30). Initialisé avec « intégrité jamais
// exécutée » ; les bits sont posés par fetch_or (lock-free) puis recopiés dans la
// zone RTC par les chemins d'écriture.
static std::atomic<uint8_t> sOomPmFlags{kOomPmFlagIntegrityNeverRun};

static_assert(sizeof(sOomPmWords) == kOomPostMortemBytes,
              "La zone post-mortem RTC doit faire exactement 32 octets");

// Écrit/actualise la zone post-mortem. APPELÉE DEPUIS LES CHEMINS D'ÉCHEC :
// stores bruts alignés uniquement, aucune fonction de l'allocateur, aucun
// verrou, aucun log. `countFailure` = comptabiliser un échec d'allocation
// (`size`, `caps`, `uptimeS`) ; sinon seuls les champs de contexte (drapeaux,
// anneau, épisodes de réserve) sont rafraîchis.
static inline void oomPostMortemStore(bool countFailure, uint32_t size, uint32_t caps,
                                     uint32_t uptimeS) {
  if (sOomPmWords[0] != kOomPostMortemMagic) {
    // Initialisation inline (power-on, ou magic invalidé par le dump du boot
    // courant). Magic écrit en DERNIER : une zone à moitié initialisée n'est
    // jamais vue comme valide si un abort survient au milieu.
    sOomPmWords[1] = 0;
    sOomPmWords[2] = 0;
    sOomPmWords[3] = 0;
    sOomPmWords[4] = 0;
    sOomPmWords[5] = 0;
    sOomPmWords[6] = 0;
    sOomPmWords[7] = 0;
    sOomPmWords[0] = kOomPostMortemMagic;
  }

  if (countFailure) {
    uint32_t n = sOomPmWords[1] + 1u;
    sOomPmWords[1] = n;
    uint16_t up16 = (uint16_t)(uptimeS & 0xFFFFu);
    if (n == 1u) {
      sOomPmWords[2] = size;                                  // firstSize
      sOomPmWords[5] = (uint32_t)up16 | ((uint32_t)up16 << 16);
    } else {
      sOomPmWords[5] = (sOomPmWords[5] & 0x0000FFFFu) | ((uint32_t)up16 << 16);
    }
    sOomPmWords[3] = size;  // lastSize
    sOomPmWords[4] = caps;  // lastCaps
  }

  // Contexte : anneau heap (miroir, jamais de mesure ici) + drapeaux + épisodes.
  uint32_t releases = sOomReleaseCount.load();
  if (releases > 255u) releases = 255u;
  sOomPmWords[6] = sHeapSampleW6.load();
  sOomPmWords[7] = (sHeapSampleUptimeS.load() & 0xFFFFu) |
                   ((uint32_t)sOomPmFlags.load() << 16) |
                   (releases << 24);
}

// new_handler (contrat libstdc++ : appelé quand malloc échoue dans operator
// new, puis le new est RE-TENTÉ). UNIQUEMENT : extraction atomique de la
// réserve + free() + état + compteur + drapeaux RTC (stores bruts). Si la
// réserve est déjà consommée → désenregistrement (le new suivant lève bad_alloc
// = comportement d'avant le parachute, coredump en filet ultime). Le log
// CRITICAL est émis en DIFFÉRÉ depuis oomGuardLoopTick() — et le drapeau RTC
// couvre le cas où un abort suit immédiatement (le différé est alors perdu :
// défaut n°2 du crash du 13/08).
static void oomNewHandler() {
  void* reserve = sOomReserve.exchange(nullptr);
  // Taille du palier consommé (course bénigne avec le réarmement : le POINTEUR
  // reste la seule autorité, cf. oomGuardLoopTick).
  uint32_t consumedBytes = sOomReserveBytes.exchange(0);
  OomHandlerAction action = oomOnAllocFailed(reserve != nullptr);
  if (action.freeReserve) {
    free(reserve);
    sOomReleaseCount.fetch_add(1);
  }
  if (action.unregisterHandler) {
    std::set_new_handler(nullptr);
    // Miroir mis à jour AU MOMENT EXACT de la désinscription : c'est ce qui
    // permet à oomGuardLoopTick() de détecter la transition et de ré-inscrire le
    // handler dès que la réserve est de nouveau armée (bloc A).
    sHandlerRegistered.store(false);
  }
  sOomState.store(action.nextState);

  // Drapeau « réserve consommée » posé IMMÉDIATEMENT en RAM RTC (spec §3).
  uint8_t bits = 0;
  if (action.freeReserve) {
    bits |= (consumedBytes >= (uint32_t)kOomReserveBytes) ? kOomPmFlagNominalUsed
                                                         : kOomPmFlagFallbackUsed;
  }
  if (action.unregisterHandler) bits |= kOomPmFlagHandlerUnregistered;
  if (bits != 0) sOomPmFlags.fetch_or(bits);
  oomPostMortemStore(false, 0, 0, 0);
}

// Callback diagnostic heap_caps (COMPLÉMENT d'attribution) : appelé depuis
// l'intérieur de l'allocateur à CHAQUE échec d'allocation (malloc C compris,
// pas seulement operator new). Mode FLAG-SEUL : pas de free, pas de log — drainé
// par la loop (WARN one-shot throttlé) — PLUS l'écriture du post-mortem RTC.
// Horloge : xTaskGetTickCount() = simple lecture de xTickCount (aucun verrou,
// aucune allocation) — millis()/esp_timer_get_time() ne sont PAS utilisés ici.
static void oomFailedAllocHook(size_t size, uint32_t caps, const char* function_name) {
  // Bloc E1 : les échecs FABRIQUÉS par le réarmement du parachute lui-même ne
  // sont ni comptés ni post-mortemés — ils saturaient le journal et faussaient
  // le verdict de famine. Simple lecture atomique, aucune allocation.
  if (!oomShouldCountFailure(sSelfAllocInFlight.load())) return;

  // Bloc D : `function_name` est un littéral .rodata de l'IDF (immortel) → on
  // publie le POINTEUR, jamais une copie (aucune écriture mémoire non triviale
  // sous le verrou du tas). Drainé par la loop, JAMAIS journalisé ici.
  sOomFailedAllocLastFn.store(function_name);
  sOomFailedAllocLastCaps.store(caps);
  sOomFailedAllocLastSize.store((uint32_t)size);
  sOomFailedAllocCount.fetch_add(1);
  uint32_t uptimeS = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
  oomPostMortemStore(true, (uint32_t)size, caps, uptimeS);
}

void initOomGuard() {
  // Réserve allouée au plus TÔT du setup : bloc contigu pris avant toute
  // fragmentation. Pas de log ici (systemLogger.begin() pas encore appelé) —
  // le setup journalise l'armement (et le palier obtenu) juste après.
  // Deux paliers (v2.18.8) : nominal, puis repli si le nominal est refusé.
  void* reserve = malloc(kOomReserveBytes);
  size_t armedBytes = kOomReserveBytes;
  if (reserve == nullptr) {
    reserve = malloc(kOomFallbackReserveBytes);
    armedBytes = kOomFallbackReserveBytes;
  }
  if (reserve != nullptr) {
    sOomReserveBytes.store((uint32_t)armedBytes);  // taille AVANT pointeur (le pointeur publie)
    sOomReserve.store(reserve);
    sOomState.store(kOomStateArmed);
  } else {
    // Improbable (boot) : la loop tentera l'armement via le chemin réarmement.
    sOomReserveBytes.store(0);
    sOomState.store(kOomStateReleased);
  }
  // INSTANTANÉ AVANT ARMEMENT (revue v2.18.8) : la zone appartient au boot
  // PRÉCÉDENT, mais le callback ci-dessous est enregistré ~200 lignes de setup
  // AVANT oomPostMortemDumpIfBoot(). Tout échec d'allocation pendant ce setup —
  // le moment le plus fragile, juste après un crash mémoire — verrait le magic
  // du boot précédent, incrémenterait son compteur et écraserait ses champs avec
  // un uptime ≈ 0 : les deux boots se mélangeraient et le diagnostic serait
  // perdu. On copie donc la zone MAINTENANT, puis on l'invalide pour que le boot
  // courant reparte d'une zone vierge ; le dump lit l'instantané.
  memcpy(sOomPmSnapshot, (const void*)sOomPmWords, sizeof(sOomPmSnapshot));
  sOomPmWords[0] = 0;  // magic invalidé : la zone est désormais au boot courant

  // Condition C1 du gate : pas de handler sans réserve armée. Si les deux malloc
  // du boot ont échoué, on laisse le tick l'inscrire dès qu'une réserve existera.
  if (oomShouldRegisterHandler(sOomReserve.load() != nullptr, false)) {
    // Miroir posé AVANT l'inscription (revue 2.20.2) : si oomNewHandler
    // s'intercalait, le pire cas devient « miroir false / handler présent »,
    // auto-réparé au tick suivant — jamais « miroir true / handler nul », qui
    // serait définitif et ferait mentir oom_handler_registered.
    sHandlerRegistered.store(true);
    std::set_new_handler(oomNewHandler);
  }
  heap_caps_register_failed_alloc_callback(oomFailedAllocHook);
}

bool oomGuardReserveArmed() {
  return sOomReserve.load() != nullptr;
}

bool oomGuardHandlerRegistered() {
  return sHandlerRegistered.load();
}

size_t oomGuardReserveArmedBytes() {
  if (sOomReserve.load() == nullptr) return 0;
  return (size_t)sOomReserveBytes.load();
}

void oomGuardNoteHeapSample(uint32_t uptimeS, uint32_t freeBytes, uint32_t maxAllocBytes) {
  // Quantification IDENTIQUE au format pur du post-mortem (free : 256 o,
  // maxBloc : 64 o) — pré-encodée en mot w6 pour que le chemin d'échec n'ait
  // qu'un unique store aligné à faire.
  uint32_t fh = freeBytes >> 8;
  if (fh > 0xFFFFu) fh = 0xFFFFu;
  uint32_t mb = maxAllocBytes >> 6;
  if (mb > 0xFFFFu) mb = 0xFFFFu;
  sHeapSampleW6.store(fh | (mb << 16));
  sHeapSampleUptimeS.store(uptimeS & 0xFFFFu);
}

void oomGuardNoteIntegrityResult(bool ok) {
  // bit4 (« jamais exécuté ») levé dès le premier passage. RMW ATOMIQUES : le
  // new_handler peut poser ses propres bits depuis une autre tâche à tout
  // instant (pas de load/store non atomique qui les écraserait).
  sOomPmFlags.fetch_and((uint8_t)~kOomPmFlagIntegrityNeverRun);
  if (!ok) sOomPmFlags.fetch_or(kOomPmFlagIntegrityFailed);
  // Persistance RTC UNIQUEMENT en cas d'échec : l'information doit survivre au
  // redémarrage propre qui suit. Un succès ne réveille PAS la zone (sinon chaque
  // boot dumperait un post-mortem vide, sans valeur diagnostique).
  if (!ok) oomPostMortemStore(false, 0, 0, 0);
}

void oomPostMortemDumpIfBoot() {
  // Copie mots → octets : sur Xtensa (little-endian) l'image mémoire est
  // EXACTEMENT le format pur documenté dans oom_guard_logic.h.
  // Lecture de l'INSTANTANÉ pris par initOomGuard() avant l'armement des chemins
  // d'échec : la zone RTC elle-même appartient déjà au boot COURANT et a pu être
  // écrite par un échec survenu pendant le setup (revue v2.18.8).
  const uint8_t* raw = sOomPmSnapshot;

  OomPostMortemFields f = oomPostMortemDecode(raw);
  if (!oomPostMortemValid(f)) return;  // power-on, 1er boot, ou RAM RTC incohérente
  if (f.failCount == 0 && (f.flags & kOomPmFlagIntegrityFailed) == 0) {
    return;  // rien à raconter (zone initialisée sans échec ni corruption)
  }

  const char* reserveLabel = "intacte";
  if ((f.flags & kOomPmFlagNominalUsed) && (f.flags & kOomPmFlagFallbackUsed)) {
    reserveLabel = "nominale+repli consommées";
  } else if (f.flags & kOomPmFlagNominalUsed) {
    reserveLabel = "nominale consommée";
  } else if (f.flags & kOomPmFlagFallbackUsed) {
    reserveLabel = "repli consommée";
  }
  const char* handlerLabel =
      (f.flags & kOomPmFlagHandlerUnregistered) ? "désenregistré" : "actif";
  // NOTE : l'ANCIENNETÉ du dernier contrôle d'intégrité n'est pas conservée (le
  // format 32 o n'a pas de champ pour elle) — seul son RÉSULTAT survit.
  const char* integrityLabel = "OK";
  if (f.flags & kOomPmFlagIntegrityNeverRun) {
    integrityLabel = "jamais exécutée";
  } else if (f.flags & kOomPmFlagIntegrityFailed) {
    integrityLabel = "ÉCHEC";
  }

  uint8_t verdict = oomPostMortemVerdict(f, kOomCorruptionMaxFails,
                                         kOomCorruptionSaneBlockRatio,
                                         kOomCorruptionBurstSpanS);
  const char* verdictLabel = "INDÉTERMINÉ";
  if (verdict == kOomVerdictStarvation) {
    verdictLabel = "FAMINE MÉMOIRE";
  } else if (verdict == kOomVerdictCorruptionSuspected) {
    verdictLabel = "SUSPICION CORRUPTION";
  }

  // Ligne UNIQUE et exploitable (buffer pile : aucun heap sollicité pour la
  // construction, le journal est le seul consommateur).
  char line[288];
  snprintf(line, sizeof(line),
           "Post-mortem OOM : échecs=%u sur %u s (1er %u o, dernier %u o "
           "caps=0x%04X, uptime=%u s), tas comptabilisé t-%us free8=%u maxB8=%u, "
           "réserve=%s, handler=%s, intégrité=%s → verdict %s",
           (unsigned)f.failCount,
           (unsigned)(uint16_t)(f.lastUptimeS - f.firstUptimeS),
           (unsigned)f.firstSize, (unsigned)f.lastSize, (unsigned)f.lastCaps,
           (unsigned)f.lastUptimeS,
           (unsigned)(uint16_t)(f.lastUptimeS - f.ringUptimeS),
           (unsigned)f.ringFreeBytes, (unsigned)f.ringMaxAllocBytes,
           reserveLabel, handlerLabel, integrityLabel, verdictLabel);

  // CRITICAL sur suspicion de corruption (information décisive, à persister),
  // WARN sinon — même politique que le dump de l'anneau heap.
  if (verdict == kOomVerdictCorruptionSuspected ||
      (f.flags & kOomPmFlagIntegrityFailed)) {
    systemLogger.critical(String(line));
  } else {
    systemLogger.warning(String(line));
  }
}

bool oomGuardIntegrityTick() {
  // Autolimitation (condition pool-chemistry C3) : cadence lente, WARN au-delà
  // de kHeapIntegrityWarnDurationUs, désactivation DÉFINITIVE au-delà de
  // kHeapIntegrityMaxDurationUs ou dès qu'une corruption est détectée.
  static bool sEnabled = true;
  static bool sEverRun = false;
  static unsigned long sLastRunMs = 0;
  static bool sDisableWarned = false;

  unsigned long now = millis();
  if (!heapIntegrityShouldRun(sEnabled, sEverRun, (uint32_t)now, (uint32_t)sLastRunMs,
                              (uint32_t)kHeapIntegrityCheckIntervalMs)) {
    return false;
  }
  sLastRunMs = now;

  // Section critique : le parcours prend les verrous de l'allocateur. AUCUN
  // esp_task_wdt_reset() DEDANS (condition C3) — juste après.
  int64_t startUs = esp_timer_get_time();
  bool ok = heap_caps_check_integrity_all(true);
  uint32_t durationUs = (uint32_t)(esp_timer_get_time() - startUs);
  esp_task_wdt_reset();

  if (!sEverRun) {
    sEverRun = true;
    // Première durée MESURÉE journalisée en INFO (chiffre réel attendu par la
    // revue pool-chemistry pour valider l'innocuité du contrôle).
    systemLogger.info("Contrôle d'intégrité du tas : 1er passage en " +
                      String(durationUs) + " µs (cadence " +
                      String(kHeapIntegrityCheckIntervalMs / 1000UL) + " s, plafond " +
                      String(kHeapIntegrityMaxDurationUs) + " µs)");
  } else if (durationUs > kHeapIntegrityWarnDurationUs) {
    systemLogger.warning("Contrôle d'intégrité du tas long : " + String(durationUs) +
                         " µs (> " + String(kHeapIntegrityWarnDurationUs) + " µs)");
  }

  if (!heapIntegrityAdmitNextRun(durationUs, kHeapIntegrityMaxDurationUs)) {
    sEnabled = false;
    if (!sDisableWarned) {
      sDisableWarned = true;
      systemLogger.warning("Contrôle d'intégrité du tas DÉSACTIVÉ : durée " +
                           String(durationUs) + " µs > plafond " +
                           String(kHeapIntegrityMaxDurationUs) + " µs");
    }
  }

  oomGuardNoteIntegrityResult(ok);
  if (!ok) {
    sEnabled = false;  // one-shot : plus aucun contrôle après détection
    return true;
  }
  return false;
}

void oomGuardLoopTick() {
  unsigned long now = millis();

  // 0) FILET D'URGENCE — ré-inscription du new_handler EN TÊTE DE FONCTION
  //    (v2.20.2, bloc A). Doit rester AVANT tout `return` : c'est précisément
  //    parce que l'unique set_new_handler vivait en FIN de fonction, derrière le
  //    `return` de l'échec de réarmement, que le parachute était mort ~2 h après
  //    le boot (le handler se désinscrit lui-même à la 2ᵉ famine).
  //    Condition C1 du gate pool-chemistry : on ne ré-inscrit QUE si une réserve
  //    est armée — un handler sans réserve n'a rien à libérer.
  bool reservePresent = (sOomReserve.load() != nullptr);
  if (reservePresent) {
    // REVUE 2.20.2 (bloquant n°1) : la (ré)inscription ne dépend VOLONTAIREMENT
    // PAS du miroir. Si elle était gardée par `!sHandlerRegistered`, une course
    // avec oomNewHandler sur async_tcp (qui fait set_new_handler(nullptr) puis
    // met le miroir à false) pouvait laisser « handler nul + miroir true » —
    // état dont plus aucun chemin ne sort, donc parachute mort jusqu'au reboot
    // ET champ oom_handler_registered MENTEUR. C'est exactement la panne du
    // 15/08 qu'on corrige. set_new_handler est idempotent : le réappeler à
    // chaque tick tant qu'une réserve est armée coûte un store et ferme la
    // fenêtre. Le miroir ne sert plus qu'au log one-shot et au diagnostic.
    // Miroir AVANT inscription, comme à l'init et au réarmement (contre-revue
    // 2.20.2). Dans l'ordre inverse, l'entrelacement suivant était possible :
    // le tick appelle set_new_handler(), puis async_tcp consomme la réserve
    // (1ʳᵉ famine) et désinscrit le handler (2ᵉ famine, miroir → false), puis le
    // tick fait exchange(true) → handler nul + réserve nulle + MIROIR TRUE. Au
    // tick suivant reservePresent est faux, donc rien ne ré-inscrit : le
    // mensonge durait jusqu'à un réarmement réussi — c'est-à-dire indéfiniment
    // dans le régime de famine qu'on cherche justement à diagnostiquer.
    bool wasRegistered = sHandlerRegistered.exchange(true);
    std::set_new_handler(oomNewHandler);
    // INFO one-shot : émis sur la TRANSITION seule, jamais à chaque tick.
    if (!wasRegistered) {
      systemLogger.info("Parachute OOM : new_handler ré-inscrit (réserve de " +
                        String(sOomReserveBytes.load()) + " o armée)");
    }
  }

  // 1) Log CRITICAL différé « réserve OOM consommée » — one-shot par épisode
  //    (drain du compteur posé par le handler). Pas de flushToDisk() ici : en
  //    pleine famine une écriture LittleFS aggraverait la pression ; le flush
  //    one-shot de checkHeapHealth() (kLowHeapWarnBytes) couvre la persistance.
  //    Le drapeau RTC posé par le handler couvre le cas d'un abort immédiat.
  static uint32_t sLoggedReleaseCount = 0;
  uint32_t releases = sOomReleaseCount.load();
  if (oomShouldLogRelease(releases, sLoggedReleaseCount)) {
    sLoggedReleaseCount = releases;
    // v2.20.2 (bloc E1) : métrique 8 BITS. Les valeurs brutes ESP.get*Heap()
    // incluent l'IRAM 32 bits inutilisable par malloc/new et affichaient un tas
    // « sain » (maxBloc 27 636 o figé) au moment même d'une famine réelle —
    // exactement le chiffre trompeur qui a égaré le diagnostic (ADR-0038).
    systemLogger.critical("Réserve OOM consommée (épisode n°" + String(releases) +
                          ") — famine heap absorbée par le parachute, maxBloc8=" +
                          String(heapLargestFreeBlock8Bit()) + " o, libre8=" +
                          String(heapFree8Bit()) + " o");
  }

  // 2) WARN différé des échecs d'allocation heap_caps (diagnostic d'attribution)
  //    — BACKOFF v2.20.2 (bloc C) : 30 s → 5 min → 30 min, retour au palier
  //    initial après kOomFailWarnDelayResetMs sans nouvel échec. Le throttle fixe
  //    de 30 s produisait ~120 WARN/h et détruisait la fenêtre forensique du
  //    journal persistant (8 Ko → 25-35 min de rétention).
  //    Condition C4 du gate : AUCUNE String supplémentaire par échec — la ligne
  //    est construite en UN snprintf sur tampon de PILE (le seul temporaire
  //    restant est celui qu'impose l'API du logger).
  //    Bloc D : l'appelant (`function_name`, littéral .rodata publié par le hook)
  //    et le masque `caps` sont drainés ICI, jamais journalisés depuis le hook.
  static uint32_t sLoggedFailedCount = 0;
  static unsigned long sLastFailWarnMs = 0;
  // Départ à 0 et NON au palier initial (revue 2.20.2) : le premier échec d'un
  // épisode est journalisé immédiatement, puis oomFailWarnNextDelayMs(0) donne
  // 30 s. Initialiser à 30 s faisait sauter ce palier — l'échelle réelle était
  // « immédiat → 5 min → 30 min », et le 30 s documenté n'existait pas.
  static uint32_t sFailWarnDelayMs = 0;
  uint32_t fails = sOomFailedAllocCount.load();
  if (fails != sLoggedFailedCount) {
    if (sLastFailWarnMs == 0 || now - sLastFailWarnMs >= sFailWarnDelayMs) {
      const char* fn = sOomFailedAllocLastFn.load();
      char ligne[160];
      snprintf(ligne, sizeof(ligne),
               "Alloc heap échouée x%u (dernière=%u o caps=0x%04X appelant=%s, cumul=%u)",
               (unsigned)(fails - sLoggedFailedCount),
               (unsigned)sOomFailedAllocLastSize.load(),
               (unsigned)sOomFailedAllocLastCaps.load(),
               (fn != nullptr) ? fn : "?",
               (unsigned)fails);
      systemLogger.warning(ligne);
      sLoggedFailedCount = fails;
      sLastFailWarnMs = now;
      sFailWarnDelayMs = oomFailWarnNextDelayMs(sFailWarnDelayMs);
    }
  } else if (sLastFailWarnMs != 0 &&
             now - sLastFailWarnMs >= (unsigned long)kOomFailWarnDelayResetMs) {
    // Une heure pleine sans nouvel échec : l'épisode est clos, le prochain doit
    // être signalé SANS délai résiduel de 30 min.
    sFailWarnDelayMs = 0;  // même convention qu'à l'initialisation (cf. ci-dessus)
    sLastFailWarnMs = 0;
  }

  // 3) Réarmement à cadence lente, à DEUX PALIERS (bug-parachute-oom-jamais-rearme).
  static unsigned long sLastRearmCheckMs = 0;
  if (now - sLastRearmCheckMs < kOomRearmCheckIntervalMs) return;
  sLastRearmCheckMs = now;

  // Critère = pointeur de réserve (pas l'état, pas la taille mémorisée) :
  // converge même si une course handler/réarmement a laissé un état Armed ou une
  // taille non nulle avec une réserve nulle (cf. logique pure).
  uint32_t armedBytes = (sOomReserve.load() == nullptr) ? 0u : sOomReserveBytes.load();
  // Ceinture (revue v2.18.8) : le handler remet la taille à 0 avant d'extraire le
  // pointeur. Lire « pointeur vivant + taille 0 » dans cette fenêtre ferait passer
  // une réserve NOMINALE pour non armée et autoriserait un DÉCLASSEMENT vers le
  // palier de repli (interdit par le design). On suppose alors le palier nominal.
  if (armedBytes == 0 && sOomReserve.load() != nullptr) armedBytes = kOomReserveBytes;
  // v2.20.2 (bloc E1) : mesure 8 BITS obligatoire ici. ESP.getMaxAllocHeap()
  // renvoyait 27 636 o FIGÉS (reliquat d'IRAM 32 bits, cf. ADR-0038) alors que le
  // plus gros bloc réellement allouable valait 4 596 o : la condition
  // « 27 636 ≥ kOomRearmMinMaxAllocBytes » était donc TOUJOURS vraie, et le tick
  // retentait un malloc(12288) voué à l'échec toutes les 10 s — la boucle infinie
  // qui fabriquait 3 « échecs d'allocation » par fenêtre de 30 s.
  uint32_t maxAlloc = heapLargestFreeBlock8Bit();
  uint32_t target = oomRearmTargetBytes(armedBytes, maxAlloc,
                                        (uint32_t)kOomReserveBytes,
                                        kOomRearmMinMaxAllocBytes,
                                        (uint32_t)kOomFallbackReserveBytes,
                                        kOomFallbackRearmMinMaxAllocBytes);
  if (target == 0) return;  // déjà au meilleur palier, ou tas insuffisant

  // SEUL malloc auto-infligé de la garde : encadré par le drapeau pour que
  // oomFailedAllocHook ne comptabilise pas son propre échec (bloc E1). Le drapeau
  // est retiré IMMÉDIATEMENT après, y compris en cas d'échec.
  sSelfAllocInFlight.store(true);
  void* reserve = malloc(target);
  sSelfAllocInFlight.store(false);
  if (reserve == nullptr) return;  // maxBloc a bougé entre-temps : retente au prochain tick

  // Ordre voulu : taille puis pointeur AVANT état. Si le handler court entre les
  // deux (nouvelle famine immédiate), il consomme la nouvelle réserve via
  // exchange — jamais de double free ni de fuite. Exchange en
  // ceinture-bretelles : une réserve résiduelle (surclassement repli → nominal,
  // ou course théorique) est libérée ici.
  sOomReserveBytes.store(target);
  void* previous = sOomReserve.exchange(reserve);
  if (previous != nullptr) free(previous);
  sOomState.store(kOomStateArmed);
  // Ré-enregistrement inconditionnel (idempotent) : CONSERVÉ en v2.20.2 comme
  // ceinture — il couvre le cas où la réserve vient d'être armée dans ce même
  // tick, sans attendre le filet d'urgence du tick suivant (bloc A). Le miroir
  // est mis à jour juste après pour que la transition ne soit journalisée qu'une
  // fois (ici, pas une seconde fois en tête du prochain tick).
  // Miroir AVANT inscription (revue 2.20.2), même raison qu'à l'init : le seul
  // état inatteignable doit rester « miroir true / handler nul ».
  sHandlerRegistered.store(true);
  std::set_new_handler(oomNewHandler);
  bool upgrade = (armedBytes > 0);
  systemLogger.info(String(upgrade ? "Parachute OOM surclassé" : "Parachute OOM réarmé") +
                    " : réserve de " + String(target) + " o (" +
                    String(target >= (uint32_t)kOomReserveBytes ? "palier nominal"
                                                                : "palier de repli") +
                    ") allouée, maxBloc8=" + String(maxAlloc) + " o");
}
