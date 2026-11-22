// Vecteurs de « ce que l'appareil sait » selon le raccordement (feature-210).
//
// L'appareil autorise une injection sur une DÉDUCTION, jamais sur une mesure :
// il sait ce qu'il a COMMANDÉ (managed), ce que son câblage IMPOSE (powered) ou
// ce qu'on lui a DIT (external). ⛔ Aucun des trois ne lit un débit.
//
// ⭐ Mutant central : servir le texte « managed » quand le mode est INCONNU.
//    `applyInstallMode()` replie déjà sur 'managed' pour masquer l'interface —
//    repli légitime — et `loadConfig()` fait `cfg.install_mode || "managed"`.
//    Reconduire ce repli sur le TEXTE affirmerait un programme et un relais qui
//    n'existent peut-être pas : c'est le défaut le plus facile à réintroduire
//    en silence, et deux vecteurs le gardent.
//
// Ce harnais EXTRAIT le code livré de data/app.js par ANCRE (ADR-0099).
//
// Exécution : node test/install_scope_vectors.js

"use strict";
const assert = require("assert");
const fs = require("fs");
const path = require("path");
const { extractBalancedFromAnchor } = require("./js_extract");

const root = path.join(__dirname, "..");
const src = fs.readFileSync(path.join(root, "data", "app.js"), "utf8");
const grab = (anchor) => extractBalancedFromAnchor(src, anchor, "{", "}").text;

const live = new Function(`
  "use strict";
  ${grab("  function installScopeText(mode) {")}
  return { installScopeText };
`)();

let failures = 0;
let ran = 0;
function check(label, f) {
  ran++;
  try { f(); console.log(`  ok   ${label}`); }
  catch (e) { failures++; console.log(`  FAIL ${label}\n       ${e.message}`); }
}

const MODES = ["managed", "powered", "external"];

// ==================================================== UN TEXTE PAR MODE
console.log("I1 un texte par mode, et trois textes DIFFÉRENTS");

check("chacun des trois modes a son texte", () => {
  MODES.forEach((m) => {
    const t = live.installScopeText(m);
    assert.ok(typeof t === "string" && t.length > 0, `aucun texte pour « ${m} »`);
  });
});

check("⭐ les trois textes sont DISTINCTS deux à deux", () => {
  const textes = MODES.map(live.installScopeText);
  const uniques = new Set(textes);
  assert.strictEqual(uniques.size, 3,
    "un même texte sert pour plusieurs modes — il serait FAUX pour au moins l'un d'eux");
});

check("chaque texte nomme la SOURCE de ce que l'appareil croit", () => {
  // Règle du gate chimique : toute phrase qui AUTORISE doit dire d'où vient
  // l'information. « commande » / « alimenté » / « envoie » sont les trois sources.
  assert.ok(/commande/i.test(live.installScopeText("managed")),
    "le mode piloté n'annonce pas qu'il COMMANDE la pompe");
  assert.ok(/aliment/i.test(live.installScopeText("powered")),
    "le mode alimenté n'annonce pas d'où vient sa déduction");
  assert.ok(/envoie|signal/i.test(live.installScopeText("external")),
    "le mode externe n'annonce pas qu'il REÇOIT un état");
});

// ============================= LE MODE INCONNU N'AFFIRME RIEN
console.log("I2 ⛔ un mode inconnu n'affirme RIEN");

check("⭐ mode absent (undefined) ⇒ aucun texte", () => {
  assert.strictEqual(live.installScopeText(undefined), null,
    "le repli affirme un raccordement qu'on ne connaît pas");
});

check("⭐ mode null, vide, inconnu, futur ⇒ aucun texte", () => {
  [null, "", "managed2", "MANAGED", "auto", "continu", 0, false, {}].forEach((v) => {
    assert.strictEqual(live.installScopeText(v), null,
      `« ${String(v)} » a produit un texte`);
  });
});

check("⛔ le texte du mode piloté n'est JAMAIS servi par défaut", () => {
  const managed = live.installScopeText("managed");
  [undefined, null, "", "inconnu"].forEach((v) => {
    assert.notStrictEqual(live.installScopeText(v), managed,
      "mode inconnu ⇒ texte du mode le MOINS exposé : c'est précisément l'erreur à éviter");
  });
});

// ==================================== LE VOCABULAIRE EST INTERDIT
console.log("I3 ⛔ aucun verbe de perception sur l'eau");

// Le gate chimique fournit cette liste : l'appareil commande, présume ou reçoit.
// ⛔ Il ne vérifie pas, ne détecte pas, ne contrôle pas, ne garantit pas.
const VERBES_INTERDITS = /v[ée]rifi|d[ée]tect|contr[oô]l|garanti|s'assure|confirme|surveill|mesure l'eau|eau pr[ée]sente/i;

check("⛔ aucun des trois textes n'emploie un verbe de perception", () => {
  MODES.forEach((m) => {
    const t = live.installScopeText(m);
    assert.ok(!VERBES_INTERDITS.test(t),
      `« ${m} » laisse croire à une constatation : « ${t} »`);
  });
});

check("⛔ aucun texte ne présente le raccordement comme dangereux", () => {
  MODES.forEach((m) => {
    assert.ok(!/danger|risqu|attention|avertissement/i.test(live.installScopeText(m)),
      `« ${m} » alarme sur un mode d'installation normal`);
  });
});

check("⛔ aucun texte n'invite à couper la régulation", () => {
  // Un bassin privé d'acide 48 h part à pH 8,2-8,4, où la fraction de chlore
  // actif tombe sous 25 %. « Coupez par prudence » est la pire consigne possible.
  MODES.forEach((m) => {
    assert.ok(!/coupez|arr[êe]tez la r[ée]gulation|d[ée]sactivez/i.test(live.installScopeText(m)),
      `« ${m} » invite à couper la régulation`);
  });
});

// =============================== LA PROMESSE FAUSSE EST PARTIE
console.log("I4 ⛔ la promesse fausse ne revit nulle part");

// ⚠️ Elle vivait en TROIS exemplaires : l'écran de réglage, l'assistant de
// première mise en service, et le guide utilisateur. Couvrir un seul fichier
// l'aurait laissée se réintroduire par les autres — c'est exactement ainsi
// qu'elle a survécu en double.
const FICHIERS = [
  "data/index.html",
  "data/wizard.html",
  "data/app.js",
  "docs/user/src/guide-utilisation.html",
  // \u26d4 Ajout\u00e9 en revue : c'est la doc de r\u00e9f\u00e9rence de l'\u00e9tape 3 de l'assistant,
  // donc l'endroit exact o\u00f9 un d\u00e9veloppeur ira recopier la formule.
  "docs/features/page-wizard.md",
  "docs/features/page-settings.md",
  // \u26d4 Ajout\u00e9 en revue : il portait la m\u00eame promesse sous une autre forme
  // (\u00ab Jamais de dose sans eau en circulation \u00bb) \u2014 d\u00e9j\u00e0 cinq exemplaires trouv\u00e9s,
  // chacun dans un fichier que la garde pr\u00e9c\u00e9dente ne couvrait pas.
  "docs/user/src/guide-regulation.html",
];

// \u26d4 Revue 2026-11-22 \u2014 l'analyse LIGNE \u00c0 LIGNE \u00e9tait contournable de trois fa\u00e7ons,
// d\u00e9montr\u00e9es par ex\u00e9cution :
//   1. la prose de ces fichiers est coup\u00e9e \u00e0 ~80 colonnes, donc la phrase fautive
//      s'\u00e9tale sur DEUX lignes dont aucune ne correspond au motif ;
//   2. l'exemption \u00ab c'est un commentaire \u00bb acceptait toute ligne contenant \u00ab \u00bb \u00bb,
//      or le guillemet fran\u00e7ais est le guillemet NORMAL de l'interface ;
//   3. une incise (\u00ab Le mode \u00ab pilot\u00e9 \u00bb garantit qu'aucun\u2026 \u00bb) passait pour la m\u00eame raison.
// \u21d2 On retire les commentaires AVANT d'analyser, on normalise les espaces, et on
//    cherche sur le DOCUMENT ENTIER. L'exemption \u00ab \u00bb \u00bb dispara\u00eet d'elle-m\u00eame.
function prose(texte) {
  return texte
    .replace(/<!--[\s\S]*?-->/g, " ")   // commentaires HTML
    .replace(/\/\*[\s\S]*?\*\//g, " ")   // commentaires JS de bloc
    .replace(/^\s*\/\/.*$/gm, " ")       // commentaires JS de ligne
    .replace(/\s+/g, " ");
}

// \u26a0\ufe0f Une documentation a le DROIT de citer ce qu'elle a retir\u00e9 \u2014 c'est m\u00eame ce qui
// emp\u00eache un futur lecteur de le r\u00e9introduire par ignorance. Ce qui est interdit,
// c'est de l'AFFIRMER. On n'accepte donc la formule que si la m\u00eame phrase porte une
// marque de r\u00e9tractation. Une recopie nue reste bloqu\u00e9e.
const RETRACTATION = /retir|faux|n'est pas vrai|ne garantit (rien|pas)|feature-210|\u26d4/i;

check("\u2b50 \u00ab garantit qu'aucun produit\u2026 \u00bb n'est AFFIRM\u00c9 dans aucun fichier", () => {
  FICHIERS.forEach((f) => {
    const texte = prose(fs.readFileSync(path.join(root, f), "utf8"));
    // D\u00e9coupage en phrases : c'est l'unit\u00e9 o\u00f9 une r\u00e9tractation fait sens.
    texte.split(/(?<=[.!?])\s+/).forEach((phrase) => {
      if (!/garanti\w*\s+qu'aucun/i.test(phrase)) return;
      assert.ok(RETRACTATION.test(phrase),
        `${f} affirme encore une garantie : \u00ab ${phrase.trim().slice(0, 140)} \u00bb`);
    });
  });
});

check("\u26d4 aucun fichier ne dit que l'appareil constate la circulation", () => {
  // \u26a0\ufe0f Le motif couvre aussi les tournures qui disent la m\u00eame chose sans le verbe :
  // \u00ab sait que l'eau circule \u00bb, \u00ab jamais d'injection sans circulation \u00bb.
  const MOTIF = /(v[\u00e9e]rifi\w*|d[\u00e9e]tect\w*|contr[o\u00f4]l\w*|sait que l'eau circule|jamais d'injection sans circulation)[^.]{0,60}(circulation|d[\u00e9e]bit)?/i;
  FICHIERS.forEach((f) => {
    const texte = prose(fs.readFileSync(path.join(root, f), "utf8"));
    [/(v[\u00e9e]rifi\w*|d[\u00e9e]tect\w*|contr[o\u00f4]l\w*)[^.]{0,40}(circulation|d[\u00e9e]bit)/i,
     /sait que l'eau circule/i,
     /jamais d'injection sans circulation/i,
     /jamais de dose sans eau en circulation/i].forEach((re) => {
      const m = texte.match(re);
      assert.ok(!m, `${f} pr\u00eate \u00e0 l'appareil une constatation : \u00ab ${m ? m[0] : ""} \u00bb`);
    });
  });
});

console.log(`\n${ran - failures}/${ran} vecteurs conformes`);
process.exit(failures ? 1 : 0);
