// Vecteurs de test de la logique PURE de la carte « Signaux capteurs »
// (feature-153, D3), extraite de `data/app.js`.
//
// Pourquoi ce fichier existe : feature-153 refond la carte « Signaux
// capteurs » de l'onglet Avancé pour qu'elle montre brut / médiane / lissé
// en Haute cadence, et la vue Journée regroupée. Trois fonctions PURES
// portent cette logique dans `data/app.js` — `groupHistoryPoints()`,
// `seriesStats()`, `traceWindowDurationMs()` — sans dépendance au DOM ni à
// `authFetch` (contrainte D3 de la spec). Un bug ici produit un trou mal
// placé, un extremum inventé, ou une durée annoncée fausse : silencieux à
// l'écran, visible seulement si on compare au comportement attendu.
//
// CE QUE CE FICHIER VISE (patron `test/sha256_vectors.js`, feature-118) :
// il EXTRAIT le texte réel des trois fonctions depuis `data/app.js`
// (recherche d'ancre + comptage de profondeur `{}` qui ignore chaînes et
// commentaires), puis ÉVALUE ce texte extrait — jamais une recopie. Toute
// divergence future entre l'implémentation livrée et ce qui est testé est
// donc structurellement impossible : ce qui tourne ICI est le code qui
// tourne dans le navigateur.
//
// Le scanner d'extraction (`findMatchingClose` / `extractBalancedFromAnchor`)
// vit désormais dans `test/js_extract.js` (feature-154, amendement ADR-0099) :
// ce fichier le REQUIERT au lieu d'en garder une copie locale — il en
// possédait une, strictement identique (au commentaire près) à celle de
// `test/sha256_vectors.js`, jusqu'à ce que ce doublon soit factorisé.
//
// Exécution : `node test/trace_logic_vectors.js` (zéro dépendance npm, sort
// avec un code non nul si un vecteur échoue — utilisable en CI et en
// pre-commit local via `./tools/check_trace_logic_vectors.sh`).
//
// Couverture demandée (spec feature-153, item D1) :
//   - groupHistoryPoints : fenêtres partielles, fenêtre entièrement vide
//     (aucun point produit, PAS un point à 0), bornes de fenêtre exactes,
//     points `null` ignorés SANS être comptés.
//   - seriesStats : `null` au milieu de la série ignoré ; n=0 -> tout null
//     (pas 0) ; n=1 -> min = max = moyenne.
//   - traceWindowDurationMs : changer `intervalMs` DOIT changer le résultat.

"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const { extractBalancedFromAnchor } = require("./js_extract");

// ---------------------------------------------------------------------
// Extraction du code RÉELLEMENT livré depuis data/app.js (voir commentaire
// d'en-tête). Trois ancres, chacune doit apparaître EXACTEMENT une fois.
// ---------------------------------------------------------------------

const APP_JS_PATH = path.join(__dirname, "..", "data", "app.js");
const appJsSource = fs.readFileSync(APP_JS_PATH, "utf8");

const groupHistoryPointsSource = extractBalancedFromAnchor(
  appJsSource,
  "function groupHistoryPoints(points, windowMs) {",
  "{",
  "}"
).text;

const seriesStatsSource = extractBalancedFromAnchor(
  appJsSource,
  "function seriesStats(values) {",
  "{",
  "}"
).text;

const traceWindowDurationMsSource = extractBalancedFromAnchor(
  appJsSource,
  "function traceWindowDurationMs(sampleCount, intervalMs) {",
  "{",
  "}"
).text;

// --- Évaluation du code EXTRAIT (jamais une recopie) ------------------------
const buildLive = new Function(`
  "use strict";
  ${groupHistoryPointsSource}
  ${seriesStatsSource}
  ${traceWindowDurationMsSource}
  return { groupHistoryPoints, seriesStats, traceWindowDurationMs };
`);
const { groupHistoryPoints, seriesStats, traceWindowDurationMs } = buildLive();

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
// groupHistoryPoints(points, windowMs)
// =============================================================================

// 1. Fenêtres partielles : deux points dans la même fenêtre de 600000 ms
// (10 min) sont moyennés ; un point isolé dans une fenêtre suivante forme
// son propre groupe.
{
  const points = [
    { timestamp: 0, ph: 7.0, orp: 600, granularity: 0 },
    { timestamp: 300000, ph: 7.2, orp: 620, granularity: 0 },
    { timestamp: 600000, ph: 7.4, orp: 640, granularity: 0 }
  ];
  const grouped = groupHistoryPoints(points, 600000);
  check("groupHistoryPoints : fenêtres partielles — nb groupes", grouped.length, 2);
  check("groupHistoryPoints : fenêtres partielles — moyenne pH du 1er groupe", grouped[0].ph, 7.1);
  check("groupHistoryPoints : fenêtres partielles — moyenne ORP du 1er groupe", grouped[0].orp, 610);
  check("groupHistoryPoints : fenêtres partielles — 2e groupe isolé", grouped[1].ph, 7.4);
}

// 2. Fenêtre entièrement vide ⇒ AUCUN point produit (pas un point à 0).
// Un grand écart temporel entre deux points ne doit PAS faire apparaître de
// groupe intermédiaire.
{
  const points = [
    { timestamp: 0, ph: 7.0, orp: 600, granularity: 0 },
    { timestamp: 36000000, ph: 7.3, orp: 610, granularity: 0 } // +10h, bien au-delà d'une fenêtre vide
  ];
  const grouped = groupHistoryPoints(points, 600000);
  check("groupHistoryPoints : fenêtre vide — aucun point inventé entre les deux", grouped.length, 2);
  check("groupHistoryPoints : fenêtre vide — aucune valeur à 0 injectée", grouped.some(g => g.ph === 0 || g.orp === 0), false);
}

// 3. Bornes de fenêtre : un timestamp exactement sur la borne (windowMs)
// appartient au groupe SUIVANT, pas au précédent (Math.floor(ts/windowMs)).
{
  const points = [
    { timestamp: 599999, ph: 7.0, granularity: 0 },
    { timestamp: 600000, ph: 8.0, granularity: 0 }
  ];
  const grouped = groupHistoryPoints(points, 600000);
  check("groupHistoryPoints : bornes — deux groupes distincts de part et d'autre de la borne", grouped.length, 2);
  check("groupHistoryPoints : bornes — groupe avant la borne", grouped[0].ph, 7.0);
  check("groupHistoryPoints : bornes — groupe à la borne", grouped[1].ph, 8.0);
}

// 4. Points `null` dans le tableau d'entrée ignorés SANS être comptés : ils
// ne doivent ni créer de groupe, ni altérer la moyenne du groupe voisin.
{
  const points = [
    { timestamp: 0, ph: 7.0, granularity: 0 },
    null,
    { timestamp: 1000, ph: 7.2, granularity: 0 },
    undefined
  ];
  const grouped = groupHistoryPoints(points, 600000);
  check("groupHistoryPoints : null ignorés — un seul groupe (fenêtre commune)", grouped.length, 1);
  check("groupHistoryPoints : null ignorés — moyenne non polluée", grouped[0].ph, 7.1);
}

// 5. Un champ sans aucune valeur finie dans son groupe rend `null`, jamais 0.
{
  const points = [{ timestamp: 0, ph: null, orp: 600, granularity: 0 }];
  const grouped = groupHistoryPoints(points, 600000);
  check("groupHistoryPoints : champ tout-null -> null (pas 0)", grouped[0].ph, null);
}

// =============================================================================
// seriesStats(values)
// =============================================================================

// 1. `null` AU MILIEU de la série ignoré (pas traité comme 0).
check(
  "seriesStats : null au milieu ignoré",
  seriesStats([7.0, null, 7.4]),
  { min: 7.0, max: 7.4, avg: 7.2, n: 2 }
);

// 2. n = 0 -> tout `null` (pas 0).
check(
  "seriesStats : série vide -> tout null",
  seriesStats([]),
  { min: null, max: null, avg: null, n: 0 }
);
check(
  "seriesStats : que des null -> tout null",
  seriesStats([null, null, undefined]),
  { min: null, max: null, avg: null, n: 0 }
);

// 3. n = 1 -> min = max = moyenne.
check(
  "seriesStats : une seule valeur -> min=max=avg",
  seriesStats([6.75]),
  { min: 6.75, max: 6.75, avg: 6.75, n: 1 }
);

// 4. Entrée non-tableau -> n=0, tout null (garde défensive).
check(
  "seriesStats : entrée non-tableau -> tout null",
  seriesStats(null),
  { min: null, max: null, avg: null, n: 0 }
);

// =============================================================================
// traceWindowDurationMs(sampleCount, intervalMs)
// =============================================================================

// 1. Changer `intervalMs` DOIT changer le résultat (c'est la propriété que
// feature-153 protège explicitement : la durée affichée se déduit de la
// cadence réelle, jamais codée en dur).
{
  const a = traceWindowDurationMs(200, 5000);
  const b = traceWindowDurationMs(200, 1000);
  check("traceWindowDurationMs : 200 éch. x 5000 ms", a, 1000000);
  check("traceWindowDurationMs : 200 éch. x 1000 ms", b, 200000);
  check("traceWindowDurationMs : changer intervalMs change le résultat", a !== b, true);
}

// 2. Entrées invalides/non positives -> 0, jamais une valeur inventée.
check("traceWindowDurationMs : sampleCount <= 0 -> 0", traceWindowDurationMs(0, 5000), 0);
check("traceWindowDurationMs : intervalMs <= 0 -> 0", traceWindowDurationMs(200, 0), 0);
check("traceWindowDurationMs : NaN -> 0", traceWindowDurationMs(NaN, 5000), 0);
check("traceWindowDurationMs : intervalMs non numérique -> 0", traceWindowDurationMs(200, "x"), 0);

if (failures > 0) {
  console.error(`\n${failures} vecteur(s) en échec.`);
  process.exit(1);
}
console.log("\nTous les vecteurs de la logique « Signaux capteurs » sont conformes (code EXTRAIT de data/app.js, pas une copie).");
