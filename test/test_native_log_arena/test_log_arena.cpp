// =============================================================================
// Tests unitaires natifs — log_arena_logic (feature-129, phase B)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / LittleFS.
//
// PIÈCE CENTRALE : logArenaPlan() porte la condition bloquante C4 de
// pool-chemistry (« pas d'enjambement » : un message de journal n'est JAMAIS
// scindé par la frontière de fin d'arène). embedded-developer a tranché entre
// deux modèles plausibles (comptabilité par octets vs géométrie de positions)
// pour une signature sous-spécifiée par l'architecte — cette suite vérifie
// que le modèle GÉOMÉTRIQUE retenu (room mesurée depuis HEAD jusqu'à tail,
// `used` levant l'ambiguïté anneau-vide/anneau-plein) satisfait bien
// l'exigence : aucun message coupé, saut de queue COMPTÉ dans bytesNeeded,
// gaspillage borné par kLogMessageMaxBytes.
//
// kLogMessageMaxBytes est repris depuis constants.h (source de vérité), pas
// recopié en dur.
// =============================================================================

#include <unity.h>
#include <string.h>

#include "constants.h"       // kLogMessageMaxBytes, kLogTruncTail
#include "log_arena_logic.h"
#include "utf8_clamp.h"

void setUp(void) {}
void tearDown(void) {}

// =============================================================================
// logArenaPlan — géométrie de l'arène circulaire
// =============================================================================

// --- Cas nominal : place large, pas de wrap, anneau non vide -----------------
void test_plan_nominal_pas_de_wrap(void) {
  // cap=1000, head=200 (ecriture), tail=800 (plus ancien vivant), used>0,
  // len=50 : largement dans la place contigue en fin d'arene (1000-200=800).
  LogArenaPlan plan = logArenaPlan(1000, 200, 800, /*used=*/500, /*len=*/50);
  TEST_ASSERT_FALSE(plan.wrapped);
  TEST_ASSERT_EQUAL_UINT16(200, plan.writeOff);
  TEST_ASSERT_EQUAL_UINT16(0, plan.tailDrop);
  // room = (tail + cap - head) % cap = (800+1000-200)%1000 = 600 >= 50+0.
  // (Hors saut, head == writeOff : les deux formules coïncident. C'est
  // seulement après un saut qu'elles divergent — cf. le post-mortem de
  // test_native_log_arena_integration.)
  TEST_ASSERT_EQUAL_UINT16(0, plan.evictBytes);
}

// --- MUTANT 11 (embedded-developer) : permutation head/tail dans le corps ---
// Ce test est le kill direct : head != tail, la valeur de writeOff EN
// NON-WRAP doit être `head`. Si le corps de logArenaPlan confond head et tail
// (permutation), writeOff vaut `tail` (800) au lieu de `head` (200).
void test_plan_writeoff_egale_head_jamais_tail(void) {
  LogArenaPlan plan = logArenaPlan(1000, 200, 800, /*used=*/500, /*len=*/50);
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(
      200, plan.writeOff,
      "writeOff doit valoir head (200), pas tail (800) : mutant permutation "
      "head/tail (mutant #11)");
}

// --- Anneau VIDE (used == 0) : toute la capacite est disponible --------------
void test_plan_anneau_vide_room_egale_cap(void) {
  // used==0 leve l'ambiguite writeOff==tail : ici head==tail==0 (etat initial).
  LogArenaPlan plan = logArenaPlan(1000, 0, 0, /*used=*/0, /*len=*/100);
  TEST_ASSERT_FALSE(plan.wrapped);
  TEST_ASSERT_EQUAL_UINT16(0, plan.writeOff);
  TEST_ASSERT_EQUAL_UINT16(0, plan.evictBytes);  // room = cap = 1000, largement suffisant
}

// --- Anneau PLEIN (used > 0, writeOff == tail) : aucune place libre ----------
void test_plan_anneau_plein_writeoff_egale_tail(void) {
  // head==tail==500, used>0 (anneau plein) : room doit valoir 0 (pas `cap`),
  // c'est le SEUL role de `used` (lever l'ambiguite). Tout len>0 doit donc
  // exiger une eviction complete.
  LogArenaPlan plan = logArenaPlan(1000, 500, 500, /*used=*/1000, /*len=*/10);
  TEST_ASSERT_FALSE(plan.wrapped);  // 1000-500=500 >= 10, pas de wrap
  TEST_ASSERT_EQUAL_UINT16(10, plan.evictBytes);  // room=0, need=10, evict=10
}

// =============================================================================
// C4 — politique « pas d'enjambement » : le coeur de la condition bloquante
// =============================================================================

// --- MUTANT 1 : >= au lieu de > sur le test d'espace libre (off-by-one) -----
// len == contiguousFreeAtEnd EXACTEMENT (le message tient pile dans la place
// restante) : NE DOIT PAS wrapper. `>= ` au lieu de `>` forcerait un wrap
// inutile ici (perte de contiguite ET de capacite sans raison).
void test_plan_len_egale_place_restante_pas_de_wrap(void) {
  // cap=1000, head=900 -> contiguousFreeAtEnd = 100. len == 100 pile.
  LogArenaPlan plan = logArenaPlan(1000, 900, 50, /*used=*/500, /*len=*/100);
  TEST_ASSERT_FALSE_MESSAGE(
      plan.wrapped,
      "len == place restante exactement : ne doit PAS wrapper (mutant >= au "
      "lieu de > sur le test d'espace libre, mutant #1)");
  TEST_ASSERT_EQUAL_UINT16(900, plan.writeOff);
  TEST_ASSERT_EQUAL_UINT16(0, plan.tailDrop);
}

// --- MUTANT 2 : ecriture a cheval au lieu du saut C4 ------------------------
// len == contiguousFreeAtEnd + 1 (un seul octet de trop) : DOIT wrapper,
// jamais ecrire a cheval sur la frontiere.
void test_plan_len_un_octet_de_trop_wrap_obligatoire(void) {
  LogArenaPlan plan = logArenaPlan(1000, 900, 50, /*used=*/500, /*len=*/101);
  TEST_ASSERT_TRUE_MESSAGE(
      plan.wrapped,
      "len > place restante : DOIT wrapper, jamais ecrire a cheval (C4, "
      "mutant #2)");
  TEST_ASSERT_EQUAL_UINT16(0, plan.writeOff);      // repart a l'offset 0
  TEST_ASSERT_EQUAL_UINT16(100, plan.tailDrop);    // les 100 o de fin abandonnes
}

// --- Gaspillage borne par kLogMessageMaxBytes (C4, tailDrop < len) ----------
void test_plan_gaspillage_toujours_borne_par_len(void) {
  // Balayage de plusieurs geometries : des que wrapped, tailDrop DOIT etre
  // strictement < len (sinon la place restante suffisait, contradiction).
  const uint16_t cap = 2000;
  const uint16_t lens[] = {1, 50, 300, 767, 768};
  for (uint16_t head = 1900; head < 2000; head += 17) {
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); ++i) {
      LogArenaPlan plan = logArenaPlan(cap, head, /*tail=*/100, /*used=*/500, lens[i]);
      if (plan.wrapped) {
        TEST_ASSERT_TRUE_MESSAGE(
            plan.tailDrop < lens[i],
            "tailDrop doit toujours etre < len quand on wrap (C4) : le "
            "gaspillage est borne par la taille du message, jamais davantage");
        TEST_ASSERT_TRUE(plan.tailDrop <= kLogMessageMaxBytes);
      }
    }
  }
}

// --- MUTANT 4 : saut de queue C4 NON compte dans bytesNeeded -----------------
// Scenario : cap=100, head=90, tail=5, used>0, len=20.
// contiguousFreeAtEnd = cap-head = 10 < 20 => wrap, tailDrop=10, writeOff=0.
//
// L'espace REELLEMENT libre se mesure de `head` a `tail` dans le sens de
// l'ecriture : 10 octets en queue [90,100) + 5 octets au debut [0,5) = 15.
// On abandonne les 10 de queue et on ecrit 20 a partir de 0, donc le budget
// consomme est len+tailDrop = 30 et il faut liberer 30-15 = 15 octets.
//
// ⚠️ CE TEST A ATTENDU 25 JUSQU'AU 16/09/2026, ET IL AVAIT TORT.
// La valeur 25 venait de room=(tail+cap-writeOff)%cap=5, c'est-a-dire de la
// formule fautive elle-meme : le test avait ete ecrit d'apres le commentaire
// de l'implementation, pas d'apres l'invariant. Il confirmait donc le defaut
// au lieu de le reveler, et il est reste vert pendant que l'arene recouvrait
// des messages vivants. C'est la suite d'integration a modele de reference
// (test_native_log_arena_integration) qui l'a trouve — un test unitaire ne
// pouvait pas : le defaut n'apparait qu'apres un etat accumule.
//
// Ne PAS deriver une valeur attendue de ce que le code calcule. La derivation
// ci-dessus part de la geometrie de l'anneau, et d'elle seule.
void test_plan_saut_de_queue_compte_dans_evictbytes(void) {
  LogArenaPlan plan = logArenaPlan(100, 90, 5, /*used=*/50, /*len=*/20);
  TEST_ASSERT_TRUE(plan.wrapped);
  TEST_ASSERT_EQUAL_UINT16(10, plan.tailDrop);
  // Tue toujours le mutant #4 : sans tailDrop dans le budget, need=20 et
  // evictBytes tomberait a 20-15 = 5 au lieu de 15.
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(
      15, plan.evictBytes,
      "evictBytes doit compter tailDrop (10) EN PLUS de len (20) dans le "
      "budget a evincer : 20+10-room(15)=15, pas 20-15=5 (mutant #4, "
      "recouvrement silencieux)");
}

// --- Garde-fou : cap == 0 -----------------------------------------------------
void test_plan_cap_zero_ne_plante_pas(void) {
  LogArenaPlan plan = logArenaPlan(0, 0, 0, 0, 10);
  TEST_ASSERT_FALSE(plan.wrapped);
  TEST_ASSERT_EQUAL_UINT16(0, plan.evictBytes);
}

// =============================================================================
// logRingEvictCount — MUTANT 3 : eviction d'une entree de trop / de moins
// =============================================================================

void test_evict_count_pile_a_la_frontiere_exacte(void) {
  // 4 creneaux de longueurs 10,20,30,40. bytesNeeded == 10+20 == 30 pile :
  // doit evincer EXACTEMENT 2 creneaux (pas 1, pas 3).
  LogSlot slots[4] = {};
  slots[0].len = 10;
  slots[1].len = 20;
  slots[2].len = 30;
  slots[3].len = 40;
  size_t n = logRingEvictCount(slots, 4, /*oldest=*/0, /*bytesNeeded=*/30);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(
      2, n,
      "bytesNeeded == somme EXACTE des 2 premiers creneaux : doit evincer "
      "2, pas 1 (evince trop peu) ni 3 (evince trop) -- mutant #3");
}

void test_evict_count_un_octet_de_plus_exige_un_creneau_de_plus(void) {
  LogSlot slots[4] = {};
  slots[0].len = 10;
  slots[1].len = 20;
  slots[2].len = 30;
  slots[3].len = 40;
  size_t n = logRingEvictCount(slots, 4, /*oldest=*/0, /*bytesNeeded=*/31);
  TEST_ASSERT_EQUAL_size_t(3, n);
}

void test_evict_count_respecte_l_ordre_chronologique_depuis_oldest(void) {
  // oldest=2 (creneau le plus ancien est l'index 2), parcours circulaire :
  // ordre attendu 2,3,0,1 -> longueurs 30,40,10,20. bytesNeeded=71 doit
  // evincer 3 creneaux (30+40+10=80 >= 71), pas se baser sur l'ordre des
  // index bruts du tableau.
  LogSlot slots[4] = {};
  slots[0].len = 10;
  slots[1].len = 20;
  slots[2].len = 30;
  slots[3].len = 40;
  size_t n = logRingEvictCount(slots, 4, /*oldest=*/2, /*bytesNeeded=*/71);
  TEST_ASSERT_EQUAL_size_t(3, n);
}

void test_evict_count_bytesneeded_zero_evince_rien(void) {
  LogSlot slots[2] = {};
  slots[0].len = 10;
  slots[1].len = 20;
  TEST_ASSERT_EQUAL_size_t(0, logRingEvictCount(slots, 2, 0, 0));
}

void test_evict_count_bytesneeded_depasse_capacite_totale_best_effort(void) {
  // bytesNeeded > somme totale des creneaux : parcourt TOUT le tableau (n),
  // sans boucler indefiniment (best-effort borne).
  LogSlot slots[3] = {};
  slots[0].len = 10;
  slots[1].len = 10;
  slots[2].len = 10;
  size_t n = logRingEvictCount(slots, 3, 0, /*bytesNeeded=*/1000);
  TEST_ASSERT_EQUAL_size_t(3, n);
}

// =============================================================================
// logTruncatePlan — MUTANTS 5, 6, 7 : marqueur AVANT le clamp UTF-8
// =============================================================================

void test_truncate_plan_len_inferieur_a_maxbytes_no_op(void) {
  bool truncated = true;
  size_t keep = logTruncatePlan("court", 5, 100, truncated);
  TEST_ASSERT_EQUAL_size_t(5, keep);
  TEST_ASSERT_FALSE(truncated);
}

void test_truncate_plan_len_egale_maxbytes_no_op(void) {
  bool truncated = true;
  size_t keep = logTruncatePlan("12345", 5, 5, truncated);
  TEST_ASSERT_EQUAL_size_t(5, keep);
  TEST_ASSERT_FALSE(truncated);
}

// --- MUTANT 7 : troncature SANS marqueur (drapeau `truncated` faux) --------
void test_truncate_plan_len_superieur_leve_le_drapeau_truncated(void) {
  char src[300];
  memset(src, 'a', sizeof(src));
  bool truncated = false;
  logTruncatePlan(src, sizeof(src), 100, truncated);
  TEST_ASSERT_TRUE_MESSAGE(
      truncated,
      "len > maxBytes DOIT lever `truncated` : sans lui, un message coupe se "
      "lit comme complet (mutant #7, securite)");
}

// --- MUTANT 5 : marqueur reserve APRES le clamp au lieu d'AVANT -------------
// Invariant central de C2 : keep + sizeof(kLogTruncTail) <= maxBytes, TOUJOURS
// -- c'est ce qui garantit que "src[0..keep) + kLogTruncTail" tient dans le
// budget. Construit avec de l'ASCII pur (les deux ordres coincident sur de
// l'ASCII pur : le vrai discriminant est le test UTF-8 ci-dessous), mais
// l'invariant numerique doit deja tenir ici.
void test_truncate_plan_reserve_toujours_respectee_ascii(void) {
  char src[500];
  memset(src, 'x', sizeof(src));
  const size_t maxBytes = 200;
  bool truncated = false;
  size_t keep = logTruncatePlan(src, sizeof(src), maxBytes, truncated);
  TEST_ASSERT_TRUE(truncated);
  TEST_ASSERT_TRUE_MESSAGE(
      keep + sizeof(kLogTruncTail) <= maxBytes,
      "keep + marqueur doit tenir dans maxBytes (C2, reserve AVANT le clamp)");
  TEST_ASSERT_EQUAL_size_t(maxBytes - sizeof(kLogTruncTail), keep);
}

// --- MUTANT 5 (discriminant reel) : le clamp UTF-8 doit s'appliquer APRES la
// reservation, sur le budget DEJA reduit -- sinon un caractere multi-octets
// qui chevauche exactement la frontiere du budget reduit peut etre coupe en
// plein milieu.
void test_truncate_plan_reserve_avant_clamp_ne_coupe_pas_un_caractere(void) {
  // src : 300 'a', puis un tiret cadratin "—" 3 octets (0xE2 0x80 0x94) aux
  // positions [300,301,302), puis a nouveau des 'a' jusqu'a 400 octets (len
  // du SOURCE doit rester > maxBytes pour que la troncature se declenche).
  // maxBytes choisi pour que budget = maxBytes - sizeof(kLogTruncTail) tombe
  // A LA POSITION 301 (EN PLEIN MILIEU du tiret cadratin, qui occupe
  // [300,303)). Le clamp doit alors reculer a 300 -- retirer TOUT le
  // caractere -- jamais s'arreter a 301 (ce qui laisserait un octet de tete
  // orphelin en fin de tampon).
  char src[400];
  memset(src, 'a', sizeof(src));
  src[300] = static_cast<char>(0xE2);
  src[301] = static_cast<char>(0x80);
  src[302] = static_cast<char>(0x94);
  // budget vise = 301 -> maxBytes = 301 + sizeof(kLogTruncTail) (< len(400))
  const size_t maxBytes = 301 + sizeof(kLogTruncTail);
  bool truncated = false;
  size_t keep = logTruncatePlan(src, sizeof(src), maxBytes, truncated);
  TEST_ASSERT_TRUE(truncated);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(
      300, keep,
      "le caractere multi-octets qui chevauche la frontiere budget doit etre "
      "retire ENTIER (300), pas coupe a 301 (mutant #5 : reserve APRES le "
      "clamp au lieu d'AVANT -- laisserait un octet de tete orphelin)");
  // Auto-consistance : keep doit deja etre une frontiere UTF-8 valide.
  TEST_ASSERT_EQUAL_size_t(keep, utf8ClampLen(src, keep));
}

// --- MUTANT 6 : clamp coupant un 'e' (2o) present dans un message de garde --
void test_truncate_plan_ne_coupe_pas_un_e_accent(void) {
  // "caf" + é (0xC3 0xA9) + remplissage jusqu'a 30 o (len du SOURCE doit
  // rester > maxBytes pour que la troncature se declenche).
  char src[30];
  memset(src, 'x', sizeof(src));
  src[0] = 'c'; src[1] = 'a'; src[2] = 'f';
  src[3] = (char)0xC3; src[4] = (char)0xA9;
  bool truncated = false;
  // maxBytes - reserve = 4 (tombe en plein milieu du 'é', qui occupe [3,5)).
  const size_t maxBytes = 4 + sizeof(kLogTruncTail);
  size_t keep = logTruncatePlan(src, sizeof(src), maxBytes, truncated);
  TEST_ASSERT_TRUE(truncated);
  TEST_ASSERT_EQUAL_size_t(3, keep);  // le 'e' entier retire, on s'arrete a "caf"
}

// =============================================================================
// logFormatHeader — MUTANT 9 : wallSec == 0 traite comme un horodatage valide
// =============================================================================

void test_header_wallsec_zero_produit_le_repli(void) {
  char out[64];
  logFormatHeader(out, sizeof(out), 0, LogLevel::INFO);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(
      "\?\?\?\?-\?\?-\?\?T\?\?:\?\?:\?\? INFO ", out,
      "wallSec==0 (horloge non fixee) doit produire le repli non date, "
      "JAMAIS une ligne '1970-01-01...' qui laisserait croire a une mesure "
      "reelle (mutant #9)");
}

void test_header_wallsec_valide_produit_une_date(void) {
  char out[64];
  // 1704067200 = 2024-01-01T00:00:00Z (epoch verifie).
  logFormatHeader(out, sizeof(out), 1704067200UL, LogLevel::WARNING);
  TEST_ASSERT_EQUAL_STRING("2024-01-01T00:00:00 WARN ", out);
}

void test_header_wallsec_annee_bissextile_29_fevrier(void) {
  // 1709164800 = 2024-02-29T00:00:00Z (2024 est bissextile).
  char out[64];
  logFormatHeader(out, sizeof(out), 1709164800UL, LogLevel::CRITICAL);
  TEST_ASSERT_EQUAL_STRING("2024-02-29T00:00:00 CRIT ", out);
}

void test_header_toutes_les_etiquettes_de_niveau(void) {
  char out[64];
  logFormatHeader(out, sizeof(out), 1704067200UL, LogLevel::DEBUG);
  TEST_ASSERT_EQUAL_STRING("2024-01-01T00:00:00 DEBUG ", out);
  logFormatHeader(out, sizeof(out), 1704067200UL, LogLevel::ERROR);
  TEST_ASSERT_EQUAL_STRING("2024-01-01T00:00:00 ERROR ", out);
}

// =============================================================================
// logShouldPersist — MUTANT 8 : < -> <= ET WARNING -> INFO
// =============================================================================

// --- Regle normale (seq >= persistEnableSeq) : tout sauf DEBUG --------------
void test_should_persist_regle_normale_debug_refuse(void) {
  TEST_ASSERT_FALSE(logShouldPersist(100, 100, LogLevel::DEBUG));
}
void test_should_persist_regle_normale_info_accepte(void) {
  TEST_ASSERT_TRUE(logShouldPersist(100, 100, LogLevel::INFO));
}
void test_should_persist_regle_normale_critical_accepte(void) {
  TEST_ASSERT_TRUE(logShouldPersist(999, 100, LogLevel::CRITICAL));
}

// --- MUTANT 8a : frontiere < vs <= sur seq/persistEnableSeq -----------------
void test_should_persist_frontiere_seq_egale_enable_regle_normale(void) {
  // seq == persistEnableSeq : PAS < persistEnableSeq -> regle NORMALE
  // (pas le filtre de demarrage). INFO doit donc etre ACCEPTE ici.
  TEST_ASSERT_TRUE_MESSAGE(
      logShouldPersist(/*seq=*/5, /*persistEnableSeq=*/5, LogLevel::INFO),
      "seq == persistEnableSeq : regle normale (pas de rattrapage), INFO "
      "accepte -- mutant #8a (< devenu <=) le refuserait a tort");
}
void test_should_persist_frontiere_seq_juste_avant_enable_regle_demarrage(void) {
  // seq == persistEnableSeq - 1 : < persistEnableSeq est vrai -> regle de
  // RATTRAPAGE (filtre WARNING+). INFO doit etre REFUSE ici.
  TEST_ASSERT_FALSE_MESSAGE(
      logShouldPersist(/*seq=*/4, /*persistEnableSeq=*/5, LogLevel::INFO),
      "seq < persistEnableSeq : regle de rattrapage (C7), INFO refuse");
}

// --- MUTANT 8b : seuil WARNING -> INFO sur le filtre de rattrapage C7 ------
void test_should_persist_rattrapage_debug_refuse(void) {
  TEST_ASSERT_FALSE(logShouldPersist(0, 100, LogLevel::DEBUG));
}
void test_should_persist_rattrapage_info_refuse(void) {
  TEST_ASSERT_FALSE_MESSAGE(
      logShouldPersist(0, 100, LogLevel::INFO),
      "C7 : au demarrage (seq < persistEnableSeq), INFO doit etre REFUSE -- "
      "sinon un boot verse plus de kMaxLogFileBytes d'INFO et rotationne "
      "immediatement, emportant les lignes de securite (mutant #8b)");
}
void test_should_persist_rattrapage_warning_accepte(void) {
  TEST_ASSERT_TRUE(logShouldPersist(0, 100, LogLevel::WARNING));
}
void test_should_persist_rattrapage_error_accepte(void) {
  TEST_ASSERT_TRUE(logShouldPersist(0, 100, LogLevel::ERROR));
}
void test_should_persist_rattrapage_critical_accepte(void) {
  TEST_ASSERT_TRUE(logShouldPersist(0, 100, LogLevel::CRITICAL));
}

// --- MUTANT 8b, preuve par le BUDGET (C7, le sens meme de la condition) ----
// Simule un flot de demarrage : `bootLines` entrees INFO emises AVANT que la
// persistance ne soit activee (persistEnableSeq = bootLines). Avec le filtre
// CORRECT (WARNING+), AUCUNE de ces lignes n'est admise au premier flush :
// l'octet compte reste a 0, tres loin sous kMaxLogFileBytes. Si le seuil
// glisse a INFO (mutant #8b), les ~100 o/ligne * 200 lignes = ~20 Ko
// franchissent kMaxLogFileBytes (8192) largement AVANT la fin du rattrapage
// -- exactement le scenario C7 que la condition existe pour empecher.
void test_should_persist_budget_demarrage_reste_sous_kmaxlogfilebytes(void) {
  // Valeur reprise de logger.h:46 (Logger::kMaxLogFileBytes = 8192) --
  // logger.h n'est pas inclus en natif (deps Arduino), donc recopiee ici
  // avec sa source citee, comme demande par la consigne de la suite.
  const size_t kMaxLogFileBytesRef = 8192;
  const uint32_t bootLines = 200;
  const size_t avgLineBytes = 100;  // gabarit realiste d'une ligne horodatee

  size_t admittedBytes = 0;
  for (uint32_t seq = 0; seq < bootLines; ++seq) {
    if (logShouldPersist(seq, /*persistEnableSeq=*/bootLines, LogLevel::INFO)) {
      admittedBytes += avgLineBytes;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(
      admittedBytes < kMaxLogFileBytesRef,
      "un flot de 200 lignes INFO de demarrage ne doit JAMAIS depasser "
      "kMaxLogFileBytes (8192) au premier flush -- sinon la rotation "
      "emporterait les lignes de securite qu'on vient d'ecrire (C7)");
  // Avec le filtre correct, c'est meme strictement zero (INFO < WARNING).
  TEST_ASSERT_EQUAL_size_t(0, admittedBytes);
}

// =============================================================================
// logNextFlushSlot — MUTANT #10 (phase B, différé faute de câblage) / #12
// (phase D, repris maintenant que logger.cpp l'appelle réellement)
// =============================================================================
// Cette fonction n'avait AUCUN test en phase B (le corps de logger.cpp qui la
// pilote — curseur `_flushCursor`, avancement `seq + 1` — n'existait pas
// encore). Elle existe maintenant (logger.cpp:591-616) : ce bloc couvre le
// contrat exact documenté par log_arena_logic.h — comparaison `>=`, jamais
// `>`, et un balayage borné à `liveCount` créneaux depuis `oldest`.

void test_next_flush_slot_rien_a_flusher_curseur_rattrape(void) {
  LogSlot slots[3] = {};
  slots[0].seq = 10;
  slots[1].seq = 11;
  slots[2].seq = 12;
  size_t idx = logNextFlushSlot(slots, 3, /*oldest=*/0, /*liveCount=*/3, /*flushCursor=*/13);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(3, idx, "curseur au-delà du plus récent : n (rien à flusher)");
}

void test_next_flush_slot_trouve_le_premier_seq_strictement_superieur(void) {
  LogSlot slots[3] = {};
  slots[0].seq = 10;
  slots[1].seq = 11;
  slots[2].seq = 12;
  size_t idx = logNextFlushSlot(slots, 3, 0, 3, /*flushCursor=*/11);
  TEST_ASSERT_EQUAL_size_t(1, idx);
}

// --- MUTANT #10/#12, LE KILL DIRECT : >= devient > sur le curseur ----------
// seq == flushCursor EXACTEMENT : DOIT être candidate (>=). Avec `>` au lieu
// de `>=`, ce créneau serait ignoré ici (index 1, seq=11, non trouvé) et le
// balayage retomberait sur le suivant (index 2, seq=12) — LIGNE SAUTÉE, le
// défaut documenté en tête de logger.cpp:591-592.
void test_next_flush_slot_seq_egal_au_curseur_pas_saute(void) {
  LogSlot slots[3] = {};
  slots[0].seq = 10;
  slots[1].seq = 11;
  slots[2].seq = 12;
  size_t idx = logNextFlushSlot(slots, 3, 0, 3, /*flushCursor=*/11);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(
      1, idx,
      "seq == flushCursor doit etre candidate (>=) : mutant #10/#12 (> au "
      "lieu de >=) sauterait cette ligne et retomberait sur la suivante");
}

// --- Créneau unique, cas limite (n=1) ---------------------------------------
void test_next_flush_slot_un_seul_creneau_egal_au_curseur(void) {
  LogSlot slots[1] = {};
  slots[0].seq = 42;
  size_t idx = logNextFlushSlot(slots, 1, 0, 1, /*flushCursor=*/42);
  TEST_ASSERT_EQUAL_size_t(0, idx);
}

// --- L'ÉVICTION PASSE DEVANT LE CURSEUR (scénario log_persist_overrun) -----
// Le plus ancien créneau VIVANT porte un seq strictement supérieur au
// curseur : les créneaux [flushCursor, oldestSeq) ont été évincés avant
// d'être flushés (c'est exactement ce que compte log_persist_overrun, C6).
// logNextFlushSlot doit retomber IMMÉDIATEMENT sur le nouveau plus ancien
// (index 0 de la fenêtre vivante), sans rester bloqué à chercher un seq qui
// n'existe plus dans l'anneau.
void test_next_flush_slot_retombe_sur_le_plus_ancien_apres_eviction_devant_le_curseur(void) {
  LogSlot slots[3] = {};
  slots[0].seq = 7;  // plus ancien VIVANT — les seq 0..6 ont été évincés
  slots[1].seq = 8;
  slots[2].seq = 9;
  size_t idx = logNextFlushSlot(slots, 3, 0, 3, /*flushCursor=*/5);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(
      0, idx,
      "curseur pointant vers un seq déjà évincé : doit retomber sur le "
      "premier créneau VIVANT, pas rester coincé (scénario log_persist_overrun)");
}

// --- Ordre chronologique depuis `oldest`, indices bruts non triés ----------
void test_next_flush_slot_respecte_l_ordre_chronologique_depuis_oldest(void) {
  // oldest=2 : ordre chronologique 2,3,0,1 -> seq 18,19,20,21 (indices bruts
  // 0 et 1 portent les DEUX plus RECENTES, écrites après le tour de l'anneau).
  LogSlot slots[4] = {};
  slots[0].seq = 20;
  slots[1].seq = 21;
  slots[2].seq = 18;
  slots[3].seq = 19;
  size_t idx = logNextFlushSlot(slots, 4, /*oldest=*/2, /*liveCount=*/4, /*flushCursor=*/19);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(3, idx, "seq 19 vit à l'index brut 3, pas au 2e rang du parcours");
}

// --- `liveCount` borne strictement le balayage : pas de faux-positif -------
// slots[1] porte un seq qui satisferait la candidature, mais liveCount=1 : il
// est HORS de la fenêtre vivante (créneau jamais écrit ou déjà évincé), donc
// ne doit JAMAIS être consulté.
void test_next_flush_slot_ignore_les_creneaux_au_dela_de_livecount(void) {
  LogSlot slots[4] = {};
  slots[0].seq = 100;  // vivant, sous le curseur -> rien à flusher
  slots[1].seq = 999;  // "fantôme" au-delà de liveCount, ne doit pas être lu
  size_t idx = logNextFlushSlot(slots, 4, /*oldest=*/0, /*liveCount=*/1, /*flushCursor=*/200);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(4, idx, "slots[1] est hors fenêtre vivante : ne doit produire aucun faux-positif");
}

// =============================================================================
// logArenaWriteOverlapsLive — détecteur d'invariant qui survit en production
// (P2 pool-chemistry, feature-129, condition bloquante de clôture)
// =============================================================================
// Filet INDÉPENDANT de logArenaPlan()/logRingEvictCount() : l'incident du
// 16/09/2026 a montré qu'un défaut de géométrie dans CES fonctions peut
// laisser passer un recouvrement SANS qu'aucun des cinq compteurs de l'arène
// ne le révèle. Ces tests valident la fonction seule (recouvrement d'intervalles
// NON circulaires, C4 garantissant que ni un créneau vivant ni le plan
// d'écriture ne franchissent jamais la frontière de fin d'arène).

void test_overlap_aucun_creneau_vivant_pas_de_recouvrement(void) {
  LogSlot slots[3] = {};
  TEST_ASSERT_FALSE(logArenaWriteOverlapsLive(slots, 3, /*oldest=*/0, /*liveCount=*/0,
                                              /*writeOff=*/0, /*writeLen=*/50));
}

void test_overlap_ecriture_longueur_zero_pas_de_recouvrement(void) {
  LogSlot slots[1] = {};
  slots[0].off = 10;
  slots[0].len = 20;
  TEST_ASSERT_FALSE(logArenaWriteOverlapsLive(slots, 1, 0, 1, /*writeOff=*/10, /*writeLen=*/0));
}

void test_overlap_plages_disjointes_avant_pas_de_recouvrement(void) {
  // Créneau vivant [100, 150). Écriture [0, 50) : loin avant, aucun recouvrement.
  LogSlot slots[1] = {};
  slots[0].off = 100;
  slots[0].len = 50;
  TEST_ASSERT_FALSE(logArenaWriteOverlapsLive(slots, 1, 0, 1, /*writeOff=*/0, /*writeLen=*/50));
}

void test_overlap_plages_disjointes_apres_pas_de_recouvrement(void) {
  // Créneau vivant [0, 50). Écriture [100, 150) : loin après.
  LogSlot slots[1] = {};
  slots[0].off = 0;
  slots[0].len = 50;
  TEST_ASSERT_FALSE(logArenaWriteOverlapsLive(slots, 1, 0, 1, /*writeOff=*/100, /*writeLen=*/50));
}

// --- MUTANT plausible : `<=` au lieu de `<` sur une des deux bornes ---------
// Deux intervalles qui se TOUCHENT exactement (l'un finit pile où l'autre
// commence) ne se recouvrent PAS -- [0,50) et [50,100) sont ADJACENTS, aucun
// octet commun. Un mutant `<=` transformerait ce contact en faux recouvrement.
void test_overlap_intervalles_adjacents_ecriture_avant_pas_de_recouvrement(void) {
  LogSlot slots[1] = {};
  slots[0].off = 50;
  slots[0].len = 50;  // [50, 100)
  TEST_ASSERT_FALSE_MESSAGE(
      logArenaWriteOverlapsLive(slots, 1, 0, 1, /*writeOff=*/0, /*writeLen=*/50),
      "ecriture [0,50) et creneau [50,100) sont ADJACENTS, pas de recouvrement "
      "-- mutant <= au lieu de < sur la borne haute");
}

void test_overlap_intervalles_adjacents_ecriture_apres_pas_de_recouvrement(void) {
  LogSlot slots[1] = {};
  slots[0].off = 0;
  slots[0].len = 50;  // [0, 50)
  TEST_ASSERT_FALSE_MESSAGE(
      logArenaWriteOverlapsLive(slots, 1, 0, 1, /*writeOff=*/50, /*writeLen=*/50),
      "creneau [0,50) et ecriture [50,100) sont ADJACENTS, pas de recouvrement "
      "-- mutant <= au lieu de < sur la borne basse");
}

// --- Recouvrement franc : le scenario meme de l'incident --------------------
void test_overlap_recouvrement_partiel_detecte(void) {
  // Creneau vivant [0, 158) (seq429 dans la trace de l'incident, cap=16384).
  // Ecriture qui commence DANS ce creneau : [100, 250).
  LogSlot slots[1] = {};
  slots[0].off = 0;
  slots[0].len = 158;
  TEST_ASSERT_TRUE_MESSAGE(
      logArenaWriteOverlapsLive(slots, 1, 0, 1, /*writeOff=*/100, /*writeLen=*/150),
      "ecriture [100,250) recoupe le creneau vivant [0,158) -- doit etre detecte "
      "(scenario de l'incident du 16/09/2026)");
}

void test_overlap_recouvrement_total_detecte(void) {
  // Ecriture qui englobe entierement un creneau vivant plus petit.
  LogSlot slots[1] = {};
  slots[0].off = 100;
  slots[0].len = 10;  // [100, 110)
  TEST_ASSERT_TRUE(logArenaWriteOverlapsLive(slots, 1, 0, 1, /*writeOff=*/0, /*writeLen=*/200));
}

// --- `liveCount` borne strictement le balayage : pas de faux-positif -------
void test_overlap_ignore_les_creneaux_au_dela_de_livecount(void) {
  LogSlot slots[2] = {};
  slots[0].off = 500;
  slots[0].len = 10;  // hors fenetre vivante (liveCount=0) : NE DOIT PAS compter
  slots[1].off = 0;
  slots[1].len = 5;
  TEST_ASSERT_FALSE_MESSAGE(
      logArenaWriteOverlapsLive(slots, 2, /*oldest=*/0, /*liveCount=*/0,
                                /*writeOff=*/500, /*writeLen=*/10),
      "slots[0] est hors fenetre vivante (liveCount=0) : ne doit produire "
      "aucun faux-positif");
}

// --- Parcours circulaire depuis `oldest`, indices bruts non tries ----------
void test_overlap_respecte_l_ordre_depuis_oldest_wrap(void) {
  // oldest=2 : ordre de parcours 2,3,0,1. Le creneau qui recoupe l'ecriture
  // est a l'index BRUT 0 (le 3e visite), pas au debut du tableau.
  LogSlot slots[4] = {};
  slots[0].off = 1000;
  slots[0].len = 50;  // [1000, 1050) -- celui qui recoupe
  slots[1].off = 2000;
  slots[1].len = 10;
  slots[2].off = 0;
  slots[2].len = 10;
  slots[3].off = 20;
  slots[3].len = 10;
  TEST_ASSERT_TRUE_MESSAGE(
      logArenaWriteOverlapsLive(slots, 4, /*oldest=*/2, /*liveCount=*/4,
                                /*writeOff=*/1010, /*writeLen=*/5),
      "le creneau recoupant est a l'index brut 0 (3e du parcours circulaire "
      "depuis oldest=2) : doit etre trouve malgre l'ordre des index bruts");
}

void test_overlap_creneau_longueur_zero_ignore(void) {
  // Un creneau de longueur 0 (defensif, ne devrait pas exister en pratique)
  // ne peut recouper aucune ecriture.
  LogSlot slots[1] = {};
  slots[0].off = 100;
  slots[0].len = 0;
  TEST_ASSERT_FALSE(logArenaWriteOverlapsLive(slots, 1, 0, 1, /*writeOff=*/100, /*writeLen=*/50));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();

  RUN_TEST(test_plan_nominal_pas_de_wrap);
  RUN_TEST(test_plan_writeoff_egale_head_jamais_tail);
  RUN_TEST(test_plan_anneau_vide_room_egale_cap);
  RUN_TEST(test_plan_anneau_plein_writeoff_egale_tail);
  RUN_TEST(test_plan_len_egale_place_restante_pas_de_wrap);
  RUN_TEST(test_plan_len_un_octet_de_trop_wrap_obligatoire);
  RUN_TEST(test_plan_gaspillage_toujours_borne_par_len);
  RUN_TEST(test_plan_saut_de_queue_compte_dans_evictbytes);
  RUN_TEST(test_plan_cap_zero_ne_plante_pas);

  RUN_TEST(test_evict_count_pile_a_la_frontiere_exacte);
  RUN_TEST(test_evict_count_un_octet_de_plus_exige_un_creneau_de_plus);
  RUN_TEST(test_evict_count_respecte_l_ordre_chronologique_depuis_oldest);
  RUN_TEST(test_evict_count_bytesneeded_zero_evince_rien);
  RUN_TEST(test_evict_count_bytesneeded_depasse_capacite_totale_best_effort);

  RUN_TEST(test_truncate_plan_len_inferieur_a_maxbytes_no_op);
  RUN_TEST(test_truncate_plan_len_egale_maxbytes_no_op);
  RUN_TEST(test_truncate_plan_len_superieur_leve_le_drapeau_truncated);
  RUN_TEST(test_truncate_plan_reserve_toujours_respectee_ascii);
  RUN_TEST(test_truncate_plan_reserve_avant_clamp_ne_coupe_pas_un_caractere);
  RUN_TEST(test_truncate_plan_ne_coupe_pas_un_e_accent);

  RUN_TEST(test_header_wallsec_zero_produit_le_repli);
  RUN_TEST(test_header_wallsec_valide_produit_une_date);
  RUN_TEST(test_header_wallsec_annee_bissextile_29_fevrier);
  RUN_TEST(test_header_toutes_les_etiquettes_de_niveau);

  RUN_TEST(test_should_persist_regle_normale_debug_refuse);
  RUN_TEST(test_should_persist_regle_normale_info_accepte);
  RUN_TEST(test_should_persist_regle_normale_critical_accepte);
  RUN_TEST(test_should_persist_frontiere_seq_egale_enable_regle_normale);
  RUN_TEST(test_should_persist_frontiere_seq_juste_avant_enable_regle_demarrage);
  RUN_TEST(test_should_persist_rattrapage_debug_refuse);
  RUN_TEST(test_should_persist_rattrapage_info_refuse);
  RUN_TEST(test_should_persist_rattrapage_warning_accepte);
  RUN_TEST(test_should_persist_rattrapage_error_accepte);
  RUN_TEST(test_should_persist_rattrapage_critical_accepte);
  RUN_TEST(test_should_persist_budget_demarrage_reste_sous_kmaxlogfilebytes);

  RUN_TEST(test_next_flush_slot_rien_a_flusher_curseur_rattrape);
  RUN_TEST(test_next_flush_slot_trouve_le_premier_seq_strictement_superieur);
  RUN_TEST(test_next_flush_slot_seq_egal_au_curseur_pas_saute);
  RUN_TEST(test_next_flush_slot_un_seul_creneau_egal_au_curseur);
  RUN_TEST(test_next_flush_slot_retombe_sur_le_plus_ancien_apres_eviction_devant_le_curseur);
  RUN_TEST(test_next_flush_slot_respecte_l_ordre_chronologique_depuis_oldest);
  RUN_TEST(test_next_flush_slot_ignore_les_creneaux_au_dela_de_livecount);

  RUN_TEST(test_overlap_aucun_creneau_vivant_pas_de_recouvrement);
  RUN_TEST(test_overlap_ecriture_longueur_zero_pas_de_recouvrement);
  RUN_TEST(test_overlap_plages_disjointes_avant_pas_de_recouvrement);
  RUN_TEST(test_overlap_plages_disjointes_apres_pas_de_recouvrement);
  RUN_TEST(test_overlap_intervalles_adjacents_ecriture_avant_pas_de_recouvrement);
  RUN_TEST(test_overlap_intervalles_adjacents_ecriture_apres_pas_de_recouvrement);
  RUN_TEST(test_overlap_recouvrement_partiel_detecte);
  RUN_TEST(test_overlap_recouvrement_total_detecte);
  RUN_TEST(test_overlap_ignore_les_creneaux_au_dela_de_livecount);
  RUN_TEST(test_overlap_respecte_l_ordre_depuis_oldest_wrap);
  RUN_TEST(test_overlap_creneau_longueur_zero_ignore);

  return UNITY_END();
}
