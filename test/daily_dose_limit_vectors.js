// Vecteurs de la LIMITE JOURNALIÈRE RÉELLEMENT OPPOSABLE (bug-208).
//
// La barre « Dosage du jour » se remplissait vers le plafond RÉGLÉ (2 000 mL)
// alors que la limite opposable est plus basse : le débit soutenable de la
// pompe × les minutes de filtration PROGRAMMÉES du jour (1 768 mL côté pH,
// ≈ 1 734 mL côté chlore). Le dosage s'arrêtait donc avec une barre à 88 %, et
// rien à l'écran ne disait pourquoi.
//
// ⭐ Mutant central : prendre l'UN des deux plafonds au lieu de leur MINIMUM.
//    • le réglé seul  ⇒ la barre vise un but inatteignable (le défaut d'origine) ;
//    • le matériel seul ⇒ BIEN PIRE. À orp_limit_minutes = 60 (réglage autorisé),
//      phSustainableAvgFlow() rend l'amplitude ENTIÈRE : ~10 400 mL/j, soit CINQ
//      FOIS le plafond réglé, et la barre marquerait 19 % à l'instant où le
//      dosage s'arrête. C'est le sens DANGEREUX, et deux vecteurs le gardent.
//
// Ce harnais EXTRAIT le code livré de data/app.js par ANCRES (ADR-0099) —
// jamais une recopie. Si une ancre disparaît, il meurt bruyamment.
//
// Exécution : node test/daily_dose_limit_vectors.js

"use strict";
const assert = require("assert");
const fs = require("fs");
const path = require("path");
const { extractBalancedFromAnchor } = require("./js_extract");

const src = fs.readFileSync(path.join(__dirname, "..", "data", "app.js"), "utf8");
function fn(name, sig, indent) {
  return extractBalancedFromAnchor(src, `${indent || ""}function ${name}${sig} {`, "{", "}").text;
}

let failures = 0;
let ran = 0;
function check(label, f) {
  ran++;
  try { f(); console.log(`  ok   ${label}`); }
  catch (e) { failures++; console.log(`  FAIL ${label}\n       ${e.message}`); }
}

// ---------------------------------------------------------------- bac à sable
function el() {
  return {
    style: {}, dataset: {}, textContent: "", value: "", className: "",
    attrs: {},
    setAttribute(k, v) { this.attrs[k] = v; },
    removeAttribute(k) { delete this.attrs[k]; },
  };
}

const prelude = [
  fn("fmtFr", "(value, decimals)", "  "),
  // La cascade de stock (bug-211) : _stockTakesPrecedence l'APPELLE, donc elle
  // est extraite avant lui.
  fn("bidonCounterState", "(remainingMl, containerMl, thresholdMl, tttState)", "  "),
  fn("_effectiveDailyMax", "(sensor)", "  "),
  fn("_stockTakesPrecedence", "(sensor)", "  "),
  fn("dailyCumulSegment", "(dailyMl, maxMl, limitReached)", "  "),
  fn("updateDailyDoseDisplay", "(type)", "  "),
].join("\n");

const factory = new Function("els", "globals", `
  const window = { _config: globals.config };
  let latestSensorData = globals.data;
  const $ = (sel) => els.get(sel);
  ${prelude}
  return { _effectiveDailyMax, _stockTakesPrecedence, dailyCumulSegment, updateDailyDoseDisplay,
           bidonCounterState };
`);

function sandbox(config, data) {
  const map = new Map();
  const els = { get: (sel) => { if (!map.has(sel)) map.set(sel, el()); return map.get(sel); } };
  return { api: factory(els, { config: config || {}, data: data || null }), els, map };
}

// Config et données « nominales » de l'appareil de production au 2026-11-22.
const CFG = { max_ph_ml_per_day: 2000, max_chlorine_ml_per_day: 2000 };
const STOCK_OK = {
  ph_tracking_enabled: true, ph_remaining_ml: 10000, ph_container_ml: 20000, ph_alert_threshold_ml: 2000,
  orp_tracking_enabled: true, orp_remaining_ml: 10000, orp_container_ml: 20000, orp_alert_threshold_ml: 2000,
};
const D = (over) => Object.assign({}, STOCK_OK, over);

// =============================================================== LE MINIMUM
console.log("L1 le dénominateur est le MINIMUM des deux plafonds");

check("réglé 2 000 > matériel 1 768 ⇒ 1 768 (le défaut d'origine)", () => {
  const { api } = sandbox(CFG, D({ ph_daily_max_ml: 1768 }));
  const lim = api._effectiveDailyMax("ph");
  assert.strictEqual(lim.effective, 1768, "la barre vise encore le plafond RÉGLÉ");
  assert.strictEqual(lim.limitedByCapacity, true);
});

check("⛔ matériel 10 404 (60 min/h) ⇒ 2 000, JAMAIS 10 404", () => {
  const { api } = sandbox(CFG, D({ orp_daily_max_ml: 10404 }));
  const lim = api._effectiveDailyMax("orp");
  assert.strictEqual(lim.effective, 2000,
    "le plafond MATÉRIEL a été pris seul : la barre marquerait 19 % quand le dosage s'arrête");
  assert.strictEqual(lim.limitedByCapacity, false, "aucun écart à expliquer dans ce sens");
});

check("⛔ matériel 6 120 (filtration en Marche) ⇒ 2 000", () => {
  const { api } = sandbox(CFG, D({ orp_daily_max_ml: 6120 }));
  assert.strictEqual(api._effectiveDailyMax("orp").effective, 2000);
});

check("les deux plafonds ÉGAUX ⇒ aucun écart", () => {
  const { api } = sandbox(CFG, D({ ph_daily_max_ml: 2000 }));
  const lim = api._effectiveDailyMax("ph");
  assert.strictEqual(lim.effective, 2000);
  assert.strictEqual(lim.limitedByCapacity, false, "une note s'afficherait sans rien avoir à dire");
});

// ================================================== ABSENCE, NULL, ZÉRO
console.log("L2 ce qui manque ne s'invente pas (ADR-0100)");

check("matériel absent (firmware antérieur) ⇒ repli sur le réglé, aucun écart", () => {
  const { api } = sandbox(CFG, D({}));
  const lim = api._effectiveDailyMax("orp");
  assert.strictEqual(lim.effective, 2000);
  assert.strictEqual(lim.limitedByCapacity, false);
});

check("matériel null (mode Arrêt, pompe non caractérisée) ⇒ repli, aucun écart", () => {
  const { api } = sandbox(CFG, D({ ph_daily_max_ml: null }));
  const lim = api._effectiveDailyMax("ph");
  assert.strictEqual(lim.effective, 2000);
  assert.strictEqual(lim.limitedByCapacity, false);
});

check("matériel 0 ⇒ repli sur le réglé, ⛔ aucune division par zéro", () => {
  const { api } = sandbox(CFG, D({ ph_daily_max_ml: 0 }));
  assert.strictEqual(api._effectiveDailyMax("ph").effective, 2000);
});

check("réglé absent mais matériel connu ⇒ le matériel fait foi", () => {
  const { api } = sandbox({}, D({ ph_daily_max_ml: 1768 }));
  const lim = api._effectiveDailyMax("ph");
  assert.strictEqual(lim.effective, 1768);
  assert.strictEqual(lim.limitedByCapacity, false);
});

check("aucun des deux ⇒ effective null, et la barre dira « Limite non configurée »", () => {
  const { api } = sandbox({}, D({}));
  assert.strictEqual(api._effectiveDailyMax("ph").effective, null);
});

check("⛔ une CHAÎNE ne passe pas pour un nombre", () => {
  const { api } = sandbox({ max_ph_ml_per_day: "2000" }, D({ ph_daily_max_ml: "1768" }));
  assert.strictEqual(api._effectiveDailyMax("ph").effective, null,
    "isNaN('') vaut FAUX : un garde sans typeof laisse passer une chaîne");
});

// ================================================ PRIMAUTÉ DU VERDICT FIRMWARE
console.log("L3 la barre ne contredit JAMAIS le firmware");

check("limite atteinte ⇒ barre pleine et rouge, même à 58 % arithmétiques", () => {
  const { api, els } = sandbox(CFG, D({ orp_daily_max_ml: 1734, orp_daily_ml: 1000, orp_limit_reached: true }));
  api.updateDailyDoseDisplay("orp");
  const bar = els.get("#daily-dose-orp-bar");
  assert.strictEqual(bar.style.width, "100%",
    "le dosage est suspendu et la barre montre 58 % — c'est le cas du Mode Boost");
  assert.ok(/danger/.test(bar.className), `variante attendue danger, obtenu « ${bar.className} »`);
  assert.strictEqual(bar.attrs["aria-valuenow"], "100");
});

check("⛔ l'inverse est INTERDIT : barre pleine ≠ « limite atteinte »", () => {
  // Mode Auto : le planning rétrécit en cours de journée, le dénominateur passe
  // SOUS le volume déjà injecté. La barre sature, le texte doit se taire.
  const { api, els } = sandbox(CFG, D({ ph_daily_max_ml: 1200, ph_daily_ml: 1500, ph_limit_reached: false }));
  api.updateDailyDoseDisplay("ph");
  assert.strictEqual(els.get("#daily-dose-ph-bar").style.width, "100%");
  assert.strictEqual(els.get("#daily-dose-ph-status").textContent, "",
    "la barre a conclu toute seule que la limite était atteinte");
});

// ====================================================== LA NOTE D'ÉCART
console.log("L4 la note dit l'écart, sans jamais suggérer un réglage");

check("écart ⇒ la note nomme le chiffre réglé et la cause", () => {
  const { api, els } = sandbox(CFG, D({ ph_daily_max_ml: 1768, ph_daily_ml: 1600 }));
  api.updateDailyDoseDisplay("ph");
  const note = els.get("#daily-dose-ph-note").textContent;
  assert.ok(/2\s*000 mL/.test(note), `le réglage a disparu de l'écran : « ${note} »`);
  assert.ok(/ne peut pas en injecter autant/.test(note), "le constat a disparu");
  // ⛔ Revue 2026-11-22 (M1) : la note ne doit accuser AUCUNE cause précise. Le
  // plafond atteignable dépend du planning de filtration, de la limite horaire ET
  // du débit calibré ; n'en nommer qu'une désigne un coupable parfois innocent
  // (filtration en Marche 24 h/24 avec une limite horaire basse).
  assert.ok(!/filtration|durée de filtration|limite horaire/i.test(note),
    `la note désigne une cause unique : « ${note} »`);
});

check("⛔ aucun impératif, aucun nom de réglage dans la note", () => {
  const { api, els } = sandbox(CFG, D({ ph_daily_max_ml: 1768, ph_daily_ml: 1600 }));
  api.updateDailyDoseDisplay("ph");
  const note = els.get("#daily-dose-ph-note").textContent;
  // Relever la limite horaire défait la garde qui protège vraiment ; relever la
  // consigne ORP ne délivre pas un mL de plus et pousse le pH vers 7,60.
  assert.ok(!/augmentez|réglez|modifiez|relevez|consigne|limite horaire/i.test(note),
    `la note pousse vers un réglage : « ${note} »`);
});

check("aucun écart ⇒ AUCUNE note", () => {
  const { api, els } = sandbox(CFG, D({ ph_daily_max_ml: 2000, ph_daily_ml: 1600 }));
  api.updateDailyDoseDisplay("ph");
  assert.strictEqual(els.get("#daily-dose-ph-note").textContent, "");
});

check("plafond matériel indisponible ⇒ aucune note, ⛔ jamais « Indisponible »", () => {
  const { api, els } = sandbox(CFG, D({ ph_daily_max_ml: null, ph_daily_ml: 1600 }));
  api.updateDailyDoseDisplay("ph");
  assert.strictEqual(els.get("#daily-dose-ph-note").textContent, "");
  assert.ok(!/Indisponible/i.test(els.get("#daily-dose-ph-value").textContent));
});

check("⭐ le STOCK passe devant : bidon au bout ⇒ pas de note de plafond", () => {
  // 2026-11-22 : orp_remaining_ml = 0 sur l'appareil réel. Annoncer un plafond
  // inatteignable donnerait une explication FAUSSE d'un symptôme VRAI.
  const { api, els } = sandbox(CFG, D({
    orp_daily_max_ml: 1734, orp_daily_ml: 1600, orp_remaining_ml: 0, orp_ttt_state: 3 }));
  api.updateDailyDoseDisplay("orp");
  assert.strictEqual(els.get("#daily-dose-orp-note").textContent, "",
    "la note de plafond masque le seul geste utile : vérifier le bidon");
});

check("stock faible ⇒ pas de note non plus", () => {
  const { api, els } = sandbox(CFG, D({
    ph_daily_max_ml: 1768, ph_daily_ml: 1600, ph_remaining_ml: 325 }));
  api.updateDailyDoseDisplay("ph");
  assert.strictEqual(els.get("#daily-dose-ph-note").textContent, "");
});

check("suivi de produit désactivé ⇒ le stock ne masque rien", () => {
  const { api, els } = sandbox(CFG, D({
    ph_daily_max_ml: 1768, ph_daily_ml: 1600, ph_remaining_ml: 0, ph_tracking_enabled: false }));
  api.updateDailyDoseDisplay("ph");
  assert.ok(/ne peut pas en injecter autant/.test(els.get("#daily-dose-ph-note").textContent),
    "sans suivi de produit, il n'y a aucun stock à opposer");
});

// ========================================= LE PLAFOND RÉELLEMENT OPPOSÉ
console.log("L4bis l'écran ne promet jamais plus que le firmware n'autorise");

check("Boost : le firmware oppose 1 000 alors que 2 000 sont réglés ⇒ 1 000", () => {
  // Le cas qui a motivé la publication de la clé : sans elle, la barre affichait
  // « 1 000 / 1 734 mL » en vert pendant que le journal écrivait
  // « LIMITE JOURNALIÈRE CHLORE ATTEINTE ».
  const { api } = sandbox(CFG, D({ orp_daily_max_ml: 1734, orp_effective_max_ml_per_day: 1000 }));
  const lim = api._effectiveDailyMax("orp");
  assert.strictEqual(lim.effective, 1000, "l'écran promet plus que le firmware n'autorise");
  assert.strictEqual(lim.limitedByCapacity, true, "l'écart au réglage doit être expliqué");
});

check("le firmware accorde PLUS que le réglage ⇒ aucun écart à expliquer", () => {
  // Boost sous le genou : 500 réglés, 750 opposés. ⛔ Surtout ne rien suggérer.
  const { api } = sandbox({ max_chlorine_ml_per_day: 500 },
    D({ orp_daily_max_ml: 1734, orp_effective_max_ml_per_day: 750 }));
  const lim = api._effectiveDailyMax("orp");
  assert.strictEqual(lim.effective, 750);
  assert.strictEqual(lim.limitedByCapacity, false);
});

check("côté pH la clé n'existe pas ⇒ repli sur le réglage, sans bruit", () => {
  const { api } = sandbox(CFG, D({ ph_daily_max_ml: 1768 }));
  const lim = api._effectiveDailyMax("ph");
  assert.strictEqual(lim.enforced, 2000, "aucun boost pH n'existe : le réglage fait foi");
  assert.strictEqual(lim.effective, 1768);
});

// ================================================== LE SEGMENT « CUMUL XX % »
console.log("L5 le tableau de bord compte sur le même plafond");

check("cumul calculé sur la limite opposable, pas sur le réglage", () => {
  const { api } = sandbox(CFG, D({ ph_daily_max_ml: 1768 }));
  const lim = api._effectiveDailyMax("ph");
  const seg = api.dailyCumulSegment(1400, lim.effective, false);
  assert.ok(seg, "aucun segment : 1400/1768 = 79 %, au-dessus du seuil de 75 %");
  assert.strictEqual(seg.text, "Cumul 79 %",
    "1400/2000 donnerait 70 %, et le segment resterait muet — c'est le défaut");
});

check("ARRONDI et plafond à 100 % conservés", () => {
  const { api } = sandbox(CFG, D({ ph_daily_max_ml: 1768 }));
  assert.strictEqual(api.dailyCumulSegment(2000, 1768, false).text, "Cumul 100 %");
});

// ============================== CAS TROUVÉS EN REVUE (2026-11-22)
console.log("L7 les cas que la revue de code a trouvés manquants");

check("⭐ réglage à ZÉRO ≠ réglage absent (M2)", () => {
  // min="0" est saisissable, et le firmware traite 0 comme « tout bloqué ».
  // Le confondre avec « absent » faisait afficher « 0 / 1 768 mL » barre PLEINE.
  const { api, els } = sandbox({ max_ph_ml_per_day: 0 },
    D({ ph_daily_max_ml: 1768, ph_daily_ml: 0, ph_limit_reached: true }));
  assert.strictEqual(api._effectiveDailyMax("ph").effective, null,
    "le zéro réglé a été pris pour une donnée manquante");
  api.updateDailyDoseDisplay("ph");
  const txt = els.get("#daily-dose-ph-value").textContent;
  assert.ok(!/1\s*768/.test(txt), `un plafond non voulu est annoncé : « ${txt} »`);
  assert.strictEqual(els.get("#daily-dose-ph-status").textContent, "Limite non configurée");
});

check("⭐ les DEUX dénominateurs sont le MÊME (barre ↔ segment Cumul)", () => {
  // Piège n° 1 du projet : deux expressions pour la même notion divergent. Ici on
  // ne compare pas les intentions, on compare les SORTIES sur la même donnée.
  const data = D({ ph_daily_max_ml: 1768, ph_daily_ml: 1400 });
  const { api, els } = sandbox(CFG, data);
  api.updateDailyDoseDisplay("ph");
  const affiche = els.get("#daily-dose-ph-value").textContent;
  const seg = api.dailyCumulSegment(1400, api._effectiveDailyMax("ph").effective, false);
  const pctBarre = Number(els.get("#daily-dose-ph-bar").attrs["aria-valuenow"]);
  assert.strictEqual(seg.text, `Cumul ${pctBarre} %`,
    `la carte et le tableau de bord comptent différemment : « ${affiche} » vs « ${seg.text} »`);
});

check("valeur aberrante du firmware (uint32 débordé) ⇒ jamais affichée", () => {
  // B1 : un réglage négatif traversait /set-config sans validation et ressortait
  // en 4 294 967 291. Le firmware sert désormais null ; le client doit tenir aussi.
  const { api, els } = sandbox(CFG, D({ orp_effective_max_ml_per_day: null, orp_daily_max_ml: 1734 }));
  assert.strictEqual(api._effectiveDailyMax("orp").effective, 1734,
    "null doit faire retomber sur le réglage et la capacité, pas tout casser");
  api.updateDailyDoseDisplay("orp");
  assert.ok(!/\d{10}/.test(els.get("#daily-dose-orp-value").textContent));
});

check("valeur inconnue ⇒ aucun pourcentage annoncé aux lecteurs d'écran", () => {
  const { api, els } = sandbox({}, D({}));
  api.updateDailyDoseDisplay("ph");
  const bar = els.get("#daily-dose-ph-bar");
  assert.strictEqual(bar.attrs["aria-valuenow"], undefined,
    "« 0 % » serait annoncé là où la valeur est INCONNUE");
  assert.strictEqual(bar.attrs["aria-valuetext"], "indisponible");
});

check("limite atteinte ⇒ l'aria énonce le verdict, pas un pourcentage contradictoire", () => {
  const { api, els } = sandbox(CFG, D({ orp_daily_max_ml: 1734, orp_daily_ml: 1000, orp_limit_reached: true }));
  api.updateDailyDoseDisplay("orp");
  const t = els.get("#daily-dose-orp-bar").attrs["aria-valuetext"];
  assert.ok(/limite journalière atteinte/.test(t), `aria contradictoire : « ${t} »`);
  assert.ok(!/100 %/.test(t), "« 100 % » contredit les deux nombres de la même phrase");
});

// ======================================================== FORMAT DES NOMBRES
console.log("L6 les deux nombres de l'en-tête au MÊME format");

check("l'en-tête porte un séparateur de milliers des deux côtés", () => {
  const { api, els } = sandbox(CFG, D({ ph_daily_max_ml: 1768, ph_daily_ml: 1600 }));
  api.updateDailyDoseDisplay("ph");
  const txt = els.get("#daily-dose-ph-value").textContent;
  assert.ok(!/\b\d{4}\b/.test(txt),
    `un nombre de quatre chiffres collés subsiste (« 2000 » à côté de « 1 768 ») : « ${txt} »`);
  assert.ok(/mL$/.test(txt), `unité attendue en fin de libellé : « ${txt} »`);
});

console.log(`\n${ran - failures}/${ran} vecteurs conformes`);
process.exit(failures ? 1 : 0);
