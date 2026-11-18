// bug-189 : ralentissement progressif des ECHECS de mot de passe (logique pure, ADR-0017).
// Les tests posent des COMPORTEMENTS (verdicts observables, debits) et non la forme du code.
//
// ⚠️ LIMITE ASSUMEE : les handlers HTTP (web_routes_auth.cpp) ne se compilent pas en natif.
// Les tests « handler » ci-dessous utilisent donc un MODELE du handler, ecrit ici a partir des
// regles de la fiche (conditions 4, 5, 10). Ils prouvent que les briques pures COMPOSEES
// ainsi tiennent la garantie A2 ; ils ne peuvent PAS voir un handler reel qui s'ecarterait
// du modele. Les FORMES de requete modelisees sont celles que produisent data/app.js,
// wizard.html, login.html et la page de secours OTA (voir uiChange/passwordOnly/tokenOnly).
// Cette lacune-la est couverte par tools/check_auth_fail_guard.sh.
#include <cstdio>
#include <cstdint>
#include <unity.h>
#include "auth_fail_backoff_logic.h"
#include "auth_admission_logic.h"
#include "auth_bootstrap_logic.h"

void setUp(void) {}
void tearDown(void) {}

static const AuthFailPolicy kIp = authFailIpPolicy();
static const AuthFailPolicy kGl = authFailGlobalPolicy();
static const uint32_t kIpCap = static_cast<uint32_t>(kAuthFailIpCapMs);
static const uint32_t kGlCap = static_cast<uint32_t>(kAuthFailGlobalCapMs);

static AuthFailVerdict chk(const AuthFailBackoff& s, uint32_t ip, uint32_t now) {
  return authFailCheck(s, ip, now, kIp, kGl);
}
static void fail(AuthFailBackoff& s, uint32_t ip, uint32_t now) {
  authFailRecordFailure(s, ip, now, kIp, kGl);
}
static bool same(const AuthFailGate& a, const AuthFailGate& b) {
  return a.failures == b.failures && a.lastFailMs == b.lastFailMs;
}
static bool sameState(const AuthFailBackoff& a, const AuthFailBackoff& b) {
  if (!same(a.global, b.global) || a.seqCounter != b.seqCounter) return false;
  for (int i = 0; i < kAuthFailIpSlots; i++) {
    if (a.slots[i].used != b.slots[i].used || a.slots[i].ip != b.slots[i].ip || !same(a.slots[i].gate, b.slots[i].gate)) return false;
  }
  for (int i = 0; i < kAuthFailExemptSlots; i++) {
    if (a.exempt[i].used != b.exempt[i].used || a.exempt[i].ip != b.exempt[i].ip ||
        a.exempt[i].seq != b.exempt[i].seq || !same(a.exempt[i].gate, b.exempt[i].gate)) return false;
  }
  return true;
}
// Pousse la porte globale au plafond SANS toucher a l'adresse `avoid` : 14 echecs, chacun
// d'une adresse distincte (un echec chacun : aucune porte IP ne refuse).
static void saturateGlobal(AuthFailBackoff& s, uint32_t now) {
  for (uint32_t k = 0; k < 14; k++) fail(s, 0x0B000000u + k, now);
}

// ---------------------------------------------------------------------------
// Modele des handlers (voir l'avertissement en tete).
// ---------------------------------------------------------------------------
static int loginModel(AuthFailBackoff& s, uint32_t ip, uint32_t now, bool userOk, bool pwOk, bool factory) {
  if (chk(s, ip, now).refuse) return 429;
  switch (decideLogin(userOk, pwOk, factory)) {
    case LoginVerdict::kAllow: authFailRecordPasswordProof(s, ip); return 200;
    case LoginVerdict::kDenyMustChangePassword: return 403;
    case LoginVerdict::kDenyCredentials:
    default: fail(s, ip, now); return 401;
  }
}
// `in.tokenValid` = « l'en-tete X-Auth-Token porte un jeton valide » (resultat de checkTokenAuth).
// Comme le handler reel : la garde est sautee si le jeton est valide, et PwdChangeInput.tokenValid
// n'est renseigne que sans currentPassword.
static int changeModel(AuthFailBackoff& s, uint32_t ip, uint32_t now, PwdChangeInput in) {
  const bool tokenOk = in.tokenValid;
  in.tokenValid = (!in.virginState && !in.hasCurrentPassword) ? tokenOk : false;
  if (in.hasCurrentPassword && !in.virginState && !tokenOk) {
    if (chk(s, ip, now).refuse) return 429;
  }
  switch (decidePasswordChange(in)) {
    case PwdChangeVerdict::kAllow:
      authFailClearExemptions(s);           // setPassword()
      authFailRecordPasswordProof(s, ip);   // reinscription de l'auteur (ip == 0 ignoree)
      return 200;
    case PwdChangeVerdict::kDenyUnauth: return 401;
    case PwdChangeVerdict::kDenyBadCurrent:
      if (!in.virginState) fail(s, ip, now);
      return 401;
    case PwdChangeVerdict::kDenyTooShort: return 400;
    case PwdChangeVerdict::kDenySame:
    default: return 400;
  }
}
// Formes REELLEMENT produites par les clients du depot :
//  - uiChange : data/app.js:15236 — X-Auth-Token ET currentPassword, ENSEMBLE (cas normal).
//  - passwordOnly : wizard.html, login.html, page de secours OTA (currentPassword seul, sans jeton).
//  - tokenOnly : AUCUN client du depot ; garde pour un client futur / un outil externe.
static PwdChangeInput uiChange(bool currentOk, bool tokenValid = true) {
  PwdChangeInput in;
  in.hasCurrentPassword = true;
  in.currentMatches = currentOk;
  in.tokenValid = tokenValid;
  in.newLenOk = true;
  in.newFamiliesOk = true;  // bug-187 : le modele represente un mot de passe CONFORME
  return in;
}
static PwdChangeInput passwordOnly(bool currentOk) { return uiChange(currentOk, false); }
static PwdChangeInput tokenOnly(bool tokenValid) {
  PwdChangeInput in;
  in.tokenValid = tokenValid;
  in.newLenOk = true;
  in.newFamiliesOk = true;  // bug-187 : le modele represente un mot de passe CONFORME
  return in;
}

// ---------------------------------------------------------------------------
// Montee, franchise, decroissance
// ---------------------------------------------------------------------------
void test_ip_franchise_puis_montee_doublee_jusqu_au_plafond(void) {
  AuthFailGate g;
  const uint32_t expect[] = {0, 0, 0, 1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000, 60000};
  for (int n = 1; n <= 12; n++) {
    authFailGateRecord(g, 5000, kIp);
    char m[40]; snprintf(m, sizeof m, "apres %d echecs", n);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(expect[n - 1], authFailRemainingMs(g, 5000, kIp), m);
  }
}
void test_franchise_ip_trois_essais_gratuits(void) {
  AuthFailBackoff s;
  for (int i = 0; i < 3; i++) { TEST_ASSERT_FALSE(chk(s, 1, 100).refuse); fail(s, 1, 100); }
  TEST_ASSERT_FALSE(chk(s, 1, 100).refuse);  // 3 echecs : encore admis
  fail(s, 1, 100);                           // 4e
  TEST_ASSERT_TRUE(chk(s, 1, 100).refuse);
  TEST_ASSERT_EQUAL_UINT32(1000, chk(s, 1, 100).retryAfterMs);
  TEST_ASSERT_FALSE(chk(s, 1, 100 + 1000).refuse);  // l'attente ecoulee, on repasse
  TEST_ASSERT_FALSE(chk(s, 2, 100).refuse);         // une autre adresse n'est pas freinee (IP seule)
}
void test_franchise_globale_dix_essais_toutes_adresses_confondues(void) {
  AuthFailBackoff s;
  for (uint32_t k = 0; k < 10; k++) fail(s, 0x0C000000u + k, 100);  // 10 adresses, 1 echec chacune
  TEST_ASSERT_FALSE(chk(s, 0x0D000001u, 100).refuse);               // une 11e adresse neuve : admise
  fail(s, 0x0C0000FFu, 100);                                        // 11e echec
  TEST_ASSERT_TRUE(chk(s, 0x0D000001u, 100).refuse);
  TEST_ASSERT_EQUAL_UINT32(1000, chk(s, 0x0D000001u, 100).retryAfterMs);
}
void test_decroissance_incrementale_un_echec_par_periode(void) {
  AuthFailGate g;
  for (int i = 0; i < 10; i++) authFailGateRecord(g, 0, kIp);
  const uint32_t d = static_cast<uint32_t>(kAuthFailIpDecayMs);
  TEST_ASSERT_EQUAL_UINT8(10, authFailEffective(g, d - 1, kIp));
  TEST_ASSERT_EQUAL_UINT8(9, authFailEffective(g, d, kIp));
  TEST_ASSERT_EQUAL_UINT8(8, authFailEffective(g, 2 * d, kIp));
  // ⛔ PAS un pardon total : apres 9 periodes il en reste un.
  TEST_ASSERT_EQUAL_UINT8(1, authFailEffective(g, 9 * d, kIp));
  TEST_ASSERT_EQUAL_UINT8(0, authFailEffective(g, 10 * d, kIp));
  // Meme loi sur la porte globale (60 s).
  AuthFailGate h;
  for (int i = 0; i < 20; i++) authFailGateRecord(h, 0, kGl);
  const uint32_t dg = static_cast<uint32_t>(kAuthFailGlobalDecayMs);
  TEST_ASSERT_EQUAL_UINT8(19, authFailEffective(h, dg, kGl));
  TEST_ASSERT_EQUAL_UINT8(15, authFailEffective(h, 5 * dg, kGl));
}
void test_une_longue_pause_rend_la_franchise(void) {
  // Comportement observable : 4 echecs (refus 1 s), 15 min de silence (3 pardons) -> 1 echec
  // restant ; un nouvel echec ne ramene PAS au refus. Sans decroissance : 5 echecs, refus 2 s.
  AuthFailBackoff s;
  for (int i = 0; i < 4; i++) fail(s, 7, 1000);
  TEST_ASSERT_TRUE(chk(s, 7, 1000).refuse);
  const uint32_t later = 1000 + 3 * static_cast<uint32_t>(kAuthFailIpDecayMs);
  fail(s, 7, later);
  TEST_ASSERT_FALSE(chk(s, 7, later).refuse);
}
void test_un_echec_pendant_le_pardon_part_de_l_effectif_pas_du_brut(void) {
  AuthFailGate g;
  for (int i = 0; i < 6; i++) authFailGateRecord(g, 0, kIp);
  const uint32_t t = 3 * static_cast<uint32_t>(kAuthFailIpDecayMs);  // 3 pardons : effectif 3
  authFailGateRecord(g, t, kIp);                                      // -> 4, pas 7
  TEST_ASSERT_EQUAL_UINT8(4, g.failures);
}

// ---------------------------------------------------------------------------
// Bornes : plafond, saturation, repliement de millis()
// ---------------------------------------------------------------------------
void test_plafond_dur_jamais_franchi_pour_tout_nombre_d_echecs(void) {
  for (int eff = 0; eff <= 255; eff++) {
    TEST_ASSERT_TRUE(authFailWaitMs(static_cast<uint8_t>(eff), kIp) <= kIpCap);
    TEST_ASSERT_TRUE(authFailWaitMs(static_cast<uint8_t>(eff), kGl) <= kGlCap);
  }
  TEST_ASSERT_EQUAL_UINT32(kIpCap, authFailWaitMs(255, kIp));
  TEST_ASSERT_EQUAL_UINT32(kGlCap, authFailWaitMs(255, kGl));
  // Le plafond est ATTEINT (pas seulement respecte) : sinon un plafond a 0 passerait.
  TEST_ASSERT_EQUAL_UINT32(kIpCap, authFailWaitMs(10, kIp));
  TEST_ASSERT_EQUAL_UINT32(kGlCap, authFailWaitMs(15, kGl));
}
void test_saturation_du_compteur_a_255(void) {
  AuthFailGate g;
  for (int i = 0; i < 400; i++) authFailGateRecord(g, 0, kIp);
  TEST_ASSERT_EQUAL_UINT8(255, g.failures);
  TEST_ASSERT_EQUAL_UINT32(kIpCap, authFailRemainingMs(g, 0, kIp));
}
void test_repliement_millis_etat_pres_de_0xFFFFFF00(void) {
  AuthFailGate g;
  for (int i = 0; i < 20; i++) authFailGateRecord(g, 0xFFFFFF00u, kGl);  // porte globale au plafond
  // 512 ms plus tard, de l'autre cote du repliement : attente EXACTE, pas une valeur geante.
  TEST_ASSERT_EQUAL_UINT32(kGlCap - 512u, authFailRemainingMs(g, 0x00000100u, kGl));
  // Meme chose au niveau du verdict complet.
  AuthFailBackoff s;
  for (int i = 0; i < 20; i++) fail(s, 0x0E000001u, 0xFFFFFF00u);
  const AuthFailVerdict v = chk(s, 0x0E000001u, 0x00000100u);
  TEST_ASSERT_TRUE(v.refuse);
  TEST_ASSERT_TRUE(v.retryAfterMs <= kIpCap);
  TEST_ASSERT_EQUAL_UINT32(kIpCap - 512u, v.retryAfterMs);
}
void test_etat_ancien_ne_bloque_jamais_au_dela_du_plafond(void) {
  // 24,8 j = 2^31 ms ; 49,7 j = 2^32 ms. Etats poses a plusieurs dates, relus apres des
  // durees autour de ces deux seuils : l'attente restante est TOUJOURS <= plafond.
  const uint32_t lasts[] = {0u, 1u, 1000u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFF00u, 0xFFFFFFFFu};
  const uint32_t elapsed[] = {0u, 1u, 9999u, 10000u, 59999u, 60000u, 299999u, 300000u, 86400000u,
                              0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFFFFFEu, 0xFFFFFFFFu};
  for (uint32_t last : lasts) {
    for (uint32_t e : elapsed) {
      AuthFailGate g; g.failures = 255; g.lastFailMs = last;
      const uint32_t now = last + e;  // arithmetique non signee, repliement voulu
      char m[64]; snprintf(m, sizeof m, "last=%08X elapsed=%08X", (unsigned)last, (unsigned)e);
      TEST_ASSERT_TRUE_MESSAGE(authFailRemainingMs(g, now, kIp) <= kIpCap, m);
      TEST_ASSERT_TRUE_MESSAGE(authFailRemainingMs(g, now, kGl) <= kGlCap, m);
    }
  }
  // Un etat plus vieux que 24,8 j (donc pardonne) ne refuse plus rien.
  AuthFailBackoff s;
  for (int i = 0; i < 30; i++) fail(s, 0x0E000002u, 5000u);
  TEST_ASSERT_FALSE(chk(s, 0x0E000002u, 5000u + 0x80000001u).refuse);
}
void test_attente_independante_de_la_position_de_l_horloge(void) {
  // Pour un ecart donne (< periode de pardon), l'attente restante ne depend pas de la date
  // absolue : meme valeur avant, pres et apres le repliement.
  const uint32_t starts[] = {0u, 12345u, 0x7FFFFF00u, 0xFFFFFC00u, 0xFFFFFFFFu};
  for (uint32_t t0 : starts) {
    AuthFailGate g;
    for (int i = 0; i < 6; i++) authFailGateRecord(g, t0, kIp);  // eff 6 -> attente 4000
    TEST_ASSERT_EQUAL_UINT32(4000u, authFailRemainingMs(g, t0, kIp));
    TEST_ASSERT_EQUAL_UINT32(3000u, authFailRemainingMs(g, t0 + 1000u, kIp));
    TEST_ASSERT_EQUAL_UINT32(0u, authFailRemainingMs(g, t0 + 4000u, kIp));
  }
}

// ---------------------------------------------------------------------------
// Refus sans effet, succes sans effet sur la globale
// ---------------------------------------------------------------------------
void test_un_refus_ne_modifie_aucun_etat(void) {
  AuthFailBackoff s;
  for (int i = 0; i < 6; i++) fail(s, 1, 100);
  saturateGlobal(s, 100);
  const AuthFailBackoff snap = s;
  for (int k = 0; k < 50; k++) {
    TEST_ASSERT_TRUE(chk(s, 1, 100 + k).refuse);
    TEST_ASSERT_TRUE(chk(s, 0x0F000001u, 100 + k).refuse);
  }
  TEST_ASSERT_TRUE(sameState(snap, s));
  // Au niveau du handler : un 429 non plus ne compte pas.
  const AuthFailBackoff snap2 = s;
  TEST_ASSERT_EQUAL(429, loginModel(s, 1, 100, true, false, false));
  TEST_ASSERT_TRUE(sameState(snap2, s));
}
void test_un_succes_ne_remet_pas_la_porte_globale_a_zero(void) {
  AuthFailBackoff s;
  saturateGlobal(s, 1000);
  const AuthFailGate before = s.global;
  authFailRecordPasswordProof(s, 0x10000001u);
  TEST_ASSERT_TRUE(same(before, s.global));
  // Et un autre client, non exempte, reste refuse.
  TEST_ASSERT_TRUE(chk(s, 0x10000002u, 1000).refuse);
  TEST_ASSERT_FALSE(chk(s, 0x10000001u, 1000).refuse);
}
void test_un_succes_libere_le_creneau_de_l_adresse(void) {
  // Les echecs de l'adresse sont effaces par la preuve. Observable apres un changement de
  // mot de passe (table videe) : l'adresse n'est plus exemptee et ne doit plus etre freinee.
  AuthFailBackoff s;
  for (int i = 0; i < 6; i++) fail(s, 0x11000001u, 1000);
  TEST_ASSERT_TRUE(chk(s, 0x11000001u, 1000).refuse);
  authFailRecordPasswordProof(s, 0x11000001u);
  authFailClearExemptions(s);
  // la globale a 6 echecs : sous la franchise ; seule la porte IP aurait pu refuser.
  TEST_ASSERT_FALSE(chk(s, 0x11000001u, 1000).refuse);
}
void test_une_preuve_remet_a_zero_la_propre_porte_de_l_exemptee(void) {
  AuthFailBackoff s;
  authFailRecordPasswordProof(s, 0x12000001u);
  for (int i = 0; i < 8; i++) fail(s, 0x12000001u, 1000);  // exemptee qui rate : SA porte monte
  TEST_ASSERT_TRUE(chk(s, 0x12000001u, 1000).refuse);
  authFailRecordPasswordProof(s, 0x12000001u);              // elle reprouve
  TEST_ASSERT_FALSE(chk(s, 0x12000001u, 1000).refuse);
}

// ---------------------------------------------------------------------------
// Composition des trois portes
// ---------------------------------------------------------------------------
void test_retry_after_est_le_plus_grand_des_delais_applicables(void) {
  // IP lente (60 s) + globale plus courte (10 s) -> 60 s.
  AuthFailBackoff a;
  for (int i = 0; i < 15; i++) fail(a, 0x13000001u, 1000);
  TEST_ASSERT_EQUAL_UINT32(kIpCap, chk(a, 0x13000001u, 1000).retryAfterMs);
  TEST_ASSERT_EQUAL_UINT32(kGlCap, chk(a, 0x13000099u, 1000).retryAfterMs);  // inconnue : globale seule
  // IP courte (1 s) + globale plus longue (10 s) -> 10 s.
  AuthFailBackoff b;
  saturateGlobal(b, 1000);
  for (int i = 0; i < 4; i++) fail(b, 0x13000002u, 1000);
  TEST_ASSERT_EQUAL_UINT32(kGlCap, chk(b, 0x13000002u, 1000).retryAfterMs);
}
void test_retry_after_secondes_arrondi_au_dessus(void) {
  AuthFailVerdict v; v.refuse = true; v.retryAfterMs = 1;
  TEST_ASSERT_EQUAL_UINT32(1, v.retryAfterSec());
  v.retryAfterMs = 1000; TEST_ASSERT_EQUAL_UINT32(1, v.retryAfterSec());
  v.retryAfterMs = 1001; TEST_ASSERT_EQUAL_UINT32(2, v.retryAfterSec());
  v.retryAfterMs = 60000; TEST_ASSERT_EQUAL_UINT32(60, v.retryAfterSec());
  v.refuse = false; TEST_ASSERT_EQUAL_UINT32(0, v.retryAfterSec());
}
void test_table_d_exemption_pleine_porte_globale_seule_sans_eviction_des_creneaux_ip(void) {
  AuthFailBackoff s;
  // 8 adresses non exemptees occupent les 8 creneaux, chacune a 4 echecs (IP : 1 s)... la
  // globale monte aussi, donc on mesure l'IP avant/apres sur un echantillon controle.
  for (uint32_t k = 0; k < 8; k++) for (int i = 0; i < 4; i++) fail(s, 0x14000000u + k, 1000);
  // Table d'exemption pleine de 4 autres adresses.
  for (uint32_t k = 0; k < 4; k++) authFailRecordPasswordProof(s, 0x15000000u + k);
  AuthFailVerdict before[8];
  for (uint32_t k = 0; k < 8; k++) before[k] = chk(s, 0x14000000u + k, 1000);
  // Une 5e adresse prouve : la plus ancienne exemptee est evincee, AUCUN creneau IP ne bouge.
  authFailRecordPasswordProof(s, 0x15000099u);
  for (uint32_t k = 0; k < 8; k++) {
    const AuthFailVerdict a = chk(s, 0x14000000u + k, 1000);
    TEST_ASSERT_EQUAL(before[k].refuse, a.refuse);
    TEST_ASSERT_EQUAL_UINT32(before[k].retryAfterMs, a.retryAfterMs);
    TEST_ASSERT_TRUE(a.refuse);
  }
  // L'evincee (0x15000000, la plus ancienne) est jugee par la globale SEULE (pas de creneau).
  const AuthFailVerdict ev = chk(s, 0x15000000u, 1000);
  const AuthFailVerdict stranger = chk(s, 0x15000FFFu, 1000);
  TEST_ASSERT_TRUE(ev.refuse);  // la globale est haute (32 echecs)
  TEST_ASSERT_EQUAL_UINT32(stranger.retryAfterMs, ev.retryAfterMs);
  TEST_ASSERT_EQUAL_UINT32(kGlCap, ev.retryAfterMs);
  // Les exemptees restantes passent.
  TEST_ASSERT_FALSE(chk(s, 0x15000001u, 1000).refuse);
  TEST_ASSERT_FALSE(chk(s, 0x15000099u, 1000).refuse);
}
void test_condition9_creneaux_ip_satures_l_exemptee_reste_plafonnee_a_60s(void) {
  AuthFailBackoff s;
  const uint32_t owner = 0x16000001u;
  authFailRecordPasswordProof(s, owner);
  for (uint32_t k = 0; k < kAuthFailIpSlots; k++) fail(s, 0x16100000u + k, 1000);  // creneaux pleins
  const AuthFailBackoff slotsFull = s;
  TEST_ASSERT_FALSE(chk(s, owner, 1000).refuse);
  for (int i = 0; i < 40; i++) fail(s, owner, 1000);  // l'exemptee martele
  const AuthFailVerdict v = chk(s, owner, 1000);
  TEST_ASSERT_TRUE(v.refuse);                       // elle A une limite
  TEST_ASSERT_EQUAL_UINT32(kIpCap, v.retryAfterMs); // et c'est 60 s, pas davantage
  // Ses echecs n'ont ni nourri la globale ni pris un creneau.
  TEST_ASSERT_TRUE(same(slotsFull.global, s.global));
  for (int i = 0; i < kAuthFailIpSlots; i++) TEST_ASSERT_TRUE(same(slotsFull.slots[i].gate, s.slots[i].gate));
  // Et une fois l'attente ecoulee elle repasse (un essai par 60 s : 1 440/jour au mieux).
  TEST_ASSERT_FALSE(chk(s, owner, 1000 + kIpCap).refuse);
}
void test_l_exemptee_echappe_a_la_globale(void) {
  AuthFailBackoff s;
  authFailRecordPasswordProof(s, 0x17000001u);
  saturateGlobal(s, 1000);
  TEST_ASSERT_TRUE(chk(s, 0x17000002u, 1000).refuse);
  TEST_ASSERT_FALSE(chk(s, 0x17000001u, 1000).refuse);
}

// ---------------------------------------------------------------------------
// Table d'exemption : eviction par seq
// ---------------------------------------------------------------------------
static void fillFourThenEvict(uint32_t startSeq) {
  AuthFailBackoff s;
  s.seqCounter = startSeq;
  const uint32_t A = 0x20000001u, B = 0x20000002u, C = 0x20000003u, D = 0x20000004u, E = 0x20000005u;
  authFailRecordPasswordProof(s, A);
  authFailRecordPasswordProof(s, B);
  authFailRecordPasswordProof(s, C);
  authFailRecordPasswordProof(s, D);
  authFailRecordPasswordProof(s, E);  // table pleine : evince la PLUS ANCIENNE, A
  saturateGlobal(s, 1000);            // seuls les non-exemptes sont refuses
  TEST_ASSERT_TRUE_MESSAGE(chk(s, A, 1000).refuse, "A (la plus ancienne) devait etre evincee");
  TEST_ASSERT_FALSE_MESSAGE(chk(s, B, 1000).refuse, "B");
  TEST_ASSERT_FALSE_MESSAGE(chk(s, C, 1000).refuse, "C");
  TEST_ASSERT_FALSE_MESSAGE(chk(s, D, 1000).refuse, "D");
  TEST_ASSERT_FALSE_MESSAGE(chk(s, E, 1000).refuse, "E (la plus recente) doit rester");
}
void test_eviction_de_la_plus_ancienne_pas_de_la_plus_recente(void) { fillFourThenEvict(0); }
void test_eviction_correcte_au_repliement_du_compteur_seq(void) {
  fillFourThenEvict(0xFFFFFFFEu);  // seq : A=0xFFFFFFFF, B=0, C=1, D=2 (replie), E=3
  fillFourThenEvict(0xFFFFFFFDu);  // repliement au milieu des quatre
  fillFourThenEvict(0xFFFFFFFFu);  // replie des la premiere inscription
}
void test_reinscription_d_une_exemptee_la_rajeunit(void) {
  AuthFailBackoff s;
  const uint32_t A = 0x21000001u, B = 0x21000002u, C = 0x21000003u, D = 0x21000004u, E = 0x21000005u;
  authFailRecordPasswordProof(s, A);
  authFailRecordPasswordProof(s, B);
  authFailRecordPasswordProof(s, C);
  authFailRecordPasswordProof(s, D);
  authFailRecordPasswordProof(s, A);  // A reprouve : desormais la plus recente
  authFailRecordPasswordProof(s, E);  // evince B
  saturateGlobal(s, 1000);
  TEST_ASSERT_TRUE(chk(s, B, 1000).refuse);
  TEST_ASSERT_FALSE(chk(s, A, 1000).refuse);
  TEST_ASSERT_FALSE(chk(s, C, 1000).refuse);
  TEST_ASSERT_FALSE(chk(s, D, 1000).refuse);
  TEST_ASSERT_FALSE(chk(s, E, 1000).refuse);
}
void test_table_jamais_au_dela_de_quatre_exemptees(void) {
  AuthFailBackoff s;
  for (uint32_t k = 0; k < 20; k++) authFailRecordPasswordProof(s, 0x22000000u + 1 + k);
  saturateGlobal(s, 1000);
  int pass = 0;
  for (uint32_t k = 0; k < 20; k++) if (!chk(s, 0x22000000u + 1 + k, 1000).refuse) pass++;
  TEST_ASSERT_EQUAL(kAuthFailExemptSlots, pass);
}

// ---------------------------------------------------------------------------
// Condition 10 : changement de mot de passe
// ---------------------------------------------------------------------------
void test_condition10_tout_kAllow_de_change_password_vide_puis_reinscrit_l_auteur(void) {
  const uint32_t me = 0x30000001u;
  PwdChangeInput paths[4] = {uiChange(true), tokenOnly(true), passwordOnly(true), PwdChangeInput()};
  paths[3].virginState = true;  // appareil vierge : droit anonyme
  paths[3].newLenOk = true;
  paths[3].newFamiliesOk = true;  // bug-187
  for (int p = 0; p < 4; p++) {
    AuthFailBackoff s;
    authFailRecordPasswordProof(s, 0x30000010u);  // anciennes preuves
    authFailRecordPasswordProof(s, 0x30000011u);
    TEST_ASSERT_EQUAL_MESSAGE(200, changeModel(s, me, 5000, paths[p]), "chemin");
    saturateGlobal(s, 5000);
    TEST_ASSERT_TRUE_MESSAGE(chk(s, 0x30000010u, 5000).refuse, "ancienne preuve non videe");
    TEST_ASSERT_TRUE_MESSAGE(chk(s, 0x30000011u, 5000).refuse, "ancienne preuve non videe");
    TEST_ASSERT_FALSE_MESSAGE(chk(s, me, 5000).refuse, "auteur non reinscrit");
  }
}
void test_condition10_ip_zero_jamais_exemptee(void) {
  AuthFailBackoff s;
  authFailRecordPasswordProof(s, 0x31000001u);
  TEST_ASSERT_EQUAL(200, changeModel(s, 0, 5000, tokenOnly(true)));  // table videe, personne reinscrit
  saturateGlobal(s, 5000);
  TEST_ASSERT_TRUE(chk(s, 0, 5000).refuse);            // ip 0 : jugee par la globale
  TEST_ASSERT_TRUE(chk(s, 0x31000001u, 5000).refuse);  // et l'ancienne preuve est tombee
  // Un login reussi depuis ip 0 non plus ne l'exempte pas.
  AuthFailBackoff t;
  TEST_ASSERT_EQUAL(200, loginModel(t, 0, 5000, true, true, false));
  saturateGlobal(t, 5000);
  TEST_ASSERT_TRUE(chk(t, 0, 5000).refuse);
}

// ---------------------------------------------------------------------------
// ⭐ A2 : un jeton valide PASSE toujours, portes IP ET globale au plafond
// ---------------------------------------------------------------------------
void test_A2_portes_ip_et_globale_au_plafond_le_jeton_valide_passe(void) {
  const uint32_t owner = 0x40000001u;
  AuthFailBackoff s;
  for (int i = 0; i < 40; i++) fail(s, owner, 1000);  // porte IP du proprietaire au plafond
  saturateGlobal(s, 1000);                            // globale au plafond
  TEST_ASSERT_EQUAL_UINT32(kIpCap, chk(s, owner, 1000).retryAfterMs);  // precondition : les portes sont BIEN fermees
  TEST_ASSERT_TRUE(chk(s, 0x40000002u, 1000).refuse);
  // L'essai de MOT DE PASSE est freine...
  TEST_ASSERT_EQUAL(429, changeModel(s, owner, 1000, passwordOnly(true)));
  TEST_ASSERT_EQUAL(429, loginModel(s, owner, 1000, true, true, false));
  // ... le changement par JETON VALIDE passe, depuis l'adresse bloquee comme depuis une autre.
  AuthFailBackoff s2 = s;
  TEST_ASSERT_EQUAL(200, changeModel(s, owner, 1000, tokenOnly(true)));
  TEST_ASSERT_EQUAL(200, changeModel(s2, 0x40000002u, 1000, tokenOnly(true)));
  // Et l'auteur est desormais exempte de la globale (reinscription).
  TEST_ASSERT_FALSE(chk(s2, 0x40000002u, 1000).refuse);
}
void test_A2_forme_reelle_de_l_interface_jeton_ET_mot_de_passe_actuel_passe(void) {
  // ⭐ data/app.js:15236 envoie X-Auth-Token ET currentPassword ensemble. C'est CETTE forme
  // qui doit passer sous attaque (le modele « jeton seul » validait un chemin sans client).
  const uint32_t owner = 0x43000001u;
  AuthFailBackoff s;
  for (int i = 0; i < 40; i++) fail(s, owner, 1000);
  saturateGlobal(s, 1000);
  TEST_ASSERT_TRUE(chk(s, owner, 1000).refuse);                                // portes fermees
  TEST_ASSERT_EQUAL(429, changeModel(s, owner, 1000, passwordOnly(true)));     // sans jeton : freine
  TEST_ASSERT_EQUAL(429, changeModel(s, owner, 1000, uiChange(true, false)));  // jeton perime : freine
  AuthFailBackoff s2 = s;
  TEST_ASSERT_EQUAL(200, changeModel(s, owner, 1000, uiChange(true)));        // forme reelle : PASSE
  TEST_ASSERT_EQUAL(200, changeModel(s2, 0x43000002u, 1000, uiChange(true))); // depuis une autre adresse aussi
  TEST_ASSERT_FALSE(chk(s2, 0x43000002u, 1000).refuse);                       // et l'auteur est reinscrit
}
void test_A2_jeton_valide_et_mauvais_mot_de_passe_actuel_est_compte_pas_freine(void) {
  // COMPORTEMENT FIGE : un porteur de jeton qui se trompe d'ancien mot de passe n'est pas
  // freine (401 meme portes fermees) mais l'echec EST compte (kDenyBadCurrent).
  AuthFailBackoff s;
  saturateGlobal(s, 1000);
  const uint32_t before = s.global.failures;
  TEST_ASSERT_EQUAL(401, changeModel(s, 0x44000001u, 1000, uiChange(false)));
  TEST_ASSERT_EQUAL_UINT8(before + 1, s.global.failures);  // non exempte : nourrit la GLOBALE
  // Exempte (cas normal d'un proprietaire connecte) : nourrit SA porte, pas la globale.
  AuthFailBackoff t;
  authFailRecordPasswordProof(t, 0x44000002u);
  saturateGlobal(t, 1000);
  const AuthFailGate g = t.global;
  TEST_ASSERT_EQUAL(401, changeModel(t, 0x44000002u, 1000, uiChange(false)));
  TEST_ASSERT_TRUE(same(g, t.global));
  TEST_ASSERT_EQUAL_UINT8(1, t.exempt[0].gate.failures);
}
void test_A2_route_protegee_n_est_jamais_freinee_par_les_portes(void) {
  // L'admission d'une route protegee ne prend AUCUN verdict de ralentissement en entree :
  // le jeton valide donne kAllow quel que soit l'etat des portes (portes au plafond).
  AuthFailBackoff s;
  for (int i = 0; i < 60; i++) fail(s, 0x41000001u, 1000);
  saturateGlobal(s, 1000);
  TEST_ASSERT_TRUE(chk(s, 0x41000001u, 1000).refuse);  // les portes SONT fermees
  AuthAdmissionInput in;
  in.authEnabled = true;
  in.publicRoute = false;
  in.rateLimitOk = true;
  in.tokenValid = true;
  TEST_ASSERT_EQUAL((int)AuthVerdict::kAllow, (int)decideAdmission(in));
  // Et une route protegee n'enregistre rien : l'etat des portes est inchange par l'admission.
  const AuthFailBackoff snap = s;
  (void)decideAdmission(in);
  TEST_ASSERT_TRUE(sameState(snap, s));
}
void test_A2_un_jeton_invalide_n_est_ni_compte_ni_freine(void) {
  AuthFailBackoff s;
  const AuthFailBackoff snap = s;
  for (int i = 0; i < 100; i++) TEST_ASSERT_EQUAL(401, changeModel(s, 0x42000001u, 1000, tokenOnly(false)));
  TEST_ASSERT_TRUE(sameState(snap, s));  // aucun essai de jeton ne nourrit une porte
  AuthAdmissionInput in; in.rateLimitOk = true; in.tokenValid = false;
  TEST_ASSERT_EQUAL((int)AuthVerdict::kDenyUnauth, (int)decideAdmission(in));
}

// ---------------------------------------------------------------------------
// Condition 4 : ce qui compte et ce qui ne compte pas
// ---------------------------------------------------------------------------
void test_condition4_les_deux_natures_d_echec_de_login_sont_comptees_a_l_identique(void) {
  AuthFailBackoff a, b;
  for (int i = 0; i < 4; i++) {  // 3 de franchise + le 4e : encore admis, puis compte
    TEST_ASSERT_EQUAL(401, loginModel(a, 0x50000001u, 1000, /*user*/ false, /*pw*/ true, false));   // nom inconnu
    TEST_ASSERT_EQUAL(401, loginModel(b, 0x50000001u, 1000, /*user*/ true, /*pw*/ false, false));   // mot de passe faux
  }
  TEST_ASSERT_TRUE(sameState(a, b));  // sinon : oracle d'enumeration du compte
  TEST_ASSERT_TRUE(chk(a, 0x50000001u, 1000).refuse);
}
void test_condition4_ne_comptent_pas_les_403_les_400_et_l_etat_vierge(void) {
  AuthFailBackoff s;
  const AuthFailBackoff snap = s;
  const uint32_t ip = 0x51000001u;
  for (int i = 0; i < 30; i++) {
    TEST_ASSERT_EQUAL(403, loginModel(s, ip, 1000, true, true, /*factory*/ true));  // 403 mot de passe d'usine
    PwdChangeInput shortPw = uiChange(true); shortPw.newLenOk = false;
    TEST_ASSERT_EQUAL(400, changeModel(s, ip, 1000, shortPw));
    PwdChangeInput same_ = uiChange(true); same_.newEqualsCurrent = true;
    TEST_ASSERT_EQUAL(400, changeModel(s, ip, 1000, same_));
    // Etat vierge, mauvais « currentPassword » : ni freine ni compte.
    PwdChangeInput virginBad;
    virginBad.virginState = true; virginBad.hasCurrentPassword = true;
    virginBad.currentIsFactoryLiteral = false; virginBad.newLenOk = true; virginBad.newFamiliesOk = true;
    TEST_ASSERT_EQUAL(401, changeModel(s, ip, 1000, virginBad));
  }
  TEST_ASSERT_TRUE(sameState(snap, s));
}
void test_condition4_kDenyBadCurrent_hors_etat_vierge_compte(void) {
  AuthFailBackoff s;
  for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL(401, changeModel(s, 0x52000001u, 1000, passwordOnly(false)));
  TEST_ASSERT_EQUAL(429, changeModel(s, 0x52000001u, 1000, passwordOnly(false)));
}
void test_prise_de_possession_sur_appareil_vierge_jamais_affamee(void) {
  // Porte globale au plafond : l'etat vierge est public, la prise de possession passe.
  AuthFailBackoff s;
  saturateGlobal(s, 1000);
  PwdChangeInput takeover;
  takeover.virginState = true; takeover.hasCurrentPassword = true;
  takeover.currentIsFactoryLiteral = true; takeover.newLenOk = true; takeover.newFamiliesOk = true;
  TEST_ASSERT_EQUAL(200, changeModel(s, 0x53000001u, 1000, takeover));
}

// ---------------------------------------------------------------------------
// ⭐ D1-bis : debit de l'attaquant optimal (fige le chiffre annonce)
// ---------------------------------------------------------------------------
// L'attaquant frappe sur l'horloge 32 bits (donc repliee) ; un compteur 64 bits ne sert
// qu'a borner l'horizon. `ipsPool` adresses tournantes ; 1 = une seule adresse.
static uint64_t simulate(uint32_t startMs, uint64_t horizonMs, uint32_t ipsPool, int burst, uint32_t silenceMs) {
  AuthFailBackoff s;
  uint64_t t = 0, tries = 0;
  int inBurst = 0;
  uint32_t k = 0;
  while (t < horizonMs) {
    const uint32_t ip = 0x60000000u + (k % ipsPool);
    const uint32_t now = startMs + static_cast<uint32_t>(t);
    const AuthFailVerdict v = chk(s, ip, now);
    if (v.refuse) { t += v.retryAfterMs; continue; }  // attend l'instant exact de la reouverture
    fail(s, ip, now);
    tries++; k++;
    if (burst > 0 && ++inBurst >= burst) { inBurst = 0; t += silenceMs; }
  }
  return tries;
}
static const uint64_t kDay = 86400000ull;

void test_debit_porte_globale_attaque_simple_8640_par_jour(void) {
  const uint64_t n = simulate(0u, 7 * kDay, 1000, 0, 0);
  const double perDay = static_cast<double>(n) / 7.0;
  char m[64]; snprintf(m, sizeof m, "essais/jour = %.1f", perDay);
  TEST_ASSERT_TRUE_MESSAGE(perDay >= 8600.0 && perDay <= 8700.0, m);
  // Meme debit quelle que soit la position de l'horloge (repliement en cours de simulation).
  const uint64_t w = simulate(0xFFFFF000u, 7 * kDay, 1000, 0, 0);
  TEST_ASSERT_TRUE(w + 5 >= n && n + 5 >= w);
}
void test_debit_porte_ip_une_seule_adresse_1440_par_jour(void) {
  const uint64_t n = simulate(0u, 7 * kDay, 1, 0, 0);
  const double perDay = static_cast<double>(n) / 7.0;
  char m[64]; snprintf(m, sizeof m, "essais/jour = %.1f", perDay);
  TEST_ASSERT_TRUE_MESSAGE(perDay >= 1435.0 && perDay <= 1450.0, m);
}
void test_debit_rafale_puis_silence_ne_bat_pas_l_attaque_simple(void) {
  // NON-REGRESSION : la revue annoncait ~16 100/jour pour « 14 en rafale + 60 s de silence »
  // (pardon suppose TOTAL). Le pardon est INCREMENTAL : mesure = ~6 368/jour, MOINS que
  // l'attaque simple. L'optimum de l'attaquant est de ne jamais se taire.
  const double simple = static_cast<double>(simulate(0u, 7 * kDay, 1000, 0, 0)) / 7.0;
  const double r60 = static_cast<double>(simulate(0u, 7 * kDay, 1000, 14, 60000)) / 7.0;
  char m[96];
  snprintf(m, sizeof m, "simple=%.0f r60=%.0f", simple, r60);
  TEST_ASSERT_TRUE_MESSAGE(r60 < simple, m);
  TEST_ASSERT_TRUE_MESSAGE(r60 >= 6300.0 && r60 <= 6450.0, m);  // 6 368 releves
  // Balayage : aucune combinaison rafale/silence ne bat l'attaque simple.
  const int bursts[] = {5, 14, 30, 100, 400};
  const uint32_t silences[] = {10000u, 60000u, 300000u, 1200000u};
  for (int b : bursts) {
    for (uint32_t sil : silences) {
      const double d = static_cast<double>(simulate(0u, 7 * kDay, 1000, b, sil)) / 7.0;
      char mm[96];
      snprintf(mm, sizeof mm, "rafale=%d silence=%u ms : %.0f/jour contre simple=%.0f", b, (unsigned)sil, d, simple);
      TEST_ASSERT_TRUE_MESSAGE(d <= simple + 1.0, mm);
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(r60 < 16100.0 / 2, m);                  // l'affirmation de la revue est fausse
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_ip_franchise_puis_montee_doublee_jusqu_au_plafond);
  RUN_TEST(test_franchise_ip_trois_essais_gratuits);
  RUN_TEST(test_franchise_globale_dix_essais_toutes_adresses_confondues);
  RUN_TEST(test_decroissance_incrementale_un_echec_par_periode);
  RUN_TEST(test_une_longue_pause_rend_la_franchise);
  RUN_TEST(test_un_echec_pendant_le_pardon_part_de_l_effectif_pas_du_brut);
  RUN_TEST(test_plafond_dur_jamais_franchi_pour_tout_nombre_d_echecs);
  RUN_TEST(test_saturation_du_compteur_a_255);
  RUN_TEST(test_repliement_millis_etat_pres_de_0xFFFFFF00);
  RUN_TEST(test_etat_ancien_ne_bloque_jamais_au_dela_du_plafond);
  RUN_TEST(test_attente_independante_de_la_position_de_l_horloge);
  RUN_TEST(test_un_refus_ne_modifie_aucun_etat);
  RUN_TEST(test_un_succes_ne_remet_pas_la_porte_globale_a_zero);
  RUN_TEST(test_un_succes_libere_le_creneau_de_l_adresse);
  RUN_TEST(test_une_preuve_remet_a_zero_la_propre_porte_de_l_exemptee);
  RUN_TEST(test_retry_after_est_le_plus_grand_des_delais_applicables);
  RUN_TEST(test_retry_after_secondes_arrondi_au_dessus);
  RUN_TEST(test_table_d_exemption_pleine_porte_globale_seule_sans_eviction_des_creneaux_ip);
  RUN_TEST(test_condition9_creneaux_ip_satures_l_exemptee_reste_plafonnee_a_60s);
  RUN_TEST(test_l_exemptee_echappe_a_la_globale);
  RUN_TEST(test_eviction_de_la_plus_ancienne_pas_de_la_plus_recente);
  RUN_TEST(test_eviction_correcte_au_repliement_du_compteur_seq);
  RUN_TEST(test_reinscription_d_une_exemptee_la_rajeunit);
  RUN_TEST(test_table_jamais_au_dela_de_quatre_exemptees);
  RUN_TEST(test_condition10_tout_kAllow_de_change_password_vide_puis_reinscrit_l_auteur);
  RUN_TEST(test_condition10_ip_zero_jamais_exemptee);
  RUN_TEST(test_A2_portes_ip_et_globale_au_plafond_le_jeton_valide_passe);
  RUN_TEST(test_A2_forme_reelle_de_l_interface_jeton_ET_mot_de_passe_actuel_passe);
  RUN_TEST(test_A2_jeton_valide_et_mauvais_mot_de_passe_actuel_est_compte_pas_freine);
  RUN_TEST(test_A2_route_protegee_n_est_jamais_freinee_par_les_portes);
  RUN_TEST(test_A2_un_jeton_invalide_n_est_ni_compte_ni_freine);
  RUN_TEST(test_condition4_les_deux_natures_d_echec_de_login_sont_comptees_a_l_identique);
  RUN_TEST(test_condition4_ne_comptent_pas_les_403_les_400_et_l_etat_vierge);
  RUN_TEST(test_condition4_kDenyBadCurrent_hors_etat_vierge_compte);
  RUN_TEST(test_prise_de_possession_sur_appareil_vierge_jamais_affamee);
  RUN_TEST(test_debit_porte_globale_attaque_simple_8640_par_jour);
  RUN_TEST(test_debit_porte_ip_une_seule_adresse_1440_par_jour);
  RUN_TEST(test_debit_rafale_puis_silence_ne_bat_pas_l_attaque_simple);
  return UNITY_END();
}
