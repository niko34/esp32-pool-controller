// Vecteurs de la convention décimale (bug-199) : extraits de `data/app.js`.
//
// Patron ADR-0099 (`test/js_extract.js`) : le code VIVANT est extrait puis évalué sous
// Node avec des bouchons minimaux. ⛔ Jamais une copie.
//
// Vecteurs :
//   M1  🔴 la largeur de barre (`updateCard`, `bar.style.width`) est une valeur CSS
//       VALIDE (point décimal) : `width: 72,4%` serait ignoré en silence par le
//       navigateur, la barre resterait figée, sans erreur ni log.
//   M2  les valeurs MACHINE (`input.value` de loadProductConfig, `dataset.value` de
//       buildPhTargetSegments) sont relisibles par parseFloat, sans virgule.
//   M3  aucun TEXTE produit par les fonctions d'affichage atteignables ne contient
//       `\d\.\d` ; complété par un balayage TEXTUEL de tous les `toFixed(` de app.js
//       (chacun suivi d'un `.replace(".", ",")`, ou l'une des 3 exceptions nommées).
//   M4  `_formatSeriesStatsText` : séparateur `·`, trois valeurs à virgule.
//   M5  (numéro conservé pour ne pas décaler les renvois) : c'était la garde
//       tools/check_decimal_format_guard.sh elle-même, couverte par son propre script et
//       par CI — PAS un vecteur de ce fichier. Son absence ici n'est pas un oubli.
//   M7  `outOfRangeText` (« Trop bas (6,9) ») avec le VRAI callback `round` de app.js,
//       et ORP entier préservé (« 820 mV ») ; l'ORP des statistiques à 0 décimale.
//   M8  🔴 texte firmware CITÉ (`_doseBlockInfo`, garde pH/ORP) : six motifs ANCRÉS (une table, pas une règle de forme) ; tout texte inconnu,
//       IP, version, « 12.5 » INCHANGÉS — ce sont les NON-conversions qui comptent.
//   M6  les libellés de stock : virgule de « Stock faible », AUCUN nombre sur le
//       compteur au bout, et appareil neuf ≠ bidon épuisé (bug-211, livré).
//
// 🔴 LIMITE DU BALAYAGE TEXTUEL (M3) — à ne pas oublier : il ne garantit QUE « aucun
// `toFixed` nu ». Il ne voit NI les littéraux de texte (« pH 7.0 » en dur, en JS ou en
// HTML), NI les nombres interpolés dans un gabarit (`${v}` rend son point tout seul).
// C'est le trou par lequel quatre défauts de bug-199 sont passés, et on s'est appuyé sur
// ce balayage pour conclure à l'exhaustivité. ⛔ Ne jamais lire « balayage vert » comme
// « plus aucun point affiché ». Les trouvailles de cette famille se vérifient par
// vecteur d'exécution (M7) ou à la main.
// ⚠️ Autre piège (substitution) : passer un bouchon `(v) => v` pour un paramètre de
// fonction extraite annule le bénéfice d'ADR-0099 — on passe le VRAI callback (REAL_ROUND).
//
// NON couvert par exécution (variables locales de fonctions trop grosses pour être
// extraites) : updateDashboardMetrics (température/pH), les `fmtPct`/`fmtMv` locaux de
// la carte Signaux, l'écran Température/pH, l'étalonnage, l'historique (13117), les
// débits pompes (13925, 14847, 14891). Ceux-là ne sont tenus que par le balayage
// textuel de M3 et par tools/check_decimal_format_guard.sh.
//
// `BUG199_APP_JS` pointe un autre fichier que data/app.js : validation PAR MUTATION sur
// une copie. Exécution : `node test/decimal_format_vectors.js`

"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const { extractBalancedFromAnchor } = require("./js_extract");

const APP_JS_PATH = process.env.BUG199_APP_JS || path.join(__dirname, "..", "data", "app.js");
const src = fs.readFileSync(APP_JS_PATH, "utf8");

function fn(name, sig, indent) {
  return extractBalancedFromAnchor(src, `${indent || ""}function ${name}${sig} {`, "{", "}").text;
}

const POINT = /\d\.\d/;
let failures = 0;
let ran = 0;
function check(label, f) {
  ran++;
  try { f(); console.log(`  ok   ${label}`); }
  catch (e) { failures++; console.log(`  FAIL ${label}\n       ${e.message}`); }
}

// ---------------------------------------------------------------- bac à sable
// Faux élément DOM : style / classList / dataset / textContent / value.
function el() {
  const classes = new Set();
  return {
    style: {}, dataset: {}, textContent: "", value: "", className: "", children: [],
    classList: { add: (c) => classes.add(c), remove: (...c) => c.forEach((x) => classes.delete(x)),
                 has: (c) => classes.has(c) },
    setAttribute() {}, removeAttribute() {},
    appendChild(c) { this.children.push(c); return c; },
  };
}

const prelude = [
  fn("fmtFr", "(value, decimals)", "  "),
  fn("_fmtPh", "(v, signed)", "  "),
  fn("_fmt2", "(v)", "  "),
  fn("_filterRawIsValid", "(v)", "  ").replace(/\n$/, ""),
  fn("_fmtFilterVal", "(prefix, v)", "  "),
  fn("_formatSeriesStatsText", "(stats, decimals)", "  "),
  fn("outOfRangeText", "(value, target, tol, round, unit, show)", "  "),
  // bug-211 : la cascade des quatre états de stock vit ici ; buildSensorSegments
  // l'APPELLE, donc elle doit être extraite AVANT lui (ADR-0099 : on extrait le
  // code livré, ⛔ on ne le recopie jamais).
  fn("bidonCounterState", "(remainingMl, containerMl, thresholdMl, tttState)", "  "),
  // bug-208 : buildSensorSegments APPELLE _effectiveDailyMax pour le segment
  // « Cumul XX % » (même dénominateur que la barre de la carte héros) — extraite
  // avant lui, comme bidonCounterState ci-dessus.
  fn("_effectiveDailyMax", "(sensor)", "  "),
  fn("buildSensorSegments", "(sensor, target, tol, round, unit, show, dailyMlKey, remKey, thrKey)", "  "),
  fn("updateCard", "(prefix, remainingMl, containerMl, thresholdMl)", "    "),
  fn("loadProductConfig", "(cfg)", "  "),
  fn("buildPhTargetSegments", '(groupId = "ph_target_group", inputId = "ph_target", warningBoxId = "ph_target_warning")', "  "),
  fn("_rescueEffectiveDailyMl", "(cfg)", "  "),
  fn("_rescueReasonText", "(data, cfg)", "  "),
].join("\n");

const factory = new Function("els", "globals", `
  const document = { createElement: () => els.make() };
  const window = { _config: globals.config };
  let latestSensorData = null;
  const _dosingCountdownShown = { ph: false, orp: false };
  const $ = (sel) => els.get(sel);
  const measuresAreValid = () => true;
  const isDoseSuspended = () => false;
  const measureStateSegments = () => [];
  const dosingLimitSegments = () => [];
  const dailyCumulSegment = () => null;
  const dosingCountdownSegment = () => null;
  const dosingStabOrMixSegment = () => null;
  const _phTargetValues = () => [6.8, 7.0, 7.2, 7.4];
  const setPhTarget = () => {};
  const updateProductUI = () => {};
  ${prelude}
  return { outOfRangeText, fmtFr, _fmtPh, _fmt2, _fmtFilterVal, _formatSeriesStatsText, buildSensorSegments,
           bidonCounterState,
           updateCard, loadProductConfig, buildPhTargetSegments, _rescueReasonText,
           setData: (d) => { latestSensorData = d; } };
`);

// VRAI callback `round` du badge pH, lu dans app.js (jamais recopié).
const realRoundMatch = src.match(/buildSensorSegments\('ph',[^\n]*?,\s*(v => \+v\.toFixed\(1\)),\s*''/);
if (!realRoundMatch) { console.log("FAIL callback round du badge pH introuvable dans app.js (reformatage de buildSensorSegments('ph'… ? ajuster la regex : elle epingle le contrat, elle ne s'adapte pas)"); process.exit(1); }
const REAL_ROUND = new Function(`return (${realRoundMatch[1]});`)();

function sandbox(config) {
  const map = new Map();
  const els = {
    get: (sel) => { if (!map.has(sel)) map.set(sel, el()); return map.get(sel); },
    make: () => el(),
  };
  return { api: factory(els, { config: config || {} }), els };
}

// ------------------------------------------------------------------------- M1
console.log("M1 largeur de barre : valeur CSS valide");
{
  const cases = [[723.456, 1000, "72.3"], [333, 1000, "33.3"], [1, 3, "33.3"], [1000, 1000, "100.0"], [0, 1000, "0.0"], [1, 7, "14.3"]];
  for (const [rem, tot, wantNum] of cases) {
    check(`barre ${rem}/${tot} => width ${wantNum}%`, () => {
      const { api, els } = sandbox();
      api.updateCard("ph", rem, tot, 100);
      const w = els.get("#product-ph-bar").style.width;
      assert.ok(/^\d+(\.\d+)?%$/.test(w), `largeur CSS invalide : ${JSON.stringify(w)}`);
      assert.ok(!w.includes(","), `virgule dans une largeur CSS : ${w}`);
      assert.strictEqual(parseFloat(w).toFixed(1), parseFloat(wantNum).toFixed(1));
    });
  }
  check("contenance absente => largeur 0.0% (valide), jamais NaN", () => {
    const { api, els } = sandbox();
    api.updateCard("orp", 500, null, 100);
    assert.strictEqual(els.get("#product-orp-bar").style.width, "0.0%");
  });
}

// ------------------------------------------------------------------------- M2
console.log("M2 valeurs machine relisibles");
check("loadProductConfig : input.value relisible, sans virgule", () => {
  for (const ml of [2000, 2500, 300, 0, 1234]) {
    const { api, els } = sandbox();
    api.loadProductConfig({ ph_alert_threshold_ml: ml, orp_alert_threshold_ml: ml });
    for (const id of ["#ph_alert_threshold_l", "#orp_alert_threshold_l"]) {
      const v = els.get(id).value;
      assert.ok(!String(v).includes(","), `virgule dans input.value : ${v}`);
      // bug-211 : le seuil d'alerte s'affiche en litres ENTIERS. Motif :
      // `.toFixed(1)` rendait « 2.0 » — un POINT — à trois centimètres de
      // « 0,3 L » et « 20,0 L » à virgule. Un <input type=number> IMPOSE le
      // point au navigateur : la seule sortie honnête est de n'avoir aucune
      // décimale. ⇒ On garde la garde « jamais de virgule » (elle casserait la
      // saisie) et on exige désormais un ENTIER NU.
      assert.ok(/^\d+$/.test(v), `format inattendu : ${v}`);
      assert.strictEqual(parseFloat(v), Math.round(ml / 1000));
    }
  }
});
check("buildPhTargetSegments : dataset.value relisible, texte affiché à virgule", () => {
  const { api, els } = sandbox();
  els.get("#ph_target_group");
  api.buildPhTargetSegments();
  const btns = els.get("#ph_target_group").children;
  assert.strictEqual(btns.length, 4);
  const want = [6.8, 7.0, 7.2, 7.4];
  btns.forEach((b, i) => {
    assert.strictEqual(parseFloat(b.dataset.value), want[i]);
    assert.ok(!b.dataset.value.includes(","), `virgule dans dataset : ${b.dataset.value}`);
    assert.ok(b.textContent.includes(",") && !POINT.test(b.textContent), `texte : ${b.textContent}`);
  });
});

// ------------------------------------------------------------------------- M3
console.log("M3 aucun texte d'affichage ne porte de point décimal");
const nums = [0, 0.04, 0.3, 0.7, 1, 7.3, 7.35, 12.5, 24.8, 555.8, 1234.5678];
check("fmtFr / _fmtPh / _fmt2 : virgule, jamais de point", () => {
  const { api } = sandbox();
  for (const v of nums) {
    for (const t of [api.fmtFr(v, 1), api.fmtFr(v, 2), api._fmtPh(v), api._fmtPh(-v, true), api._fmt2(v)]) {
      assert.ok(!POINT.test(t) && !t.includes("."), `point dans « ${t} » (v=${v})`);
    }
  }
});
check("« Stock faible » (carte pH/ORP) : virgule pour plusieurs restes", () => {
  // bug-211 : ⛔ `rem = 0` a QUITTÉ cette liste. Ce n'est plus « Stock faible
  // (0,0 L) » — c'est « Compteur du bidon à zéro », SANS nombre. Le cas est
  // couvert par M6, qui exige l'absence de tout chiffre. Les valeurs ci-dessous
  // sont les restes NON NULS, seuls à afficher encore une quantité.
  for (const rem of [40, 300, 700, 1500, 1999]) {
    const { api } = sandbox();
    api.setData({ ph_remaining_ml: rem, ph_container_ml: 20000, ph_alert_threshold_ml: 2000, ph_tracking_enabled: true });
    const segs = api.buildSensorSegments("ph", 7.2, 0.2, REAL_ROUND, "", api._fmtPh, "a", "ph_remaining_ml", "ph_alert_threshold_ml");
    const s = segs.find((x) => /Stock faible/.test(x.text));
    assert.ok(s, `segment absent pour rem=${rem}`);
    assert.ok(!POINT.test(s.text), `point dans « ${s.text} »`);
    assert.ok(/\(\d+,\d L\)/.test(s.text), `forme inattendue « ${s.text} »`);
  }
});
check("cartes de dosage : reste et contenance à virgule", () => {
  for (const [rem, tot] of [[300, 2000], [723, 1500], [0, 1000], [1999, 2500]]) {
    const { api, els } = sandbox();
    api.updateCard("orp", rem, tot, 100);
    for (const id of ["#product-orp-remaining", "#product-orp-total"]) {
      const t = els.get(id).textContent;
      assert.ok(!POINT.test(t) && /,/.test(t), `« ${t} »`);
    }
  }
});
check("bandeau « Chloration suspendue » : pH à virgule", () => {
  for (const ph of [7.65, 7.7, 8.04, 9.4]) {
    const { api } = sandbox();
    const t = api._rescueReasonText({ water_present: true, phFiltered: ph }, { ph_offset: 0 });
    assert.ok(/Chloration suspendue : pH \d,\d /.test(t), `« ${t} »`);
    assert.ok(!POINT.test(t), `point dans « ${t} »`);
  }
});
check("_fmtFilterVal : pH 2 décimales à virgule, ORP entier", () => {
  const { api } = sandbox();
  for (const v of [6.8, 7.123, 7.0]) assert.ok(!POINT.test(api._fmtFilterVal("ph", v)));
  assert.strictEqual(api._fmtFilterVal("ph", 7.1), "7,10");
  assert.strictEqual(api._fmtFilterVal("orp", 555.8), "556 mV");
});

// Balayage TEXTUEL : couvre aussi les sites non extractibles (voir l'en-tête).
check("balayage : tout toFixed( est converti, ou l'une des 3 exceptions nommées", () => {
  const lines = src.split("\n");
  // bug-211 : les DEUX exceptions `*Alert.value = (… / 1000).toFixed(1)` ont
  // DISPARU — le seuil d'alerte est passé en litres entiers (Math.round), il
  // n'emploie plus toFixed du tout. ⛔ Les laisser ici rendrait la sentinelle
  // MENTEUSE : elle exigerait la présence d'un motif absent du code livré.
  const exceptions = [
    /performance\.now\(\) - t0\)\.toFixed\(0\)/,          // debugLog (journal de développement)
    /v => \+v\.toFixed\(1\)/,                              // nombre, pas affichage
    /bar\.style\.width = pct\.toFixed\(1\) \+ '%'/,        // valeur CSS
  ];
  const used = new Set();
  const bare = [];
  lines.forEach((l, i) => {
    if (!l.includes(".toFixed(") || /^\s*\/\//.test(l)) return;
    const converted = /\.toFixed\([^)]*\)\.replace\("\.", ","\)/.test(l);
    if (converted) return;
    const k = exceptions.findIndex((re) => re.test(l));
    if (k === -1) bare.push(`${i + 1}: ${l.trim()}`);
    else used.add(k);
  });
  assert.deepStrictEqual(bare, [], `toFixed nu hors exceptions :\n${bare.join("\n")}`);
  assert.strictEqual(used.size, 3, `exceptions attendues toutes présentes, vues : ${used.size}`);
});

// ------------------------------------------------------------------------- M7
console.log("M7 « Trop bas / Trop élevé » : virgule affichée, comparaison intacte");
check("pH 6,92 => « Trop bas (6,9) », pH 7,55 => « Trop élevé (7,5) » (vrai callback)", () => {
  const { api } = sandbox();
  assert.strictEqual(api.outOfRangeText(6.92, 7.2, 0.2, REAL_ROUND, "", api._fmtPh), "Trop bas (6,9)");
  assert.strictEqual(api.outOfRangeText(7.55, 7.2, 0.2, REAL_ROUND, "", api._fmtPh), "Trop élevé (7,5)");
  assert.strictEqual(api.outOfRangeText(7.2, 7.2, 0.2, REAL_ROUND, "", api._fmtPh), null);
});
check("ORP entier préservé : « Trop élevé (820 mV) », jamais « 820,0 »", () => {
  const { api } = sandbox();
  assert.strictEqual(api.outOfRangeText(820, 650, 150, Math.round, " mV", String), "Trop élevé (820 mV)");
  assert.strictEqual(api.outOfRangeText(400, 650, 150, Math.round, " mV", String), "Trop bas (400 mV)");
});
check("l'écran pH n'affiche aucun point dans « Trop bas/élevé »", () => {
  const { api } = sandbox();
  for (const v of [6.0, 6.45, 6.92, 7.55, 8.04, 9.4]) {
    const t = api.outOfRangeText(v, 7.2, 0.2, REAL_ROUND, "", api._fmtPh);
    assert.ok(t === null || !POINT.test(t), `point dans « ${t} »`);
    // CONTRAT : une assertion qui dit ce qui ne doit PAS etre present ne verifie pas ce qui DOIT l'etre.
    assert.ok(t === null || /\(\d+,\d\)$/.test(t), `contrat 1 decimale violé : « ${t} »`);
  }
});
check("pH entier : « Trop bas (6,0) » / « Trop élevé (10,0) », jamais « (6) » ; ORP « (-46 mV) »", () => {
  const { api } = sandbox();
  assert.strictEqual(api.outOfRangeText(6.0, 7.2, 0.2, REAL_ROUND, "", api._fmtPh), "Trop bas (6,0)");
  assert.strictEqual(api.outOfRangeText(9.97, 7.2, 0.2, REAL_ROUND, "", api._fmtPh), "Trop élevé (10,0)");
  const o = api.outOfRangeText(-46, 650, 150, Math.round, " mV", String);
  assert.ok(/\(-?\d+ mV\)$/.test(o), o);
  assert.strictEqual(o, "Trop bas (-46 mV)");
});
check("dialogue de réinitialisation : la consigne pH passe par _fmtPh (pas de 3e expression)", () => {
  const m = src.match(/key: "ph_target",[^\n]*?fmt: \(v\) => (`[^`]*`)/);
  assert.ok(m, "ligne ph_target de kRegulationResetFields introuvable (reformatage ? ajuster la regex)");
  const { api } = sandbox();
  const fmt = new Function("_fmtPh", `return (v) => ${m[1]};`)(api._fmtPh);
  assert.strictEqual(fmt(7.2), "7,2 pH");
  assert.strictEqual(fmt(7.35), api._fmtPh(7.35) + " pH");
  assert.ok(!POINT.test(fmt(7.2)) && !POINT.test(fmt(7.35)));
});
check("statistiques : pH 2 décimales ET ORP 0 décimale, les huit sites ensemble", () => {
  const rows = [...src.matchAll(/label: "(pH|ORP)[^"]*",\s*stats: seriesStats\(\w+\),\s*decimals: (\d)/g)];
  const ph = rows.filter((m) => m[1] === "pH").map((m) => m[2]);
  const orp = rows.filter((m) => m[1] === "ORP").map((m) => m[2]);
  assert.deepStrictEqual(ph, ["2", "2", "2", "2"], `pH : ${ph}`);
  assert.deepStrictEqual(orp, ["0", "0", "0", "0"], `ORP : ${orp}`);
  const { api } = sandbox();
  assert.strictEqual(api._formatSeriesStatsText({ n: 3, min: 540.2, max: 560.7, avg: 551.4 }, 0), "min 540 · max 561 · moy 551");
});

// ------------------------------------------------------------------------- M4
console.log("M4 statistiques de série");
check("séparateur « · », trois valeurs à virgule", () => {
  const { api } = sandbox();
  const t = api._formatSeriesStatsText({ n: 3, min: 1.2, max: 3.4, avg: 2.3 }, 1);
  assert.strictEqual(t, "min 1,2 · max 3,4 · moy 2,3");
  assert.strictEqual(t.split(" · ").length, 3);
  assert.ok(!POINT.test(t));
});
check("2 décimales, et série vide => « — »", () => {
  const { api } = sandbox();
  assert.strictEqual(api._formatSeriesStatsText({ n: 2, min: 6.8, max: 7.25, avg: 7.0 }, 2), "min 6,80 · max 7,25 · moy 7,00");
  assert.strictEqual(api._formatSeriesStatsText({ n: 0 }, 1), "—");
});

// ------------------------------------------------------------------------- M8
console.log("M8 texte firmware cité : valeurs de capteurs seules");
{
  const blk = src.match(/^  \/\/ >>> FIRMWARE-TEXT-MOTIFS:DEBUT\n([\s\S]*?)^  \/\/ <<< FIRMWARE-TEXT-MOTIFS:FIN$/m);
  if (!blk) { console.log("FAIL bloc FIRMWARE-TEXT-MOTIFS introuvable dans app.js"); process.exit(1); }
  const m8 = new Function(`
    const DOSE_BLOCK_CODE_GUARD = 250;
    const DOSE_BLOCK_CODES = { 250: { toneOrp: 'normal' } };
    ${blk[1]}
    ${fn("_citeFirmwareText", "(text)", "  ")}
    ${fn("_doseBlockInfo", "(json, prefix)", "  ")}
    return { _citeFirmwareText, _doseBlockInfo };
  `)();
  const cite = m8._citeFirmwareText;
  const guard = (t, prefix) => m8._doseBlockInfo({ [(prefix || "ph") + "DoseBlockedCode"]: 250, [(prefix || "ph") + "DoseBlockedReason"]: t }, prefix || "ph").label;
  const same = [
    ["IP", "Capteur injoignable 192.168.1.48 depuis 3 min"],
    ["version", "Firmware version 3.0.1 incompatible"],
    ["version v", "Mise à jour v2.33.0 requise"],
    ["triplet", "Référence 1.2.3 inconnue"],
    ["entier", "pH trop élevé (seuil 7) — dosage chlore suspendu"],
    ["sans nombre", "Sécurité pH/ORP active"],
    ["trop de décimales", "pH trop élevé (seuil 7.612) — dosage chlore suspendu"],
    // NON-conversions : un texte INCONNU n'est jamais touché, même de « bonne forme »
    // (mutation : revenir à une règle de forme => ces vecteurs meurent).
    ["inconnu 12.5", "Débit 12.5 L/h inattendu"],
    ["pourcentage", "Cuve remplie à 12.5 %"],
    ["inconnu début de chaîne", "12.5"],
    ["inconnu pH", "pH 6.75"],
    ["inconnu °C", "25.4 °C"],
    ["libellé reformulé", "pH trop haut (seuil 7.6) — dosage chlore suspendu"],
    ["motif en milieu de phrase", "Alerte : pH trop élevé (seuil 7.6) — dosage chlore suspendu"],
    ["valeur pointée", "pH trop élevé (seuil 7.6.1) — dosage chlore suspendu"],
  ];
  const real = [
    ["pH brut au-dessus de la butée de sécurité (8.5) — dosage pH+ suspendu.", "pH brut au-dessus de la butée de sécurité (8,5) — dosage pH+ suspendu."],
    ["pH au-dessus du plafond de sécurité (8.2) — dosage pH+ suspendu.", "pH au-dessus du plafond de sécurité (8,2) — dosage pH+ suspendu."],
    ["Vérifiez l'offset pH : brut sous 6.0, corrigé au-dessus de 6.7 — dosage acide suspendu.", "Vérifiez l'offset pH : brut sous 6,0, corrigé au-dessus de 6,7 — dosage acide suspendu."],
    ["pH sous le plancher de sécurité (6.7) — dosage acide suspendu.", "pH sous le plancher de sécurité (6,7) — dosage acide suspendu."],
    ["pH brut en butée haute (butée 8.5) — dosage chlore suspendu", "pH brut en butée haute (butée 8,5) — dosage chlore suspendu"],
    ["pH trop élevé (seuil 7.6) — dosage chlore suspendu", "pH trop élevé (seuil 7,6) — dosage chlore suspendu"],
  ];
  for (const [i, r] of real.entries()) check(`message firmware réel n°${i + 1} converti`, () => assert.strictEqual(cite(r[0]), r[1]));
  for (const [n, t] of same) check(`inchangé : ${n}`, () => assert.strictEqual(cite(t), t));
  check("aucune allocation inutile : même chaîne rendue", () => { const t = "Sécurité pH/ORP active"; assert.strictEqual(cite(t), t); assert.strictEqual(cite(""), ""); });
  check("branche garde, voie pH ET ORP, via le VRAI _doseBlockInfo", () => {
    assert.strictEqual(guard("pH trop élevé (seuil 7.6) — dosage chlore suspendu", "orp"), "pH trop élevé (seuil 7,6) — dosage chlore suspendu");
    assert.strictEqual(guard("brut sous 6.0 sur 192.168.1.48", "ph"), "brut sous 6.0 sur 192.168.1.48");
    assert.strictEqual(guard("", "ph"), "Sécurité pH/ORP active");
  });
  check("repli firmware sans code : même normalisation", () => {
    assert.strictEqual(m8._doseBlockInfo({ phDoseBlockedReason: "pH trop élevé (seuil 7.6) — dosage chlore suspendu" }, "ph").label, "pH trop élevé (seuil 7,6) — dosage chlore suspendu");
  });
}

// ------------------------------------------------------------------------- M6
// bug-211 (2026-11-22) : CE BLOC A ÉTÉ RÉÉCRIT, et c'est la sentinelle qui l'a
// exigé — elle a échoué dès que bug-211 a touché le libellé, ce qui est très
// exactement son rôle. Ce qu'elle gardait et qui N'EXISTE PLUS :
//   1. « Stock faible (0,0 L) » — ⛔ c'était LE défaut de bug-211 : l'écran
//      annonçait un stock « faible » sur un compteur arrivé au bout, avec un
//      nombre qui SATURE à zéro. Remplacé par « Compteur du bidon à zéro »,
//      SANS aucun nombre.
//   2. Le vecteur du « badge des cartes de dosage » : il testait du CODE MORT
//      (#product-<p>-status n'existe dans aucun HTML), supprimé par bug-211.
// Ce qui reste gardé ici : la VIRGULE de l'état « faible », seul état qui
// affiche encore un nombre — c'est l'acquis de bug-199 qu'il faut protéger.
console.log("M6 les libellés de stock (bug-211) et leur virgule");
check("« Stock faible (0,3 L) » : virgule conservée", () => {
  const { api } = sandbox();
  api.setData({ ph_remaining_ml: 300, ph_container_ml: 20000, ph_alert_threshold_ml: 2000, ph_tracking_enabled: true });
  const segs = api.buildSensorSegments("ph", 7.2, 0.2, REAL_ROUND, "", api._fmtPh, "a", "ph_remaining_ml", "ph_alert_threshold_ml");
  const seg = segs.find((x) => /Stock faible/.test(x.text));
  assert.ok(seg, "segment « Stock faible » absent");
  assert.strictEqual(seg.text, "Stock faible (0,3 L)");
  assert.ok(!/\d\.\d/.test(seg.text), "un POINT décimal a reparu");
});
check("compteur au bout : AUCUN nombre affiché", () => {
  const { api } = sandbox();
  api.setData({ ph_remaining_ml: 0, ph_container_ml: 20000, ph_alert_threshold_ml: 2000, ph_tracking_enabled: true });
  const segs = api.buildSensorSegments("ph", 7.2, 0.2, REAL_ROUND, "", api._fmtPh, "a", "ph_remaining_ml", "ph_alert_threshold_ml");
  const seg = segs.find((x) => /Compteur du bidon/.test(x.text));
  assert.ok(seg, "segment « Compteur du bidon à zéro » absent");
  assert.strictEqual(seg.text, "Compteur du bidon à zéro");
  assert.ok(!/\d/.test(seg.text), "un CHIFFRE est réapparu dans l'état nul — c'était le défaut de bug-211");
  assert.ok(!/Stock faible/.test(segs.map((x) => x.text).join(" ")), "« Stock faible » survit à l'état nul");
});
check("contenance non renseignée ≠ compteur au bout", () => {
  const { api } = sandbox();
  api.setData({ ph_remaining_ml: 0, ph_container_ml: 0, ph_alert_threshold_ml: 2000, ph_tracking_enabled: true });
  const segs = api.buildSensorSegments("ph", 7.2, 0.2, REAL_ROUND, "", api._fmtPh, "a", "ph_remaining_ml", "ph_alert_threshold_ml");
  const txt = segs.map((x) => x.text).join(" ");
  assert.ok(/Contenance du bidon à renseigner/.test(txt), "l'appareil neuf n'invite pas à renseigner la contenance");
  assert.ok(!/Compteur du bidon à zéro/.test(txt), "appareil neuf confondu avec un bidon épuisé — LE défaut de bug-211");
});
// ⛔ Revue bug-211 (2026-11-22) : ces DEUX vecteurs sont nés d'un défaut réel.
// La cascade ne lisait pas `*_tracking_enabled`, qui vaut FALSE en sortie
// d'usine (src/config.h) — un appareil neuf criait une alerte permanente vers
// une page dont le corps de carte est justement masqué. Le même garde ferme le
// démarrage, où latestSensorData est encore null.
check("suivi de produit désactivé ⇒ AUCUN segment de stock", () => {
  const { api } = sandbox();
  for (const d of [{ ph_remaining_ml: 0, ph_container_ml: 0 },
                   { ph_remaining_ml: 0, ph_container_ml: 20000 },
                   { ph_remaining_ml: 300, ph_container_ml: 20000 }]) {
    api.setData({ ...d, ph_alert_threshold_ml: 2000, ph_tracking_enabled: false });
    const segs = api.buildSensorSegments("ph", 7.2, 0.2, REAL_ROUND, "", api._fmtPh, "a", "ph_remaining_ml", "ph_alert_threshold_ml");
    const txt = segs.map((x) => x.text).join(" ");
    assert.ok(!/Contenance du bidon|Compteur du bidon|Stock faible/.test(txt),
      `le suivi est COUPÉ et l'appareil parle quand même du bidon : « ${txt} »`);
  }
});
check("démarrage (aucune donnée reçue) ⇒ AUCUN segment de stock", () => {
  const { api } = sandbox();
  api.setData(null);
  const segs = api.buildSensorSegments("ph", 7.2, 0.2, REAL_ROUND, "", api._fmtPh, "a", "ph_remaining_ml", "ph_alert_threshold_ml");
  const txt = segs.map((x) => x.text).join(" ");
  assert.ok(!/Contenance du bidon|Compteur du bidon|Stock faible/.test(txt),
    `faux positif AVANT la première réponse de /data : « ${txt} »`);
});
check("⛔ aucun texte n'affirme que le bidon est vide", () => {
  const { api } = sandbox();
  for (const d of [{ ph_remaining_ml: 0, ph_container_ml: 20000 }, { ph_remaining_ml: 0, ph_container_ml: 20000, ph_ttt_state: 3 }]) {
    api.setData({ ...d, ph_alert_threshold_ml: 2000, ph_tracking_enabled: true });
    const segs = api.buildSensorSegments("ph", 7.2, 0.2, REAL_ROUND, "", api._fmtPh, "a", "ph_remaining_ml", "ph_alert_threshold_ml");
    const txt = segs.map((x) => x.text).join(" ");
    assert.ok(!/\bvide\b/i.test(txt), "l'interface AFFIRME que le bidon est vide — l'appareil ne le sait pas (ADR-0141)");
  }
});

console.log(`\n${ran - failures}/${ran} vecteurs conformes`);
process.exit(failures ? 1 : 0);
