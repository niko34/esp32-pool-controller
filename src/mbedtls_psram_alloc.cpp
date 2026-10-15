#include "mbedtls_psram_alloc.h"

#include <atomic>
#include <cstdlib>             // calloc()/free() de la libc — déjà PSRAM-aware
#include <esp_memory_utils.h>  // esp_ptr_external_ram (témoin, même idiome que heap_metrics.h)

#include <mbedtls/platform.h>  // mbedtls_platform_set_calloc_free, mbedtls_calloc/free

namespace {
// État observable (kMbedtlsPsramAlloc*), lu par /get-system-info.
std::atomic<uint8_t> gState{kMbedtlsPsramAllocAbsent};
// Garde de non-réentrance : POSE UNIQUE (voir .h). static à ce fichier, jamais
// partagée — simple bool suffit, écrit exclusivement par mbedtlsPsramAllocInstall()
// elle-même, appelée une seule fois depuis main.cpp avant tout multitâche réseau.
bool gInstallAttempted = false;
}  // namespace

bool mbedtlsPsramAllocInstall() {
  if (gInstallAttempted) {
    // Second appel (ne devrait pas arriver, cf. contrat .h) : no-op, renvoie
    // l'état déjà établi plutôt que de reposer le crochet ou de refaire le
    // témoin — une seconde pose écraserait un état déjà vérifié sans rien
    // apprendre de plus.
    return gState.load() == kMbedtlsPsramAllocActive;
  }
  gInstallAttempted = true;

  // ⚠️ Condition C1 : VÉRIFIER la valeur de retour, ne pas la supposer nulle.
  const int ret = mbedtls_platform_set_calloc_free(calloc, free);
  if (ret != 0) {
    // Pose refusée par mbedtls — ne devrait jamais survenir avec la config
    // des libs pioarduino (MBEDTLS_PLATFORM_MEMORY actif, pas de macro figée,
    // vérifié par lecture de esp_config.h). mbedtls garde alors SON
    // allocateur d'origine (RAM interne, cause racine inchangée).
    gState.store(kMbedtlsPsramAllocAbsent);
    return false;
  }

  // TÉMOIN ANTI-DÉCORATIF (condition C2) : allouer VIA LE POINTEUR MBEDTLS
  // (mbedtls_calloc), JAMAIS via calloc() directement — sinon le témoin ne
  // prouverait que la politique d'allocation du projet, pas que le CROCHET
  // mbedtls l'emprunte réellement. 2 048 o > kPsramExtmemThresholdBytes
  // (1 024 o) : classe ATTENDUE en PSRAM par la politique posée dans
  // main.cpp. Libéré IMMÉDIATEMENT (aucune rétention, aucun risque).
  void* probe = mbedtls_calloc(1, 2048);
  bool active = false;
  if (probe != nullptr) {
    active = esp_ptr_external_ram(probe);
    mbedtls_free(probe);
  }
  // probe == nullptr (allocation du témoin elle-même refusée) : traité comme
  // décoratif, PAS comme « absent » — le crochet EST posé (ret == 0), mais son
  // effet n'a pas pu être confirmé. Mieux vaut un faux « décoratif » visible
  // qu'un « absent » qui masquerait une pose réussie mais non vérifiée.
  // Classification déléguée au verdict PUR (mbedtls_psram_alloc.h), testé
  // nativement par mutation (test_native_oom_guard, bug-165/bug-108 T4) : ce
  // fichier ne fait plus que PRODUIRE les deux booléens ci-dessus.
  const uint8_t verdict = mbedtlsPsramAllocVerdict(/*hookInstalled=*/true, active);
  gState.store(verdict);
  return verdict == kMbedtlsPsramAllocActive;
}

uint8_t mbedtlsPsramAllocatorState() {
  return gState.load();
}
