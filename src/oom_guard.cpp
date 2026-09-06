#include <Arduino.h>
#include <new>       // std::set_new_handler
#include <atomic>
#include <stdio.h>   // snprintf (ligne de WARN construite sur la PILE, aucune String)
#include <string.h>  // memcpy (copie de la zone RTC vers un tampon d'octets)
#include <esp_heap_caps.h>  // heap_caps_malloc/free, register_failed_alloc_callback, check_integrity_all
#include <esp_task_wdt.h>   // esp_task_wdt_reset (après la section critique du contrôle d'intégrité)
#include <esp_timer.h>      // esp_timer_get_time (mesure de durée du contrôle d'intégrité)
#include <esp_cpu.h>        // esp_cpu_get_cycle_count (horloge SANS verrou de la sonde du refus)
#include <esp_memory_utils.h>  // esp_ptr_internal (IDF 5) — vérification C1 de l'emplacement
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>  // xTaskGetTickCount (horloge SANS verrou pour le chemin d'échec)
#include "oom_guard.h"
#include "oom_guard_logic.h"
#include "constants.h"
// heapFree8Bit / heapLargestFreeBlock8Bit / heapMetricsSnapshot : JAMAIS depuis
// les chemins d'échec (aucune borne sur leur coût interruptions coupées).
// heapProbeForCaps / heapCaps8BitMask : SEULE mesure autorisée depuis le chemin
// d'échec, et uniquement via l'entonnoir borné oomProbeAtFailure() ci-dessous —
// la restriction est un COÛT INTERRUPTIONS COUPÉES, pas un risque de deadlock
// (requalification bug-080 B-2, cf. heap_metrics.h).
#include "heap_metrics.h"
#include "logger.h"

// =============================================================================
// INTERDICTION DE malloc() DANS CE FICHIER (bug-080 B-1, condition C3 du gate)
// =============================================================================
// Depuis heap_caps_malloc_extmem_enable(kPsramExtmemThresholdBytes), un malloc()
// ordinaire de plus de 4095 o est tenté EN PSRAM D'ABORD. La réserve du parachute
// (12 288 o) tomberait donc en PSRAM — et libérer de la PSRAM pendant une famine
// de RAM INTERNE ne sauve strictement rien : le parachute deviendrait décoratif.
// Le poison rend l'erreur IMPOSSIBLE À RECOMMETTRE : tout retour de malloc() dans
// ce fichier casse la compilation. Placé APRÈS tous les #include (un header du
// core qui déclare malloc casserait le build sinon).
// free() N'EST PAS empoisonné : la réserve est libérée par free() (heap_caps_free
// ≡ free sur un pointeur issu de heap_caps_malloc), y compris depuis le
// new_handler où l'on ne veut aucun appel non trivial.
// calloc/realloc/strdup sont empoisonnés AUSSI (test-engineer + revue) : ils
// passent par le MÊME allocateur par défaut et contourneraient l'entonnoir tout
// aussi bien — `calloc(1, target)` compilait sans broncher avant ce durcissement.
// `new` est un mot-clé, non empoisonnable : c'est assumé (aucun new ici).
#pragma GCC poison malloc calloc realloc strdup

// Entonnoir UNIQUE d'allocation de la réserve d'urgence (les trois sites —
// nominal, repli, réarmement — passent par ici, et par ici seulement).
// MALLOC_CAP_INTERNAL : jamais de PSRAM, quelle que soit la politique globale.
// MALLOC_CAP_8BIT : mémoire adressable octet par octet, la seule que malloc/new
// ordinaires savent servir — c'est bien celle-là dont la famine nous prive
// (l'IRAM 32 bits gonflait la métrique historique, ADR-0038).
static void* oomAllocReserveInternal(size_t bytes) {
  return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

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
// Revue B-1 (priorité 1) : drapeau d'alerte « aucune réserve armable », levé ici
// et DRAINÉ par mqtt_manager depuis son propre contexte. oom_guard ne publie pas
// lui-même : il est une feuille et ne doit rien allouer en famine.
static std::atomic<bool>     sNoReserveAlertPending{false};
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
// TROISIÈME zone RTC noinit du projet, magic PROPRE (0xF00DFA13 depuis bug-080
// B-2, cf. kOomPostMortemMagic) — distinct de l'anneau heap (0xB10C4EAA,
// heap_ring.cpp) et de l'auto-guérison (0xA5C0FFEE, main.cpp). Budget RAM RTC
// noinit total, RE-DOCUMENTÉ après l'extension 32 → 48 o de B-2 :
// auto-heal 9 o + anneau 1032 o + post-mortem 48 o = 1089 o sur 8192 disponibles.
//
// UNE seule zone, UN seul dump, UN seul budget : le drapeau « réserve consommée »
// demandé par la spec EST l'octet `flags` de cette zone (pas une variable RTC
// séparée). ⚠️ Cet octet est SATURÉ (8/8 bits) depuis B-2.
//
// La zone est vue comme 12 mots de 32 bits ALIGNÉS : les chemins d'échec
// (oomFailedAllocHook, oomNewHandler) n'y font que des STORES BRUTS ALIGNÉS.
// RESTRICTION REQUALIFIÉE (bug-080 B-2) — ce n'est PAS un risque de deadlock :
// le désassemblage de libheap.a montre que `heap_caps.c.obj` ne porte AUCUNE
// relocation vers multi_heap_internal_lock/_unlock (le verrou vit entièrement
// dans multi_heap.c.obj) et que `heap_caps_alloc_failed` (statique, .iram1.0)
// n'est atteinte qu'APRÈS le retour complet de heap_caps_malloc_base. Aucun
// verrou de tas n'est détenu ici. Ce qui reste INTERDIT, et pour une autre
// raison — le COÛT INTERRUPTIONS COUPÉES d'un parcours TLSF, dans un chemin
// déclenchable en rafale et appelable depuis une ISR — : ESP.get*Heap(), tout
// verrou, toute allocation, tout log, et tout appel heap_caps_* HORS de
// l'exception bornée introduite par B-2 (oomProbeAtFailure : jamais en ISR, un
// échantillon par épisode, plancher de kOomProbeMinIntervalMs, durée mesurée,
// désactivation définitive au-delà de kOomProbeMaxDurationUs).
// Correspondance mot ↔ champs, IDENTIQUE au format pur (little-endian, cf.
// oom_guard_logic.h) :
//   w0 = magic | w1 = failCount | w2 = firstSize | w3 = lastSize | w4 = lastCaps
//   w5 = firstUptimeS | (lastUptimeS << 16)
//   w6 = (ringFree >> 8) | ((ringMaxB >> 6) << 16)
//   w7 = ringUptimeS | (flags << 16) | (releaseCount << 24)
//   w8 = probeSize (exact)
//   w9 = (probeCapsMaxB >> 6) | ((probeCapsFree >> 8) << 16)
//   w10 = (probeRefMaxB >> 6) | ((probeRefFree >> 8) << 16)
//   w11 = probeCaps (16 bits bas) | (probeDurationUs saturée 16 bits << 16)
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
              "La zone post-mortem RTC doit faire exactement kOomPostMortemBytes octets");

// =============================================================================
// SONDE DU TAS AU MOMENT EXACT DU REFUS (bug-080, lot B-2)
// =============================================================================
// État de la sonde. TOUT est atomique : le hook s'exécute depuis n'importe quelle
// tâche, sur l'un ou l'autre cœur. Aucun log, aucune allocation, et aucun verrou
// PRIS PAR NOUS depuis le hook — les messages (INFO du 1er échantillon, WARN de
// sonde lente, WARN de désactivation) sont DIFFÉRÉS et drainés par
// oomGuardLoopTick().
// NUANCE À NE PAS PERDRE (revue B-2, même exigence d'honnêteté que la
// requalification de heap_metrics.h) : la sonde elle-même entre dans
// multi_heap_get_info_impl, qui prend `multi_heap_internal_lock`. Nous ne
// prenons aucun verrou, mais nous en ATTENDONS un : sur bicœur, la sonde peut
// bloquer sur ce spinlock si l'autre cœur est au milieu d'un multi_heap_malloc
// long. CCOUNT le verra APRÈS COUP (la durée mesurée inclut l'attente et
// déclenchera WARN ou désactivation), la borne a priori de la condition 2 NON —
// elle borne le parcours, pas la contention.
//
// sProbeArmed : JETON « un échantillon par ÉPISODE ». Consommé par un exchange
// atomique dans le hook (deux cœurs en échec simultané ne peuvent pas sonder
// tous les deux), REPOSÉ uniquement par oomGuardLoopTick(), JAMAIS par le hook.
// Il suit donc exactement le backoff du WARN d'échec (immédiat → 30 s → 5 min →
// 30 min) : aucun état de cadence supplémentaire n'est introduit.
// Il démarre à FALSE : condition 5 du gate pool-chemistry — aucune sonde tant que
// oomGuardLoopTick() n'a pas tourné au moins une fois (l'état n'est initialisé
// que là, et non dans initOomGuard(), pour qu'aucun échec du setup ne sonde).
static std::atomic<bool>     sProbeArmed{false};
static std::atomic<bool>     sProbeDisabled{false};        // Désactivation DÉFINITIVE (durée > plafond)
static std::atomic<bool>     sProbeDisableWarnPending{false};  // WARN one-shot différé de la désactivation
static std::atomic<uint32_t> sProbeDisableDurationUs{0};   // Durée fautive, pour ce WARN
static std::atomic<uint32_t> sProbeSlowUs{0};              // Durée > kOomProbeWarnDurationUs à signaler (0 = rien)
static std::atomic<uint32_t> sProbeLastMs{0};              // Horodatage de la dernière sonde (plancher temporel)
static std::atomic<uint32_t> sProbeCount{0};               // Sondes prises depuis le boot (diagnostic)
static std::atomic<uint32_t> sProbeFirstDurationUs{0};     // Durée du 1er échantillon (INFO one-shot)
static std::atomic<uint32_t> sProbeLastDurationUs{0};      // Durée du dernier COUPLE mesuré
static std::atomic<uint32_t> sProbeMaxDurationUs{0};       // Pire durée observée (diagnostic)
static std::atomic<uint32_t> sProbeLastCaps{0};            // Caps de la dernière sonde
// Valeurs EXACTES (non quantifiées) de la dernière sonde, pour le WARN VIVANT —
// la zone RTC, elle, ne conserve que la version quantifiée. Ce WARN est le seul
// signal exploitable sans crash : il ne doit pas être moins précis que le dump.
static std::atomic<uint32_t> sProbeCapsMaxB{0};
static std::atomic<uint32_t> sProbeCapsFree{0};
static std::atomic<uint32_t> sProbeRefMaxB{0};
static std::atomic<uint32_t> sProbeRefFree{0};
// Cycles CPU par microseconde, calculé UNE fois par initOomGuard() : le hook ne
// fait qu'une division entière. Pas d'atomique — écrit au setup avant que le
// moindre chemin d'échec puisse tourner, lu seul ensuite.
static uint32_t sProbeCyclesPerUs = kOomProbeDefaultCpuMhz;

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
    // Boucle plutôt que 11 stores littéraux depuis l'extension à 12 mots (B-2) :
    // même effet, moins de flash, et impossible d'oublier un mot neuf.
    for (uint32_t w = 1; w < kOomPostMortemBytes / 4u; w++) sOomPmWords[w] = 0;
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

// Recopie le miroir de drapeaux dans l'octet `flags` de w7, SANS toucher aux
// autres champs du mot (uptime de l'anneau, compteur d'épisodes). Store brut
// aligné : aucun verrou, aucune allocation, aucun log — appelable depuis le
// chemin d'échec.
//
// INDISPENSABLE (revue B-2, défaut BLOQUANT) : oomPostMortemStore() tourne AVANT
// la sonde, donc tout bit posé APRÈS lui (ProbeTaken, ProbeIsrSkipped,
// ProbeDisabled) ne serait flushé en RAM RTC qu'au refus SUIVANT — c'est-à-dire
// JAMAIS si un abort suit, précisément le scénario que le post-mortem sert à
// qualifier. Sans ce flush, la zone contient w8..w11 renseignés mais un `flags`
// sans ProbeTaken : au boot, la sonde est déclarée « absente », le verdict
// retombe en INDÉTERMINÉ et la ligne 2 n'est pas émise — la mesure est écrite
// puis silencieusement jetée.
// Le MASQUE lui-même est un prédicat PUR (oomPmPackFlagsIntoW7, ADR-0017) :
// écraser par erreur les bits 0-15 effacerait l'uptime de l'échantillon
// d'anneau, les bits 24-31 le compteur d'épisodes.
static inline void oomPostMortemFlushFlags() {
  sOomPmWords[7] = oomPmPackFlagsIntoW7(sOomPmWords[7], sOomPmFlags.load());
}

// Pose un ou plusieurs drapeaux ET les fait atteindre la RAM RTC dans le MÊME
// geste. TOUT ajout de drapeau depuis le chemin d'échec DOIT passer par ici :
// séparer la pose du flush EST le défaut B2 (mesure écrite puis silencieusement
// jetée quand un abort suit le refus), et cette séparation n'est détectable par
// AUCUN test natif — le défaut n'est pas dans une valeur, il est dans l'ABSENCE
// d'une instruction. On le rend donc inexprimable : reproduire B2 exigerait
// d'écrire délibérément un `fetch_or` nu. Coût flash nul (inline).
// EXCEPTION VOLONTAIRE : oomNewHandler() garde son `fetch_or` nu, car il
// enchaîne sur oomPostMortemStore(), qui reconstruit w7 EN ENTIER. Chemin
// validé — ne pas l'aligner ici par symétrie cosmétique.
static inline void oomPmSetFlags(uint8_t bits) {
  sOomPmFlags.fetch_or(bits);
  oomPostMortemFlushFlags();
}

// -----------------------------------------------------------------------------
// Sonde du tas AU MOMENT EXACT du refus (bug-080 B-2) — entonnoir UNIQUE
// -----------------------------------------------------------------------------
// CE QUE ÇA CORRIGE : le verdict du post-mortem confrontait la taille refusée à
// l'échantillon de l'ANNEAU (périmé de 0 à 2 s, mesuré sur des caps FIXES
// INTERNAL|8BIT). Sur les 6 échecs réels du 05/09/2026 — 1 622 o refusés avec
// caps=0x080C (8BIT|DMA|INTERNAL) — il concluait « SUSPICION CORRUPTION » sur un
// maxBloc de 26 612 o qui ne répondait pas à la question posée. La sonde mesure
// DEUX fois : les caps DE L'ÉCHEC (le pool réellement à sec) et la RÉFÉRENCE
// INTERNAL|8BIT (le tas général) — d'où le verdict « FAMINE DMA (pool restreint) ».
//
// AUCUN FILTRAGE sur `caps`, `size` ou `function_name` : le hook est appelé
// depuis 12 sites de l'IDF (malloc, malloc_default, realloc, realloc_default,
// malloc_prefer, realloc_prefer, calloc, calloc_prefer, aligned_alloc,
// aligned_alloc_default...) et TOUS doivent pouvoir rendre un verdict. Le seul
// filtre conservé est oomShouldCountFailure (bruit auto-infligé). Pour les caps
// SPIRAM, la sonde ne saute pas : elle change d'algorithme (O(régions) au lieu
// d'O(blocs)) — la borne a priori de la condition 2 du gate est dans
// heapProbeForCaps(), pas ici.
//
// ORDRE IMPÉRATIF DES GARDES (décision 2 de l'architecte) :
//   désactivation → ISR → plancher temporel → consommation du jeton.
// Tester l'ISR APRÈS le jeton le brûlerait sans jamais sonder ; consommer le
// jeton avant le plancher temporel le gaspillerait sur une rafale.
//
// PAS D'esp_task_wdt_reset() APRÈS LA SECTION — divergence ASSUMÉE et validée
// par le gate face au modèle d'oomGuardIntegrityTick() : ce dernier vise un
// parcours de 5 à 20 ms exécuté DANS la loop ; ici la mesure est plafonnée à
// kOomProbeMaxDurationUs (1,5 ms) et ce chemin est appelable depuis n'importe
// quelle tâche. Prendre le verrou du TWDT depuis un contexte non-loop serait un
// risque net pour un bénéfice nul.
static inline void oomProbeAtFailure(uint32_t size, uint32_t caps) {
  if (sProbeDisabled.load()) return;  // désactivation DÉFINITIVE (autolimitation)

  // JAMAIS depuis une ISR : le hook y est appelable, et un parcours de tas
  // interruptions coupées dans un contexte d'interruption est inacceptable.
  if (xPortInIsrContext()) {
    oomPmSetFlags(kOomPmFlagProbeIsrSkipped);  // l'indétermination sera ATTRIBUABLE
    return;
  }

  // Plancher temporel ABSOLU (condition 3 du gate), EN PLUS du jeton : le
  // backoff du WARN démarre à 0 ms, donc son drain immédiat pourrait réarmer le
  // jeton quelques millisecondes après la sonde précédente. millis() n'est
  // atteint qu'ICI, après la garde ISR — donc toujours en contexte tâche.
  // Prédicat PUR (ADR-0017) : la décision est dans oom_guard_logic.h, ce chemin
  // ne fait que lire les atomiques et l'horloge.
  uint32_t nowMs = (uint32_t)millis();
  if (!oomProbeFloorElapsed(sProbeCount.load(), nowMs, sProbeLastMs.load(),
                            kOomProbeMinIntervalMs)) {
    return;
  }

  // Jeton : un échantillon par épisode. La consommation DOIT rester un exchange
  // atomique (oomProbeTakeTokenModel en est le MODÈLE pur, testé en natif mais
  // jamais exécuté : déléguer perdrait l'atomicité) : deux
  // cœurs en échec simultané ne doivent pas sonder tous les deux.
  if (!sProbeArmed.exchange(false)) return;
  sProbeLastMs.store(nowMs);

  // Horloge CCOUNT : UNE instruction (rsr.ccount), aucun verrou — contrairement
  // à esp_timer_get_time() qui prend un portMUX dans certaines implémentations.
  // Le compteur est PAR CŒUR : si la tâche migre pendant la mesure, la
  // soustraction n'a aucun sens. On lit donc le cœur avant et après ; en cas de
  // migration la durée est marquée INVALIDE (0xFFFF) et l'autolimitation n'est
  // PAS appliquée — une migration ne doit jamais désactiver la sonde à vie.
  // Repliement du compteur à ~17,9 s (240 MHz) sans effet : la soustraction
  // uint32_t reste exacte pour une mesure de l'ordre de la milliseconde.
  uint32_t coreBefore = (uint32_t)xPortGetCoreID();
  uint32_t c0 = (uint32_t)esp_cpu_get_cycle_count();
  HeapProbe pc = heapProbeForCaps(caps);              // caps DE L'ÉCHEC
  HeapProbe pr = heapProbeForCaps(heapCaps8BitMask());  // RÉFÉRENCE INTERNAL|8BIT
  uint32_t c1 = (uint32_t)esp_cpu_get_cycle_count();
  uint32_t coreAfter = (uint32_t)xPortGetCoreID();

  // Durée du COUPLE (les DEUX parcours), pas de chacun : c'est l'assiette
  // validée par la condition 1 du gate pour kOomProbeWarnDurationUs / MaxDurationUs.
  // 0xFFFF est RÉSERVÉ au marqueur « durée invalide » (migration de cœur) : une
  // durée réelle est donc saturée à 0xFFFE, pour que les deux cas restent
  // discernables dans le dump comme dans le diagnostic.
  bool durationValid = (coreBefore == coreAfter);
  uint32_t durationUs = kOomProbeDurationInvalid;
  if (durationValid) {
    durationUs = oomProbeSaturateDurationUs((c1 - c0) / sProbeCyclesPerUs);
  }

  uint32_t n = sProbeCount.fetch_add(1) + 1u;
  sProbeLastDurationUs.store(durationUs);
  sProbeLastCaps.store(caps);
  sProbeCapsMaxB.store(pc.largestBlockBytes);
  sProbeCapsFree.store(pc.freeBytes);
  sProbeRefMaxB.store(pr.largestBlockBytes);
  sProbeRefFree.store(pr.freeBytes);
  if (n == 1u) sProbeFirstDurationUs.store(durationUs);
  if (durationValid && durationUs > sProbeMaxDurationUs.load()) {
    sProbeMaxDurationUs.store(durationUs);
  }
  oomPmSetFlags(kOomPmFlagProbeTaken);

  // Stores BRUTS ALIGNÉS dans la zone RTC. oomPostMortemStore() a déjà tourné
  // (donc le magic est posé et w8..w11 remis à zéro si la zone venait d'être
  // initialisée) : l'ordre hook → sonde est impératif.
  sOomPmWords[8] = size;
  sOomPmWords[9] = oomPmPackProbeWord(pc.largestBlockBytes, pc.freeBytes);
  sOomPmWords[10] = oomPmPackProbeWord(pr.largestBlockBytes, pr.freeBytes);
  sOomPmWords[11] = (caps & 0xFFFFu) | (durationUs << 16);

  // Autolimitation. POST-HOC par construction (le dépassement a DÉJÀ eu lieu) —
  // c'est précisément pourquoi le gate exige en plus la borne a priori du pire
  // cas PSRAM dans heapProbeForCaps(). Le WARN est DIFFÉRÉ : jamais de log ici.
  if (durationValid && durationUs > kOomProbeWarnDurationUs) {
    sProbeSlowUs.store(durationUs);
  }
  if (durationValid && !heapIntegrityAdmitNextRun(durationUs, kOomProbeMaxDurationUs)) {
    sProbeDisabled.store(true);
    sProbeDisableDurationUs.store(durationUs);
    sProbeDisableWarnPending.store(true);
    // oomPmSetFlags : pose ET flush RTC en un geste. oomPostMortemStore() est
    // déjà passé et ne repassera qu'au refus SUIVANT — qui n'aura jamais lieu si
    // un abort suit, c'est-à-dire précisément quand le post-mortem sert.
    oomPmSetFlags(kOomPmFlagProbeDisabled);
  }
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

  // Sonde du tas AU MOMENT EXACT du refus (bug-080 B-2). Appelée APRÈS
  // oomPostMortemStore() : c'est lui qui pose le magic et remet w8..w11 à zéro
  // lors de l'initialisation de la zone — sonder avant les effacerait.
  oomProbeAtFailure((uint32_t)size, caps);
}

void initOomGuard() {
  // Conversion cycles → µs de la sonde, calculée UNE SEULE FOIS ici : le hook
  // ne fait ensuite qu'une division entière (bug-080 B-2). La zone RTC stocke
  // des MICROSECONDES, jamais des cycles — pas d'ambiguïté de format si la
  // fréquence CPU diffère entre deux boots.
  uint32_t cpuMhz = (uint32_t)getCpuFrequencyMhz();
  sProbeCyclesPerUs = (cpuMhz == 0u) ? kOomProbeDefaultCpuMhz : cpuMhz;

  // Réserve allouée au plus TÔT du setup : bloc contigu pris avant toute
  // fragmentation. Pas de log ici (systemLogger.begin() pas encore appelé) —
  // le setup journalise l'armement (et le palier obtenu) juste après.
  // Deux paliers (v2.18.8) : nominal, puis repli si le nominal est refusé.
  // bug-080 B-1 : allocation via l'entonnoir INTERNAL|8BIT (jamais malloc()).
  void* reserve = oomAllocReserveInternal(kOomReserveBytes);
  size_t armedBytes = kOomReserveBytes;
  if (reserve == nullptr) {
    reserve = oomAllocReserveInternal(kOomFallbackReserveBytes);
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

bool oomGuardReserveIsInternalRam() {
  // Condition C1 du gate : l'invariant « la réserve vit en RAM interne » est LU
  // sur le pointeur publié, jamais déduit du code d'allocation. esp_ptr_internal()
  // est une comparaison d'adresse en ligne (inline, aucun verrou, aucun parcours
  // de tas) : appelable depuis la loop comme depuis un handler web.
  void* reserve = sOomReserve.load();
  if (reserve == nullptr) return false;  // pas de réserve = pas d'invariant à tenir
  return esp_ptr_internal(reserve);
}

bool oomGuardConsumeNoReserveAlert() {
  // Drain ONE-SHOT, appelé par mqtt_manager depuis SA tâche : il alloue déjà,
  // connaît son état de connexion, et n'est pas le filet de sécurité de plus bas
  // niveau. Voir la note « priorité 1 » du bloc C2 dans oomGuardLoopTick().
  return sNoReserveAlertPending.exchange(false, std::memory_order_relaxed);
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
  } else if (verdict == kOomVerdictDmaStarvation) {
    verdictLabel = "FAMINE DMA (pool restreint)";
  }

  // Sous-cas de la sonde, NOMMÉ dans la ligne de verdict : une indétermination
  // doit toujours être ATTRIBUABLE (« absente » ≠ « ISR » ≠ « désactivée » ≠
  // « autre refus », ce dernier signifiant que la sonde décrit une classe
  // d'échec différente du dernier échec enregistré — le jeton « un échantillon
  // par épisode » rend ce cas possible).
  const char* probeLabel = "absente";
  if (f.flags & kOomPmFlagProbeDisabled) {
    probeLabel = "désactivée";
  } else if (f.flags & kOomPmFlagProbeTaken) {
    probeLabel = oomPostMortemProbeRepresentative(f) ? "oui" : "autre refus";
  } else if (f.flags & kOomPmFlagProbeIsrSkipped) {
    probeLabel = "ISR";
  }

  // Tampon RECALCULÉ au pire cas en B-2, puis RE-RECALCULÉ en revue (288 → 384,
  // et NON 320 : ce dernier n'avait qu'UN octet de marge). Pire cas en octets
  // UTF-8 (accents 2 o, `—`/`→` 3 o) : **319 o**, et **323 o** si `f.lastCaps`
  // ≥ 0x10000 — ce qui est atteignable, car oomPostMortemValid() ne borne QUE
  // firstSize / lastSize / failCount, et toute la machinerie de plausibilité
  // existe justement parce que la RAM RTC peut être corrompue : `%04X` imprime
  // alors 8 chiffres. À noter aussi que `sonde=désactivée` (12 o) et
  // `verdict=FAMINE DMA (pool restreint)` (27 o) sont CO-OCCURRENTS — probeLabel
  // teste ProbeDisabled en premier, mais oomPostMortemVerdict() ignore ce
  // drapeau, donc les deux pires cas s'additionnent bel et bien.
  // Ligne 1 : la comptabilité de l'épisode (buffer pile : aucun heap sollicité
  // pour la construction, le journal est le seul consommateur).
  char line[384];
  snprintf(line, sizeof(line),
           "Post-mortem OOM : échecs=%u sur %u s (1er %u o, dernier %u o "
           "caps=0x%04X, uptime=%u s), tas comptabilisé t-%us free8=%u maxB8=%u, "
           "réserve=%s, handler=%s, intégrité=%s, sonde=%s → verdict %s",
           (unsigned)f.failCount,
           (unsigned)(uint16_t)(f.lastUptimeS - f.firstUptimeS),
           (unsigned)f.firstSize, (unsigned)f.lastSize, (unsigned)f.lastCaps,
           (unsigned)f.lastUptimeS,
           (unsigned)(uint16_t)(f.lastUptimeS - f.ringUptimeS),
           (unsigned)f.ringFreeBytes, (unsigned)f.ringMaxAllocBytes,
           reserveLabel, handlerLabel, integrityLabel, probeLabel, verdictLabel);

  // CRITICAL sur suspicion de corruption (information décisive, à persister),
  // WARN sinon — même politique que le dump de l'anneau heap. La FAMINE DMA
  // n'est PAS une corruption : elle reste en WARN.
  if (verdict == kOomVerdictCorruptionSuspected ||
      (f.flags & kOomPmFlagIntegrityFailed)) {
    systemLogger.critical(String(line));
  } else {
    systemLogger.warning(String(line));
  }

  // Ligne 2 (bug-080 B-2) : la SONDE prise au moment exact du refus. Une ligne
  // SÉPARÉE, volontairement : fusionner les deux ramènerait le risque de
  // troncature silencieuse corrigé en B-1. Émise SEULEMENT si la sonde a été
  // prise — sinon la ligne 1 dit déjà pourquoi (sonde=absente/ISR/désactivée) et
  // afficher des zéros passerait pour une mesure.
  if (f.flags & kOomPmFlagProbeTaken) {
    // Pire cas calculé : partie fixe 110 o + libellé 12 o + 7 champs = 181 o.
    char probeLine[224];
    snprintf(probeLine, sizeof(probeLine),
             "Post-mortem OOM (sonde %s) : au refus de %u o caps=0x%04X → maxBloc=%u o "
             "libre=%u o ; réf8 maxBloc=%u o libre=%u o, en %u µs",
             probeLabel, (unsigned)f.probeSize, (unsigned)f.probeCaps,
             (unsigned)f.probeCapsMaxB, (unsigned)f.probeCapsFree,
             (unsigned)f.probeRefMaxB, (unsigned)f.probeRefFree,
             (unsigned)f.probeDurationUs);
    systemLogger.warning(probeLine);
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

OomProbeStats oomGuardProbeStats() {
  OomProbeStats s;
  s.count = sProbeCount.load();
  s.lastDurationUs = sProbeLastDurationUs.load();
  s.maxDurationUs = sProbeMaxDurationUs.load();
  s.lastCaps = sProbeLastCaps.load();
  s.enabled = !sProbeDisabled.load();
  return s;
}

void oomGuardLoopTick() {
  unsigned long now = millis();

  // 0 quater) SONDE DU TAS AU REFUS (bug-080 B-2) — armement initial et drains.
  //   CONDITION 5 DU GATE : aucune sonde tant que ce tick n'a pas tourné une
  //   fois. L'état est donc initialisé ICI, pas dans initOomGuard() : un échec
  //   d'allocation survenu pendant le setup (le moment le plus fragile) ne doit
  //   pas déclencher un parcours de tas avant que la loop vive.
  static bool sProbeEverArmed = false;
  if (!sProbeEverArmed) {
    sProbeEverArmed = true;
    sProbeArmed.store(true);
  }
  //   Les TROIS messages d'observabilité de la sonde partagent UN SEUL site
  //   d'émission (snprintf sur tampon de PILE, convention du fichier). Trois
  //   chaînes de String concaténées coûtaient ~500 o de flash sur une marge de
  //   13 Ko, pour trois lignes qui ne diffèrent que par un motif, un seuil et un
  //   niveau. Priorité : désactivation > 1er échantillon > sonde lente ; les cas
  //   sont exclusifs en pratique. Les drapeaux « désactivation » et « 1er
  //   échantillon » non servis à ce tick le seront au suivant (ils restent
  //   posés). CE N'EST PAS LE CAS de `slowUs` : son exchange ci-dessous a lieu
  //   AVANT de savoir quelle branche gagne, donc la valeur est PERDUE si une
  //   branche plus prioritaire l'emporte. Sans conséquence — le WARN est
  //   one-shot par boot et se redéclenchera à la prochaine sonde lente — mais à
  //   ne pas confondre avec les deux autres.
  //     - condition 4 du gate : le CHIFFRE RÉEL du 1er échantillon en INFO,
  //       one-shot — la mesure qui valide (ou non) l'innocuité de la sonde ;
  //     - condition 1 : WARN au-delà de kOomProbeWarnDurationUs, sonde CONSERVÉE ;
  //     - A5 : WARN one-shot de la désactivation définitive (jamais journalisée
  //       depuis le hook, qui n'a le droit ni de logger ni d'allouer).
  //   Le WARN « sonde lente » est ONE-SHOT PAR BOOT, volontairement : le
  //   plancher temporel autorise une sonde par seconde, donc un WARN par sonde
  //   lente ferait 3 600 lignes/h dans un journal de 8 Ko dont la rétention est
  //   de 25-35 min — la fenêtre forensique détruite par notre propre
  //   instrumentation (défaut n°3 du 15/08). Les durées restent lisibles par
  //   oomGuardProbeStats() (max observé + compteur + caps).
  static bool sProbeFirstLogged = false;
  static bool sProbeSlowWarned = false;
  uint32_t slowUs = sProbeSlowUs.exchange(0);
  const char* probeMotif = nullptr;
  uint32_t probeUs = 0;
  uint32_t probeSeuilUs = kOomProbeMaxDurationUs;
  bool probeWarn = true;
  if (sProbeDisableWarnPending.exchange(false)) {
    probeMotif = "DÉSACTIVÉE définitivement";
    probeUs = sProbeDisableDurationUs.load();
  } else if (!sProbeFirstLogged && sProbeCount.load() != 0u) {
    sProbeFirstLogged = true;
    probeMotif = "1er échantillon (couple de 2 parcours)";
    probeUs = sProbeFirstDurationUs.load();
    probeWarn = false;
  } else if (slowUs != 0u && !sProbeSlowWarned) {
    sProbeSlowWarned = true;
    probeMotif = "LENTE, mais CONSERVÉE";
    probeUs = slowUs;
    probeSeuilUs = kOomProbeWarnDurationUs;
  }
  if (probeMotif != nullptr) {
    // Condition 4 du gate : le CHIFFRE RÉEL. Une durée invalide (migration de
    // cœur entre les deux lectures de CCOUNT) ne doit donc PAS s'afficher
    // « 65535 µs », qui serait un chiffre inventé et alarmant — on dit ce qui
    // s'est passé.
    char ligneSonde[144];  // pire cas : 46 (fixe) + 39 (motif) + 2 × 10 = 105 o
    if ((uint16_t)probeUs == kOomProbeDurationInvalid) {
      snprintf(ligneSonde, sizeof(ligneSonde),
               "Sonde OOM %s : durée non mesurable (migration de cœur)", probeMotif);
    } else {
      snprintf(ligneSonde, sizeof(ligneSonde), "Sonde OOM %s : %u µs (seuil %u µs)",
               probeMotif, (unsigned)probeUs, (unsigned)probeSeuilUs);
    }
    if (probeWarn) {
      systemLogger.warning(ligneSonde);
    } else {
      systemLogger.info(ligneSonde);
    }
  }

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

  // 0 bis) CONDITION C2 DU GATE (bug-080 B-1) : aucun déclassement silencieux.
  //    (a) WARN ONE-SHOT si le palier NOMINAL a été refusé et que le parachute
  //        tourne sur son palier de REPLI. Couvre les deux origines : le boot
  //        (initOomGuard n'a pas de logger) et le réarmement dégradé.
  //        Lignes construites en UN snprintf sur tampon de PILE (convention du
  //        fichier) : elles décrivent une pénurie et peuvent donc s'émettre
  //        PENDANT une pénurie — y construire des String allouerait précisément
  //        quand l'allocation échoue.
  static bool sDowngradeWarned = false;
  if (reservePresent && !sDowngradeWarned) {
    uint32_t armed = sOomReserveBytes.load();
    if (armed > 0 && armed < (uint32_t)kOomReserveBytes) {
      sDowngradeWarned = true;
      // Revue B1 : 159 o de partie fixe (accents 2 o, cadratin 3 o) + 3 × %u.
      // Pire cas 190 o : un [176] tronquait SILENCIEUSEMENT un WARN de
      // diagnostic — exactement ce que ce lot existe pour supprimer.
      char ligne[224];
      snprintf(ligne, sizeof(ligne),
               "Parachute OOM DÉCLASSÉ : palier de repli de %u o armé (nominal %u o "
               "refusé en RAM interne 8 bits), maxBloc8=%u o — surclassement auto "
               "dès que le tas le permet",
               (unsigned)armed, (unsigned)kOomReserveBytes,
               (unsigned)heapLargestFreeBlock8Bit());
      systemLogger.warning(ligne);
    }
  }
  //    (b) CRITICAL + alerte MQTT si AUCUNE réserve n'est armée depuis plus de
  //        kOomNoReserveCriticalDelayMs. C'est exactement le régime « des heures
  //        sans filet » des 13 et 15/08/2026, qu'aucun signal ne dénonçait.
  //        One-shot par épisode : le compteur repart dès qu'une réserve existe.
  static bool sNoReserveTracking = false;
  static unsigned long sNoReserveSinceMs = 0;
  static bool sNoReserveAlerted = false;
  if (reservePresent) {
    sNoReserveTracking = false;
    sNoReserveAlerted = false;
  } else if (!sNoReserveTracking) {
    sNoReserveTracking = true;
    sNoReserveSinceMs = now;
  } else if (!sNoReserveAlerted && now - sNoReserveSinceMs >= kOomNoReserveCriticalDelayMs) {
    sNoReserveAlerted = true;
    char ligne[224];  // même dimensionnement que (a) — revue B1
    snprintf(ligne, sizeof(ligne),
             "Parachute OOM SANS RÉSERVE depuis %u min (aucun palier armable) — plus "
             "aucun filet anti-bad_alloc, maxBloc8=%u o, libre8=%u o",
             (unsigned)(kOomNoReserveCriticalDelayMs / 60000UL),
             (unsigned)heapLargestFreeBlock8Bit(), (unsigned)heapFree8Bit());
    systemLogger.critical(ligne);
    // REVUE (priorité 1) : on ne PUBLIE PAS l'alerte MQTT d'ici. `publishAlert`
    // alloue (2 String au site d'appel, String msg, String payload, ~2 tours de
    // JsonDocument) et se termine par un systemLogger.warning() qui écrirait le
    // MÊME message une SECONDE fois — dans un journal de 8 Ko dont la rétention
    // est de 25-35 min, soit la fenêtre forensique de l'épisode qu'on documente,
    // divisée par deux. Or la précondition d'arrivée ici est que le tas a refusé
    // 1024 o trente fois de suite : demander ~1 Ko en une vingtaine de blocs y
    // est absurde. Enfin, `oom_guard` est une FEUILLE (constants / oom_guard_logic
    // / heap_metrics / logger) : la faire dépendre du sous-système applicatif le
    // plus haut inverse les couches, alors qu'initOomGuard() est le TOUT PREMIER
    // appel de setup(). On lève donc un drapeau, drainé par mqtt_manager depuis
    // SON contexte, qui alloue déjà et connaît son état de connexion.
    sNoReserveAlertPending.store(true, std::memory_order_relaxed);
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
      // Tampon RECALCULÉ au pire cas en B-2 (160 → 256) : la ligne porte
      // désormais la sonde. Partie fixe 125 o (accents 2 o, cadratin/flèche 3 o)
      // + jusqu'à 32 o de nom d'appelant + 8 champs numériques = 241 o. Un [160]
      // tronquerait SILENCIEUSEMENT le WARN de diagnostic — précisément le
      // défaut que ce lot supprime.
      char ligne[256];
      int tete = snprintf(ligne, sizeof(ligne),
                          "Alloc heap échouée x%u (dernière=%u o caps=0x%04X appelant=%s,"
                          " cumul=%u)",
                          (unsigned)(fails - sLoggedFailedCount),
                          (unsigned)sOomFailedAllocLastSize.load(),
                          (unsigned)sOomFailedAllocLastCaps.load(),
                          (fn != nullptr) ? fn : "?",
                          (unsigned)fails);
      // La queue « sonde » n'est ajoutée QUE si une sonde existe : sans cette
      // garde, la ligne affichait « sonde(0x0000) maxBloc=0 libre=0 … en 0 µs »,
      // c'est-à-dire des zéros qui passent pour une mesure. Le dump se protégeait
      // déjà de ça, pas ce WARN (revue B-2). `tete` est borné par snprintf, mais
      // on le re-borne : un retour tronqué vaut la taille VOULUE, pas l'écrite.
      if (tete > 0 && (size_t)tete < sizeof(ligne)) {
        if (sProbeCount.load() != 0u) {
          snprintf(ligne + tete, sizeof(ligne) - (size_t)tete,
                   " — sonde(0x%04X) maxBloc=%u libre=%u, réf8 maxBloc=%u libre=%u,"
                   " en %u µs",
                   (unsigned)sProbeLastCaps.load(),
                   (unsigned)sProbeCapsMaxB.load(), (unsigned)sProbeCapsFree.load(),
                   (unsigned)sProbeRefMaxB.load(), (unsigned)sProbeRefFree.load(),
                   (unsigned)sProbeLastDurationUs.load());
        } else {
          snprintf(ligne + tete, sizeof(ligne) - (size_t)tete, " — sonde=absente");
        }
      }
      systemLogger.warning(ligne);
      sLoggedFailedCount = fails;
      sLastFailWarnMs = now;
      sFailWarnDelayMs = oomFailWarnNextDelayMs(sFailWarnDelayMs);
      // (a) RÉARMEMENT DU JETON DE SONDE (décision 2 de l'architecte) : le
      //     jeton est reposé au DRAIN du WARN, donc il suit exactement le
      //     backoff existant (immédiat → 30 s → 5 min → 30 min). Aucun état de
      //     cadence neuf. Jamais réarmé depuis le hook.
      if (!sProbeDisabled.load()) sProbeArmed.store(true);
    }
  } else if (sLastFailWarnMs != 0 &&
             now - sLastFailWarnMs >= (unsigned long)kOomFailWarnDelayResetMs) {
    // Une heure pleine sans nouvel échec : l'épisode est clos, le prochain doit
    // être signalé SANS délai résiduel de 30 min.
    sFailWarnDelayMs = 0;  // même convention qu'à l'initialisation (cf. ci-dessus)
    sLastFailWarnMs = 0;
    // (b) Épisode CLOS : le prochain épisode doit pouvoir sonder immédiatement.
    if (!sProbeDisabled.load()) sProbeArmed.store(true);
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

  // SEULE allocation auto-infligée de la garde : encadrée par le drapeau pour que
  // oomFailedAllocHook ne comptabilise pas son propre échec (bloc E1). Le drapeau
  // est retiré IMMÉDIATEMENT après, y compris en cas d'échec.
  // bug-080 B-1 : c'est LE site le plus important de l'entonnoir INTERNAL|8BIT —
  // il s'exécute en loop, donc APRÈS heap_caps_malloc_extmem_enable(). Un malloc()
  // ici armerait une réserve en PSRAM, inutile contre une famine de RAM interne.
  sSelfAllocInFlight.store(true);
  void* reserve = oomAllocReserveInternal(target);
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
  // Condition C1 du gate (bug-080 B-1) : l'EMPLACEMENT réel de la réserve est
  // journalisé à chaque réarmement — c'est le seul chemin d'allocation qui
  // s'exécute APRÈS la bascule PSRAM du setup, donc le seul qui pourrait dériver.
  systemLogger.info(String(upgrade ? "Parachute OOM surclassé" : "Parachute OOM réarmé") +
                    " : réserve de " + String(target) + " o (" +
                    String(target >= (uint32_t)kOomReserveBytes ? "palier nominal"
                                                                : "palier de repli") +
                    ") allouée en " +
                    // Revue B2 : on lit le pointeur LOCAL, pas l'atomique. Relire
                    // sOomReserve exposerait à une course avec oomNewHandler, qui
                    // peut consommer la réserve entre le exchange et ce log — la
                    // fenêtre s'ouvre précisément en famine, c'est-à-dire quand ce
                    // message s'émet. On annoncerait alors « PSRAM (ANOMALIE) »
                    // pour une réserve parfaitement interne : un faux positif
                    // alarmant, dans la trace même ajoutée pour éviter ça.
                    String(esp_ptr_internal(reserve) ? "RAM interne" : "PSRAM (ANOMALIE)") +
                    ", maxBloc8=" + String(maxAlloc) + " o");
}
