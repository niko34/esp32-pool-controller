// Vecteurs du rendu du journal (bug-179) — formatLogLine() et le rendu DOM.
//
// Le journal contient des textes qui peuvent venir de l'extérieur. Le rendu est en
// TEXTE (textContent) : la fonction pure formatLogLine() renvoie une chaîne, jamais
// de balisage, et DOIT laisser `<b>`, `->`, `<=` TELS QUELS (c'est textContent, pas
// un échappement manuel, qui protège). Un sur-échappement (&lt;) dégraderait le
// diagnostic : les logs contiennent « pH 7.2 -> 7.1 », « ph <= 6.8 ».
//
// ADR-0099 : EXTRACTION du code vivant de data/app.js, jamais une recopie.
//   - formatLogTimestamp / formatLogLine / LOG_LEVEL_CLASS / buildLogLine /
//     _logStateMessage / renderLogs sont extraits puis évalués sur un DOM factice
//     qui COMPTE les createElement.
//
// MUTATIONS (bug-179 D2) : M2 (le niveau n'est plus émis) et M3 (textContent
// remplacé par un échappement manuel en entités) doivent tuer ce harnais.
//
// Exécution : node test/log_line_vectors.js
"use strict";
process.env.TZ = "UTC";
const assert = require("assert");
const fs = require("fs");
const path = require("path");
const { extractBalancedFromAnchor } = require("./js_extract");

const src = fs.readFileSync(path.join(__dirname, "..", "data", "app.js"), "utf8");
const grab = (anchor, open, close) => extractBalancedFromAnchor(src, anchor, open, close).text;

const code = {
  ts: grab("function formatLogTimestamp(ms) {", "{", "}"),
  cls: grab("const LOG_LEVEL_CLASS = {", "{", "}"),
  fmt: grab("function formatLogLine(entry) {", "{", "}"),
  build: grab("function buildLogLine(entry) {", "{", "}"),
  state: grab("function _logStateMessage(content, text) {", "{", "}"),
  render: grab("function renderLogs(scroll = true) {", "{", "}"),
  vis: grab("function isLogEntryVisible(entry) {", "{", "}"),
};

// ---------------------------------------------------------------- DOM factice
class FakeNode {
  constructor(tag, doc) { this.tag = tag; this.doc = doc; this.children = []; this._text = ""; this.className = ""; this.checked = true; this.scrollTop = 0; this.scrollHeight = 0;
    const self = this; this.classList = { add(c) { self.className = (self.className + " " + c).trim(); } }; }
  appendChild(n) {
    if (n.tag === "#frag") { this.children.push(...n.children); n.children = []; } else this.children.push(n);
    return n;
  }
  replaceChildren(...ns) { this.children = []; for (const n of ns) this.appendChild(n); }
  // bug-179 B3 : renderLogs retire par le HAUT les lignes chassées de l'anneau
  // quand le tampon est plein. Sans ces deux membres, le DOM factice laissait
  // la boucle de retrait tourner à vide et le vecteur « après rotation »
  // comptait 501 lignes au lieu de 500 — un faux rouge du HARNAIS, pas du code.
  get firstChild() { return this.children.length ? this.children[0] : null; }
  removeChild(n) { const i = this.children.indexOf(n); if (i >= 0) this.children.splice(i, 1); return n; }
  set textContent(v) { this.children = []; this._text = String(v); }
  get textContent() { return this._text + this.children.map(c => c.textContent).join(""); }
}
function makeDoc() {
  const doc = { created: 0 };
  doc.createElement = (t) => { doc.created++; return new FakeNode(t, doc); };
  doc.createTextNode = (t) => { const n = new FakeNode("#text", doc); n._text = String(t); return n; };
  doc.createDocumentFragment = () => new FakeNode("#frag", doc);
  return doc;
}
const countLines = (content) => content.children.filter(c => c.className.includes("log-line")).length;

function makeLive(entries, bootMs = Date.UTC(2026, 0, 1, 0, 0, 0)) {
  const doc = makeDoc();
  const content = doc.createElement("div"); doc.created = 0;
  const container = doc.createElement("div");
  const checks = {};
  const $ = (sel) => (sel === "#logs_content" ? content : sel === "#logs_container" ? container : checks[sel] || { checked: true });
  const factory = new Function("document", "$", "allLogEntries", "requestAnimationFrame", "_bootEpochMs", `
    "use strict";
    ${code.ts}
    ${code.cls}
    ${code.fmt}
    ${code.vis}
    ${code.build}
    let _logRender = null;
    ${code.state}
    ${code.render}
    return { formatLogTimestamp, formatLogLine, buildLogLine, renderLogs };
  `);
  const live = factory(doc, $, entries, (f) => f(), bootMs);
  return { live, doc, content, container, checks, entries };
}

let n = 0;
const eq = (a, b, msg) => { assert.strictEqual(a, b, msg); n++; };
const ok = (c, msg) => { assert.ok(c, msg); n++; };

const { live } = makeLive([]);
const T = 3723000; // 01:02:03 après un démarrage à minuit UTC

// ---- formatLogLine : formes d'entrée
eq(live.formatLogLine("texte brut"), "texte brut", "entrée chaîne");
eq(live.formatLogLine({ timestamp: T, level: "INFO", message: "hello" }), "[01:02:03] INFO: hello", "entrée objet");
eq(live.formatLogLine({ timestamp: T, message: "sans niveau" }), "[01:02:03] sans niveau", "level absent");
eq(live.formatLogLine({ timestamp: T, level: "WARN" }), "[01:02:03] WARN: ", "message absent");
eq(live.formatLogLine({ level: "ERROR", message: "sans heure" }), "ERROR: sans heure", "timestamp absent");
eq(live.formatLogLine({}), "", "objet vide");
eq(live.formatLogLine({ timestamp: 0, level: "INFO", message: "zéro" }), "[00:00:00] INFO: zéro", "timestamp 0 est une heure valide");
// M2 : le niveau doit être émis
ok(live.formatLogLine({ level: "CRITICAL", message: "x" }).includes("CRITICAL"), "le niveau est émis");

// ---- Balisage : ressort TEL QUEL dans le texte (textContent protège)
eq(live.formatLogLine({ timestamp: T, level: "WARN", message: "GET /get-config/<b>BUG179</b>" }),
   "[01:02:03] WARN: GET /get-config/<b>BUG179</b>", "<b> inchangé dans la chaîne");
eq(live.formatLogLine("<img src=x onerror=alert(1)>"), "<img src=x onerror=alert(1)>", "chaîne avec balise inchangée");

// ---- B2 : -> et <= VERBATIM, aucune entité (tue un sur-échappement)
for (const m of ["pH 7.2 -> 7.1", "ph <= 6.8", "a < b && c > d", "ORP >= 650 mV", "\"quoted\" 'single'"]) {
  const out = live.formatLogLine({ timestamp: T, level: "INFO", message: m });
  ok(out.endsWith(m), `verbatim : ${m}`);
  ok(!/&(lt|gt|amp|quot|#\d+|#x[0-9a-f]+);/i.test(out), `aucune entité dans « ${out} »`);
}

// ---- Rendu DOM : le texte est porté par textContent, jamais interprété
{
  const entries = [{ timestamp: T, level: "WARN", message: "Accès <b>BUG179</b> pH <= 6.8 -> 7" }, "<i>brut</i>"];
  const r = makeLive(entries);
  r.live.renderLogs();
  const lines = r.content.children;
  eq(lines.length, 2, "deux lignes");
  eq(lines[0].textContent, "[01:02:03]" + " WARN: Accès <b>BUG179</b> pH <= 6.8 -> 7", "texte de ligne objet");
  eq(lines[1].textContent, "<i>brut</i>", "texte de ligne chaîne");
  ok(lines[0].className.includes("log-line--warn"), "classe de niveau WARN");
  ok(!JSON.stringify(lines.map(l => l.tag)).includes("\"b\""), "aucun nœud <b> créé : le balisage n'est pas interprété");
  // aucun nœud ne porte d'entité
  ok(!/&lt;|&gt;|&amp;/.test(r.content.textContent), "aucune entité dans le texte rendu");
}

// ---- Idempotence : le même tableau rendu deux fois ⇒ le même nombre de nœuds
{
  const entries = []; for (let i = 0; i < 40; i++) entries.push({ timestamp: i * 1000, level: i % 2 ? "INFO" : "WARN", message: "m" + i });
  const r = makeLive(entries);
  r.live.renderLogs(); const first = countLines(r.content);
  r.live.renderLogs(); const second = countLines(r.content);
  eq(first, 40, "40 lignes au premier rendu");
  eq(second, first, "même tableau rendu deux fois ⇒ même nombre de nœuds (pas de doublon)");
  const txt1 = r.content.textContent;
  r.live.renderLogs();
  eq(r.content.textContent, txt1, "texte identique après un 3e rendu");
}

// ---- Incrémental : ajout de queue, rotation du tampon, filtre
{
  const entries = []; for (let i = 0; i < 500; i++) entries.push({ timestamp: i, level: "INFO", message: "m" + i });
  const r = makeLive(entries);
  r.live.renderLogs();
  eq(countLines(r.content), 500, "500 lignes");
  // régime établi : aucun changement ⇒ 0 nœud créé
  r.doc.created = 0; r.live.renderLogs();
  eq(r.doc.created, 0, "cycle sans ligne neuve ⇒ 0 createElement");
  // 3 lignes neuves, sans rotation
  entries.length = 497; r.live.renderLogs(); // reconstruit (le dernier change)
  for (let i = 0; i < 3; i++) entries.push({ timestamp: 1000 + i, level: "INFO", message: "n" + i });
  r.doc.created = 0; r.live.renderLogs();
  eq(r.doc.created, 6, "3 lignes neuves ⇒ 6 createElement (div + span horodatage par ligne : proportionnel aux lignes NEUVES, pas à 500)");
  eq(countLines(r.content), 500, "toujours 500, ni doublon ni perte");
  eq(r.content.children[499].textContent, "[00:00:01] INFO: n2", "la dernière ligne est la bonne");
  // rotation (push + shift) : la première change ⇒ reconstruction, même total
  entries.push({ timestamp: 2000, level: "INFO", message: "tournant" }); entries.shift();
  r.live.renderLogs();
  eq(countLines(r.content), 500, "après rotation : 500 lignes");
  eq(r.content.children[0].textContent.includes("m1"), true, "la ligne la plus ancienne a bien tourné");
  // filtre : masquer INFO ⇒ état vide propre, puis retour
  r.checks["#log_level_info"] = { checked: false };
  r.live.renderLogs();
  eq(r.content.children.length, 1, "filtre tout masqué : un seul message d'état");
  ok(/masqu/.test(r.content.textContent), "message d'état explicite");
  r.checks["#log_level_info"] = { checked: true };
  r.live.renderLogs();
  eq(countLines(r.content), 500, "filtre levé : les 500 lignes reviennent");
}

// ---- Défilement : la position est conservée lors d'une reconstruction, sans auto-scroll
{
  const entries = []; for (let i = 0; i < 50; i++) entries.push({ timestamp: i, level: "INFO", message: "m" + i });
  const r = makeLive(entries);
  r.live.renderLogs(false);
  r.checks["#logs_auto_scroll"] = { checked: false };
  r.container.scrollTop = 123;
  r.checks["#log_level_warn"] = { checked: false }; // change la signature ⇒ reconstruction
  r.live.renderLogs(false);
  eq(r.container.scrollTop, 123, "scrollTop conservé après reconstruction (auto-scroll coché OFF)");
  r.checks["#logs_auto_scroll"] = { checked: true };
  r.container.scrollHeight = 9999;
  r.live.renderLogs(false);
  eq(r.container.scrollTop, 9999, "auto-scroll coché : descend en bas");
}

// ---- B3 : au plafond de 500 lignes (push + shift), une ligne neuve ne doit pas tout reconstruire.
// ⭐ C'est le régime NORMAL : l'anneau de l'appareil est plein en permanence.
// Défaut mesuré le 2026-11-21 (shift() changeait `first` ⇒ ≈1000 createElement par ligne
// neuve, toutes les 5 s), CORRIGÉ le même jour : renderLogs retrouve son ancre et ne touche
// qu'aux deux bouts. ⛔ Le vecteur est STRICT — c'est lui qui empêche la repousse.
{
  const entries = []; for (let i = 0; i < 500; i++) entries.push({ timestamp: i, level: "INFO", message: "m" + i });
  const r = makeLive(entries);
  r.live.renderLogs();
  entries.push({ timestamp: 9999, level: "INFO", message: "neuve" }); entries.shift();
  r.doc.created = 0; r.live.renderLogs();
  eq(countLines(r.content), 500, "au plafond : toujours 500 lignes (pas de doublon)");
  ok(r.doc.created <= 5, `au plafond : ${r.doc.created} createElement pour 1 ligne neuve (seuil 5)`);
}

// ---- ⭐ Les niveaux RÉELLEMENT émis par le firmware (src/log_arena_logic.cpp:121-130).
// DÉFAUT TROUVÉ À L'ESSAI DE TERRAIN du 2026-11-21 : le firmware écrit « CRIT », la page
// attendait « CRITICAL ». ⇒ la case CRITICAL ne filtrait RIEN (les lignes tombaient dans
// « niveaux inconnus, affichés par défaut ») et la classe de couleur ne s'appliquait pas.
// ⛔ Ce vecteur énumère les chaînes du firmware : si l'une change d'un côté sans l'autre,
//    il meurt. C'est lui qui aurait attrapé le défaut.
{
  const NIVEAUX = [
    ["DEBUG", "log-line--debug", "#log_level_debug"],
    ["INFO",  "",                "#log_level_info"],
    ["WARN",  "log-line--warn",  "#log_level_warn"],
    ["ERROR", "log-line--error", "#log_level_error"],
    ["CRIT",  "log-line--critical", "#log_level_critical"],
  ];
  for (const [lvl, cls, sel] of NIVEAUX) {
    const r = makeLive([{ timestamp: 1000, level: lvl, message: "m" }]);
    r.live.renderLogs();
    const line = r.content.children[0];
    if (cls) ok(line.className.includes(cls), `niveau ${lvl} : classe ${cls} appliquée`);
    else ok(line.className.trim() === "log-line", `niveau ${lvl} : aucune classe de couleur`);
    // ⭐ et la case du MÊME niveau doit réellement le masquer
    const r2 = makeLive([{ timestamp: 1000, level: lvl, message: "m" }]);
    r2.checks[sel] = { checked: false };
    r2.live.renderLogs();
    eq(countLines(r2.content), 0, `niveau ${lvl} : décocher ${sel} le masque`);
  }
}

console.log(`log_line_vectors : ${n} assertions OK`);
