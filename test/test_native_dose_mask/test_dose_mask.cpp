// =============================================================================
// Tests natifs — bug-098 : le MASQUE DE VOIES de dosage, de bout en bout
// =============================================================================
// CE QUE CE LOT PROTÈGE
// --------------------
// L'idiome du dépôt : « le domaine d'une valeur ne porte jamais l'absence de
// valeur ». Pour un flottant, l'absence se dit NAN ; pour une grandeur
// DISCRÈTE, elle se dit par un BIT dans un champ de bits, jamais par une
// valeur de la grandeur elle-même.
//
// Le défaut corrigé ne perdait pas seulement de l'information : il FABRIQUAIT
// une donnée fausse et FAVORABLE. `obj["d"] = phDosing || orpDosing` écrivait
// `true` pour une injection de CHLORE, et `dp.phDosing = point["d"]` la
// relisait en injection d'ACIDE. L'historique sous-déclarait donc la
// consommation de chlore et surdéclarait celle d'acide — puis
// `consolidateData()` figeait le mensonge dans les agrégats DAILY, 75 à 90
// jours durant.
//
// POURQUOI CES TESTS LISENT LE TEXTE DES SOURCES
// ----------------------------------------------
// `HistoryManager::saveToFile/loadFromFile` et `/history/import` ne sont PAS
// compilables en natif (LittleFS, FreeRTOS, sensors, pump_controller,
// ArduinoJson) et l'env `native` ne compile que les modules purs
// (platformio.ini, build_src_filter). C'est le patron déjà en place dans
// test_native_history (« ASSERTIONS SUR LES SOURCES ») et dans les deux suites
// de parité WebSocket.
//
// Mais on ne s'arrête pas à « telle chaîne est présente ». Les faits extraits
// des sources ALIMENTENT un codec de round-trip (`DoseCodec`) : les tests de
// comportement — trois formats de fichier, deux cycles consécutifs, union à
// l'agrégation — s'exécutent RÉELLEMENT, sur une politique lue dans le code de
// production. Changer `& 0x07` en `& 0x03` dans src/history.cpp change donc le
// RÉSULTAT de ces tests, pas seulement une assertion textuelle.
//
// CE QUE CETTE SUITE NE PROUVE PAS : la sémantique exacte d'ArduinoJson 7
// (`is<bool>()` sur un entier, `is<int>()` sur un booléen). Elle est verrouillée
// STRUCTURELLEMENT — le discriminant doit être là — et reste à vérifier sur
// cible par les scénarios d'intégration du rapport.
// =============================================================================

#include <unity.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Le CONTRAT de bits, tel qu'il doit être dans src/history.h
// -----------------------------------------------------------------------------
// Recopiés en littéraux ici À DESSEIN : test_contrat_valeurs_de_bits_figees()
// vérifie que src/history.h porte bien ces valeurs. Inclure history.h est
// impossible (il tire <vector> et LittleFS.h).
static const uint8_t kPh      = 0x01;  // kHistDosePh
static const uint8_t kOrp     = 0x02;  // kHistDoseOrp
static const uint8_t kUnknown = 0x04;  // kHistDoseUnknownChannel
static const uint8_t kAllBits = 0x07;

// =============================================================================
// Lecture des sources du projet
// =============================================================================

static void stripComments(char* s);

static char* readProjectFile(const char* rel, long* outLen) {
  char path[512];
  char rooted[512];
  const char* candidates[2];

  rooted[0] = '\0';
  {
    const char* f = __FILE__;
    const char* m = strstr(f, "test/test_native_dose_mask/");
    if (m != NULL && (size_t)(m - f) < sizeof(rooted) - 256) {
      const size_t n = (size_t)(m - f);
      memcpy(rooted, f, n);
      rooted[n] = '\0';
      strncat(rooted, rel, sizeof(rooted) - strlen(rooted) - 1);
    }
  }
  candidates[0] = rooted[0] ? rooted : NULL;
  candidates[1] = rel;

  const char* prefixes[3] = {"../", "../../", "../../../"};
  for (int i = 0; i < 5; ++i) {
    if (i >= 2) {
      snprintf(path, sizeof(path), "%s%s", prefixes[i - 2], rel);
    } else {
      if (candidates[i] == NULL) continue;
      snprintf(path, sizeof(path), "%s", candidates[i]);
    }
    FILE* fp = fopen(path, "rb");
    if (fp == NULL) continue;
    fseek(fp, 0, SEEK_END);
    const long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0 || len > 4000000L) { fclose(fp); continue; }
    char* buf = (char*)malloc((size_t)len + 1);
    if (buf == NULL) { fclose(fp); continue; }
    const size_t got = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    buf[got] = '\0';
    stripComments(buf);
    *outLen = (long)strlen(buf);
    return buf;
  }
  return NULL;
}

// Neutralise les COMMENTAIRES (remplacés par des espaces) et le contenu des
// littéraux de chaîne/caractère. Indispensable ici : les commentaires de
// bug-098 CITENT les formes fautives (« `item["dosing"] | false` rend false sur
// un entier ») — un test qui scanne le texte brut les prendrait pour du code et
// échouerait sur la version CORRECTE. On mesure ce que le compilateur voit.
static void stripComments(char* s) {
  size_t i = 0;
  while (s[i] != '\0') {
    if (s[i] == '/' && s[i + 1] == '/') {
      while (s[i] != '\0' && s[i] != '\n') s[i++] = ' ';
    } else if (s[i] == '/' && s[i + 1] == '*') {
      s[i++] = ' ';
      s[i++] = ' ';
      while (s[i] != '\0' && !(s[i] == '*' && s[i + 1] == '/')) {
        if (s[i] != '\n') s[i] = ' ';
        i++;
      }
      if (s[i] != '\0') { s[i++] = ' '; s[i++] = ' '; }
    } else if (s[i] == '\'') {
      // Littéral caractère : conservé tel quel (jamais porteur de motif ici).
      i++;
      if (s[i] == '\\' && s[i + 1] != '\0') i += 2;
      else if (s[i] != '\0') i++;
      if (s[i] == '\'') i++;
    } else if (s[i] == '"') {
      // Chaîne CONSERVÉE : les motifs recherchés (obj["d"], item["dosing"])
      // contiennent eux-mêmes des littéraux de clé JSON.
      i++;
      while (s[i] != '\0' && s[i] != '"') {
        if (s[i] == '\\' && s[i + 1] != '\0') i++;
        i++;
      }
      if (s[i] != '\0') i++;
    } else {
      i++;
    }
  }
}

// Sous-chaîne allant de `from` (inclus) à `to` (exclu).
static const char* slice(const char* src, const char* from, const char* to,
                         size_t* outLen) {
  const char* a = strstr(src, from);
  if (a == NULL) return NULL;
  const char* b = strstr(a + strlen(from), to);
  *outLen = (b != NULL) ? (size_t)(b - a) : strlen(a);
  return a;
}

static int countIn(const char* hay, size_t len, const char* needle) {
  int c = 0;
  const size_t n = strlen(needle);
  for (size_t i = 0; i + n <= len; ++i) {
    if (memcmp(hay + i, needle, n) == 0) c++;
  }
  return c;
}

static int containsIn(const char* hay, size_t len, const char* needle) {
  return countIn(hay, len, needle) > 0;
}

// =============================================================================
// LE CODEC : la politique de sérialisation, EXTRAITE des sources
// =============================================================================
// Chaque champ répond à une question posée au code de production. Les tests de
// comportement en dépendent : une mutation dans src/ déplace un champ, donc un
// résultat, donc une assertion.
struct DoseCodec {
  int      writeIsMask;      // saveToFile écrit-il le MASQUE (et non un OU logique) ?
  int      hasBoolBranch;    // le discriminant `is<bool>()` est-il présent ?
  uint8_t  legacyTrueBits;   // ce qu'un `"d":true` de ≤ 2.46 devient
  uint8_t  readMaskBits;     // masque appliqué à la valeur ENTIÈRE relue
  int      aggregateUnions;  // consolidateData fait-elle une UNION bit à bit ?
  int      dedupUnions;      // dedupAggregatedPoints fait-elle une UNION ?
  int      exportPassesMask; // /get-history passe-t-il p.doseMask tel quel ?
};

// Extrait le masque appliqué derrière `as<unsigned int>() &`. Reconnaît les
// trois constantes nommées ET un littéral hexadécimal — un mutant écrit sous
// l'une ou l'autre forme (`& 0x03` comme `& (kHistDosePh | kHistDoseOrp)`)
// produit donc bien un masque à 2 bits ici, et non un faux « tout va bien ».
static uint8_t extractReadMask(const char* body, size_t bodyLen) {
  const char* anchor = "as<unsigned int>() &";
  const char* a = NULL;
  const size_t an = strlen(anchor);
  for (size_t i = 0; i + an <= bodyLen; ++i) {
    if (memcmp(body + i, anchor, an) == 0) { a = body + i + an; break; }
  }
  if (a == NULL) return 0;
  const char* end = strchr(a, ';');
  if (end == NULL) return 0;
  const size_t n = (size_t)(end - a);

  uint8_t bits = 0;
  if (containsIn(a, n, "kHistDosePh")) bits |= kPh;
  if (containsIn(a, n, "kHistDoseOrp")) bits |= kOrp;
  if (containsIn(a, n, "kHistDoseUnknownChannel")) bits |= kUnknown;
  // Forme littérale : 0xNN / décimal nu.
  for (size_t i = 0; i + 2 <= n; ++i) {
    if (a[i] == '0' && (a[i + 1] == 'x' || a[i + 1] == 'X')) {
      bits |= (uint8_t)strtoul(a + i, NULL, 16);
    }
  }
  return bits;
}

// Politique du fichier persisté /history.json (saveToFile / loadFromFile).
static DoseCodec persistCodec(void) {
  DoseCodec c;
  memset(&c, 0, sizeof(c));

  long len = 0;
  char* hist = readProjectFile("src/history.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(hist, "src/history.cpp introuvable");

  size_t saveLen = 0;
  const char* save = slice(hist, "void HistoryManager::saveToFile()",
                           "void HistoryManager::loadFromFile()", &saveLen);
  TEST_ASSERT_NOT_NULL(save);
  c.writeIsMask = containsIn(save, saveLen, "obj[\"d\"] = point.doseMask;") &&
                  !containsIn(save, saveLen, "point.phDosing") &&
                  !containsIn(save, saveLen, "point.orpDosing");

  size_t loadLen = 0;
  const char* load = slice(hist, "void HistoryManager::loadFromFile()",
                           "void HistoryManager::migrateLegacyHistory", &loadLen);
  TEST_ASSERT_NOT_NULL(load);
  c.hasBoolBranch = containsIn(load, loadLen, "dosingVar.is<bool>()");
  c.legacyTrueBits =
      containsIn(load, loadLen, "dosingVar.as<bool>() ? kHistDoseUnknownChannel : 0")
          ? kUnknown : 0;
  c.readMaskBits = extractReadMask(load, loadLen);

  size_t consoLen = 0;
  const char* conso = slice(hist, "void HistoryManager::consolidateData()",
                            "void HistoryManager::dedupAggregatedPoints", &consoLen);
  TEST_ASSERT_NOT_NULL(conso);
  // Les DEUX boucles (HOURLY et DAILY) doivent unir, pas écraser.
  c.aggregateUnions =
      countIn(conso, consoLen, "doseMaskUnion | p.doseMask") == 2 &&
      countIn(conso, consoLen, "avgPoint.doseMask = doseMaskUnion;") == 2;

  size_t dedupLen = 0;
  const char* dedup = slice(hist, "void HistoryManager::dedupAggregatedPoints",
                            "bool HistoryManager::clearHistory", &dedupLen);
  TEST_ASSERT_NOT_NULL(dedup);
  c.dedupUnions = containsIn(dedup, dedupLen, "dup->doseMask | p.doseMask");

  free(hist);
  return c;
}

// Politique de la route /history/import (format EXPORTÉ, clés longues).
static DoseCodec importCodec(void) {
  DoseCodec c;
  memset(&c, 0, sizeof(c));

  long len = 0;
  char* web = readProjectFile("src/web_routes_data.cpp", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(web, "src/web_routes_data.cpp introuvable");

  size_t impLen = 0;
  const char* imp = slice(web, "\"/history/import\"", "if (imported.empty())", &impLen);
  TEST_ASSERT_NOT_NULL(imp);
  c.hasBoolBranch = containsIn(imp, impLen, "dosingVar.is<bool>()");
  c.legacyTrueBits =
      containsIn(imp, impLen, "dosingVar.as<bool>() ? kHistDoseUnknownChannel : 0")
          ? kUnknown : 0;
  c.readMaskBits = extractReadMask(imp, impLen);
  // Le piège du lot : `item["dosing"] | false` rend false sur un entier.
  c.writeIsMask = !containsIn(imp, impLen, "dp.doseMask = item[\"dosing\"]") &&
                  !containsIn(imp, impLen, "dp.phDosing = item[\"dosing\"]") &&
                  !containsIn(imp, impLen, "item[\"dosing\"] | false");

  size_t expLen = 0;
  const char* exp = slice(web, "static void handleGetHistory", "server->on(\"/get-history\"",
                          &expLen);
  TEST_ASSERT_NOT_NULL(exp);
  c.exportPassesMask = containsIn(exp, expLen, "p.filtrationActive, p.doseMask,") &&
                       !containsIn(exp, expLen, "p.phDosing || p.orpDosing");

  free(web);
  return c;
}

// =============================================================================
// LE ROUND-TRIP, joué avec la politique extraite
// =============================================================================
// `rtWrite` reproduit l'écriture d'un point (clés courtes du fichier persisté),
// `rtRead` la relecture. Les DEUX consultent le codec : c'est ce qui rend ces
// tests sensibles aux mutations du code de production.

// Écrit un point tel que saveToFile l'écrirait sous la politique `c`.
static void rtWrite(char* out, size_t cap, uint8_t mask, const DoseCodec& c) {
  if (c.writeIsMask) {
    snprintf(out, cap, "{\"t\":1757000000,\"p\":7.23,\"f\":true,\"d\":%u,\"g\":0}",
             (unsigned)mask);
  } else {
    // Mutant : refusion des deux voies en un booléen.
    snprintf(out, cap, "{\"t\":1757000000,\"p\":7.23,\"f\":true,\"d\":%s,\"g\":0}",
             mask ? "true" : "false");
  }
}

// Relit la clé "d" d'un point tel que loadFromFile / import le relirait.
static uint8_t rtRead(const char* json, const DoseCodec& c) {
  const char* v = strstr(json, "\"d\":");
  if (v == NULL) return 0;  // clé absente : aucun dosage
  v += 4;
  if (strncmp(v, "true", 4) == 0) {
    // Sans le discriminant `is<bool>()`, le booléen tombe dans la branche
    // entière : ArduinoJson en tire 1 (ou rien), c'est-à-dire un dosage pH
    // INVENTÉ (ou l'ancien mensonge favorable). Jamais « voie indéterminée ».
    return c.hasBoolBranch ? c.legacyTrueBits : (uint8_t)(1u & c.readMaskBits);
  }
  if (strncmp(v, "false", 5) == 0) return 0;
  return (uint8_t)(strtoul(v, NULL, 10) & c.readMaskBits);
}

// =============================================================================
// 1 — LE CONTRAT DE BITS
// =============================================================================

void test_contrat_valeurs_de_bits_figees(void) {
  // Ces trois valeurs sont PERSISTÉES dans /history.json et SERVIES par
  // /get-history : les renuméroter rendrait illisible tout fichier déjà écrit.
  long len = 0;
  char* h = readProjectFile("src/history.h", &len);
  TEST_ASSERT_NOT_NULL_MESSAGE(h, "src/history.h introuvable");
  TEST_ASSERT_TRUE_MESSAGE(
      containsIn(h, (size_t)len, "constexpr uint8_t kHistDosePh  = 0x01;"),
      "kHistDosePh renumerote : les fichiers deja ecrits deviennent illisibles");
  TEST_ASSERT_TRUE_MESSAGE(
      containsIn(h, (size_t)len, "constexpr uint8_t kHistDoseOrp = 0x02;"),
      "kHistDoseOrp renumerote");
  TEST_ASSERT_TRUE_MESSAGE(
      containsIn(h, (size_t)len, "constexpr uint8_t kHistDoseUnknownChannel = 0x04;"),
      "kHistDoseUnknownChannel renumerote");
  // Trois bits DISJOINTS : c'est ce qui permet le cumul (union à l'agrégation).
  TEST_ASSERT_EQUAL_UINT8(0, kPh & kOrp);
  TEST_ASSERT_EQUAL_UINT8(0, kPh & kUnknown);
  TEST_ASSERT_EQUAL_UINT8(0, kOrp & kUnknown);
  TEST_ASSERT_EQUAL_UINT8(kAllBits, kPh | kOrp | kUnknown);
  free(h);
}

void test_contrat_les_deux_booleens_ont_disparu_de_datapoint(void) {
  // Tant que `phDosing`/`orpDosing` existent dans DataPoint, un chemin peut
  // encore les écrire et refabriquer la fusion. Le champ de bits les remplace,
  // il ne les double pas — et il tient dans UN octet là où ils en prenaient deux.
  long len = 0;
  char* h = readProjectFile("src/history.h", &len);
  TEST_ASSERT_NOT_NULL(h);
  size_t bodyLen = 0;
  const char* body = slice(h, "struct DataPoint {", "};", &bodyLen);
  TEST_ASSERT_NOT_NULL(body);
  TEST_ASSERT_TRUE_MESSAGE(containsIn(body, bodyLen, "uint8_t doseMask;"),
      "DataPoint::doseMask absent");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(body, bodyLen, "bool phDosing"),
      "phDosing survit dans DataPoint : la fusion peut etre refabriquee");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, countIn(body, bodyLen, "bool orpDosing"),
      "orpDosing survit dans DataPoint");
  free(h);
}

void test_contrat_enregistrement_n_intervertit_pas_les_deux_pompes(void) {
  // L'ORIGINE de la donnée : recordDataPoint() compose le masque depuis les
  // deux getters de PumpController. Intervertir les bits ici referait le bug à
  // l'envers — un dosage d'acide compté comme du chlore — sans qu'aucun
  // round-trip ne s'en aperçoive (le masque circulerait intact, mais faux).
  // On vérifie donc l'APPARIEMENT, par l'ordre : chaque getter est suivi de SON
  // bit, et aucun autre entre les deux.
  long len = 0;
  char* src = readProjectFile("src/history.cpp", &len);
  TEST_ASSERT_NOT_NULL(src);
  size_t bodyLen = 0;
  const char* body = slice(src, "void HistoryManager::recordDataPoint()",
                           "void HistoryManager::saveToFile()", &bodyLen);
  TEST_ASSERT_NOT_NULL(body);

  const char* phFn = strstr(body, "PumpController.isPhDosing()");
  const char* phBit = strstr(body, "kHistDosePh");
  const char* orpFn = strstr(body, "PumpController.isOrpDosing()");
  const char* orpBit = strstr(body, "kHistDoseOrp");
  TEST_ASSERT_NOT_NULL_MESSAGE(phFn, "isPhDosing() ne nourrit plus le masque");
  TEST_ASSERT_NOT_NULL_MESSAGE(orpFn, "isOrpDosing() ne nourrit plus le masque");
  TEST_ASSERT_NOT_NULL(phBit);
  TEST_ASSERT_NOT_NULL(orpBit);
  TEST_ASSERT_TRUE_MESSAGE(phFn < phBit && phBit < orpFn && orpFn < orpBit,
      "les deux voies de dosage sont interverties a l'enregistrement : "
      "un dosage d'acide serait compte comme du chlore");

  // Un point VIVANT connaît toujours sa voie : le bit « indéterminée » est
  // réservé à la relecture d'un fichier hérité. L'écrire ici masquerait la voie
  // réelle d'une injection en cours.
  TEST_ASSERT_EQUAL_INT_MESSAGE(0,
      countIn(body, bodyLen, "kHistDoseUnknownChannel"),
      "kHistDoseUnknownChannel ecrit sur un point vivant : la voie reelle, "
      "pourtant connue, serait perdue");
  free(src);
}

// =============================================================================
// 2 — LA POLITIQUE LUE DANS LES SOURCES
// =============================================================================

void test_politique_ecriture_est_le_masque_pas_un_ou_logique(void) {
  const DoseCodec c = persistCodec();
  TEST_ASSERT_TRUE_MESSAGE(c.writeIsMask,
      "saveToFile ne serialise plus le masque (fusion phDosing || orpDosing ?) : "
      "un dosage ORP se relira en dosage pH");
}

void test_politique_discriminant_de_type_present_aux_deux_relectures(void) {
  // Sans `is<bool>()`, un fichier ≤ 2.46 n'est plus reconnu comme ancien : son
  // `true` passe par la branche entière et vaut 1, soit un dosage pH INVENTÉ.
  const DoseCodec p = persistCodec();
  const DoseCodec i = importCodec();
  TEST_ASSERT_TRUE_MESSAGE(p.hasBoolBranch,
      "loadFromFile a perdu le discriminant is<bool>() : ancien format mal relu");
  TEST_ASSERT_TRUE_MESSAGE(i.hasBoolBranch,
      "/history/import a perdu le discriminant is<bool>()");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kUnknown, p.legacyTrueBits,
      "un `true` herite ne devient plus « voie indeterminee »");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kUnknown, i.legacyTrueBits,
      "un `true` herite ne devient plus « voie indeterminee » a l'import");
}

// LE test de l'écart le plus subtil du lot.
void test_politique_le_masque_de_relecture_porte_LES_TROIS_BITS(void) {
  // Le plan initial disait de relire avec `& 0x03`. C'était FAUX : le bit
  // kHistDoseUnknownChannel aurait été rasé à chaque relecture. Le premier
  // cycle sauvegarde/relecture après l'OTA (5 minutes) aurait écrit `"d":4`
  // puis relu 0 — « voie indéterminée » redevenant « aucun dosage », soit le
  // mensonge favorable réintroduit par le correctif lui-même.
  const DoseCodec p = persistCodec();
  const DoseCodec i = importCodec();
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kAllBits, p.readMaskBits,
      "loadFromFile relit sur moins de 3 bits (& 0x03 ?) : le bit « voie "
      "indeterminee » est rase des le premier cycle apres l'OTA");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kAllBits, i.readMaskBits,
      "/history/import relit sur moins de 3 bits (& 0x03 ?)");
}

void test_politique_import_ne_reprend_pas_l_operateur_pipe_false(void) {
  // `item["dosing"] | false` rend `false` sur un ENTIER (l'opérateur | exige la
  // concordance de type) : tout masque d'un export neuf serait effacé.
  const DoseCodec i = importCodec();
  TEST_ASSERT_TRUE_MESSAGE(i.writeIsMask,
      "`item[\"dosing\"] | false` de retour : tout masque importe est efface");
}

void test_politique_export_passe_le_masque_tel_quel(void) {
  const DoseCodec i = importCodec();
  TEST_ASSERT_TRUE_MESSAGE(i.exportPassesMask,
      "/get-history refusionne les voies avant le format (p.phDosing || p.orpDosing)");
}

void test_politique_agregation_unit_bit_a_bit(void) {
  const DoseCodec p = persistCodec();
  TEST_ASSERT_TRUE_MESSAGE(p.aggregateUnions,
      "consolidateData n'unit plus les masques (= au lieu de |=) : le dernier "
      "point du creneau ecrase les voies des precedents");
  TEST_ASSERT_TRUE_MESSAGE(p.dedupUnions,
      "dedupAggregatedPoints n'unit plus les masques");
}

// =============================================================================
// 3 — ROUND-TRIP DES TROIS FORMATS DE FICHIER
// =============================================================================

void test_roundtrip_fichier_neuf_toutes_les_voies(void) {
  // Format ≥ 2.47 : `"d"` ENTIER. Les huit combinaisons des trois bits
  // survivent à un aller-retour, à l'identique.
  const DoseCodec c = persistCodec();
  for (uint8_t m = 0; m <= kAllBits; ++m) {
    char buf[160];
    rtWrite(buf, sizeof(buf), m, c);
    const uint8_t relu = rtRead(buf, c);
    char msg[128];
    snprintf(msg, sizeof(msg), "masque %u ecrit, %u relu : perte au round-trip",
             (unsigned)m, (unsigned)relu);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(m, relu, msg);
  }
}

void test_roundtrip_fichier_ancien_booleen_devient_voie_indeterminee(void) {
  // Format ≤ 2.46 : `"d"` BOOLÉEN. `true` ne dit PAS quelle pompe a tourné :
  // le relire en kHistDosePh inventerait un dosage d'acide sur une injection
  // de chlore. Il devient donc « dosage, voie indéterminée ».
  const DoseCodec c = persistCodec();
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kUnknown,
      rtRead("{\"t\":1757000000,\"f\":true,\"d\":true,\"g\":0}", c),
      "un `true` herite doit devenir « voie indeterminee », jamais un dosage pH");
  TEST_ASSERT_EQUAL_UINT8(0, rtRead("{\"t\":1757000000,\"d\":false,\"g\":0}", c));
  // Clé ABSENTE (points d'agrégat des toutes premières versions) : aucun
  // dosage, et surtout aucune erreur de relecture.
  TEST_ASSERT_EQUAL_UINT8(0, rtRead("{\"t\":1757000000,\"p\":7.2,\"g\":1}", c));
}

void test_roundtrip_fichier_mixte_se_relit_point_par_point(void) {
  // LE cas réel de l'OTA : /history.json contient les points de la veille
  // (booléens) et ceux d'après la mise à jour (entiers), plus des agrégats sans
  // la clé. Chaque point est relu selon SON format, aucun ne contamine l'autre.
  const DoseCodec c = persistCodec();
  struct { const char* json; uint8_t attendu; } fichier[] = {
      {"{\"t\":1,\"d\":true,\"g\":0}",  kUnknown},           // ancien, dosage
      {"{\"t\":2,\"d\":false,\"g\":0}", 0},                  // ancien, rien
      {"{\"t\":3,\"g\":1}",             0},                  // clé absente
      {"{\"t\":4,\"d\":2,\"g\":0}",     kOrp},               // neuf, ORP seul
      {"{\"t\":5,\"d\":1,\"g\":0}",     kPh},                // neuf, pH seul
      {"{\"t\":6,\"d\":3,\"g\":0}",     (uint8_t)(kPh | kOrp)},
      {"{\"t\":7,\"d\":4,\"g\":1}",     kUnknown},           // agrégat hérité
      {"{\"t\":8,\"d\":7,\"g\":2}",     kAllBits},           // union complète
      {"{\"t\":9,\"d\":true,\"g\":0}",  kUnknown},           // retour à l'ancien
  };
  for (size_t i = 0; i < sizeof(fichier) / sizeof(fichier[0]); ++i) {
    char msg[128];
    snprintf(msg, sizeof(msg), "point %u du fichier mixte mal relu",
             (unsigned)(i + 1));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(fichier[i].attendu,
                                    rtRead(fichier[i].json, c), msg);
  }
}

void test_roundtrip_bits_hors_contrat_sont_ignores_sans_erreur(void) {
  // Un firmware futur pourrait poser un 4e bit (0x08). Un firmware ANCIEN doit
  // le laisser tomber sans se tromper sur les trois qu'il connaît — et surtout
  // sans rejeter le fichier.
  const DoseCodec c = persistCodec();
  TEST_ASSERT_EQUAL_UINT8(kAllBits, rtRead("{\"d\":255}", c));
  TEST_ASSERT_EQUAL_UINT8(kOrp, rtRead("{\"d\":10}", c));  // 0x0A → 0x02
}

// =============================================================================
// 4 — LE BIT « VOIE INDÉTERMINÉE » SURVIT À PLUSIEURS CYCLES
// =============================================================================

void test_deux_cycles_consecutifs_conservent_la_voie_indeterminee(void) {
  // LE test qui meurt si quelqu'un remet `& 0x03`.
  //
  // Chronologie réelle : l'appareil est mis à jour, relit /history.json écrit
  // par ≤ 2.46 (`"d":true` → masque 4), puis sauvegarde CINQ MINUTES plus tard
  // (`"d":4`) et relira à son prochain démarrage. Avec un masque de relecture
  // à 2 bits, ce deuxième cycle rend 0 : « dosage, voie indéterminée » devient
  // « aucun dosage » — le mensonge favorable, réintroduit par le correctif.
  const DoseCodec c = persistCodec();

  // Cycle 0 — l'héritage : le fichier de l'ancien firmware.
  uint8_t mask = rtRead("{\"t\":1757000000,\"f\":true,\"d\":true,\"g\":0}", c);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kUnknown, mask,
      "relecture du fichier <= 2.46 : le dosage doit rester signale");

  // Cycles 1 et 2 — les sauvegardes/relectures de 2.47, à 5 minutes d'écart.
  for (int cycle = 1; cycle <= 2; ++cycle) {
    char buf[160];
    rtWrite(buf, sizeof(buf), mask, c);
    char attendu[32];
    snprintf(attendu, sizeof(attendu), "\"d\":%u", (unsigned)kUnknown);
    char msgW[160];
    snprintf(msgW, sizeof(msgW),
             "cycle %d : le masque 4 n'est pas ECRIT tel quel (%s)", cycle, buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, attendu), msgW);

    mask = rtRead(buf, c);
    char msgR[160];
    snprintf(msgR, sizeof(msgR),
             "cycle %d : masque 4 ecrit, %u relu — la voie indeterminee a ete "
             "rasee (& 0x03 ?), le dosage passe pour inexistant",
             cycle, (unsigned)mask);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(kUnknown, mask, msgR);
  }

  // Et le bit n'a pas dérivé vers une VOIE : il ne faut pas non plus qu'un
  // dosage indéterminé finisse par se dire « pH ».
  TEST_ASSERT_EQUAL_UINT8(0, mask & kPh);
  TEST_ASSERT_EQUAL_UINT8(0, mask & kOrp);
}

void test_dix_cycles_ne_derivent_pas(void) {
  // Le point vit jusqu'à 90 jours dans l'anneau : il traverse des centaines de
  // cycles. Aucun ne doit ni perdre ni ajouter un bit.
  const DoseCodec c = persistCodec();
  for (uint8_t depart = 0; depart <= kAllBits; ++depart) {
    uint8_t m = depart;
    for (int i = 0; i < 10; ++i) {
      char buf[160];
      rtWrite(buf, sizeof(buf), m, c);
      m = rtRead(buf, c);
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "masque %u derive a %u en 10 cycles",
             (unsigned)depart, (unsigned)m);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(depart, m, msg);
  }
}

// =============================================================================
// 5 — UNION BIT À BIT À L'AGRÉGATION
// =============================================================================

// Replie un créneau comme consolidateData(), sous la politique extraite :
// union bit à bit si le code de production unit, ÉCRASEMENT sinon.
static uint8_t foldGroup(const uint8_t* masks, size_t n, const DoseCodec& c) {
  uint8_t acc = 0;
  for (size_t i = 0; i < n; ++i) {
    acc = c.aggregateUnions ? (uint8_t)(acc | masks[i]) : masks[i];
  }
  return acc;
}

void test_agregation_ph_seul_et_orp_seul_donnent_les_deux_bits(void) {
  // Deux points de la MÊME heure : l'un pH seul, l'autre ORP seul. L'agrégat
  // HOURLY doit porter LES DEUX. Avec un écrasement (`=`), il ne porterait que
  // le dernier — et l'heure dirait « seulement du chlore » ou « seulement de
  // l'acide » selon l'ordre des points dans le créneau.
  const DoseCodec c = persistCodec();
  const uint8_t creneau[] = {kPh, kOrp};
  TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)(kPh | kOrp),
      foldGroup(creneau, 2, c),
      "l'agregat ne porte pas les deux voies : le dernier point ECRASE les autres");
  // Et l'ordre inverse donne le même résultat — une union est commutative,
  // un écrasement ne l'est pas.
  const uint8_t inverse[] = {kOrp, kPh};
  TEST_ASSERT_EQUAL_UINT8(foldGroup(creneau, 2, c), foldGroup(inverse, 2, c));
}

void test_agregation_point_herite_ne_se_fait_pas_ecraser(void) {
  // Un point relu d'un fichier ≤ 2.46 (0x04) agrégé avec un point pH (0x01) :
  // les DEUX bits, jamais l'un à la place de l'autre. C'est ce qui empêche un
  // dosage certain mais de voie inconnue de disparaître de l'agrégat DAILY, où
  // il resterait figé 75 à 90 jours.
  const DoseCodec c = persistCodec();
  const uint8_t creneau[] = {kUnknown, kPh};
  const uint8_t r = foldGroup(creneau, 2, c);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)(kUnknown | kPh), r,
      "le bit « voie indeterminee » disparait de l'agregat");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(kUnknown, r & kUnknown,
      "un dosage certain de voie inconnue est perdu a l'agregation");
  TEST_ASSERT_EQUAL_UINT8(kPh, r & kPh);
}

void test_agregation_creneau_sans_dosage_reste_a_zero(void) {
  // L'union n'INVENTE pas : douze points sans dosage font un agrégat nul.
  const DoseCodec c = persistCodec();
  uint8_t creneau[12];
  memset(creneau, 0, sizeof(creneau));
  TEST_ASSERT_EQUAL_UINT8(0, foldGroup(creneau, 12, c));
  // Un seul point dosant dans le créneau suffit à marquer l'heure — c'est le
  // « au moins un » d'avant, appliqué VOIE PAR VOIE.
  creneau[7] = kOrp;
  TEST_ASSERT_EQUAL_UINT8(kOrp, foldGroup(creneau, 12, c));
  TEST_ASSERT_EQUAL_UINT8(0, foldGroup(creneau, 12, c) & kPh);
}

void test_agregation_creneau_realiste_de_douze_points(void) {
  // Une heure de points RAW (cadence 5 min) : deux injections d'acide, une de
  // chlore, et un point hérité relu d'avant l'OTA.
  const DoseCodec c = persistCodec();
  const uint8_t heure[12] = {0, kPh, 0, 0, kPh, 0, kOrp, 0, 0, kUnknown, 0, 0};
  TEST_ASSERT_EQUAL_UINT8(kAllBits, foldGroup(heure, 12, c));
}

// =============================================================================
// 6 — BUDGET DE LA PARTITION `history` (critère d'acceptation de bug-098)
// =============================================================================
// Le mode de défaillance d'un dépassement n'est PAS la corruption mais le
// SILENCE : `historySaveAdmit()` refuse, un WARNING part une fois par heure, et
// /history.json cesse simplement d'être réécrit. Le chiffre doit donc être
// MESURÉ, pas postulé.

// Longueur de la clé « d » telle que PERSISTÉE, dans les deux formats.
static size_t doseKeyLen(unsigned value, int asMask) {
  char b[32];
  if (asMask) return (size_t)snprintf(b, sizeof(b), ",\"d\":%u", value);
  return (size_t)snprintf(b, sizeof(b), ",\"d\":%s", value ? "true" : "false");
}

void test_budget_le_masque_ne_coute_JAMAIS_plus_que_le_booleen(void) {
  // Un entier de 0 à 7 s'écrit sur UN chiffre ; `true`/`false` en prennent
  // quatre ou cinq. Le champ de bits est donc plus court sur la partition que
  // le booléen qu'il remplace — pour les huit valeurs atteignables.
  for (unsigned m = 0; m <= 7u; ++m) {
    char msg[96];
    snprintf(msg, sizeof(msg), "masque %u plus long que l'ancien booleen", m);
    TEST_ASSERT_TRUE_MESSAGE(doseKeyLen(m, 1) <= doseKeyLen(m, 0), msg);
  }
  TEST_ASSERT_EQUAL_size_t(6, doseKeyLen(0, 1));   // ,"d":0
  TEST_ASSERT_EQUAL_size_t(10, doseKeyLen(0, 0));  // ,"d":false
  TEST_ASSERT_EQUAL_size_t(6, doseKeyLen(7, 1));   // ,"d":7
  TEST_ASSERT_EQUAL_size_t(9, doseKeyLen(1, 0));   // ,"d":true
  // Même le pire cas hors contrat (3 chiffres) reste ≤ `false`.
  TEST_ASSERT_EQUAL_size_t(8, doseKeyLen(255, 1));
  TEST_ASSERT_TRUE(doseKeyLen(255, 1) < doseKeyLen(0, 0));
}

void test_budget_charge_realiste_de_339_points_retrecit(void) {
  // Les plafonds de constants.h : 96 RAW + 168 HOURLY + 75 DAILY. Régime
  // observé : les points RAW ne dosent pas la plupart du temps (`false` → 0),
  // les agrégats portent au moins une voie (`true` → un chiffre).
  const DoseCodec c = persistCodec();
  TEST_ASSERT_TRUE_MESSAGE(c.writeIsMask,
      "le budget ci-dessous ne vaut que si saveToFile ecrit bien le masque");

  size_t avant = 0, apres = 0;
  for (int i = 0; i < 96; ++i) { avant += doseKeyLen(0, 0); apres += doseKeyLen(0, 1); }
  for (int i = 0; i < 168; ++i) { avant += doseKeyLen(1, 0); apres += doseKeyLen(3, 1); }
  for (int i = 0; i < 75; ++i) { avant += doseKeyLen(1, 0); apres += doseKeyLen(7, 1); }

  // Mesuré : 3 147 o de clés « d » avant, 2 034 après.
  TEST_ASSERT_EQUAL_size_t(3147, avant);
  TEST_ASSERT_EQUAL_size_t(2034, apres);
  TEST_ASSERT_EQUAL_size_t(1113, avant - apres);
  TEST_ASSERT_TRUE_MESSAGE(apres < avant,
      "bug-098 ferait GROSSIR /history.json : le budget de partition doit etre "
      "rechiffre avant de livrer");
}

// =============================================================================

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  // 1 — contrat de bits
  RUN_TEST(test_contrat_valeurs_de_bits_figees);
  RUN_TEST(test_contrat_les_deux_booleens_ont_disparu_de_datapoint);
  RUN_TEST(test_contrat_enregistrement_n_intervertit_pas_les_deux_pompes);

  // 2 — politique lue dans les sources
  RUN_TEST(test_politique_ecriture_est_le_masque_pas_un_ou_logique);
  RUN_TEST(test_politique_discriminant_de_type_present_aux_deux_relectures);
  RUN_TEST(test_politique_le_masque_de_relecture_porte_LES_TROIS_BITS);
  RUN_TEST(test_politique_import_ne_reprend_pas_l_operateur_pipe_false);
  RUN_TEST(test_politique_export_passe_le_masque_tel_quel);
  RUN_TEST(test_politique_agregation_unit_bit_a_bit);

  // 3 — round-trip des trois formats
  RUN_TEST(test_roundtrip_fichier_neuf_toutes_les_voies);
  RUN_TEST(test_roundtrip_fichier_ancien_booleen_devient_voie_indeterminee);
  RUN_TEST(test_roundtrip_fichier_mixte_se_relit_point_par_point);
  RUN_TEST(test_roundtrip_bits_hors_contrat_sont_ignores_sans_erreur);

  // 4 — deux cycles consécutifs (le mutant clé : & 0x03)
  RUN_TEST(test_deux_cycles_consecutifs_conservent_la_voie_indeterminee);
  RUN_TEST(test_dix_cycles_ne_derivent_pas);

  // 5 — union bit à bit
  RUN_TEST(test_agregation_ph_seul_et_orp_seul_donnent_les_deux_bits);
  RUN_TEST(test_agregation_point_herite_ne_se_fait_pas_ecraser);
  RUN_TEST(test_agregation_creneau_sans_dosage_reste_a_zero);
  RUN_TEST(test_agregation_creneau_realiste_de_douze_points);

  // 6 — budget de partition
  RUN_TEST(test_budget_le_masque_ne_coute_JAMAIS_plus_que_le_booleen);
  RUN_TEST(test_budget_charge_realiste_de_339_points_retrecit);

  return UNITY_END();
}
