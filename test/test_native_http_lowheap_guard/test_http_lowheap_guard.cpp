// =============================================================================
// Tests unitaires natifs — Garde 503 « fichiers statiques » (bug-080 lot B-3)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// Cible : src/http_lowheap_logic.h (header pur, ADR-0017).
//
// Ce que la garde ferme : servir une page statique passe par fopen(), qui
// demande à la libc un verrou (~88 o) ; si ce malloc échoue, lock_init_generic
// **aborte** — aucun code de retour à tester. La seule défense est de ne PAS
// entrer dans fopen(). D'où un prédicat de refus, et un bornage du journal qui
// ne doit surtout pas écrire une ligne par requête refusée (le journal
// persistant fait 8 Ko : sur-journaliser détruirait la fenêtre forensique de
// l'épisode, leçon du crash du 15/08).
//
// Les repères TERRAIN mesurés le 05/09 sont dans les tests EN VALEURS ABSOLUES :
// ce sont eux qui justifient le seuil, et ce sont eux qui doivent mourir si
// quelqu'un le déplace.
//   - plateau calme post-B-2 ......... 13 300 o → doit être SERVI
//   - plancher rafale 6 requêtes ..... 14 324 o → doit être SERVI
//   - plancher absolu du firmware ....  1 908 o → doit être REFUSÉ
//
// Les tests de frontière et de calibrage utilisent DÉLIBÉRÉMENT la vraie
// constante kStaticFileMinAllocBytes (constants.h) plutôt qu'un 8192 recopié :
// un test qui recopie le seuil ne peut pas détecter qu'on l'a déplacé.
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include "http_lowheap_logic.h"
#include "constants.h"  // kStaticFileMinAllocBytes, kHttpLowHeapEpisodeQuietMs

void setUp(void) {}
void tearDown(void) {}

// --- Repères terrain du 05/09 (bug-080, mesures firmware) --------------------
static const uint32_t kTerrainPlateauCalme = 13300;   // plateau post-B-2
static const uint32_t kTerrainPlancherRafale = 14324; // 6 requêtes statiques
static const uint32_t kTerrainPlancherAbsolu = 1908;  // pire valeur enregistrée

// Besoin d'un service de fichier statique SANS bégaiement TCP :
// ~600 o de chemin abort-prone + 2 pbuf lwip de 1 622 o ≈ 3 850 o.
static const uint32_t kBesoinSansBegaiement = 3850;

// Fabrique un épisode OUVERT et PEUPLÉ (piège de vacuité : évaluer les
// prédicats sur une structure vierge ne prouverait rien).
static HttpLowHeapEpisode episodeOuvert(uint32_t refusalsAtStart,
                                        uint32_t drained,
                                        uint32_t startMs,
                                        uint32_t lastRefusalMs) {
  HttpLowHeapEpisode st;
  st.active = true;
  st.refusalsAtStart = refusalsAtStart;
  st.drainedRefusals = drained;
  st.episodeStartMs = startMs;
  st.lastRefusalMs = lastRefusalMs;
  return st;
}

// ---------------------------------------------------------------------------
// T1 — Décision de refus, et frontière EXACTE
// ---------------------------------------------------------------------------

void test_frontiere_exacte_en_valeurs_absolues(void) {
  // 8 191 refusé / 8 192 servi / 8 193 servi, contre la VRAIE constante.
  TEST_ASSERT_TRUE(httpLowHeapShouldRefuse(8191, kStaticFileMinAllocBytes));
  TEST_ASSERT_FALSE(httpLowHeapShouldRefuse(8192, kStaticFileMinAllocBytes));
  TEST_ASSERT_FALSE(httpLowHeapShouldRefuse(8193, kStaticFileMinAllocBytes));
}

void test_frontiere_est_stricte_le_seuil_lui_meme_est_servi(void) {
  // Un tas EXACTEMENT au seuil couvre encore ×13,6 le besoin dur : refuser y
  // serait gratuit. Formulé relativement au seuil pour rester vrai s'il bouge —
  // c'est le test qui interdit `<` → `<=`.
  TEST_ASSERT_FALSE(httpLowHeapShouldRefuse(kStaticFileMinAllocBytes,
                                            kStaticFileMinAllocBytes));
  TEST_ASSERT_TRUE(httpLowHeapShouldRefuse(kStaticFileMinAllocBytes - 1,
                                           kStaticFileMinAllocBytes));
}

void test_tas_effondre_toujours_refuse(void) {
  TEST_ASSERT_TRUE(httpLowHeapShouldRefuse(0, kStaticFileMinAllocBytes));
  TEST_ASSERT_TRUE(httpLowHeapShouldRefuse(88, kStaticFileMinAllocBytes));
}

void test_tas_confortable_toujours_servi(void) {
  TEST_ASSERT_FALSE(httpLowHeapShouldRefuse(42996, kStaticFileMinAllocBytes));
}

// ---------------------------------------------------------------------------
// T2 — Non-régression sur les repères TERRAIN (justification du seuil)
// ---------------------------------------------------------------------------

void test_plateau_calme_terrain_est_servi(void) {
  // 13 300 o = régime NORMAL de l'appareil après le lot B-2. Si la garde mord
  // ici, l'UI est perdue en fonctionnement nominal.
  TEST_ASSERT_FALSE(httpLowHeapShouldRefuse(kTerrainPlateauCalme,
                                            kStaticFileMinAllocBytes));
}

void test_plancher_de_rafale_terrain_est_servi(void) {
  // Plancher observé pendant une rafale de 6 requêtes statiques : charger une
  // page ne doit produire AUCUN refus en régime calme.
  TEST_ASSERT_FALSE(httpLowHeapShouldRefuse(kTerrainPlancherRafale,
                                            kStaticFileMinAllocBytes));
}

void test_plancher_absolu_terrain_est_refuse(void) {
  // 1 908 o : le tas où le PANIC du 29/08 s'est produit. La garde DOIT mordre.
  TEST_ASSERT_TRUE(httpLowHeapShouldRefuse(kTerrainPlancherAbsolu,
                                           kStaticFileMinAllocBytes));
}

void test_seuil_laisse_le_regime_calme_encaisser_une_rafale(void) {
  // CALIBRAGE, pas cosmétique : sous le plateau calme mesuré, il doit rester au
  // moins de quoi servir un fichier sans bégaiement TCP (~3 850 o) AVANT que la
  // garde ne morde. Autrement dit le seuil doit rester ≤ 13 300 − 3 850 = 9 450.
  // C'est ce test qui rend un seuil à 12 288 inacceptable : il ne laisserait que
  // 1 012 o sous le régime réel de l'appareil.
  TEST_ASSERT_FALSE(httpLowHeapShouldRefuse(
    kTerrainPlateauCalme - kBesoinSansBegaiement, kStaticFileMinAllocBytes));
}

void test_seuil_reste_au_dessus_du_plancher_interdit_par_adr(void) {
  // Interdiction portée par l'ADR : ne jamais descendre sous 6 144 o pour
  // « faire taire » la garde (en dessous, ambigu avec le barreau WS et marge
  // < ×1,6 du besoin réel). Et jamais au-dessus du barreau WARN heap.
  TEST_ASSERT_TRUE(kStaticFileMinAllocBytes > 6144);
  TEST_ASSERT_TRUE(kStaticFileMinAllocBytes < kLowHeapWarnBytes);
}

void test_barreaux_inferieurs_de_l_echelle_restent_sous_la_garde(void) {
  // Condition C5 des lots B-1/B-2 : la garde 503 s'intercale AU-DESSUS du
  // reboot d'auto-guérison et des pushes WS sautés — sinon l'appareil
  // rebooterait avant d'avoir jamais refusé une page.
  TEST_ASSERT_TRUE(httpLowHeapShouldRefuse(kRestartMaxAllocHeapBytes,
                                           kStaticFileMinAllocBytes));
  TEST_ASSERT_TRUE(httpLowHeapShouldRefuse(kWsPushMinAllocBytes,
                                           kStaticFileMinAllocBytes));
}

// ---------------------------------------------------------------------------
// T3 — Machine à états du bornage du journal
// ---------------------------------------------------------------------------

void test_front_montant_emet_une_seule_ligne(void) {
  HttpLowHeapEpisode st;  // aucun épisode en cours
  HttpLowHeapDrainResult r = httpLowHeapDrain(st, 1, 1000, 0,
                                              kStaticFileMinAllocBytes,
                                              kHttpLowHeapEpisodeQuietMs);
  TEST_ASSERT_EQUAL(HttpLowHeapLogAction::EpisodeStart, r.action);
  TEST_ASSERT_TRUE(st.active);
  TEST_ASSERT_EQUAL_UINT32(1000, st.episodeStartMs);
}

void test_rafale_de_refus_ne_produit_aucune_ligne_supplementaire(void) {
  // 20 refus = 1 ligne, pas 20. C'est TOUT l'objet du bornage.
  HttpLowHeapEpisode st;
  uint32_t lignes = 0;
  for (uint32_t i = 1; i <= 20; ++i) {
    HttpLowHeapDrainResult r = httpLowHeapDrain(st, i, 1000 + i * 50, 0,
                                                kStaticFileMinAllocBytes,
                                                kHttpLowHeapEpisodeQuietMs);
    if (r.action != HttpLowHeapLogAction::None) lignes++;
  }
  TEST_ASSERT_EQUAL_UINT32(1, lignes);
  TEST_ASSERT_TRUE(st.active);
}

void test_drain_sans_nouveau_refus_hors_episode_ne_dit_rien(void) {
  // Le cas de très loin le plus fréquent : la loop draine des milliers de fois
  // par minute sans qu'il ne se passe rien.
  HttpLowHeapEpisode st;
  st.drainedRefusals = 7;  // état PEUPLÉ : 7 refus déjà drainés, épisode clos
  for (uint32_t t = 0; t < 200000; t += 5000) {
    HttpLowHeapDrainResult r = httpLowHeapDrain(st, 7, t, 40000,
                                                kStaticFileMinAllocBytes,
                                                kHttpLowHeapEpisodeQuietMs);
    TEST_ASSERT_EQUAL(HttpLowHeapLogAction::None, r.action);
  }
  TEST_ASSERT_FALSE(st.active);
}

void test_front_descendant_emet_une_ligne_avec_refus_et_duree(void) {
  // Épisode ouvert à t=1 000, 4 refus, dernier refus à t=3 000, tas rétabli,
  // silence écoulé → fermeture avec le bilan.
  HttpLowHeapEpisode st = episodeOuvert(/*atStart=*/0, /*drained=*/4,
                                        /*startMs=*/1000, /*lastMs=*/3000);
  HttpLowHeapDrainResult r =
    httpLowHeapDrain(st, 4, 3000 + kHttpLowHeapEpisodeQuietMs,
                     kTerrainPlateauCalme, kStaticFileMinAllocBytes,
                     kHttpLowHeapEpisodeQuietMs);
  TEST_ASSERT_EQUAL(HttpLowHeapLogAction::EpisodeEnd, r.action);
  TEST_ASSERT_EQUAL_UINT32(4, r.refusals);
  TEST_ASSERT_EQUAL_UINT32(2000 + kHttpLowHeapEpisodeQuietMs, r.durationMs);
  TEST_ASSERT_FALSE(st.active);
}

void test_front_descendant_ne_se_rejoue_pas(void) {
  HttpLowHeapEpisode st = episodeOuvert(0, 4, 1000, 3000);
  httpLowHeapDrain(st, 4, 40000, kTerrainPlateauCalme,
                   kStaticFileMinAllocBytes, kHttpLowHeapEpisodeQuietMs);
  // Deuxième passage de loop, rien n'a changé : silence.
  HttpLowHeapDrainResult r2 =
    httpLowHeapDrain(st, 4, 45000, kTerrainPlateauCalme,
                     kStaticFileMinAllocBytes, kHttpLowHeapEpisodeQuietMs);
  TEST_ASSERT_EQUAL(HttpLowHeapLogAction::None, r2.action);
}

void test_silence_de_30s_exige_avant_fermeture(void) {
  // Tas rétabli, plus aucun refus, mais 29 999 ms seulement depuis le dernier :
  // on ne ferme pas encore. À 30 000 ms exactement, on ferme.
  HttpLowHeapEpisode st = episodeOuvert(0, 3, 1000, 5000);
  HttpLowHeapDrainResult tot =
    httpLowHeapDrain(st, 3, 5000 + 29999, kTerrainPlateauCalme,
                     kStaticFileMinAllocBytes, kHttpLowHeapEpisodeQuietMs);
  TEST_ASSERT_EQUAL(HttpLowHeapLogAction::None, tot.action);
  TEST_ASSERT_TRUE(st.active);

  HttpLowHeapDrainResult pile =
    httpLowHeapDrain(st, 3, 5000 + 30000, kTerrainPlateauCalme,
                     kStaticFileMinAllocBytes, kHttpLowHeapEpisodeQuietMs);
  TEST_ASSERT_EQUAL(HttpLowHeapLogAction::EpisodeEnd, pile.action);
}

void test_tas_oscillant_autour_du_seuil_ne_produit_pas_deux_lignes_par_oscillation(void) {
  // LE scénario que le silence de 30 s existe pour tuer : le tas passe sous le
  // seuil, remonte, repasse... toutes les 5 s pendant 2 minutes. Sans le
  // silence : une paire de lignes par oscillation, journal détruit.
  HttpLowHeapEpisode st;
  uint32_t refus = 0, lignes = 0;
  for (uint32_t t = 0; t < 120000; t += 5000) {
    const bool sousLeSeuil = ((t / 5000) % 2) == 0;
    if (sousLeSeuil) refus++;  // async_tcp a incrémenté le compteur
    const uint32_t tas = sousLeSeuil ? 5000 : kTerrainPlateauCalme;
    HttpLowHeapDrainResult r =
      httpLowHeapDrain(st, refus, t, tas, kStaticFileMinAllocBytes,
                       kHttpLowHeapEpisodeQuietMs);
    if (r.action != HttpLowHeapLogAction::None) lignes++;
  }
  TEST_ASSERT_EQUAL_UINT32(1, lignes);  // le seul front montant initial
}

void test_silence_seul_ne_ferme_pas_si_le_tas_est_toujours_affame(void) {
  // Absence de visiteur ≠ famine résorbée. Fermer ici mentirait au journal.
  HttpLowHeapEpisode st = episodeOuvert(0, 3, 1000, 5000);
  HttpLowHeapDrainResult r =
    httpLowHeapDrain(st, 3, 5000 + 60000, kTerrainPlancherAbsolu,
                     kStaticFileMinAllocBytes, kHttpLowHeapEpisodeQuietMs);
  TEST_ASSERT_EQUAL(HttpLowHeapLogAction::None, r.action);
  TEST_ASSERT_TRUE(st.active);
}

void test_compteur_remis_a_zero_entre_deux_episodes(void) {
  // Épisode 1 : 3 refus. Épisode 2 : 2 refus. Le second bilan doit dire 2,
  // pas 5. Sans remise à zéro de la baseline, chaque épisode hériterait de
  // toute l'histoire de l'appareil.
  HttpLowHeapEpisode st;
  for (uint32_t i = 1; i <= 3; ++i) {
    httpLowHeapDrain(st, i, 1000 + i * 100, 0, kStaticFileMinAllocBytes,
                     kHttpLowHeapEpisodeQuietMs);
  }
  HttpLowHeapDrainResult fin1 =
    httpLowHeapDrain(st, 3, 1300 + 30000, kTerrainPlateauCalme,
                     kStaticFileMinAllocBytes, kHttpLowHeapEpisodeQuietMs);
  TEST_ASSERT_EQUAL(HttpLowHeapLogAction::EpisodeEnd, fin1.action);
  TEST_ASSERT_EQUAL_UINT32(3, fin1.refusals);

  // Second épisode, 100 s plus tard : le compteur atomique poursuit à 4 puis 5.
  HttpLowHeapDrainResult debut2 =
    httpLowHeapDrain(st, 4, 140000, 0, kStaticFileMinAllocBytes,
                     kHttpLowHeapEpisodeQuietMs);
  TEST_ASSERT_EQUAL(HttpLowHeapLogAction::EpisodeStart, debut2.action);
  httpLowHeapDrain(st, 5, 141000, 0, kStaticFileMinAllocBytes,
                   kHttpLowHeapEpisodeQuietMs);
  HttpLowHeapDrainResult fin2 =
    httpLowHeapDrain(st, 5, 141000 + 30000, kTerrainPlateauCalme,
                     kStaticFileMinAllocBytes, kHttpLowHeapEpisodeQuietMs);
  TEST_ASSERT_EQUAL(HttpLowHeapLogAction::EpisodeEnd, fin2.action);
  TEST_ASSERT_EQUAL_UINT32(2, fin2.refusals);
  TEST_ASSERT_EQUAL_UINT32(31000, fin2.durationMs);
}

// ---------------------------------------------------------------------------
// T3 bis — httpLowHeapNeedsHeapSample : ne pas parcourir le tas pour rien
// ---------------------------------------------------------------------------

void test_pas_de_mesure_du_tas_hors_episode(void) {
  // Cas nominal (99,99 % du temps) : la loop ne doit JAMAIS walker le tas.
  HttpLowHeapEpisode st;
  st.drainedRefusals = 12;   // état PEUPLÉ, mais épisode clos
  st.lastRefusalMs = 1000;
  TEST_ASSERT_FALSE(httpLowHeapNeedsHeapSample(st, 500000,
                                               kHttpLowHeapEpisodeQuietMs));
}

void test_pas_de_mesure_du_tas_pendant_la_famine_active(void) {
  // Épisode OUVERT et refus récents : parcourir le tas à la fréquence de la
  // loop, pendant la famine, serait le pire moment possible.
  HttpLowHeapEpisode st = episodeOuvert(0, 5, 1000, 20000);
  TEST_ASSERT_FALSE(httpLowHeapNeedsHeapSample(st, 20000,
                                               kHttpLowHeapEpisodeQuietMs));
  TEST_ASSERT_FALSE(httpLowHeapNeedsHeapSample(st, 20000 + 29999,
                                               kHttpLowHeapEpisodeQuietMs));
}

void test_mesure_du_tas_seulement_quand_la_fermeture_est_en_jeu(void) {
  HttpLowHeapEpisode st = episodeOuvert(0, 5, 1000, 20000);
  TEST_ASSERT_TRUE(httpLowHeapNeedsHeapSample(st, 20000 + 30000,
                                              kHttpLowHeapEpisodeQuietMs));
}

// ---------------------------------------------------------------------------
// T4 — Repliement de millis() (24,8 j). Le projet s'y est déjà fait mordre.
// ---------------------------------------------------------------------------

void test_duree_d_episode_juste_au_repliement_de_millis(void) {
  // Épisode démarré 2 s avant le repliement, dernier refus 1 s avant, fermé
  // 30 s APRÈS le repliement : la durée doit valoir 32 s, pas ~49 jours.
  const uint32_t kMax = 0xFFFFFFFFu;
  const uint32_t start = kMax - 1999;   // t0
  const uint32_t last = kMax - 999;     // t0 + 1 s
  const uint32_t now = last + 31000;    // replié
  HttpLowHeapEpisode st = episodeOuvert(0, 6, start, last);
  HttpLowHeapDrainResult r =
    httpLowHeapDrain(st, 6, now, kTerrainPlateauCalme,
                     kStaticFileMinAllocBytes, kHttpLowHeapEpisodeQuietMs);
  TEST_ASSERT_EQUAL(HttpLowHeapLogAction::EpisodeEnd, r.action);
  TEST_ASSERT_EQUAL_UINT32(32000, r.durationMs);
  TEST_ASSERT_EQUAL_UINT32(6, r.refusals);
}

void test_silence_juste_au_repliement_de_millis(void) {
  // Même repliement, côté garde de coût : 29 999 ms → pas encore, 30 000 → oui.
  const uint32_t last = 0xFFFFFFFFu - 999;
  HttpLowHeapEpisode st = episodeOuvert(0, 6, last - 5000, last);
  TEST_ASSERT_FALSE(httpLowHeapNeedsHeapSample(st, last + 29999,
                                               kHttpLowHeapEpisodeQuietMs));
  TEST_ASSERT_TRUE(httpLowHeapNeedsHeapSample(st, last + 30000,
                                              kHttpLowHeapEpisodeQuietMs));
}

void test_compteur_de_refus_replie_reste_juste(void) {
  // Le compteur atomique est un uint32 monotone alimenté par fetch_add : il
  // finit par se replier. Le bilan d'épisode reste correct (soustraction non
  // signée) : 3 refus, pas 4 milliards.
  HttpLowHeapEpisode st;
  st.drainedRefusals = 0xFFFFFFFEu;
  httpLowHeapDrain(st, 0xFFFFFFFFu, 1000, 0, kStaticFileMinAllocBytes,
                   kHttpLowHeapEpisodeQuietMs);  // front montant
  httpLowHeapDrain(st, 0u, 1100, 0, kStaticFileMinAllocBytes,
                   kHttpLowHeapEpisodeQuietMs);  // repliement du compteur
  httpLowHeapDrain(st, 1u, 1200, 0, kStaticFileMinAllocBytes,
                   kHttpLowHeapEpisodeQuietMs);
  HttpLowHeapDrainResult fin =
    httpLowHeapDrain(st, 1u, 1200 + 30000, kTerrainPlateauCalme,
                     kStaticFileMinAllocBytes, kHttpLowHeapEpisodeQuietMs);
  TEST_ASSERT_EQUAL(HttpLowHeapLogAction::EpisodeEnd, fin.action);
  TEST_ASSERT_EQUAL_UINT32(3, fin.refusals);
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  // T1 — décision
  RUN_TEST(test_frontiere_exacte_en_valeurs_absolues);
  RUN_TEST(test_frontiere_est_stricte_le_seuil_lui_meme_est_servi);
  RUN_TEST(test_tas_effondre_toujours_refuse);
  RUN_TEST(test_tas_confortable_toujours_servi);
  // T2 — repères terrain + calibrage
  RUN_TEST(test_plateau_calme_terrain_est_servi);
  RUN_TEST(test_plancher_de_rafale_terrain_est_servi);
  RUN_TEST(test_plancher_absolu_terrain_est_refuse);
  RUN_TEST(test_seuil_laisse_le_regime_calme_encaisser_une_rafale);
  RUN_TEST(test_seuil_reste_au_dessus_du_plancher_interdit_par_adr);
  RUN_TEST(test_barreaux_inferieurs_de_l_echelle_restent_sous_la_garde);
  // T3 — machine à états du bornage
  RUN_TEST(test_front_montant_emet_une_seule_ligne);
  RUN_TEST(test_rafale_de_refus_ne_produit_aucune_ligne_supplementaire);
  RUN_TEST(test_drain_sans_nouveau_refus_hors_episode_ne_dit_rien);
  RUN_TEST(test_front_descendant_emet_une_ligne_avec_refus_et_duree);
  RUN_TEST(test_front_descendant_ne_se_rejoue_pas);
  RUN_TEST(test_silence_de_30s_exige_avant_fermeture);
  RUN_TEST(test_tas_oscillant_autour_du_seuil_ne_produit_pas_deux_lignes_par_oscillation);
  RUN_TEST(test_silence_seul_ne_ferme_pas_si_le_tas_est_toujours_affame);
  RUN_TEST(test_compteur_remis_a_zero_entre_deux_episodes);
  // T3 bis — garde de coût de la mesure
  RUN_TEST(test_pas_de_mesure_du_tas_hors_episode);
  RUN_TEST(test_pas_de_mesure_du_tas_pendant_la_famine_active);
  RUN_TEST(test_mesure_du_tas_seulement_quand_la_fermeture_est_en_jeu);
  // T4 — repliement de millis()
  RUN_TEST(test_duree_d_episode_juste_au_repliement_de_millis);
  RUN_TEST(test_silence_juste_au_repliement_de_millis);
  RUN_TEST(test_compteur_de_refus_replie_reste_juste);
  return UNITY_END();
}
