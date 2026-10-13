// Vecteurs de test de la statistique de stabilité de calibration (bug-160),
// extraite de `data/app.js`.
//
// Pourquoi ce fichier existe : feature-034 annonçait « Stable — vous pouvez
// calibrer » dès que `max − min` (une ÉTENDUE, qui croît avec le nombre
// d'échantillons et mélange bruit et dérive) passait sous 0,9 pH / 10 mV sur
// une fenêtre de 20 s. Le gate pool-chemistry a chiffré le défaut réel : à
// τ = 30 s (sonde saine), l'écart RÉEL au moment du vert atteignait 0,95 pH ;
// à τ = 60 s, 1,3 pH. `_calStabilityVerdict()` remplace cette étendue par une
// dérive MÉDIANE moitié récente / moitié ancienne, gardée par une INÉGALITÉ
// (`G ≥ G_MIN`) sur les temps RÉELLEMENT observés — jamais par une simple
// fenêtre nominale, qui ne garantit rien si la cadence varie.
//
// CE QUE CE FICHIER VISE (patron `test/cal_points_acquired_vectors.js`,
// feature-159 ; ADR-0099) : il EXTRAIT le texte réel de `CAL_STAB`,
// `_calStabilityVerdict` et `pushStabilitySample` depuis `data/app.js` (ancre
// + comptage de profondeur `{}` via `test/js_extract.js`) et ÉVALUE ce texte
// extrait — jamais une recopie. Toute divergence future entre
// l'implémentation livrée et ce qui est testé est donc structurellement
// impossible : ce qui juge ici est le code qui tourne dans le navigateur.
//
// Couverture demandée (bug-160, section 8) :
//   - rampes pH(t) = 4 + 3·e^(−t/τ) pour τ ∈ {10,30,60,120} s ⇒ jamais
//     « stable » tant que le résiduel RÉEL (3·e^(−t/τ)) dépasse 0,10 pH ;
//   - bruit pur sans dérive ⇒ « stable » (là où `max − min` disait
//     « instable » dès que le bruit dépassait l'ancien seuil) ;
//   - cadence dégradée 30 s ⇒ le verdict finit par être RENDU (n ≥ 6 suffit
//     à satisfaire G ≥ G_MIN à cette cadence, cf. dérivation de WINDOW) ;
//   - rafale 40 échantillons sur 18 s ⇒ G < G_MIN ⇒ MUET (coupure au
//     COMPTAGE seule, sans garde-fou temporel, laisserait passer ce cas) ;
//   - recalage d'horloge : `pushStabilitySample` n'utilise PLUS `Date.now()`
//     (vérifié sur le texte extrait) et `_calStabilityVerdict` est invariante
//     à un décalage constant de l'origine des temps (seules les séparations
//     RELATIVES comptent, jamais l'époque absolue) ;
//   - tampon borné à 256 échantillons, quel que soit l'âge ;
//   - WINDOW recontrôlée depuis τmax et la cadence dégradée (jamais choisie) ;
//   - régime du SECOND TAMPON (revue bloquante, 2026-10-13) : une rampe qui
//     repart d'un résidu MODESTE (électrode déjà partiellement équilibrée
//     après un changement d'étape) plutôt que de 3 pH — c'est LE régime où
//     G_MIN mord réellement (la rampe à 3 pH ne le fait pas, G y sature bien
//     après que D soit passé sous le seuil) ;
//   - priorité des SIX états et textes T-D1/T-D2 VERBATIM, EXÉCUTÉS sous un
//     DOM minimal mocké (`renderStability()` extraite telle quelle), pas
//     seulement déduits de la lecture du code.
//
// Exécution : `node test/cal_stability_vectors.js` (zéro dépendance npm, sort
// avec un code non nul si un vecteur échoue) — utilisable en CI et en
// pre-commit local via `./tools/check_cal_stability_vectors.sh`.

"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const { extractBalancedFromAnchor } = require("./js_extract");

// ---------------------------------------------------------------------
// Extraction du code RÉELLEMENT livré depuis data/app.js (voir commentaire
// d'en-tête). Chaque ancre doit apparaître EXACTEMENT une fois.
// ---------------------------------------------------------------------

const APP_JS_PATH = path.join(__dirname, "..", "data", "app.js");
const appJsSource = fs.readFileSync(APP_JS_PATH, "utf8");

const calStabSource = extractBalancedFromAnchor(
  appJsSource,
  "const CAL_STAB = {",
  "{",
  "}"
).text;

const calStabilityVerdictSource = extractBalancedFromAnchor(
  appJsSource,
  "function _calStabilityVerdict(product, samples, nowMs) {",
  "{",
  "}"
).text;

const pushStabilitySampleSource = extractBalancedFromAnchor(
  appJsSource,
  "function pushStabilitySample(product, value) {",
  "{",
  "}"
).text;

// renderStability() EXÉCUTÉE sous un DOM minimal MOCKÉ (revue bloquante,
// 2026-10-13 : « aucun vecteur n'épingle T-D1/T-D2 verbatim ni l'ordre de
// priorité des six états »). Extraite comme le reste, jamais recopiée.
const renderStabilitySource = extractBalancedFromAnchor(
  appJsSource,
  "function renderStability(product) {",
  "{",
  "}"
).text;

const calDiagLinesSource = extractBalancedFromAnchor(
  appJsSource,
  "function _calDiagLines(product, verdict) {",
  "{",
  "}"
).text;

const calRenderStabilityLinesSource = extractBalancedFromAnchor(
  appJsSource,
  "function _calRenderStabilityLines(el, text) {",
  "{",
  "}"
).text;

// Constante scalaire (pas de bloc `{}` à extraire par comptage de
// profondeur) — lue par une ancre de texte simple, directement dans la
// source réelle, jamais recopiée à la main.
function extractConstNumber(src, name) {
  const re = new RegExp("const\\s+" + name + "\\s*=\\s*([0-9.]+)\\s*;");
  const m = src.match(re);
  if (!m) throw new Error(`Constante introuvable dans data/app.js : ${name}`);
  return Number(m[1]);
}
const CAL_STAB_MAX_SAMPLES = extractConstNumber(appJsSource, "CAL_STAB_MAX_SAMPLES");
const CAL_STAB_DIAG_MS = extractConstNumber(appJsSource, "CAL_STAB_DIAG_MS");

// `CAL_STAB_DIAG_MIN` est une EXPRESSION (`CAL_STAB_DIAG_MS / 60000`), pas un
// scalaire — capturée telle quelle (jamais recalculée en parallèle).
function extractStatementRhs(src, name) {
  const re = new RegExp("const\\s+" + name + "\\s*=\\s*([^;]+);");
  const m = src.match(re);
  if (!m) throw new Error(`Instruction introuvable dans data/app.js : ${name}`);
  return m[1];
}
const calStabDiagMinExpr = extractStatementRhs(appJsSource, "CAL_STAB_DIAG_MIN");

// --- Évaluation du code EXTRAIT (jamais une recopie) ------------------------
// `pushStabilitySample` référence `performance.now()`, `_calStab` et
// `_calStabSessionStart` en variables libres (le MÊME scope de module que
// dans data/app.js) : on fournit un `performance` MOCK (horloge pilotée par
// le test, jamais par le système — c'est `_calStabilityVerdict`/les vecteurs
// « rampe » qui vérifient le COMPORTEMENT dans le temps, pas cette horloge).
const buildLive = new Function(`
  "use strict";
  let _mockNowMs = 0;
  const performance = { now: () => _mockNowMs };
  ${calStabSource}
  const CAL_STAB_MAX_SAMPLES = ${CAL_STAB_MAX_SAMPLES};
  const _calStab = { ph: [], orp: [] };
  const _calStabSessionStart = { ph: null, orp: null };
  ${pushStabilitySampleSource}
  ${calStabilityVerdictSource}
  return {
    CAL_STAB,
    _calStabilityVerdict,
    pushStabilitySample,
    setMockNowMs: (v) => { _mockNowMs = v; },
    getBuffer: (p) => _calStab[p],
    resetBuffer: (p) => { _calStab[p] = []; _calStabSessionStart[p] = null; },
  };
`);
const { CAL_STAB, _calStabilityVerdict, pushStabilitySample, setMockNowMs, getBuffer, resetBuffer } = buildLive();

// --- Second bac à sable : renderStability() sous un DOM MINIMAL MOCKÉ ------
// (revue bloquante : priorité des six états + verbatim T-D1/T-D2). Les
// éléments ciblés par `$` sont des objets PLATS (classList = Set, textContent
// avec effacement des enfants à l'écriture, comme le DOM réel) — juste assez
// pour que le code EXTRAIT de `renderStability`/`_calRenderStabilityLines`
// s'exécute sans modification.
function makeFakeEl() {
  const classes = new Set();
  const el = {
    _children: [],
    _text: "",
    get textContent() { return this._text; },
    set textContent(v) { this._text = v; this._children = []; },
    classList: {
      add: (...cs) => cs.forEach((c) => classes.add(c)),
      remove: (...cs) => cs.forEach((c) => classes.delete(c)),
      contains: (c) => classes.has(c),
    },
    appendChild(child) { this._children.push(child); },
    _classes: classes,
  };
  return el;
}

const buildRenderSandbox = new Function(`
  "use strict";
  let _mockNowMs2 = 0;
  const performance = { now: () => _mockNowMs2 };
  const document = { createElement: () => ({ textContent: "" }) };
  ${calStabSource}
  ${calStabilityVerdictSource}
  const CAL_STAB_DIAG_MS = ${CAL_STAB_DIAG_MS};
  const CAL_STAB_DIAG_MIN = ${calStabDiagMinExpr};
  ${calDiagLinesSource}
  ${calRenderStabilityLinesSource}
  const _calStab = { ph: [], orp: [] };
  const _calStabSessionStart = { ph: null, orp: null };
  const _calActiveIdx = { ph: 0, orp: 0 };
  let _calAwait = null;
  const _calFailure = { ph: { mid: null, low: null }, orp: { orp: null } };
  const _elements = {};
  const $ = (sel) => _elements[sel];
  ${renderStabilitySource}
  return {
    renderStability,
    setMockNowMs: (v) => { _mockNowMs2 = v; },
    setActiveIdx: (product, idx) => { _calActiveIdx[product] = idx; },
    setAwait: (v) => { _calAwait = v; },
    setFailure: (product, step, msg) => { _calFailure[product][step] = msg; },
    registerEl: (sel, el) => { _elements[sel] = el; },
    pushSample: (product, t, v) => { _calStab[product].push({ t, v }); },
    setSessionStart: (product, t) => { _calStabSessionStart[product] = t; },
    getStab: (product) => _calStab[product],
  };
`);

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
function ok(label, cond) {
  if (cond) { console.log(`OK  ${label}`); }
  else { failures++; console.error("ÉCHEC", label); }
}

// =============================================================================
// 1) Table CAL_STAB — valeurs GO du gate pool-chemistry, WINDOW recontrôlée
//    depuis τmax et la cadence dégradée (JAMAIS choisie indépendamment).
// =============================================================================
// Revue bloquante (2026-10-13) : G_MIN est désormais PROPRE À CHAQUE PRODUIT
// (`cfg.G_MIN`, utilisé par `_calStabilityVerdict`) — plus un G_MIN partagé
// dérivé de `ph` SEUL, qui n'aurait pas suivi un futur changement du ratio
// ORP. Les deux DOIVENT néanmoins rester égaux (le facteur de Nernst
// s'annule) : épinglé EXPLICITEMENT, pas supposé par lecture du code.
check("CAL_STAB.ph.G_MIN = SEUIL/RÉSIDUEL_MAX (pH)", CAL_STAB.ph.G_MIN, CAL_STAB.ph.THRESHOLD / CAL_STAB.ph.RESIDUAL_MAX);
check("CAL_STAB.orp.G_MIN = SEUIL/RÉSIDUEL_MAX (ORP)", CAL_STAB.orp.G_MIN, CAL_STAB.orp.THRESHOLD / CAL_STAB.orp.RESIDUAL_MAX);
check("CAL_STAB.ph.G_MIN === CAL_STAB.orp.G_MIN (identité PROUVÉE, pas supposée)", CAL_STAB.ph.G_MIN, CAL_STAB.orp.G_MIN);
ok("CAL_STAB.ph.G_MIN ≈ 0,5 (gate, aucun degré de liberté)", Math.abs(CAL_STAB.ph.G_MIN - 0.5) < 1e-9);
ok("CAL_STAB.MIN_SAMPLES = 6 (SOURCE UNIQUE, cf. revue bloquante : deux littéraux indépendants avant correctif)", CAL_STAB.MIN_SAMPLES === 6);
ok("CAL_STAB.HARD_AGE_FACTOR déclaré (ADR-0098, source « produit »)", typeof CAL_STAB.HARD_AGE_FACTOR === "number" && CAL_STAB.HARD_AGE_FACTOR > 0);
check("CAL_STAB.ph.HARD_AGE_MS = WINDOW_MS × HARD_AGE_FACTOR", CAL_STAB.ph.HARD_AGE_MS, CAL_STAB.ph.WINDOW_MS * CAL_STAB.HARD_AGE_FACTOR);
check("CAL_STAB.ph.WINDOW_MS = TAU_MAX_MS(ph) − SLOW_CADENCE_MS", CAL_STAB.ph.WINDOW_MS, CAL_STAB.ph.TAU_MAX_MS - CAL_STAB.SLOW_CADENCE_MS);
check("CAL_STAB.orp.WINDOW_MS = TAU_MAX_MS(orp) − SLOW_CADENCE_MS", CAL_STAB.orp.WINDOW_MS, CAL_STAB.orp.TAU_MAX_MS - CAL_STAB.SLOW_CADENCE_MS);
ok("CAL_STAB.ph.WINDOW_MS = 90 000 ms (valeur GO du gate)", CAL_STAB.ph.WINDOW_MS === 90000);
ok("CAL_STAB.orp.WINDOW_MS = 150 000 ms (valeur GO du gate)", CAL_STAB.orp.WINDOW_MS === 150000);
ok("CAL_STAB.orp.THRESHOLD = 0,05 × Nernst (jamais un littéral 3,0)", Math.abs(CAL_STAB.orp.THRESHOLD - 0.05 * CAL_STAB.NERNST_MV_PER_PH) < 1e-9);

// =============================================================================
// 2) Rampes pH(t) = 4 + 3·e^(−t/τ) — jamais « stable » tant que le résiduel
//    RÉEL (3·e^(−t/τ)) dépasse 0,10 pH. ⚠️ À matérialiser sur le code AVANT
//    correctif : échoue à τ = 30 s et τ = 60 s avec l'ancien seuil 0,9/20 s
//    (cf. section mutation plus bas, qui le redémontre explicitement).
// =============================================================================
// Utilise le PIPELINE RÉEL (pushStabilitySample, retention/purge comprises)
// — pas une accumulation maison sans purge, qui laverait le transitoire trop
// tôt et rendrait ce vecteur insensible à WINDOW/G_MIN (vérifié par mutation,
// cf. plus bas : une accumulation non purgée ne tue PAS le mutant G_MIN=0,4).
function simulateRamp(tau_s, cadenceMs, totalMs) {
  const tauMs = tau_s * 1000;
  resetBuffer("ph");
  let firstViolation = null;
  for (let t = 0; t <= totalMs; t += cadenceMs) {
    const residual = 3 * Math.exp(-t / tauMs);
    setMockNowMs(t);
    pushStabilitySample("ph", 4 + residual);
    const verdict = _calStabilityVerdict("ph", getBuffer("ph"), t);
    if (residual > 0.10 && verdict.state === "stable" && firstViolation === null) {
      firstViolation = { t, residual, verdict };
    }
  }
  return firstViolation;
}
[10, 30, 60, 120].forEach((tau) => {
  // Horizon large (8·τ) pour couvrir tout le transitoire, cadence 5 s (C9).
  const violation = simulateRamp(tau, 5000, tau * 1000 * 8);
  ok(`rampe τ=${tau}s : jamais "stable" tant que le résiduel > 0,10 pH`
     + (violation ? ` (violation à t=${violation.t}ms, résiduel=${violation.residual.toFixed(3)})` : ""),
     violation === null);
});

// =============================================================================
// 2bis) RÉGIME DU SECOND TAMPON (revue bloquante, 2026-10-13) — celui que la
//    rampe 3 pH ci-dessus NE COUVRE PAS. Un changement d'étape vide le tampon
//    (`_calResetStabBuffer`) mais l'électrode est déjà PARTIELLEMENT
//    équilibrée : la rampe repart d'un résidu modeste, pas de 3 pH. Avec un
//    grand résidu initial, D reste très au-dessus du seuil pendant toute la
//    phase où G traverse [0,4 ; 0,5) — le mutant G_MIN=0,4 ne mord jamais
//    (vérifié : il sature G≈0,61 bien après que D soit déjà passé sous le
//    seuil). Ici R0 = 0,1932 pH, τ = τmax = 120 s, cadence 5 s (C9) : à
//    n = 15 (t = 70 s), G = SEUIL/G_MIN exactement à la frontière du gate
//    (G ≈ 0,448 < 0,5) alors que D est DÉJÀ sous 0,05 et le résiduel réel
//    (0,1078 pH) dépasse encore 0,10 — c'est exactement le cas que G_MIN
//    protège, PAS un montage taillé sur le mutant : c'est le régime que
//    l'utilisateur rencontre à CHAQUE étalonnage, à la deuxième étape.
// =============================================================================
{
  const R0 = 0.1932, tauMs = 120000, cadenceMs = 5000, n = 15;
  resetBuffer("ph");
  let atN15 = null;
  for (let k = 0; k < n; k++) {
    const t = k * cadenceMs;
    setMockNowMs(t);
    pushStabilitySample("ph", 7.00 + R0 * Math.exp(-t / tauMs));
    const verdict = _calStabilityVerdict("ph", getBuffer("ph"), t);
    if (k === n - 1) atN15 = { t, verdict, Rreal: R0 * Math.exp(-t / tauMs) };
  }
  ok(`second tampon, n=15/t=70s : résiduel réel > 0,10 pH (R=${atN15.Rreal.toFixed(4)})`, atN15.Rreal > 0.10);
  ok(`second tampon, n=15/t=70s : G < G_MIN livré (G=${atN15.verdict.G.toFixed(4)} < ${CAL_STAB.ph.G_MIN})`, atN15.verdict.G < CAL_STAB.ph.G_MIN);
  ok("second tampon, n=15/t=70s ⇒ MUET avec le G_MIN livré (protège le résiduel > 0,10 pH)", atN15.verdict.state === "muted");
}

// =============================================================================
// 3) Bruit pur sans dérive ⇒ « stable » — là où `max − min` (ancien critère)
//    aurait pu dire « instable » dès que l'amplitude crête-à-crête dépassait
//    le seuil, malgré une médiane parfaitement constante.
// =============================================================================
{
  const cadenceMs = 5000;
  const samples = [];
  for (let i = 0; i < 20; i++) {
    samples.push({ t: i * cadenceMs, v: 7.00 + (i % 2 === 0 ? 0.03 : -0.03) });
  }
  const verdict = _calStabilityVerdict("ph", samples, 19 * cadenceMs);
  ok("bruit pur sans dérive ⇒ verdict RENDU (pas muet)", verdict.state !== "muted");
  ok("bruit pur sans dérive ⇒ stable", verdict.state === "stable");
}

// =============================================================================
// 4) Cadence dégradée 30 s (sans WebSocket) ⇒ le verdict finit par être
//    RENDU — jamais d'état muet permanent (n = 6 suffit à cette cadence,
//    cf. dérivation de WINDOW depuis τmax et SLOW_CADENCE_MS).
// =============================================================================
{
  const cadenceMs = 30000;
  const samples = [];
  for (let i = 0; i < 6; i++) samples.push({ t: i * cadenceMs, v: 7.00 });
  const verdict = _calStabilityVerdict("ph", samples, 5 * cadenceMs);
  ok("cadence 30 s, n=6 ⇒ verdict RENDU (G ≥ G_MIN)", verdict.state !== "muted");
}

// =============================================================================
// 5) Rafale : 40 échantillons compressés sur 18 s ⇒ G < G_MIN ⇒ MUET. Une
//    coupure au seul COMPTAGE (n ≥ 6) sans garde-fou temporel laisserait
//    passer ce cas — c'est G qui porte la garantie, pas le mode de coupure.
// =============================================================================
{
  const n = 40, totalMs = 18000;
  const cadenceMs = totalMs / (n - 1);
  const samples = [];
  for (let i = 0; i < n; i++) samples.push({ t: i * cadenceMs, v: 7.00 });
  const verdict = _calStabilityVerdict("ph", samples, totalMs);
  ok(`rafale 40/18s ⇒ G < G_MIN (G=${verdict.G.toFixed(4)})`, verdict.G < CAL_STAB.ph.G_MIN);
  ok("rafale 40/18s ⇒ MUET", verdict.state === "muted");
}

// =============================================================================
// 6) Recalage d'horloge : SANS EFFET.
//    a) `pushStabilitySample` n'utilise PLUS `Date.now()` (texte extrait,
//       preuve structurelle — un recalage NTP ne peut plus ni vider ni faire
//       exploser le tampon).
//    b) `_calStabilityVerdict` est INVARIANTE à un décalage constant de
//       l'origine des temps : seules les séparations RELATIVES comptent.
// =============================================================================
// Comparaison sur le CODE, jamais les commentaires (qui mentionnent
// volontairement l'ancienne horloge `Date.now()` pour expliquer le
// correctif) — strip naïf // et /* */ suffisant ici (pas de chaîne
// contenant ces séquences dans cette fonction).
const pushStabilitySampleCode = pushStabilitySampleSource
  .replace(/\/\*[\s\S]*?\*\//g, "")
  .replace(/\/\/.*$/gm, "");
ok("pushStabilitySample n'appelle PAS Date.now()", !pushStabilitySampleCode.includes("Date.now()"));
ok("pushStabilitySample appelle performance.now()", pushStabilitySampleCode.includes("performance.now()"));
{
  const samplesA = [0, 5000, 10000, 15000, 20000, 25000].map((t) => ({ t, v: 7.00 + (t % 10000 === 0 ? 0.02 : -0.02) }));
  const verdictA = _calStabilityVerdict("ph", samplesA, 25000);
  const HUGE = 1e12; // simule un saut d'horloge arbitraire (recalage NTP)
  const samplesB = samplesA.map((s) => ({ t: s.t + HUGE, v: s.v }));
  const verdictB = _calStabilityVerdict("ph", samplesB, 25000 + HUGE);
  check("verdict invariant à un décalage constant de l'origine des temps", verdictB, verdictA);
}

// =============================================================================
// 7) Tampon borné à 256 échantillons (section 2), quel que soit l'âge —
//    horloge mock IMMOBILE (purge par ÂGE neutralisée : age=0 pour tous les
//    échantillons) pour isoler STRICTEMENT le plafond de COMPTAGE.
// =============================================================================
{
  setMockNowMs(0);
  for (let i = 0; i < 300; i++) pushStabilitySample("ph", 7.00);
  ok(`tampon plafonné à ${CAL_STAB_MAX_SAMPLES} échantillons (300 poussés)`, getBuffer("ph").length === CAL_STAB_MAX_SAMPLES);
}

// =============================================================================
// 8) L'affichage d'une durée dans l'état « instable » DOIT venir de
//    `verdict.S` (RÉALISÉ) — jamais une constante nominale (CAL_STAB_WINDOW_S,
//    supprimée par ce lot) reconstituerait exactement le défaut Δ60 s/Δ20 s.
//    Contrôle STRUCTUREL sur le texte réel (renderStability() touche le DOM,
//    non exécuté ici) — la mutation correspondante (section 8 de la spec) est
//    rejouée manuellement avant livraison, cf. rapport du lot.
// =============================================================================
ok("renderStability() construit la durée affichée depuis verdict.S (réalisée)", renderStabilitySource.includes("verdict.S"));
ok("renderStability() construit l'écart affiché depuis verdict.D (réalisé)", renderStabilitySource.includes("verdict.D"));
ok("la constante de fenêtre NOMINALE (CAL_STAB_WINDOW_S) a bien disparu de data/app.js", !appJsSource.includes("CAL_STAB_WINDOW_S"));

// =============================================================================
// 9) PRIORITÉ DES SIX ÉTATS — EXÉCUTÉE (revue bloquante), pas seulement
//    déduite de la lecture du code : en cours > échec > diagnostic >
//    muet/instable/stable. Un DOM minimal MOCKÉ, le code de renderStability()
//    EXTRAIT tel quel (ADR-0099).
// =============================================================================
{
  const sb = buildRenderSandbox();
  const el = makeFakeEl();
  sb.registerEl("#cal-ph-mid-stability", el);
  sb.setActiveIdx("ph", 1); // étape « Calibrer » active = mid

  // --- en cours (busy) : doit gagner même si le tampon dirait « instable ". --
  sb.setSessionStart("ph", -1000000); // diagnostic serait dû depuis longtemps
  for (let i = 0; i < 20; i++) sb.pushSample("ph", i * 5000, 7.00 + (i % 2 ? 1 : -1)); // très instable
  sb.setAwait({ product: "ph", step: "mid" });
  sb.setMockNowMs(1000000);
  sb.renderStability("ph");
  ok("priorité 1/6 : « en cours » gagne sur diagnostic/instable", el.textContent.startsWith("↻") && el._classes.has("is-busy"));

  // --- échec gagne sur diagnostic (await levé, échec posé). ------------------
  sb.setAwait(null);
  sb.setFailure("ph", "mid", "panne de test");
  sb.renderStability("ph");
  ok("priorité 2/6 : « échec » gagne sur diagnostic (en l'absence d'« en cours »)",
     el.textContent === "✕ panne de test" && el._classes.has("is-failed") && !el._classes.has("is-busy"));

  // --- diagnostic gagne sur instable (échec levé, session ancienne, encore pas stable). --
  // ⚠️ DEUX HORLOGES DISTINCTES, et le premier jet de ce vecteur les confondait :
  //   - l'horloge de SESSION (`_calStabSessionStart`) doit être ANCIENNE pour que
  //     le diagnostic soit dû ;
  //   - l'horloge des ÉCHANTILLONS doit rester FRAÎCHE, sinon la purge d'âge dur
  //     (HARD_AGE_FACTOR × WINDOW = 360 s) les élimine TOUS, le verdict devient
  //     MUET, `verdict.D` vaut null, et la garde de `diagDue` supprime le
  //     diagnostic — à JUSTE TITRE : sans mesure fraîche, on ne peut pas
  //     affirmer « toujours instable ». C'est le code qui avait raison.
  // Les vecteurs 1 et 2 ci-dessus ne souffraient pas du problème : « en cours »
  // et « échec » priment sur le verdict, quel qu'il soit.
  sb.getStab("ph").length = 0;
  // Dérive MONOTONE (et non une alternance ±1) : deux demi-fenêtres alternées
  // auraient des médianes quasi égales ⇒ D ≈ 0 ⇒ « stable », l'inverse du cas visé.
  for (let i = 0; i < 20; i++) sb.pushSample("ph", i * 5000, 7.00 + i * 0.05);
  sb.setMockNowMs(19 * 5000);
  sb.setFailure("ph", "mid", null);
  sb.renderStability("ph");
  ok("priorité 3/6 : « diagnostic » gagne sur instable (session > 5 min, pas stable)",
     el._classes.has("is-diag") && !el._classes.has("is-failed") && el._children.length === 2);

  // --- stable gagne sur diagnostic dès que le critère est de nouveau vrai. --
  // ⚠️ `_calStabSessionStart` reste ANCIEN à dessein (sessionStart = -1000000,
  // non réinitialisé) : la condition TEMPORELLE du diagnostic reste « due »,
  // seule l'apparition de « stable » doit lui faire céder la priorité — cf.
  // `verdict.state !== "stable"` dans renderStability(). L'horloge des
  // ÉCHANTILLONS redémarre à 0 (nouveau tampon, cohérent en interne) ; seule
  // l'horloge de SESSION (domaine distinct, cf. `_calStabSessionStart`) reste
  // ancienne — exactement le découplage voulu par le lot (section 4).
  sb.getStab("ph").length = 0; // nouveau tampon : bruit pur sans dérive
  for (let i = 0; i < 20; i++) sb.pushSample("ph", i * 5000, 7.00 + (i % 2 ? 0.02 : -0.02));
  sb.setMockNowMs(19 * 5000);
  sb.renderStability("ph");
  ok("priorité 4/6 : « stable » gagne sur diagnostic dès que le critère redevient vrai",
     el.textContent === "● Stable — vous pouvez calibrer" && el._classes.has("is-stable") && !el._classes.has("is-diag"));
}

// =============================================================================
// 10) TEXTES T-D1 (pH) / T-D2 (ORP) — VERBATIM, EXÉCUTÉS (revue bloquante).
//     D/S dans la ligne 1 proviennent du MÊME verdict que celui qui a décidé
//     (C5) — jamais des valeurs d'exemple de la spec (0,12 pH/45 s, 4 mV/75 s
//     sont des gabarits de FORMAT, pas des littéraux).
// =============================================================================
{
  const sb = buildRenderSandbox();
  const elPh = makeFakeEl();
  sb.registerEl("#cal-ph-mid-stability", elPh);
  sb.setActiveIdx("ph", 1);
  sb.setSessionStart("ph", -1000000);
  // Dérive LENTE : D encore au-dessus du seuil, verdict "unstable" (pas muet),
  // pour obtenir des D/S non nuls et vérifier le gabarit au-delà du texte fixe.
  for (let i = 0; i < 15; i++) sb.pushSample("ph", i * 5000, 7.00 + 0.2 * Math.exp(-i * 5000 / 120000));
  sb.setMockNowMs(14 * 5000);
  sb.renderStability("ph");
  ok("T-D1 (pH) ligne 2 VERBATIM (renvoi à la pente « État sonde pH »)",
     elPh._children[1].textContent === "Vous pouvez calibrer quand même. Une dérive qui dure est aussi un signe d'usure de la sonde : vérifiez la pente dans « État sonde pH » après l'étalonnage.");
  ok("T-D1 (pH) ligne 1 au gabarit imposé (« Toujours instable après N min (écart … sur … s). »)",
     /^Toujours instable après \d+ min \(écart -?\d+,\d{2} pH sur \d+ s\)\.$/.test(elPh._children[0].textContent));
  ok("T-D1 (pH) : aucune icône dans le texte (posée en CSS, cf. app.css .is-diag::before)",
     !elPh._children[0].textContent.includes("⚠") && !elPh._children[1].textContent.includes("⚠"));

  const elOrp = makeFakeEl();
  sb.registerEl("#cal-orp-stability", elOrp);
  sb.setActiveIdx("orp", 2);
  sb.setSessionStart("orp", -1000000);
  for (let i = 0; i < 15; i++) sb.pushSample("orp", i * 5000, 530 + 10 * Math.exp(-i * 5000 / 180000));
  sb.setMockNowMs(14 * 5000);
  sb.renderStability("orp");
  ok("T-D2 (ORP) ligne 2 VERBATIM (aucun renvoi à une pente — un seul point de calibration)",
     elOrp._children[1].textContent === "Vous pouvez calibrer quand même. Une dérive qui dure est aussi un signe d'usure de la sonde redox.");
  ok("T-D2 (ORP) ligne 1 au gabarit imposé, en mV (jamais « pH »)",
     /^Toujours instable après \d+ min \(écart -?\d+ mV sur \d+ s\)\.$/.test(elOrp._children[0].textContent));
  ok("T-D2 (ORP) : pas de symétrisation avec T-D1 (textes distincts)", elOrp._children[1].textContent !== elPh._children[1].textContent);
}

if (failures > 0) {
  console.error(`\n${failures} vecteur(s) en échec.`);
  process.exit(1);
}
console.log("\nTous les vecteurs de stabilité de calibration (bug-160) sont conformes.");
