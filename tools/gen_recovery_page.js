#!/usr/bin/env node
// =============================================================================
// gen_recovery_page.js — génère src/ota_recovery_page.cpp (feature-151 D3-b1)
// =============================================================================
// La page de secours OTA doit calculer le SHA-256 de l'image dans le navigateur
// (crypto.subtle est indisponible : HTTP clair sur IP de LAN). Ce code existe
// UNE SEULE FOIS, dans data/app.js ; ce script l'en EXTRAIT par ancre
// (test/js_extract.js, ADR-0099) et l'injecte dans le gabarit
// src/ota_recovery_page.html.in. Aucune copie manuelle, donc aucune dérive
// silencieuse — et tools/check_recovery_page_generated.sh re-génère et compare.
//
// VARIANTE « génération au COMMIT » (ADR-0124) : le .cpp produit est VERSIONNÉ.
// `pio run` ne dépend donc pas de node (gate CI de compilation), et la page
// reste lisible dans le diff.
//
// ⛔ ÉCHOUE FORT : une ancre absente ou présente plus d'une fois, un marqueur de
// gabarit qui n'apparaît pas exactement une fois, ou un motif interdit (S6).
//
// Usage : node tools/gen_recovery_page.js [--out <fichier>]
//         (défaut : src/ota_recovery_page.cpp)
// =============================================================================
"use strict";

const fs = require("fs");
const path = require("path");
const { extractBalancedFromAnchor } = require("../test/js_extract.js");

const root = path.join(__dirname, "..");
const appJs = fs.readFileSync(path.join(root, "data/app.js"), "utf8");
const template = fs.readFileSync(path.join(root, "src/ota_recovery_page.html.in"), "utf8");

const outIdx = process.argv.indexOf("--out");
const outPath = outIdx > -1 ? process.argv[outIdx + 1] : path.join(root, "src/ota_recovery_page.cpp");

function die(msg) {
  console.error("ERREUR gen_recovery_page : " + msg);
  process.exit(1);
}

// Retire l'indentation de 2 espaces qu'avaient ces blocs dans app.js.
// Retire aussi les lignes qui ne sont QUE des commentaires (économie de flash ;
// aucun littéral multi-lignes dans ces cinq blocs, vérifié par le test de syntaxe).
function dedent(text) {
  return text.split("\n").filter((l) => !/^\s*\/\/.*$/.test(l)).map((l, i) => (i === 0 ? l : l.replace(/^ {1,2}/, ""))).join("\n");
}

// --- Les cinq extraits, chacun par ancre unique ------------------------------
const kAnchor = "const SHA256_K = new Uint32Array([";
const k = extractBalancedFromAnchor(appJs, kAnchor, "[", "]");
if (appJs.slice(k.closeIdx + 1, k.closeIdx + 3) !== ");") {
  die("la forme de SHA256_K a changé (attendu ');' après le tableau)");
}
const pieces = {
  SHA256_K: k.text + ");",
  CREATE_SHA256: extractBalancedFromAnchor(appJs, "function createSha256() {", "{", "}").text,
  SHA256_OF_FILE: extractBalancedFromAnchor(appJs, "async function sha256OfFile(file, onProgress) {", "{", "}").text,
  PARSE_MANIFEST: extractBalancedFromAnchor(appJs, "function parseOtaManifestLine(line) {", "{", "}").text,
  PARSE_SIG: extractBalancedFromAnchor(appJs, "function parseSigFile(text) {", "{", "}").text,
};

// --- Injection : chaque marqueur exactement une fois -------------------------
let html = template;
for (const [name, text] of Object.entries(pieces)) {
  const marker = `/*@@${name}@@*/`;
  const first = html.indexOf(marker);
  if (first === -1) die(`marqueur ${marker} absent du gabarit`);
  if (html.indexOf(marker, first + 1) !== -1) die(`marqueur ${marker} présent plus d'une fois`);
  html = html.replace(marker, () => dedent(text));
}
if (/\/\*@@/.test(html)) die("marqueur de gabarit résiduel");

// --- S6 : motifs interdits dans la page FINALE (gabarit + code extrait) ------
const forbidden = [
  [/innerHTML/, "innerHTML"],
  [/insertAdjacentHTML/, "insertAdjacentHTML"],
  [/document\.write/, "document.write"],
  [/\beval\b/, "eval"],
  [/outerHTML/, "outerHTML"],
  [/location\.href/, "location.href"],
  [/new\s+Function/, "new Function"],
  [/location\.search/, "location.search"],
  [/location\.hash/, "location.hash"],
  [/\bhttps?:\/\//, "URL absolue http(s)"],
  [/\bwss?:\/\//, "URL absolue ws(s)"],
  [/["'(=]\/\/[A-Za-z0-9]/, "URL relative au protocole"],
  // Condition B11 (D3-b2) : l'interrupteur de signature n'existe JAMAIS dans la page de secours.
  [/ota-signature-switch/, "ota-signature-switch (B11)"],
];
for (const [re, label] of forbidden) {
  if (re.test(html)) die(`motif interdit (condition S6) dans la page : ${label}`);
}

// --- Émission du .cpp ---------------------------------------------------------
const delim = "RECOVERYPAGE";
if (html.includes(`)${delim}"`)) die("le délimiteur du littéral brut apparaît dans la page");

// --- Version (D3-b2 B1/B3/B4bis) : APRÈS les contrôles ci-dessus ---------------
// Le marqueur est remplacé par le JETON FIRMWARE_VERSION, résolu par le
// compilateur (concaténation de littéraux) : le .cpp ne contient jamais le
// numéro, donc le contrôle de génération reste vert à chaque bump de version.h.
// On valide pourtant le format lu dans src/version.h (garde-fou anti-injection).
const kVersionMarker = "<!--@@VERSION@@-->";
const vFirst = html.indexOf(kVersionMarker);
if (vFirst === -1) die(`marqueur ${kVersionMarker} absent du gabarit`);
if (html.indexOf(kVersionMarker, vFirst + 1) !== -1) die(`marqueur ${kVersionMarker} présent plus d'une fois`);
const vMatch = /#define\s+FIRMWARE_VERSION\s+"([^"]*)"/.exec(fs.readFileSync(path.join(root, "src/version.h"), "utf8"));
if (!vMatch) die("FIRMWARE_VERSION introuvable dans src/version.h");
if (!/^[0-9]+\.[0-9]+\.[0-9]+$/.test(vMatch[1])) die(`version « ${vMatch[1]} » invalide (attendu X.Y.Z)`);
const htmlBefore = html.slice(0, vFirst);
const htmlAfter = html.slice(vFirst + kVersionMarker.length);
const literal = `R"${delim}(${htmlBefore})${delim}" FIRMWARE_VERSION R"${delim}(${htmlAfter})${delim}"`;

const cpp =
`// =============================================================================
// FICHIER GÉNÉRÉ — NE PAS ÉDITER À LA MAIN (feature-151 D3-b1, ADR-0124)
// Source : src/ota_recovery_page.html.in + extraits de data/app.js
// Régénérer : node tools/gen_recovery_page.js
// Contrôle  : tools/check_recovery_page_generated.sh (rouge à la moindre dérive)
// =============================================================================
#include "ota_recovery_page.h"
#include "version.h"

const char kOtaRecoveryPageHtml[] = ${literal};
const size_t kOtaRecoveryPageHtmlLen = sizeof(kOtaRecoveryPageHtml) - 1;
`;
fs.writeFileSync(outPath, cpp);
console.log(`OK — ${outPath} (${html.length - kVersionMarker.length + vMatch[1].length} octets de page)`);
