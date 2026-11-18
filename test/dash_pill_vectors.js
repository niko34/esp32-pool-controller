// Vecteurs de la décision PURE « quelle teinte pour la pastille d'état du
// tableau de bord ? » (feature-195, condition B1 du gate pool-chemistry).
//
// B1 : la puce AMBRE d'une carte pH/ORP ⟺ doseBlockVariant(...) === 'danger'
// (tones fault/unknown). Une règle naïve « ambre si dosage bloqué » rallumerait
// l'ambre TOUTES LES NUITS (code 2 « Filtration arrêtée », 7, 8, 10-14... sont
// des marches attendues, tone 'normal'). Ce harnais EXTRAIT le code livré de
// data/app.js (ADR-0099) — DOSE_BLOCK_CODES, _doseBlockInfo, doseBlockVariant,
// dashModePillTone, dashFiltrationTone — et l'évalue : jamais une recopie.
//
// MUTATION EXIGÉE : passer un `tone: 'normal'` de DOSE_BLOCK_CODES en
// 'fault' (ex. code 2) ⇒ le vecteur « marche attendue ⇒ jamais ambre » meurt.
//
// Exécution : node test/dash_pill_vectors.js

"use strict";
const assert = require("assert");
const fs = require("fs");
const path = require("path");
const { extractBalancedFromAnchor } = require("./js_extract");

const src = fs.readFileSync(path.join(__dirname, "..", "data", "app.js"), "utf8");
const grab = (anchor, open, close) => extractBalancedFromAnchor(src, anchor, open, close).text;

const codes = grab("const DOSE_BLOCK_CODES = {", "{", "}");
const info = grab("function _doseBlockInfo(json, prefix) {", "{", "}");
const variant = grab("function doseBlockVariant(tone, code) {", "{", "}");
const tone = grab("function dashModePillTone(mode, doseTone, doseCode) {", "{", "}");
const filt = grab("function dashFiltrationTone(forced, frost, running) {", "{", "}");

const live = new Function(`
  "use strict";
  const DOSE_BLOCK_CODE_GUARD = 250;
  function _measuresWaitText() { return "attente"; }
  ${codes};
  ${info}
  ${variant}
  ${tone}
  ${filt}
  return { DOSE_BLOCK_CODES, _doseBlockInfo, dashModePillTone, dashFiltrationTone };
`)();

function pill(kind, code, mode = "automatic") {
  const json = { [kind + "DoseBlockedCode"]: code };
  const i = live._doseBlockInfo(json, kind);
  return live.dashModePillTone(mode, i.tone, i.code);
}

let n = 0;
const ok = (cond, msg) => { assert.ok(cond, msg); n++; };

// Marches attendues : JAMAIS ambre, sur pH comme sur ORP.
for (const code of [2, 7, 8, 9, 10, 11, 12, 13, 14, 15, 18, 24, 25]) {
  for (const kind of ["ph", "orp"]) {
    ok(pill(kind, code) !== "warn", `code ${code} (${kind}) ne doit pas être ambre`);
  }
}
// Pannes / mesure en cause : ambre.
for (const code of [1, 3, 6, 16, 17]) {
  for (const kind of ["ph", "orp"]) {
    ok(pill(kind, code) === "warn", `code ${code} (${kind}) doit être ambre`);
  }
}
// Garde 250 : pH = ambre ; ORP = inhibition normale, pas ambre.
ok(pill("ph", 250) === "warn", "250 pH ambre");
ok(pill("orp", 250) !== "warn", "250 ORP pas ambre");
// Aucun blocage / donnée absente.
ok(pill("ph", 0) === "ok" && pill("ph", null) === "ok", "0/null => vert");
ok(live.dashModePillTone("automatic", "none", 0) === "ok", "json absent => vert");
// Modes.
ok(pill("ph", 0, "manual") === "off", "Arrêt => gris");
ok(pill("ph", 0, "scheduled") === "ok", "Programme => vert");
// Filtration : forcé = ambre ; hors gel = gris ; sinon vert/gris.
ok(live.dashFiltrationTone(true, false, false) === "warn", "forcé => ambre");
ok(live.dashFiltrationTone(true, true, true) === "off", "hors gel => gris");
ok(live.dashFiltrationTone(false, false, true) === "ok", "en marche => vert");
ok(live.dashFiltrationTone(false, false, false) === "off", "arrêtée normale => gris");

console.log(`dash_pill_vectors : ${n} vérifications OK`);
