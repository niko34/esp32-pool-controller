// Vecteurs de test de la logique PURE de l'APERÇU d'accord pH sur un test
// manuel (feature-155, revu par feature-168), extraite de `data/app.js`.
//
// Pourquoi ce fichier existe : l'écran « Correction manuelle de la mesure »
// (anciennement « offset pH ») laisse l'utilisateur AMENER un nombre à la
// valeur lue sur son test, et affiche un APERÇU en direct (ce que l'appareil
// lit, ce que dit le test, l'écart qui en résulte) AVANT que la décision
// réelle soit prise — côté SERVEUR, par `evaluatePhTune()`
// (`src/ph_tune_logic.h`), seule autorité sur le dosage (condition 1 du gate
// pool-chemistry). Un bug dans l'aperçu n'est pas cosmétique : c'est
// précisément un écart entre aperçu et serveur qui a révélé, dans le lot
// feature-155, que le refus OutOfRange serveur était MORT — trouvé par
// web-ui-developer en câblant l'aperçu, PAS par la relecture du firmware. Ce
// harnais existe pour que cette classe de défaut soit détectée par une
// machine, pas par hasard.
//
// 🔴 feature-168 — RÉÉCRITURE DE FOND (décision utilisateur du 2026-10-16) :
// la correction de mesure pH N'EST PLUS BORNÉE. `evaluatePhTunePreview()` ne
// refuse plus rien (« horsBornes » a disparu) : elle rend désormais DEUX
// signaux NON BLOQUANTS —
//   - inhabituel : |écart| > unusualAbs (l'ANCIENNE borne ±0,50, conservée
//     comme SIGNAL, plus un refus) ;
//   - impasse : l'écart pousserait le filet BRUT (fixe) à mordre AVANT la
//     consigne/le plafond CORRIGÉS — ecart > impasseThreshold + epsilon
//     (acidifier) OU ecart < impasseThresholdHigh − epsilon (alcaliniser,
//     symétrique C5bis, comparaison STRICTE des deux côtés).
// La SIGNATURE change en conséquence : plus d'offsetMin/offsetMax, mais
// offsetStep/unusualAbs/impasseThreshold/impasseThresholdHigh — tous SERVIS
// par /get-config (ADR-0100), jamais recopiés en dur côté client.
//
// 🔴 Correctif 2026-10-17 (fausse alerte pH EN PRODUCTION) : un offset de
// +1,20 pile a déclenché un CRITICAL firmware à tort (`impasseThreshold` est
// dérivé en float côté C, ≈1,1999998, pas 1,2 exact) — et l'écran affichait
// l'INVERSE (« Avertissement », faute de tolérance locale alors que le seuil
// qui lui est servi est arrondi à 1,2 pile). La signature gagne un 7e
// paramètre, `impasseEpsilon` — MÊME tolérance que `kPhImpasseEpsilon`
// (`ph_guard_logic.h`, = `kPhOffsetStep / 2`), DÉRIVÉE du pas servi par
// l'appelant (jamais calculée dans le corps testé : un `/2` y serait un
// littéral interdit par l'assertion structurelle ci-dessous).
//
// CE QUE CE FICHIER VISE (patron `test/sha256_vectors.js`, feature-118 ;
// `test/trace_logic_vectors.js`, feature-153 ; `test/ph_health_vectors.js`,
// feature-154) : il EXTRAIT le texte réel d'`evaluatePhTunePreview` depuis
// `data/app.js` (ancre + comptage de profondeur `{}` via `test/js_extract.js`,
// ADR-0099 amendé), puis ÉVALUE ce texte extrait — jamais une recopie.
//
// ⚠️ ASSERTION STRUCTURELLE (ADR-0100, reconduite depuis feature-154) : le
// texte extrait ne doit contenir AUCUN littéral numérique hors 0 et 1 — les
// seuils (pas, inhabituel, impasse) sont SERVIS par /get-config
// (ph_offset_step, ph_offset_unusual_abs, ph_offset_impasse_threshold,
// ph_offset_impasse_threshold_high), jamais réécrits en dur côté client.
//
// Exécution : `node test/ph_tune_vectors.js` (zéro dépendance npm, sort avec
// un code non nul si un vecteur échoue — utilisable en CI et en pre-commit
// local via `./tools/check_ph_tune_vectors.sh`).
//
// Couverture demandée (spec feature-168, critères d'acceptation + C8/C5bis) :
//   - mesure indisponible (phValueRaw non fini) ⇒ mesureIndisponible=true,
//     aucun autre champ signifiant (inhabituel/impasse à false, pas true) ;
//   - écart dans la zone calme (|ecart| <= unusualAbs) ⇒ inhabituel=false,
//     impasse=false ;
//   - 🔴 écart +0,51 (juste au-delà de l'ancienne borne ±0,50) :
//     inhabituel=true, impasse=false — SANS REFUS, contrairement à
//     feature-155 où ce même vecteur était hors bornes et bloquant ;
//   - écart EXACTEMENT à unusualAbs (+0,50/-0,50) : inhabituel=false (borne
//     incluse dans la zone calme, comparaison stricte `>`) ;
//   - au-delà de impasseThreshold + tolérance (+1,30) : impasse=true ET
//     inhabituel=true (l'impasse est TOUJOURS un sur-ensemble de l'inhabituel) ;
//   - 🔴 PILE à impasseThreshold (+1,20, cas de PRODUCTION) : impasse=false —
//     c'est tout l'objet du correctif du 2026-10-17 ;
//   - sous impasseThresholdHigh − tolérance (-1,00) : impasse=true (côté
//     alcalinisant, C5bis, comparaison STRICTE comme le côté bas) ;
//   - 🔴 PILE à impasseThresholdHigh (-0,90, symétrique du cas de PRODUCTION) :
//     impasse=false — même tolérance que le côté bas ;
//   - écart +0,80 : l'aperçu ne MASQUE rien — ecart et valeurAtteinte
//     reflètent la demande NON écrêtée (aucun bornage, feature-168) ;
//   - valeurAtteinte = phValueRaw + ecart, jamais valeurAmenee (Piège 1,
//     hérité de feature-155) ;
//   - 🔴 COHÉRENCE APERÇU ↔ FIRMWARE : pour un même jeu d'entrées, l'écart
//     calculé par l'aperçu coïncide avec celui que calculerait
//     evaluatePhTune() (réimplémenté ici en MIROIR minimal de la partie
//     arrondi, seule partie commune aux deux depuis que la salubrité de
//     domaine — seul refus restant côté firmware — est hors de portée du
//     pas-à-pas, qui ne peut déjà plus sortir de [0 ; 14]).

"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const { extractBalancedFromAnchor } = require("./js_extract");

// ---------------------------------------------------------------------
// Extraction du code RÉELLEMENT livré depuis data/app.js.
// ---------------------------------------------------------------------

const APP_JS_PATH = path.join(__dirname, "..", "data", "app.js");
const appJsSource = fs.readFileSync(APP_JS_PATH, "utf8");

const previewSource = extractBalancedFromAnchor(
  appJsSource,
  "function evaluatePhTunePreview(valeurAmenee, phValueRaw, offsetStep, unusualAbs, impasseThreshold, impasseThresholdHigh, impasseEpsilon) {",
  "{",
  "}"
).text;

// --- Évaluation du code EXTRAIT (jamais une recopie) ------------------------
const buildLive = new Function(`
  "use strict";
  ${previewSource}
  return { evaluatePhTunePreview };
`);
const { evaluatePhTunePreview } = buildLive();

let failures = 0;

function check(label, actual, expected) {
  try {
    assert.deepStrictEqual(actual, expected, label);
    console.log(`OK  ${label}`);
  } catch (e) {
    failures++;
    console.error("ÉCHEC", label, "—", e.message);
  }
}

function checkBool(label, actual, expected) {
  try {
    assert.strictEqual(actual, expected, label);
    console.log(`OK  ${label}`);
  } catch (e) {
    failures++;
    console.error("ÉCHEC", label, "—", e.message);
  }
}

function checkFloat(label, actual, expected, eps) {
  const e = eps || 1e-6;
  try {
    assert.ok(
      Math.abs(actual - expected) <= e,
      `${label} — attendu ${expected}, obtenu ${actual}`
    );
    console.log(`OK  ${label}`);
  } catch (e2) {
    failures++;
    console.error("ÉCHEC", label, "—", e2.message);
  }
}

// Seuils de référence, strictement ceux servis par /get-config en feature-168
// (constants.h : kPhOffsetUnusualAbs, kPhOffsetImpasseThreshold,
// kPhOffsetImpasseThresholdHigh) — jamais recopiés DANS la fonction testée,
// seulement dans ce fichier de vecteurs (qui, lui, A LE DROIT de connaître la
// valeur nominale pour construire ses cas).
const UNUSUAL_ABS = 0.5;
const IMPASSE_THRESHOLD = 1.2;
const IMPASSE_THRESHOLD_HIGH = -0.9;
const STEP = 0.1;
// Correctif 2026-10-17 : MÊME tolérance que kPhImpasseEpsilon
// (ph_guard_logic.h, = kPhOffsetStep / 2) — DÉRIVÉE du pas ci-dessus, jamais
// une valeur indépendante (c'est le pas SERVI qui fixe la tolérance côté
// firmware comme côté aperçu).
const IMPASSE_EPSILON = STEP / 2;

// =============================================================================
// Assertion structurelle (ADR-0100) : aucun littéral numérique hors 0 et 1
// dans le texte EXTRAIT.
// =============================================================================
{
  function stripStringsAndComments(src) {
    let out = "";
    let i = 0;
    while (i < src.length) {
      const c = src[i];
      if (c === '"' || c === "'" || c === "`") {
        const quote = c;
        i++;
        while (i < src.length && src[i] !== quote) {
          i += src[i] === "\\" ? 2 : 1;
        }
        i++;
        continue;
      }
      if (c === "/" && src[i + 1] === "/") {
        i += 2;
        while (i < src.length && src[i] !== "\n") i++;
        continue;
      }
      if (c === "/" && src[i + 1] === "*") {
        i += 2;
        while (i < src.length && !(src[i] === "*" && src[i + 1] === "/")) i++;
        i += 2;
        continue;
      }
      out += c;
      i++;
    }
    return out;
  }

  const codeOnly = stripStringsAndComments(previewSource);
  const numberRe = /\b\d+(?:\.\d+)?\b/g;
  const forbidden = [];
  let m;
  while ((m = numberRe.exec(codeOnly)) !== null) {
    if (m[0] !== "0" && m[0] !== "1") forbidden.push(m[0]);
  }
  checkBool(
    "evaluatePhTunePreview : aucun littéral numérique hors 0 et 1 dans le code extrait",
    forbidden.length === 0,
    true
  );
  if (forbidden.length > 0) {
    console.error("  littéraux trouvés :", forbidden.join(","));
  }
}

// =============================================================================
// Mesure indisponible — aucun autre champ n'est alors signifiant.
// =============================================================================

check(
  "phValueRaw NaN -> mesureIndisponible",
  evaluatePhTunePreview(7.4, NaN, STEP, UNUSUAL_ABS, IMPASSE_THRESHOLD, IMPASSE_THRESHOLD_HIGH, IMPASSE_EPSILON),
  { mesureIndisponible: true, ecart: null, inhabituel: false, impasse: false, valeurAtteinte: null }
);
check(
  "phValueRaw undefined -> mesureIndisponible",
  evaluatePhTunePreview(7.4, undefined, STEP, UNUSUAL_ABS, IMPASSE_THRESHOLD, IMPASSE_THRESHOLD_HIGH, IMPASSE_EPSILON),
  { mesureIndisponible: true, ecart: null, inhabituel: false, impasse: false, valeurAtteinte: null }
);
check(
  "phValueRaw chaîne -> mesureIndisponible",
  evaluatePhTunePreview(7.4, "7.13", STEP, UNUSUAL_ABS, IMPASSE_THRESHOLD, IMPASSE_THRESHOLD_HIGH, IMPASSE_EPSILON),
  { mesureIndisponible: true, ecart: null, inhabituel: false, impasse: false, valeurAtteinte: null }
);

// =============================================================================
// Zone calme : |ecart| <= unusualAbs ⇒ inhabituel=false, impasse=false.
// Bornes EXACTES ±0,50 incluses (comparaison stricte `>` dans la fonction).
// =============================================================================
{
  const r = evaluatePhTunePreview(7.5, 7.0, STEP, UNUSUAL_ABS, IMPASSE_THRESHOLD, IMPASSE_THRESHOLD_HIGH, IMPASSE_EPSILON);
  checkFloat("écart +0,50 (borne incluse) : ecart", r.ecart, 0.5);
  checkBool("écart +0,50 (borne incluse) : inhabituel=false", r.inhabituel, false);
  checkBool("écart +0,50 (borne incluse) : impasse=false", r.impasse, false);
}
{
  const r = evaluatePhTunePreview(6.5, 7.0, STEP, UNUSUAL_ABS, IMPASSE_THRESHOLD, IMPASSE_THRESHOLD_HIGH, IMPASSE_EPSILON);
  checkFloat("écart -0,50 (borne incluse) : ecart", r.ecart, -0.5);
  checkBool("écart -0,50 (borne incluse) : inhabituel=false", r.inhabituel, false);
  checkBool("écart -0,50 (borne incluse) : impasse=false", r.impasse, false);
}

// =============================================================================
// 🔴 Juste au-delà de la zone calme (ecart = 0,60, le pas suivant après la
// borne ±0,50 — pas 0,51 : le pas de 0,10 ne permet aucune valeur intermédiaire,
// l'arrondi au pas le plus proche collapserait 0,51 sur 0,50) : inhabituel=true,
// impasse=false — SANS REFUS (contrairement à feature-155, où ce même type de
// vecteur était un refus bloquant « hors bornes »).
// =============================================================================
{
  const r = evaluatePhTunePreview(7.6, 7.0, STEP, UNUSUAL_ABS, IMPASSE_THRESHOLD, IMPASSE_THRESHOLD_HIGH, IMPASSE_EPSILON);
  checkBool("écart +0,60 : inhabituel=true", r.inhabituel, true);
  checkBool("écart +0,60 : impasse=false", r.impasse, false);
  checkBool("écart +0,60 : mesureIndisponible=false (jamais un refus)", r.mesureIndisponible, false);
}
{
  const r = evaluatePhTunePreview(6.4, 7.0, STEP, UNUSUAL_ABS, IMPASSE_THRESHOLD, IMPASSE_THRESHOLD_HIGH, IMPASSE_EPSILON);
  checkBool("écart -0,60 : inhabituel=true", r.inhabituel, true);
  checkBool("écart -0,60 : impasse=false", r.impasse, false);
}

// =============================================================================
// Impasse basse (acidification) : ecart > impasseThreshold + epsilon, STRICT.
// impasse ⇒ TOUJOURS inhabituel (sur-ensemble).
//
// 🔴 Correctif 2026-10-17 : la borne EXACTE +1,20 est maintenant TESTABLE
// (elle ne l'était pas avant epsilon — un ecart "nominal 1,20" obtenu par
// arrondi au pas, 12 × 0,10, tombe très légèrement AU-DESSUS du littéral 1.2
// en IEEE-754 double précision, 12*0.1 !== 1.2 — une comparaison SANS marge
// aurait donc pu faire basculer ce vecteur selon le bruit de quantification).
// C'est EXACTEMENT le vecteur de PRODUCTION qui a motivé ce correctif :
// impasse DOIT être false ici, epsilon absorbant ce bruit des deux côtés.
// =============================================================================
{
  const r = evaluatePhTunePreview(8.1, 7.0, STEP, UNUSUAL_ABS, IMPASSE_THRESHOLD, IMPASSE_THRESHOLD_HIGH, IMPASSE_EPSILON);
  checkFloat("écart +1,10 (sous l'impasse basse) : ecart", r.ecart, 1.1, 1e-6);
  checkBool("écart +1,10 : impasse=false", r.impasse, false);
  checkBool("écart +1,10 : inhabituel=true", r.inhabituel, true);
}
{
  // 🔴 CAS DE PRODUCTION (2026-10-17) : offset +1,20 PILE sur le seuil —
  // DOIT rester impasse=false. Sans la tolérance `IMPASSE_EPSILON`, ce
  // vecteur bascule à true (bruit de quantification 12*0.1 > 1.2) : c'est
  // exactement le mutant à tuer en retirant la tolérance (cf. rapport).
  const r = evaluatePhTunePreview(8.2, 7.0, STEP, UNUSUAL_ABS, IMPASSE_THRESHOLD, IMPASSE_THRESHOLD_HIGH, IMPASSE_EPSILON);
  checkFloat("écart +1,20 (PILE au seuil, cas de production) : ecart", r.ecart, 1.2, 1e-6);
  checkBool("écart +1,20 (cas de production) : impasse=false (tolérance)", r.impasse, false);
  checkBool("écart +1,20 : inhabituel=true", r.inhabituel, true);
}
{
  const r = evaluatePhTunePreview(8.3, 7.0, STEP, UNUSUAL_ABS, IMPASSE_THRESHOLD, IMPASSE_THRESHOLD_HIGH, IMPASSE_EPSILON);
  checkFloat("écart +1,30 (au-delà de l'impasse basse, hors tolérance) : ecart", r.ecart, 1.3);
  checkBool("écart +1,30 : impasse=true", r.impasse, true);
  checkBool("écart +1,30 : inhabituel=true (impasse ⇒ inhabituel)", r.inhabituel, true);
}

// =============================================================================
// Impasse haute (alcalinisation, C5bis) : ecart < impasseThresholdHigh −
// epsilon, comparaison STRICTE — même forme que le côté bas depuis le
// correctif 2026-10-17 (avant : `<=`, asymétrie corrigée).
// =============================================================================
{
  const r = evaluatePhTunePreview(6.2, 7.0, STEP, UNUSUAL_ABS, IMPASSE_THRESHOLD, IMPASSE_THRESHOLD_HIGH, IMPASSE_EPSILON);
  checkFloat("écart -0,80 (sous l'impasse haute) : ecart", r.ecart, -0.8);
  checkBool("écart -0,80 : impasse=false", r.impasse, false);
  checkBool("écart -0,80 : inhabituel=true", r.inhabituel, true);
}
{
  // 🔴 CAS SYMÉTRIQUE DU CAS DE PRODUCTION : -0,90 PILE sur le seuil haut —
  // AVANT ce correctif, la comparaison `<=` en faisait un impasse=true
  // (même borne EXACTE qu'avant, assertion volontairement déplacée ici).
  // DOIT devenir impasse=false, même tolérance que le côté bas.
  const r = evaluatePhTunePreview(6.1, 7.0, STEP, UNUSUAL_ABS, IMPASSE_THRESHOLD, IMPASSE_THRESHOLD_HIGH, IMPASSE_EPSILON);
  checkFloat("écart -0,90 (PILE au seuil, symétrique du cas de production) : ecart", r.ecart, -0.9);
  checkBool("écart -0,90 (symétrique production) : impasse=false (tolérance)", r.impasse, false);
  checkBool("écart -0,90 : inhabituel=true", r.inhabituel, true);
}
{
  const r = evaluatePhTunePreview(6.0, 7.0, STEP, UNUSUAL_ABS, IMPASSE_THRESHOLD, IMPASSE_THRESHOLD_HIGH, IMPASSE_EPSILON);
  checkFloat("écart -1,00 (au-delà de l'impasse haute, hors tolérance) : ecart", r.ecart, -1.0);
  checkBool("écart -1,00 : impasse=true", r.impasse, true);
  checkBool("écart -1,00 : inhabituel=true", r.inhabituel, true);
}

// =============================================================================
// 🔴 Écart +0,80 : l'aperçu ne MASQUE rien — ecart et valeurAtteinte reflètent
// la demande NON écrêtée (aucun bornage, feature-168).
// =============================================================================
{
  const r = evaluatePhTunePreview(7.8, 7.0, STEP, UNUSUAL_ABS, IMPASSE_THRESHOLD, IMPASSE_THRESHOLD_HIGH, IMPASSE_EPSILON);
  checkFloat("écart +0,80 : ecart NON écrêté", r.ecart, 0.8);
  checkBool("écart +0,80 : inhabituel=true", r.inhabituel, true);
  checkBool("écart +0,80 : impasse=false (sous +1,20)", r.impasse, false);
  checkFloat("écart +0,80 : valeurAtteinte = phValueRaw + ecart", r.valeurAtteinte, 7.8);
}

// =============================================================================
// valeurAtteinte = phValueRaw + ecart, JAMAIS valeurAmenee (Piège 1).
// Référence 7,45 / lu 7,13 ⇒ ecart arrondi au pas le plus proche de 0,32,
// soit 0,30 (pasAmene = round(0,32/0,1) = round(3,2) = 3 -> 0,30).
// =============================================================================
{
  const r = evaluatePhTunePreview(7.45, 7.13, STEP, UNUSUAL_ABS, IMPASSE_THRESHOLD, IMPASSE_THRESHOLD_HIGH, IMPASSE_EPSILON);
  checkFloat("7,45/7,13 : ecart arrondi au pas", r.ecart, 0.3, 1e-4);
  checkBool("7,45/7,13 : inhabituel=false", r.inhabituel, false);
  checkFloat("7,45/7,13 : valeurAtteinte = 7,43 (PAS 7,45)", r.valeurAtteinte, 7.43, 1e-4);
}

// =============================================================================
// 🔴 COHÉRENCE APERÇU ↔ FIRMWARE : le MIROIR minimal de l'arrondi
// (roundPhOffsetToStep côté C) doit produire exactement le même `ecart` que
// l'aperçu, pour un même jeu d'entrées. C'est la partie commune restante aux
// deux depuis que la salubrité de domaine (seul refus firmware, |écart| <=
// 14) est hors de portée du pas-à-pas.
// =============================================================================
function mirrorRoundToStep(offsetDemande, pas) {
  const enPas = offsetDemande / pas;
  const enPasArrondi = (enPas < 0) ? -Math.round(-enPas) : Math.round(enPas);
  return enPasArrondi * pas;
}

const parityVectors = [
  { ref: 7.5, raw: 7.0 },    // borne incluse +0,50
  { ref: 6.5, raw: 7.0 },    // borne incluse -0,50
  { ref: 7.51, raw: 7.0 },   // juste au-delà de +0,50
  { ref: 6.49, raw: 7.0 },   // juste au-delà de -0,50
  { ref: 7.8, raw: 7.0 },    // 🔴 le vecteur historique de feature-155
  { ref: 8.3, raw: 7.0 },    // au-delà de l'impasse basse
  { ref: 6.1, raw: 7.0 },    // à l'impasse haute (borne exacte)
  { ref: 7.45, raw: 7.13 },  // cas nominal du gate
  { ref: 7.0, raw: 7.0 },    // écart nul
];

for (const v of parityVectors) {
  const preview = evaluatePhTunePreview(v.ref, v.raw, STEP, UNUSUAL_ABS, IMPASSE_THRESHOLD, IMPASSE_THRESHOLD_HIGH, IMPASSE_EPSILON);
  const mirrorEcart = mirrorRoundToStep(v.ref - v.raw, STEP);
  checkFloat(
    `parité aperçu/firmware (ref=${v.ref}, raw=${v.raw}) : ecart identique à l'arrondi C`,
    preview.ecart,
    mirrorEcart,
    1e-6
  );
}

if (failures > 0) {
  console.error(`\n${failures} vecteur(s) en échec.`);
  process.exit(1);
}
console.log("\nTous les vecteurs d'accord pH sont conformes (code EXTRAIT de data/app.js, pas une copie).");
