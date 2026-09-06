#ifndef FS_GUARD_H
#define FS_GUARD_H

// =============================================================================
// fs_guard — POINT D'ADMISSION UNIQUE de tout fopen interne (bug-100, ADR-0070)
// =============================================================================
// Tout `fopen` que le firmware exécute POUR SON PROPRE COMPTE passe par ici.
// C'est le seul fichier de `src/` autorisé à appeler `fs::FS::open()` — le
// contrôle `tools/check_fopen_guard.sh` échoue si un autre `.cpp` le fait.
// Rendre le défaut INEXPRIMABLE plutôt qu'untesté : une garde par site est
// explicitement rejetée par l'ADR, c'est le mode de défaillance du lot B-3
// (un chemin fermé, 14 sites laissés ouverts).
//
// ⚠️ `exists()` COMPTE COMME UNE OUVERTURE. `VFSImpl::exists()` du framework ne
// fait PAS de `stat()` : il construit un `VFSFileImpl(this, fpath, "r")`, donc
// un fopen complet, verrou newlib compris. Aucun `grep "open("` ne trouve ce
// site — c'est la leçon la plus transférable du cycle 1.
//
// ⚠️ AUCUN LOG ICI, à aucun niveau. Ce chemin existe pour éviter `fopen` ; un
// ERROR/CRITICAL y déclencherait le flush synchrone de logger.cpp:71-73 →
// open("/system.log","a") → l'abort() même que l'on prévient. L'observabilité
// DURABLE est le COMPTEUR, pas la ligne de journal : sous famine, un WARN reste
// de toute façon prisonnier du _persistBuffer. Le drain de la loop journalise,
// le helper non.
// =============================================================================

#include <FS.h>
#include <stdint.h>

/**
 * @brief Seuil « admission déjà prononcée en amont ».
 *
 * À passer quand la décision a déjà été prise, sur une mesure fraîche, à un
 * point où l'opération pouvait encore NE RIEN FAIRE — typiquement la tête de
 * `HistoryManager::saveToFile()` (l'opération n'est pas atomique : le `remove()`
 * précède l'`open("w")`, une garde sur l'ouverture échangerait l'abort() contre
 * la DESTRUCTION du fichier d'historique) et l'entrée de `Logger::flushToDisk()`
 * (une mesure pour 7 ouvertures, jamais sept). Aucune mesure n'est alors prise.
 */
constexpr uint32_t kFsGuardAlreadyAdmitted = 0;

/**
 * @brief Décide seule, sans ouvrir : l'appelant a-t-il le droit de toucher au FS ?
 *
 * À utiliser aux points d'ADMISSION d'une opération non atomique, avant tout
 * effet de bord. Mesure `heapLargestFreeBlock8Bit()` AU MOMENT DE LA DÉCISION
 * (ADR-0058:99) — jamais `ESP.getFreeHeap()`, jamais `ESP.getMaxAllocHeap()`
 * (IRAM 32 bits comprise, aveugles à la famine 8 bits), jamais le miroir
 * atomique de `oomGuardLastMaxAlloc8()` (rejeté nommément par ADR-0058:79 :
 * des effondrements de ~45 Ko ont été observés entre deux échantillons de 2 s).
 *
 * Sans log, sans allocation. Appelable depuis n'importe quelle tâche.
 *
 * APPEL SOUS MUTEX TENU : autorisé, et c'est un constat mesuré, pas une
 * tolérance. `HistoryManager::saveToFile()` est appelée avec `_mutex` tenu
 * depuis `consolidateData()` (chemin des 5 min) ET depuis `importData()` — ce
 * n'est donc pas un cas de bord évitable. Trois raisons rendent l'appel sûr :
 *   1. ORDRES DE GRANDEUR — un parcours TLSF coûte ~10² µs ; l'écriture
 *      LittleFS déjà faite sous ce même mutex coûte 10¹-10² ms. Deux à trois
 *      ordres de grandeur d'écart : la mesure ne change pas le profil de
 *      contention du verrou.
 *   2. NATURE DU COÛT — le coût réel est « interruptions coupées »
 *      (heap_metrics.h), et il est IDENTIQUE avec ou sans mutex tenu. Relâcher
 *      le mutex n'en économiserait pas une microseconde.
 *   3. PAS D'INVERSION POSSIBLE — le verrou du tas est terminal : rien n'est
 *      pris en le tenant, aucun cycle d'attente ne peut se former.
 * Ce qui reste vrai : ne pas appeler depuis une ISR, ni depuis le chemin
 * d'échec d'allocation (cf. l'interdit de heap_metrics.h, qui y réserve
 * l'entonnoir borné d'oom_guard.cpp).
 *
 * @param minAllocBytes seuil de la classe de contention (8 192 = classe A,
 *                      kInternalFopenMinAllocBytes = 5 120 = classe B), ou
 *                      kFsGuardAlreadyAdmitted.
 * @return true si l'opération est admise.
 */
bool fsGuardAdmit(uint32_t minAllocBytes);

/**
 * @brief Ouvre un fichier SI et seulement si le tas 8 bits le permet.
 * @param fs            système de fichiers (jamais nullptr en pratique ; un
 *                      nullptr renvoie un `File` invalide sans rien tenter)
 * @param path          chemin absolu
 * @param mode          "r", "w", "a"…
 * @param minAllocBytes cf. fsGuardAdmit()
 * @return le `File` ouvert, ou un `File` invalide (opérateur bool à false) si
 *         l'ouverture a été refusée ou a échoué. Le refus est COMPTÉ, jamais
 *         journalisé ici.
 */
fs::File openIfHeapAllows(fs::FS* fs, const char* path, const char* mode,
                          uint32_t minAllocBytes);

/**
 * @brief Équivalent gardé de `fs->exists()` — qui OUVRE le fichier (cf. entête).
 * @return false si le fichier est absent OU si l'ouverture a été refusée. Un
 *         appelant qui doit distinguer les deux cas utilise `fsGuardAdmit()`
 *         explicitement avant.
 */
bool existsIfHeapAllows(fs::FS* fs, const char* path, uint32_t minAllocBytes);

/**
 * @brief Drain de l'épisode de famine — exécuté depuis la LOOP uniquement.
 *
 * Quelques lignes par épisode, jamais une par refus : un journal persistant de
 * 8 Ko a 25-35 min de rétention, journaliser par refus effacerait la fenêtre
 * forensique de l'épisode que l'on cherche à documenter. WARN/INFO seulement
 * pendant l'épisode ; le `critical()` est DIFFÉRÉ au front descendant, quand le
 * flush synchrone peut à nouveau atteindre le disque.
 */
void fsGuardDrain();

/** @brief Refus cumulés depuis le boot, tous sites confondus (diagnostic). */
uint32_t fsGuardDeferralCount();
/** @brief Refus servis SANS mesure (cache du refus) depuis le boot. */
uint32_t fsGuardCachedDeferralCount();
/** @brief maxBloc8 relevé au dernier refus mesuré (0 si aucun).
 *  ⚠️ Ne rien afficher de cette valeur sans consulter
 *  fsGuardLastRefusalMeasured() : un refus servi par le cache ne mesure rien. */
uint32_t fsGuardLastMaxAlloc8();

/** @brief Le dernier refus a-t-il été pris sur une mesure FRAÎCHE (true) ou
 *  servi par le cache du refus (false) ? Conditionne l'honnêteté de tout
 *  libellé qui cite fsGuardLastMaxAlloc8(). */
bool fsGuardLastRefusalMeasured();

#endif  // FS_GUARD_H
