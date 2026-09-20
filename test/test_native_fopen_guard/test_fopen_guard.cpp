// =============================================================================
// Tests unitaires natifs — Admission des `fopen` INTERNES (bug-100, ADR-0070)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// Cible : src/fopen_guard_logic.{h,cpp} (module pur, ADR-0017).
//
// CE QUE CETTE SUITE VERROUILLE, et rien d'autre :
//
//   L'ASYMÉTRIE MESURE/DÉCISION (ADR-0070 §e).
//   > Autoriser exige une mesure fraîche. Refuser n'en exige aucune.
//   > Une autorisation périmée coûte un abort(). Un refus périmé coûte une
//   > sauvegarde différée de 5 min.
//
// C'est un invariant DISSYMÉTRIQUE : la moitié « refuser sans mesurer » est une
// optimisation, la moitié « ne jamais autoriser sans mesurer » est une garantie
// de sûreté. Un correctif futur qui « symétriserait » le cache — par souci de
// cohérence, ce qui est exactement la tentation — réintroduirait le crash du
// 05/09/2026. C'est le défaut qu'ADR-0058 avait déjà pesé et rejeté.
//
// Les tests de seuil utilisent DÉLIBÉRÉMENT les vraies constantes de
// constants.h plutôt qu'un 5120/8192 recopié : un test qui recopie le seuil ne
// peut pas détecter qu'on l'a déplacé.
//
// Repères TERRAIN, en valeurs absolues (relevés bug-080 du 05/09/2026,
// HISTORIQUES — état d'alors, avant feature-129/feature-131) :
//   - plateau calme post-B-2 ......... 13 300 o → doit être ADMIS (A et B)
//   - plancher rafale 6 requêtes ..... 14 324 o → doit être ADMIS (A et B)
//   - plancher absolu du firmware ....  1 908 o → doit être REFUSÉ (A et B)
//
// Repère feature-132 (2026-09-20, post-feature-131) : plancher glissant le
// plus bas mesuré sous rafale de 9 requêtes parallèles = 19 444 o (0 refus sur
// 171 requêtes). Le seuil n'a PAS bougé (cf. constants.h) : ces repères de
// 05/09 restent valides comme plancher de non-régression, ils ne sont plus le
// pire cas observé.
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include "fopen_guard_logic.h"
#include "constants.h"  // kInternalFopenMinAllocBytes, kStaticFileMinAllocBytes, …

void setUp(void) {}
void tearDown(void) {}

// --- Repères terrain du 05/09 (bug-080, mesures firmware, HISTORIQUES) -------
// Revérifiés le 2026-09-20 (feature-132) : toujours des planchers valides,
// mais plus le pire cas observé (cf. commentaire d'en-tête, 19 444 o).
static const uint32_t kTerrainPlateauCalme   = 13300;
static const uint32_t kTerrainPlancherRafale = 14324;
static const uint32_t kTerrainPlancherAbsolu = 1908;

// Besoin DUR d'une ouverture, valeurs RÉELLES recomptées au cycle 1 de bug-100
// (92 o de verrou + 428 o d'expansion inlinée + ~190 + ~64 + ~100) — ADR-0070 §b.
static const uint32_t kBesoinDurOuverture = 874;
// Besoin AVEC 2 pbuf lwip en vol : 874 + 2 × 1622.
static const uint32_t kBesoinAvecContentionTcp = 4118;

// La valeur passée par la coquille quand elle N'A PAS mesuré (fs_guard.cpp).
// Ce n'est pas « zéro octet libre », c'est « je ne sais pas ».
static const uint32_t kMesureAbsente = 0;

// Fabrique un épisode OUVERT et PEUPLÉ : évaluer les prédicats sur une structure
// vierge ne prouverait rien (piège de vacuité).
static FopenGuardEpisode episodeOuvert(uint32_t drained, uint32_t atStart,
                                       uint32_t startMs, uint32_t lastDeferralMs,
                                       uint32_t lastProbeMs, bool armed) {
  FopenGuardEpisode st;
  st.active = true;
  st.probeFloorArmed = armed;
  st.escalated = false;
  st.drainedDeferrals = drained;
  st.deferralsAtStart = atStart;
  st.episodeStartMs = startMs;
  st.lastDeferralMs = lastDeferralMs;
  st.lastProbeMs = lastProbeMs;
  return st;
}

// ===========================================================================
// T1 — Le prédicat de refus, et sa FRONTIÈRE STRICTE
// ===========================================================================

static void test_frontiere_stricte_le_seuil_lui_meme_est_admis(void) {
  // `< seuil` refuse, `>= seuil` ouvre. La valeur EXACTE du seuil est admise.
  TEST_ASSERT_TRUE(fopenGuardShouldDefer(kInternalFopenMinAllocBytes - 1,
                                         kInternalFopenMinAllocBytes));
  TEST_ASSERT_FALSE(fopenGuardShouldDefer(kInternalFopenMinAllocBytes,
                                          kInternalFopenMinAllocBytes));
  TEST_ASSERT_FALSE(fopenGuardShouldDefer(kInternalFopenMinAllocBytes + 1,
                                          kInternalFopenMinAllocBytes));
}

static void test_frontiere_stricte_classe_A_aussi(void) {
  TEST_ASSERT_TRUE(fopenGuardShouldDefer(kStaticFileMinAllocBytes - 1,
                                         kStaticFileMinAllocBytes));
  TEST_ASSERT_FALSE(fopenGuardShouldDefer(kStaticFileMinAllocBytes,
                                          kStaticFileMinAllocBytes));
  TEST_ASSERT_FALSE(fopenGuardShouldDefer(kStaticFileMinAllocBytes + 1,
                                          kStaticFileMinAllocBytes));
}

static void test_frontieres_en_valeurs_absolues(void) {
  // Si quelqu'un déplace un seuil, CES lignes doivent mourir — elles ne
  // recopient pas la constante, elles la datent.
  TEST_ASSERT_EQUAL_UINT32(5120, kInternalFopenMinAllocBytes);
  TEST_ASSERT_EQUAL_UINT32(8192, kStaticFileMinAllocBytes);
  TEST_ASSERT_TRUE(fopenGuardShouldDefer(5119, kInternalFopenMinAllocBytes));
  TEST_ASSERT_FALSE(fopenGuardShouldDefer(5120, kInternalFopenMinAllocBytes));
  TEST_ASSERT_TRUE(fopenGuardShouldDefer(8191, kStaticFileMinAllocBytes));
  TEST_ASSERT_FALSE(fopenGuardShouldDefer(8192, kStaticFileMinAllocBytes));
}

static void test_tas_effondre_toujours_refuse_dans_les_deux_classes(void) {
  TEST_ASSERT_TRUE(fopenGuardShouldDefer(0, kInternalFopenMinAllocBytes));
  TEST_ASSERT_TRUE(fopenGuardShouldDefer(kTerrainPlancherAbsolu,
                                         kInternalFopenMinAllocBytes));
  TEST_ASSERT_TRUE(fopenGuardShouldDefer(kTerrainPlancherAbsolu,
                                         kStaticFileMinAllocBytes));
}

static void test_reperes_terrain_confortables_sont_admis(void) {
  TEST_ASSERT_FALSE(fopenGuardShouldDefer(kTerrainPlateauCalme,
                                          kInternalFopenMinAllocBytes));
  TEST_ASSERT_FALSE(fopenGuardShouldDefer(kTerrainPlateauCalme,
                                          kStaticFileMinAllocBytes));
  TEST_ASSERT_FALSE(fopenGuardShouldDefer(kTerrainPlancherRafale,
                                          kStaticFileMinAllocBytes));
}

static void test_admission_deja_prononcee_n_est_jamais_refusee(void) {
  // kFsGuardAlreadyAdmitted (= 0) : la décision a été prise en amont, sur une
  // mesure fraîche, au point où l'opération pouvait encore NE RIEN FAIRE. Ce
  // chemin ne doit JAMAIS refuser — sinon saveToFile() détruirait le fichier
  // d'historique entre le remove() et l'open("w") (ADR-0070 §d).
  TEST_ASSERT_FALSE(fopenGuardShouldDefer(0, 0));
  TEST_ASSERT_FALSE(fopenGuardShouldDefer(1, 0));
  TEST_ASSERT_FALSE(fopenGuardShouldDefer(kTerrainPlancherAbsolu, 0));
  TEST_ASSERT_FALSE(fopenGuardShouldDefer(0xFFFFFFFFu, 0));
}

// ===========================================================================
// T2 — Calibrage : les deux classes couvrent les besoins qui les justifient
// ===========================================================================

static void test_classe_B_couvre_le_besoin_dur_avec_marge(void) {
  // Marge ×5,9 sur 874 o, revendiquée par ADR-0070 §b.
  TEST_ASSERT_TRUE(kInternalFopenMinAllocBytes >= 5 * kBesoinDurOuverture);
  TEST_ASSERT_FALSE(fopenGuardShouldDefer(kInternalFopenMinAllocBytes,
                                          kBesoinDurOuverture));
}

static void test_classe_A_couvre_le_besoin_avec_contention_tcp(void) {
  // Marge ×1,99 sur 4 118 o. C'est CE chiffre qui interdit 5 120 pour la classe
  // A (×1,24 seulement — option D, rejetée).
  // Marge en centièmes pour rester en arithmétique entière : 8192 / 4118 = 1,98.
  TEST_ASSERT_TRUE(100u * kStaticFileMinAllocBytes >= 198u * kBesoinAvecContentionTcp);
  // Et 5 120 n'y suffirait PAS : 5120 / 4118 = 1,24 (option D, rejetée).
  TEST_ASSERT_TRUE(100u * kInternalFopenMinAllocBytes < 150u * kBesoinAvecContentionTcp);
}

static void test_echelle_ADR0058_reste_ordonnee_a_sept_barreaux(void) {
  // Le 7ᵉ barreau s'intercale STRICTEMENT entre l'auto-guérison et les pushes
  // WS. Sous kRestartMaxAllocHeapBytes, l'appareil rebooterait avant d'avoir
  // jamais démarré sans historique : le tripwire serait MORT-NÉ.
  // Ce test double au RUNTIME le static_assert de constants.h — pour qu'une
  // suppression de l'assert ne rende pas l'ordre libre.
  TEST_ASSERT_TRUE(kOomFallbackRearmMinMaxAllocBytes < kRestartMaxAllocHeapBytes);
  TEST_ASSERT_TRUE(kRestartMaxAllocHeapBytes < kInternalFopenMinAllocBytes);
  TEST_ASSERT_TRUE(kInternalFopenMinAllocBytes < kWsPushMinAllocBytes);
  TEST_ASSERT_TRUE(kWsPushMinAllocBytes < kStaticFileMinAllocBytes);
  TEST_ASSERT_TRUE(kStaticFileMinAllocBytes < kLowHeapWarnBytes);
  // En valeurs absolues : 4096 < 5120 < 6144.
  TEST_ASSERT_EQUAL_UINT32(4096, kRestartMaxAllocHeapBytes);
  TEST_ASSERT_EQUAL_UINT32(6144, kWsPushMinAllocBytes);
}

static void test_tripwire_de_boot_est_atteignable_avant_le_reboot(void) {
  // Bande 4 096 – 5 120 : l'appareil DÉMARRE SANS HISTORIQUE PERSISTANT MAIS EN
  // DOSANT. C'est la conséquence voulue d'ADR-0070 §c, et elle n'existe que si
  // le 7ᵉ barreau est STRICTEMENT au-dessus du reboot d'auto-guérison.
  TEST_ASSERT_TRUE(kInternalFopenMinAllocBytes > kRestartMaxAllocHeapBytes);
  for (uint32_t m = kRestartMaxAllocHeapBytes; m < kInternalFopenMinAllocBytes; ++m) {
    // Dans toute cette bande : fopen de boot refusé (tripwire), pas de reboot.
    TEST_ASSERT_TRUE(fopenGuardShouldDefer(m, kInternalFopenMinAllocBytes));
    TEST_ASSERT_TRUE(m >= kRestartMaxAllocHeapBytes);
  }
}

// ===========================================================================
// T3 — L'ASYMÉTRIE : quand faut-il PAYER un parcours du tas ?
// ===========================================================================

static void test_hors_episode_la_mesure_est_toujours_due(void) {
  // Hors épisode, AUTORISER est le verdict probable → jamais de décision sur une
  // valeur d'âge inconnu. Vrai quelles que soient les horloges : même avec un
  // lastProbeMs d'il y a 1 ms, même en plein silence nul.
  FopenGuardEpisode st;  // vierge : active = false
  TEST_ASSERT_TRUE(fopenGuardNeedsFreshMeasure(st, 0, kInternalFopenEpisodeQuietMs,
                                               kInternalFopenProbeMinIntervalMs));

  FopenGuardEpisode chaud = episodeOuvert(40, 10, 1000, 100000, 100000, true);
  chaud.active = false;  // épisode CLOS, mais horodatages tout frais
  TEST_ASSERT_TRUE(fopenGuardNeedsFreshMeasure(chaud, 100001,
                                               kInternalFopenEpisodeQuietMs,
                                               kInternalFopenProbeMinIntervalMs));
}

static void test_episode_ouvert_mais_tas_confortable_re_mesure_toujours(void) {
  // probeFloorArmed = false ⇒ le dernier relevé était >= 2 × seuil. Le cache
  // n'est PAS armé : chaque décision re-mesure, donc AUCUN refus périmé ne peut
  // être produit en régime normal. Le cache ne coûte que là où il rapporte.
  FopenGuardEpisode st = episodeOuvert(40, 10, 1000, 100000, 100000,
                                       /*armed=*/false);
  TEST_ASSERT_TRUE(fopenGuardNeedsFreshMeasure(st, 100001,
                                               kInternalFopenEpisodeQuietMs,
                                               kInternalFopenProbeMinIntervalMs));
}

static void test_plancher_de_sonde_refuse_sans_mesurer(void) {
  // Plancher DUR : deux parcours TLSF ne peuvent pas être plus rapprochés que
  // kInternalFopenProbeMinIntervalMs. Sous ce plancher → false ⇒ l'appelant
  // REFUSE sans mesurer. C'est le côté BON MARCHÉ de l'asymétrie.
  FopenGuardEpisode st = episodeOuvert(40, 10, 1000, 100000, 100000, true);
  const uint32_t now = 100000 + kInternalFopenProbeMinIntervalMs - 1;
  TEST_ASSERT_FALSE(fopenGuardNeedsFreshMeasure(st, now,
                                                kInternalFopenEpisodeQuietMs,
                                                kInternalFopenProbeMinIntervalMs));
}

static void test_plancher_de_sonde_PRIME_sur_le_silence(void) {
  // ⚠️ Le cas qui SÉPARE réellement les deux garde-fous — et le seul qui tue un
  // plancher neutralisé. Le critère de silence est SATISFAIT (dernier refus il y
  // a 100 s, bien au-delà des 30 s), mais un parcours du tas vient d'être payé
  // il y a 10 ms. Configuration parfaitement atteignable : le drain de la loop
  // sonde, trouve le tas encore affamé, et une demande d'admission arrive dans
  // la foulée. Sans plancher, CHAQUE demande re-parcourrait le tas — au moment
  // exact où ce parcours coûte le plus cher (TLSF fragmenté).
  const uint32_t now = 100000;
  FopenGuardEpisode st = episodeOuvert(40, 10, /*startMs=*/0,
                                       /*lastDeferral=*/0,      // silence de 100 s
                                       /*lastProbe=*/now - 10,  // sonde d'il y a 10 ms
                                       /*armed=*/true);
  TEST_ASSERT_TRUE((now - st.lastDeferralMs) >= kInternalFopenEpisodeQuietMs);
  TEST_ASSERT_FALSE(fopenGuardNeedsFreshMeasure(st, now,
                                                kInternalFopenEpisodeQuietMs,
                                                kInternalFopenProbeMinIntervalMs));
  // Et dès le plancher franchi, le silence reprend la main : la sortie
  // d'épisode redevient possible. Le plancher DIFFÈRE, il ne verrouille pas.
  st.lastProbeMs = now - kInternalFopenProbeMinIntervalMs;
  TEST_ASSERT_TRUE(fopenGuardNeedsFreshMeasure(st, now,
                                               kInternalFopenEpisodeQuietMs,
                                               kInternalFopenProbeMinIntervalMs));
}

static void test_plancher_de_sonde_borne_le_nombre_de_parcours_de_tas(void) {
  // Formulation en COÛT, pas en branche : sur une rafale d'admissions à 1 ms
  // d'intervalle, le tas ne peut pas être parcouru plus souvent qu'une fois par
  // kInternalFopenProbeMinIntervalMs. C'est la raison d'être du plancher.
  // Le silence est SATISFAIT en permanence (dernier refus au temps 0, on
  // observe à partir de 100 s) : sans plancher, chaque demande re-mesurerait.
  const uint32_t t0 = 100000;
  FopenGuardEpisode st = episodeOuvert(40, 10, 0, /*lastDeferral=*/0,
                                       /*lastProbe=*/t0, true);
  int parcours = 0;
  const uint32_t duree = 10000;  // 10 s de rafale continue, 1 demande/ms
  for (uint32_t now = t0 + 1; now <= t0 + duree; ++now) {
    if (fopenGuardNeedsFreshMeasure(st, now, kInternalFopenEpisodeQuietMs,
                                    kInternalFopenProbeMinIntervalMs)) {
      parcours++;
      st.lastProbeMs = now;  // la coquille horodate le parcours payé
    }
  }
  // Borne dure : jamais plus d'un parcours par plancher. 10 000 demandes → 100
  // parcours, pas 10 000. Le test est encadré des DEUX côtés : trop peu de
  // parcours signifierait que la sortie d'épisode n'est plus observée du tout.
  TEST_ASSERT_TRUE(parcours <= (int)(duree / kInternalFopenProbeMinIntervalMs) + 1);
  TEST_ASSERT_TRUE(parcours >= (int)(duree / kInternalFopenProbeMinIntervalMs) - 1);
}

static void test_plancher_de_sonde_frontiere_exacte(void) {
  // À l'instant EXACT du plancher, le plancher ne bloque plus : la décision
  // repasse au critère de silence.
  FopenGuardEpisode st = episodeOuvert(40, 10, 1000,
                                       /*lastDeferral=*/100000,
                                       /*lastProbe=*/100000, true);
  const uint32_t auPlancher = 100000 + kInternalFopenProbeMinIntervalMs;
  // Plancher franchi mais silence insuffisant → toujours pas de mesure.
  TEST_ASSERT_FALSE(fopenGuardNeedsFreshMeasure(st, auPlancher,
                                                kInternalFopenEpisodeQuietMs,
                                                kInternalFopenProbeMinIntervalMs));
  // Plancher franchi ET silence atteint → mesure due.
  FopenGuardEpisode calme = episodeOuvert(40, 10, 1000,
                                          /*lastDeferral=*/100000,
                                          /*lastProbe=*/100000, true);
  const uint32_t apresSilence = 100000 + kInternalFopenEpisodeQuietMs;
  TEST_ASSERT_TRUE(fopenGuardNeedsFreshMeasure(calme, apresSilence,
                                               kInternalFopenEpisodeQuietMs,
                                               kInternalFopenProbeMinIntervalMs));
}

static void test_rafale_en_cours_ne_paie_jamais_le_tas(void) {
  // Des refus arrivent encore : la famine est manifestement là. On ne re-mesure
  // qu'après quietMs de silence, c'est-à-dire quand une SORTIE devient
  // plausible. Le parcours TLSF est le plus cher exactement dans ce régime.
  FopenGuardEpisode st = episodeOuvert(40, 10, 1000,
                                       /*lastDeferral=*/199990,
                                       /*lastProbe=*/100000, true);
  TEST_ASSERT_FALSE(fopenGuardNeedsFreshMeasure(st, 200000,
                                                kInternalFopenEpisodeQuietMs,
                                                kInternalFopenProbeMinIntervalMs));
}

static void test_silence_frontiere_exacte(void) {
  FopenGuardEpisode st = episodeOuvert(40, 10, 1000,
                                       /*lastDeferral=*/100000,
                                       /*lastProbe=*/100000, true);
  // silence == quietMs - 1 → pas encore.
  TEST_ASSERT_FALSE(fopenGuardNeedsFreshMeasure(
      st, 100000 + kInternalFopenEpisodeQuietMs - 1,
      kInternalFopenEpisodeQuietMs, kInternalFopenProbeMinIntervalMs));
  // silence == quietMs → oui.
  TEST_ASSERT_TRUE(fopenGuardNeedsFreshMeasure(
      st, 100000 + kInternalFopenEpisodeQuietMs,
      kInternalFopenEpisodeQuietMs, kInternalFopenProbeMinIntervalMs));
}

static void test_asymetrie_le_cache_ne_produit_que_des_refus(void) {
  // Formulation directe de l'invariant : CHAQUE FOIS que le prédicat renvoie
  // false (= « ne mesure pas »), la seule conduite possible de l'appelant est de
  // REFUSER. Il n'existe aucun état où « ne pas mesurer » autorise.
  // On balaie l'espace d'états atteignable.
  for (int actif = 0; actif <= 1; ++actif) {
    for (int arme = 0; arme <= 1; ++arme) {
      for (uint32_t dt = 0; dt <= 60000; dt += 4999) {
        FopenGuardEpisode st = episodeOuvert(40, 10, 0, 0, 0, arme != 0);
        st.active = (actif != 0);
        const bool mesure = fopenGuardNeedsFreshMeasure(
            st, dt, kInternalFopenEpisodeQuietMs, kInternalFopenProbeMinIntervalMs);
        if (!mesure) {
          // Sans mesure, la coquille passe 0 au drain → 0 < seuil → refus.
          TEST_ASSERT_TRUE(st.active);       // jamais hors épisode
          TEST_ASSERT_TRUE(st.probeFloorArmed);  // jamais cache désarmé
          TEST_ASSERT_TRUE(fopenGuardShouldDefer(kMesureAbsente,
                                                 kInternalFopenMinAllocBytes));
        }
      }
    }
  }
}

static void test_repliement_de_millis_ne_relache_pas_la_garde(void) {
  // Soustractions non signées : justes au repliement de millis() (24,8 j).
  const uint32_t avant = 0xFFFFFF00u;  // ~256 ms avant le repliement
  FopenGuardEpisode st = episodeOuvert(40, 10, avant,
                                       /*lastDeferral=*/avant,
                                       /*lastProbe=*/avant, true);
  // now = 0x40 après repliement → écoulé = 0x140 = 320 ms > plancher 100 ms,
  // mais silence = 320 ms < 30 s → toujours pas de mesure.
  TEST_ASSERT_FALSE(fopenGuardNeedsFreshMeasure(st, 0x40u,
                                                kInternalFopenEpisodeQuietMs,
                                                kInternalFopenProbeMinIntervalMs));
  // 30 s après le repliement : mesure due.
  const uint32_t apres = (uint32_t)(avant + kInternalFopenEpisodeQuietMs);
  TEST_ASSERT_TRUE(fopenGuardNeedsFreshMeasure(st, apres,
                                               kInternalFopenEpisodeQuietMs,
                                               kInternalFopenProbeMinIntervalMs));
  // Sous le plancher au repliement : pas de mesure.
  FopenGuardEpisode serre = episodeOuvert(40, 10, avant, avant, avant, true);
  TEST_ASSERT_FALSE(fopenGuardNeedsFreshMeasure(serre, 0x10u,
                                                kInternalFopenEpisodeQuietMs,
                                                kInternalFopenProbeMinIntervalMs));
}

// ===========================================================================
// T3bis — fopenGuardNoteRefusal() : LA règle du front montant, en isolation
// ===========================================================================
// Cible directement la fonction extraite : elle seule doit décider quand
// stamper `episodeStartMs`, et elle doit le faire QUE le module soit exercé
// via fopenGuardDrain() ou appelé nu, comme fsGuardAdmit() le fait en
// production.

static void test_note_refusal_pose_active_et_episode_start_ms_au_premier_appel(void) {
  FopenGuardEpisode st;  // vierge : active = false
  TEST_ASSERT_TRUE(fopenGuardNoteRefusal(st, 4242));  // front montant
  TEST_ASSERT_TRUE(st.active);
  TEST_ASSERT_EQUAL_UINT32(4242, st.episodeStartMs);
  TEST_ASSERT_EQUAL_UINT32(4242, st.lastDeferralMs);
}

static void test_note_refusal_ne_re_stampe_pas_episode_start_ms_ensuite(void) {
  // Deuxième, troisième refus du MÊME épisode : `episodeStartMs` ne doit plus
  // bouger (sinon la durée rapportée daterait du dernier refus, pas du
  // premier), mais `lastDeferralMs` doit suivre à chaque appel.
  FopenGuardEpisode st;
  TEST_ASSERT_TRUE(fopenGuardNoteRefusal(st, 1000));
  TEST_ASSERT_FALSE(fopenGuardNoteRefusal(st, 1500));  // plus un front montant
  TEST_ASSERT_EQUAL_UINT32(1000, st.episodeStartMs);   // inchangé
  TEST_ASSERT_EQUAL_UINT32(1500, st.lastDeferralMs);   // suit
  TEST_ASSERT_FALSE(fopenGuardNoteRefusal(st, 9000));
  TEST_ASSERT_EQUAL_UINT32(1000, st.episodeStartMs);
  TEST_ASSERT_EQUAL_UINT32(9000, st.lastDeferralMs);
}

static void test_note_refusal_rouvre_un_nouveau_front_montant_apres_fermeture(void) {
  // Un épisode clos (active revenu à false) doit pouvoir en rouvrir un
  // nouveau, avec un episodeStartMs qui lui est propre.
  FopenGuardEpisode st;
  fopenGuardNoteRefusal(st, 1000);
  st.active = false;  // simule la fermeture prononcée par fopenGuardDrain()
  TEST_ASSERT_TRUE(fopenGuardNoteRefusal(st, 7000));  // nouveau front montant
  TEST_ASSERT_EQUAL_UINT32(7000, st.episodeStartMs);
}

static void test_episode_duration_ms_mesure_depuis_le_debut_de_l_episode(void) {
  FopenGuardEpisode st;
  fopenGuardNoteRefusal(st, 1000);
  TEST_ASSERT_EQUAL_UINT32(0, fopenGuardEpisodeDurationMs(st, 1000));
  TEST_ASSERT_EQUAL_UINT32(29000, fopenGuardEpisodeDurationMs(st, 30000));
}

static void test_episode_duration_ms_survit_au_repliement_de_millis(void) {
  const uint32_t avant = 0xFFFFFF00u;  // ~256 ms avant le repliement
  FopenGuardEpisode st;
  fopenGuardNoteRefusal(st, avant);
  // 0x140 après le repliement = 320 ms écoulées, pas un nombre négatif géant.
  TEST_ASSERT_EQUAL_UINT32(0x140u, fopenGuardEpisodeDurationMs(st, 0x40u));
}

// ===========================================================================
// T4 — Machine à états de l'épisode
// ===========================================================================

static void test_front_montant_ouvre_l_episode_une_seule_fois(void) {
  FopenGuardEpisode st;  // vierge
  // Modélise la séquence réelle : fsGuardAdmit() note le refus (pose
  // active/episodeStartMs) AVANT que la loop n'appelle fopenGuardDrain() — et
  // ce n'est PAS au même instant (le drain tourne à sa propre cadence). Des
  // horodatages DÉLIBÉRÉMENT distincts (900 vs 1000) tuent le mutant « c'est le
  // drain qui pose episodeStartMs » : s'il le posait, il le daterait de 1000,
  // pas de 900.
  TEST_ASSERT_TRUE(fopenGuardNoteRefusal(st, 900));
  TEST_ASSERT_EQUAL(FopenGuardAction::EpisodeStart,
                    fopenGuardDrain(st, 1, 1000, kMesureAbsente,
                                    kStaticFileMinAllocBytes,
                                    kInternalFopenEscalateAfterRefusals));
  TEST_ASSERT_TRUE(st.active);
  TEST_ASSERT_EQUAL_UINT32(900, st.episodeStartMs);
  TEST_ASSERT_EQUAL_UINT32(0, st.deferralsAtStart);
  TEST_ASSERT_EQUAL_UINT32(1, st.drainedDeferrals);
  // Un 2ᵉ refus ne réémet RIEN : un seul WARN, même pour 200 refus (le journal
  // persistant fait 8 Ko, sur-journaliser détruirait la fenêtre forensique).
  TEST_ASSERT_EQUAL(FopenGuardAction::None,
                    fopenGuardDrain(st, 2, 1100, kMesureAbsente,
                                    kStaticFileMinAllocBytes,
                                    kInternalFopenEscalateAfterRefusals));
}

static void test_rafale_ne_produit_qu_une_ouverture_et_une_escalade(void) {
  FopenGuardEpisode st;
  int starts = 0, escalades = 0, autres = 0;
  for (uint32_t n = 1; n <= 200; ++n) {
    const FopenGuardAction a =
        fopenGuardDrain(st, n, 1000 + n, kMesureAbsente,
                        kStaticFileMinAllocBytes,
                        kInternalFopenEscalateAfterRefusals);
    if (a == FopenGuardAction::EpisodeStart) starts++;
    else if (a == FopenGuardAction::Escalate) escalades++;
    else if (a != FopenGuardAction::None) autres++;
  }
  TEST_ASSERT_EQUAL_INT(1, starts);
  TEST_ASSERT_EQUAL_INT(1, escalades);
  TEST_ASSERT_EQUAL_INT(0, autres);
}

static void test_escalade_au_Nieme_refus_exactement(void) {
  // Le seuil d'escalade est LU dans constants.h. Si quelqu'un le déplace, ce
  // test meurt — c'est son unique raison d'être.
  TEST_ASSERT_EQUAL_UINT32(5, kInternalFopenEscalateAfterRefusals);
  FopenGuardEpisode st;
  TEST_ASSERT_EQUAL(FopenGuardAction::EpisodeStart,
                    fopenGuardDrain(st, 1, 1000, kMesureAbsente,
                                    kStaticFileMinAllocBytes,
                                    kInternalFopenEscalateAfterRefusals));
  // Refus 2, 3, 4 : rien. (Avec un seuil abaissé à 3 ou 4, ces lignes meurent.)
  for (uint32_t n = 2; n <= kInternalFopenEscalateAfterRefusals - 1; ++n) {
    TEST_ASSERT_EQUAL(FopenGuardAction::None,
                      fopenGuardDrain(st, n, 1000 + n, kMesureAbsente,
                                      kStaticFileMinAllocBytes,
                                      kInternalFopenEscalateAfterRefusals));
  }
  // Refus 5 : escalade. (Avec un seuil relevé à 6, cette ligne meurt.)
  TEST_ASSERT_EQUAL(FopenGuardAction::Escalate,
                    fopenGuardDrain(st, kInternalFopenEscalateAfterRefusals,
                                    2000, kMesureAbsente,
                                    kStaticFileMinAllocBytes,
                                    kInternalFopenEscalateAfterRefusals));
  TEST_ASSERT_TRUE(st.escalated);
  // Et une seule fois.
  TEST_ASSERT_EQUAL(FopenGuardAction::None,
                    fopenGuardDrain(st, kInternalFopenEscalateAfterRefusals + 1,
                                    2100, kMesureAbsente,
                                    kStaticFileMinAllocBytes,
                                    kInternalFopenEscalateAfterRefusals));
}

static void test_escalade_desactivee_si_seuil_nul(void) {
  FopenGuardEpisode st;
  fopenGuardDrain(st, 1, 1000, kMesureAbsente, kStaticFileMinAllocBytes, 0);
  for (uint32_t n = 2; n <= 50; ++n) {
    TEST_ASSERT_EQUAL(FopenGuardAction::None,
                      fopenGuardDrain(st, n, 1000 + n, kMesureAbsente,
                                      kStaticFileMinAllocBytes, 0));
  }
}

static void test_compteur_de_refus_avance_reellement(void) {
  // drainedDeferrals doit SUIVRE le compteur cumulé. S'il se fige, l'épisode ne
  // peut plus jamais se clore (la règle « compteur inchangé » devient
  // inatteignable) : l'appareil resterait sans historique jusqu'au reboot.
  FopenGuardEpisode st;
  fopenGuardDrain(st, 1, 1000, kMesureAbsente, kStaticFileMinAllocBytes,
                  kInternalFopenEscalateAfterRefusals);
  fopenGuardDrain(st, 7, 1100, kMesureAbsente, kStaticFileMinAllocBytes,
                  kInternalFopenEscalateAfterRefusals);
  TEST_ASSERT_EQUAL_UINT32(7, st.drainedDeferrals);
  TEST_ASSERT_EQUAL_UINT32(1100, st.lastDeferralMs);
  fopenGuardDrain(st, 12, 1200, kMesureAbsente, kStaticFileMinAllocBytes,
                  kInternalFopenEscalateAfterRefusals);
  TEST_ASSERT_EQUAL_UINT32(12, st.drainedDeferrals);
}

static void test_front_descendant_ferme_sur_mesure_fraiche(void) {
  FopenGuardEpisode st;
  fopenGuardNoteRefusal(st, 1000);  // fsGuardAdmit() au 1er refus réel
  fopenGuardDrain(st, 1, 1000, kMesureAbsente, kStaticFileMinAllocBytes,
                  kInternalFopenEscalateAfterRefusals);
  fopenGuardDrain(st, 3, 1100, kMesureAbsente, kStaticFileMinAllocBytes,
                  kInternalFopenEscalateAfterRefusals);
  // Compteur figé + mesure FRAÎCHE au-dessus du seuil → fermeture.
  TEST_ASSERT_EQUAL(FopenGuardAction::EpisodeEnd,
                    fopenGuardDrain(st, 3, 40000, kTerrainPlateauCalme,
                                    kStaticFileMinAllocBytes,
                                    kInternalFopenEscalateAfterRefusals));
  TEST_ASSERT_FALSE(st.active);
  TEST_ASSERT_FALSE(st.probeFloorArmed);
  // Les repères de l'épisode CLOS survivent : la coquille en tire la durée et le
  // total qu'elle journalise.
  TEST_ASSERT_EQUAL_UINT32(1000, st.episodeStartMs);
  TEST_ASSERT_EQUAL_UINT32(0, st.deferralsAtStart);
  // Et le front descendant ne se rejoue pas.
  TEST_ASSERT_EQUAL(FopenGuardAction::None,
                    fopenGuardDrain(st, 3, 40100, kTerrainPlateauCalme,
                                    kStaticFileMinAllocBytes,
                                    kInternalFopenEscalateAfterRefusals));
}

static void test_fermeture_exige_le_seuil_pas_un_octet_de_moins(void) {
  FopenGuardEpisode st;
  fopenGuardNoteRefusal(st, 1000);
  fopenGuardDrain(st, 1, 1000, kMesureAbsente, kStaticFileMinAllocBytes,
                  kInternalFopenEscalateAfterRefusals);
  // Un octet sous le seuil : l'épisode tient.
  TEST_ASSERT_EQUAL(FopenGuardAction::None,
                    fopenGuardDrain(st, 1, 40000, kStaticFileMinAllocBytes - 1,
                                    kStaticFileMinAllocBytes,
                                    kInternalFopenEscalateAfterRefusals));
  TEST_ASSERT_TRUE(st.active);
  // Au seuil exact : il se ferme.
  TEST_ASSERT_EQUAL(FopenGuardAction::EpisodeEnd,
                    fopenGuardDrain(st, 1, 40100, kStaticFileMinAllocBytes,
                                    kStaticFileMinAllocBytes,
                                    kInternalFopenEscalateAfterRefusals));
}

static void test_deux_episodes_successifs_rebasent_le_compteur(void) {
  FopenGuardEpisode st;
  fopenGuardNoteRefusal(st, 1000);  // fsGuardAdmit() au 1er refus du 1er épisode
  fopenGuardDrain(st, 1, 1000, kMesureAbsente, kStaticFileMinAllocBytes,
                  kInternalFopenEscalateAfterRefusals);
  for (uint32_t n = 2; n <= 9; ++n) {
    fopenGuardDrain(st, n, 1000 + n, kMesureAbsente, kStaticFileMinAllocBytes,
                    kInternalFopenEscalateAfterRefusals);
  }
  TEST_ASSERT_TRUE(st.escalated);
  TEST_ASSERT_EQUAL(FopenGuardAction::EpisodeEnd,
                    fopenGuardDrain(st, 9, 50000, kTerrainPlateauCalme,
                                    kStaticFileMinAllocBytes,
                                    kInternalFopenEscalateAfterRefusals));
  // 2ᵉ épisode : nouveau front montant (fsGuardAdmit() au 1er refus du 2ᵉ
  // épisode, `active` était retombé à false à la fermeture précédente) — la
  // baseline repart de 9, l'escalade est réarmée.
  fopenGuardNoteRefusal(st, 60000);
  TEST_ASSERT_EQUAL(FopenGuardAction::EpisodeStart,
                    fopenGuardDrain(st, 10, 60000, kMesureAbsente,
                                    kStaticFileMinAllocBytes,
                                    kInternalFopenEscalateAfterRefusals));
  TEST_ASSERT_EQUAL_UINT32(9, st.deferralsAtStart);
  TEST_ASSERT_FALSE(st.escalated);
  TEST_ASSERT_EQUAL_UINT32(60000, st.episodeStartMs);
  // Le 2ᵉ épisode escalade à son propre 5ᵉ refus, pas plus tôt.
  for (uint32_t n = 11; n <= 13; ++n) {
    TEST_ASSERT_EQUAL(FopenGuardAction::None,
                      fopenGuardDrain(st, n, 60000 + n, kMesureAbsente,
                                      kStaticFileMinAllocBytes,
                                      kInternalFopenEscalateAfterRefusals));
  }
  TEST_ASSERT_EQUAL(FopenGuardAction::Escalate,
                    fopenGuardDrain(st, 14, 60100, kMesureAbsente,
                                    kStaticFileMinAllocBytes,
                                    kInternalFopenEscalateAfterRefusals));
}

static void test_compteur_replie_reste_juste(void) {
  // uint32 monotone : la soustraction non signée survit au repliement.
  FopenGuardEpisode st;
  fopenGuardNoteRefusal(st, 1000);
  fopenGuardDrain(st, 0xFFFFFFFEu, 1000, kMesureAbsente,
                  kStaticFileMinAllocBytes, kInternalFopenEscalateAfterRefusals);
  TEST_ASSERT_TRUE(st.active);
  st.deferralsAtStart = 0xFFFFFFFDu;  // baseline juste avant l'épisode
  TEST_ASSERT_EQUAL(FopenGuardAction::None,
                    fopenGuardDrain(st, 0xFFFFFFFFu, 1100, kMesureAbsente,
                                    kStaticFileMinAllocBytes,
                                    kInternalFopenEscalateAfterRefusals));
  TEST_ASSERT_EQUAL(FopenGuardAction::None,
                    fopenGuardDrain(st, 0x00000000u, 1200, kMesureAbsente,
                                    kStaticFileMinAllocBytes,
                                    kInternalFopenEscalateAfterRefusals));
  // 0 - 0xFFFFFFFD = 3 refus cumulés → toujours sous le seuil de 5.
  TEST_ASSERT_FALSE(st.escalated);
  TEST_ASSERT_EQUAL(FopenGuardAction::None,
                    fopenGuardDrain(st, 0x00000001u, 1300, kMesureAbsente,
                                    kStaticFileMinAllocBytes,
                                    kInternalFopenEscalateAfterRefusals));
  TEST_ASSERT_EQUAL(FopenGuardAction::Escalate,
                    fopenGuardDrain(st, 0x00000002u, 1400, kMesureAbsente,
                                    kStaticFileMinAllocBytes,
                                    kInternalFopenEscalateAfterRefusals));
}

// ===========================================================================
// T5 — LE CŒUR : aucun épisode ne peut se clore sur une mesure PÉRIMÉE
// ===========================================================================
// Ces tests protègent la moitié NON NÉGOCIABLE de l'asymétrie. Une régression
// qui « symétriserait » le cache — par souci de cohérence, ce qui est
// exactement la tentation — rendrait au firmware l'abort() du 05/09/2026.

static void test_episode_ne_se_clot_jamais_sans_mesure(void) {
  // La coquille passe 0 quand elle n'a PAS mesuré. 0 est toujours sous le seuil
  // ⇒ pas de fermeture. Même après des heures de silence.
  FopenGuardEpisode st;
  fopenGuardNoteRefusal(st, 1000);
  fopenGuardDrain(st, 1, 1000, kMesureAbsente, kStaticFileMinAllocBytes,
                  kInternalFopenEscalateAfterRefusals);
  for (uint32_t t = 2000; t <= 3600000; t += 60000) {
    TEST_ASSERT_EQUAL(FopenGuardAction::None,
                      fopenGuardDrain(st, 1, t, kMesureAbsente,
                                      kStaticFileMinAllocBytes,
                                      kInternalFopenEscalateAfterRefusals));
    TEST_ASSERT_TRUE(st.active);
  }
}

static void test_silence_seul_n_autorise_rien(void) {
  // Le silence est une CONDITION DE RE-MESURE, jamais une autorisation. Aucun
  // délai, si long soit-il, ne peut fermer l'épisode sans un chiffre frais.
  FopenGuardEpisode st;
  fopenGuardNoteRefusal(st, 1000);
  fopenGuardDrain(st, 4, 1000, kMesureAbsente, kStaticFileMinAllocBytes,
                  kInternalFopenEscalateAfterRefusals);
  const uint32_t bienApres = 1000 + 100 * kInternalFopenEpisodeQuietMs;
  TEST_ASSERT_EQUAL(FopenGuardAction::None,
                    fopenGuardDrain(st, 4, bienApres, kMesureAbsente,
                                    kStaticFileMinAllocBytes,
                                    kInternalFopenEscalateAfterRefusals));
  TEST_ASSERT_TRUE(st.active);
}

static void test_mesure_fraiche_mais_encore_affamee_ne_clot_pas(void) {
  FopenGuardEpisode st;
  fopenGuardNoteRefusal(st, 1000);
  fopenGuardDrain(st, 1, 1000, kMesureAbsente, kStaticFileMinAllocBytes,
                  kInternalFopenEscalateAfterRefusals);
  TEST_ASSERT_EQUAL(FopenGuardAction::None,
                    fopenGuardDrain(st, 1, 90000, kTerrainPlancherAbsolu,
                                    kStaticFileMinAllocBytes,
                                    kInternalFopenEscalateAfterRefusals));
  TEST_ASSERT_TRUE(st.active);
}

static void test_composition_le_drain_ne_ferme_que_quand_il_a_mesure(void) {
  // Reproduction FIDÈLE de la boucle de fsGuardDrain() : elle ne mesure que si
  // fopenGuardNeedsFreshMeasure() le dit, et passe 0 sinon. On simule un tas
  // REVENU AU CONFORT dès la 1re milliseconde, et l'on vérifie que la sortie
  // n'a lieu qu'après la fenêtre de silence — jamais avant, faute de mesure.
  FopenGuardEpisode st;
  fopenGuardNoteRefusal(st, 1000);
  fopenGuardDrain(st, 1, 1000, kMesureAbsente, kStaticFileMinAllocBytes,
                  kInternalFopenEscalateAfterRefusals);
  st.probeFloorArmed = true;   // la coquille l'a armé : dernier relevé < 2×seuil
  st.lastProbeMs = 1000;
  st.lastDeferralMs = 1000;

  const uint32_t tasReel = kTerrainPlateauCalme;  // confortable, mais NON MESURÉ
  bool ferme = false;
  uint32_t tFermeture = 0;
  for (uint32_t now = 1001; now <= 1000 + 2 * kInternalFopenEpisodeQuietMs;
       now += 10) {
    uint32_t maxAlloc8 = 0;
    if (st.active && fopenGuardNeedsFreshMeasure(st, now,
                                                 kInternalFopenEpisodeQuietMs,
                                                 kInternalFopenProbeMinIntervalMs)) {
      maxAlloc8 = tasReel;          // parcours du tas payé
      st.lastProbeMs = now;
      st.probeFloorArmed = (maxAlloc8 < 2u * kStaticFileMinAllocBytes);
    }
    if (fopenGuardDrain(st, 1, now, maxAlloc8, kStaticFileMinAllocBytes,
                        kInternalFopenEscalateAfterRefusals) ==
        FopenGuardAction::EpisodeEnd) {
      ferme = true;
      tFermeture = now;
      break;
    }
  }
  TEST_ASSERT_TRUE(ferme);
  // Pas avant la fenêtre de silence : le tas était bon dès 1001 ms, mais nul ne
  // le savait. C'est le prix ASSUMÉ du refus périmé — 30 s, pas un abort().
  TEST_ASSERT_TRUE(tFermeture >= 1000 + kInternalFopenEpisodeQuietMs);
}

static void test_composition_un_tas_toujours_affame_ne_sort_jamais(void) {
  FopenGuardEpisode st;
  fopenGuardNoteRefusal(st, 1000);
  fopenGuardDrain(st, 1, 1000, kMesureAbsente, kStaticFileMinAllocBytes,
                  kInternalFopenEscalateAfterRefusals);
  st.probeFloorArmed = true;
  st.lastProbeMs = 1000;
  st.lastDeferralMs = 1000;
  for (uint32_t now = 1001; now <= 600000; now += 250) {
    uint32_t maxAlloc8 = 0;
    if (st.active && fopenGuardNeedsFreshMeasure(st, now,
                                                 kInternalFopenEpisodeQuietMs,
                                                 kInternalFopenProbeMinIntervalMs)) {
      maxAlloc8 = kTerrainPlancherAbsolu;
      st.lastProbeMs = now;
      st.probeFloorArmed = (maxAlloc8 < 2u * kStaticFileMinAllocBytes);
    }
    TEST_ASSERT_EQUAL(FopenGuardAction::None,
                      fopenGuardDrain(st, 1, now, maxAlloc8,
                                      kStaticFileMinAllocBytes,
                                      kInternalFopenEscalateAfterRefusals));
  }
  TEST_ASSERT_TRUE(st.active);
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  // T1 — prédicat et frontière stricte
  RUN_TEST(test_frontiere_stricte_le_seuil_lui_meme_est_admis);
  RUN_TEST(test_frontiere_stricte_classe_A_aussi);
  RUN_TEST(test_frontieres_en_valeurs_absolues);
  RUN_TEST(test_tas_effondre_toujours_refuse_dans_les_deux_classes);
  RUN_TEST(test_reperes_terrain_confortables_sont_admis);
  RUN_TEST(test_admission_deja_prononcee_n_est_jamais_refusee);
  // T2 — calibrage des deux classes + échelle ADR-0058
  RUN_TEST(test_classe_B_couvre_le_besoin_dur_avec_marge);
  RUN_TEST(test_classe_A_couvre_le_besoin_avec_contention_tcp);
  RUN_TEST(test_echelle_ADR0058_reste_ordonnee_a_sept_barreaux);
  RUN_TEST(test_tripwire_de_boot_est_atteignable_avant_le_reboot);
  // T3 — asymétrie mesure/décision
  RUN_TEST(test_hors_episode_la_mesure_est_toujours_due);
  RUN_TEST(test_episode_ouvert_mais_tas_confortable_re_mesure_toujours);
  RUN_TEST(test_plancher_de_sonde_refuse_sans_mesurer);
  RUN_TEST(test_plancher_de_sonde_PRIME_sur_le_silence);
  RUN_TEST(test_plancher_de_sonde_borne_le_nombre_de_parcours_de_tas);
  RUN_TEST(test_plancher_de_sonde_frontiere_exacte);
  RUN_TEST(test_rafale_en_cours_ne_paie_jamais_le_tas);
  RUN_TEST(test_silence_frontiere_exacte);
  RUN_TEST(test_asymetrie_le_cache_ne_produit_que_des_refus);
  RUN_TEST(test_repliement_de_millis_ne_relache_pas_la_garde);
  // T3bis — fopenGuardNoteRefusal() en isolation
  RUN_TEST(test_note_refusal_pose_active_et_episode_start_ms_au_premier_appel);
  RUN_TEST(test_note_refusal_ne_re_stampe_pas_episode_start_ms_ensuite);
  RUN_TEST(test_note_refusal_rouvre_un_nouveau_front_montant_apres_fermeture);
  RUN_TEST(test_episode_duration_ms_mesure_depuis_le_debut_de_l_episode);
  RUN_TEST(test_episode_duration_ms_survit_au_repliement_de_millis);
  // T4 — machine à états
  RUN_TEST(test_front_montant_ouvre_l_episode_une_seule_fois);
  RUN_TEST(test_rafale_ne_produit_qu_une_ouverture_et_une_escalade);
  RUN_TEST(test_escalade_au_Nieme_refus_exactement);
  RUN_TEST(test_escalade_desactivee_si_seuil_nul);
  RUN_TEST(test_compteur_de_refus_avance_reellement);
  RUN_TEST(test_front_descendant_ferme_sur_mesure_fraiche);
  RUN_TEST(test_fermeture_exige_le_seuil_pas_un_octet_de_moins);
  RUN_TEST(test_deux_episodes_successifs_rebasent_le_compteur);
  RUN_TEST(test_compteur_replie_reste_juste);
  // T5 — aucune fermeture sur mesure périmée
  RUN_TEST(test_episode_ne_se_clot_jamais_sans_mesure);
  RUN_TEST(test_silence_seul_n_autorise_rien);
  RUN_TEST(test_mesure_fraiche_mais_encore_affamee_ne_clot_pas);
  RUN_TEST(test_composition_le_drain_ne_ferme_que_quand_il_a_mesure);
  RUN_TEST(test_composition_un_tas_toujours_affame_ne_sort_jamais);
  return UNITY_END();
}
