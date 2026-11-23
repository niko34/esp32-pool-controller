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
// bug-199 : _doseBlockInfo CITE le texte firmware via _citeFirmwareText, qui
// s'appuie sur la table _FIRMWARE_TEXT_MOTIFS. ⛔ Sans ces deux extractions,
// l'évaluation lève un ReferenceError — ⭐ ce harnais a d'ailleurs échoué
// BRUYAMMENT en CI plutôt que de passer en silence, ce qui est la propriété
// voulue. ⚠️ Toute dépendance NOUVELLE de _doseBlockInfo devra être ajoutée ici.
// ⚠️ Extrait par ANCRES, ⛔ pas par comptage de crochets : la table contient des
// EXPRESSIONS RÉGULIÈRES dont les classes `[...]` feraient dérailler le
// compteur. ⭐ Les deux ancres existent déjà — tools/check_dose_guard_text_parity.sh
// s'en sert pour comparer ces motifs aux chaînes réelles du firmware.
const motifs = (() => {
  const m = src.match(/\/\/ >>> FIRMWARE-TEXT-MOTIFS:DEBUT\n([\s\S]*?)\n\s*\/\/ <<< FIRMWARE-TEXT-MOTIFS:FIN/);
  assert.ok(m, "bloc FIRMWARE-TEXT-MOTIFS introuvable dans app.js");
  return m[1];
})();
const cite = grab("function _citeFirmwareText(text) {", "{", "}");
const variant = grab("function doseBlockVariant(tone, code) {", "{", "}");
const tone = grab("function dashModePillTone(mode, doseTone, doseCode) {", "{", "}");
const filt = grab("function dashFiltrationTone(forced, running) {", "{", "}");
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
  ${motifs};
  ${cite}
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
// Filtration : forcé = ambre ; sinon vert/gris. Le froid n'a plus de teinte (bug-197).
ok(live.dashFiltrationTone(true, false) === "warn", "forcé arrêt => ambre");
ok(live.dashFiltrationTone(true, true) === "warn", "forcé marche => ambre");
ok(live.dashFiltrationTone(false, true) === "ok", "en marche => vert");
ok(live.dashFiltrationTone(false, false) === "off", "arrêtée normale => gris");

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

// ---- bug-212 : la PORTE hors ligne de setDashPill ---------------------------
// setDashPill() est l'écrivain UNIQUE des pastilles. Il doit lire
// isAppDisconnected() (perte CONSTATÉE, classe `app--disconnected`) et ⛔ PAS
// isAppOffline() : celle-ci couvre aussi « Connexion… » (`mid`), posé juste
// APRÈS une config lue avec succès — annoncer « Hors ligne » y est faux.
// On exécute le setDashPill LIVRÉ (extrait par ancre) contre un `document` feint
// à trois jeux de classes. ⭐ Le vecteur central est la ligne `mid` : remettre
// isAppOffline() dans setDashPill la fait mourir.
// ℹ️ ui_shot.sh ne produit que `ok` et `bad` ; « Connexion… » n'est pas
// photographiable (feature-213). Mais il est EXÉCUTABLE : ce vecteur le fige en
// CI, ce qu'une capture ne ferait pas.
{
  const gateOff = grab("function isAppOffline() {", "{", "}");
  const gateDis = grab("function isAppDisconnected() {", "{", "}");
  const setPill = grab("function setDashPill(id, text, tone, keepTextOffline) {", "{", "}");
  const ecrire = (classes, keep) => {
    const el = { textContent: "initial", dataset: {} };
    // Seul `.app.app--xxx` est simulé : présent ssi la classe demandée est dans le jeu.
    const doc = { querySelector: (sel) => {
      const m = sel.match(/^\.app\.(app--[a-z]+)$/);
      if (!m) return el;
      return classes.includes(m[1]) ? {} : null;
    } };
    new Function("document", "el", `
      "use strict";
      const $ = (sel) => document.querySelector(sel);
      ${gateOff}
      ${gateDis}
      ${setPill}
      setDashPill("x", "En marche", "ok", ${keep});
    `)(doc, el);
    return { text: el.textContent, tone: el.dataset.tone };
  };
  const etats = [
    // [nom, classes, attendu sans keepText, attendu avec keepText]
    ["ok", [], { text: "En marche", tone: "ok" }, { text: "En marche", tone: "ok" }],
    ["mid (Connexion…)", ["app--offline"], { text: "En marche", tone: "ok" }, { text: "En marche", tone: "ok" }],
    ["bad (perte constatée)", ["app--offline", "app--disconnected"],
      { text: "Hors ligne", tone: "off" }, { text: "En marche", tone: "off" }],
  ];
  for (const [nom, classes, sans, avec] of etats) {
    const a = ecrire(classes, false), b = ecrire(classes, true);
    ok(a.text === sans.text && a.tone === sans.tone,
      `état ${nom}, sans keepTextOffline : attendu ${JSON.stringify(sans)}, reçu ${JSON.stringify(a)}`);
    ok(b.text === avec.text && b.tone === avec.tone,
      `état ${nom}, avec keepTextOffline : attendu ${JSON.stringify(avec)}, reçu ${JSON.stringify(b)}`);
  }
  // Jamais « Hors ligne » tant que la perte n'est pas constatée.
  ok(ecrire(["app--offline"], false).text !== "Hors ligne", "mid : JAMAIS « Hors ligne »");
}

// ---- bug-212 : la POSE des classes par setNetStatus --------------------------
// Moitié manquante de l'invariant : ci-dessus on vérifie la LECTURE de
// `app--disconnected` ; ici on exécute le setNetStatus() LIVRÉ (extrait par
// ancre) avec un faux `.app` dont classList est un vrai ensemble, pour que
// `state !== "ok"` à la place de `=== "bad"` (retour exact de bug-212) fasse
// poser `app--disconnected` sur `mid` et MEURE. On part de chaque état
// initial possible (aucune classe / les deux) : une classe jamais retirée meurt aussi.
// ⚠️ `app--offline` DOIT rester posée sur `mid` : c'est le verrou des commandes
// pendant « Connexion… » — le déverrouiller serait un défaut de sécurité d'usage.
//
// LIMITES CONNUES (volontaires — les couvrir redeviendrait lexical) :
//  - un SECOND appelant de getFiltrationState ajouté ailleurs n'est pas vu
//    (seul updateFiltrationBadges est exécuté) ;
//  - un AUTRE écrivain de pastille, contournant setDashPill, n'est pas vu ;
//  - setNetStatus est supposé le SEUL écrivain de `app--disconnected` : un autre
//    écrivain ne serait pas détecté.
{
  const setNet = grab("function setNetStatus(state, text) {", "{", "}");
  const poser = (etat, initiales) => {
    const set = new Set(initiales);
    const appEl = { classList: {
      add: (...c) => c.forEach((x) => set.add(x)),
      remove: (...c) => c.forEach((x) => set.delete(x)),
      toggle: (c, force) => { const v = force === undefined ? !set.has(c) : !!force; v ? set.add(c) : set.delete(c); return v; },
      contains: (c) => set.has(c),
    } };
    const neutre = { classList: { add() {}, remove() {} }, textContent: "" };
    new Function("document", "neutre", "appEl", `
      "use strict";
      const $ = () => neutre;
      const refreshDashPills = () => {};
      ${setNet}
      setNetStatus(${JSON.stringify(etat)}, "t");
    `)({ querySelector: (sel) => (sel === ".app" ? appEl : null) }, neutre, appEl);
    return { offline: set.has("app--offline"), disconnected: set.has("app--disconnected") };
  };
  const attendu = {
    ok:  { offline: false, disconnected: false },
    mid: { offline: true,  disconnected: false },
    bad: { offline: true,  disconnected: true },
  };
  for (const initiales of [[], ["app--offline", "app--disconnected"], ["app--offline"]]) {
    for (const etat of ["ok", "mid", "bad"]) {
      const r = poser(etat, initiales);
      const a = attendu[etat];
      ok(r.offline === a.offline && r.disconnected === a.disconnected,
        `setNetStatus("${etat}") depuis [${initiales}] : attendu ${JSON.stringify(a)}, reçu ${JSON.stringify(r)}`);
    }
  }
  ok(poser("mid", []).disconnected === false, "mid ne pose JAMAIS app--disconnected");
  ok(poser("mid", []).offline === true, "mid garde app--offline (verrou des commandes)");
}

console.log(`dash_pill_vectors : ${n} vérifications OK`);
