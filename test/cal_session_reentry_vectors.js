// Vecteurs de test de la RÉENTRÉE dans l'écran de calibration (bug-167,
// BLOCAGE 3 de la revue du 2026-10-16), extraits de `data/app.js`.
//
// Pourquoi ce fichier existe : `test/cal_points_acquired_vectors.js`
// (feature-159) couvre `_calPointsAcquired` — la décision PURE « ce point
// est-il acquis d'après l'historique de la sonde ? » — mais AUCUN harnais du
// dépôt ne couvrait `_calSessionAcquired`, le drapeau « ce point a-t-il été
// acquis DANS LA SESSION EN COURS ? » qui décide du GEL des boutons
// « Calibrer ». C'est très exactement ce qui a laissé passer le défaut du
// BLOCAGE 1 : aucun harnais ne rejouait une 2ᵉ entrée dans l'écran.
//
// Défaut couvert (reproduit en navigateur AVANT correctif, cf. notes
// techniques de bug-167 et le rapport de ce lot — `./tools/ui_shot.sh`,
// séquence trigger → calibrer → fermer → rouvrir, CAL_SETTLE_MS abaissé
// temporairement pour accélérer le règlement de l'attente réseau simulée,
// jamais commité) : `resetCalStepper()` DÉCIDE du gel en lisant
// `_calSessionAcquired[product]` — si ce drapeau n'est pas remis à zéro
// AVANT son appel, un point acquis lors d'une session précédente (via
// `completeCalStep`) le laisse à `true`, et la 2ᵉ ouverture de l'écran REGÈLE
// les DEUX boutons depuis l'historique de la sonde (`phCalPoints`/
// `orpCalPoints`), rendant toute calibration impossible — seule échappatoire,
// « Recommencer », qui efface aussi l'autre point. Corrigé en extrayant la
// remise à zéro dans `_calResetSessionState()`, appelée SYNCHRONE et EN
// PREMIER par `_calEnterScreen()` — AVANT `resetCalStepper()` — au lieu de
// vivre dans `_calSwitchEnter()` (async, dont la partie synchrone ne
// s'exécutait qu'APRÈS que `resetCalStepper()` ait déjà tranché).
//
// CE QUE CE FICHIER VISE (patron `test/ph_tune_remove_vectors.js`,
// feature-157 : bouchon DOM minimal, zéro dépendance npm) : il EXTRAIT le
// texte réel de `_calActiveIdx`, `_calFrozen`, `_calSessionAcquired`,
// `_calPtsKnownOnce` (état de module), `_calIsPtsKnown`, `_calPointsAcquired`,
// `_calReenableOnFirstKnownPts`, `resetCalStepper`, `completeCalStep`,
// `_calResetSessionState`, `_calEnterScreen` et `_calRefreshFrozenButtons`
// depuis `data/app.js` (ancre + comptage de profondeur `{}`,
// `test/js_extract.js`, ADR-0099), les assemble dans un SEUL bac à sable
// (même scope de module qu'en production — les fonctions référencent l'état
// en variables libres, exactement comme dans data/app.js), puis ÉVALUE ce
// texte extrait — jamais une recopie. Les dépendances RESTANTES
// (`_calResetStabBuffer`, `_calClearFailure`, `setCalStepState`,
// `renderStability`, `focusActiveStep`) sont bouchonnées (no-op, sauf
// `setCalStepState` qui tient `_calActiveIdx` à jour — nécessaire pour que
// `_calRefreshFrozenButtons` reste fidèle) : elles ne participent pas à la
// décision de gel, seulement à l'affichage du pas-à-pas et à la stabilité.
//
// Exécution : `node test/cal_session_reentry_vectors.js` (zéro dépendance
// npm, sort avec un code non nul si un vecteur échoue — utilisable en CI et
// en pre-commit local via `./tools/check_cal_session_reentry_vectors.sh`).
//
// Couverture demandée (revue bug-167, BLOCAGE 3) :
//   - 1ʳᵉ entrée, sonde déjà calibrée (pts=2 dès l'ouverture) : AUCUN gel —
//     mutation (a) : rétablir le gel sur `phCalPoints` (retirer la garde
//     `if (_calSessionAcquired[product])` dans `resetCalStepper`) doit TUER
//     ce vecteur ;
//   - acquisition d'un point dans la session (`completeCalStep`) : le
//     drapeau s'arme, le point se gèle ;
//   - 🔴 RÉENTRÉE (le vecteur qui a manqué) : fermeture (aucun effet sur les
//     drapeaux, comme le onExit réel) puis réouverture via `_calEnterScreen`
//     — LES DEUX points doivent être DÉGELÉS, identique à une toute première
//     ouverture — mutation (c) : appeler `resetCalStepper` directement (en
//     court-circuitant `_calEnterScreen`, donc SANS la remise à zéro
//     préalable) doit TUER ce vecteur ;
//   - `_calReenableOnFirstKnownPts` : réarme SANS regeler — mutation (b) :
//     réintroduire un `Object.assign(_calFrozen[...], _calPointsAcquired(...))`
//     dans cette fonction doit TUER le vecteur dédié ;
//   - le gel reste PAR PRODUIT : acquérir un point pH ne gèle rien côté ORP.

"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const { extractBalancedFromAnchor } = require("./js_extract");

// ---------------------------------------------------------------------
// Extraction du code RÉELLEMENT livré depuis data/app.js. Chaque ancre doit
// apparaître EXACTEMENT une fois (extractBalancedFromAnchor le garantit).
// ---------------------------------------------------------------------

const APP_JS_PATH = path.join(__dirname, "..", "data", "app.js");
const appJsSource = fs.readFileSync(APP_JS_PATH, "utf8");

function extract(anchor, openChar, closeChar) {
  return extractBalancedFromAnchor(appJsSource, anchor, openChar, closeChar).text;
}

const calActiveIdxSrc        = extract("const _calActiveIdx = {", "{", "}") + ";";
const calFrozenSrc           = extract("const _calFrozen = {", "{", "}") + ";";
const calSessionAcquiredSrc  = extract("const _calSessionAcquired = {", "{", "}") + ";";
const calPtsKnownOnceSrc     = extract("const _calPtsKnownOnce = {", "{", "}") + ";";
const isPtsKnownSrc          = extract("function _calIsPtsKnown(pts) {", "{", "}");
const pointsAcquiredSrc      = extract("function _calPointsAcquired(product, pts) {", "{", "}");
const reenableSrc            = extract("function _calReenableOnFirstKnownPts(json) {", "{", "}");
const resetSessionStateSrc   = extract("function _calResetSessionState() {", "{", "}");
const enterScreenSrc         = extract("function _calEnterScreen(product) {", "{", "}");
const resetCalStepperSrc     = extract("function resetCalStepper(product) {", "{", "}");
const completeCalStepSrc     = extract("function completeCalStep(product, idx) {", "{", "}");
const refreshFrozenBtnsSrc   = extract("function _calRefreshFrozenButtons() {", "{", "}");

// =============================================================================
// Bouchon DOM minimal : éléments en objets {disabled, hidden}, indexés par
// sélecteur "#id" — $(sel) retourne l'objet existant ou null, jamais
// undefined (comme querySelector). Même patron que ph_tune_remove_vectors.js.
// =============================================================================

function makeDom() {
  const elements = {
    "#btn-cal-ph-mid":    { disabled: false },
    "#btn-cal-ph-low":    { disabled: false },
    "#btn-cal-orp":       { disabled: false },
    "#cal-recal-ph-mid":  { hidden: true },
    "#cal-recal-ph-low":  { hidden: true },
    "#cal-recal-orp":     { hidden: true },
  };
  const dollar = (sel) =>
    Object.prototype.hasOwnProperty.call(elements, sel) ? elements[sel] : null;
  return { elements, dollar };
}

// Assemble TOUTES les fonctions/état extraits dans UN SEUL bac à sable — même
// scope de module qu'en production (chaque fonction référence l'état en
// variable libre, exactement comme dans data/app.js). Les dépendances
// RESTANTES sont bouchonnées : aucune ne participe à la décision de gel.
function buildSandbox() {
  const { elements, dollar } = makeDom();
  const latestSensorData = {};
  const factory = new Function("$", "latestSensorData", `
    "use strict";
    ${calActiveIdxSrc}
    ${calFrozenSrc}
    ${calSessionAcquiredSrc}
    ${calPtsKnownOnceSrc}
    ${isPtsKnownSrc}
    ${pointsAcquiredSrc}
    ${reenableSrc}
    ${resetSessionStateSrc}
    ${enterScreenSrc}
    ${refreshFrozenBtnsSrc}
    function _calResetStabBuffer(product) {}
    function _calClearFailure(product) {}
    function setCalStepState(product, idx) { _calActiveIdx[product] = idx; }
    function renderStability(product) {}
    function focusActiveStep(product) {}
    ${resetCalStepperSrc}
    ${completeCalStepSrc}
    return {
      _calActiveIdx, _calFrozen, _calSessionAcquired, _calPtsKnownOnce,
      _calIsPtsKnown, _calPointsAcquired, _calReenableOnFirstKnownPts,
      _calResetSessionState, _calEnterScreen, _calRefreshFrozenButtons,
      resetCalStepper, completeCalStep,
    };
  `);
  const sandbox = factory(dollar, latestSensorData);
  return { sandbox, elements, latestSensorData };
}

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
// 1ʳᵉ entrée, sonde déjà calibrée (pts=2 DÈS L'OUVERTURE, aucun geste dans
// cette session) : AUCUN gel — comportement attendu n°1 de la spec bug-167.
// Mutation (a) : rétablir le gel sur phCalPoints (retirer la garde
// `if (_calSessionAcquired[product])`) doit TUER ce vecteur.
// =============================================================================
{
  const { sandbox, elements, latestSensorData } = buildSandbox();
  latestSensorData.phCalPoints = 2;
  sandbox._calEnterScreen("ph");
  check("1ʳᵉ entrée, pts=2 : _calFrozen.ph.mid=false (pas de gel depuis l'historique)",
    sandbox._calFrozen.ph.mid, false);
  check("1ʳᵉ entrée, pts=2 : _calFrozen.ph.low=false", sandbox._calFrozen.ph.low, false);
  check("1ʳᵉ entrée, pts=2 : bouton mid NON désactivé par le gel",
    elements["#btn-cal-ph-mid"].disabled, false);
  check("1ʳᵉ entrée, pts=2 : bouton low NON désactivé par le gel",
    elements["#btn-cal-ph-low"].disabled, false);
}

// =============================================================================
// Acquisition d'un point DANS la session (completeCalStep) : le drapeau
// s'arme, et SEUL ce point se gèle (comportement attendu n°2).
// =============================================================================
{
  const { sandbox, elements, latestSensorData } = buildSandbox();
  latestSensorData.phCalPoints = 2;
  sandbox._calEnterScreen("ph");
  sandbox.completeCalStep("ph", 1); // étape "mid" franchie
  sandbox._calFrozen.ph.mid = true; // cf. checkCalAwait() réel (:10253, hors périmètre de ce harnais)
  sandbox._calRefreshFrozenButtons();
  check("acquisition mid : _calSessionAcquired.ph=true", sandbox._calSessionAcquired.ph, true);
  check("acquisition mid : bouton mid désactivé", elements["#btn-cal-ph-mid"].disabled, true);
  check("acquisition mid : lien « Recommencer » mid visible", elements["#cal-recal-ph-mid"].hidden, false);
  check("acquisition mid : point low PAS encore acquis -> _calFrozen.ph.low=false",
    sandbox._calFrozen.ph.low, false);
}

// =============================================================================
// 🔴 RÉENTRÉE — le vecteur qui a manqué avant ce lot. Fermeture (AUCUN effet
// sur les drapeaux — onExit réel ne les touche pas) puis réouverture via
// _calEnterScreen : LES DEUX points doivent être DÉGELÉS, identique à une
// toute première ouverture (comportement attendu n°3 de la spec bug-167).
// Mutation (c) : appeler resetCalStepper DIRECTEMENT (en court-circuitant
// _calEnterScreen, donc SANS remise à zéro préalable) doit TUER ce vecteur.
// =============================================================================
{
  const { sandbox, elements, latestSensorData } = buildSandbox();
  latestSensorData.phCalPoints = 2;
  sandbox._calEnterScreen("ph");           // 1ʳᵉ entrée
  sandbox.completeCalStep("ph", 1);        // acquiert le point mid
  sandbox._calFrozen.ph.mid = true;
  sandbox._calRefreshFrozenButtons();
  // --- Fermeture de l'écran : AUCUNE ligne ici, exactement comme onExit réel
  //     (phCalibrationActive=false, updatePhModeControls(), _calSwitchExit,
  //     _calStabStopPolling() — AUCUN ne touche _calSessionAcquired/_calFrozen).
  // --- Réouverture : le SEUL point d'entrée du pas-à-pas pH en production.
  sandbox._calEnterScreen("ph");
  check("🔴 réentrée : _calSessionAcquired.ph=false (remis à zéro par _calResetSessionState)",
    sandbox._calSessionAcquired.ph, false);
  check("🔴 réentrée : _calFrozen.ph.mid=false (dégelé, PAS regelé depuis phCalPoints=2)",
    sandbox._calFrozen.ph.mid, false);
  check("🔴 réentrée : _calFrozen.ph.low=false", sandbox._calFrozen.ph.low, false);
  check("🔴 réentrée : bouton mid RÉACTIVÉ", elements["#btn-cal-ph-mid"].disabled, false);
  check("🔴 réentrée : bouton low RÉACTIVÉ", elements["#btn-cal-ph-low"].disabled, false);
  check("🔴 réentrée : lien « Recommencer » mid de nouveau masqué",
    elements["#cal-recal-ph-mid"].hidden, true);
}

// =============================================================================
// Le gel reste PAR PRODUIT : acquérir un point pH ne gèle rien côté ORP
// (comportement attendu n°4 de la spec bug-167).
// =============================================================================
{
  const { sandbox, elements, latestSensorData } = buildSandbox();
  latestSensorData.phCalPoints = 2;
  latestSensorData.orpCalPoints = 1;
  sandbox._calEnterScreen("ph");
  sandbox._calEnterScreen("orp");
  sandbox.completeCalStep("ph", 1);
  sandbox._calFrozen.ph.mid = true;
  sandbox._calRefreshFrozenButtons();
  check("point pH acquis : _calSessionAcquired.orp reste false", sandbox._calSessionAcquired.orp, false);
  check("point pH acquis : _calFrozen.orp.orp reste false", sandbox._calFrozen.orp.orp, false);
  check("point pH acquis : bouton ORP NON désactivé", elements["#btn-cal-orp"].disabled, false);
}

// =============================================================================
// _calReenableOnFirstKnownPts : réarme SANS regeler. Mutation (b) :
// réintroduire un Object.assign(_calFrozen[...], _calPointsAcquired(...))
// dans cette fonction (l'ancien défaut, corrigé une 1ʳᵉ fois par bug-167)
// doit TUER ce vecteur.
// =============================================================================
{
  const { sandbox, elements } = buildSandbox();
  // Écran ouvert AVANT que /data ait répondu : resetCalStepper() a déjà
  // désactivé les boutons (état "inconnu", fail-closed, feature-159).
  elements["#btn-cal-ph-mid"].disabled = true;
  elements["#btn-cal-ph-low"].disabled = true;
  // `/data` répond ENFIN, avec une sonde déjà calibrée (2 points) — EXACTEMENT
  // le scénario qui a révélé le défaut historique : réarmer puis regeler
  // AUSSITÔT aurait rendu la calibration impossible pour toute la session.
  sandbox._calReenableOnFirstKnownPts({ phCalPoints: 2 });
  check("1er pts connu (sonde déjà calibrée) : bouton mid RÉARMÉ, PAS regelé",
    elements["#btn-cal-ph-mid"].disabled, false);
  check("1er pts connu : bouton low RÉARMÉ, PAS regelé", elements["#btn-cal-ph-low"].disabled, false);
  check("1er pts connu : _calFrozen.ph.mid reste false (rien ne l'a regelé)",
    sandbox._calFrozen.ph.mid, false);
  check("1er pts connu : _calPtsKnownOnce.ph=true (one-shot armé)", sandbox._calPtsKnownOnce.ph, true);

  // Un 2e `json` avec un pts différent ne doit RIEN réarmer de plus (one-shot).
  elements["#btn-cal-ph-mid"].disabled = true; // simulerait un gel réel entre-temps
  sandbox._calReenableOnFirstKnownPts({ phCalPoints: 1 });
  check("2e pts connu : one-shot déjà consommé -> bouton mid NON touché (reste désactivé)",
    elements["#btn-cal-ph-mid"].disabled, true);
}

if (failures > 0) {
  console.error(`\n${failures} vecteur(s) en échec.`);
  process.exit(1);
}
console.log("\nTous les vecteurs de réentrée dans l'écran de calibration sont conformes (code EXTRAIT de data/app.js, pas une copie).");
