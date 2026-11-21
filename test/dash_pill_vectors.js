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
const { extractBalancedFromAnchor, findMatchingClose } = require("./js_extract");

const src = fs.readFileSync(path.join(__dirname, "..", "data", "app.js"), "utf8");
const grab = (anchor, open, close) => extractBalancedFromAnchor(src, anchor, open, close).text;

const codes = grab("const DOSE_BLOCK_CODES = {", "{", "}");
const info = grab("function _doseBlockInfo(json, prefix) {", "{", "}");
const variant = grab("function doseBlockVariant(tone, code) {", "{", "}");
const tone = grab("function dashModePillTone(mode, doseTone, doseCode) {", "{", "}");
const filt = grab("function dashFiltrationTone(forced, frost, running) {", "{", "}");
// bug-200 : le TEXTE de la pastille (inline dans l'appelant) et le plan du bandeau.
const chipLine = (src.match(/const suspendChip = [^\n]*;/) || [])[0];
assert.ok(chipLine, "ligne suspendChip introuvable dans app.js");
const pillCall = (() => {
  const a = src.indexOf("setDashPill(`detail-${kind}-mode`,");
  assert.ok(a > 0 && src.indexOf("setDashPill(`detail-${kind}-mode`,", a + 1) === -1, "ancre setDashPill");
  const open = a + "setDashPill(".length - 1;
  return src.slice(a, findMatchingClose(src, open, "(", ")") + 1);
})();
const suspendFn = grab("function _doseSuspendOutranksMixing(code) {", "{", "}");
const isSusp = grab("function isDoseSuspended(json) {", "{", "}");
const rowData = grab("function _doseBlockRowData(json, prefix) {", "{", "}");
const plan = grab("function _doseBlockBannerPlan(json) {", "{", "}");
const durTxt = grab("function _doseSuspendDurationText(durationS) {", "{", "}");
const blkDur = grab("function _doseBlockDurationText(durationS) {", "{", "}");
const fsc = (src.match(/const DOSE_BLOCK_FILTRATION_STOPPED_CODE = \d+;/) || [])[0];
const elapsed = grab("function _elapsedText(ms, preposition, instantMs = null, instantText = \"à l'instant\") {", "{", "}");
const suspConst = (src.match(/const DOSE_BLOCK_CODE_SUSPEND = \d+;/) || [])[0];
const maxS = (src.match(/const DOSE_BLOCKED_FOR_MAX_S = \d+;/) || [])[0];
assert.ok(suspConst && maxS);

const live = new Function(`
  "use strict";
  const DOSE_BLOCK_CODE_GUARD = 250;
  function _measuresWaitText() { return "attente"; }
  ${codes};
  ${info}
  ${variant}
  ${tone}
  ${filt}
  ${suspConst}
  ${maxS}
  ${suspendFn}
  ${isSusp}
  ${rowData}
  ${plan}
  ${durTxt}
  ${blkDur}
  ${fsc}
  ${elapsed}
  function pillText(json, kind, mode) {
    const latestSensorData = json;
    const modeLabels = { automatic: "Auto", manual: "Arrêt", scheduled: "Programme" };
    let out = null;
    const setDashPill = (id, text) => { out = text; };
    const doseInfo = _doseBlockInfo(latestSensorData, kind);
    ${chipLine}
    ${pillCall};
    return out;
  }
  return { DOSE_BLOCK_CODES, _doseBlockInfo, dashModePillTone, dashFiltrationTone,
           _doseSuspendOutranksMixing, _doseBlockBannerPlan, _doseSuspendDurationText, pillText };
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

// ---- bug-200 : arrêt de sécurité (code 26) ---------------------------------
for (const kind of ["ph", "orp"]) {
  ok(pill(kind, 26) === "warn", `26 (${kind}) doit être ambre`);
  const j = { [kind + "DoseBlockedCode"]: 26 };
  // Le TEXTE change, pas seulement le ton (daltonien / lecteur d'écran).
  const t = live.pillText(j, kind, "automatic");
  ok(t === "Arrêt sécurité", `26 (${kind}) : texte « Arrêt sécurité », reçu « ${t} »`);
  ok(!/Mode Auto/.test(t), "26 ne doit jamais porter « Mode Auto »");
  // Une nuit ordinaire garde son texte habituel.
  ok(live.pillText({ [kind + "DoseBlockedCode"]: 2 }, kind, "automatic") === "Mode Auto", "code 2 => Mode Auto");
}
// Cumul : 26 sur une voie, 2 sur l'autre => la maintenance prime (ambre + texte + bandeau).
ok(pill("ph", 26) === "warn" && pill("orp", 2) !== "warn", "cumul : ORP sous 2 seul reste non ambre");
ok(live.pillText({ phDoseBlockedCode: 26, orpDoseBlockedCode: 2 }, "ph", "automatic") === "Arrêt sécurité", "cumul pH 26");
{
  const p = live._doseBlockBannerPlan({ phDoseBlockedCode: 2, orpDoseBlockedCode: 26 });
  ok(p.suspend && p.visible && p.rowPh === null && p.rowOrp === null, "cumul 2+26 : bandeau d'arrêt seul");
}
// Régression de non-ambre : nuit ordinaire (2 seul) et marches attendues.
for (const code of [2, 7, 8, 10, 11, 12, 13, 14]) {
  const j = { phDoseBlockedCode: code, orpDoseBlockedCode: code };
  for (const kind of ["ph", "orp"]) {
    ok(pill(kind, code) !== "warn", `nuit/marche ${code} (${kind}) jamais ambre`);
    ok(live.pillText(j, kind, "automatic") === "Mode Auto", `code ${code} garde « Mode Auto »`);
  }
  ok(!live._doseBlockBannerPlan(j).suspend, `code ${code} ne déclenche pas le bandeau d'arrêt`);
}
// Pause mélange : 26 la fait taire ; 8 (mélange réel) non.
ok(live._doseSuspendOutranksMixing(26) === true, "26 prime sur la pause mélange");
ok(live._doseSuspendOutranksMixing(8) === false, "8 ne prime pas");
// Bandeau : UNE ligne fusionnée, ton fault, carries des deux côtés.
{
  const p = live._doseBlockBannerPlan({ phDoseBlockedCode: 26, orpDoseBlockedCode: 26,
                                        phDoseBlockedForS: 120, orpDoseBlockedForS: 300 });
  ok(p.visible === true && p.suspend && p.rowPh === null && p.rowOrp === null, "bandeau : une seule ligne fusionnée");
  ok(p.carries.ph === true && p.carries.orp === true, "bandeau : carries vrai des deux côtés");
  ok(p.suspend.durationS === 300, "durée = la plus longue des deux voies");
  ok(live._doseBlockInfo({ phDoseBlockedCode: 26 }, "ph").tone === "fault", "ton fault");
  // Une seule voie en 26 : le bandeau couvre quand même les deux.
  const q = live._doseBlockBannerPlan({ orpDoseBlockedCode: 26 });
  ok(q.suspend && q.carries.ph && q.carries.orp, "une voie en 26 => bandeau pour les deux");
}
// Durée inconnue : plan sans durée, texte VIDE (jamais « — »).
{
  const p = live._doseBlockBannerPlan({ phDoseBlockedCode: 26, orpDoseBlockedCode: 26 });
  ok(p.suspend.durationS === null, "durée inconnue => null");
  for (const bad of [null, undefined, NaN, -1, "x"]) {
    ok(live._doseSuspendDurationText(bad) === "", `durée ${String(bad)} => aucune ligne`);
  }
  ok(!/—/.test(live._doseSuspendDurationText(null)), "jamais de tiret");
  ok(/^Arrêt depuis/.test(live._doseSuspendDurationText(90)), "durée connue => « Arrêt depuis … »");
  ok(live._doseSuspendDurationText(86400) === "Arrêt depuis plus de 24 h", "plafond 24 h");
}

console.log(`dash_pill_vectors : ${n} vérifications OK`);
