#ifndef OOM_GUARD_LOGIC_H
#define OOM_GUARD_LOGIC_H

// =============================================================================
// oom_guard_logic — Logique PURE du parachute OOM global
// =============================================================================
// bug-oom-accept-http-durcissement (crash du 11/08/2026) : std::bad_alloc non
// rattrapé dans async_tcp (operator new(356) à l'accept HTTP) → abort. Le
// parachute pré-alloue une réserve d'urgence (kOomReserveBytes) libérée par le
// std::set_new_handler à la première famine — le new en cours est alors
// RE-TENTÉ par libstdc++ (contrat new_handler) et réussit au lieu d'aborter.
//
// Ce module contient la logique pure testable en natif (ADR-0017, modèle
// heap_ring_logic.h) :
//   - machine à états Armed/Released/Depleted du parachute ;
//   - décision du handler à l'échec d'allocation (libérer / se désenregistrer) ;
//   - décision de réarmement (hystérésis kOomRearmMinMaxAllocBytes) et choix du
//     palier nominal/repli (oomRearmTargetBytes, v2.18.8) ;
//   - one-shot du log CRITICAL différé (drainé depuis la loop) ;
//   - encodage/décodage/validation/verdict du post-mortem d'allocation RTC
//     (48 octets depuis bug-080 B-2 : ajout de la SONDE prise au moment exact du
//     refus) et cadence du contrôle d'intégrité du tas ;
//   - prédicat de garde TLS sortante (PSRAM LIBRE + heap), PARTAGÉ par
//     /check-update et /download-update (web_routes_ota.cpp) — aucune logique
//     dupliquée.
// La coquille impure (malloc/free, std::set_new_handler, std::atomic,
// heap_caps_register_failed_alloc_callback, systemLogger) vit dans oom_guard.cpp.
//
// CONTRAINTE : pas d'Arduino.h, pas de FreeRTOS, pas d'ESP.* — <stdint.h> et
// "constants.h" (lui-même auto-suffisant : <stdint.h> + <time.h>, aucune
// dépendance Arduino) uniquement, fonctions inline (header-only, rien à ajouter
// au build natif). constants.h est tiré depuis la v2.20.2 pour les PALIERS du
// backoff du WARN d'échec (oomFailWarnNextDelayMs), dont la signature imposée ne
// prend pas les seuils en paramètres.
// La PSRAM LIBRE est donc MESURÉE par la route (ESP.getFreePsram()) et passée
// en paramètre, jamais lue ici. C'est bien la PSRAM LIBRE, et non la taille
// installée (ESP.getPsramSize(), constante et jamais décroissante) : avec
// SPIRAM_USE_MALLOC les allocations > 4 Ko partent en PSRAM, donc une PSRAM
// installée mais SATURÉE ne protège de rien — mbedTLS retomberait sur le heap
// interne, exactement le régime que la v2.18.5 a prouvé insuffisant.
// =============================================================================

#include <stdint.h>

#include "constants.h"  // Paliers kOomFailWarnDelay*Ms (backoff du WARN d'échec)

// États du parachute (stockés côté impur dans un std::atomic<uint8_t>).
enum : uint8_t {
  kOomStateArmed = 0,     // Réserve allouée, handler actif — régime nominal
  kOomStateReleased = 1,  // Réserve libérée (famine absorbée), réarmement possible
  kOomStateDepleted = 2,  // Réserve déjà consommée : handler désenregistré
                          // (le new suivant échoue en bad_alloc = comportement
                          // d'avant le parachute) — réarmable par la loop
};

// Action à exécuter par le new_handler (décidée PUREMENT, exécutée côté impur).
struct OomHandlerAction {
  bool freeReserve;        // true → free() de la réserve (déjà extraite atomiquement)
  bool unregisterHandler;  // true → std::set_new_handler(nullptr)
  uint8_t nextState;       // Nouvel état du parachute
};

// Décision du handler à l'échec d'un operator new. `reserveAvailable` = la
// réserve a été extraite atomiquement (exchange non nul) par l'appelant :
//   - réserve disponible → la libérer (le new est re-tenté et réussit) ;
//   - réserve déjà consommée → se désenregistrer puis rendre la main : le new
//     suivant lève bad_alloc (comportement actuel, filet coredump conservé).
// AUCUNE allocation, AUCUN mutex, AUCUN log dans ce chemin.
inline OomHandlerAction oomOnAllocFailed(bool reserveAvailable) {
  OomHandlerAction a;
  if (reserveAvailable) {
    a.freeReserve = true;
    a.unregisterHandler = false;
    a.nextState = kOomStateReleased;
  } else {
    a.freeReserve = false;
    a.unregisterHandler = true;
    a.nextState = kOomStateDepleted;
  }
  return a;
}

// Décision de réarmement (loop, cadence kOomRearmCheckIntervalMs) — hystérésis :
// la réserve est libérée quand maxBloc ≈ épuisé, ré-allouée SEULEMENT quand le
// plus gros bloc contigu repasse au-dessus de rearmMinMaxAllocBytes (32 Ko),
// pour ne pas osciller autour du seuil de famine.
// Le critère est l'ABSENCE DE RÉSERVE, pas l'état : si le handler consomme la
// réserve entre l'exchange du réarmement et le store(Armed) (course revue
// v2.18.5), l'état peut afficher Armed avec une réserve nulle — fonder la
// décision sur le pointeur garantit la convergence au tick suivant.
inline bool oomShouldRearm(bool reserveAbsent, uint32_t maxAllocBytes,
                           uint32_t rearmMinMaxAllocBytes) {
  return reserveAbsent && maxAllocBytes >= rearmMinMaxAllocBytes;
}

// ---------------------------------------------------------------------------
// Réarmement à DEUX PALIERS (bug-parachute-oom-jamais-rearme, v2.18.8)
// ---------------------------------------------------------------------------
// Défaut n°1 confirmé le 13/08/2026 : avec un seuil unique de 32 Ko alors que le
// plus gros bloc réel vaut 27-30 Ko, la réserve n'était JAMAIS ré-allouée après
// sa première consommation (parachute one-shot). Deux paliers désormais :
//   - NOMINAL (kOomReserveBytes / kOomRearmMinMaxAllocBytes) ;
//   - REPLI, plus petit (kOomFallbackReserveBytes /
//     kOomFallbackRearmMinMaxAllocBytes) : « toujours armé » vaut mieux que
//     « jamais réarmé ».
// `armedBytes` = taille de la réserve ACTUELLEMENT armée (0 = aucune réserve —
// l'appelant DOIT la dériver du POINTEUR de réserve, seule autorité, cf.
// oomShouldRearm). Retour = taille à allouer, 0 = NE RIEN FAIRE.
// Le surclassement repli → nominal est autorisé (une réserve de 2 Ko est
// remplacée par 12 Ko dès que le tas le permet) ; le déclassement nominal →
// repli est INTERDIT (jamais d'oscillation, jamais de dégradation silencieuse).
inline uint32_t oomRearmTargetBytes(uint32_t armedBytes, uint32_t maxAllocBytes,
                                   uint32_t nominalBytes, uint32_t nominalMinMaxAlloc,
                                   uint32_t fallbackBytes, uint32_t fallbackMinMaxAlloc) {
  if (armedBytes >= nominalBytes) return 0;  // déjà au meilleur palier : rien à faire
  if (maxAllocBytes >= nominalMinMaxAlloc) return nominalBytes;  // (ré)armement ou surclassement
  if (armedBytes > 0) return 0;              // repli déjà armé et tas insuffisant pour le nominal
  if (maxAllocBytes >= fallbackMinMaxAlloc) return fallbackBytes;  // aucune réserve → palier de repli
  return 0;                                  // tas trop dégradé, même pour le repli
}

// One-shot du log CRITICAL « réserve OOM consommée » : émis en DIFFÉRÉ depuis
// la loop, une seule fois par épisode (comparaison compteur d'épisodes vs
// compteur déjà journalisé — les compteurs ne décroissent jamais).
inline bool oomShouldLogRelease(uint32_t releaseCount, uint32_t loggedCount) {
  return releaseCount != loggedCount;
}

// ---------------------------------------------------------------------------
// FILET D'URGENCE : ré-inscription du new_handler (v2.20.2, bloc A)
// ---------------------------------------------------------------------------
// Défaut PROUVÉ le 15/08/2026 : le `std::set_new_handler(oomNewHandler)` de la
// coquille se trouvait en FIN de oomGuardLoopTick(), donc APRÈS le `return` de la
// branche d'échec de réarmement. Or le handler se DÉSINSCRIT lui-même à la 2ᵉ
// famine (état Depleted) : passé ce point, plus aucun chemin ne le ré-inscrivait
// et le parachute était mort ~2 h après le boot. La ré-inscription est désormais
// faite en TÊTE du tick, avant tout `return`.
//
// Condition C1 du gate pool-chemistry : on ne ré-inscrit QUE si une réserve est
// effectivement armée. Un handler sans réserve ne peut rien libérer — il ne
// ferait que retarder d'un tour le bad_alloc, en masquant l'état réel.
inline bool oomShouldRegisterHandler(bool reservePresent, bool handlerRegistered) {
  return reservePresent && !handlerRegistered;
}

// ---------------------------------------------------------------------------
// BACKOFF du WARN « allocation heap échouée » (v2.20.2, bloc C)
// ---------------------------------------------------------------------------
// Défaut n°3 du 15/08 : throttle FIXE de 30 s → ~120 WARN/h → le journal
// persistant (kMaxLogFileBytes = 8 Ko) tournait en 25-35 min et détruisait le
// post-mortem du crash avant qu'il puisse être lu. Le backoff élargit la fenêtre
// forensique à > 12 h sans jamais faire taire complètement le signal :
//   30 s → 5 min → 30 min (saturation).
// Retourne le délai à appliquer APRÈS l'émission dont le délai courant était
// `currentDelayMs` (0 = jamais émis → palier initial). Le RETOUR au palier
// initial après une heure sans nouvel échec (kOomFailWarnDelayResetMs) est du
// ressort de l'appelant, qui seul dispose d'une horloge.
inline uint32_t oomFailWarnNextDelayMs(uint32_t currentDelayMs) {
  if (currentDelayMs < (uint32_t)kOomFailWarnDelayInitialMs) {
    return (uint32_t)kOomFailWarnDelayInitialMs;
  }
  if (currentDelayMs < (uint32_t)kOomFailWarnDelayMidMs) {
    return (uint32_t)kOomFailWarnDelayMidMs;
  }
  return (uint32_t)kOomFailWarnDelayMaxMs;  // saturation
}

// ---------------------------------------------------------------------------
// Échecs AUTO-INFLIGÉS (v2.20.2, bloc E1)
// ---------------------------------------------------------------------------
// Mesure du 15/08 : sur 1025 échecs d'allocation comptés, ~100 % étaient le
// `malloc(target)` du réarmement du parachute lui-même, qui visait un palier
// inatteignable en régime fragmenté. Compter ces échecs revenait à s'alarmer de
// son propre bruit — et saturait le journal. L'appelant pose un drapeau autour du
// SEUL malloc de réarmement ; le hook l'interroge pour ne rien comptabiliser.
inline bool oomShouldCountFailure(bool selfAllocInFlight) {
  return !selfAllocInFlight;
}

// ---------------------------------------------------------------------------
// BORNAGE DE LA SONDE DU REFUS (bug-080 B-2) — prédicats PURS
// ---------------------------------------------------------------------------
// ADR-0017 : la coquille impure (oom_guard.cpp, heap_metrics.cpp) ne doit
// contenir QUE des atomiques, des appels heap_caps_* et des horloges. Toute
// DÉCISION est ici, sinon elle n'est testable ni en natif ni par mutation — et
// ce projet a déjà payé trois fois le prix de prédicats laissés dans la
// coquille (repliement de millis(), propagation d'une constante de pouls, terme
// intégral inopérant, tous invisibles jusqu'à l'extraction).

// Bit MALLOC_CAP_INTERNAL de l'IDF (1 << 11). RECOPIÉ ici pour que la SÉLECTION
// DE BRANCHE de la sonde soit décidable sans esp_heap_caps.h ; heap_metrics.cpp
// verrouille l'égalité avec la valeur réelle de l'IDF par un static_assert, de
// sorte qu'une divergence casse la COMPILATION du firmware, pas seulement un test.
constexpr uint32_t kOomProbeInternalCapBit = 0x0800u;

// true → la sonde doit emprunter le parcours O(RÉGIONS) (agrégats
// heap_caps_get_free_size) au lieu du parcours O(BLOCS) (largest_free_block).
//
// CONDITION 2 DU GATE pool-chemistry : sur 4 Mo de PSRAM, le parcours O(blocs)
// n'a pas de pire cas connu et s'exécute interruptions coupées. La sonde n'est
// JAMAIS sautée (interdit : les 12 sites d'appel du hook doivent tous rendre un
// verdict) — elle change d'ALGORITHME, et le champ « plus gros bloc » est alors
// marqué kOomProbeBlockUnknown. Le pire cas est ainsi borné AVANT coup, et pas
// seulement rattrapé après par l'autolimitation (qui est post-hoc par
// construction : le dépassement qui la déclenche a DÉJÀ eu lieu).
//
// ⚠️ PRÉDICAT FAIL-SAFE, et il DOIT le rester. La 1ʳᵉ écriture de ce lot testait
// la PRÉSENCE du bit SPIRAM — défaut BLOQUANT relevé en revue, qui défaisait la
// condition 2 sur la classe d'échec PRINCIPALE. Désassemblage de `libheap.a`
// (`heap_caps.c.obj`, `.iram1.2`, `.literal+0x8 = 0x00001000`) : tout échec de
// `malloc()` / `operator new` ordinaire remonte `caps = MALLOC_CAP_DEFAULT`
// (0x1000), SANS bit SPIRAM — c'est le cas des crashs du 11/08 (bad_alloc à
// l'accept HTTP) et du 13/08 (156 o), qui fondent tout ce chantier. Or le tas
// PSRAM est enregistré avec `DEFAULT|SPIRAM|8BIT|32BIT` (c'est ce qui rend
// opérant le heap_caps_malloc_extmem_enable() de B-1), donc `heap_caps_match()`
// le sélectionne et `tlsf_walk_pool` parcourrait les 4 Mo interruptions coupées.
// La sonde se serait désactivée dès le premier `new` refusé : morte sur son cas
// d'usage principal.
// D'où l'inversion : O(blocs) UNIQUEMENT si le masque GARANTIT la RAM interne.
// Tout masque qui peut atteindre la PSRAM (SPIRAM, DEFAULT, 8BIT seul…) prend
// les agrégats. `0x080C` (les 6 refus du 05/09) et la référence `0x0804`
// gardent le parcours complet : le cas motivant reste intact.
//
// PAS DE TROU DANS LE SENS DANGEREUX (vérifié en revue) : heap_caps_match()
// exige `(get_all_caps(heap) & caps) == caps`, et le tas PSRAM agrège
// `SPIRAM|DEFAULT|8BIT|32BIT` = 0x1406, donc `0x1406 & 0x0800 == 0` — un masque
// portant INTERNAL ne peut JAMAIS le sélectionner. Ce n'est pas une convention
// d'enregistrement révisable : MALLOC_CAP_INTERNAL est DÉFINI comme « ne
// disparaît pas quand le cache flash/spiram est coupé », ce que la PSRAM ne peut
// structurellement pas satisfaire.
//
// LISTE CONSERVATRICE CONNUE ET ASSUMÉE (ce n'est pas un oubli) : plusieurs
// masques garantissent la RAM interne SANS porter INTERNAL — MALLOC_CAP_EXEC
// (1<<0), MALLOC_CAP_IRAM_8BIT (1<<13), MALLOC_CAP_RTCRAM (1<<15), et
// MALLOC_CAP_DMA seul (0x0008) sur ESP32 classique. Ils prennent le chemin
// O(régions) : `maxBloc` est perdu et le verdict retombe sur la branche « libre
// seul ». PERTE DE PRÉCISION, JAMAIS UN TROU — l'erreur est du bon côté.
//
// ⚠️ PIÈGE FUTUR — NE JAMAIS AJOUTER `DMA` À LA LISTE DES MASQUES RÉPUTÉS
// INTERNES. Ce serait correct sur ESP32 classique (PSRAM non accessible en DMA)
// et FAUX sur ESP32-S3, où la PSRAM EST DMA-capable : la sonde repartirait
// parcourir 4 Mo interruptions coupées dans le chemin d'échec. Le plan de
// migration core 3.x / S3 est ouvert dans les specs — la tentation
// d'« optimiser » viendra.
inline bool oomProbeUsesRegionScan(uint32_t caps) {
  return (caps & kOomProbeInternalCapBit) == 0u;
}

// PLANCHER TEMPOREL ABSOLU entre deux sondes (condition 3 du gate), EN PLUS du
// jeton « un échantillon par épisode ». Sans lui, le drain immédiat du backoff
// du WARN d'échec (qui démarre à 0 ms) réarmerait le jeton quelques
// millisecondes seulement après la sonde précédente : deux parcours de tas
// interruptions coupées quasi simultanés, exactement ce que le bornage
// interdit. La PREMIÈRE sonde du boot (probeCount == 0) n'est jamais bloquée —
// sinon `lastProbeMs` valant 0 ferait dépendre le tout premier échantillon de la
// date de boot.
// Arithmétique non signée : correcte au repliement de millis() (49,7 j).
inline bool oomProbeFloorElapsed(uint32_t probeCount, uint32_t nowMs,
                                 uint32_t lastProbeMs, uint32_t minIntervalMs) {
  if (probeCount == 0u) return true;
  return (uint32_t)(nowMs - lastProbeMs) >= minIntervalMs;
}

// JETON « un échantillon par ÉPISODE » — forme pure de l'exchange atomique du
// hook. Consomme le jeton et dit s'il était armé. Le RÉARMEMENT n'est JAMAIS
// fait ici : il appartient à la loop (drain du WARN d'échec, donc backoff
// immédiat → 30 s → 5 min → 30 min ; et clôture d'épisode). Côté impur, la
// consommation DOIT rester un exchange atomique — deux cœurs en échec simultané
// ne doivent pas sonder tous les deux.
//
// ⚠️ MODÈLE — cette fonction n'est appelée par AUCUN chemin du firmware. Le code
// RÉELLEMENT EXÉCUTÉ est `sProbeArmed.exchange(false)` dans
// `oom_guard.cpp:oomProbeAtFailure()`, qui ne peut pas déléguer ici sans perdre
// l'atomicité. Le suffixe `Model` est là pour qu'aucun lecteur ne croie le jeton
// couvert par les tests natifs : ils valident la RÈGLE, pas son exécution.
inline bool oomProbeTakeTokenModel(bool& tokenArmed) {
  if (!tokenArmed) return false;
  tokenArmed = false;
  return true;
}

// Marqueur « durée NON MESURABLE » du champ RTC de 16 bits. Les compteurs
// CCOUNT sont PAR CŒUR : si la tâche migre entre les deux lectures, la
// soustraction n'a aucun sens. La durée est alors marquée invalide, et
// l'autolimitation n'est PAS appliquée sur cet échantillon — une migration ne
// doit jamais désactiver la sonde à vie.
constexpr uint16_t kOomProbeDurationInvalid = 0xFFFFu;

// Saturation d'une durée mesurée au format du champ RTC. 0xFFFF étant RÉSERVÉ
// au marqueur ci-dessus, une durée RÉELLE plafonne à 0xFFFE : « très lente » et
// « non mesurable » restent discernables dans le dump comme au diagnostic. Sans
// cette réservation, une sonde de 70 ms (pathologique mais mesurée) serait
// relue comme « mesure impossible » et n'aurait déclenché aucune désactivation.
inline uint16_t oomProbeSaturateDurationUs(uint32_t durationUs) {
  return (durationUs > 0xFFFEu) ? (uint16_t)0xFFFEu : (uint16_t)durationUs;
}

// ===========================================================================
// POST-MORTEM D'ALLOCATION (bug-parachute-oom-jamais-rearme, v2.18.8)
// ===========================================================================
// Défaut n°2 du 13/08 : le CRITICAL « réserve OOM consommée » est émis en
// DIFFÉRÉ par la loop → perdu quand l'abort suit immédiatement, et le journal
// persistant d'avant crash est perdu aussi (flush toutes les 10 min). Seule la
// RAM RTC noinit survit : on y enregistre l'état AU MOMENT EXACT de l'échec
// depuis le callback heap_caps / le new_handler (stores bruts uniquement, cf.
// oom_guard.cpp), et on le dumpe au boot suivant.
//
// FORMAT DE LA ZONE (48 octets, little-endian) :
//   [0..3]   u32 magic          — kOomPostMortemMagic (0xF00DFA13)
//   [4..7]   u32 failCount      — échecs d'allocation cumulés du boot
//   [8..11]  u32 firstSize      — taille demandée au 1er échec (octets)
//   [12..15] u32 lastSize       — taille demandée au dernier échec (octets)
//   [16..19] u32 lastCaps       — masque heap_caps du dernier échec
//   [20..21] u16 firstUptimeS   — uptime (s) du 1er échec, modulo 65536
//   [22..23] u16 lastUptimeS    — uptime (s) du dernier échec, modulo 65536
//   [24..25] u16 ringFree >> 8  — heap libre 8 bits du dernier échantillon de l'anneau (quantum 256 o)
//   [26..27] u16 ringMaxB >> 6  — plus gros bloc 8 bits du dernier échantillon (quantum 64 o)
//   [28..29] u16 ringUptimeS    — uptime (s) de cet échantillon (→ âge = last - ring)
//   [30]     u8  flags          — bits kOomPmFlag* (réserve consommée, handler, intégrité, sonde)
//   [31]     u8  releaseCount   — épisodes de libération de la réserve (saturé 255)
//   [32..35] u32 probeSize      — taille refusée au moment de la SONDE (EXACTE, non quantifiée)
//   [36..37] u16 probeCapsMaxB >> 6  — plus gros bloc pour les caps DE L'ÉCHEC (quantum 64 o)
//   [38..39] u16 probeCapsFree >> 8  — libre total pour les caps de l'échec (quantum 256 o)
//   [40..41] u16 probeRefMaxB  >> 6  — plus gros bloc de RÉFÉRENCE INTERNAL|8BIT (quantum 64 o)
//   [42..43] u16 probeRefFree  >> 8  — libre total de référence INTERNAL|8BIT (quantum 256 o)
//   [44..45] u16 probeCaps           — masque heap_caps sondé, TRONQUÉ à 16 bits
//            ⚠️ La ligne 1 du dump imprime `f.lastCaps` (32 bits), la ligne 2
//            `f.probeCaps` (16 bits) : identiques sur les caps utilisées ici,
//            MAIS l'IDF 5.5 déclare déjà des bits ≥ 16 (TCM 1<<16,
//            DMA_DESC_AHB 1<<17, CACHE_ALIGNED 1<<19, SIMD 1<<20). Si l'un
//            d'eux apparaissait dans un masque, probeCaps le perdrait — et
//            oomPostMortemProbeRepresentative(), qui compare probeCaps aux
//            16 bits bas de lastCaps, resterait cohérente mais moins
//            discriminante. Un champ de 32 bits exigerait un 13ᵉ mot.
//   [46..47] u16 probeDurationUs     — durée du COUPLE des 2 parcours, saturée (0xFFFF = invalide)
// Les deux champs « ring » proviennent d'un miroir atomique de l'anneau heap
// (valeurs DÉJÀ mesurées à la cadence 2 s) : ils sont donc PÉRIMÉS de 0 à 2 s.
// Les champs « probe » (bug-080 B-2) sont au contraire mesurés AU MOMENT EXACT
// du refus, sur les caps DE L'ÉCHEC et sur une référence INTERNAL|8BIT : c'est
// la double mesure qui permet de distinguer une famine GÉNÉRALE d'une famine de
// POOL RESTREINT (DMA), indiscernables autrement.

// MAGIC BUMPÉ EN v2.19.0 (0xF00DFA11 → 0xF00DFA12) : les champs « ring » de la
// zone portent désormais la mesure 8 bits (mémoire réellement allouable). Une
// zone écrite par un firmware 2.18.x contient l'ANCIENNE métrique (IRAM 32 bits
// comprise) ; la relire avec les nouveaux libellés (« free8= », « maxB8= »)
// afficherait un chiffre faux et crédible. Perdre le post-mortem du boot de
// bascule est acceptable ; afficher une valeur trompeuse ne l'est pas.
// RE-BUMPÉ EN bug-080 B-2 (0xF00DFA12 → 0xF00DFA13) : la zone passe de 32 à
// 48 o. MÊME raisonnement, en plus grave — sans bump, les mots w8..w11 seraient
// lus dans une zone de 32 o écrite par l'ancien firmware, c'est-à-dire dans de la
// RAM RTC non initialisée : des valeurs de sonde INVENTÉES ET CRÉDIBLES. Le
// post-mortem du boot de bascule est perdu, c'est accepté.
constexpr uint32_t kOomPostMortemMagic = 0xF00DFA13u;  // DISTINCT de l'anneau (0xB10C4EAA) et de l'auto-heal (0xA5C0FFEE)
constexpr uint32_t kOomPostMortemBytes = 48;           // Taille de la zone RTC noinit
// Budget RAM RTC noinit du projet après B-2 : auto-heal 9 o + anneau heap 1032 o
// + post-mortem 48 o = 1089 o sur 8192 disponibles.
static_assert(kOomPostMortemBytes == 48 && kOomPostMortemBytes % 4 == 0,
              "La zone post-mortem doit faire 48 octets ET rester un multiple de 4 : "
              "les chemins d'echec n'y font que des stores BRUTS ALIGNES de 32 bits.");

// Drapeaux (octet 30) — posés au plus tôt pour survivre à un abort immédiat.
// ⚠️ OCTET SATURÉ (8/8 bits) depuis bug-080 B-2 : tout drapeau supplémentaire
// exigera un MOT de plus dans la zone (et donc un nouveau bump de magic).
constexpr uint8_t kOomPmFlagNominalUsed        = 0x01;  // bit0 : réserve NOMINALE consommée
constexpr uint8_t kOomPmFlagFallbackUsed       = 0x02;  // bit1 : réserve de REPLI consommée
constexpr uint8_t kOomPmFlagHandlerUnregistered = 0x04; // bit2 : new_handler désenregistré (bad_alloc laissé suivre)
constexpr uint8_t kOomPmFlagIntegrityFailed    = 0x08;  // bit3 : contrôle d'intégrité du tas ÉCHOUÉ
constexpr uint8_t kOomPmFlagIntegrityNeverRun  = 0x10;  // bit4 : contrôle d'intégrité jamais exécuté
constexpr uint8_t kOomPmFlagProbeTaken         = 0x20;  // bit5 : une sonde au refus a été prise (w8..w11 renseignés)
constexpr uint8_t kOomPmFlagProbeIsrSkipped    = 0x40;  // bit6 : au moins un refus survenu en ISR (sonde interdite)
constexpr uint8_t kOomPmFlagProbeDisabled      = 0x80;  // bit7 : sonde DÉSACTIVÉE définitivement (durée > plafond)

// Plafonds de plausibilité du décodage (RAM RTC potentiellement corrompue).
constexpr uint32_t kOomPmMaxSizeBytes = 4194304u;  // 4 Mio : au-delà, champ insensé
constexpr uint32_t kOomPmMaxFailCount = 1000000u;  // 1e6 échecs : au-delà, champ insensé

struct OomPostMortemFields {
  uint32_t magic;
  uint32_t failCount;
  uint32_t firstSize;
  uint32_t lastSize;
  uint32_t lastCaps;
  uint16_t firstUptimeS;
  uint16_t lastUptimeS;
  uint32_t ringFreeBytes;      // Reconstruit (quantum 256 o) — PÉRIMÉ de 0 à 2 s
  uint32_t ringMaxAllocBytes;  // Reconstruit (quantum 64 o) — PÉRIMÉ de 0 à 2 s
  uint16_t ringUptimeS;
  uint8_t  flags;
  uint8_t  releaseCount;
  // Sonde AU MOMENT EXACT du refus (bug-080 B-2). Valeurs à 0 tant que
  // kOomPmFlagProbeTaken n'est pas levé — ne JAMAIS les lire sans ce drapeau.
  uint32_t probeSize = 0;          // Taille refusée sondée (exacte)
  uint32_t probeCapsMaxB = 0;      // Plus gros bloc pour les caps DE L'ÉCHEC (ou kOomProbeBlockUnknown)
  uint32_t probeCapsFree = 0;      // Libre total pour les caps de l'échec
  uint32_t probeRefMaxB = 0;       // Plus gros bloc de RÉFÉRENCE INTERNAL|8BIT
  uint32_t probeRefFree = 0;       // Libre total de référence INTERNAL|8BIT
  uint16_t probeCaps = 0;          // Masque sondé (16 bits bas des MALLOC_CAP_*)
  uint16_t probeDurationUs = 0;    // Durée du COUPLE (0xFFFF = invalide/saturée)
};

// Écrit un u32 little-endian (les 12 mots de la zone sont alignés).
inline void oomPmPutU32(uint8_t* out, uint32_t v) {
  out[0] = (uint8_t)(v & 0xFFu);
  out[1] = (uint8_t)((v >> 8) & 0xFFu);
  out[2] = (uint8_t)((v >> 16) & 0xFFu);
  out[3] = (uint8_t)((v >> 24) & 0xFFu);
}

// Lit un u32 little-endian.
inline uint32_t oomPmGetU32(const uint8_t* in) {
  return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
         ((uint32_t)in[3] << 24);
}

// Empaquette un couple (plus gros bloc, libre) au format d'un mot de sonde :
// bits 0-15 = maxBloc >> 6 (quantum 64 o), bits 16-31 = libre >> 8 (quantum
// 256 o). Quanta STRICTEMENT identiques à ceux des champs « ring » : aucune
// règle de décodage neuve. La coquille impure appelle cette même fonction pour
// que le store brut du chemin d'échec et l'encodeur pur ne puissent pas diverger.
inline uint32_t oomPmPackProbeWord(uint32_t maxBlockBytes, uint32_t freeBytes) {
  uint32_t mb = maxBlockBytes >> 6;
  if (mb > 0xFFFFu) mb = 0xFFFFu;
  uint32_t fb = freeBytes >> 8;
  if (fb > 0xFFFFu) fb = 0xFFFFu;
  return mb | (fb << 16);
}

// Recopie l'octet `flags` dans le MOT 7 sans toucher aux deux autres champs
// qu'il transporte : ringUptimeS (bits 0-15) et releaseCount (bits 24-31). Forme
// PURE du store brut d'oomPostMortemFlushFlags() (oom_guard.cpp), extraite pour
// que le masque soit testable en natif — ADR-0017.
//
// POURQUOI CE FLUSH EXISTE (défaut B2, revue de B-2) : oomPostMortemStore()
// construit w7 AVANT que la sonde tourne, donc tout drapeau posé APRÈS lui
// (ProbeTaken, ProbeIsrSkipped, ProbeDisabled) n'atteindrait la RAM RTC qu'au
// refus SUIVANT — c'est-à-dire JAMAIS si un abort suit, précisément le scénario
// que le post-mortem sert à qualifier. La zone contiendrait alors w8..w11
// renseignés et un `flags` sans ProbeTaken : sonde déclarée « absente », verdict
// retombé en INDÉTERMINÉ, ligne 2 non émise. La mesure serait écrite, puis
// silencieusement jetée.
//
// Un masque erroné est tout aussi destructeur qu'un flush absent : écraser les
// bits 0-15 effacerait l'uptime de l'échantillon d'anneau (donc son âge), et
// écraser les bits 24-31 remettrait le compteur d'épisodes à zéro.
inline uint32_t oomPmPackFlagsIntoW7(uint32_t w7, uint8_t flags) {
  return (w7 & 0xFF00FFFFu) | ((uint32_t)flags << 16);
}

// Encode la zone dans `out` (kOomPostMortemBytes octets). Saturations : tailles
// et compteurs plafonnés à leur dynamique, uptimes tronqués modulo 65536.
inline void oomPostMortemEncode(uint8_t* out, const OomPostMortemFields& f) {
  uint32_t rf = f.ringFreeBytes >> 8;
  if (rf > 0xFFFFu) rf = 0xFFFFu;
  uint32_t rm = f.ringMaxAllocBytes >> 6;
  if (rm > 0xFFFFu) rm = 0xFFFFu;
  const uint32_t words[] = {f.magic, f.failCount, f.firstSize, f.lastSize, f.lastCaps};
  for (uint32_t w = 0; w < 5u; w++) {
    oomPmPutU32(out + w * 4, words[w]);
  }
  oomPmPutU32(out + 32, f.probeSize);
  oomPmPutU32(out + 36, oomPmPackProbeWord(f.probeCapsMaxB, f.probeCapsFree));
  oomPmPutU32(out + 40, oomPmPackProbeWord(f.probeRefMaxB, f.probeRefFree));
  oomPmPutU32(out + 44, (uint32_t)f.probeCaps | ((uint32_t)f.probeDurationUs << 16));
  out[20] = (uint8_t)(f.firstUptimeS & 0xFFu);
  out[21] = (uint8_t)(f.firstUptimeS >> 8);
  out[22] = (uint8_t)(f.lastUptimeS & 0xFFu);
  out[23] = (uint8_t)(f.lastUptimeS >> 8);
  out[24] = (uint8_t)(rf & 0xFFu);
  out[25] = (uint8_t)((rf >> 8) & 0xFFu);
  out[26] = (uint8_t)(rm & 0xFFu);
  out[27] = (uint8_t)((rm >> 8) & 0xFFu);
  out[28] = (uint8_t)(f.ringUptimeS & 0xFFu);
  out[29] = (uint8_t)(f.ringUptimeS >> 8);
  out[30] = f.flags;
  out[31] = f.releaseCount;
}

// Décode la zone depuis `in` (kOomPostMortemBytes octets). Aucune validation
// ici : voir oomPostMortemValid().
inline OomPostMortemFields oomPostMortemDecode(const uint8_t* in) {
  OomPostMortemFields f;
  f.magic = oomPmGetU32(in + 0);
  f.failCount = oomPmGetU32(in + 4);
  f.firstSize = oomPmGetU32(in + 8);
  f.lastSize = oomPmGetU32(in + 12);
  f.lastCaps = oomPmGetU32(in + 16);
  f.probeSize = oomPmGetU32(in + 32);
  uint32_t wCaps = oomPmGetU32(in + 36);
  f.probeCapsMaxB = (wCaps & 0xFFFFu) << 6;
  f.probeCapsFree = (wCaps >> 16) << 8;
  uint32_t wRef = oomPmGetU32(in + 40);
  f.probeRefMaxB = (wRef & 0xFFFFu) << 6;
  f.probeRefFree = (wRef >> 16) << 8;
  uint32_t wMeta = oomPmGetU32(in + 44);
  f.probeCaps = (uint16_t)(wMeta & 0xFFFFu);
  f.probeDurationUs = (uint16_t)(wMeta >> 16);
  f.firstUptimeS = (uint16_t)(in[20] | ((uint16_t)in[21] << 8));
  f.lastUptimeS = (uint16_t)(in[22] | ((uint16_t)in[23] << 8));
  f.ringFreeBytes = ((uint32_t)(in[24] | ((uint32_t)in[25] << 8))) << 8;
  f.ringMaxAllocBytes = ((uint32_t)(in[26] | ((uint32_t)in[27] << 8))) << 6;
  f.ringUptimeS = (uint16_t)(in[28] | ((uint16_t)in[29] << 8));
  f.flags = in[30];
  f.releaseCount = in[31];
  return f;
}

// Plausibilité de la zone décodée (la RAM RTC n'est pas initialisée au power-on
// et peut être corrompue) : magic EXACT, uptimes cohérents (le dernier échec ne
// précède pas le premier), tailles ≤ 4 Mio, compteur d'échecs ≤ 1e6.
inline bool oomPostMortemValid(const OomPostMortemFields& f) {
  if (f.magic != kOomPostMortemMagic) return false;
  // PAS de contrôle « lastUptimeS >= firstUptimeS » (revue v2.18.8) : les
  // uptimes sont tronqués sur 16 bits (repliement toutes les 18 h 12), or
  // l'appareil tourne des JOURS. Un premier échec bénin situé avant un
  // repliement rendrait la zone « invalide » et le post-mortem ne serait
  // JAMAIS émis — précisément au moment où il sert. Les durées affichées
  // utilisent l'arithmétique modulo (oomPostMortemSpanS), qui reste correcte.
  if (f.firstSize > kOomPmMaxSizeBytes || f.lastSize > kOomPmMaxSizeBytes) return false;
  if (f.failCount > kOomPmMaxFailCount) return false;
  return true;
}

// Durée écoulée entre le PREMIER et le DERNIER échec, en secondes, en
// arithmétique modulo 16 bits (les uptimes se replient toutes les 18 h 12).
inline uint16_t oomPostMortemSpanS(const OomPostMortemFields& f) {
  return (uint16_t)(f.lastUptimeS - f.firstUptimeS);
}

// Verdict d'attribution du crash.
enum : uint8_t {
  kOomVerdictUndetermined = 0,         // Pas assez d'éléments (aucun échec, pas de mesure exploitable)
  kOomVerdictStarvation = 1,           // Famine mémoire réelle (le tas était bien à sec)
  kOomVerdictCorruptionSuspected = 2,  // Un bloc suffisant existait AU REFUS → corruption suspectée
  kOomVerdictDmaStarvation = 3,        // Famine d'un POOL RESTREINT (DMA) alors que le tas 8 bits reste sain
};

// true si la sonde du refus est exploitable pour rendre le verdict : elle a été
// prise, et elle porte sur la MÊME CLASSE D'ÉCHEC que le dernier échec
// enregistré (même taille demandée, mêmes capacités). Sans cette égalité, la
// sonde décrit un autre refus que celui qu'on cherche à qualifier — le jeton
// « un échantillon par épisode » rend ce cas parfaitement possible.
inline bool oomPostMortemProbeRepresentative(const OomPostMortemFields& f) {
  return (f.flags & kOomPmFlagProbeTaken) != 0 && f.probeSize == f.lastSize &&
         (uint32_t)f.probeCaps == (f.lastCaps & 0xFFFFu);
}

// Départage famine générale (a), corruption (b) et famine de pool restreint (c)
// à partir de la zone décodée.
//
// RÈGLE, DANS CET ORDRE (révisée par bug-080 B-2) :
//  1. aucun échec exploitable → INDÉTERMINÉ ;
//  2. sonde REPRÉSENTATIVE → elle fait AUTORITÉ, l'anneau est IGNORÉ (il est
//     périmé de 0 à 2 s et mesuré sur des caps fixes) :
//       - plus gros bloc « non renseigné » (sonde O(régions), caps SPIRAM) :
//         on tranche sur le LIBRE seul, et à défaut on s'abstient ;
//       - maxBloc(caps de l'échec) < taille demandée → l'allocateur disait vrai :
//           * la RÉFÉRENCE INTERNAL|8BIT reste saine (≥ ratio × taille) →
//             FAMINE DMA (pool restreint) — cas des 6 échecs du 05/09/2026,
//             1 622 o refusés en caps=0x080C alors que le tas 8 bits annonçait
//             26 612 o : ce n'était NI une corruption NI une famine générale ;
//           * sinon → FAMINE MÉMOIRE (tout le tas est à sec) ;
//       - maxBloc(caps de l'échec) ≥ taille demandée → un bloc EXISTAIT à
//         l'instant du refus → SUSPICION CORRUPTION. C'est désormais le SEUL
//         chemin vers ce verdict, et il repose sur une mesure CONTEMPORAINE.
//  3. sonde absente / non représentative → règle historique de la v2.18.8, mais
//     AMPUTÉE de sa conclusion : elle ne conclut plus jamais « corruption » sur
//     une donnée vieille de 2 s ; l'indétermination est préférée, et le libellé
//     du dump dit POURQUOI (sonde absente / ISR / désactivée).
inline uint8_t oomPostMortemVerdict(const OomPostMortemFields& f, uint32_t maxFails,
                                    uint32_t saneBlockRatio, uint16_t burstSpanS) {
  if (f.failCount == 0 || f.lastSize == 0) return kOomVerdictUndetermined;

  if (oomPostMortemProbeRepresentative(f)) {
    if (f.probeCapsMaxB == kOomProbeBlockUnknown) {
      // Sonde O(régions) : le plus gros bloc n'a pas été parcouru (borne a
      // priori du pire cas PSRAM). Le libre total reste un indice honnête.
      return (f.probeCapsFree < f.probeSize) ? kOomVerdictStarvation
                                             : kOomVerdictUndetermined;
    }
    if (f.probeCapsMaxB < f.probeSize) {
      // Division plutôt que multiplication : pas de débordement sur une taille géante.
      bool refSane = (saneBlockRatio == 0) ||
                     (f.probeRefMaxB / saneBlockRatio >= f.probeSize);
      return refSane ? kOomVerdictDmaStarvation : kOomVerdictStarvation;
    }
    return kOomVerdictCorruptionSuspected;
  }

  // Aucun échantillon d'anneau (boot très précoce) : rien à confronter.
  if (f.ringMaxAllocBytes == 0 && f.ringFreeBytes == 0) return kOomVerdictUndetermined;
  // Famine sur le NOMBRE d'échecs : uniquement s'ils forment une RAFALE (revue
  // v2.18.8). Le compteur est cumulé sur tout le boot : sans fenêtre, quelques
  // échecs bénins étalés sur des heures concluraient « famine » sur le scénario
  // même que ce verdict doit trancher (13/08 : 2 échecs, tas sain).
  if (f.failCount > maxFails && oomPostMortemSpanS(f) <= burstSpanS) {
    return kOomVerdictStarvation;
  }
  // Division plutôt que multiplication : pas de débordement sur lastSize géant.
  bool saneAccounting = (saneBlockRatio == 0) ||
                        (f.ringMaxAllocBytes / saneBlockRatio >= f.lastSize);
  if (!saneAccounting) return kOomVerdictStarvation;
  return kOomVerdictUndetermined;
}

// ---------------------------------------------------------------------------
// Contrôle d'intégrité périodique du tas — cadence + autolimitation (C3)
// ---------------------------------------------------------------------------
// Cadence lente : premier passage dès que l'appelant est éligible (`everRun`
// faux), puis toutes les `intervalMs`. `enabled` porte la désactivation
// définitive (durée excessive ou corruption déjà détectée).
inline bool heapIntegrityShouldRun(bool enabled, bool everRun, uint32_t nowMs,
                                   uint32_t lastRunMs, uint32_t intervalMs) {
  if (!enabled) return false;
  if (!everRun) return true;
  return (uint32_t)(nowMs - lastRunMs) >= intervalMs;
}

// Autolimitation : le contrôle n'est réadmis que si son parcours est resté sous
// le plafond (kHeapIntegrityMaxDurationUs). Un tas devenu si gros/fragmenté que
// le parcours dure plus longtemps ne doit PAS être re-parcouru (gigue watchdog
// et régulation) — désactivation définitive côté appelant.
inline bool heapIntegrityAdmitNextRun(uint32_t durationUs, uint32_t maxDurationUs) {
  return durationUs <= maxDurationUs;
}

// Motifs de refus de la garde TLS sortante (retour de oomTlsGateReason).
enum : uint8_t {
  kOomTlsGateOk = 0,        // Session TLS autorisée
  kOomTlsGateNoPsram = 1,   // PSRAM libre absente/insuffisante (module sans PSRAM
                            // ou PSRAM saturée) → refus matériel
  kOomTlsGateLowMemory = 2, // PSRAM libre suffisante mais heap momentanément dégradé → réessayable
};

// Prédicat de garde avant TOUTE session TLS sortante (partagé : /check-update,
// /download-update). Renvoie un MOTIF (pas un booléen) : l'appelant doit
// distinguer un refus définitif (matériel) d'un refus temporaire (mémoire).
//
// 1) PSRAM LIBRE d'abord, FAIL-CLOSED (bug-garde-tls-trop-permissive-sans-psram) :
//    mbedTLS réclame plusieurs blocs contigus d'environ 16 Ko. Sans PSRAM, le
//    régime NORMAL d'un WROOM après quelques minutes (mesuré en v2.18.5 :
//    free=87708 o, maxBloc=34804 o) PASSAIT la garde bicritère puis affamait le
//    tas jusqu'à refuser une allocation de 100 o pendant le handshake — la
//    fenêtre exacte du crash bad_alloc du 11/08/2026. Relever le seuil maxBloc
//    (48-50 Ko) ne serait pas une protection : le parachute OOM réserve 12 Ko,
//    donc le maxBloc nominal d'un WROOM tombe de ~49 à ~37 Ko ; un tel seuil ne
//    passerait QUE parachute DÉSARMÉ, c'est-à-dire précisément quand le filet
//    anti-bad_alloc est absent. D'où : PSRAM requise, sinon upload manuel.
//    Le critère porte sur la PSRAM LIBRE (`psramFreeBytes`), pas sur la taille
//    installée : cette dernière est constante et ne décroît jamais, donc un
//    module PSRAM saturé (tout consommé par SPIRAM_USE_MALLOC) ouvrirait quand
//    même la garde alors que mbedTLS retomberait sur la RAM interne — le régime
//    de crash exact. Sur un module de 4 Mo, exiger 1 Mo LIBRE couvre à la fois
//    l'absence de PSRAM et sa saturation.
// 2) Ensuite seulement, le bicritère heap historique (ADR-0032) : heap TOTAL
//    suffisant ET plus gros bloc contigu décent. Seuils INCLUSIFS (>=).
inline uint8_t oomTlsGateReason(uint32_t psramFreeBytes, uint32_t freeHeapBytes,
                                uint32_t maxAllocBytes, uint32_t minPsramBytes,
                                uint32_t minFreeBytes, uint32_t minMaxAllocBytes) {
  if (psramFreeBytes < minPsramBytes) return kOomTlsGateNoPsram;
  if (freeHeapBytes < minFreeBytes || maxAllocBytes < minMaxAllocBytes) {
    return kOomTlsGateLowMemory;
  }
  return kOomTlsGateOk;
}

#endif  // OOM_GUARD_LOGIC_H
