// =============================================================================
// Tests unitaires natifs — Empreinte + décision de republication de la
// découverte HA (discovery_fingerprint_logic, bug-121, ADR-0017)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / MQTT / NVS / systemLogger.
// On teste le COMPORTEMENT observable du header pur
// src/discovery_fingerprint_logic.h : discoveryDecide() (le SEUL arbitrage qui
// décide si l'appareil republie ~60 entités retenues côté courtier), le FNV-1a
// qui produit l'empreinte, et discoveryRecordDiffers() (la garde d'écriture
// NVS).
//
// ENJEU CENTRAL : le cas NOMINAL est `DiscoveryReason::None` — empreinte
// inchangée, AUCUNE republication, y compris sur des reconnexions en rafale
// (le relevé du 08/09 montrait 4 rafales en 65 s). Un test qui ne vérifie que
// les cas où l'on republie ne prouve rien sur le défaut réel : le silence est
// le cas qui compte.
//
// ⚠️ `DiscoveryReason::Recovery` et le paramètre `forced` n'ont AUCUN appelant
// firmware (bouton HTTP retiré du périmètre de bug-121). Ils sont testés ICI,
// EN ISOLATION, jamais via un scénario d'intégration mqtt_manager — la
// fonction doit rester testable dans sa forme complète (voir le docstring de
// discoveryDecide()), mais aucun test ne doit laisser croire qu'un chemin de
// récupération forcée existe côté firmware aujourd'hui.
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include <string.h>
#include "discovery_fingerprint_logic.h"

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Outils de test
// ---------------------------------------------------------------------------

static DiscoveryRecord makeRecord(uint32_t fp, uint32_t entityHash, uint16_t entityCount,
                                   uint8_t complete) {
  DiscoveryRecord r;
  r.fingerprint = fp;
  r.entityHash = entityHash;
  r.entityCount = entityCount;
  r.complete = complete;
  return r;
}

// Reproduit EXACTEMENT la chaîne de hachage de computeDiscoveryFingerprint()
// (mqtt_manager.cpp, impur — Preferences/configMutex non disponibles en
// native) : version, préfixe HA, id d'appareil, topic de base, serveur, port,
// dans cet ordre. Toute dérive de cet ordre côté firmware sans mise à jour
// symétrique ici serait elle-même un signal (les tests ci-dessous figeraient
// une réalité différente de la production) — c'est un choix assumé plutôt que
// de dupliquer configMutex/Preferences dans le harnais natif.
static uint32_t computeFingerprint(const char* version, const char* haPrefix,
                                    const char* deviceId, const char* baseTopic,
                                    const char* server, uint32_t port) {
  uint32_t h = kDiscoveryFnvOffsetBasis;
  h = discoveryFnv1aStr(h, version);
  h = discoveryFnv1aStr(h, haPrefix);
  h = discoveryFnv1aStr(h, deviceId);
  h = discoveryFnv1aStr(h, baseTopic);
  h = discoveryFnv1aStr(h, server);
  h = discoveryFnv1aU32(h, port);
  return h;
}

// Configuration de référence, réutilisée par tous les tests d'empreinte.
struct Cfg {
  const char* version;
  const char* haPrefix;
  const char* deviceId;
  const char* baseTopic;
  const char* server;
  uint32_t port;
};

static const Cfg kBaseline = {"2.53.1", "homeassistant", "pool_controller",
                               "piscine/pool_controller", "192.168.1.50", 1883};

static uint32_t fpOf(const Cfg& c) {
  return computeFingerprint(c.version, c.haPrefix, c.deviceId, c.baseTopic, c.server, c.port);
}

// ---------------------------------------------------------------------------
// 1. FNV-1a — vecteurs connus, indépendance à nullptr, cohérence U32/octets
// ---------------------------------------------------------------------------

// Vecteurs FNV-1a 32 bits standard (offset basis 2166136261, prime 16777619),
// recalculés indépendamment en Python (implémentation manuelle octet par
// octet) pour ce rapport — jamais recopiés d'une table externe non vérifiée.
void test_fnv1a_known_vectors(void) {
  TEST_ASSERT_EQUAL_UINT32(2166136261u,
                            discoveryFnv1aStr(kDiscoveryFnvOffsetBasis, ""));
  TEST_ASSERT_EQUAL_UINT32(3826002220u,
                            discoveryFnv1aStr(kDiscoveryFnvOffsetBasis, "a"));
  TEST_ASSERT_EQUAL_UINT32(3214735720u,
                            discoveryFnv1aStr(kDiscoveryFnvOffsetBasis, "foobar"));
}

// nullptr toléré : no-op, l'accumulateur ressort inchangé (docstring explicite).
void test_fnv1a_str_nullptr_is_noop(void) {
  const uint32_t h = discoveryFnv1aStr(0xDEADBEEFu, nullptr);
  TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, h);
  // Depuis l'offset de départ standard aussi.
  TEST_ASSERT_EQUAL_UINT32(kDiscoveryFnvOffsetBasis,
                            discoveryFnv1aStr(kDiscoveryFnvOffsetBasis, nullptr));
}

// discoveryFnv1aU32 doit produire EXACTEMENT le même résultat que l'enchaînement
// manuel de ses 4 octets (LSB d'abord) — c'est ce qui rend le hachage
// indépendant de l'endianness de la plateforme d'exécution (ESP32 vs harnais
// natif x86/ARM), condition nécessaire pour qu'une empreinte enregistrée par
// l'appareil et relue par lui reste stable.
void test_fnv1a_u32_matches_manual_lsb_first_byte_chain(void) {
  const uint32_t v = 0x12345678u;
  const uint32_t viaU32 = discoveryFnv1aU32(kDiscoveryFnvOffsetBasis, v);

  uint32_t manual = kDiscoveryFnvOffsetBasis;
  manual = discoveryFnv1aByte(manual, (uint8_t)(v & 0xFFu));
  manual = discoveryFnv1aByte(manual, (uint8_t)((v >> 8) & 0xFFu));
  manual = discoveryFnv1aByte(manual, (uint8_t)((v >> 16) & 0xFFu));
  manual = discoveryFnv1aByte(manual, (uint8_t)((v >> 24) & 0xFFu));

  TEST_ASSERT_EQUAL_UINT32(manual, viaU32);
  TEST_ASSERT_EQUAL_UINT32(2741278597u, viaU32);  // valeur figée, recalculée indépendamment
}

// Deux entiers voisins (ports adjacents typiques : 1883/1884, 8883/8884) ne
// doivent PAS collisionner — ce sont exactement les valeurs qui comptent en
// pratique (MQTT clair vs MQTT TLS, ou reconfiguration d'un port voisin).
void test_fnv1a_u32_no_collision_on_adjacent_ports(void) {
  const uint32_t h1883 = discoveryFnv1aU32(kDiscoveryFnvOffsetBasis, 1883);
  const uint32_t h1884 = discoveryFnv1aU32(kDiscoveryFnvOffsetBasis, 1884);
  const uint32_t h8883 = discoveryFnv1aU32(kDiscoveryFnvOffsetBasis, 8883);
  const uint32_t h8884 = discoveryFnv1aU32(kDiscoveryFnvOffsetBasis, 8884);
  TEST_ASSERT_NOT_EQUAL(h1883, h1884);
  TEST_ASSERT_NOT_EQUAL(h8883, h8884);
  TEST_ASSERT_NOT_EQUAL(h1883, h8883);
  TEST_ASSERT_NOT_EQUAL(h1883, h8884);
  TEST_ASSERT_NOT_EQUAL(h1884, h8883);
}

// Deux chaînes voisines (versions patch adjacentes) ne collisionnent pas non
// plus — c'est le cas « mise à jour de firmware » qui doit déclencher A2.
void test_fnv1a_str_no_collision_on_adjacent_versions(void) {
  const uint32_t v1 = discoveryFnv1aStr(kDiscoveryFnvOffsetBasis, "2.53.0");
  const uint32_t v2 = discoveryFnv1aStr(kDiscoveryFnvOffsetBasis, "2.53.1");
  const uint32_t v3 = discoveryFnv1aStr(kDiscoveryFnvOffsetBasis, "2.53.10");
  TEST_ASSERT_NOT_EQUAL(v1, v2);
  TEST_ASSERT_NOT_EQUAL(v2, v3);
  TEST_ASSERT_NOT_EQUAL(v1, v3);
}

// ---------------------------------------------------------------------------
// 2. Empreinte de configuration — stable, sensible à CHAQUE champ contributif
// ---------------------------------------------------------------------------

// Même configuration → même empreinte, à chaque appel (déterminisme, condition
// nécessaire pour comparer une empreinte calculée à une empreinte persistée).
void test_fingerprint_is_stable_on_identical_inputs(void) {
  const uint32_t a = fpOf(kBaseline);
  const uint32_t b = fpOf(kBaseline);
  TEST_ASSERT_EQUAL_UINT32(a, b);
  TEST_ASSERT_EQUAL_UINT32(2912970238u, a);  // valeur de référence, recalculée indépendamment
}

// Version de firmware changée (le cas qui a fait passer 54 à 60 entités) →
// empreinte différente.
void test_fingerprint_changes_with_firmware_version(void) {
  Cfg c = kBaseline;
  c.version = "2.53.2";
  TEST_ASSERT_NOT_EQUAL(fpOf(kBaseline), fpOf(c));
}

// Préfixe de découverte HA changé → empreinte différente (reconfiguration).
void test_fingerprint_changes_with_ha_discovery_prefix(void) {
  Cfg c = kBaseline;
  c.haPrefix = "homeassistant2";
  TEST_ASSERT_NOT_EQUAL(fpOf(kBaseline), fpOf(c));
}

// Identifiant d'appareil changé → empreinte différente.
void test_fingerprint_changes_with_device_id(void) {
  Cfg c = kBaseline;
  c.deviceId = "pool_controller_2";
  TEST_ASSERT_NOT_EQUAL(fpOf(kBaseline), fpOf(c));
}

// Topic de base changé (reconfiguration MQTT) → empreinte différente.
void test_fingerprint_changes_with_base_topic(void) {
  Cfg c = kBaseline;
  c.baseTopic = "piscine/autre_topic";
  TEST_ASSERT_NOT_EQUAL(fpOf(kBaseline), fpOf(c));
}

// ⚠️ Cas explicitement signalé par bug-121 : « courtier neuf » SANS sentinelle
// dédiée — server et port doivent, à eux seuls, changer l'empreinte, sinon un
// changement de courtier (perte de tous les messages retenus) ne serait jamais
// détecté sans redémarrer l'appareil.
void test_fingerprint_changes_with_broker_server(void) {
  Cfg c = kBaseline;
  c.server = "192.168.1.99";  // nouveau courtier, tout le reste identique
  TEST_ASSERT_NOT_EQUAL(fpOf(kBaseline), fpOf(c));
}

void test_fingerprint_changes_with_broker_port(void) {
  Cfg c = kBaseline;
  c.port = 8883;  // même courtier, port différent (ex : bascule TLS)
  TEST_ASSERT_NOT_EQUAL(fpOf(kBaseline), fpOf(c));
}

// Les deux ensemble (le vrai cas « courtier neuf ») : server ET port changent.
void test_fingerprint_changes_with_new_broker_entirely(void) {
  Cfg c = kBaseline;
  c.server = "mqtt.nouveau-courtier.local";
  c.port = 8883;
  TEST_ASSERT_NOT_EQUAL(fpOf(kBaseline), fpOf(c));
}

// ---------------------------------------------------------------------------
// 3. discoveryDecide() — les six issues, dans l'ordre de priorité documenté
// ---------------------------------------------------------------------------

// Cas NOMINAL — celui qui compte le plus : empreinte inchangée, dernière
// rafale complète, pas de redémarrage HA, pas de récupération forcée →
// AUCUNE republication. C'est le comportement que bug-121 doit garantir.
void test_decide_none_is_the_nominal_case(void) {
  const uint32_t fp = fpOf(kBaseline);
  const DiscoveryRecord stored = makeRecord(fp, 0xAAAAAAAAu, 60, /*complete=*/1);
  const DiscoveryReason r = discoveryDecide(stored, /*storedValid=*/true, fp,
                                             /*forced=*/false, /*haRestart=*/false);
  TEST_ASSERT_TRUE(r == DiscoveryReason::None);
}

// Le motif qui a lancé bug-121 : 4-5 reconnexions en rafale (10 s à 1 s
// d'intervalle, relevé du 08/09) ne doivent produire qu'UNE republication AU
// PLUS — ici zéro, puisque rien n'a changé entre les appels successifs.
void test_decide_stays_none_across_reconnection_burst(void) {
  const uint32_t fp = fpOf(kBaseline);
  const DiscoveryRecord stored = makeRecord(fp, 0xAAAAAAAAu, 60, /*complete=*/1);
  int republishCount = 0;
  // 5 "reconnexions" rapprochées, empreinte et enregistrement inchangés.
  for (int i = 0; i < 5; i++) {
    const DiscoveryReason r = discoveryDecide(stored, /*storedValid=*/true, fp,
                                               /*forced=*/false, /*haRestart=*/false);
    if (r != DiscoveryReason::None) republishCount++;
  }
  TEST_ASSERT_EQUAL_INT(0, republishCount);
}

// Aucun enregistrement NVS valide (1er boot après flash, NVS effacée, taille
// lue ≠ sizeof(DiscoveryRecord)) → FirstBoot, quel que soit le contenu de
// `stored` (storedValid=false doit tout court-circuiter).
void test_decide_first_boot_when_no_valid_record(void) {
  const uint32_t fp = fpOf(kBaseline);
  const DiscoveryRecord garbage = makeRecord(fp, 0, 60, 1);  // contenu "propre" mais invalide
  const DiscoveryReason r = discoveryDecide(garbage, /*storedValid=*/false, fp,
                                             /*forced=*/false, /*haRestart=*/false);
  TEST_ASSERT_TRUE(r == DiscoveryReason::FirstBoot);
}

// Empreinte différente de celle enregistrée → FingerprintChanged.
void test_decide_fingerprint_changed(void) {
  const uint32_t storedFp = fpOf(kBaseline);
  Cfg changed = kBaseline;
  changed.version = "2.54.0";
  const uint32_t currentFp = fpOf(changed);
  const DiscoveryRecord stored = makeRecord(storedFp, 0xAAAAAAAAu, 60, 1);
  const DiscoveryReason r = discoveryDecide(stored, /*storedValid=*/true, currentFp,
                                             /*forced=*/false, /*haRestart=*/false);
  TEST_ASSERT_TRUE(r == DiscoveryReason::FingerprintChanged);
}

// La rafale PRÉCÉDENTE avait au moins un échec (complete == 0) → le jeu
// retenu côté courtier est incomplet, republication même si l'empreinte n'a
// pas bougé.
void test_decide_previous_incomplete(void) {
  const uint32_t fp = fpOf(kBaseline);
  const DiscoveryRecord stored = makeRecord(fp, 0xAAAAAAAAu, 60, /*complete=*/0);
  const DiscoveryReason r = discoveryDecide(stored, /*storedValid=*/true, fp,
                                             /*forced=*/false, /*haRestart=*/false);
  TEST_ASSERT_TRUE(r == DiscoveryReason::PreviousIncomplete);
}

// Redémarrage HA détecté (message de naissance) → HaRestart, MÊME si
// l'empreinte est inchangée et la dernière rafale complète : c'est le seul
// chemin de récupération sans redémarrer l'appareil (A3).
void test_decide_ha_restart(void) {
  const uint32_t fp = fpOf(kBaseline);
  const DiscoveryRecord stored = makeRecord(fp, 0xAAAAAAAAu, 60, /*complete=*/1);
  const DiscoveryReason r = discoveryDecide(stored, /*storedValid=*/true, fp,
                                             /*forced=*/false, /*haRestart=*/true);
  TEST_ASSERT_TRUE(r == DiscoveryReason::HaRestart);
}

// ⚠️ Recovery/forced — EN ISOLATION uniquement (aucun appelant firmware,
// bouton HTTP retiré du périmètre de bug-121). Ne teste QUE le comportement
// de la fonction pure, ne prétend PAS qu'un chemin d'appel existe.
void test_decide_recovery_forced_in_isolation(void) {
  const uint32_t fp = fpOf(kBaseline);
  const DiscoveryRecord stored = makeRecord(fp, 0xAAAAAAAAu, 60, /*complete=*/1);
  const DiscoveryReason r = discoveryDecide(stored, /*storedValid=*/true, fp,
                                             /*forced=*/true, /*haRestart=*/false);
  TEST_ASSERT_TRUE(r == DiscoveryReason::Recovery);
}

// ---------------------------------------------------------------------------
// 3bis. discoveryHaBirthIsGenuineRestart — bug-121 (revue, bloquant 1, 2ᵉ passe)
// ---------------------------------------------------------------------------
// RÉGRESSION CIBLÉE : un message de naissance HA RETENU côté courtier est
// redélivré en quelques ms après le subscribe() de CHAQUE reconnexion — donc
// à CHAQUE reconnexion, quel que soit son espacement avec la précédente
// rafale (13/13 sur le relevé du 08/09, espacées de 15 min à 5 h 10). Le seul
// discriminant qui les distingue d'un vrai redémarrage HA est le temps depuis
// LA CONNEXION — jamais depuis la dernière rafale.

// LE test qui fixe le contrat : birth vu à T+200 ms de la connexion (grace
// par défaut 5000 ms, cf. kDiscoveryHaRestartGraceMs) ⇒ PAS un redémarrage
// réel ⇒ aucune rafale ne doit être déclenchée côté appelant.
void test_ha_birth_at_200ms_after_connect_is_not_genuine_restart(void) {
  TEST_ASSERT_FALSE(discoveryHaBirthIsGenuineRestart(/*elapsedSinceConnectMs=*/200, /*graceMs=*/5000));
}

void test_ha_birth_just_before_grace_boundary_is_not_genuine(void) {
  TEST_ASSERT_FALSE(discoveryHaBirthIsGenuineRestart(4999, 5000));
}

void test_ha_birth_exactly_at_grace_boundary_is_genuine(void) {
  // Frontière INCLUSIVE côté "genuine" : >= grace, pas > grace (cohérent avec
  // discoveryDecide et les autres comparaisons de seuil du module).
  TEST_ASSERT_TRUE(discoveryHaBirthIsGenuineRestart(5000, 5000));
}

void test_ha_birth_long_after_connect_is_genuine_restart(void) {
  // Session établie depuis longtemps (30 min) : cas du VRAI redémarrage HA,
  // qui ne doit jamais être avalé par le filtre anti-redélivrance.
  TEST_ASSERT_TRUE(discoveryHaBirthIsGenuineRestart(30UL * 60UL * 1000UL, 5000));
}

void test_ha_birth_at_connect_instant_zero_elapsed_is_not_genuine(void) {
  // Cas limite réel : le courtier redélivre AVANT même le premier tour de
  // mqttTask qui aurait pu faire avancer millis() de façon mesurable.
  TEST_ASSERT_FALSE(discoveryHaBirthIsGenuineRestart(0, 5000));
}

// ---------------------------------------------------------------------------
// 4. Priorité entre motifs — l'ordre documenté doit être RESPECTÉ
// ---------------------------------------------------------------------------

// storedValid=false l'emporte sur TOUT le reste (y compris haRestart/forced).
void test_priority_invalid_record_beats_everything(void) {
  const uint32_t fp = fpOf(kBaseline);
  const DiscoveryRecord stored = makeRecord(fp, 0xAAAAAAAAu, 60, /*complete=*/0);
  const DiscoveryReason r = discoveryDecide(stored, /*storedValid=*/false, fp,
                                             /*forced=*/true, /*haRestart=*/true);
  TEST_ASSERT_TRUE(r == DiscoveryReason::FirstBoot);
}

// haRestart l'emporte sur forced (les deux vrais en même temps).
void test_priority_ha_restart_beats_forced(void) {
  const uint32_t fp = fpOf(kBaseline);
  const DiscoveryRecord stored = makeRecord(fp, 0xAAAAAAAAu, 60, /*complete=*/1);
  const DiscoveryReason r = discoveryDecide(stored, /*storedValid=*/true, fp,
                                             /*forced=*/true, /*haRestart=*/true);
  TEST_ASSERT_TRUE(r == DiscoveryReason::HaRestart);
}

// forced l'emporte sur "rafale précédente incomplète".
void test_priority_forced_beats_previous_incomplete(void) {
  const uint32_t fp = fpOf(kBaseline);
  const DiscoveryRecord stored = makeRecord(fp, 0xAAAAAAAAu, 60, /*complete=*/0);
  const DiscoveryReason r = discoveryDecide(stored, /*storedValid=*/true, fp,
                                             /*forced=*/true, /*haRestart=*/false);
  TEST_ASSERT_TRUE(r == DiscoveryReason::Recovery);
}

// "rafale précédente incomplète" l'emporte sur "empreinte changée" (les deux
// vrais en même temps : complete==0 ET fingerprint différent).
void test_priority_previous_incomplete_beats_fingerprint_changed(void) {
  const uint32_t storedFp = fpOf(kBaseline);
  Cfg changed = kBaseline;
  changed.version = "2.54.0";
  const uint32_t currentFp = fpOf(changed);
  const DiscoveryRecord stored = makeRecord(storedFp, 0xAAAAAAAAu, 60, /*complete=*/0);
  const DiscoveryReason r = discoveryDecide(stored, /*storedValid=*/true, currentFp,
                                             /*forced=*/false, /*haRestart=*/false);
  TEST_ASSERT_TRUE(r == DiscoveryReason::PreviousIncomplete);
}

// ---------------------------------------------------------------------------
// 5. Mutation (c) — persistance NVS absente ⇒ republication à CHAQUE boot
// ---------------------------------------------------------------------------
// La persistance elle-même vit dans mqtt_manager.cpp (Preferences, impur,
// hors du harnais natif). Ce que ce module PUR garantit, et que ce test
// verrouille : si aucun enregistrement valide n'est JAMAIS obtenu (exactement
// ce qui se produirait si l'écriture NVS était supprimée — le champ
// `_recordValid` de mqtt_manager resterait constamment false d'un boot à
// l'autre), discoveryDecide() ne renvoie JAMAIS None. Sans cette garantie,
// supprimer l'écriture NVS pourrait passer inaperçu en session (la première
// connexion republie déjà, comme un boot normal) et ne se révélerait qu'au
// redémarrage suivant.
void test_missing_persistence_causes_republish_every_boot(void) {
  const uint32_t fp = fpOf(kBaseline);
  const DiscoveryRecord neverPersisted = makeRecord(0, 0, 0, 0);  // jamais chargé avec succès

  // "Boot" n°1
  const DiscoveryReason boot1 = discoveryDecide(neverPersisted, /*storedValid=*/false, fp,
                                                 false, false);
  TEST_ASSERT_TRUE(boot1 == DiscoveryReason::FirstBoot);
  TEST_ASSERT_FALSE(boot1 == DiscoveryReason::None);

  // "Boot" n°2 — SANS écriture NVS entre les deux, storedValid reste false :
  // la republication a lieu à NOUVEAU, ce qui est exactement le défaut que la
  // persistance doit empêcher.
  const DiscoveryReason boot2 = discoveryDecide(neverPersisted, /*storedValid=*/false, fp,
                                                 false, false);
  TEST_ASSERT_TRUE(boot2 == DiscoveryReason::FirstBoot);
  TEST_ASSERT_FALSE(boot2 == DiscoveryReason::None);
}

// Contre-épreuve : AVEC persistance (storedValid=true dès le 2e appel, comme
// si l'écriture avait eu lieu et été relue), la 2e "reconnexion" à empreinte
// inchangée retombe bien sur None. Isole la garantie ci-dessus de tout autre
// facteur (elle ne serait pas contraignante si None était inatteignable).
void test_persistence_restores_none_on_next_boot(void) {
  const uint32_t fp = fpOf(kBaseline);
  const DiscoveryRecord persisted = makeRecord(fp, 0xAAAAAAAAu, 60, /*complete=*/1);
  const DiscoveryReason boot2 = discoveryDecide(persisted, /*storedValid=*/true, fp,
                                                 false, false);
  TEST_ASSERT_TRUE(boot2 == DiscoveryReason::None);
}

// ---------------------------------------------------------------------------
// 6. discoveryRecordDiffers() — la garde d'écriture NVS
// ---------------------------------------------------------------------------

void test_records_differ_returns_false_for_identical_records(void) {
  const DiscoveryRecord a = makeRecord(111, 222, 60, 1);
  const DiscoveryRecord b = makeRecord(111, 222, 60, 1);
  TEST_ASSERT_FALSE(discoveryRecordDiffers(a, b));
}

void test_records_differ_on_fingerprint(void) {
  const DiscoveryRecord a = makeRecord(111, 222, 60, 1);
  const DiscoveryRecord b = makeRecord(999, 222, 60, 1);
  TEST_ASSERT_TRUE(discoveryRecordDiffers(a, b));
}

void test_records_differ_on_entity_hash(void) {
  const DiscoveryRecord a = makeRecord(111, 222, 60, 1);
  const DiscoveryRecord b = makeRecord(111, 999, 60, 1);
  TEST_ASSERT_TRUE(discoveryRecordDiffers(a, b));
}

void test_records_differ_on_entity_count(void) {
  const DiscoveryRecord a = makeRecord(111, 222, 60, 1);
  const DiscoveryRecord b = makeRecord(111, 222, 61, 1);
  TEST_ASSERT_TRUE(discoveryRecordDiffers(a, b));
}

void test_records_differ_on_complete_flag(void) {
  const DiscoveryRecord a = makeRecord(111, 222, 60, 1);
  const DiscoveryRecord b = makeRecord(111, 222, 60, 0);
  TEST_ASSERT_TRUE(discoveryRecordDiffers(a, b));
}

// ---------------------------------------------------------------------------
// 7. discoveryReasonLabel() — le motif qui doit apparaître dans la ligne (A4)
// ---------------------------------------------------------------------------

void test_reason_labels_are_all_distinct_and_non_empty(void) {
  const DiscoveryReason reasons[] = {
      DiscoveryReason::None,       DiscoveryReason::FirstBoot,
      DiscoveryReason::FingerprintChanged, DiscoveryReason::PreviousIncomplete,
      DiscoveryReason::Recovery,   DiscoveryReason::HaRestart,
  };
  const size_t n = sizeof(reasons) / sizeof(reasons[0]);
  for (size_t i = 0; i < n; i++) {
    const char* li = discoveryReasonLabel(reasons[i]);
    TEST_ASSERT_NOT_NULL(li);
    TEST_ASSERT_TRUE(strlen(li) > 0);
    for (size_t j = i + 1; j < n; j++) {
      TEST_ASSERT_NOT_EQUAL(0, strcmp(li, discoveryReasonLabel(reasons[j])));
    }
  }
}

void test_reason_label_none_is_aucune(void) {
  TEST_ASSERT_EQUAL_STRING("aucune", discoveryReasonLabel(DiscoveryReason::None));
}

void test_reason_label_previous_incomplete_is_the_longest(void) {
  // Utilisé par kDiscoverySummaryLineMax (constants.h) comme pire cas de
  // longueur de libellé — verrouillé ici pour détecter toute dérive.
  const char* labels[] = {
      discoveryReasonLabel(DiscoveryReason::FirstBoot),
      discoveryReasonLabel(DiscoveryReason::FingerprintChanged),
      discoveryReasonLabel(DiscoveryReason::PreviousIncomplete),
      discoveryReasonLabel(DiscoveryReason::Recovery),
      discoveryReasonLabel(DiscoveryReason::HaRestart),
  };
  const char* longest = discoveryReasonLabel(DiscoveryReason::PreviousIncomplete);
  for (const char* l : labels) {
    TEST_ASSERT_TRUE(strlen(l) <= strlen(longest));
  }
  TEST_ASSERT_EQUAL_INT(31, (int)strlen(longest));  // "rafale précédente incomplète", en octets UTF-8
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fnv1a_known_vectors);
  RUN_TEST(test_fnv1a_str_nullptr_is_noop);
  RUN_TEST(test_fnv1a_u32_matches_manual_lsb_first_byte_chain);
  RUN_TEST(test_fnv1a_u32_no_collision_on_adjacent_ports);
  RUN_TEST(test_fnv1a_str_no_collision_on_adjacent_versions);

  RUN_TEST(test_fingerprint_is_stable_on_identical_inputs);
  RUN_TEST(test_fingerprint_changes_with_firmware_version);
  RUN_TEST(test_fingerprint_changes_with_ha_discovery_prefix);
  RUN_TEST(test_fingerprint_changes_with_device_id);
  RUN_TEST(test_fingerprint_changes_with_base_topic);
  RUN_TEST(test_fingerprint_changes_with_broker_server);
  RUN_TEST(test_fingerprint_changes_with_broker_port);
  RUN_TEST(test_fingerprint_changes_with_new_broker_entirely);

  RUN_TEST(test_decide_none_is_the_nominal_case);
  RUN_TEST(test_decide_stays_none_across_reconnection_burst);
  RUN_TEST(test_decide_first_boot_when_no_valid_record);
  RUN_TEST(test_decide_fingerprint_changed);
  RUN_TEST(test_decide_previous_incomplete);
  RUN_TEST(test_decide_ha_restart);
  RUN_TEST(test_decide_recovery_forced_in_isolation);

  RUN_TEST(test_ha_birth_at_200ms_after_connect_is_not_genuine_restart);
  RUN_TEST(test_ha_birth_just_before_grace_boundary_is_not_genuine);
  RUN_TEST(test_ha_birth_exactly_at_grace_boundary_is_genuine);
  RUN_TEST(test_ha_birth_long_after_connect_is_genuine_restart);
  RUN_TEST(test_ha_birth_at_connect_instant_zero_elapsed_is_not_genuine);

  RUN_TEST(test_priority_invalid_record_beats_everything);
  RUN_TEST(test_priority_ha_restart_beats_forced);
  RUN_TEST(test_priority_forced_beats_previous_incomplete);
  RUN_TEST(test_priority_previous_incomplete_beats_fingerprint_changed);

  RUN_TEST(test_missing_persistence_causes_republish_every_boot);
  RUN_TEST(test_persistence_restores_none_on_next_boot);

  RUN_TEST(test_records_differ_returns_false_for_identical_records);
  RUN_TEST(test_records_differ_on_fingerprint);
  RUN_TEST(test_records_differ_on_entity_hash);
  RUN_TEST(test_records_differ_on_entity_count);
  RUN_TEST(test_records_differ_on_complete_flag);

  RUN_TEST(test_reason_labels_are_all_distinct_and_non_empty);
  RUN_TEST(test_reason_label_none_is_aucune);
  RUN_TEST(test_reason_label_previous_incomplete_is_the_longest);
  return UNITY_END();
}
