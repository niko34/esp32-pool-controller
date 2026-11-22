// Vecteurs de test du bouton « Supprimer la correction » et du formatage pH
// (feature-157), extraits de `data/app.js`.
//
// Pourquoi ce fichier existe : feature-157 a introduit deux choses qui ne
// sont couvertes par AUCUN harnais du dépôt avant ce lot —
//   1. `_fmtPh()` : c'est la fonction qui porte la feature elle-même (une
//      seule décimale, virgule française) ; elle est PARTAGÉE par plusieurs
//      écrans (bannière de correction caduque, carte de correction
//      manuelle, aide « pH cible », étiquettes du pas-à-pas…) — un futur lot
//      qui la remettrait à `toFixed(2)` pour un autre écran annulerait
//      feature-157 en silence, sans qu'aucun test ni aucune revue ne le
//      voie ;
//   2. `renderPhTuneRemoveRow()` (~ligne 2150) : condition d'affichage du
//      bouton « Supprimer la correction » et MASQUAGE MUTUEL avec la
//      bannière « correction caduque » (`ph_offset_stale`) — les deux
//      contrôles ne doivent jamais être visibles en même temps (B3,
//      feature-157), et un bouton de suppression ne doit JAMAIS apparaître
//      pour une correction inconnue (offset absent/non numérique traité
//      comme 0).
//
// CE QUE CE FICHIER VISE (patron `test/sha256_vectors.js`, feature-118 ;
// `test/trace_logic_vectors.js`, feature-153 ; `test/ph_health_vectors.js`,
// feature-154 ; `test/ph_tune_vectors.js`, feature-155) : il EXTRAIT le texte
// réel de `_fmtPh` et de `renderPhTuneRemoveRow` depuis `data/app.js` (ancre +
// comptage de profondeur `{}` via `test/js_extract.js`, ADR-0099), puis
// ÉVALUE ce texte extrait — jamais une recopie. Toute divergence future entre
// l'implémentation livrée et ce qui est testé est donc structurellement
// impossible : ce qui juge ici est le code qui tourne dans le navigateur.
//
// `renderPhTuneRemoveRow()` touche le DOM (`$("#...")`, `.hidden`,
// `.textContent`) : ce fichier fournit un bouchon `$` MINIMAL (objets
// `{hidden, textContent}` indexés par sélecteur), sans aucune dépendance npm
// — comme tous les harnais voisins. `renderPhTuneRemoveRow` référence aussi
// `_fmtPh` en variable libre : les deux textes extraits sont assemblés dans
// le MÊME bac à sable (`new Function("$", ...)`) pour que la fermeture
// fonctionne exactement comme dans `data/app.js` ; `_fmtPh` est en outre
// évalué SEUL (sans `$`) pour ses propres vecteurs, afin de ne pas mélanger
// les deux diagnostics en cas d'échec.
//
// ⚠️ CONSTAT, PAS UNE CORRECTION (hors mandat de ce lot — test-engineer ne
// corrige pas `data/app.js`) : `_fmtPh()` ne se défend PAS elle-même contre
// une valeur non finie. Vérifié en isolant la fonction : `_fmtPh(null)`
// retourne "0,0" (coïncidence de `Math.abs(null) === 0`, pas une garde),
// mais `_fmtPh(undefined)` et `_fmtPh(NaN)` retournent littéralement la
// chaîne "NaN", et `_fmtPh(Infinity)` retourne littéralement "Infinity". La
// garantie produit « jamais de NaN affiché » ne tient AUJOURD'HUI que parce
// que tous les appelants connus de `data/app.js` filtrent la valeur AVANT
// l'appel — notamment `renderPhTuneRemoveRow`, qui calcule son `offset` via
// `(typeof cfg?.ph_offset === "number" && isFinite(cfg.ph_offset)) ? ... : 0`
// (ligne ~2158) avant de le passer à `_fmtPh`. Les vecteurs ci-dessous
// documentent le COMPORTEMENT RÉEL de `_fmtPh` en isolation (y compris ce
// risque latent) ET vérifient séparément que `renderPhTuneRemoveRow` ne lui
// transmet jamais une valeur non finie. Un futur appel direct à `_fmtPh`
// sans un tel filtre réintroduirait "NaN"/"Infinity" à l'écran — signalé ici,
// non corrigé.
//
// Exécution : `node test/ph_tune_remove_vectors.js` (zéro dépendance npm,
// sort avec un code non nul si un vecteur échoue — utilisable en CI et en
// pre-commit local via `./tools/check_ph_tune_remove_vectors.sh`).
//
// Couverture demandée (feature-157) :
//   - `_fmtPh` : une décimale sur positif/négatif/nul, préfixe de signe
//     (second paramètre), séparateur décimal virgule, comportement RÉEL
//     (pas souhaité) sur null/NaN/undefined/Infinity (voir constat ci-dessus) ;
//   - `renderPhTuneRemoveRow` : offset nul (absent/0/non numérique/NaN/
//     Infinity) ⇒ ligne, bouton et conseil masqués ; offset non nul sans
//     bannière caduque ⇒ tout affiché ; offset non nul AVEC
//     `ph_offset_stale: true` ⇒ bouton AFFICHÉ (bug-201 (C49) : plus de masquage
//     mutuel, c'est désormais l'unique chemin destructif de la carte) ;
//     `ph_offset_stale` vrai mais pas strictement `=== true` (ex. `1`) ⇒
//     n'active PAS le masquage ; DOM partiellement absent ⇒ garde
//     défensive, aucun élément restant touché.

"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const { extractBalancedFromAnchor } = require("./js_extract");

// ---------------------------------------------------------------------
// Extraction du code RÉELLEMENT livré depuis data/app.js (voir commentaire
// d'en-tête). Deux ancres, chacune devant apparaître EXACTEMENT une fois.
// ---------------------------------------------------------------------

const APP_JS_PATH = path.join(__dirname, "..", "data", "app.js");
const appJsSource = fs.readFileSync(APP_JS_PATH, "utf8");

const fmtPhSource = extractBalancedFromAnchor(
  appJsSource,
  "function _fmtPh(v, signed) {",
  "{",
  "}"
).text;

// bug-201 : renderPhTuneRemoveRow désarme l'état de friction et n'écrit `hidden` que sur
// changement — ses dépendances sont extraites AUSSI, jamais bouchonnées (un bouchon
// ne prouverait rien du désarmement).
const fn2 = (sig) => extractBalancedFromAnchor(appJsSource, sig, "{", "}").text;
const depsSource = [
  fn2("function _fmt2(v) {"),
  fn2("function _setHiddenIfChanged(el, hidden) {"),
  fn2("function phTuneRemoveNeedsFriction(offset, unusualAbs) {"),
  fn2("function phTuneRemoveWarningParts(offset, ph, phValueRaw) {"),
  "let _phTuneRemoveArmed = false;",
  fn2("function _phTuneRemoveDisarm(restoreFocus) {"),
].join("\n");
if ((appJsSource.match(/^ *let _phTuneRemoveArmed = false;$/gm) || []).length !== 1) {
  throw new Error("déclaration `let _phTuneRemoveArmed` introuvable ou ambiguë dans data/app.js");
}

const renderRemoveRowSource = extractBalancedFromAnchor(
  appJsSource,
  "function renderPhTuneRemoveRow(cfg) {",
  "{",
  "}"
).text;

// --- Évaluation du code EXTRAIT (jamais une recopie) ------------------------

// _fmtPh seule, pour ses propres vecteurs (pas de $ nécessaire).
const buildFmtPhOnly = new Function(`
  "use strict";
  ${fmtPhSource}
  return { _fmtPh };
`);
const { _fmtPh } = buildFmtPhOnly();

// _fmtPh + renderPhTuneRemoveRow assemblées dans le MÊME bac à sable : la
// fonction référence `_fmtPh` en variable libre, exactement comme dans
// data/app.js où les deux fonctions partagent le même scope de module.
function buildRemoveRow(dollarStub) {
  const factory = new Function("$", `
    "use strict";
    ${fmtPhSource}
    ${depsSource}
    ${renderRemoveRowSource}
    return {
      renderPhTuneRemoveRow,
      phTuneRemoveNeedsFriction,
      phTuneRemoveWarningParts,
      armedFlag: () => _phTuneRemoveArmed,
      forceArm: () => { _phTuneRemoveArmed = true; },
    };
  `);
  return factory(dollarStub);
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
// Bouchon DOM minimal : éléments en objets {hidden, textContent}, indexés par
// sélecteur "#id" — $(sel) retourne l'objet existant ou null (comme
// querySelector), jamais undefined.
// =============================================================================

function makeDom(overrides) {
  const elements = Object.assign(
    {
      "#ph_tune_remove_btn": { hidden: true, textContent: "" },
      "#ph_tune_remove_hint": { hidden: true, textContent: "" },
    },
    overrides || {}
  );
  const dollar = (sel) =>
    Object.prototype.hasOwnProperty.call(elements, sel) && elements[sel] !== null
      ? elements[sel]
      : null;
  return { elements, dollar };
}

function renderWithCfg(cfg, overrides) {
  const { elements, dollar } = makeDom(overrides);
  const { renderPhTuneRemoveRow } = buildRemoveRow(dollar);
  renderPhTuneRemoveRow(cfg);
  return elements;
}

// =============================================================================
// _fmtPh : une décimale, signe, séparateur français — vecteurs positifs,
// négatifs, nul, zéro négatif (-0 < 0 est FAUX en JS — piège vérifié).
// =============================================================================

check("_fmtPh(7,456 ; non signé) -> une décimale, virgule", _fmtPh(7.456, false), "7,5");
check("_fmtPh(-7,456 ; non signé) -> signe moins toujours visible", _fmtPh(-7.456, false), "−7,5");
check("_fmtPh(-7,456 ; signé) -> signe moins (identique au non signé côté négatif)", _fmtPh(-7.456, true), "−7,5");
check("_fmtPh(7,456 ; signé) -> préfixe plus explicite", _fmtPh(7.456, true), "+7,5");
check("_fmtPh(0 ; non signé) -> aucun préfixe", _fmtPh(0, false), "0,0");
check("_fmtPh(0 ; signé) -> préfixe plus (0 n'est pas < 0)", _fmtPh(0, true), "+0,0");
check("_fmtPh(-0 ; signé) -> préfixe plus aussi (piège JS : -0 < 0 est faux)", _fmtPh(-0, true), "+0,0");
check("_fmtPh(1,25) -> arrondi à une décimale (1,3)", _fmtPh(1.25, false), "1,3");
check("_fmtPh(-1,25 ; signé) -> arrondi à une décimale, signe moins", _fmtPh(-1.25, true), "−1,3");
check("_fmtPh(0,049) -> arrondi vers le bas (0,0)", _fmtPh(0.049, false), "0,0");
check("_fmtPh(0,05) -> arrondi vers le haut (0,1)", _fmtPh(0.05, false), "0,1");

// =============================================================================
// Constat (hors mandat de ce lot) : comportement RÉEL de _fmtPh en isolation
// sur des valeurs non exploitables — voir le commentaire d'en-tête. Ces
// vecteurs documentent ce qui EST, pas ce qui devrait être.
// =============================================================================

check("_fmtPh(null) -> \"0,0\" (coïncidence Math.abs(null)===0, PAS une garde)", _fmtPh(null, false), "0,0");
check("_fmtPh(undefined) -> littéralement \"NaN\" (aucune garde interne)", _fmtPh(undefined, false), "NaN");
check("_fmtPh(NaN) -> littéralement \"NaN\" (aucune garde interne)", _fmtPh(NaN, false), "NaN");
check("_fmtPh(Infinity) -> littéralement \"Infinity\" (aucune garde interne)", _fmtPh(Infinity, false), "Infinity");
check("_fmtPh(Infinity ; signé) -> \"+Infinity\"", _fmtPh(Infinity, true), "+Infinity");
check("_fmtPh(-Infinity ; signé) -> \"−Infinity\"", _fmtPh(-Infinity, true), "−Infinity");

// =============================================================================
// renderPhTuneRemoveRow : offset nul (absent/0/non numérique/non fini) =>
// ligne, bouton et conseil MASQUÉS — jamais un bouton qui supprimerait une
// correction inconnue.
// =============================================================================

{
  const el = renderWithCfg({});
  check("offset absent -> bouton masqué", el["#ph_tune_remove_btn"].hidden, true);
  check("offset absent -> bouton masqué", el["#ph_tune_remove_btn"].hidden, true);
  check("offset absent -> conseil masqué", el["#ph_tune_remove_hint"].hidden, true);
}

{
  const el = renderWithCfg({ ph_offset: 0 });
  check("offset explicite 0 -> tout masqué", el["#ph_tune_remove_btn"].hidden, true);
  check("offset explicite 0 -> bouton masqué", el["#ph_tune_remove_btn"].hidden, true);
}

{
  const el = renderWithCfg({ ph_offset: "0.30" });
  check("offset non numérique (chaîne) -> traité comme 0 -> masqué", el["#ph_tune_remove_btn"].hidden, true);
  check("offset non numérique (chaîne) -> bouton masqué", el["#ph_tune_remove_btn"].hidden, true);
}

{
  const el = renderWithCfg({ ph_offset: NaN });
  check("offset NaN (type number non fini) -> traité comme 0 -> masqué", el["#ph_tune_remove_btn"].hidden, true);
}

{
  const el = renderWithCfg({ ph_offset: Infinity });
  check("offset Infinity (non fini) -> traité comme 0 -> masqué", el["#ph_tune_remove_btn"].hidden, true);
}

{
  const el = renderWithCfg(undefined);
  check("cfg undefined -> ne lève pas, masqué", el["#ph_tune_remove_btn"].hidden, true);
}

{
  const el = renderWithCfg(null);
  check("cfg null -> ne lève pas, masqué", el["#ph_tune_remove_btn"].hidden, true);
}

// =============================================================================
// Offset non nul, SANS bannière caduque -> ligne, bouton et conseil AFFICHÉS.
// =============================================================================

{
  const el = renderWithCfg({ ph_offset: 0.3 });
  check("offset 0,30 sans bannière caduque -> bouton affiché", el["#ph_tune_remove_btn"].hidden, false);
  check("offset 0,30 sans bannière caduque -> bouton affiché", el["#ph_tune_remove_btn"].hidden, false);
  check("offset 0,30 sans bannière caduque -> conseil affiché", el["#ph_tune_remove_hint"].hidden, false);
}

{
  const el = renderWithCfg({ ph_offset: -0.5 });
  check("offset -0,50 sans bannière caduque -> affiché", el["#ph_tune_remove_btn"].hidden, false);
}

{
  // ph_offset_stale vrai mais NON strictement === true (comparaison stricte
  // exigée par le code réel) -> ne doit PAS activer le masquage.
  const el = renderWithCfg({ ph_offset: 0.3, ph_offset_stale: 1 });
  check("ph_offset_stale=1 (pas === true) -> N'ACTIVE PAS le masquage -> affiché", el["#ph_tune_remove_btn"].hidden, false);
}

{
  const el = renderWithCfg({ ph_offset: 0.3, ph_offset_stale: "true" });
  check("ph_offset_stale=\"true\" (chaîne, pas === true) -> N'ACTIVE PAS le masquage -> affiché", el["#ph_tune_remove_btn"].hidden, false);
}

// =============================================================================
// bug-201 (C49) : ⛔ PLUS de masquage mutuel. Bandeau « correction à vérifier »
// affiché, « Supprimer la correction » RESTE visible : c'est désormais l'unique
// chemin destructif de la carte (« Remettre à zéro » a quitté le bandeau). Sans
// cela, le bandeau n'offrirait que « garder » — un bandeau qui demande de
// vérifier et n'offre que de conserver pousse à la confirmation sans vérification.
// =============================================================================

{
  const el = renderWithCfg({ ph_offset: 0.3, ph_offset_stale: true });
  check("bug-201 (C49) offset 0,30 + bandeau -> bouton AFFICHÉ", el["#ph_tune_remove_btn"].hidden, false);
  check("bug-201 (C49) offset 0,30 + bandeau -> conseil affiché", el["#ph_tune_remove_hint"].hidden, false);
}

{
  const el = renderWithCfg({ ph_offset: 0, ph_offset_stale: true });
  check("offset 0 + ph_offset_stale=true -> masqué (offset nul l'emporte)", el["#ph_tune_remove_btn"].hidden, true);
}

// =============================================================================
// bug-201 (C41) : seuil de friction. Lu depuis la clé SERVIE (paramètre), strict.
// =============================================================================

{
  const { phTuneRemoveNeedsFriction: f } = buildRemoveRow(makeDom().dollar);
  check("bug-201 (C41) |1,20| > 0,50 -> friction", f(1.2, 0.5), true);
  check("bug-201 (C41) |−0,60| > 0,50 -> friction (le signe ne compte pas)", f(-0.6, 0.5), true);
  check("bug-201 (C41) |0,50| = seuil -> AUCUNE friction (strict)", f(0.5, 0.5), false);
  check("bug-201 (C41) |−0,30| < seuil -> AUCUNE friction", f(-0.3, 0.5), false);
  check("bug-201 (C41) le seuil est LU : 0,60 -> 0,60 sans friction, 0,70 avec", [f(0.6, 0.6), f(0.7, 0.6)], [false, true]);
  check("bug-201 (C41) seuil non fini -> friction (échec côté prudent)", [f(0.3, NaN), f(0.3, undefined)], [true, true]);
}

// =============================================================================
// bug-201 (C45/C46) : texte de friction — deux variantes selon le SIGNE, les deux
// nombres lus (phValueRaw puis ph), repli sur le seul ph_offset.
// =============================================================================

{
  const { phTuneRemoveWarningParts: w } = buildRemoveRow(makeDom().dollar);
  const flat = (parts) => parts.map((p) => p.text).join("");
  const bold = (parts) => parts.filter((p) => p.strong).map((p) => p.text);
  const POS = "Sans cette correction, l'appareil lira 6,10 au lieu de 7,30 : il cessera d'ajouter de l'acide et laissera le pH monter. Pour la remettre, il faudra refaire un test manuel.";
  const NEG = "Sans cette correction, l'appareil lira 7,90 au lieu de 7,30 : il ajoutera de l'acide en trop et pourra arrêter le chlore en croyant le pH trop haut. Pour la remettre, il faudra refaire un test manuel.";
  check("bug-201 (C45) variante POSITIVE mot pour mot", flat(w(1.2, 7.30, 6.10)), POS);
  check("bug-201 (C46) positive : seuls les deux nombres sont en gras, phValueRaw d'abord", bold(w(1.2, 7.30, 6.10)), ["6,10", "7,30"]);
  check("bug-201 (C45) variante NÉGATIVE mot pour mot", flat(w(-0.6, 7.30, 7.90)), NEG);
  check("bug-201 (C45) la variante négative nomme l'acide en trop ET l'arrêt du chlore",
        [/acide en trop/.test(flat(w(-0.6, 7.3, 7.9))), /arrêter le chlore/.test(flat(w(-0.6, 7.3, 7.9)))], [true, true]);
  check("bug-201 (C45) la variante positive n'est JAMAIS servie à une correction négative",
        /cessera d'ajouter/.test(flat(w(-0.6, 7.3, 7.9))), false);
  check("bug-201 (C46) repli (ph null, phValueRaw null) positif",
        flat(w(1.2, null, null)), "Sans cette correction, l'appareil lira 1,20 pH de moins qu'aujourd'hui : il cessera d'ajouter de l'acide et laissera le pH monter. Pour la remettre, il faudra refaire un test manuel.");
  check("bug-201 (C46) repli négatif : « de plus »",
        flat(w(-0.6, null, null)), "Sans cette correction, l'appareil lira 0,60 pH de plus qu'aujourd'hui : il ajoutera de l'acide en trop et pourra arrêter le chlore en croyant le pH trop haut. Pour la remettre, il faudra refaire un test manuel.");
  check("bug-201 (C46) repli si UN SEUL des deux manque (ph seul)", flat(w(1.2, 7.3, null)).includes("1,20 pH de moins"), true);
  check("bug-201 (C46) repli si UN SEUL des deux manque (raw seul, NaN)", flat(w(1.2, NaN, 6.1)).includes("1,20 pH de moins"), true);
  check("bug-201 (C46) la friction ne contient jamais « NaN » ni « null » en repli",
        /NaN|null|undefined/.test(flat(w(1.2, undefined, NaN))), false);
}

// =============================================================================
// bug-201 (C47) : un état armé ne survit JAMAIS à un re-rendu / retour de /get-config.
// =============================================================================

{
  const box = { hidden: false, contains: () => false };
  const txt = { textContent: "x", replaceChildren() { this.textContent = ""; } };
  const { elements, dollar } = makeDom({
    "#ph_tune_remove_btn": { hidden: true, textContent: "" },
    "#ph_tune_remove_hint": { hidden: true, textContent: "" },
    "#ph_tune_remove_confirm": box,
    "#ph_tune_remove_confirm_text": txt,
  });
  const api = buildRemoveRow(dollar);
  api.forceArm();
  check("bug-201 (C47) état armé (préparation)", api.armedFlag(), true);
  api.renderPhTuneRemoveRow({ ph_offset: 1.2 });
  check("bug-201 (C47) re-rendu => désarmé", api.armedFlag(), false);
  check("bug-201 (C47) re-rendu => bloc refermé", box.hidden, true);
  check("bug-201 (C47) re-rendu => texte vidé", txt.textContent, "");
  check("bug-201 (C47) re-rendu => bouton de nouveau visible", elements["#ph_tune_remove_btn"].hidden, false);
}

// =============================================================================
// Garde défensive : DOM partiellement absent -> la fonction retourne SANS
// toucher aux éléments restants (aucune exception, aucun effet partiel).
// =============================================================================

{
  const sentinelHint = { hidden: false, textContent: "intact" };
  const { elements, dollar } = makeDom({
    "#ph_tune_remove_btn": null,
    "#ph_tune_remove_hint": sentinelHint,
  });
  const { renderPhTuneRemoveRow } = buildRemoveRow(dollar);
  renderPhTuneRemoveRow({ ph_offset: 0.5 });
  check("bouton absent -> conseil NON touché (retour anticipé)", elements["#ph_tune_remove_hint"].textContent, "intact");
}

if (failures > 0) {
  console.error(`\n${failures} vecteur(s) en échec.`);
  process.exit(1);
}
console.log("\nTous les vecteurs du bouton « Supprimer la correction » et de _fmtPh sont conformes (code EXTRAIT de data/app.js, pas une copie).");
