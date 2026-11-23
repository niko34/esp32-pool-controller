// =============================================================================
// Tests natifs — RENDEMENT DU CHLORE APPRIS (feature-203)
// Cible : src/orp_yield_learn_logic.h (module PUR, ADR-0017).
//
// Deux familles :
//  - A..H : comportement du module pur (vecteurs numériques, bornes).
//  - S    : GARDES LEXICALES sur src/pump_controller.cpp. Le cycle de vie de
//           l'épisode (jets) et la condition d'arrêt vivent dans ce fichier, hors
//           du build natif ; sans extraction elles ne sont atteignables que par
//           ancrage de texte (même précédent : test_native_ws_data_parity).
//           Chaque garde cible une MUTATION précise (voir rapport de passage).
//
// ⛔ Les valeurs 456 mL / +106 mV / 51 mV sont des VECTEURS DE TEST (relevé du
// 18/11), jamais des valeurs d'usine : le test S0 échoue si on les retrouve
// dans constants.h.
// =============================================================================
#include <unity.h>
#include <math.h>
#include <stdio.h>
#include <string>
#include <map>
#include <regex>

#include "orp_yield_learn_logic.h"

void setUp(void) {}
void tearDown(void) {}

// --- Utilitaires -------------------------------------------------------------
static OrpYieldStore learn3(float y0, float y1, float y2, uint16_t d0, uint16_t d1, uint16_t d2) {
  OrpYieldStore s;
  OrpYieldVerdict v;
  v.kind = OrpYieldKind::kCredited;
  const float ys[3] = {y0, y1, y2};
  const uint16_t ds[3] = {d0, d1, d2};
  for (int i = 0; i < 3; ++i) {
    v.yieldMvPerMl = ys[i];
    v.openDeficitMv = ds[i];
    orpYieldRecord(s, v);
  }
  return s;
}

// Épisode complet : ouverture à (consigne - deficit), volume, plateau.
static OrpYieldVerdict runEpisode(float openOrp, float deficit, float vol, float plateau) {
  OrpYieldEpisode e;
  orpYieldOpen(e, openOrp, deficit);
  orpYieldAddVolume(e, vol);
  return orpYieldClose(e, plateau);
}

static std::string readFile(const char* path) {
  std::string out;
  FILE* fp = fopen(path, "rb");
  if (!fp) return out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
  fclose(fp);
  return out;
}

// Extrait [from, to) borné par deux ancres ; vide si introuvable.
static std::string between(const std::string& s, const char* from, const char* to) {
  const size_t a = s.find(from);
  if (a == std::string::npos) return "";
  const size_t b = s.find(to, a);
  if (b == std::string::npos) return "";
  return s.substr(a, b - a);
}

// --- A : vecteur arithmétique (D6) -------------------------------------------
void test_D6_vecteur_18_11_rendement_et_budget(void) {
  const OrpYieldVerdict v = runEpisode(479.0f, 51.0f, 456.0f, 585.0f);
  TEST_ASSERT_TRUE(v.kind == OrpYieldKind::kCredited);
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 0.2325f, v.yieldMvPerMl);  // 106/456
  const OrpYieldStore s = learn3(v.yieldMvPerMl, v.yieldMvPerMl, v.yieldMvPerMl, 51, 51, 51);
  const float b = orpYieldBudgetMl(51.0f, s);
  // marge 2,0 : 2 x 51 / 0,23246 = 438,8 mL. (La fiche annonce 219 : chiffre écrit AVANT
  // la décision de marge ; 219 serait le budget à marge 1, que C2 interdit.)
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 439.0f, b);
  TEST_ASSERT_TRUE(fabsf(b - 219.0f) > 100.0f);
}

void test_D6_aucune_valeur_du_vecteur_dans_constants_h(void) {
  const std::string c = readFile("src/constants.h");
  TEST_ASSERT_TRUE(c.size() > 1000);
  TEST_ASSERT_TRUE(c.find("0.2325") == std::string::npos);
  TEST_ASSERT_TRUE(c.find("0.23f") == std::string::npos);
  TEST_ASSERT_TRUE(c.find("456.0f") == std::string::npos);
}

// --- B : D5 apprentissage absent => comportement d'aujourd'hui ---------------
void test_D5_sous_trois_episodes_aucun_budget(void) {
  OrpYieldStore s;
  for (int n = 0; n < 3; ++n) {
    TEST_ASSERT_TRUE(isnan(orpYieldBudgetMl(30.0f, s)));
    TEST_ASSERT_FALSE(orpYieldBudgetReached(orpYieldBudgetMl(30.0f, s), 1.0e6f));
    OrpYieldVerdict v;
    v.kind = OrpYieldKind::kCredited;
    v.yieldMvPerMl = 0.2f;
    v.openDeficitMv = 60;
    orpYieldRecord(s, v);
  }
  TEST_ASSERT_TRUE(orpYieldIsLearned(s));
  TEST_ASSERT_FALSE(isnan(orpYieldBudgetMl(30.0f, s)));
}

void test_D5_episode_jete_ne_compte_pas(void) {
  OrpYieldStore s;
  const OrpYieldVerdict j = runEpisode(479.0f, 51.0f, 456.0f, 470.0f);  // pas de montée
  orpYieldRecord(s, j);
  TEST_ASSERT_EQUAL_UINT8(0, s.count);
}

// --- C : NaN, montée <= 0, volume mini (D12 D13 D14) -------------------------
void test_D12_mesure_nan_budget_nan_jamais_zero(void) {
  const OrpYieldStore s = learn3(0.2f, 0.2f, 0.2f, 80, 80, 80);
  const float b = orpYieldBudgetMl(NAN, s);
  TEST_ASSERT_TRUE(isnan(b));
  // Un budget 0 serait « atteint » dès le volume 0 : panne déguisée.
  TEST_ASSERT_FALSE(orpYieldBudgetReached(b, 0.0f));
  TEST_ASSERT_FALSE(orpYieldBudgetReached(b, 1.0e6f));
  TEST_ASSERT_TRUE(isnan(orpYieldBudgetMl(0.0f, s)));
  TEST_ASSERT_TRUE(isnan(orpYieldBudgetMl(-5.0f, s)));
}

void test_D12_ouverture_sur_mesure_nan_episode_jete(void) {
  const OrpYieldVerdict v = runEpisode(NAN, NAN, 100.0f, 585.0f);
  TEST_ASSERT_TRUE(v.kind == OrpYieldKind::kDiscarded);
  TEST_ASSERT_TRUE(v.reason == OrpYieldReason::kDirty);
}

void test_D13_montee_nulle_ou_negative_jete_sans_division(void) {
  const float plateaux[3] = {479.0f, 470.0f, NAN};
  for (int i = 0; i < 3; ++i) {
    const OrpYieldVerdict v = runEpisode(479.0f, 51.0f, 100.0f, plateaux[i]);
    TEST_ASSERT_TRUE(v.kind == OrpYieldKind::kDiscarded);
    TEST_ASSERT_TRUE(v.reason == (i == 2 ? OrpYieldReason::kInvalid : OrpYieldReason::kNoRise));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.yieldMvPerMl);
  }
}

void test_D14_volume_sous_plus_petite_dose_jete(void) {
  const OrpYieldVerdict bas = runEpisode(479.0f, 51.0f, kPulseMinVolumeMl * 0.99f, 500.0f);
  TEST_ASSERT_TRUE(bas.kind == OrpYieldKind::kDiscarded);
  TEST_ASSERT_TRUE(bas.reason == OrpYieldReason::kTooSmall);
  const OrpYieldVerdict ok = runEpisode(479.0f, 51.0f, kPulseMinVolumeMl, 500.0f);
  TEST_ASSERT_TRUE(ok.kind == OrpYieldKind::kCredited);
}

// --- D : D3 écrêtage haut ----------------------------------------------------
void test_D3_ecretage_haut_du_rendement(void) {
  const float vol = kPulseMinVolumeMl * 1.01f;
  const OrpYieldVerdict v = runEpisode(400.0f, 100.0f, vol, 400.0f + 900.0f);  // ~73 mV/mL
  TEST_ASSERT_TRUE(v.kind == OrpYieldKind::kCredited);
  TEST_ASSERT_TRUE(v.capped);
  TEST_ASSERT_EQUAL_FLOAT(kOrpYieldMaxMvPerMl, v.yieldMvPerMl);
  const OrpYieldVerdict n = runEpisode(479.0f, 51.0f, 456.0f, 585.0f);
  TEST_ASSERT_FALSE(n.capped);
}

void test_D3_pas_de_borne_basse(void) {
  const OrpYieldVerdict v = runEpisode(479.0f, 51.0f, 5000.0f, 480.0f);  // 0,0002 mV/mL
  TEST_ASSERT_TRUE(v.kind == OrpYieldKind::kCredited);
  TEST_ASSERT_FALSE(v.capped);
  TEST_ASSERT_TRUE(v.yieldMvPerMl < 0.001f);
}

void test_D3_snapshot_refuse_rendement_au_dessus_du_plafond(void) {
  OrpYieldStore s = learn3(0.2f, 0.2f, 0.2f, 50, 50, 50);
  s.yieldMvPerMl[2] = kOrpYieldMaxMvPerMl * 2.0f;
  OrpYieldSnapshot snap = orpYieldSnapshotFromStore(s, 1234u);
  TEST_ASSERT_FALSE(orpYieldSnapshotIntact(snap, sizeof(snap)));
}

// --- E : D7 domaine d'application, médiane et non max ------------------------
void test_D7_budget_seulement_jusqu_a_la_mediane_des_manques(void) {
  const OrpYieldStore s = learn3(0.2f, 0.2f, 0.2f, 30, 50, 80);  // médiane 50 (max 80)
  TEST_ASSERT_FALSE(isnan(orpYieldBudgetMl(50.0f, s)));
  TEST_ASSERT_TRUE(isnan(orpYieldBudgetMl(50.5f, s)));
  TEST_ASSERT_TRUE(isnan(orpYieldBudgetMl(70.0f, s)));   // dans le max, hors médiane
  TEST_ASSERT_FALSE(isnan(orpYieldBudgetMl(10.0f, s)));
}

void test_mediane_toutes_permutations(void) {
  const float v[6][3] = {{1, 2, 3}, {1, 3, 2}, {2, 1, 3}, {2, 3, 1}, {3, 1, 2}, {3, 2, 1}};
  for (int i = 0; i < 6; ++i) TEST_ASSERT_EQUAL_FLOAT(2.0f, orpYieldMid3(v[i][0], v[i][1], v[i][2]));
  TEST_ASSERT_EQUAL_FLOAT(5.0f, orpYieldMid3(5, 5, 9));
  TEST_ASSERT_EQUAL_FLOAT(5.0f, orpYieldMid3(1, 5, 5));
}

void test_anneau_le_4e_chasse_le_1er(void) {
  OrpYieldStore s = learn3(0.1f, 0.2f, 0.3f, 10, 20, 30);
  OrpYieldVerdict v;
  v.kind = OrpYieldKind::kCredited;
  v.yieldMvPerMl = 0.4f;
  v.openDeficitMv = 40;
  orpYieldRecord(s, v);
  TEST_ASSERT_EQUAL_FLOAT(0.3f, orpYieldMedian(s));  // {0.2,0.3,0.4}
  TEST_ASSERT_EQUAL_UINT8(4, s.count);
}

// --- F : D9 recalcul sur le manque COURANT (pur) -----------------------------
void test_D9_budget_suit_le_manque_courant(void) {
  const OrpYieldStore s = learn3(0.2f, 0.2f, 0.2f, 80, 80, 80);
  const float b51 = orpYieldBudgetMl(51.0f, s);
  const float b70 = orpYieldBudgetMl(70.0f, s);
  const float b20 = orpYieldBudgetMl(20.0f, s);
  TEST_ASSERT_TRUE(b70 > b51);
  TEST_ASSERT_TRUE(b20 < b51);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.0f * 51.0f / 0.2f, b51);
  // Épisode qui ne se ferme pas : volume resté à budget(51). Si le manque remonte à 70
  // (consommation), la chloration auto DOIT reprendre. Un budget figé la bloquerait.
  TEST_ASSERT_TRUE(orpYieldBudgetReached(b51, b51));
  TEST_ASSERT_FALSE(orpYieldBudgetReached(b70, b51));
}

// --- G : D2 le budget ne fait qu'arrêter (pur) -------------------------------
void test_D2_budget_atteint_est_monotone_et_jamais_avant_le_volume(void) {
  const float b = 100.0f;
  TEST_ASSERT_FALSE(orpYieldBudgetReached(b, 0.0f));
  TEST_ASSERT_FALSE(orpYieldBudgetReached(b, 99.9f));
  TEST_ASSERT_TRUE(orpYieldBudgetReached(b, 100.0f));
  TEST_ASSERT_TRUE(orpYieldBudgetReached(b, 1.0e6f));
  TEST_ASSERT_FALSE(orpYieldBudgetReached(NAN, 1.0e6f));
  TEST_ASSERT_FALSE(orpYieldBudgetReached(INFINITY, 1.0e6f));
}

// --- H : D8 biais de comptage +6 % -------------------------------------------
void test_D8_biais_de_comptage_cote_prudent(void) {
  const float volPhys = 456.0f, rise = 106.0f, def = 51.0f;
  const float yTrue = rise / volPhys;
  const float bTrue = orpYieldBudgetMl(def, learn3(yTrue, yTrue, yTrue, 51, 51, 51));
  // Apprentissage sur un compteur qui SURcompte de 6 % : rendement pessimiste.
  const OrpYieldVerdict vb = runEpisode(479.0f, def, volPhys * 1.06f, 479.0f + rise);
  const float bBiased = orpYieldBudgetMl(def, learn3(vb.yieldMvPerMl, vb.yieldMvPerMl, vb.yieldMvPerMl, 51, 51, 51));
  TEST_ASSERT_TRUE(vb.yieldMvPerMl < yTrue);
  // Appliqué avec un compteur exact : on s'arrête PLUS TARD (côté sous-dosage évité), jamais avant.
  TEST_ASSERT_FLOAT_WITHIN(0.5f, bTrue * 1.06f, bBiased);
  TEST_ASSERT_TRUE(bBiased >= bTrue);
  // Biais identique à l'apprentissage et à l'application : il s'annule (volume physique égal).
  TEST_ASSERT_FLOAT_WITHIN(0.5f, bTrue, bBiased / 1.06f);
}

// --- I : D10 sous-dosage chronique -------------------------------------------
void test_D10_trois_arrets_budget_sans_consigne_alerte(void) {
  uint8_t st = 0;
  st = orpYieldStreakNext(st, true, false);
  st = orpYieldStreakNext(st, true, false);
  TEST_ASSERT_FALSE(orpYieldUndersupply(st));
  st = orpYieldStreakNext(st, true, false);
  TEST_ASSERT_TRUE(orpYieldUndersupply(st));
}

void test_D10_consigne_atteinte_remet_a_zero(void) {
  uint8_t st = 0;
  st = orpYieldStreakNext(st, true, false);
  st = orpYieldStreakNext(st, true, false);
  st = orpYieldStreakNext(st, false, true);
  TEST_ASSERT_EQUAL_UINT8(0, st);
  st = orpYieldStreakNext(st, true, false);
  TEST_ASSERT_FALSE(orpYieldUndersupply(st));
  // Une séance qui n'est pas arrêtée par le budget ne casse pas la suite.
  st = orpYieldStreakNext(2, false, false);
  TEST_ASSERT_EQUAL_UINT8(2, st);
}

// --- J : D11 (pur) épisode jamais ouvert / sale ------------------------------
void test_D11_cloture_d_un_episode_jamais_ouvert_est_jetee(void) {
  OrpYieldEpisode e;
  orpYieldAddVolume(e, 100.0f);
  const OrpYieldVerdict v = orpYieldClose(e, 600.0f);
  TEST_ASSERT_TRUE(v.kind == OrpYieldKind::kDiscarded);
}

void test_cloture_remet_l_episode_a_zero(void) {
  OrpYieldEpisode e;
  orpYieldOpen(e, 479.0f, 51.0f);
  orpYieldAddVolume(e, 100.0f);
  (void)orpYieldClose(e, 585.0f);
  TEST_ASSERT_FALSE(e.open);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, e.volMl);
}

// --- K : persistance ---------------------------------------------------------
void test_snapshot_aller_retour_et_corruptions(void) {
  const OrpYieldStore s = learn3(0.1f, 0.2f, 0.3f, 10, 20, 30);
  const uint32_t ctx = orpYieldContextHash(530.0f, 1);
  OrpYieldSnapshot snap = orpYieldSnapshotFromStore(s, ctx);
  TEST_ASSERT_TRUE(orpYieldSnapshotIntact(snap, sizeof(snap)));
  const OrpYieldStore r = orpYieldStoreFromSnapshot(snap);
  TEST_ASSERT_EQUAL_FLOAT(0.2f, orpYieldMedian(r));
  TEST_ASSERT_FALSE(orpYieldSnapshotIntact(snap, sizeof(snap) - 1));
  OrpYieldSnapshot bad = snap;
  bad.yieldMvPerMl[1] = 0.9f;  // CRC non rescellé
  TEST_ASSERT_FALSE(orpYieldSnapshotIntact(bad, sizeof(bad)));
  OrpYieldSnapshot nan = snap;
  nan.yieldMvPerMl[2] = NAN;
  orpYieldSealSnapshot(nan);
  TEST_ASSERT_FALSE(orpYieldSnapshotIntact(nan, sizeof(nan)));
  OrpYieldSnapshot sch = snap;
  sch.schema = (uint8_t)(kOrpYieldLearnSchema + 1);
  orpYieldSealSnapshot(sch);
  TEST_ASSERT_FALSE(orpYieldSnapshotIntact(sch, sizeof(sch)));
}

void test_empreinte_de_contexte(void) {
  const uint32_t a = orpYieldContextHash(530.0f, 1);
  TEST_ASSERT_EQUAL_UINT32(a, orpYieldContextHash(530.0f, 1));
  TEST_ASSERT_TRUE(a != orpYieldContextHash(540.0f, 1));
  TEST_ASSERT_TRUE(a != orpYieldContextHash(530.0f, 2));
}

// --- S : GARDES LEXICALES sur pump_controller.cpp ----------------------------
static const char* kPc = "src/pump_controller.cpp";

// D4a (kCeiling & toutes causes de l'observation) : jet INCONDITIONNEL dans la branche kDiscarded.
void test_S_D4_jet_avec_l_observation_pour_toutes_les_causes(void) {
  const std::string src = readFile(kPc);
  const std::string blk = between(src, "} else if (v.outcome == OrpMixingOutcome::kDiscarded) {",
                                  "bool PumpControllerClass::isPhMixingDelayActive");
  TEST_ASSERT_TRUE(blk.size() > 100);
  TEST_ASSERT_TRUE(blk.find("\n    discardOrpYieldEpisode(") != std::string::npos);  // 4 espaces, sans if
  TEST_ASSERT_TRUE(blk.find("v.discard ==") == std::string::npos);
  TEST_ASSERT_TRUE(blk.find("v.discard !=") == std::string::npos);
}

// D4b (environnement sale) : un tick sale JETTE l'épisode ouvert.
void test_S_D4_jet_sur_environnement_sale(void) {
  const std::string src = readFile(kPc);
  const std::string blk = between(src, "void PumpControllerClass::tickOrpLearn(",
                                  "_orpYieldTaint = false;");
  TEST_ASSERT_TRUE(blk.size() > 100);
  const size_t p = blk.find("!(orpEnvClean && !_flowAssistSuspend");
  TEST_ASSERT_TRUE(p != std::string::npos);
  const size_t d = blk.find("discardOrpYieldEpisode(\"conditions perturbées\")", p);
  TEST_ASSERT_TRUE(d != std::string::npos);
  // l'appel suit immédiatement le `if` (aucune condition supplémentaire entre les deux)
  TEST_ASSERT_TRUE(blk.substr(p, d - p).find("if (") == std::string::npos);
}

// D4c / D11 : injection manuelle ou programmée pendant l'épisode => taint => jet ; et
// l'ouverture exige une séance AUTOMATIQUE.
void test_S_D4_D11_jet_sur_injection_manuelle_et_ouverture_auto_seule(void) {
  const std::string src = readFile(kPc);
  const std::string cr = between(src, "// feature-203 : POINT UNIQUE du cumul d'épisode",
                                 "if (tracking) {\n      productCfg.orpTotalInjectedMl");
  TEST_ASSERT_TRUE(cr.size() > 100);
  TEST_ASSERT_TRUE(cr.find("if (_orpAutoSession && _pulseSession[1] && !_acctManual[1]) {") != std::string::npos);
  TEST_ASSERT_TRUE(cr.find("} else if (_orpYieldEp.open) {\n      _orpYieldTaint = true;") != std::string::npos);
  const std::string tk = between(src, "void PumpControllerClass::tickOrpLearn(", "_orpYieldTaint = false;");
  TEST_ASSERT_TRUE(tk.find("if (_orpYieldTaint) {\n      discardOrpYieldEpisode(") != std::string::npos);
}

// D2 / D9 / D10 : la condition d'arrêt.
static std::string stopBlock() {
  return between(readFile(kPc), "// feature-203 — BUDGET DE VOLUME d'épisode", "const bool orpStartDecided");
}

void test_S_D2_le_budget_ne_peut_que_arreter(void) {
  const std::string b = stopBlock();
  TEST_ASSERT_TRUE(b.size() > 300);
  TEST_ASSERT_TRUE(b.find("shouldDose = false;") != std::string::npos);
  TEST_ASSERT_TRUE(b.find("shouldDose = true") == std::string::npos);
  TEST_ASSERT_TRUE(b.find("shouldDose |=") == std::string::npos);
  TEST_ASSERT_TRUE(b.find("shouldDose = !") == std::string::npos);
  TEST_ASSERT_TRUE(b.find("shouldDose = budget") == std::string::npos);
  // et l'arrêt n'agit que si on dosait déjà
  TEST_ASSERT_TRUE(b.find("const bool stoppedByBudget = shouldDose && budgetReached;") != std::string::npos);
}

void test_S_D9_budget_recalcule_sur_le_manque_courant_jamais_fige(void) {
  const std::string b = stopBlock();
  TEST_ASSERT_TRUE(b.find("orpYieldBudgetMl(error, _orpYieldStore)") != std::string::npos);
  TEST_ASSERT_TRUE(b.find("_orpYieldBudgetServedMl.load()") == std::string::npos);  // pas de relecture
  TEST_ASSERT_TRUE(b.find("openDeficit") == std::string::npos);
  TEST_ASSERT_TRUE(b.find("_orpYieldEp.volMl") != std::string::npos);
}

void test_S_D10_desarme_ou_sans_episode_aucun_budget(void) {
  const std::string b = stopBlock();
  TEST_ASSERT_TRUE(b.find("const bool armed = !_orpYieldDisarmed.load() && _orpYieldEp.open;") != std::string::npos);
  TEST_ASSERT_TRUE(b.find("armed ? orpYieldBudgetMl(error, _orpYieldStore) : NAN") != std::string::npos);
  TEST_ASSERT_TRUE(b.find("if (orpYieldUndersupply(_orpYieldStopStreak)) _orpYieldDisarmed.store(true);") != std::string::npos);
}

// --- Défaut 1 (visa P2) : ré-ouverture en cours de séance --------------------
// Un épisode rouvert au milieu d'une séance porte un volume TRONQUÉ mais est clos avec la
// montée ENTIÈRE : le rendement calculé est SURESTIMÉ (budget trop petit, sous-chloration).
void test_P2_episode_rouvert_tronque_surestime_le_rendement(void) {
  const float volTotal = 456.0f, volAvantJet = 300.0f, rise = 106.0f;
  const OrpYieldVerdict reel = runEpisode(479.0f, 51.0f, volTotal, 479.0f + rise);
  // Jet après 300 mL, puis réouverture : seuls les 156 mL restants sont comptés, mais la
  // montée portée par l'observation reste celle de TOUT le chlore.
  const OrpYieldVerdict rouvert = runEpisode(479.0f, 51.0f, volTotal - volAvantJet, 479.0f + rise);
  TEST_ASSERT_TRUE(rouvert.kind == OrpYieldKind::kCredited);  // le module pur NE PEUT PAS le voir
  TEST_ASSERT_TRUE(rouvert.yieldMvPerMl > reel.yieldMvPerMl * 1.4f);
  // conséquence : budget plus petit que le vrai => arrêt trop tôt, systématique
  const float bReel = orpYieldBudgetMl(51.0f, learn3(reel.yieldMvPerMl, reel.yieldMvPerMl, reel.yieldMvPerMl, 51, 51, 51));
  const float bRouv = orpYieldBudgetMl(51.0f, learn3(rouvert.yieldMvPerMl, rouvert.yieldMvPerMl, rouvert.yieldMvPerMl, 51, 51, 51));
  TEST_ASSERT_TRUE(bRouv < bReel * 0.75f);
}

// Le module pur ne peut pas refuser la réouverture : c'est le CÂBLAGE qui doit l'interdire
// tant que la séance court. Garde lexicale : le chemin d'ouverture teste l'observation.
void test_S_P2_pas_de_reouverture_en_cours_de_seance(void) {
  const std::string src = readFile(kPc);
  const std::string cr = between(src, "// feature-203 : POINT UNIQUE du cumul d'épisode", "orpYieldAddVolume(_orpYieldEp, volumeMl);");
  TEST_ASSERT_TRUE(cr.size() > 100);
  const size_t o = cr.find("orpYieldOpen(_orpYieldEp");
  TEST_ASSERT_TRUE(o != std::string::npos);
  // La condition d'ouverture (ou un drapeau posé sur `!_orpObs.active`) doit apparaître AVANT l'ouverture.
  TEST_ASSERT_TRUE_MESSAGE(cr.substr(0, o).find("_orpObs.active") != std::string::npos ||
                           cr.substr(0, o).find("_orpYieldMayOpen") != std::string::npos,
      "DEFAUT 1 : rien n'interdit de rouvrir un episode tant que l'observation court (rendement surestime)");
}

// --- Défaut 2 (visa P2) : la cause d'arrêt affichée n'est PAS l'écrêtage -----
static std::string keyOnLineWith(const std::string& src, const char* accessor) {
  const size_t p = src.find(accessor);
  if (p == std::string::npos) return "";
  const size_t ls = src.rfind('\n', p) + 1;
  const size_t le = src.find('\n', p);
  const std::string line = src.substr(ls, le - ls);
  std::smatch m;
  if (std::regex_search(line, m, std::regex("\\[\"([a-z_0-9]+)\"\\]"))) return m[1];
  return "";
}

void test_S_P2_cause_d_arret_servie_depuis_la_vraie_grandeur(void) {
  const std::string http = readFile("src/web_routes_data.cpp");
  const std::string ws = readFile("src/ws_manager.cpp");
  const std::string kh = keyOnLineWith(http, "isOrpStoppedByYieldBudget");
  const std::string kw = keyOnLineWith(ws, "isOrpStoppedByYieldBudget");
  TEST_ASSERT_TRUE_MESSAGE(!kh.empty() && !kw.empty(),
      "DEFAUT 2 : isOrpStoppedByYieldBudget() n'est servi ni dans /data ni dans le WS");
  TEST_ASSERT_EQUAL_STRING(kh.c_str(), kw.c_str());
  // et le champ qui pilote la pastille « quantité atteinte » dans app.js est CE champ
  const std::string js = readFile("data/app.js");
  TEST_ASSERT_TRUE(js.find(kh) != std::string::npos);
}

// MUTATION défaut 2 : piloter la cause d'arrêt sur l'écrêtage. Aucun champ servi sous un
// nom d'arrêt/budget ne peut venir de isOrpYieldCapped().
void test_S_P2_ecretage_ne_pilote_pas_la_cause_d_arret(void) {
  const char* files[2] = {"src/web_routes_data.cpp", "src/ws_manager.cpp"};
  for (int i = 0; i < 2; ++i) {
    const std::string src = readFile(files[i]);
    size_t p = 0;
    while ((p = src.find("isOrpYieldCapped", p)) != std::string::npos) {
      const size_t ls = src.rfind('\n', p) + 1;
      const size_t le = src.find('\n', p);
      const std::string line = src.substr(ls, le - ls);
      TEST_ASSERT_TRUE_MESSAGE(line.find("stop") == std::string::npos && line.find("reached") == std::string::npos &&
                               line.find("budget_stop") == std::string::npos,
          "L'ecretage de plausibilite pilote un champ de cause d'arret");
      TEST_ASSERT_TRUE_MESSAGE(line.find("capped") != std::string::npos,
          "isOrpYieldCapped servi sous un nom qui n'annonce pas l'ecretage");
      p = le;
    }
  }
  // Et dans app.js, la pastille « quantité atteinte » ne lit jamais une clé *capped*.
  const std::string js = readFile("data/app.js");
  size_t q = 0;
  while ((q = js.find("orp_yield_capped", q)) != std::string::npos) {
    const size_t ls = js.rfind('\n', q) + 1, le = js.find('\n', q);
    const std::string line = js.substr(ls, le - ls);
    TEST_ASSERT_TRUE_MESSAGE(line.find("atteinte") == std::string::npos && line.find("Atteinte") == std::string::npos,
        "la pastille quantite atteinte lit encore orp_yield_capped (ecretage != cause d'arret)");
    q = le;
  }
}

// --- D1 : parité /data <-> WS des champs orp_yield_*, indépendante des NOMS ---
// Survit à un renommage/retrait : compare l'ENSEMBLE des clés `orp_yield_*` et, pour chaque
// clé, l'EXPRESSION (receveur doc/d neutralisé). Un champ servi d'un seul côté, ou avec une
// expression divergente, échoue. NE couvre PAS les autres familles (voir test_native_ws_data_parity).
static std::map<std::string, std::string> yieldExprs(const std::string& src) {
  std::map<std::string, std::string> out;
  size_t pos = 0;
  while (pos < src.size()) {
    size_t e = src.find('\n', pos);
    if (e == std::string::npos) e = src.size();
    std::string line = src.substr(pos, e - pos);
    pos = e + 1;
    const size_t t = line.find_first_not_of(" \t");
    if (t == std::string::npos || line.compare(t, 2, "//") == 0) continue;
    std::smatch m;
    if (!std::regex_search(line, m, std::regex("\\[\"(orp_yield_[a-z_0-9]+)\"\\]"))) continue;
    std::string expr = line.substr(t);
    expr = std::regex_replace(expr, std::regex("\\b(doc|d)\\["), "X[");
    out[m[1]] += expr + "|";
  }
  return out;
}

void test_D1_parite_des_champs_orp_yield(void) {
  const auto h = yieldExprs(readFile("src/web_routes_data.cpp"));
  const auto w = yieldExprs(readFile("src/ws_manager.cpp"));
  TEST_ASSERT_TRUE_MESSAGE(h.size() >= 3, "aucun champ orp_yield_* trouve cote /data : sentinelle aveugle");
  for (const auto& kv : h) {
    char msg[200];
    snprintf(msg, sizeof(msg), "« %s » servi par /data mais pas par le WS", kv.first.c_str());
    TEST_ASSERT_TRUE_MESSAGE(w.count(kv.first) == 1, msg);
    snprintf(msg, sizeof(msg), "« %s » : EXPRESSION divergente entre /data et WS", kv.first.c_str());
    TEST_ASSERT_TRUE_MESSAGE(w.at(kv.first) == kv.second, msg);
  }
  for (const auto& kv : w) {
    char msg[200];
    snprintf(msg, sizeof(msg), "« %s » servi par le WS mais pas par /data", kv.first.c_str());
    TEST_ASSERT_TRUE_MESSAGE(h.count(kv.first) == 1, msg);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_D6_vecteur_18_11_rendement_et_budget);
  RUN_TEST(test_D6_aucune_valeur_du_vecteur_dans_constants_h);
  RUN_TEST(test_D5_sous_trois_episodes_aucun_budget);
  RUN_TEST(test_D5_episode_jete_ne_compte_pas);
  RUN_TEST(test_D12_mesure_nan_budget_nan_jamais_zero);
  RUN_TEST(test_D12_ouverture_sur_mesure_nan_episode_jete);
  RUN_TEST(test_D13_montee_nulle_ou_negative_jete_sans_division);
  RUN_TEST(test_D14_volume_sous_plus_petite_dose_jete);
  RUN_TEST(test_D3_ecretage_haut_du_rendement);
  RUN_TEST(test_D3_pas_de_borne_basse);
  RUN_TEST(test_D3_snapshot_refuse_rendement_au_dessus_du_plafond);
  RUN_TEST(test_D7_budget_seulement_jusqu_a_la_mediane_des_manques);
  RUN_TEST(test_mediane_toutes_permutations);
  RUN_TEST(test_anneau_le_4e_chasse_le_1er);
  RUN_TEST(test_D9_budget_suit_le_manque_courant);
  RUN_TEST(test_D2_budget_atteint_est_monotone_et_jamais_avant_le_volume);
  RUN_TEST(test_D8_biais_de_comptage_cote_prudent);
  RUN_TEST(test_D10_trois_arrets_budget_sans_consigne_alerte);
  RUN_TEST(test_D10_consigne_atteinte_remet_a_zero);
  RUN_TEST(test_D11_cloture_d_un_episode_jamais_ouvert_est_jetee);
  RUN_TEST(test_cloture_remet_l_episode_a_zero);
  RUN_TEST(test_snapshot_aller_retour_et_corruptions);
  RUN_TEST(test_empreinte_de_contexte);
  RUN_TEST(test_S_D4_jet_avec_l_observation_pour_toutes_les_causes);
  RUN_TEST(test_S_D4_jet_sur_environnement_sale);
  RUN_TEST(test_S_D4_D11_jet_sur_injection_manuelle_et_ouverture_auto_seule);
  RUN_TEST(test_S_D2_le_budget_ne_peut_que_arreter);
  RUN_TEST(test_S_D9_budget_recalcule_sur_le_manque_courant_jamais_fige);
  RUN_TEST(test_S_D10_desarme_ou_sans_episode_aucun_budget);
  RUN_TEST(test_P2_episode_rouvert_tronque_surestime_le_rendement);
  RUN_TEST(test_S_P2_pas_de_reouverture_en_cours_de_seance);
  RUN_TEST(test_S_P2_cause_d_arret_servie_depuis_la_vraie_grandeur);
  RUN_TEST(test_S_P2_ecretage_ne_pilote_pas_la_cause_d_arret);
  RUN_TEST(test_D1_parite_des_champs_orp_yield);
  return UNITY_END();
}
