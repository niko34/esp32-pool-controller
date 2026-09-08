#include "fs_guard.h"

#include <Arduino.h>
#include <atomic>

#include "constants.h"
#include "fopen_guard_logic.h"
#include "heap_metrics.h"
#include "logger.h"
#include "mqtt_manager.h"

// =============================================================================
// fs_guard — coquille IMPURE du point d'admission unique (bug-100, ADR-0070)
// =============================================================================
// Elle ne fait que trois choses : MESURER (heapLargestFreeBlock8Bit), COMPTER
// (trois atomiques relaxés, modèle sStaticRefusalCount de web_server.cpp:29) et
// OUVRIR. Toute la décision vit dans fopen_guard_logic (module pur ADR-0017).
//
// AUCUN mutex, AUCUNE tâche neuve : le drain se greffe sur la loop existante.
// L'état d'épisode est un simple struct partagé, sans verrou, et c'est SÛR par
// construction — une lecture périmée ne peut produire qu'un REFUS périmé (coût :
// une sauvegarde différée de 5 min), jamais une AUTORISATION périmée (coût : un
// abort()). Toute autorisation traverse une mesure fraîche.
// =============================================================================

namespace {

// Les trois compteurs du helper. `relaxed` : ce sont des compteurs de
// diagnostic, aucune donnée n'est publiée à travers eux.
std::atomic<uint32_t> sFopenDeferrals{0};        // refus cumulés, tous sites
std::atomic<uint32_t> sFopenCachedDeferrals{0};  // dont refus servis SANS mesure
std::atomic<uint32_t> sFopenLastMaxAlloc8{0};    // maxBloc8 du dernier refus mesuré
// Le dernier refus a-t-il été MESURÉ, ou servi par le cache ? Sans ce drapeau,
// sFopenLastMaxAlloc8 serait affiché comme s'il datait du refus courant alors
// qu'un refus en cache ne mesure rien (asymétrie ADR-0070 §e) : un chiffre
// périmé présenté comme frais est pire qu'une absence de chiffre.
std::atomic<bool> sFopenLastRefusalMeasured{false};

// État d'épisode : écrit au point de décision (n'importe quelle tâche) et par le
// drain (loop). Cf. le commentaire de tête sur l'absence de verrou.
FopenGuardEpisode sFopenEpisode;

}  // namespace

bool fsGuardAdmit(uint32_t minAllocBytes) {
  // Admission déjà prononcée en amont (tête de saveToFile, entrée de
  // flushToDisk) : ne rien mesurer, ne rien compter.
  if (minAllocBytes == kFsGuardAlreadyAdmitted) return true;

  const uint32_t now = millis();

  // Asymétrie mesure/décision (ADR-0070 §e) : si la logique pure dit qu'une
  // mesure fraîche n'est pas due, c'est qu'un épisode est ouvert — on refuse
  // SANS parcourir le tas. Strictement conservateur.
  if (!fopenGuardNeedsFreshMeasure(sFopenEpisode, now, kInternalFopenEpisodeQuietMs,
                                   kInternalFopenProbeMinIntervalMs)) {
    sFopenDeferrals.fetch_add(1, std::memory_order_relaxed);
    sFopenCachedDeferrals.fetch_add(1, std::memory_order_relaxed);
    sFopenLastRefusalMeasured.store(false, std::memory_order_relaxed);
    sFopenEpisode.lastDeferralMs = now;
    return false;
  }

  // Mesure AU MOMENT DE LA DÉCISION, en métrique 8 bits : l'allocation qui
  // aborte demande caps = 0x804 = MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL, soit
  // exactement heapCaps8BitMask(). L'exigence d'ADR-0058 est ici juste AU BIT
  // PRÈS, pas stylistique.
  const uint32_t maxAlloc8 = heapLargestFreeBlock8Bit();
  // feature-122 (A1) : valeur DÉJÀ mesurée pour la décision → poussée vers le
  // plancher glissant 0x0804 (fetch_min, aucun parcours neuf). Ce site s'exécute
  // depuis n'importe quelle tâche, `async_tcp` comprise : aucun log, aucune
  // allocation. Le seuil et la métrique de la garde restent INCHANGÉS.
  heapNoteMaxAlloc8Sample(maxAlloc8);
  sFopenEpisode.lastProbeMs = now;
  // Le cache du refus n'est armé que dans la zone de danger. Au-dessus de
  // 2 × seuil, chaque décision re-mesure : le cache ne peut alors produire aucun
  // refus périmé en régime normal.
  // ⚠️ L'armement est ici relatif au seuil DE LA CLASSE OUVRANTE
  // (`minAllocBytes`), alors que le drain l'évalue sur `kStaticFileMinAllocBytes`.
  // C'EST VOLONTAIRE, ne pas « aligner » : au point de décision on connaît la
  // classe et on la respecte ; le drain, lui, ne sait pas quelle classe a ouvert
  // l'épisode et prend donc la PLUS STRICTE des deux. Les deux évaluations vont
  // dans le sens conservateur, chacune avec l'information dont elle dispose.
  sFopenEpisode.probeFloorArmed = (maxAlloc8 < 2u * minAllocBytes);

  if (fopenGuardShouldDefer(maxAlloc8, minAllocBytes)) {
    sFopenDeferrals.fetch_add(1, std::memory_order_relaxed);
    sFopenLastMaxAlloc8.store(maxAlloc8, std::memory_order_relaxed);
    sFopenLastRefusalMeasured.store(true, std::memory_order_relaxed);
    sFopenEpisode.active = true;
    sFopenEpisode.lastDeferralMs = now;
    return false;
  }
  return true;
}

fs::File openIfHeapAllows(fs::FS* fs, const char* path, const char* mode,
                          uint32_t minAllocBytes) {
  if (!fs) return fs::File();
  if (!fsGuardAdmit(minAllocBytes)) return fs::File();
  // L'UNIQUE appel à fs::FS::open() de tout src/ (cf. tools/check_fopen_guard.sh).
  return fs->open(path, mode);
}

bool existsIfHeapAllows(fs::FS* fs, const char* path, uint32_t minAllocBytes) {
  // On n'appelle PAS fs->exists() : il construit un VFSFileImpl, donc un fopen
  // non gardé. Ouvrir en lecture par le helper est strictement équivalent, et
  // rend le coût visible là où il est payé.
  fs::File f = openIfHeapAllows(fs, path, "r", minAllocBytes);
  if (!f) return false;
  f.close();
  return true;
}

uint32_t fsGuardDeferralCount() {
  return sFopenDeferrals.load(std::memory_order_relaxed);
}

uint32_t fsGuardCachedDeferralCount() {
  return sFopenCachedDeferrals.load(std::memory_order_relaxed);
}

uint32_t fsGuardLastMaxAlloc8() {
  return sFopenLastMaxAlloc8.load(std::memory_order_relaxed);
}

bool fsGuardLastRefusalMeasured() {
  return sFopenLastRefusalMeasured.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Drain de l'épisode — LOOP UNIQUEMENT (main.cpp, après systemLogger.update())
// ---------------------------------------------------------------------------
// NIVEAUX : WARN pendant l'épisode, INFO/CRITICAL au front descendant. Un
// ERROR/CRITICAL pendant l'épisode déclencherait le flushToDisk() synchrone de
// logger.cpp:71-73 → fopen() → l'abort() prévenu. Le critical() est donc
// DIFFÉRÉ à la sortie, quand le flush peut de nouveau atteindre le disque : on
// ne renonce ni à l'audibilité ni à la durabilité, on les désynchronise.
void fsGuardDrain() {
  const uint32_t now = millis();
  const uint32_t deferrals = sFopenDeferrals.load(std::memory_order_relaxed);

  // Le tas n'est parcouru QUE si la fermeture d'un épisode est réellement en
  // jeu : hors épisode il n'y a rien à décider ici (la décision se prend au
  // point d'admission), et pendant l'épisode la logique pure impose le plancher
  // de sonde. Sans ce double garde-fou, la loop walkerait le tas à sa propre
  // fréquence — au pire moment possible.
  uint32_t maxAlloc8 = 0;
  if (sFopenEpisode.active &&
      fopenGuardNeedsFreshMeasure(sFopenEpisode, now, kInternalFopenEpisodeQuietMs,
                                  kInternalFopenProbeMinIntervalMs)) {
    maxAlloc8 = heapLargestFreeBlock8Bit();
    heapNoteMaxAlloc8Sample(maxAlloc8);  // feature-122 (A1) : plancher, sans parcours neuf
    sFopenEpisode.lastProbeMs = now;
    // Seuil de la classe la PLUS STRICTE : le drain ignore quelle classe a
    // ouvert l'épisode (cf. la note appariée dans fsGuardAdmit). Divergence
    // volontaire, conservatrice des deux côtés.
    sFopenEpisode.probeFloorArmed = (maxAlloc8 < 2u * kStaticFileMinAllocBytes);
  }

  const FopenGuardAction action =
    fopenGuardDrain(sFopenEpisode, deferrals, now, maxAlloc8,
                    kStaticFileMinAllocBytes, kInternalFopenEscalateAfterRefusals);
  if (action == FopenGuardAction::None) return;

  // Tampon de PILE, un seul snprintf par branche : la console doit rester
  // fonctionnelle sous heap critique, et la partition applicative n'a pas à
  // payer trois formats dupliqués.
  char ligne[128];
  switch (action) {
    case FopenGuardAction::EpisodeStart:
      snprintf(ligne, sizeof(ligne),
               "Ouvertures de fichiers internes suspendues — maxBloc8=%u o",
               (unsigned)sFopenLastMaxAlloc8.load(std::memory_order_relaxed));
      systemLogger.warning(ligne);  // JAMAIS error() : flush disque synchrone → fopen()
      break;

    case FopenGuardAction::Escalate:
      snprintf(ligne, sizeof(ligne),
               "%u ouvertures de fichiers refusees (famine memoire, maxBloc8=%u o)",
               (unsigned)(deferrals - sFopenEpisode.deferralsAtStart),
               (unsigned)sFopenLastMaxAlloc8.load(std::memory_order_relaxed));
      // publishAlert passe par enqueueOutbound et se termine par un warning() —
      // jamais error/critical (mqtt_manager.cpp). Les compteurs, eux, partent
      // dans le payload RETENU <base>/diagnostic : c'est l'observabilité
      // durable, la ligne de journal reste prisonnière du _persistBuffer tant
      // que la famine dure.
      mqttManager.publishAlert("fopen_defer", ligne);
      break;

    case FopenGuardAction::EpisodeEnd:
      snprintf(ligne, sizeof(ligne),
               "Ouvertures de fichiers retablies — %u refus en %u s",
               (unsigned)(deferrals - sFopenEpisode.deferralsAtStart),
               (unsigned)((now - sFopenEpisode.episodeStartMs) / 1000UL));
      // Front descendant SEULEMENT : le tas vient d'être mesuré au-dessus du
      // seuil, le flush synchrone du critical() peut atteindre le disque.
      if (sFopenEpisode.escalated) {
        systemLogger.critical(ligne);
      } else {
        systemLogger.info(ligne);
      }
      break;

    case FopenGuardAction::None:
      break;
  }
}
