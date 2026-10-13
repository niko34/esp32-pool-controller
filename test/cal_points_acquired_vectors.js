// Vecteurs de test de la logique PURE « ce point de calibration est-il
// acquis ? » (feature-159, correctif bloquant du code-reviewer), extraite de
// `data/app.js`.
//
// Pourquoi ce fichier existe : le code-reviewer a rendu « Request changes »
// sur un défaut FAIL-OPEN atteignable en session réelle — `resetCalStepper()`
// ne lisait `phCalPoints`/`orpCalPoints` qu'une seule fois, et seulement
// `pts === -1` (EZO explicitement injoignable) désarmait les boutons
// « Calibrer » du pas-à-pas. Un `pts` non numérique (`undefined`/`null` —
// `/data` pas encore répondu, ou hors ligne) se comportait comme « non
// calibré » : TOUS les boutons restaient armés, y compris pour un point déjà
// acquis, pour toute la session. `_calPointsAcquired(product, pts)` est la
// décision PURE extraite pour corriger ce défaut : un `pts` inconnu ne doit
// RIEN autoriser (fail-closed), exactement comme un EZO injoignable.
//
// CE QUE CE FICHIER VISE (patron `test/ph_health_vectors.js`, feature-154 ;
// `test/ph_tune_remove_vectors.js`, feature-157) : il EXTRAIT le texte réel
// de `_calIsPtsKnown` ET `_calPointsAcquired` depuis `data/app.js` (ancre +
// comptage de profondeur `{}` via `test/js_extract.js`, ADR-0099), les
// assemble dans le MÊME bac à sable (`_calPointsAcquired` référence
// `_calIsPtsKnown` en variable libre, exactement comme dans data/app.js), et
// ÉVALUE ce texte extrait — jamais une recopie. Toute divergence future
// entre l'implémentation livrée et ce qui est testé est donc
// structurellement impossible : ce qui juge ici est le code qui tourne dans
// le navigateur.
//
// Exécution : `node test/cal_points_acquired_vectors.js` (zéro dépendance
// npm, sort avec un code non nul si un vecteur échoue — utilisable en CI et
// en pre-commit local via `./tools/check_cal_points_acquired_vectors.sh`).
//
// Couverture demandée (code-reviewer, feature-159) :
//   - `pts === -1` (EZO injoignable) ⇒ rien d'acquis ;
//   - `pts === undefined` et `pts === null` (le défaut d'aujourd'hui,
//     AVANT ce correctif) ⇒ rien d'acquis, comme -1 ;
//   - `pts === 0` ⇒ rien d'acquis ;
//   - `pts === 1` ⇒ pH : mid acquis, low NON acquis ;
//   - `pts === 2` ⇒ pH : les deux acquis ;
//   - ORP : `pts >= 1` ⇒ acquis.

"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const { extractBalancedFromAnchor } = require("./js_extract");

// ---------------------------------------------------------------------
// Extraction du code RÉELLEMENT livré depuis data/app.js (voir commentaire
// d'en-tête). Une seule ancre, qui doit apparaître EXACTEMENT une fois.
// ---------------------------------------------------------------------

const APP_JS_PATH = path.join(__dirname, "..", "data", "app.js");
const appJsSource = fs.readFileSync(APP_JS_PATH, "utf8");

const isPtsKnownSource = extractBalancedFromAnchor(
  appJsSource,
  "function _calIsPtsKnown(pts) {",
  "{",
  "}"
).text;

const calPointsAcquiredSource = extractBalancedFromAnchor(
  appJsSource,
  "function _calPointsAcquired(product, pts) {",
  "{",
  "}"
).text;

// --- Évaluation du code EXTRAIT (jamais une recopie) ------------------------
// `_calPointsAcquired` référence `_calIsPtsKnown` en variable libre (le MÊME
// scope de module que dans data/app.js) : les deux textes sont assemblés
// dans le MÊME bac à sable, comme `ph_tune_remove_vectors.js` le fait pour
// `_fmtPh` + `renderPhTuneRemoveRow`.
const buildLive = new Function(`
  "use strict";
  ${isPtsKnownSource}
  ${calPointsAcquiredSource}
  return { _calIsPtsKnown, _calPointsAcquired };
`);
const { _calIsPtsKnown, _calPointsAcquired } = buildLive();

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

// =============================================================================
// _calIsPtsKnown : frontière exacte « pts exploitable ? » — testée
// DIRECTEMENT (pas seulement via _calPointsAcquired) car c'est elle qui gate
// aussi le bouton déclencheur hors stepper (renderCalibrationStatus), et un
// élargissement de sa frontière (ex. `>= -1` au lieu de `>= 0`) ne changerait
// RIEN à la matrice _calPointsAcquired (les seuils >= 1/>= 2 restent faux
// pour -1) — seul un vecteur direct sur _calIsPtsKnown tue ce mutant.
// =============================================================================
check("_calIsPtsKnown(-1) -> inconnu (frontière EXACTE : -1 exclu)", _calIsPtsKnown(-1), false);
check("_calIsPtsKnown(0) -> connu", _calIsPtsKnown(0), true);
check("_calIsPtsKnown(undefined) -> inconnu", _calIsPtsKnown(undefined), false);
check("_calIsPtsKnown(null) -> inconnu", _calIsPtsKnown(null), false);
check("_calIsPtsKnown(\"2\") -> inconnu (chaîne, typeof !== \"number\")", _calIsPtsKnown("2"), false);
check("_calIsPtsKnown(NaN) -> inconnu", _calIsPtsKnown(NaN), false);

// =============================================================================
// pH : pts === -1 (EZO injoignable) ⇒ rien d'acquis.
// =============================================================================
check("pH pts=-1 -> rien acquis", _calPointsAcquired("ph", -1), { mid: false, low: false });

// =============================================================================
// pH : pts inconnu (undefined/null) ⇒ rien acquis, EXACTEMENT comme -1 —
// c'est le défaut corrigé par ce lot (fail-open avant correctif).
// =============================================================================
check("pH pts=undefined -> rien acquis (fail-closed, PAS fail-open)", _calPointsAcquired("ph", undefined), { mid: false, low: false });
check("pH pts=null -> rien acquis (fail-closed, PAS fail-open)", _calPointsAcquired("ph", null), { mid: false, low: false });

// =============================================================================
// pH : pts === 0 ⇒ rien acquis (aucun point calibré, mais connu).
// =============================================================================
check("pH pts=0 -> rien acquis", _calPointsAcquired("ph", 0), { mid: false, low: false });

// =============================================================================
// pH : pts === 1 ⇒ mid acquis, low NON acquis.
// =============================================================================
check("pH pts=1 -> mid acquis, low non", _calPointsAcquired("ph", 1), { mid: true, low: false });

// =============================================================================
// pH : pts === 2 ⇒ les deux acquis.
// =============================================================================
check("pH pts=2 -> les deux acquis", _calPointsAcquired("ph", 2), { mid: true, low: true });

// =============================================================================
// ORP : pts >= 1 ⇒ acquis ; -1/undefined/null/0 ⇒ non acquis.
// =============================================================================
check("ORP pts=-1 -> non acquis", _calPointsAcquired("orp", -1), { orp: false });
check("ORP pts=undefined -> non acquis (fail-closed)", _calPointsAcquired("orp", undefined), { orp: false });
check("ORP pts=null -> non acquis (fail-closed)", _calPointsAcquired("orp", null), { orp: false });
check("ORP pts=0 -> non acquis", _calPointsAcquired("orp", 0), { orp: false });
check("ORP pts=1 -> acquis", _calPointsAcquired("orp", 1), { orp: true });
check("ORP pts=2 -> acquis (au-delà de 1, toujours acquis)", _calPointsAcquired("orp", 2), { orp: true });

// =============================================================================
// Garde-fous additionnels : chaîne non numérique et NaN ne doivent jamais
// être traités comme acquis (même piège que pts inconnu).
// =============================================================================
check("pH pts=\"2\" (chaîne) -> rien acquis (typeof !== \"number\")", _calPointsAcquired("ph", "2"), { mid: false, low: false });
check("pH pts=NaN -> rien acquis (NaN >= 1 est faux)", _calPointsAcquired("ph", NaN), { mid: false, low: false });

if (failures > 0) {
  console.error(`\n${failures} vecteur(s) en échec.`);
  process.exit(1);
}
console.log("\nTous les vecteurs _calPointsAcquired sont conformes.");
