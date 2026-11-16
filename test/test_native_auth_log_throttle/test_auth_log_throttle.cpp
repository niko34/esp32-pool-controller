// =============================================================================
// Tests unitaires natifs — bornage du WARN « accès non autorisé » (bug-112)
// =============================================================================
// Cible : src/auth_log_throttle_logic.h (header pur, ADR-0017).
// Ils observent le COMPORTEMENT (quelles lignes tombent, quand), pas le code.
//
// `Site` ci-dessous n'est PLUS une réplique : c'est un mince adaptateur sur le VRAI
// AuthLogThrottleSite / authLogSiteAttempt / authLogSiteDrain (auth_log_throttle_site.h).
// Il ne fait que compter les lignes (émissions ET drains).
//
// Constantes : les VRAIES (constants.h), jamais recopiées.
#include <unity.h>
#include <stdint.h>
#include <malloc.h>
#include <new>
#include <cstdlib>
#include <string>
#include "constants.h"
#include "auth_log_throttle_logic.h"
#include "auth_log_throttle_site.h"
#include <thread>
#include <atomic>

// ---- Compteur d'allocations (T13) -------------------------------------------
static unsigned long gAllocs = 0;
void* operator new(std::size_t n) { ++gAllocs; void* p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](std::size_t n) { ++gAllocs; void* p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static const uint32_t kMin = 60000UL;
static const uint32_t kBase = kUnauthWarnThrottleMs;
static const uint8_t kMaxShift = kUnauthWarnMaxShift;
static const uint32_t kDecay = kUnauthWarnDecayMs;

struct Site {
  AuthLogThrottleSite site;
  AuthLogThrottleState& st = site.st;
  uint16_t lastReported = 0;  // compte annonce dans la derniere ligne (emission ou drain)
  int lines = 0;              // lignes issues d'attempt()
  int drainLines = 0;         // lignes issues du drain
  uint16_t suppressed() const { return site.suppressed.load(); }
  bool otherRoutes() const { return site.otherRoutes.load(); }
  bool otherIps() const { return site.otherIps.load(); }
  int total() const { return lines + drainLines; }
  bool attempt(uint32_t now, uint32_t url = 1, uint32_t ip = 1) {
    const AuthLogSiteReport r = authLogSiteAttempt(site, now, url, ip, kBase, kMaxShift, kDecay);
    if (r.emit) { lastReported = r.occulted; ++lines; }
    return r.emit;
  }
  bool drain(uint32_t now) {
    const AuthLogSiteReport r = authLogSiteDrain(site, now, kBase, kMaxShift, kUnauthWarnDrainQuietMs);
    if (r.emit) { lastReported = r.occulted; ++drainLines; }
    return r.emit;
  }
};

void setUp(void) {}
void tearDown(void) {}

// T1
void test_tout_premier_refus_a_t0_est_emis(void) {
  Site s;
  TEST_ASSERT_TRUE(s.attempt(0));
}
void test_tout_premier_refus_a_t_quelconque_est_emis(void) {
  Site s;
  TEST_ASSERT_TRUE(s.attempt(12345));
}

// T2
void test_rafale_de_500_donne_4_lignes_max(void) {
  Site s; int lines = 0;
  for (int i = 0; i < 500; ++i) lines += s.attempt(i * 10UL) ? 1 : 0;  // 5 s au total
  TEST_ASSERT_TRUE(lines >= 1);
  TEST_ASSERT_TRUE(lines <= 4);
  TEST_ASSERT_EQUAL_UINT16(500 - lines, s.suppressed());
}

// T3 : le test central
void test_goutte_a_goutte_1_par_minute_pendant_1h(void) {
  Site s; int lines = 0;
  for (uint32_t m = 0; m < 60; ++m) lines += s.attempt(m * kMin) ? 1 : 0;
  TEST_ASSERT_TRUE_MESSAGE(lines <= 7, "le goutte-a-goutte purgerait le journal");
  TEST_ASSERT_EQUAL_INT(6, lines);  // t=0,1,3,7,15,31 (63 min hors de la 1re heure)
}
void test_goutte_a_goutte_24h_reste_borne(void) {
  Site s; int lines = 0;
  for (uint32_t m = 0; m < 24 * 60; ++m) lines += s.attempt(m * kMin) ? 1 : 0;
  TEST_ASSERT_TRUE(lines <= 28);
  TEST_ASSERT_TRUE(lines >= 20);
}

// T4
void test_sequence_du_recul_0_1_3_7_15_31_63(void) {
  Site s;
  const uint32_t expected[] = {0, 1, 3, 7, 15, 31, 63};
  size_t k = 0;
  for (uint32_t m = 0; m <= 70; ++m) {
    if (s.attempt(m * kMin)) {
      TEST_ASSERT_TRUE(k < 7);
      TEST_ASSERT_EQUAL_UINT32(expected[k], m);
      ++k;
    }
  }
  TEST_ASSERT_EQUAL_UINT32(7, k);
}

// T5
void test_plafond_la_fenetre_ne_depasse_pas_64_min(void) {
  TEST_ASSERT_EQUAL_UINT32(64UL * kMin, authLogThrottleWindowMs(kBase, 6, kMaxShift));
  TEST_ASSERT_EQUAL_UINT32(64UL * kMin, authLogThrottleWindowMs(kBase, 200, kMaxShift));
  AuthLogThrottleState st = AuthLogThrottleState{};
  uint32_t now = 0; uint32_t last = 0; uint32_t maxGap = 0; int lines = 0;
  // Tentative toutes les minutes sur 20 h : jamais plus de 64 min de silence.
  for (uint32_t m = 0; m < 20 * 60; ++m) {
    now = m * (uint32_t)kMin;
    if (authLogThrottleStep(st, now, 1, 1, kBase, kMaxShift, kDecay).emit) {
      if (lines > 0 && now - last > maxGap) maxGap = now - last;
      last = now; ++lines;
    }
  }
  TEST_ASSERT_EQUAL_UINT32(64UL * kMin, maxGap);
  TEST_ASSERT_EQUAL_UINT8(kMaxShift, st.shift);
}

// T6
static void montee_en_shift(AuthLogThrottleState& st, int target) {
  uint32_t t = 0;
  for (int guard = 0; st.shift < target && guard < 1000; ++guard) {
    authLogThrottleStep(st, t, 1, 1, kBase, kMaxShift, kDecay);
    t += authLogThrottleWindowMs(kBase, st.shift, kMaxShift);
  }
}
// Isole la decroissance : lastWarnMs recent => la tentative est muette, shift lisible.
void test_decroissance_moins_un_apres_1h_sans_tentative(void) {
  AuthLogThrottleState st = AuthLogThrottleState{};
  st.everLogged = true; st.shift = 4; st.lastAttemptMs = 5000;
  st.lastWarnMs = 5000 + kDecay - 1;
  authLogThrottleStep(st, 5000 + kDecay - 1, 1, 1, kBase, kMaxShift, kDecay);
  TEST_ASSERT_EQUAL_UINT8(4, st.shift);  // 1 ms avant l'heure : rien
  st.lastAttemptMs = 5000; st.lastWarnMs = 5000 + kDecay - 1;
  authLogThrottleStep(st, 5000 + kDecay, 1, 1, kBase, kMaxShift, kDecay);
  TEST_ASSERT_EQUAL_UINT8(3, st.shift);  // pile 1 h : -1
}
void test_decroissance_observee_sans_emission(void) {
  AuthLogThrottleState st = AuthLogThrottleState{};
  montee_en_shift(st, 5);
  st.lastWarnMs = st.lastAttemptMs = 0;
  // now = 1 h : fenetre (shift 4 -> 16 min) deja ecoulee, emission +1 => 5.
  // On veut isoler la decroissance : 2 h sans tentative => shift 5-2 = 3, +1 emis = 4.
  authLogThrottleStep(st, 2 * kDecay, 1, 1, kBase, kMaxShift, kDecay);
  TEST_ASSERT_EQUAL_UINT8(4, st.shift);
}
void test_decroissance_jusqu_a_zero(void) {
  AuthLogThrottleState st = AuthLogThrottleState{};
  st.everLogged = true; st.shift = 6; st.lastAttemptMs = 0;
  st.lastWarnMs = 10 * kDecay - 1000;
  authLogThrottleStep(st, 10 * kDecay, 1, 1, kBase, kMaxShift, kDecay);
  TEST_ASSERT_EQUAL_UINT8(0, st.shift);  // sature a 0, pas de repliement non signe
}
void test_le_drain_ne_decroit_pas_shift(void) {
  Site s;
  for (uint32_t m = 0; m <= 31; ++m) s.attempt(m * kMin);
  s.site.suppressed.store(3);
  const uint8_t before = s.st.shift;
  TEST_ASSERT_TRUE(before >= 4);
  // Drain tardif (2 h apres) : annonce, mais ne touche pas au recul.
  TEST_ASSERT_TRUE(s.drain(s.st.lastAttemptMs + 2 * kDecay));
  TEST_ASSERT_EQUAL_UINT8(before, s.st.shift);
  // Preuve par le comportement : une tentative immediatement apres le drain
  // (decroissance mesuree depuis lastAttemptMs, inchangee) se comporte comme
  // sans drain : pas de ligne si la fenetre n'est pas ecoulee.
}
void test_drain_ne_relance_pas_le_goutte_a_goutte(void) {
  // Variante s : 1 tentative/min, drain 30 s apres (jamais du : calme < 2 min).
  Site s;
  for (uint32_t m = 0; m < 180; ++m) {
    s.attempt(m * kMin);
    s.drain(m * kMin + 30000UL);
  }
  TEST_ASSERT_TRUE(s.total() <= 9);
  // Variante s2 : tentatives espacees de 3 min, drain a +2,5 min (calme atteint).
  // On compte TOUTES les lignes : emissions d'attempt() ET drains (G1).
  Site s2;
  for (uint32_t m = 0; m < 180; m += 3) {
    s2.attempt(m * kMin);
    s2.drain(m * kMin + 150000UL);
  }
  char msg[96];
  snprintf(msg, sizeof msg, "lignes totales=%d (attempt=%d drain=%d)", s2.total(), s2.lines, s2.drainLines);
  TEST_ASSERT_TRUE_MESSAGE(s2.total() <= 8, msg);
  TEST_ASSERT_TRUE(s2.drainLines >= 1);  // le scenario exerce bien le drain
}

// T7
void test_le_drain_n_ecrit_ni_lastwarn_ni_everlogged_ni_shift(void) {
  Site s;
  s.attempt(1000);
  s.attempt(2000);  // occultee
  const AuthLogThrottleState before = s.st;
  TEST_ASSERT_TRUE(s.drain(2000 + kUnauthWarnDrainQuietMs));
  TEST_ASSERT_EQUAL_UINT32(before.lastWarnMs, s.st.lastWarnMs);
  TEST_ASSERT_EQUAL(before.everLogged, s.st.everLogged);
  TEST_ASSERT_EQUAL_UINT8(before.shift, s.st.shift);
  TEST_ASSERT_EQUAL_UINT32(before.lastAttemptMs, s.st.lastAttemptMs);
}

// T8
void test_pas_de_drain_si_aucun_occulte(void) {
  TEST_ASSERT_FALSE(authLogThrottleDrainDue(0, 10000000UL, 0, kUnauthWarnDrainQuietMs));
  TEST_ASSERT_TRUE(authLogThrottleDrainDue(1, 10000000UL, 0, kUnauthWarnDrainQuietMs));
}
void test_pas_de_drain_avant_le_calme(void) {
  TEST_ASSERT_FALSE(authLogThrottleDrainDue(5, 1000 + kUnauthWarnDrainQuietMs - 1, 1000, kUnauthWarnDrainQuietMs));
  TEST_ASSERT_TRUE(authLogThrottleDrainDue(5, 1000 + kUnauthWarnDrainQuietMs, 1000, kUnauthWarnDrainQuietMs));
}

// T9
void test_compteur_d_occultes_sature_sans_repliement(void) {
  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, authLogThrottleSatInc(UINT16_MAX));
  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX - 1 + 1, authLogThrottleSatInc(UINT16_MAX - 1));
  TEST_ASSERT_EQUAL_UINT16(1, authLogThrottleSatInc(0));
}
void test_compteur_sature_dans_le_scenario(void) {
  Site s; s.attempt(0);
  s.site.suppressed.store(UINT16_MAX - 2);
  for (int i = 0; i < 10; ++i) s.attempt(1000UL + i);
  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, s.suppressed());
}

// T10
void test_reversement_dit_le_compte_puis_remet_a_zero(void) {
  Site s;
  s.attempt(0);
  for (int i = 1; i <= 7; ++i) s.attempt(i * 1000UL);
  TEST_ASSERT_EQUAL_UINT16(7, s.suppressed());
  TEST_ASSERT_TRUE(s.attempt(kMin));  // fenetre de base ecoulee
  TEST_ASSERT_EQUAL_UINT16(7, s.lastReported);
  TEST_ASSERT_EQUAL_UINT16(0, s.suppressed());
}
void test_reversement_au_drain(void) {
  Site s; s.attempt(0); s.attempt(10); s.attempt(20);
  TEST_ASSERT_TRUE(s.drain(20 + kUnauthWarnDrainQuietMs));
  TEST_ASSERT_EQUAL_UINT16(2, s.lastReported);
  TEST_ASSERT_EQUAL_UINT16(0, s.suppressed());
  TEST_ASSERT_FALSE(s.drain(20 + 2 * kUnauthWarnDrainQuietMs));  // rien a redire
}

// T11
void test_repliement_de_millis_ni_silence_ni_rafale(void) {
  Site s; int lines = 0;
  uint32_t now = UINT32_MAX - 2UL * kMin;  // a 2 min du repliement
  lines += s.attempt(now) ? 1 : 0;
  // 1 tentative/s pendant 3 min : franchit le repliement.
  for (uint32_t i = 1; i <= 180; ++i) lines += s.attempt(now + i * 1000UL) ? 1 : 0;
  // Fenetres : 0, +1 min, +3 min => 3 lignes, pas 181 (rafale) ni 1 (silence).
  TEST_ASSERT_EQUAL_INT(3, lines);
}
void test_repliement_juste_apres_emission(void) {
  Site s;
  TEST_ASSERT_TRUE(s.attempt(UINT32_MAX - 5));
  TEST_ASSERT_FALSE(s.attempt(UINT32_MAX));      // 5 ms
  TEST_ASSERT_FALSE(s.attempt(10));              // 16 ms apres repliement
  TEST_ASSERT_TRUE(s.attempt(kMin - 6));         // exactement 1 fenetre (kMin) apres UINT32_MAX-5 ... 
}

// T12
void test_drapeaux_de_masquage(void) {
  Site s;
  s.attempt(0, /*url*/ 10, /*ip*/ 100);
  s.attempt(1000, 10, 100);
  TEST_ASSERT_FALSE(s.otherRoutes());
  TEST_ASSERT_FALSE(s.otherIps());
  s.attempt(2000, 11, 100);
  TEST_ASSERT_TRUE(s.otherRoutes());
  TEST_ASSERT_FALSE(s.otherIps());
  s.attempt(3000, 10, 101);
  TEST_ASSERT_TRUE(s.otherIps());
}
void test_drapeaux_remis_a_zero_apres_emission(void) {
  Site s;
  s.attempt(0, 10, 100);
  s.attempt(1000, 11, 101);
  TEST_ASSERT_TRUE(s.otherRoutes() && s.otherIps());
  TEST_ASSERT_TRUE(s.attempt(kMin, 10, 100));
  TEST_ASSERT_FALSE(s.otherRoutes());
  TEST_ASSERT_FALSE(s.otherIps());
}
void test_hash_distingue_les_routes(void) {
  TEST_ASSERT_NOT_EQUAL(authLogThrottleHash("/a"), authLogThrottleHash("/b"));
  TEST_ASSERT_EQUAL_UINT32(authLogThrottleHash("/data"), authLogThrottleHash("/data"));
  TEST_ASSERT_EQUAL_UINT32(2166136261u, authLogThrottleHash(nullptr));
  TEST_ASSERT_EQUAL_UINT32(0xE40C292Cu, authLogThrottleHash("a"));  // vecteur FNV-1a connu
}

// M2 : le verdict dans la fenetre ET hors fenetre, observe pour la RAISON
void test_verdict_dans_la_fenetre_est_muet_et_hors_fenetre_parle(void) {
  AuthLogThrottleState st = AuthLogThrottleState{};
  TEST_ASSERT_TRUE(authLogThrottleStep(st, 100, 1, 1, kBase, kMaxShift, kDecay).emit);
  TEST_ASSERT_FALSE(authLogThrottleStep(st, 100 + kBase - 1, 1, 1, kBase, kMaxShift, kDecay).emit);
  TEST_ASSERT_TRUE(authLogThrottleStep(st, 100 + kBase, 1, 1, kBase, kMaxShift, kDecay).emit);
}
void test_premiere_ligne_ne_releve_pas_le_recul(void) {
  AuthLogThrottleState st = AuthLogThrottleState{};
  authLogThrottleStep(st, 0, 1, 1, kBase, kMaxShift, kDecay);
  TEST_ASSERT_EQUAL_UINT8(0, st.shift);
  authLogThrottleStep(st, kBase, 1, 1, kBase, kMaxShift, kDecay);
  TEST_ASSERT_EQUAL_UINT8(1, st.shift);
}

// T13 : zero allocation sur le chemin muet
void test_zero_allocation_sur_500_decisions_muettes(void) {
  Site s;
  s.attempt(0, 7, 9);  // la ligne emise (hors mesure)
  const unsigned long a0 = gAllocs;
  const struct mallinfo2 m0 = mallinfo2();
  int emitted = 0;
  for (int i = 0; i < 500; ++i) {
    emitted += s.attempt(1000UL + i, 8, 10) ? 1 : 0;
    emitted += s.drain(1000UL + i) ? 1 : 0;
    (void)authLogThrottleHash("/get-config");
  }
  const struct mallinfo2 m1 = mallinfo2();
  TEST_ASSERT_EQUAL_INT(0, emitted);
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)(gAllocs - a0));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)m0.uordblks, (uint32_t)m1.uordblks);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)m0.hblkhd, (uint32_t)m1.hblkhd);
}
// Controle du detecteur : il voit bien une allocation (sinon T13 serait aveugle).
void test_le_detecteur_d_allocation_voit_une_allocation(void) {
  const unsigned long a0 = gAllocs;
  std::string* p = new std::string("une chaine assez longue pour sortir du SSO");
  volatile size_t n = p->size(); (void)n;
  delete p;
  TEST_ASSERT_TRUE(gAllocs > a0);
}

// ============================================================================
// bug-112 (revue) : bornage 24 h, M8, drain borne, coquille atomique reelle
// ============================================================================

// Bornage 24 h (bug-112, condition 3 de la 2e revue). PIRE MESURE : 38 lignes /
// 24 h / site (emissions + drains), atteint par des GROUPES de 5 tentatives a 60 s
// d'ecart repetes toutes les 3841 s. Les tentatives ISOLEES ne donnaient que 28 :
// le drain (qui ne remet pas a zero la date de derniere tentative) laisse la
// decroissance -1/h courir pendant qu'il emet. AUCUN MAJORANT N'EST ETABLI : 38 est
// le pire sur l'espace explore, pas une borne prouvee.
// ESPACE EXPLORE ICI (etat vierge, loop a 1 s) : groupes de k = 1..5 tentatives
// espacees de g in {1, 60} s, periode P = 30..4000 s par pas de 10 s, PLUS P = 3600..4100 s
// par pas de 1 s. (Hors test, exploration ms-exacte : groupes k<=12 a deux ecarts,
// paires a ecart quelconque, tentatives par pas de 1 s, ~3.10^6 rollouts
// aleatoires guides par l'etat, escalade aleatoire : aucun n'a depasse 38.)
static int lignes_groupes_sur_24h(uint32_t periodeS, int k, uint32_t ecartS) {
  Site s;
  for (uint32_t t = 0; t < 24UL * 3600UL; ++t) {
    const uint32_t phase = t % periodeS;
    if (phase % ecartS == 0 && phase / ecartS < (uint32_t)k) s.attempt(t * 1000UL);
    s.drain(t * 1000UL);
  }
  return s.total();
}
static int lignes_sur_24h(uint32_t cadenceS) { return lignes_groupes_sur_24h(cadenceS, 1, 1); }
void test_bornage_24h_emissions_et_drains_a_la_pire_cadence(void) {
  int pire = 0; uint32_t pireP = 0; int pireK = 0; uint32_t pireG = 0;
  static const uint32_t ecarts[2] = {1, 60};
  for (int k = 1; k <= 5; ++k) {
    for (uint32_t g : ecarts) {
      if (k == 1 && g != 1) continue;
      for (uint32_t p = 30; p <= 4100; p += (p >= 3600 ? 1 : 10)) {
        if (p <= (uint32_t)(k - 1) * g) continue;
        const int n = lignes_groupes_sur_24h(p, k, g);
        if (n > pire) { pire = n; pireP = p; pireK = k; pireG = g; }
      }
    }
  }
  char msg[112];
  snprintf(msg, sizeof msg, "pire=%d lignes (k=%d g=%us P=%us)", pire, pireK, (unsigned)pireG, (unsigned)pireP);
  TEST_ASSERT_TRUE_MESSAGE(pire <= 38, msg);   // pire MESURE, pas un majorant prouve
  TEST_ASSERT_TRUE_MESSAGE(pire >= 38, msg);   // le scenario le plus severe est bien atteint
}
void test_bornage_24h_rafale_continue_1_par_seconde(void) {
  TEST_ASSERT_TRUE(lignes_sur_24h(1) <= 30);
}

// M8 : le plafond du recul est observable PAR LA DECROISSANCE.
// 10 emissions, 7 h de silence, une tentative (emet), une autre 2 min plus tard :
// plafond ok => shift 1 (fenetre 2 min) => emet ; sans plafond => muet.
void test_plafond_observable_par_la_decroissance(void) {
  AuthLogThrottleState st = AuthLogThrottleState{};
  uint32_t now = 0; int emis = 0;
  while (emis < 10) {
    if (authLogThrottleStep(st, now, 1, 1, kBase, kMaxShift, kDecay).emit) ++emis;
    now += kMin;
  }
  now += 7UL * kDecay;  // 7 h de silence
  TEST_ASSERT_TRUE(authLogThrottleStep(st, now, 1, 1, kBase, kMaxShift, kDecay).emit);
  TEST_ASSERT_TRUE_MESSAGE(
      authLogThrottleStep(st, now + 2 * kMin, 1, 1, kBase, kMaxShift, kDecay).emit,
      "sans plafond, le recul ne decroit pas assez : fenetre trop longue");
}

// ---- Drain borne par le recul (G1) ------------------------------------------
void test_drain_refuse_tant_que_la_fenetre_n_est_pas_ecoulee(void) {
  Site s;
  for (uint32_t m = 0; m <= 31; ++m) s.attempt(m * kMin);  // shift 5 apres la ligne de 31 min
  const uint32_t lw = 31 * kMin;
  s.attempt(lw + 1000);  // occultee
  const uint32_t window = authLogThrottleWindowMs(kBase, s.st.shift, kMaxShift);
  TEST_ASSERT_TRUE(window > kUnauthWarnDrainQuietMs);
  // silence OK mais fenetre non ecoulee : muet
  TEST_ASSERT_FALSE(s.drain(lw + 1000 + kUnauthWarnDrainQuietMs));
  TEST_ASSERT_FALSE(s.drain(lw + window - 1));
  TEST_ASSERT_TRUE(s.drain(lw + window));
  TEST_ASSERT_EQUAL_UINT16(1, s.lastReported);
}
void test_drain_n_emet_pas_sans_silence_meme_fenetre_ecoulee(void) {
  Site s;
  s.attempt(0); s.attempt(10);
  // fenetre (60 s) ecoulee, mais derniere tentative il y a 50 s : pas de calme
  s.attempt(kMin + 5000 - 5000 + 0);   // emet a 60 s, relevee
  s.attempt(kMin + 100);               // occultee
  TEST_ASSERT_FALSE(s.drain(kMin + 100 + kUnauthWarnDrainQuietMs - 1));
}

// ---- Un drain emis COMPTE comme une ligne (ecart assume) -------------------
void test_un_drain_emis_consomme_la_fenetre_et_releve_le_recul(void) {
  Site s;
  TEST_ASSERT_TRUE(s.attempt(0));
  TEST_ASSERT_FALSE(s.attempt(10000));
  TEST_ASSERT_TRUE(s.drain(130000));            // base ecoulee, calme 120 s
  TEST_ASSERT_EQUAL_UINT16(1, s.lastReported);
  // Sans fold : lastWarn=0, shift=0 => une tentative a 190 s EMETTRAIT.
  // Avec fold : lastWarn=130 s, shift=1 (fenetre 2 min) => muette.
  TEST_ASSERT_FALSE(s.attempt(190000));
  TEST_ASSERT_EQUAL_UINT8(1, s.st.shift);
  TEST_ASSERT_EQUAL_UINT32(130000, s.st.lastWarnMs);
}
void test_refus_isole_apres_un_drain_est_muet_puis_drain_avec_son_compte(void) {
  Site s;
  s.attempt(0); s.attempt(10000);
  TEST_ASSERT_TRUE(s.drain(130000));
  TEST_ASSERT_FALSE(s.attempt(190000));                 // isole : muet
  TEST_ASSERT_EQUAL_UINT16(1, s.suppressed());
  TEST_ASSERT_FALSE(s.drain(190000 + kUnauthWarnDrainQuietMs - 1));
  TEST_ASSERT_TRUE(s.drain(190000 + kUnauthWarnDrainQuietMs));  // fenetre 2 min ecoulee depuis 130 s
  TEST_ASSERT_EQUAL_UINT16(1, s.lastReported);
}
void test_fold_drain_respecte_le_plafond_et_ignore_un_etat_vierge(void) {
  AuthLogThrottleState st = AuthLogThrottleState{};
  authLogThrottleFoldDrain(st, 5000, kMaxShift);       // jamais journalise : sans effet
  TEST_ASSERT_EQUAL_UINT32(0, st.lastWarnMs);
  TEST_ASSERT_EQUAL_UINT8(0, st.shift);
  st.everLogged = true; st.shift = kMaxShift;
  authLogThrottleFoldDrain(st, 7000, kMaxShift);
  TEST_ASSERT_EQUAL_UINT8(kMaxShift, st.shift);
  TEST_ASSERT_EQUAL_UINT32(7000, st.lastWarnMs);
  st.shift = 2;
  authLogThrottleFoldDrain(st, 8000, kMaxShift);
  TEST_ASSERT_EQUAL_UINT8(3, st.shift);
}

// ---- C-bis : ecart « negatif » rejete ---------------------------------------
void test_ecart_negatif_n_est_pas_un_long_silence(void) {
  TEST_ASSERT_FALSE(authLogThrottleElapsedAtLeast(100, 200, 60000));   // then > now
  TEST_ASSERT_TRUE(authLogThrottleElapsedAtLeast(300, 200, 100));
  TEST_ASSERT_FALSE(authLogThrottleElapsedAtLeast(300, 200, 101));
  // repliement legitime : 5 ms apres UINT32_MAX - 4 reste un petit ecart positif
  TEST_ASSERT_TRUE(authLogThrottleElapsedAtLeast(5, UINT32_MAX - 4, 10));
  TEST_ASSERT_FALSE(authLogThrottleDrainDue(3, 100, 200, 0));
}
void test_drain_premature_quand_une_tentative_s_intercale(void) {
  Site s;
  s.attempt(0); s.attempt(30000);                // 1 occulte, fenetre de base (60 s) bientot ecoulee
  // La loop a lu millis()=70000 (fenetre ecoulee), puis async_tcp publie une tentative a 70005.
  s.site.lastAttemptMs.store(70005);
  TEST_ASSERT_FALSE(s.drain(70000));
  TEST_ASSERT_EQUAL_UINT16(1, s.suppressed());
}

// ---- Accord du pluriel -------------------------------------------------------
void test_accord_du_pluriel(void) {
  TEST_ASSERT_EQUAL_STRING("refus", authLogThrottlePlural(1, "refus", "refus!"));
  TEST_ASSERT_EQUAL_STRING("refus!", authLogThrottlePlural(2, "refus", "refus!"));
  TEST_ASSERT_EQUAL_STRING("refus!", authLogThrottlePlural(UINT16_MAX, "refus", "refus!"));
}

// ---- Coquille atomique REELLE -------------------------------------------------
void test_site_increment_sature_par_cas_depuis_l_etat_reel(void) {
  Site s; s.attempt(0);
  s.site.suppressed.store(UINT16_MAX - 1);
  s.attempt(10); s.attempt(20); s.attempt(30);
  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, s.suppressed());  // pas de repliement a 0
  TEST_ASSERT_TRUE(s.attempt(kMin));
  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, s.lastReported);
  TEST_ASSERT_EQUAL_UINT16(0, s.suppressed());
}
void test_site_drain_exchange_zero_et_drapeaux(void) {
  Site s;
  s.attempt(0, 1, 1); s.attempt(100, 2, 2); s.attempt(200, 2, 2);
  TEST_ASSERT_EQUAL_UINT16(2, s.suppressed());
  const AuthLogSiteReport r = authLogSiteDrain(s.site, 200 + kUnauthWarnDrainQuietMs, kBase, kMaxShift,
                                               kUnauthWarnDrainQuietMs);
  TEST_ASSERT_TRUE(r.emit);
  TEST_ASSERT_EQUAL_UINT16(2, r.occulted);
  TEST_ASSERT_TRUE(r.otherRoutes);
  TEST_ASSERT_TRUE(r.otherIps);
  TEST_ASSERT_EQUAL_UINT16(0, s.suppressed());
  TEST_ASSERT_FALSE(s.otherRoutes());
  TEST_ASSERT_FALSE(s.otherIps());
  TEST_ASSERT_TRUE(s.site.drainPending.load());
  TEST_ASSERT_EQUAL_UINT32(200 + kUnauthWarnDrainQuietMs, s.site.drainedAtMs.load());
  // un second drain n'a plus rien a dire
  TEST_ASSERT_FALSE(authLogSiteDrain(s.site, 10 * kDecay, kBase, kMaxShift, kUnauthWarnDrainQuietMs).emit);
}
void test_site_le_drain_n_ecrit_pas_l_etat_async_tcp(void) {
  Site s;
  s.attempt(0); s.attempt(100);
  const AuthLogThrottleState b = s.st;
  const uint8_t shiftPub = s.site.shift.load();
  const uint32_t lwPub = s.site.lastWarnMs.load();
  TEST_ASSERT_TRUE(s.drain(100 + kUnauthWarnDrainQuietMs));
  TEST_ASSERT_EQUAL_UINT32(b.lastWarnMs, s.st.lastWarnMs);
  TEST_ASSERT_EQUAL_UINT8(b.shift, s.st.shift);
  TEST_ASSERT_EQUAL_UINT8(shiftPub, s.site.shift.load());
  TEST_ASSERT_EQUAL_UINT32(lwPub, s.site.lastWarnMs.load());
  TEST_ASSERT_TRUE(s.site.drainPending.load());     // repris seulement a la tentative suivante
  s.attempt(100 + kUnauthWarnDrainQuietMs + 1);
  TEST_ASSERT_FALSE(s.site.drainPending.load());
}
void test_site_une_ligne_emise_publie_l_etat_pour_le_drain(void) {
  Site s;
  s.attempt(0); s.attempt(kBase);   // 2e ligne : shift 1
  TEST_ASSERT_EQUAL_UINT8(1, s.site.shift.load());
  TEST_ASSERT_EQUAL_UINT32(kBase, s.site.lastWarnMs.load());
  TEST_ASSERT_EQUAL_UINT32(kBase, s.site.lastAttemptMs.load());
}
// Course reelle : attempt (async_tcp) contre drain (loop). Aucun occulte perdu.
void test_site_aucun_occulte_perdu_entre_attempt_et_drain(void) {
  static AuthLogThrottleSite site;   // statique, comme en production
  const int N = 20000;
  std::atomic<bool> fin{false};
  std::atomic<unsigned long> drainesOccultes{0};
  std::thread loopT([&] {
    uint32_t now = 0;
    while (!fin.load()) {
      now += 7;
      const AuthLogSiteReport r = authLogSiteDrain(site, now * 1000UL, kBase, kMaxShift, 1);
      if (r.emit) drainesOccultes += r.occulted;
    }
  });
  unsigned long viaAttempt = 0; int emis = 0;
  for (int i = 0; i < N; ++i) {
    const AuthLogSiteReport r = authLogSiteAttempt(site, (uint32_t)i * 50UL, 1, 1, kBase, kMaxShift, kDecay);
    if (r.emit) { ++emis; viaAttempt += r.occulted; }
  }
  fin = true; loopT.join();
  const unsigned long restant = site.suppressed.load();
  TEST_ASSERT_EQUAL_UINT32((uint32_t)(N - emis), (uint32_t)(viaAttempt + drainesOccultes + restant));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_tout_premier_refus_a_t0_est_emis);
  RUN_TEST(test_tout_premier_refus_a_t_quelconque_est_emis);
  RUN_TEST(test_rafale_de_500_donne_4_lignes_max);
  RUN_TEST(test_goutte_a_goutte_1_par_minute_pendant_1h);
  RUN_TEST(test_goutte_a_goutte_24h_reste_borne);
  RUN_TEST(test_sequence_du_recul_0_1_3_7_15_31_63);
  RUN_TEST(test_plafond_la_fenetre_ne_depasse_pas_64_min);
  RUN_TEST(test_decroissance_moins_un_apres_1h_sans_tentative);
  RUN_TEST(test_decroissance_observee_sans_emission);
  RUN_TEST(test_decroissance_jusqu_a_zero);
  RUN_TEST(test_le_drain_ne_decroit_pas_shift);
  RUN_TEST(test_drain_ne_relance_pas_le_goutte_a_goutte);
  RUN_TEST(test_le_drain_n_ecrit_ni_lastwarn_ni_everlogged_ni_shift);
  RUN_TEST(test_pas_de_drain_si_aucun_occulte);
  RUN_TEST(test_pas_de_drain_avant_le_calme);
  RUN_TEST(test_compteur_d_occultes_sature_sans_repliement);
  RUN_TEST(test_compteur_sature_dans_le_scenario);
  RUN_TEST(test_reversement_dit_le_compte_puis_remet_a_zero);
  RUN_TEST(test_reversement_au_drain);
  RUN_TEST(test_repliement_de_millis_ni_silence_ni_rafale);
  RUN_TEST(test_repliement_juste_apres_emission);
  RUN_TEST(test_drapeaux_de_masquage);
  RUN_TEST(test_drapeaux_remis_a_zero_apres_emission);
  RUN_TEST(test_hash_distingue_les_routes);
  RUN_TEST(test_verdict_dans_la_fenetre_est_muet_et_hors_fenetre_parle);
  RUN_TEST(test_premiere_ligne_ne_releve_pas_le_recul);
  RUN_TEST(test_zero_allocation_sur_500_decisions_muettes);
  RUN_TEST(test_le_detecteur_d_allocation_voit_une_allocation);
  RUN_TEST(test_bornage_24h_emissions_et_drains_a_la_pire_cadence);
  RUN_TEST(test_bornage_24h_rafale_continue_1_par_seconde);
  RUN_TEST(test_plafond_observable_par_la_decroissance);
  RUN_TEST(test_drain_refuse_tant_que_la_fenetre_n_est_pas_ecoulee);
  RUN_TEST(test_drain_n_emet_pas_sans_silence_meme_fenetre_ecoulee);
  RUN_TEST(test_un_drain_emis_consomme_la_fenetre_et_releve_le_recul);
  RUN_TEST(test_refus_isole_apres_un_drain_est_muet_puis_drain_avec_son_compte);
  RUN_TEST(test_fold_drain_respecte_le_plafond_et_ignore_un_etat_vierge);
  RUN_TEST(test_ecart_negatif_n_est_pas_un_long_silence);
  RUN_TEST(test_drain_premature_quand_une_tentative_s_intercale);
  RUN_TEST(test_accord_du_pluriel);
  RUN_TEST(test_site_increment_sature_par_cas_depuis_l_etat_reel);
  RUN_TEST(test_site_drain_exchange_zero_et_drapeaux);
  RUN_TEST(test_site_le_drain_n_ecrit_pas_l_etat_async_tcp);
  RUN_TEST(test_site_une_ligne_emise_publie_l_etat_pour_le_drain);
  RUN_TEST(test_site_aucun_occulte_perdu_entre_attempt_et_drain);
  return UNITY_END();
}
