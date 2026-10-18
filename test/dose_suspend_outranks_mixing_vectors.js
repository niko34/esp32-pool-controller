// Vecteurs de test de la logique PURE de bug-172 (H5), extraite de
// `data/app.js` : `_doseSuspendOutranksMixing(code)`.
//
// Pourquoi ce fichier existe : l'utilisateur a vu « pause mélange : 207 s »
// affiché alors que la filtration était arrêtée. Cause racine (cf. spec
// bug-172) : le bloc de suspension globale d'`updateDosingCore()`
// (`src/pump_controller.cpp`) coupe les pompes et `return`-e AVANT
// `canDose()` pour QUATRE causes (filtration arrêtée, étalonnage en cours,
// stabilisation post-calibration, assistant de mesure du débit) — tant que
// l'une d'elles est active, `*_mix_remaining_s` / `*MixingDelayActive` ne
// sont plus recalculés et peuvent porter une valeur RÉMANENTE d'avant la
// suspension. `_doseSuspendOutranksMixing()` est la liste UNIQUE des codes
// (2 / 7 / 24 / 25) pour lesquels les DEUX points d'appel côté client
// (`dosingStabOrMixSegment()`, `_renderDosePill()`) doivent taire le solde
// mélange plutôt que l'afficher.
//
// Patron `test/ph_health_vectors.js` (feature-154) / `test/sha256_vectors.js`
// (feature-118) : ce fichier EXTRAIT le texte réel de la fonction depuis
// `data/app.js` (ancre + comptage de profondeur `{}` via
// `test/js_extract.js`, ADR-0099), puis ÉVALUE ce texte extrait — jamais une
// recopie. Toute divergence future entre l'implémentation livrée et ce qui
// est testé est donc structurellement impossible.
//
// Couverture :
//   - les quatre codes réels (2, 7, 24, 25) ⇒ true (cause dominante,
//     solde mélange à taire) ;
//   - codes voisins NON concernés (8 = Mélange en cours, le fait réel que ce
//     helper ne doit PAS taire, sinon un vrai mélange en cours disparaîtrait
//     de l'écran) ⇒ false ;
//   - 0 (aucun refus), 1, 3, 6, 10-14, 18, 250 (sentinelle de garde) ⇒ false ;
//   - undefined / null (information absente ou aucun refus) ⇒ false.
//
// Exécution : `node test/dose_suspend_outranks_mixing_vectors.js` (zéro dépendance
// npm, code de sortie non nul si un vecteur échoue — utilisable en CI et en
// pre-commit local via `./tools/check_dose_suspend_outranks_mixing_vectors.sh`).

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

const doseSuspendOutranksMixingSource = extractBalancedFromAnchor(
  appJsSource,
  "function _doseSuspendOutranksMixing(code) {",
  "{",
  "}"
).text;

// --- Évaluation du code EXTRAIT (jamais une recopie) ------------------------
const buildLive = new Function(`
  "use strict";
  ${doseSuspendOutranksMixingSource}
  return { _doseSuspendOutranksMixing };
`);
const { _doseSuspendOutranksMixing } = buildLive();

let failures = 0;

function check(label, actual, expected) {
  try {
    assert.strictEqual(actual, expected, label);
    console.log(`OK  ${label}`);
  } catch (e) {
    failures++;
    console.error("ÉCHEC", label, "—", e.message);
  }
}

// Les quatre codes réels de suspension globale (dosing_logic.h) — cause dominante
// possible, le solde mélange doit se taire.
check("code 2 (FiltrationOff) ⇒ prime sur le mélange", _doseSuspendOutranksMixing(2), true);
check("code 7 (StabilizationActive) ⇒ prime sur le mélange", _doseSuspendOutranksMixing(7), true);
check("code 24 (CalibrationSessionOpen) ⇒ prime sur le mélange", _doseSuspendOutranksMixing(24), true);
check("code 25 (FlowAssistSuspend) ⇒ prime sur le mélange", _doseSuspendOutranksMixing(25), true);

// Code 8 = « Mélange en cours » : c'est le fait RÉEL que ce helper ne doit
// jamais taire, sinon un vrai mélange en cours disparaîtrait de l'écran.
check("code 8 (Mélange en cours, fait réel) ⇒ ne prime pas", _doseSuspendOutranksMixing(8), false);

// Codes voisins non concernés.
[0, 1, 3, 6, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 250].forEach((code) => {
  check(`code ${code} ⇒ ne prime pas`, _doseSuspendOutranksMixing(code), false);
});

// Information absente / aucun refus — ne doit jamais devenir true par accident.
check("code undefined ⇒ ne prime pas", _doseSuspendOutranksMixing(undefined), false);
check("code null ⇒ ne prime pas", _doseSuspendOutranksMixing(null), false);

if (failures > 0) {
  console.error(`\n${failures} vecteur(s) en échec sur _doseSuspendOutranksMixing.`);
  process.exit(1);
}

console.log("\nTous les vecteurs _doseSuspendOutranksMixing sont conformes.");
process.exit(0);
