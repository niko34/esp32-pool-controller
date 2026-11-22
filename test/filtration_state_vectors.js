// Vecteurs de la bande « eau froide » et de l'état de filtration (bug-197).
//
// L'appareil est TÉMOIN du froid, jamais agent : coldWaterState() relève une
// température, getFiltrationState() dit l'état de la pompe, et AUCUN texte ne
// promet une protection antigel. Ce harnais EXTRAIT le code livré de
// data/app.js par ANCRES (ADR-0099) — jamais une recopie. Si une ancre
// disparaît, il meurt bruyamment.
//
// ⭐ Mutant central (M6) : coldWaterState passant sous measuresAreValid() se
// tairait pompe arrêtée (water_present=false), c'est-à-dire dans le cas
// dangereux. Le vecteur « eau froide + pompe arrêtée ⇒ l'écran dit quelque
// chose » le tue.
//
// Exécution : node test/filtration_state_vectors.js

"use strict";
const assert = require("assert");
const fs = require("fs");
const path = require("path");
const { extractBalancedFromAnchor } = require("./js_extract");

const src = fs.readFileSync(path.join(__dirname, "..", "data", "app.js"), "utf8");
const grab = (anchor) => extractBalancedFromAnchor(src, anchor, "{", "}").text;

const seuilMatch = src.match(/const TEMP_WINTER_MIN_C = (\d+(?:\.\d+)?);/);
assert.ok(seuilMatch, "constante TEMP_WINTER_MIN_C introuvable dans app.js");
const seuilLine = seuilMatch[0];

const fmtFr = grab("function fmtFr(value, decimals) {");
const valid = grab("function measuresAreValid(data) {");
const unknown = grab("function isWaterRoleUnknown(data) {");
const cold = grab("function coldWaterState(data, isRunning) {");
const state = grab("function getFiltrationState(config, data) {");

const live = new Function(`
  "use strict";
  let filtrationRunningOverride = null;
  ${seuilLine}
  ${fmtFr}
  ${valid}
  ${unknown}
  ${cold}
  ${state}
  return {
    cold: coldWaterState,
    state: (config, data, override) => { filtrationRunningOverride = override; return getFiltrationState(config, data); },
    seuil: TEMP_WINTER_MIN_C,
  };
`)();

let n = 0;
const ok = (c, m) => { assert.ok(c, m); n++; };
const eq = (a, b, m) => { assert.deepStrictEqual(a, b, m); n++; };
const base = { sondes_identified: true, sondes_detected: 2, water_present: true };
const d = (over) => Object.assign({}, base, over);

// Seuil figé : la valeur tranchée (bug-197 A2) est 4. Le vecteur à 4,5 °C vient
// EN PREMIER : c'est lui qui doit tuer la mutation 4 -> 5.
ok(!live.cold(d({ temperature: 4.5 }), true).cold, "4,5 °C : pas de bande (FIGE le seuil 4, pas 5)");
eq(live.seuil, 4, "seuil TEMP_WINTER_MIN_C figé à 4");
ok(!live.cold(d({ temperature: 4.5 }), false).cold, "4,5 °C pompe arrêtée : pas de bande");
ok(live.cold(d({ temperature: 3.9 }), true).cold, "3,9 °C : bande");
ok(!live.cold(d({ temperature: 4.0 }), true).cold, "4,0 °C pile : pas de bande");
ok(!live.cold(d({ temperature: 4.04 }), true).cold, "4,04 °C (arrondi 4,0) : pas de bande");
ok(!live.cold(d({ temperature: 3.96 }), true).cold, "3,96 °C (arrondi 4,0, brut < 4) : pas de bande");
ok(live.cold(d({ temperature: 3.94 }), true).cold, "3,94 °C (arrondi 3,9) : bande");

// Silence : donnée absente / invalide.
ok(!live.cold(d({ temperature: NaN }), true).cold, "NaN : silence");
ok(!live.cold(d({ temperature: null }), true).cold, "null : silence");
ok(!live.cold(d({}), true).cold, "température absente : silence");
ok(!live.cold(null, true).cold, "data null : silence");
ok(!live.cold(d({ temperature: "abc" }), true).cold, "texte non numérique : silence");
// Revue 2026-11-22 : ⚠️ `isNaN("")` et `isNaN(false)` valent FAUX — ces deux
// valeurs traversaient la garde et produisaient « Eau à 0,0 °C », c'est-à-dire
// une AFFIRMATION DE FROID sur une donnée vide. ⛔ Seul `typeof t !== 'number'`
// les arrête : retirer ce filtre doit TUER les deux vecteurs ci-dessous.
ok(!live.cold(d({ temperature: "" }), true).cold, "chaîne vide : silence (isNaN(\"\") est FAUX)");
ok(!live.cold(d({ temperature: false }), true).cold, "booléen false : silence (isNaN(false) est FAUX)");

// Sonde non identifiée => silence (ce peut être le coffret).
ok(!live.cold(d({ temperature: 2, sondes_identified: false, sondes_detected: 1 }), true).cold,
  "sonde non identifiée => silence");
ok(!live.cold(d({ temperature: 2, sondes_identified: false, sondes_detected: 1 }), false).cold,
  "sonde non identifiée, pompe arrêtée => silence");

// ⭐ Mutant central : eau froide + pompe arrêtée (water_present=false) => PARLE.
const stopped = live.cold(d({ temperature: 3, water_present: false, measures_valid: false }), false);
ok(stopped.cold && stopped.lines.length === 2, "eau froide + pompe arrêtée => l'écran dit quelque chose");
ok(live.cold(d({ temperature: 3, measures_valid: false }), true).cold, "eau froide + mesures invalides => parle");

// Sonde figée pompe en marche => silence ; pompe arrêtée => on parle.
ok(!live.cold(d({ temperature: 3, tempFrozenAlarm: true }), true).cold, "sonde figée pompe en marche => silence");
ok(live.cold(d({ temperature: 3, tempFrozenAlarm: true, water_present: false }), false).cold,
  "sonde figée mais pompe arrêtée => parle");

// Libellés : marche vs arrêt.
const run = live.cold(d({ temperature: 3 }), true);
const stp = live.cold(d({ temperature: 3, water_present: false }), false);
eq(run.lines[0], "Eau à 3,0 °C (sous 4 °C).", "libellé pompe en marche");
eq(stp.lines[0], "Eau des tuyaux à 3,0 °C (sous 4 °C).", "libellé pompe arrêtée");
eq(run.lines[1], "Le gel reste à votre charge.", "ligne 2 : gel à la charge de l'utilisateur");
eq(live.cold(d({ temperature: 3.94 }), true).lines[0], "Eau à 3,9 °C (sous 4 °C).", "température arrondie, virgule");
eq(run.running, true, "running=true propagé");
eq(stp.running, false, "running=false propagé");

// getFiltrationState : texte, teinte, override (null / true / false).
const cfg = {};
eq(live.state(null, {}, null).text, "Chargement…", "sans config : Chargement");
eq(live.state(cfg, { filtration_running: true }, null).text, "En marche", "/data en marche");
eq(live.state(cfg, { filtration_running: false }, null).text, "Arrêtée", "/data arrêtée");
eq(live.state(cfg, { filtration_running: true }, null).pillClass, "ok", "en marche => ok");
eq(live.state(cfg, { filtration_running: false }, null).pillClass, "", "arrêtée => neutre, jamais rouge");
eq(live.state(cfg, { filtration_running: true }, false).text, "Arrêtée", "override=false AVEC filtration_running=true => Arrêtée");
eq(live.state(cfg, { filtration_running: true }, false).running, false, "override=false => running false");
eq(live.state(cfg, { filtration_running: false }, true).text, "En marche", "override=true AVEC filtration_running=false => En marche");
eq(live.state(cfg, { filtration_running: false }, true).running, true, "override=true => running true");
eq(live.state(cfg, undefined, null).text, "Arrêtée", "data absente, config présente : Arrêtée");

// Interdiction de vocabulaire : aucun texte rendu ne promet de protection.
const interdits = /hors gel|anti-?gel|protection|pause/i;
const textes = [];
for (const ov of [null, true, false]) for (const fr of [true, false]) for (const cf of [cfg, null])
  textes.push(live.state(cf, { filtration_running: fr }, ov).text);
for (const t of [-5, 0, 3, 3.9]) for (const run of [true, false])
  for (const w of [true, false])
    textes.push(...live.cold(d({ temperature: t, water_present: w }), run).lines);
ok(textes.length > 20, "corpus de textes non vide");
for (const t of textes) ok(!interdits.test(t), `texte interdit rendu : « ${t} »`);

console.log(`filtration_state_vectors : ${n} assertions OK`);
