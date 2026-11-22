// Vecteurs de la carte « Correction manuelle de la mesure » rendue PERMANENTE
// (bug-201). Harnais neuf, écrit par test-engineer, validé par mutation.
//
// POURQUOI CE FICHIER : bug-201 sort `#ph-card-tune` du flux maintenance (le « péage » :
// entrer en maintenance gèle le dosage) et pose, en échange, une FRICTION sur le seul
// geste destructif. Aucune de ces garanties n'est visible dans un test de logique pure :
// elles vivent dans le MARQUAGE (`index.html`), dans une LISTE (`cardIds`), dans l'ORDRE
// des nœuds, dans le FOCUS, dans le NOMBRE d'écritures DOM. Ce harnais les vérifie sur
// le code VIVANT : `data/app.js` est EXTRAIT (ancre + comptage d'accolades,
// `test/js_extract.js`, ADR-0099) et évalué contre un DOM factice ; `data/index.html`
// est lu tel quel. Aucune recopie.
//
// ⛔ Les identifiants de condition sont toujours préfixés : trois séries coexistent
// (feature-168, feature-170, bug-201). Un `C49` nu est ambigu.
//
// Exécution : `node test/ph_card_permanent_vectors.js`
//             (ou `./tools/check_ph_card_permanent_vectors.sh`).

"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const { extractBalancedFromAnchor } = require("./js_extract");

const APP_JS_PATH = process.env.PH_CARD_APP_JS || path.join(__dirname, "..", "data", "app.js");
const INDEX_PATH = process.env.PH_CARD_INDEX_HTML || path.join(__dirname, "..", "data", "index.html");
const src = fs.readFileSync(APP_JS_PATH, "utf8");
const html = fs.readFileSync(INDEX_PATH, "utf8");

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

// ---------------------------------------------------------------------------
// Utilitaires HTML (sans dépendance : on lit des balises, pas un arbre complet)
// ---------------------------------------------------------------------------

// Retire les commentaires HTML : plusieurs citent des identifiants pour expliquer
// POURQUOI ils ont disparu — un test ne doit pas les confondre avec du marquage.
const htmlLive = html.replace(/<!--[\s\S]*?-->/g, "");

function openingTag(id) {
  const m = htmlLive.match(new RegExp(`<([a-zA-Z0-9]+)\\b[^>]*\\bid="${id}"[^>]*>`));
  return m ? m[0] : null;
}
function hasClass(tag, cls) {
  const m = tag && tag.match(/\bclass="([^"]*)"/);
  return !!m && m[1].split(/\s+/).includes(cls);
}
function hasBoolAttr(tag, attr) {
  return !!tag && new RegExp(`\\s${attr}(\\s|>|=)`).test(tag);
}
// Fragment HTML d'un élément identifié, par équilibrage des balises de MÊME nom.
function elementHtml(id) {
  const tag = openingTag(id);
  if (!tag) return null;
  const name = tag.match(/^<([a-zA-Z0-9]+)/)[1];
  const start = htmlLive.indexOf(tag);
  const re = new RegExp(`<(/?)${name}\\b[^>]*>`, "g");
  re.lastIndex = start;
  let depth = 0, m;
  while ((m = re.exec(htmlLive))) {
    depth += m[1] ? -1 : 1;
    if (depth === 0) return htmlLive.slice(start, re.lastIndex);
  }
  return null;
}
const plain = (h) => h.replace(/<[^>]+>/g, "").replace(/\s+/g, " ").trim();

// ===========================================================================
// bug-201 (C40) — la carte est hors du péage. Deux gestes opposés, DEUX tests.
// ===========================================================================

{
  const tag = openingTag("ph-card-tune");
  check("bug-201 (C40) #ph-card-tune existe", tag !== null, true);
  check("bug-201 (C40) #ph-card-tune SANS `hidden` (permanente)", hasBoolAttr(tag, "hidden"), false);
  // 🔴 Le piège inverse : retirer la CLASSE au lieu de l'entrée de cardIds ferait masquer
  // la carte par `#view-ph.is-flow-maintenance > :not(.flow-maintenance)` PENDANT le flux
  // d'étalonnage, c'est-à-dire quand la cause « calibration » vient de se lever.
  check("bug-201 (C40) #ph-card-tune GARDE la classe .flow-maintenance", hasClass(tag, "flow-maintenance"), true);
  check("bug-201 (C40) la carte d'étalonnage EZO reste, elle, derrière le péage (hidden)",
        hasBoolAttr(openingTag("ph-card-calibration"), "hidden"), true);
}

{
  // cardIds de la page pH : on lit le tableau littéral, pas les commentaires voisins.
  const at = src.indexOf('viewId: "view-ph"');
  if (at === -1) throw new Error('ancre `viewId: "view-ph"` introuvable dans data/app.js');
  const rest = src.slice(at);
  const m = rest.match(/cardIds:\s*(\[[^\]]*\])/);
  if (!m) throw new Error("`cardIds` de la page pH introuvable");
  const cardIds = JSON.parse(m[1]);
  check("bug-201 (C40) cardIds de la page pH ne contient PAS ph-card-tune", cardIds.includes("ph-card-tune"), false);
  check("bug-201 (C40) cardIds de la page pH contient ph-card-calibration", cardIds.includes("ph-card-calibration"), true);
}

// ===========================================================================
// bug-201 (C49) — « Remettre à zéro » n'existe plus ; UN SEUL contrôle destructif.
// ===========================================================================

check("bug-201 (C49) #ph_tune_stale_cancel_btn absent du marquage",
      /id="ph_tune_stale_cancel_btn"/.test(htmlLive), false);
check("bug-201 (C49) #ph_tune_stale_cancel_btn n'est plus référencé par app.js (sélecteur)",
      /["'`]#ph_tune_stale_cancel_btn["'`]/.test(src.replace(/\/\/.*$/gm, "")), false);
check("bug-201 (C49) le bandeau garde « Je l'ai vérifiée — garder »",
      /id="ph_tune_stale_keep_btn"[^>]*>\s*Je l'ai vérifiée — garder\s*</.test(htmlLive), true);

{
  // bug-201 (C48) : tout contrôle destructif de la carte vit dans la zone unique.
  const card = elementHtml("ph-card-tune");
  const zone = elementHtml("ph_tune_remove_zone");
  const banner = elementHtml("ph_tune_stale_banner");
  const destructive = (h) => (h.match(/<button\b[^>]*\b(action-destructive|btn--danger)\b[^>]*>/g) || []);
  check("bug-201 (C48) aucun contrôle destructif dans le bandeau", destructive(banner).length, 0);
  check("bug-201 (C48) tous les contrôles destructifs de la carte sont dans #ph_tune_remove_zone",
        destructive(card).length, destructive(zone).length);
  check("bug-201 (C48) la zone ne contient que le bouton et son remplaçant (2)", destructive(zone).length, 2);
}

// ===========================================================================
// bug-201 (C47) — ordre du DOM : le choix sûr d'abord, le destructif ensuite.
// ===========================================================================

{
  const confirm = elementHtml("ph_tune_remove_confirm");
  const iSafe = confirm.indexOf('id="ph_tune_remove_cancel_btn"');
  const iDestr = confirm.indexOf('id="ph_tune_remove_confirm_btn"');
  check("bug-201 (C47) « Ne rien changer » présent", iSafe >= 0, true);
  check("bug-201 (C47) « Ne rien changer » AVANT le destructif dans le DOM", iSafe >= 0 && iDestr > iSafe, true);
  check("bug-201 (C47) le bloc est masqué au repos", hasBoolAttr(openingTag("ph_tune_remove_confirm"), "hidden"), true);
  check("bug-201 (C47) le bouton d'origine est masqué au repos",
        hasBoolAttr(openingTag("ph_tune_remove_btn"), "hidden"), true);
  check("bug-201 (C47) le texte de friction est dans le bloc (aucun texte inerte entre phrase et boutons)",
        confirm.indexOf('id="ph_tune_remove_confirm_text"') >= 0
          && confirm.indexOf('id="ph_tune_remove_confirm_text"') < iSafe, true);
}

// ===========================================================================
// bug-201 (C50) — les deux phrases gelées, mot pour mot, dans le DOM rendu.
// ===========================================================================

{
  const stakes = elementHtml("ph_tune_stakes");
  check("bug-201 (C50) #ph_tune_stakes toujours présent (suppression REFUSÉE)", stakes !== null, true);
  const text = plain(stakes);
  check("bug-201 (C50) « L'écart vient de l'installation, pas de la sonde. » mot pour mot",
        text.includes("L'écart vient de l'installation, pas de la sonde."), true);
  const P2 = "Ce réglage ne vaut que ce que vaut votre test. La valeur que vous indiquez ne change pas que l'affichage : c'est elle qui décidera des injections d'acide.";
  check("bug-201 (C50) second paragraphe entier mot pour mot", text.includes(P2), true);
  // Le GRAS du second paragraphe est exigé (le gras de la 1re phrase a été retiré
  // volontairement : « quand tout est en gras, plus rien n'est souligné »).
  const strongs = (stakes.match(/<strong>[\s\S]*?<\/strong>/g) || []).map(plain);
  check("bug-201 (C50) le second paragraphe est EN GRAS en entier", strongs.includes(P2), true);
  check("bug-201 (C50) ni <details> ni repli autour du second paragraphe", /<details/.test(stakes), false);
}

// ===========================================================================
// bug-201 (C51) — marquage : refus et avertissement sont DEUX éléments distincts.
// ===========================================================================

{
  const refusTag = openingTag("ph_tune_refus");
  check("bug-201 (C51) #ph_tune_refus existe, en .field__hint", hasClass(refusTag, "field__hint"), true);
  check("bug-201 (C51) #ph_tune_refus n'est PAS .muted (un refus n'est pas un conseil)", hasClass(refusTag, "muted"), false);
  check("bug-201 (C51) #ph_tune_refus est EN DEHORS de #ph_tune_blocked",
        elementHtml("ph_tune_blocked").includes('id="ph_tune_refus"'), false);
  check("bug-201 (C51) #ph_tune_blocked reste un callout--warn",
        hasClass(openingTag("ph_tune_blocked"), "callout--warn"), true);
  const blocked = elementHtml("ph_tune_blocked");
  check("bug-201 (C51) #ph_tune_blocked garde chapeau + repliable (feature-168 (C8))",
        [/id="ph_tune_blocked_eyebrow"/.test(blocked), /<details\b/.test(blocked)], [true, true]);
  check("bug-201 (C51) « Corriger » décrit par #ph_tune_refus (aria-describedby)",
        /aria-describedby="ph_tune_refus"/.test(openingTag("ph_tune_submit_btn")), true);
  const cardHtml = elementHtml("ph-card-tune");
  const iBtn = cardHtml.indexOf('id="ph_tune_submit_btn"');
  const iRefus = cardHtml.indexOf('id="ph_tune_refus"');
  const afterBtn = cardHtml.slice(cardHtml.indexOf("</button>", iBtn) + "</button>".length, cardHtml.lastIndexOf("<", iRefus));
  check("bug-201 (C51) #ph_tune_refus ADJACENT à « Corriger » (rien entre eux qu'une fermeture de bloc)",
        iRefus > iBtn && /^\s*<\/div>\s*$/.test(afterBtn), true);
}

// ---------------------------------------------------------------------------
// DOM factice : assez pour exécuter le vrai code extrait, rien de plus.
// ---------------------------------------------------------------------------

class FakeEl {
  constructor(id, { hidden = false, text = "", ancestry = [] } = {}) {
    this.id = id;
    this._hidden = hidden;
    this._text = text;
    this._kids = null;
    this.disabled = false;
    this.ancestry = [id, ...ancestry];
    this.listeners = {};
    this.writes = { text: 0, hidden: 0 };
    this.ops = [];          // ordre des écritures (bug-201 (C51) : écriture / affichage)
    this.announced = [];    // textes écrits ALORS QUE l'élément était dans l'arbre d'accessibilité
    this.focusCalls = 0;
    this.classes = new Set();
    this.members = new Set();
    this.classList = {
      add: (...c) => c.forEach((x) => this.classes.add(x)),
      remove: (...c) => c.forEach((x) => this.classes.delete(x)),
      toggle: (c, on) => { if (on) this.classes.add(c); else this.classes.delete(c); },
    };
    this.style = {};
  }
  get hidden() { return this._hidden; }
  set hidden(v) { this.writes.hidden++; this.ops.push("hidden:" + v); this._hidden = v; }
  get textContent() { return this._kids ? this._kids.map((k) => k.textContent).join("") : this._text; }
  set textContent(v) {
    this.writes.text++; this.ops.push("text:" + v); this._kids = null; this._text = String(v);
    // Une région role="status" n'annonce que ce qui s'écrit pendant qu'elle est rendue.
    if (!this._hidden && String(v) !== "") this.announced.push(String(v));
  }
  replaceChildren(...k) { this.writes.text++; this._kids = k; this._text = ""; }
  setAttribute() {}
  addEventListener(type, fn) { (this.listeners[type] = this.listeners[type] || []).push(fn); }
  contains(n) { return n === this || this.members.has(n); }
  closest(sel) {
    const ids = sel.split(",").map((s) => s.trim().replace(/^#/, ""));
    return ids.some((i) => this.ancestry.includes(i)) ? this : null;
  }
  focus() { this.focusCalls++; fake.document.activeElement = this; }
}

const fake = { document: null };

function makeWorld() {
  const els = {};
  const add = (id, opts) => (els[id] = new FakeEl(id, opts));
  // Carte de correction
  add("ph_tune_value"); add("ph_tune_dec"); add("ph_tune_inc"); add("ph_tune_result");
  add("ph_tune_stakes");
  add("ph_tune_blocked", { hidden: true });
  add("ph_tune_blocked_eyebrow", { hidden: true });
  add("ph_tune_blocked_lead"); add("ph_tune_blocked_more", { hidden: true }); add("ph_tune_blocked_detail");
  add("ph_tune_refus", { hidden: true });
  add("ph_tune_submit_btn"); add("ph_tune_error");
  add("ph_tune_stale_keep_btn");
  // Zone destructive
  add("ph_tune_remove_btn", { hidden: false, ancestry: ["ph_tune_remove_zone"] });
  add("ph_tune_remove_hint", { hidden: false });
  add("ph_tune_remove_confirm", { hidden: true, ancestry: ["ph_tune_remove_zone"] });
  add("ph_tune_remove_confirm_text", { ancestry: ["ph_tune_remove_confirm", "ph_tune_remove_zone"] });
  add("ph_tune_remove_cancel_btn", { ancestry: ["ph_tune_remove_confirm", "ph_tune_remove_zone"] });
  add("ph_tune_remove_confirm_btn", { ancestry: ["ph_tune_remove_confirm", "ph_tune_remove_zone"] });
  ["ph_tune_remove_confirm_text", "ph_tune_remove_cancel_btn", "ph_tune_remove_confirm_btn"]
    .forEach((i) => els.ph_tune_remove_confirm.members.add(els[i]));
  const outside = new FakeEl("ailleurs");

  const docListeners = {};
  const winListeners = {};
  const document = {
    activeElement: null,
    createTextNode: (t) => ({ textContent: t, isText: true }),
    createElement: (tag) => { const e = new FakeEl("<" + tag + ">"); e.tag = tag; return e; },
    addEventListener: (t, f) => { (docListeners[t] = docListeners[t] || []).push(f); },
  };
  fake.document = document;
  const window = { _config: {}, addEventListener: (t, f) => { (winListeners[t] = winListeners[t] || []).push(f); } };
  const calls = { send: [], toast: [], loadConfig: 0, authFetch: [], sendImpl: null };
  const stubs = {
    $: (sel) => (els[sel.replace(/^#/, "")] || null),
    document, window,
    sendConfigDetailed: async (body) => { calls.send.push(body); return calls.sendImpl ? calls.sendImpl(body) : { ok: true }; },
    showToast: (m, t) => calls.toast.push([m, t]),
    loadConfig: () => { calls.loadConfig++; },
    authFetch: async (url, opts) => { calls.authFetch.push([url, opts]); return { ok: true, json: async () => ({}) }; },
    setBtnSpinnerContent: () => {},
    setTimeout: () => 0,
  };
  return { els, outside, document, window, docListeners, winListeners, calls, stubs };
}

// Code extrait (jamais recopié). Les constantes de configuration servies sont posées en
// préambule ; tout ce qui DÉCIDE (rendus, friction, gardes) vient de data/app.js.
function fn(sig) { return extractBalancedFromAnchor(src, sig, "{", "}").text; }
function mustHave(re, label) {
  if (!re.test(src)) throw new Error("déclaration d'état introuvable dans data/app.js : " + label);
}
mustHave(/^\s*let _phTuneValue = null;/m, "_phTuneValue");
mustHave(/^\s*let _phTuneTouched = false;/m, "_phTuneTouched");
mustHave(/^\s*let _phTuneSubmitBusy = false;/m, "_phTuneSubmitBusy");
mustHave(/^\s*let _phTuneRemoveArmed = false;$/m, "_phTuneRemoveArmed");

const extracted = [
  fn("function _fmt2(v) {"),
  fn("function _fmtPh(v, signed) {"),
  fn("function _fmtPhOffset2(v) {"),
  fn("function _setTextIfChanged(el, text) {"),
  fn("function _setHiddenIfChanged(el, hidden) {"),
  fn("function evaluatePhTunePreview(valeurAmenee, phValueRaw, offsetStep, unusualAbs, impasseThreshold, impasseThresholdHigh, impasseEpsilon) {"),
  fn("function _phTuneRoundTenth(v) {"),
  fn("function _phTuneLiveRaw() {"),
  fn("function renderPhTuneCard() {"),
  fn("function phTuneRemoveNeedsFriction(offset, unusualAbs) {"),
  fn("function phTuneRemoveWarningParts(offset, ph, phValueRaw) {"),
  fn("function _phTuneRemoveDisarm(restoreFocus) {"),
  fn("function _phTuneRemoveArm() {"),
  fn("function renderPhTuneRemoveRow(cfg) {"),
  fn("function bindPhTuneRemoveRow() {"),
  fn("function bindPhOffsetStaleBanner() {"),
  fn("function bindPhTuneSubmit() {"),
].join("\n");

const factory = new Function(
  "$", "document", "window", "sendConfigDetailed", "showToast", "loadConfig", "authFetch",
  "setBtnSpinnerContent", "setTimeout",
  `"use strict";
   let latestSensorData = null;
   let _phBounds = { offsetStep: 0.10 };
   let _phOffsetThresholds = { unusualAbs: 0.5, impasseThreshold: 1.2, impasseThresholdHigh: -0.9,
                               rawFloorAbsolute: 5.5, rawCeilingAbsolute: 8.5 };
   let _phTuneCalPointsMin = 2;
   let _phTuneMinFiltrationMs = 600000;
   const PH_EPS = 1e-9;
   const PH_TUNE_SCALE_FLOOR = 0;
   const PH_TUNE_SCALE_CEIL = 14;
   let _phTuneValue = null;
   let _phTuneTouched = false;
   let _phTuneSubmitBusy = false;
   let _phTuneRemoveArmed = false;
   function bindPhTuneStepper() {}
   ${extracted}
   return {
     renderPhTuneCard, renderPhTuneRemoveRow, bindPhTuneRemoveRow, bindPhOffsetStaleBanner,
     bindPhTuneSubmit, phTuneRemoveWarningParts,
     setSensors: (s) => { latestSensorData = s; },
     setThreshold: (v) => { _phOffsetThresholds.unusualAbs = v; },
     armed: () => _phTuneRemoveArmed,
     touch: (v) => { _phTuneTouched = true; _phTuneValue = v; },
   };`
);

function boot() {
  const w = makeWorld();
  const api = factory(w.stubs.$, w.document, w.window, w.stubs.sendConfigDetailed, w.stubs.showToast,
                      w.stubs.loadConfig, w.stubs.authFetch, w.stubs.setBtnSpinnerContent, w.stubs.setTimeout);
  return { w, api };
}

// Un clic réel : l'élément d'abord, puis le document (remontée de l'évènement).
async function click(w, el) {
  const ev = { target: el, key: undefined, preventDefault() {} };
  for (const f of el.listeners.click || []) await f(ev);
  for (const f of w.docListeners.click || []) await f(ev);
}
function pressKey(w, key) {
  for (const f of w.docListeners.keydown || []) f({ key, target: w.outside });
}

const SENS_OK = { ph: 7.3, phValueRaw: 6.1, phRaw: 5.5, filtration_running: true, filtration_elapsed_s: 3600, phCalPoints: 2 };

(async () => {

// ===========================================================================
// bug-201 (C41) — le seuil de friction est LU sur la clé servie, strict.
// ===========================================================================
{
  // Seuil servi 0,80 ; correction 0,60 : directement supprimée (aucune friction).
  // Mutation « 0,50 en dur » : la friction s'armerait ici.
  const { w, api } = boot();
  w.window._config = { ph_offset: 0.6 };
  api.setSensors(SENS_OK);
  api.setThreshold(0.8);
  api.bindPhTuneRemoveRow();
  await click(w, w.els.ph_tune_remove_btn);
  check("bug-201 (C41) seuil servi 0,80, correction 0,60 : AUCUNE friction, suppression directe",
        [w.calls.send, api.armed()], [[{ ph_offset: 0 }], false]);
}
{
  const { w, api } = boot();
  w.window._config = { ph_offset: 0.5 };
  api.setSensors(SENS_OK);
  api.bindPhTuneRemoveRow();
  await click(w, w.els.ph_tune_remove_btn);
  check("bug-201 (C41) |o| = 0,50 (= seuil servi) : AUCUNE friction", [w.calls.send.length, api.armed()], [1, false]);
}
{
  const { w, api } = boot();
  w.window._config = { ph_offset: 0.6 };
  api.setSensors(SENS_OK);
  api.bindPhTuneRemoveRow();
  await click(w, w.els.ph_tune_remove_btn);
  check("bug-201 (C41) |o| = 0,60 : friction, RIEN n'est envoyé au premier appui", [w.calls.send.length, api.armed()], [0, true]);
}
{
  // o = 0 : aucun bouton destructif ; o = NaN / absent : aucun bouton.
  for (const [label, cfg] of [["o = 0", { ph_offset: 0 }], ["o = NaN", { ph_offset: NaN }],
                              ["o absent", {}], ["o = null", { ph_offset: null }]]) {
    const { w, api } = boot();
    api.renderPhTuneRemoveRow(cfg);
    check(`bug-201 (C41) ${label} : aucun bouton destructif visible`, w.els.ph_tune_remove_btn.hidden, true);
  }
}

// ===========================================================================
// bug-201 (C47) — la forme de la friction : focus, double appui, sorties, un seul clic.
// ===========================================================================
{
  const { w, api } = boot();
  w.window._config = { ph_offset: 1.2 };
  api.setSensors(SENS_OK);
  api.bindPhTuneRemoveRow();
  const { els } = w;
  await click(w, els.ph_tune_remove_btn);
  check("bug-201 (C47) 1er geste : le bouton d'origine disparaît, le bloc apparaît",
        [els.ph_tune_remove_btn.hidden, els.ph_tune_remove_confirm.hidden], [true, false]);
  check("bug-201 (C47) le focus va sur « Ne rien changer » (jamais sur le destructif)",
        [w.document.activeElement === els.ph_tune_remove_cancel_btn, els.ph_tune_remove_confirm_btn.focusCalls], [true, 0]);
  check("bug-201 (C47) la phrase de conséquence est à l'écran avec ses deux nombres lus",
        els.ph_tune_remove_confirm_text.textContent.startsWith("Sans cette correction, l'appareil lira 6,10 au lieu de 7,30 :"), true);

  // Double appui rapide sur le bouton d'origine : le second tombe sur du texte inerte
  // (le bouton est masqué), jamais sur une destruction.
  await click(w, els.ph_tune_remove_btn);
  check("bug-201 (C47) double appui sur le bouton d'origine : toujours RIEN d'envoyé", w.calls.send.length, 0);
  // Un appui sur la zone de texte (inerte) n'envoie rien non plus et ne désarme pas.
  await click(w, els.ph_tune_remove_confirm_text);
  check("bug-201 (C47) appui sur le texte inerte : rien d'envoyé, bloc toujours armé", [w.calls.send.length, api.armed()], [0, true]);

  // Sortie : « Ne rien changer » — défaut sûr, focus rendu au bouton d'origine.
  await click(w, els.ph_tune_remove_cancel_btn);
  check("bug-201 (C47) « Ne rien changer » : désarmé, rien d'envoyé, bouton d'origine revenu",
        [api.armed(), w.calls.send.length, els.ph_tune_remove_btn.hidden, els.ph_tune_remove_confirm.hidden],
        [false, 0, false, true]);
  check("bug-201 (C47) le focus est rendu au bouton d'origine", w.document.activeElement === els.ph_tune_remove_btn, true);

  // Échap
  await click(w, els.ph_tune_remove_btn);
  pressKey(w, "Escape");
  check("bug-201 (C47) Échap referme (défaut sûr)", [api.armed(), w.calls.send.length], [false, 0]);
  pressKey(w, "Enter");
  check("bug-201 (C47) une autre touche n'arme ni ne détruit rien", w.calls.send.length, 0);

  // Clic extérieur
  await click(w, els.ph_tune_remove_btn);
  await click(w, w.outside);
  check("bug-201 (C47) un clic extérieur referme", [api.armed(), w.calls.send.length], [false, 0]);

  // Changement de page
  await click(w, els.ph_tune_remove_btn);
  for (const f of w.winListeners.hashchange || []) f({});
  check("bug-201 (C47) un changement de page désarme", api.armed(), false);

  // Le second geste détruit, UNE fois — même sur double appui.
  await click(w, els.ph_tune_remove_btn);
  await Promise.all([click(w, els.ph_tune_remove_confirm_btn), click(w, els.ph_tune_remove_confirm_btn)]);
  check("bug-201 (C47) second geste : UN envoi {ph_offset: 0}, même sur double appui", w.calls.send, [{ ph_offset: 0 }]);
  check("bug-201 (C47) après la suppression, l'état n'est plus armé", api.armed(), false);
}
{
  // Retour de /get-config (re-rendu de la ligne) : un état armé ne survit jamais.
  const { w, api } = boot();
  w.window._config = { ph_offset: 1.2 };
  api.setSensors(SENS_OK);
  api.bindPhTuneRemoveRow();
  await click(w, w.els.ph_tune_remove_btn);
  api.renderPhTuneRemoveRow({ ph_offset: 1.3 });
  check("bug-201 (C47) retour de /get-config : désarmé, bloc refermé, bouton revenu",
        [api.armed(), w.els.ph_tune_remove_confirm.hidden, w.els.ph_tune_remove_btn.hidden], [false, true, false]);
}

// ===========================================================================
// bug-201 (C47, exigence 6) — la friction ne porte QUE sur le geste destructif.
// ===========================================================================
{
  const { w, api } = boot();
  w.window._config = { ph_offset: 1.2 };
  api.bindPhOffsetStaleBanner();
  await click(w, w.els.ph_tune_stale_keep_btn);
  check("bug-201 (C47) « Je l'ai vérifiée — garder » : UN appui suffit, aucune friction",
        [w.calls.send, api.armed(), w.els.ph_tune_remove_confirm.hidden], [[{ ph_offset: 1.2 }], false, true]);
}
{
  const { w, api } = boot();
  w.window._config = { ph_offset: 1.2 };
  api.setSensors(SENS_OK);
  api.touch(7.4);                       // l'utilisateur a utilisé le pas-à-pas
  api.bindPhTuneSubmit();               // câble « Corriger » (et le rendu initial)
  w.els.ph_tune_submit_btn.classList = { add() {}, remove() {}, toggle() {} };
  w.els.ph_tune_submit_btn.disabled = false;
  await click(w, w.els.ph_tune_submit_btn);
  check("bug-201 (C47) « Corriger » : UN appui suffit (POST /tune-ph-offset), aucune friction",
        [w.calls.authFetch.map((c) => c[0]), api.armed()], [["/tune-ph-offset"], false]);
}

// ===========================================================================
// bug-201 (C45) — le texte de friction choisit sa variante sur le SIGNE (par le vrai
// chemin d'armement, pas seulement par la fonction pure).
// ===========================================================================
{
  const { w, api } = boot();
  w.window._config = { ph_offset: -0.6 };
  api.setSensors({ ph: 7.3, phValueRaw: 7.9, phRaw: 7.9 });
  api.bindPhTuneRemoveRow();
  await click(w, w.els.ph_tune_remove_btn);
  const t = w.els.ph_tune_remove_confirm_text.textContent;
  check("bug-201 (C45) correction NÉGATIVE : variante négative mot pour mot",
        t, "Sans cette correction, l'appareil lira 7,90 au lieu de 7,30 : il ajoutera de l'acide en trop et pourra arrêter le chlore en croyant le pH trop haut. Pour la remettre, il faudra refaire un test manuel.");
  check("bug-201 (C45) correction négative : JAMAIS « il cessera d'ajouter de l'acide »", /cessera d'ajouter/.test(t), false);
}
{
  const { w, api } = boot();
  w.window._config = { ph_offset: 1.2 };
  api.setSensors(SENS_OK);
  api.bindPhTuneRemoveRow();
  await click(w, w.els.ph_tune_remove_btn);
  check("bug-201 (C45) correction POSITIVE : variante positive mot pour mot",
        w.els.ph_tune_remove_confirm_text.textContent,
        "Sans cette correction, l'appareil lira 6,10 au lieu de 7,30 : il cessera d'ajouter de l'acide et laissera le pH monter. Pour la remettre, il faudra refaire un test manuel.");
}

// ===========================================================================
// bug-201 (C46) — phValueRaw et ph LUS, jamais phRaw ; repli obligatoire.
// ===========================================================================
{
  // phRaw volontairement décalé (instantané EZO) : le texte NE DOIT PAS bouger.
  const texts = [];
  for (const phRaw of [5.5, 5.93, 9.99, null, undefined]) {
    const { w, api } = boot();
    w.window._config = { ph_offset: 1.2 };
    api.setSensors({ ph: 7.3, phValueRaw: 6.1, phRaw });
    api.bindPhTuneRemoveRow();
    await click(w, w.els.ph_tune_remove_btn);
    texts.push(w.els.ph_tune_remove_confirm_text.textContent);
  }
  check("bug-201 (C46) phRaw décalé (5,50 / 5,93 / 9,99 / absent) : le texte ne bouge pas", new Set(texts).size, 1);
  check("bug-201 (C46) le texte ne contient jamais le nombre de phRaw", /5,93|9,99|5,50/.test(texts.join("|")), false);
}
{
  // 🔴 L'état RÉEL de l'appareil : ph null, phValueRaw null, phRaw 5,93, correction +1,20.
  const { w, api } = boot();
  w.window._config = { ph_offset: 1.2 };
  api.setSensors({ ph: null, phValueRaw: null, phRaw: 5.93 });
  api.renderPhTuneRemoveRow(w.window._config);
  check("bug-201 (C46) repli : le bouton est visible (la friction ne dépend d'aucune mesure)",
        w.els.ph_tune_remove_btn.hidden, false);
  api.bindPhTuneRemoveRow();
  await click(w, w.els.ph_tune_remove_btn);
  const t = w.els.ph_tune_remove_confirm_text.textContent;
  check("bug-201 (C46) repli : bouton TOUJOURS ferré (armé, rien d'envoyé)", [api.armed(), w.calls.send.length], [true, 0]);
  check("bug-201 (C46) repli : texte bâti sur le seul ph_offset",
        t, "Sans cette correction, l'appareil lira 1,20 pH de moins qu'aujourd'hui : il cessera d'ajouter de l'acide et laissera le pH monter. Pour la remettre, il faudra refaire un test manuel.");
  check("bug-201 (C46) repli : AUCUNE apparition de 5,93 ni de nombre inventé", /5,93|NaN|null|undefined/.test(t), false);
}
{
  // Aucune mesure du tout (latestSensorData nul) : la friction demeure.
  const { w, api } = boot();
  w.window._config = { ph_offset: -0.6 };
  api.setSensors(null);
  api.bindPhTuneRemoveRow();
  await click(w, w.els.ph_tune_remove_btn);
  check("bug-201 (C46) aucune donnée capteur : friction maintenue, texte « de plus »",
        [api.armed(), /0,60 pH de plus qu'aujourd'hui/.test(w.els.ph_tune_remove_confirm_text.textContent)], [true, true]);
}

// ===========================================================================
// bug-201 (C51) — DEUX rendus distincts : refus = texte simple ; feature-168 (C8) = callout--warn.
// ===========================================================================
const REFUS_MESURE = "L'appareil n'a pas de mesure assez récente. Attendez qu'une valeur s'affiche, puis recommencez.";
{
  // Un refus bloquant (mesure absente) : texte dans #ph_tune_refus, rien dans #ph_tune_blocked.
  const { w, api } = boot();
  w.window._config = { ph_offset: 1.2 };
  api.setSensors({ ph: null, phValueRaw: null, filtration_running: true, filtration_elapsed_s: 3600, phCalPoints: 2 });
  api.renderPhTuneCard();
  check("bug-201 (C51) refus : texte mot pour mot dans #ph_tune_refus", w.els.ph_tune_refus.textContent, REFUS_MESURE);
  check("bug-201 (C51) refus : #ph_tune_refus visible", w.els.ph_tune_refus.hidden, false);
  check("bug-201 (C51) refus : l'encadré feature-168 (C8) reste MASQUÉ et vide (pas de refus dedans)",
        [w.els.ph_tune_blocked.hidden, w.els.ph_tune_blocked_lead.textContent, w.els.ph_tune_blocked_eyebrow.hidden],
        [true, "", true]);
  check("bug-201 (C51) refus : « Corriger » désactivé", w.els.ph_tune_submit_btn.disabled, true);
}
{
  const { w, api } = boot();
  w.window._config = { ph_offset: 0 };
  api.setSensors({ ph: 7.3, phValueRaw: 7.3, filtration_running: false, filtration_elapsed_s: 0, phCalPoints: 2 });
  api.renderPhTuneCard();
  check("bug-201 (C51) refus filtration : texte dans #ph_tune_refus",
        w.els.ph_tune_refus.textContent.startsWith("La filtration doit tourner depuis 10 minutes"), true);
}
{
  const { w, api } = boot();
  w.window._config = { ph_offset: 0 };
  api.setSensors({ ph: 7.3, phValueRaw: 7.3, filtration_running: true, filtration_elapsed_s: 3600, phCalPoints: 1 });
  api.renderPhTuneCard();
  check("bug-201 (C51) refus étalonnage : texte dans #ph_tune_refus",
        w.els.ph_tune_refus.textContent.startsWith("La sonde n'a pas encore ses deux points de référence."), true);
}
{
  // L'avertissement feature-168 (C8) : écart > seuil, aucun refus.
  const { w, api } = boot();
  w.window._config = { ph_offset: 1.2, ph_correction_type: "ph_minus", ph_impasse_low: false, ph_impasse_high: false };
  api.setSensors(SENS_OK);
  api.renderPhTuneCard();
  const e = w.els;
  check("bug-201 (C51) feature-168 (C8) : l'encadré callout--warn est affiché, #ph_tune_refus MASQUÉ et vide",
        [e.ph_tune_blocked.hidden, e.ph_tune_refus.hidden, e.ph_tune_refus.textContent], [false, true, ""]);
  check("bug-201 (C51) feature-168 (C8) : le chapeau est affiché et dit « Avertissement »",
        [e.ph_tune_blocked_eyebrow.hidden, e.ph_tune_blocked_eyebrow.textContent], [false, "Avertissement"]);
  check("bug-201 (C51) feature-168 (C8) : le lead est porté par l'encadré (1re phrase)",
        e.ph_tune_blocked_lead.textContent.startsWith("Au-delà de 0,50 pH, l'appareil régule sur une valeur qu'il ne mesure pas"), true);
  check("bug-201 (C51) feature-168 (C8) : le détail repliable est disponible et rempli",
        [e.ph_tune_blocked_more.hidden, e.ph_tune_blocked_detail.textContent.length > 100], [false, true]);
  check("bug-201 (C51) feature-168 (C8) : « Corriger » N'est PAS bloqué par l'avertissement (non bloquant)",
        e.ph_tune_submit_btn.disabled, true /* !touched : désactivé, mais pas par un refus */);
  check("bug-201 (C51) feature-168 (C8) : l'enjeu #ph_tune_stakes s'efface (jamais empilé avec feature-168 (C8))", e.ph_tune_stakes.hidden, true);
}
{
  // Avertissement, une fois le pas-à-pas touché : le bouton est ACTIF malgré feature-168 (C8).
  const { w, api } = boot();
  w.window._config = { ph_offset: 1.2, ph_correction_type: "ph_minus", ph_impasse_low: false, ph_impasse_high: false };
  api.setSensors(SENS_OK);
  api.touch(7.3);
  api.renderPhTuneCard();
  check("bug-201 (C51) feature-168 (C8) non bloquant : « Corriger » actif quand le pas-à-pas a été touché",
        [w.els.ph_tune_blocked.hidden, w.els.ph_tune_submit_btn.disabled], [false, false]);
}
{
  // Le passage refus -> feature-168 (C8) -> refus ne laisse JAMAIS un texte de l'un dans l'autre.
  const { w, api } = boot();
  w.window._config = { ph_offset: 1.2, ph_correction_type: "ph_minus", ph_impasse_low: false, ph_impasse_high: false };
  api.setSensors(SENS_OK);
  api.renderPhTuneCard();
  api.setSensors({ ...SENS_OK, ph: null, phValueRaw: null });
  api.renderPhTuneCard();
  check("bug-201 (C51) feature-168 (C8) puis refus : l'encadré est vidé et masqué, le refus affiché",
        [w.els.ph_tune_blocked.hidden, w.els.ph_tune_blocked_lead.textContent, w.els.ph_tune_refus.hidden, w.els.ph_tune_refus.textContent],
        [true, "", false, REFUS_MESURE]);
}

// ===========================================================================
// bug-201 (a11y) — n'écrire QUE ce qui a changé : trois rendus identiques = zéro
// écriture supplémentaire (sinon un lecteur d'écran relit l'alerte toutes les 30 s).
// ===========================================================================
const WATCHED = ["ph_tune_refus", "ph_tune_blocked", "ph_tune_blocked_eyebrow", "ph_tune_blocked_lead",
                 "ph_tune_blocked_more", "ph_tune_blocked_detail", "ph_tune_stakes",
                 "ph_tune_remove_btn", "ph_tune_remove_hint"];
function snapshotWrites(w) {
  const o = {};
  for (const id of WATCHED) o[id] = { ...w.els[id].writes };
  return o;
}
for (const [label, cfg, sens] of [
  ["refus", { ph_offset: 1.2 }, { ph: null, phValueRaw: null, filtration_running: true, filtration_elapsed_s: 3600, phCalPoints: 2 }],
  ["avertissement feature-168 (C8)", { ph_offset: 1.2, ph_correction_type: "ph_minus", ph_impasse_low: false, ph_impasse_high: false }, SENS_OK],
  ["aucun message", { ph_offset: 0 }, { ph: 7.3, phValueRaw: 7.3, filtration_running: true, filtration_elapsed_s: 3600, phCalPoints: 2 }],
]) {
  const { w, api } = boot();
  w.window._config = cfg;
  api.setSensors(sens);
  api.renderPhTuneCard(); api.renderPhTuneRemoveRow(cfg);
  const first = snapshotWrites(w);
  api.renderPhTuneCard(); api.renderPhTuneRemoveRow(cfg);
  api.renderPhTuneCard(); api.renderPhTuneRemoveRow(cfg);
  check(`bug-201 (a11y) ${label} : trois rendus identiques = AUCUNE écriture après le premier`,
        snapshotWrites(w), first);
}
{
  // Et un texte qui change est bien réécrit (le garde-fou ne fige pas l'écran).
  const { w, api } = boot();
  w.window._config = { ph_offset: 0 };
  api.setSensors({ ph: 7.3, phValueRaw: 7.3, filtration_running: false, filtration_elapsed_s: 0, phCalPoints: 2 });
  api.renderPhTuneCard();
  const before = w.els.ph_tune_refus.textContent;
  api.setSensors({ ph: 7.3, phValueRaw: 7.3, filtration_running: true, filtration_elapsed_s: 3600, phCalPoints: 1 });
  api.renderPhTuneCard();
  check("bug-201 (a11y) un refus qui CHANGE est réécrit", [before !== w.els.ph_tune_refus.textContent, w.els.ph_tune_refus.writes.text], [true, 2]);
}

// ===========================================================================
// bug-201 (C51) — relevé en revue (R1) : le passage avertissement -> refus remet
// AUSSI le chapeau et le repliable à l'état neutre.
// ===========================================================================
{
  const { w, api } = boot();
  w.window._config = { ph_offset: 1.2, ph_correction_type: "ph_minus", ph_impasse_low: false, ph_impasse_high: false };
  api.setSensors(SENS_OK);
  api.renderPhTuneCard();
  const e = w.els;
  check("bug-201 (C51) R1 pré-condition : avertissement affiché (chapeau « Avertissement », repliable visible)",
        [e.ph_tune_blocked_eyebrow.textContent, e.ph_tune_blocked_eyebrow.hidden, e.ph_tune_blocked_more.hidden],
        ["Avertissement", false, false]);
  e.ph_tune_blocked_more.open = true;       // l'utilisateur a déplié le détail
  api.setSensors({ ...SENS_OK, ph: null, phValueRaw: null });
  api.renderPhTuneCard();
  check("bug-201 (C51) R1 avertissement puis refus : chapeau VIDE et masqué, repliable masqué, détail vide",
        [e.ph_tune_blocked_eyebrow.textContent, e.ph_tune_blocked_eyebrow.hidden,
         e.ph_tune_blocked_more.hidden, e.ph_tune_blocked_detail.textContent,
         e.ph_tune_blocked_lead.textContent, e.ph_tune_blocked.hidden],
        ["", true, true, "", "", true]);
}

// ===========================================================================
// bug-201 (C51) — relevé en revue (R4) : le refus est ANNONCÉ à l'apparition.
// `hidden` levé AVANT l'écriture (masqué->visible), écriture vide AVANT le masquage.
// ===========================================================================
{
  const { w, api } = boot();
  w.window._config = { ph_offset: 0 };
  api.setSensors({ ph: null, phValueRaw: null, filtration_running: true, filtration_elapsed_s: 3600, phCalPoints: 2 });
  api.renderPhTuneCard();
  const r = w.els.ph_tune_refus;
  check("bug-201 (C51) R4 apparition : le refus est ANNONCÉ (écrit pendant qu'il est rendu)",
        r.announced, [REFUS_MESURE]);
  check("bug-201 (C51) R4 apparition : ordre = lever `hidden` PUIS écrire",
        r.ops, ["hidden:false", "text:" + REFUS_MESURE]);
  // Disparition : on écrit (vide) AVANT de masquer.
  api.setSensors({ ph: 7.3, phValueRaw: 7.3, filtration_running: true, filtration_elapsed_s: 3600, phCalPoints: 2 });
  api.renderPhTuneCard();
  check("bug-201 (C51) R4 disparition : écriture vide PUIS masquage, rien d'annoncé de plus",
        [r.ops.slice(2), r.announced.length], [["text:", "hidden:true"], 1]);
}
{
  // Refus -> refus d'un AUTRE texte : visible tout du long, réécrit, annoncé.
  const { w, api } = boot();
  w.window._config = { ph_offset: 0 };
  api.setSensors({ ph: 7.3, phValueRaw: 7.3, filtration_running: false, filtration_elapsed_s: 0, phCalPoints: 2 });
  api.renderPhTuneCard();
  api.setSensors({ ph: 7.3, phValueRaw: 7.3, filtration_running: true, filtration_elapsed_s: 3600, phCalPoints: 1 });
  api.renderPhTuneCard();
  check("bug-201 (C51) R4 changement de refus : les DEUX textes sont annoncés",
        [w.els.ph_tune_refus.announced.length, w.els.ph_tune_refus.writes.hidden], [2, 1]);
}

// ===========================================================================
// bug-201 (R2) — « Ne rien changer » est inerte pendant l'envoi (honnêteté d'écran).
// ===========================================================================
{
  const { w, api } = boot();
  w.window._config = { ph_offset: 1.2 };
  api.setSensors(SENS_OK);
  api.bindPhTuneRemoveRow();
  let release;
  w.calls.sendImpl = () => new Promise((res) => { release = () => res({ ok: true }); });
  await click(w, w.els.ph_tune_remove_btn);                       // arme
  const inflight = click(w, w.els.ph_tune_remove_confirm_btn);    // envoi EN COURS
  await Promise.resolve();
  check("bug-201 (R2) pré-condition : l'envoi est parti", w.calls.send, [{ ph_offset: 0 }]);
  await click(w, w.els.ph_tune_remove_cancel_btn);
  check("bug-201 (R2) « Ne rien changer » pendant l'envoi : le bloc reste affiché, toujours armé",
        [api.armed(), w.els.ph_tune_remove_confirm.hidden, w.els.ph_tune_remove_btn.hidden], [true, false, true]);
  release(); await inflight;
  check("bug-201 (R2) une fois l'envoi fini : désarmé, un seul envoi", [api.armed(), w.calls.send.length], [false, 1]);
}

// ===========================================================================
// bug-201 (R3) — la garde fail-open n'est pas annulée par son site d'appel : un
// ph_offset NON NUMÉRIQUE ne tombe JAMAIS sur la suppression directe.
// ===========================================================================
for (const [label, cfg] of [["NaN", { ph_offset: NaN }], ["null", { ph_offset: null }],
                            ["absent", {}], ["chaîne", { ph_offset: "1.2" }],
                            ["Infinity", { ph_offset: Infinity }]]) {
  const { w, api } = boot();
  w.window._config = cfg;
  api.setSensors(SENS_OK);
  api.bindPhTuneRemoveRow();
  await click(w, w.els.ph_tune_remove_btn);
  check(`bug-201 (R3) ph_offset ${label} : AUCUN envoi, clic sans effet (défaut sûr)`,
        [w.calls.send.length, api.armed(), w.els.ph_tune_remove_confirm.hidden], [0, false, true]);
}

// ===========================================================================
// bug-201 (R5) — un envoi qui LÈVE (réseau coupé) est nommé, jamais un rejet muet.
// ===========================================================================
{
  const { w, api } = boot();
  w.window._config = { ph_offset: 0.3 };
  api.setSensors(SENS_OK);
  api.bindPhTuneRemoveRow();
  w.calls.sendImpl = async () => { throw new Error("réseau"); };
  let rejected = null;
  try { await click(w, w.els.ph_tune_remove_btn); } catch (e) { rejected = e; }
  check("bug-201 (R5) suppression directe, envoi qui lève : pas de rejet, message nommé",
        [rejected, w.calls.toast], [null, [["Échec de la suppression — réseau indisponible", "error"]]]);
  // Et le bouton reste utilisable (busy relâché).
  w.calls.sendImpl = null;
  await click(w, w.els.ph_tune_remove_btn);
  check("bug-201 (R5) après l'échec, un nouvel appui envoie bien", w.calls.send.length, 2);
}
{
  const { w, api } = boot();
  w.window._config = { ph_offset: 1.2 };
  api.setSensors(SENS_OK);
  api.bindPhTuneRemoveRow();
  await click(w, w.els.ph_tune_remove_btn);
  w.calls.sendImpl = async () => { throw new Error("réseau"); };
  let rejected = null;
  try { await click(w, w.els.ph_tune_remove_confirm_btn); } catch (e) { rejected = e; }
  check("bug-201 (R5) confirmation, envoi qui lève : pas de rejet, message nommé, bloc désarmé",
        [rejected, w.calls.toast, api.armed(), w.els.ph_tune_remove_confirm_btn.disabled],
        [null, [["Échec de la suppression — réseau indisponible", "error"]], false, false]);
}

// ===========================================================================
// Textes : mode réglage de la page pH (pas de mutation exigée).
// ===========================================================================
check("bug-201 : MAINTENANCE_PAGES.ph.subMaintenance = « Étalonnage de la sonde »",
      /ph:\s*\{[\s\S]*?subMaintenance:\s*"Étalonnage de la sonde"/.test(src), true);
check("bug-201 : plus aucun `C8` nu dans data/index.html (qualifié feature-168)",
      /(^|[^\w(-])C8\b/.test(htmlLive.replace(/feature-168 \(C8/g, "")), false);

if (failures > 0) {
  console.error(`\n${failures} vecteur(s) en échec.`);
  process.exit(1);
}
console.log("\nTous les vecteurs de la carte permanente (bug-201) sont conformes.");

})().catch((e) => { console.error("ERREUR harnais :", e); process.exit(2); });
