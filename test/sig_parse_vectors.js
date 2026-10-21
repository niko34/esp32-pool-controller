// Vecteurs de test du parseur PUR du fichier de signature OTA ".sig"
// (feature-151 D1-bis), de la décision PURE de blocage de l'envoi manuel
// (`otaManualSendBlocked`) et de la décision PURE de variant de hint par
// défaut (`sigHintDefaultCase`, revue de code — points A-D), extraits de
// `data/app.js`.
//
// Pourquoi ce fichier existe : la carte "Mise à jour manuelle" accepte
// désormais un fichier `.sig` optionnel (2 lignes : manifeste signé en
// clair, puis signature base64) à côté du `.bin`. `parseSigFile(text)` est la
// décision PURE qui valide ce fichier AVANT tout envoi — elle ne touche
// jamais le DOM et n'exécute AUCUNE cryptographie (la page TRANSPORTE,
// l'appareil VÉRIFIE). Sa grammaire est un MIROIR volontaire de
// `src/ota_manifest_logic.{h,cpp}` : les deux évoluent ensemble.
//
// 🔴 Correctif de revue (même lot) : le brief initial affirmait qu'une seule
// condition bloquait l'envoi (la comparaison d'empreinte B4). C'était FAUX —
// la divergence nom↔genre (B3, `typeConflict`) bloquait ELLE AUSSI, y
// compris quand l'empreinte venait de PROUVER (`bindingState === "match"`)
// que le manifeste décrit bien ce fichier. Faire gagner une heuristique de
// nom contre une preuve cryptographique est un refus qui ne protège de
// rien. `otaManualSendBlocked({hasFile, sigReadState, typeConflict,
// bindingState})` est la décision PURE qui encode désormais cette priorité
// (preuve > heuristique) — voir `specs/features/doing/feature-151-signature-
// de-l-image-ota.md`, section D1-bis, paragraphe "Corrigé en cours de lot".
//
// CE QUE CE FICHIER VISE (patron `test/resolve_selector_mode_vectors.js`,
// feature-161 ; `test/ph_health_vectors.js`, feature-154) : il EXTRAIT le
// texte réel de `parseSigFile` (et de son auxiliaire `parseOtaManifestLine`)
// depuis `data/app.js` (ancre + comptage de profondeur `{}` via
// `test/js_extract.js`, ADR-0099) et l'ÉVALUE — jamais une recopie. Toute
// divergence future entre l'implémentation livrée et ce qui est testé est
// donc structurellement impossible.
//
// Exécution : `node test/sig_parse_vectors.js` (zéro dépendance npm, sort
// avec un code non nul si un vecteur échoue — utilisable en CI et en
// pre-commit local via `./tools/check_sig_parse_vectors.sh`).
//
// Couverture demandée (brief D1-bis) :
//   - nominal firmware, nominal filesystem (⇒ ok:true, champs corrects) ;
//   - \r\n (fichier produit sous Windows) ⇒ toléré, résultat identique ;
//   - LF final unique ⇒ toléré ;
//   - 3 lignes (contenu après la ligne 2) ⇒ "lignes" ;
//   - ligne 1 hors grammaire : préfixe altéré, genre inconnu/mal casé,
//     version non SemVer (zéro de tête, lettre, 4 composants), hex en
//     majuscules, hex de longueur ≠ 64 ⇒ "manifeste" ;
//   - ligne 2 : longueur ≠ 512, caractère hors alphabet base64 ⇒ "signature" ;
//   - fichier vide ⇒ "vide" ;
//   - fichier avec seulement une ligne, ou une ligne vide ⇒ "lignes".
//
// Mutants couverts (section D du brief, résultats détaillés dans
// tools/check_sig_parse_vectors.sh) :
//   - accepter l'hex en majuscules                 ⇒ tué par le vecteur hex majuscules ;
//   - accepter 3 lignes (ne pas refuser le surplus) ⇒ tué par le vecteur 3 lignes ;
//   - ne pas retirer le \n du manifeste             ⇒ tué par le vecteur "manifest exact" ;
//   - accepter un zéro de tête dans la version      ⇒ tué par le vecteur zéro de tête.

"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const { extractBalancedFromAnchor } = require("./js_extract");

// ---------------------------------------------------------------------
// Extraction du code RÉELLEMENT livré depuis data/app.js (voir commentaire
// d'en-tête). Deux ancres : l'auxiliaire `parseOtaManifestLine` est appelé
// PAR `parseSigFile` — les deux doivent être extraites et évaluées ensemble.
// ---------------------------------------------------------------------

const APP_JS_PATH = path.join(__dirname, "..", "data", "app.js");
const appJsSource = fs.readFileSync(APP_JS_PATH, "utf8");

const parseOtaManifestLineSource = extractBalancedFromAnchor(
  appJsSource,
  "function parseOtaManifestLine(line) {",
  "{",
  "}"
).text;

const parseSigFileSource = extractBalancedFromAnchor(
  appJsSource,
  "function parseSigFile(text) {",
  "{",
  "}"
).text;

const otaManualSendBlockedSource = extractBalancedFromAnchor(
  appJsSource,
  "function otaManualSendBlocked({ hasFile, sigReadState, typeConflict, bindingState }) {",
  "{",
  "}"
).text;

const sigHintDefaultCaseSource = extractBalancedFromAnchor(
  appJsSource,
  "function sigHintDefaultCase({ sigReadState, hasFile, typeFromName }) {",
  "{",
  "}"
).text;

// --- Évaluation du code EXTRAIT (jamais une recopie) ------------------------
const buildLive = new Function(`
  "use strict";
  ${parseOtaManifestLineSource}
  ${parseSigFileSource}
  ${otaManualSendBlockedSource}
  ${sigHintDefaultCaseSource}
  return { parseSigFile, parseOtaManifestLine, otaManualSendBlocked, sigHintDefaultCase };
`);
const { parseSigFile, otaManualSendBlocked, sigHintDefaultCase } = buildLive();

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

function checkFail(label, text, expectedCode) {
  const result = parseSigFile(text);
  try {
    assert.strictEqual(result.ok, false, label + " (ok devrait être false)");
    assert.strictEqual(result.code, expectedCode, label + ` (code attendu "${expectedCode}", obtenu "${result.code}")`);
    console.log(`OK  ${label}`);
  } catch (e) {
    failures++;
    console.error("ÉCHEC", label, "—", e.message);
  }
}

// =============================================================================
// Fixtures valides (hex et base64 réellement générés, pas des chaînes au
// hasard tapées à la main — une empreinte à 63 caractères par faute de frappe
// invaliderait silencieusement un vecteur "nominal").
// =============================================================================

const HEX_A = "a19d0444d96180703c7a52c0f2dfdf6a00d3020ff61ab95eac4d7664cc63d4a1";
assert.strictEqual(HEX_A.length, 64, "fixture HEX_A doit faire 64 caractères");

const HEX_B = "19ba22b001d0e53496d2ca02700955b54bb2839e9765746be37e1fa5713cf71e";
assert.strictEqual(HEX_B.length, 64, "fixture HEX_B doit faire 64 caractères");

const B64_A =
  "HA+ekXlBh0Hr6gHmFjv9jOKxOvJ3GOYYlmIcAFPKAoLDZsPJPrLJPFELVNrj03Kly6mHCeQ+/CXXW8sktCqZfp602JyT/u+IOBoZ6i06+2dU5xeMLxbH5gn9aXV1polUwj98gNYg0NTNCbaDRHXMYXDAVjRdMdFDDXKXe/XhZE1pmUpPWCn6vFoP5Dk2X6xtKSdz9SXubkVkBsLbDG8YVzVAvLLugTTBlTleM04dFeLzn1lsRYTeQyhxYg8eGTQVhXjUFBJLJ/ddcS5rljJ1rtYOZ8ZM9tgeC8LXuXlgcRIKlIV51THn/mOzJLnwBik2sSNWzgBdx7PHZEst7JyexY/iLUWeByHg4qYI2QpHudh5C0eBZl5poFRoXONU9Lk1UFu6I/sLsD+pDbCQ584aMQyfmATlFeyFjiETN0eAtKltSvH4CJKYq6GkwsHLKEgkHZZ31bLaapaAqHuQ2FMYESEVzXZ+JAU50KQYl+loCFbV8uj38i5hvGCcKBAvC4O5";
assert.strictEqual(B64_A.length, 512, "fixture B64_A doit faire 512 caractères");

function sigText(genre, version, hex, b64) {
  return `pool-ota/1 ${genre} ${version} sha256:${hex}\n${b64}\n`;
}

// =============================================================================
// Nominal — firmware et filesystem, les deux genres acceptés.
// =============================================================================
{
  const text = sigText("firmware", "2.86.0", HEX_A, B64_A);
  const r = parseSigFile(text);
  check("nominal firmware : ok", r.ok, true);
  if (r.ok) {
    check("nominal firmware : genre", r.genre, "firmware");
    check("nominal firmware : version", r.version, "2.86.0");
    check("nominal firmware : digest", r.digest, HEX_A);
    check("nominal firmware : signature", r.signature, B64_A);
    check("nominal firmware : manifest EXACT (sans \\n)", r.manifest,
      `pool-ota/1 firmware 2.86.0 sha256:${HEX_A}`);
  }
}
{
  const text = sigText("filesystem", "2.86.0", HEX_B, B64_A);
  const r = parseSigFile(text);
  check("nominal filesystem : ok", r.ok, true);
  if (r.ok) {
    check("nominal filesystem : genre", r.genre, "filesystem");
    check("nominal filesystem : digest", r.digest, HEX_B);
  }
}

// =============================================================================
// Tolérances explicites : \r\n (Windows) et LF final.
// =============================================================================
{
  const crlfText = `pool-ota/1 firmware 2.86.0 sha256:${HEX_A}\r\n${B64_A}\r\n`;
  const r = parseSigFile(crlfText);
  check("CRLF toléré : ok", r.ok, true);
  if (r.ok) {
    check("CRLF toléré : manifest sans \\r ni \\n", r.manifest,
      `pool-ota/1 firmware 2.86.0 sha256:${HEX_A}`);
    check("CRLF toléré : signature sans \\r", r.signature, B64_A);
  }
}
{
  // Sans LF final du tout (2 lignes, pas de saut de ligne terminal).
  const noFinalLf = `pool-ota/1 firmware 2.86.0 sha256:${HEX_A}\n${B64_A}`;
  const r = parseSigFile(noFinalLf);
  check("sans LF final : ok", r.ok, true);
}

// =============================================================================
// "lignes" — ≠ 2 lignes utiles, ou contenu au-delà de la ligne 2.
// =============================================================================
checkFail("3 lignes (contenu après la signature) : lignes",
  `pool-ota/1 firmware 2.86.0 sha256:${HEX_A}\n${B64_A}\nligne de trop\n`, "lignes");
checkFail("LF final EN PLUS (ligne vide supplémentaire) : lignes",
  `pool-ota/1 firmware 2.86.0 sha256:${HEX_A}\n${B64_A}\n\n`, "lignes");
checkFail("une seule ligne : lignes",
  `pool-ota/1 firmware 2.86.0 sha256:${HEX_A}`, "lignes");
checkFail("ligne 2 vide : lignes",
  `pool-ota/1 firmware 2.86.0 sha256:${HEX_A}\n`, "lignes");

// =============================================================================
// "vide" — fichier vide.
// =============================================================================
checkFail("fichier vide : vide", "", "vide");

// =============================================================================
// "manifeste" — ligne 1 hors grammaire.
// =============================================================================
checkFail("préfixe magique altéré : manifeste",
  sigText("firmware", "2.86.0", HEX_A, B64_A).replace("pool-ota/1", "pool-ota/2"), "manifeste");
checkFail("genre inconnu : manifeste",
  sigText("Firmware", "2.86.0", HEX_A, B64_A), "manifeste");  // casse sensible
checkFail("genre totalement inconnu : manifeste",
  `pool-ota/1 bootloader 2.86.0 sha256:${HEX_A}\n${B64_A}\n`, "manifeste");
checkFail("version avec zéro de tête : manifeste",
  `pool-ota/1 firmware 02.86.0 sha256:${HEX_A}\n${B64_A}\n`, "manifeste");
checkFail("version non SemVer (lettre) : manifeste",
  `pool-ota/1 firmware 2.x.0 sha256:${HEX_A}\n${B64_A}\n`, "manifeste");
checkFail("version à 4 composants : manifeste",
  `pool-ota/1 firmware 2.86.0.1 sha256:${HEX_A}\n${B64_A}\n`, "manifeste");
checkFail("hex en MAJUSCULES : manifeste",
  `pool-ota/1 firmware 2.86.0 sha256:${HEX_A.toUpperCase()}\n${B64_A}\n`, "manifeste");
checkFail("hex de longueur 63 : manifeste",
  `pool-ota/1 firmware 2.86.0 sha256:${HEX_A.slice(0, 63)}\n${B64_A}\n`, "manifeste");
checkFail("espace double entre les jetons : manifeste",
  `pool-ota/1 firmware  2.86.0 sha256:${HEX_A}\n${B64_A}\n`, "manifeste");

// =============================================================================
// "signature" — ligne 2 hors base64 strict.
// =============================================================================
checkFail("signature trop courte (511 caractères) : signature",
  sigText("firmware", "2.86.0", HEX_A, B64_A.slice(0, 511)), "signature");
checkFail("signature trop longue (513 caractères) : signature",
  sigText("firmware", "2.86.0", HEX_A, B64_A + "A"), "signature");
checkFail("signature avec caractère hors alphabet (= au milieu) : signature",
  sigText("firmware", "2.86.0", HEX_A, "=" + B64_A.slice(1)), "signature");

// =============================================================================
// otaManualSendBlocked — décision de blocage de l'envoi (correctif de revue).
// ⭐ Cas pivot : bindingState "match" + typeConflict ⇒ NON bloqué (la preuve
// cryptographique gagne sur l'heuristique de nom). Tous les autres cas
// restent bloqués par B4 (checking/mismatch) ou par l'absence de fichier /
// une lecture de .sig en échec.
// =============================================================================
function checkBlocked(label, args, expected) {
  check(`otaManualSendBlocked: ${label}`, otaManualSendBlocked(args), expected);
}

checkBlocked("match + typeConflict ⇒ NON bloqué (preuve > heuristique)",
  { hasFile: true, sigReadState: "ok", typeConflict: true, bindingState: "match" }, false);
checkBlocked("match SANS typeConflict ⇒ non bloqué",
  { hasFile: true, sigReadState: "ok", typeConflict: false, bindingState: "match" }, false);
checkBlocked("mismatch + typeConflict ⇒ bloqué",
  { hasFile: true, sigReadState: "ok", typeConflict: true, bindingState: "mismatch" }, true);
checkBlocked("mismatch SANS typeConflict ⇒ bloqué quand même (B4 seul suffit)",
  { hasFile: true, sigReadState: "ok", typeConflict: false, bindingState: "mismatch" }, true);
checkBlocked("checking ⇒ bloqué (en attente de preuve)",
  { hasFile: true, sigReadState: "ok", typeConflict: false, bindingState: "checking" }, true);
checkBlocked("checking + typeConflict ⇒ bloqué",
  { hasFile: true, sigReadState: "ok", typeConflict: true, bindingState: "checking" }, true);
checkBlocked("sigReadState error ⇒ bloqué, même sans conflit ni liaison",
  { hasFile: true, sigReadState: "error", typeConflict: false, bindingState: "unknown" }, true);
checkBlocked("pas de fichier ⇒ bloqué, même .sig valide",
  { hasFile: false, sigReadState: "ok", typeConflict: false, bindingState: "unknown" }, true);
checkBlocked("unknown SANS .sig + fichier présent ⇒ NON bloqué (envoi sans signature légitime en D1/D2)",
  { hasFile: true, sigReadState: "empty", typeConflict: false, bindingState: "unknown" }, false);
checkBlocked("unknown + typeConflict (sig valide, liaison pas encore prouvée) ⇒ bloqué",
  { hasFile: true, sigReadState: "ok", typeConflict: true, bindingState: "unknown" }, true);

// =============================================================================
// sigHintDefaultCase — variant de hint par défaut sous "Signature (.sig)"
// quand AUCUN .sig n'est chargé (revue de code, point D "capacité perdue").
// ⭐ Cas pivot : sigReadState "empty" + fichier présent + nom NON reconnu
// ⇒ "defaulted" — c'est le cas où effectiveType() retombe SILENCIEUSEMENT
// sur "firmware" ; sans ce variant, rien ne le dit à l'opérateur.
// =============================================================================
function checkHintCase(label, args, expected) {
  check(`sigHintDefaultCase: ${label}`, sigHintDefaultCase(args), expected);
}

checkHintCase("sig lue (ok) ⇒ skip (handleSigChange a déjà écrit le hint), quel que soit le reste",
  { sigReadState: "ok", hasFile: true, typeFromName: "firmware" }, "skip");
checkHintCase("sig en échec (error) ⇒ skip, quel que soit le reste",
  { sigReadState: "error", hasFile: false, typeFromName: null }, "skip");
checkHintCase("empty + pas de fichier ⇒ no-file",
  { sigReadState: "empty", hasFile: false, typeFromName: null }, "no-file");
checkHintCase("empty + pas de fichier, typeFromName non nul (cas contrivé) ⇒ no-file quand même (hasFile prime)",
  { sigReadState: "empty", hasFile: false, typeFromName: "firmware" }, "no-file");
checkHintCase("empty + fichier + nom reconnu firmware ⇒ deduced",
  { sigReadState: "empty", hasFile: true, typeFromName: "firmware" }, "deduced");
checkHintCase("empty + fichier + nom reconnu filesystem ⇒ deduced",
  { sigReadState: "empty", hasFile: true, typeFromName: "filesystem" }, "deduced");
checkHintCase("empty + fichier + nom NON reconnu ⇒ defaulted (cas pivot : fallback silencieux à rendre visible)",
  { sigReadState: "empty", hasFile: true, typeFromName: null }, "defaulted");

// =============================================================================
// Épilogue
// =============================================================================
if (failures > 0) {
  console.error(`\n${failures} vecteur(s) en échec.`);
  process.exit(1);
}
console.log("\nTous les vecteurs sig_parse sont conformes.");
