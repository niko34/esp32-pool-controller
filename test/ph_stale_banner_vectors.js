// Vecteurs du bandeau « Correction à vérifier » et des seuils servis de la
// correction pH (feature-170), extraits de `data/app.js`.
//
// Patron `test/ph_tune_remove_vectors.js` (feature-157) et ADR-0099 : le code
// VIVANT est extrait (ancre + comptage de profondeur, `test/js_extract.js`) puis
// évalué dans un bac à sable Node avec un bouchon DOM minimal. ⛔ Jamais une
// copie : ce qui juge ici est le code qui tourne dans le navigateur.
//
// Vecteurs portés :
//   V7  feature-170 (C33a) — une config SANS ph_offset_confirm_interval_days et
//       avec les cinq autres champs => les CINQ seuils servis sont adoptés, pas de
//       repli en bloc (violation d'ADR-0100 déclenchée par un retrait).
//   V8  feature-170 (C33c) — cause héritée « boot » / « expiration » => TEXTE
//       NEUTRE, jamais le texte « calibration » ; « calibration » et `undefined`
//       (firmware antérieur) => texte feature-170 (C29), compatibilité ascendante.
//   V9  (non listé par la fiche, imposé par la capture, écart n°1 tranché) — le
//       chapeau « Étalonnage modifié » nomme un ÉVÉNEMENT : masqué dès que la cause
//       n'est pas calibration/undefined.
//   +   les QUATRE rendus mot pour mot de feature-170 (C29), la phrase d'acte
//       (P2 jamais sans P1), l'absence du mot « sonde », la ligne de date de
//       feature-170 (C28).
//
// ⚠️ La parité de la phrase d'acte avec le firmware n'est PAS ici : elle est
// l'affaire de tools/check_ph_stale_text_parity.sh (ce harnais compare l'acte à la
// phrase de la fiche, pas à src/ph_stale_message.h).
//
// `PH_STALE_APP_JS` (variable d'environnement) pointe un autre fichier que
// data/app.js : c'est ainsi qu'on valide ce harnais PAR MUTATION sur une copie,
// sans toucher au code livré.
//
// Exécution : `node test/ph_stale_banner_vectors.js`
//   (via ./tools/check_ph_stale_banner_vectors.sh).

"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const { extractBalancedFromAnchor } = require("./js_extract");

const APP_JS_PATH = process.env.PH_STALE_APP_JS || path.join(__dirname, "..", "data", "app.js");
const src = fs.readFileSync(APP_JS_PATH, "utf8");

// Fonction entière : de « function nom( » jusqu'à l'accolade fermante du corps.
function fn(name, sig) {
  return extractBalancedFromAnchor(src, `function ${name}${sig} {`, "{", "}").text;
}

// Ligne unique (déclaration d'une ligne) : exactement UNE occurrence exigée.
function line(re, label) {
  const g = new RegExp(re.source, "gm");
  const found = src.match(g) || [];
  if (found.length !== 1) {
    throw new Error(`Extraction ambiguë ou vide (${found.length} occurrences) : ${label}`);
  }
  return found[0].trim();
}

const parts = [
  line(/^\s*const _phNum = .*;$/m, "const _phNum"),
  extractBalancedFromAnchor(src, "const PH_OFFSET_THRESHOLDS_FALLBACK = Object.freeze(", "(", ")").text + ";",
  line(/^\s*let _phOffsetThresholds = .*;$/m, "let _phOffsetThresholds"),
  fn("_applyPhOffsetThresholds", "(cfg)"),
  fn("_fmtPhOffset2", "(v)"),
  fn("_fmtPhStaleDate", "(epoch)"),
  fn("renderPhOffsetStaleBanner", "(cfg)"),
  fn("_phStaleBannerCarriesFact", "(cfg)"),
  fn("renderPhTuneAppliedLine", "(cfg)"),
];

const factory = new Function("$", "document", `
  "use strict";
  ${parts.join("\n")}
  return {
    apply: _applyPhOffsetThresholds,
    thresholds: () => _phOffsetThresholds,
    fallback: PH_OFFSET_THRESHOLDS_FALLBACK,
    renderBanner: renderPhOffsetStaleBanner,
    renderApplied: renderPhTuneAppliedLine,
  };
`);

// ---- Bouchon DOM minimal ----------------------------------------------------

function makeDom() {
  const els = {
    "#ph_tune_stale_banner": { hidden: true, textContent: "" },
    "#ph_tune_stale_fait": { hidden: true, textContent: "" },
    "#ph_tune_stale_acte": { hidden: true, textContent: "" },
    "#ph_tune_stale_eyebrow": { hidden: false, textContent: "Étalonnage modifié" },
    "#ph_tune_applied": {
      hidden: true,
      nodes: [],
      replaceChildren(...n) { this.nodes = n; },
      get textContent() { return this.nodes.map((x) => x.text !== undefined ? x.text : x.textContent).join(""); },
    },
  };
  const dollar = (sel) => (Object.prototype.hasOwnProperty.call(els, sel) ? els[sel] : null);
  const doc = {
    createElement: () => ({ textContent: "" }),
    createTextNode: (t) => ({ text: t }),
  };
  return { els, api: factory(dollar, doc) };
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

// ============================================================================
// V7 — feature-170 (C33a) : seuils servis sans ph_offset_confirm_interval_days
// ============================================================================

const FIVE = {
  ph_offset_unusual_abs: 0.60,
  ph_offset_impasse_threshold: 1.30,
  ph_offset_impasse_threshold_high: -0.80,
  ph_raw_floor_absolute: 5.60,
  ph_raw_ceiling_absolute: 8.40,
};
const FIVE_ADOPTED = {
  unusualAbs: 0.60, impasseThreshold: 1.30, impasseThresholdHigh: -0.80,
  rawFloorAbsolute: 5.60, rawCeilingAbsolute: 8.40,
};

{
  const { api } = makeDom();
  api.apply(Object.assign({}, FIVE));
  check("V7 sans ph_offset_confirm_interval_days : les CINQ seuils servis sont adoptés (pas de repli en bloc)",
        api.thresholds(), FIVE_ADOPTED);
}
{
  // Un firmware antérieur qui sert encore l'ancien champ : ignoré, les cinq adoptés.
  const { api } = makeDom();
  api.apply(Object.assign({ ph_offset_confirm_interval_days: 30 }, FIVE));
  check("V7 ancien champ présent : ignoré, cinq seuils adoptés", api.thresholds(), FIVE_ADOPTED);
  check("V7 le repli ne porte plus confirmIntervalDays",
        Object.prototype.hasOwnProperty.call(api.fallback, "confirmIntervalDays"), false);
  check("V7 l'objet de seuils adopté ne porte plus confirmIntervalDays",
        Object.prototype.hasOwnProperty.call(api.thresholds(), "confirmIntervalDays"), false);
}
{
  const { api } = makeDom();
  const initial = Object.assign({}, api.thresholds());
  check("repli initial = valeurs d'usine (0,50 / 1,20 / −0,90 / 5,50 / 8,50)", initial, {
    unusualAbs: 0.50, impasseThreshold: 1.20, impasseThresholdHigh: -0.90,
    rawFloorAbsolute: 5.50, rawCeilingAbsolute: 8.50,
  });
  api.apply({});
  check("lot ABSENT : seuils déjà connus conservés", api.thresholds(), initial);
  api.apply(Object.assign({}, FIVE, { ph_offset_impasse_threshold: 0.40 })); // it <= u : incohérent
  check("lot incohérent (impasse <= inhabituel) : ignoré en bloc", api.thresholds(), initial);
  api.apply(Object.assign({}, FIVE, { ph_raw_floor_absolute: 9, ph_raw_ceiling_absolute: 8 }));
  check("lot incohérent (filet bas >= filet haut) : ignoré en bloc", api.thresholds(), initial);
  api.apply(Object.assign({}, FIVE, { ph_offset_unusual_abs: "0.6" }));
  check("seuil non numérique : lot ignoré en bloc", api.thresholds(), initial);
}

// ============================================================================
// Rendu du bandeau — feature-170 (C29), (C31), (C33c)
// ============================================================================

const ACTE = "Refaites un test manuel et accordez la mesure dessus, ou remettez la correction à zéro.";
const NEUTRE = "L'appareil signale cette correction comme à revérifier.";
const CAL = 1794657600;  // 14/11/2026 12:00 UTC
const OFF = 1792152000;  // 16/10/2026 12:00 UTC

function render(cfg) {
  const { els, api } = makeDom();
  api.renderBanner(cfg);
  return els;
}
const base = (extra) => Object.assign({ ph_offset: 1.20, ph_offset_stale: true }, extra);

// -- Les QUATRE rendus mot pour mot de feature-170 (C29) ---------------------
{
  const e = render(base({ ph_offset_stale_cause: "calibration", ph_cal_epoch: CAL, ph_offset_epoch: OFF }));
  check("feature-170 (C29) (a) deux dates connues : P1 mot pour mot", e["#ph_tune_stale_fait"].textContent,
    "L'étalonnage a changé le 14/11/2026. La correction en vigueur (+1,20) a été posée le 16/10/2026, donc avant : ce changement a déplacé la mesure sur laquelle elle avait été calculée.");
  check("feature-170 (C29) (a) P2 = la phrase d'acte", e["#ph_tune_stale_acte"].textContent, ACTE);
  check("feature-170 (C29) (a) bandeau visible", e["#ph_tune_stale_banner"].hidden, false);
  check("feature-170 (C29) (a) chapeau « Étalonnage modifié » visible", e["#ph_tune_stale_eyebrow"].hidden, false);
// ----------------------------------------------------------------------------
// feature-170 (C29), arbitrage Q14 : ⛔ MOTS BANNIS de TOUS les rendus.
// 🔴 markPhCalibrationStamped() est aussi appelée sur un EFFACEMENT d'étalonnage
// (ClearPhCal, sensors.cpp) ⇒ « refait » et « vient d'avoir lieu » y seraient
// FAUX. ⭐ Le texte nomme l'ÉTAT, ⛔ jamais le chemin (ADR-0139).
// ⚠️ On assertte le texte PRODUIT (16 combinaisons cause × dates), ⛔ pas la
// source : un contrôle sur la source se tromperait sur les commentaires, qui
// citent volontairement ces mots pour les interdire.
// ----------------------------------------------------------------------------
for (const mot of ["refait", "vient d'avoir lieu", "sonde"]) {
  for (const cause of ["calibration", undefined, "boot", "expiration"]) {
    for (const [lc, lo] of [[CAL, OFF], [0, OFF], [CAL, 0], [0, 0]]) {
      const e = render({ ph_offset: 1.20, ph_offset_stale: true, ph_offset_stale_cause: cause,
                         ph_cal_epoch: lc, ph_offset_epoch: lo });
      const txt = e["#ph_tune_stale_fait"].textContent + " " + e["#ph_tune_stale_acte"].textContent
                + " " + e["#ph_tune_stale_eyebrow"].textContent;
      check(`feature-170 (C29) mot banni « ${mot} » absent (cause ${cause}, dates ${lc}/${lo})`,
            txt.includes(mot), false);
    }
  }
}
}
{
  const e = render(base({ ph_offset_stale_cause: "calibration", ph_cal_epoch: 0, ph_offset_epoch: OFF }));
  check("feature-170 (C29) (b) ph_cal_epoch = 0 : P1 mot pour mot", e["#ph_tune_stale_fait"].textContent,
    "L'étalonnage a changé à une date que l'appareil n'a pas pu enregistrer (son horloge n'était pas à l'heure). La correction en vigueur (+1,20) a été posée le 16/10/2026, donc avant : ce changement a déplacé la mesure sur laquelle elle avait été calculée.");
}
{
  const e = render(base({ ph_offset_stale_cause: "calibration", ph_cal_epoch: CAL, ph_offset_epoch: 0 }));
  check("feature-170 (C29) (c) ph_offset_epoch = 0 : P1 mot pour mot", e["#ph_tune_stale_fait"].textContent,
    "L'étalonnage a changé le 14/11/2026. La correction en vigueur (+1,20) a été posée avant, à une date que l'appareil n'a pas pu enregistrer (son horloge n'était pas à l'heure) : ce changement a déplacé la mesure sur laquelle elle avait été calculée.");
}
{
  const e = render(base({ ph_offset_stale_cause: "calibration", ph_cal_epoch: 0, ph_offset_epoch: 0 }));
  check("feature-170 (C29) (d) les deux à 0 : rendu DÉDIÉ, P1 mot pour mot", e["#ph_tune_stale_fait"].textContent,
    "L'étalonnage a changé après la pose de la correction en vigueur (+1,20) — l'appareil n'a pu enregistrer aucune des deux dates, son horloge n'étant pas à l'heure. Ce changement a déplacé la mesure sur laquelle elle avait été calculée.");
  check("feature-170 (C29) (d) P2 = la phrase d'acte", e["#ph_tune_stale_acte"].textContent, ACTE);
}
{
  const e = render(base({ ph_offset: -0.90, ph_offset_stale_cause: "calibration", ph_cal_epoch: CAL, ph_offset_epoch: OFF }));
  check("valeur négative : signe « − » (U+2212) et 2 décimales dans P1",
        e["#ph_tune_stale_fait"].textContent.includes("(−0,90)"), true);
}

// -- V8 : cause héritée => texte neutre, jamais « calibration » -------------
for (const cause of ["boot", "expiration"]) {
  const e = render(base({ ph_offset_stale_cause: cause, ph_cal_epoch: CAL, ph_offset_epoch: OFF }));
  check(`V8 cause « ${cause} » : P1 = texte NEUTRE`, e["#ph_tune_stale_fait"].textContent, NEUTRE);
  check(`V8 cause « ${cause} » : JAMAIS le texte d'étalonnage`,
        e["#ph_tune_stale_fait"].textContent.includes("L'étalonnage a changé"), false);
  check(`V8 cause « ${cause} » : aucune date ni valeur affirmée`,
        /\d/.test(e["#ph_tune_stale_fait"].textContent), false);
  check(`V8 cause « ${cause} » : P2 = la phrase d'acte`, e["#ph_tune_stale_acte"].textContent, ACTE);
  check(`V8 cause « ${cause} » : bandeau visible`, e["#ph_tune_stale_banner"].hidden, false);
  // V9 — le chapeau affirmerait un événement qui n'a pas eu lieu.
  check(`V9 cause « ${cause} » : chapeau « Étalonnage modifié » MASQUÉ`, e["#ph_tune_stale_eyebrow"].hidden, true);
}
{
  const e = render(base({ ph_offset_stale_cause: "inconnue-demain", ph_cal_epoch: CAL, ph_offset_epoch: OFF }));
  check("V8 cause inconnue (autre firmware) : texte neutre aussi", e["#ph_tune_stale_fait"].textContent, NEUTRE);
  check("V9 cause inconnue : chapeau masqué", e["#ph_tune_stale_eyebrow"].hidden, true);
}
{
  const e = render(base({ ph_cal_epoch: CAL, ph_offset_epoch: OFF })); // cause `undefined`
  check("V8 cause `undefined` (firmware antérieur) : texte « calibration » (compat. ascendante)",
        e["#ph_tune_stale_fait"].textContent.startsWith("L'étalonnage a changé le 14/11/2026."), true);
  check("V9 cause `undefined` : chapeau visible", e["#ph_tune_stale_eyebrow"].hidden, false);
}
{
  const e = render(base({ ph_offset_stale_cause: "calibration", ph_cal_epoch: CAL, ph_offset_epoch: OFF }));
  // Réaffichage après une cause héritée : le chapeau doit REVENIR (même bandeau, re-rendu).
  const { els, api } = makeDom();
  api.renderBanner(base({ ph_offset_stale_cause: "boot" }));
  check("V9 (précondition) chapeau masqué après « boot »", els["#ph_tune_stale_eyebrow"].hidden, true);
  api.renderBanner(base({ ph_offset_stale_cause: "calibration", ph_cal_epoch: CAL, ph_offset_epoch: OFF }));
  check("V9 le chapeau REVIENT quand la cause redevient « calibration »", els["#ph_tune_stale_eyebrow"].hidden, false);
  check("(témoin) rendu calibration indépendant", e["#ph_tune_stale_banner"].hidden, false);
}

// -- Le bandeau reste masqué quand il ne doit pas s'afficher ---------------
for (const [label, cfg] of [
  ["ph_offset_stale = false", { ph_offset: 1.20, ph_offset_stale: false }],
  ["ph_offset_stale non strictement true (1)", { ph_offset: 1.20, ph_offset_stale: 1 }],
  ["ph_offset_stale chaîne « true »", { ph_offset: 1.20, ph_offset_stale: "true" }],
  ["correction nulle", { ph_offset: 0, ph_offset_stale: true }],
  ["correction NaN", { ph_offset: NaN, ph_offset_stale: true }],
  ["correction absente", { ph_offset_stale: true }],
  ["cfg null", null],
  ["cfg undefined", undefined],
]) {
  const e = render(cfg);
  check(`bandeau masqué : ${label}`, e["#ph_tune_stale_banner"].hidden, true);
  check(`bandeau masqué : ${label} — P1/P2 non écrits`,
        e["#ph_tune_stale_fait"].textContent + e["#ph_tune_stale_acte"].textContent, "");
}

// -- P2 jamais sans P1, aucun « sonde » -----------------------------------
for (const cause of [undefined, "calibration", "boot", "expiration"]) {
  for (const [ce, oe] of [[CAL, OFF], [0, OFF], [CAL, 0], [0, 0]]) {
    const e = render(base({ ph_offset_stale_cause: cause, ph_cal_epoch: ce, ph_offset_epoch: oe }));
    const tag = `cause=${cause} cal=${ce} off=${oe}`;
    check(`P2 jamais sans P1 (${tag})`,
          e["#ph_tune_stale_banner"].hidden === false && e["#ph_tune_stale_fait"].textContent !== "" && e["#ph_tune_stale_acte"].textContent !== "", true);
    check(`aucun mot « sonde » (${tag})`,
          /sonde/i.test(e["#ph_tune_stale_fait"].textContent + e["#ph_tune_stale_acte"].textContent), false);
  }
}

// ============================================================================
// feature-170 (C28) — ligne de date de pose
// ============================================================================
function applied(cfg) {
  const { els, api } = makeDom();
  api.renderApplied(cfg);
  return els["#ph_tune_applied"];
}
{
  const el = applied({ ph_offset: 1.20, ph_offset_epoch: OFF });
  check("feature-170 (C28) date connue", el.textContent, "Correction +1,20 pH — posée le 16/10/2026");
  check("feature-170 (C28) date connue : visible", el.hidden, false);
}
check("feature-170 (C28) ph_offset_epoch = 0 : « date de pose inconnue » (jamais 1970)",
      applied({ ph_offset: 1.20, ph_offset_epoch: 0 }).textContent, "Correction +1,20 pH — date de pose inconnue");
check("feature-170 (C28) correction négative", applied({ ph_offset: -0.30, ph_offset_epoch: OFF }).textContent,
      "Correction −0,30 pH — posée le 16/10/2026");
for (const [label, cfg] of [["nulle", { ph_offset: 0 }], ["NaN", { ph_offset: NaN }], ["absente", {}],
                            ["chaîne", { ph_offset: "1.2" }], ["cfg null", null]]) {
  const el = applied(cfg);
  check(`feature-170 (C28) correction ${label} : ligne masquée et vide (jamais « Correction +0,00 »)`, [el.hidden, el.textContent], [true, ""]);
}

// ----------------------------------------------------------------------------
// feature-170 (C28), écart n°2 tranché SUR CAPTURE : la ligne s'efface derrière
// le bandeau QUAND CELUI-CI PORTE DÉJÀ LE FAIT — et SEULEMENT dans ce cas.
// ⚠️ Défaut qui a motivé la règle : « +1,20 » et « posée le 16/10/2026 » étaient
// affichés DEUX FOIS en 2 cm à 390 px.
// ----------------------------------------------------------------------------
for (const cause of ["calibration", undefined]) {
  const el = applied({ ph_offset: 1.20, ph_offset_epoch: OFF, ph_offset_stale: true,
                       ph_offset_stale_cause: cause });
  check(`feature-170 (C28) bandeau « ${cause === undefined ? "absente" : cause} » : ligne MASQUÉE (le bandeau porte le fait)`,
        [el.hidden, el.textContent], [true, ""]);
}
// ⛔ Le texte NEUTRE de feature-170 (C33c) ne cite NI la valeur NI la date :
// masquer la ligne ferait PERDRE le fait. Elle reste visible.
for (const cause of ["boot", "expiration"]) {
  const el = applied({ ph_offset: 1.20, ph_offset_epoch: OFF, ph_offset_stale: true,
                       ph_offset_stale_cause: cause });
  check(`feature-170 (C28) cause héritée « ${cause} » : ligne VISIBLE (le texte neutre ne porte pas le fait)`,
        [el.hidden, el.textContent], [false, "Correction +1,20 pH — posée le 16/10/2026"]);
}
// Le bandeau n'est pas affiché (ph_offset_stale absent ou faux) ⇒ la ligne reste.
check("feature-170 (C28) sans bandeau : ligne visible même avec une cause servie",
      applied({ ph_offset: 1.20, ph_offset_epoch: OFF, ph_offset_stale: false,
                ph_offset_stale_cause: "calibration" }).hidden, false);

if (failures > 0) {
  console.error(`\n${failures} vecteur(s) en échec.`);
  process.exit(1);
}
console.log("\nTous les vecteurs de feature-170 (bandeau, seuils, ligne de date) sont conformes.");
