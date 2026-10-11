// Vecteurs de test du hachage SHA-256 pur JS (feature-118, page d'upload OTA).
//
// Pourquoi ce fichier existe : `crypto.subtle` est INDISPONIBLE sur la page
// d'upload (HTTP en clair sur une IP de LAN, contexte non sécurisé). La page
// embarque donc une implémentation SHA-256 pure JS (voir `data/app.js`,
// fonctions `createSha256()` / `sha256OfFile()`). Un faux positif de ce calcul
// se traduit par un refus d'upload à l'écran : un bug ici rend la page
// inutilisable, d'où l'exigence de vecteurs VERSIONNÉS (condition C5 /
// security-reviewer P1, feature-118), pas seulement un prototype de scratchpad.
//
// ⚠️ CE QUE CE FICHIER VISE — corrigé après P2/security-reviewer (condition 1,
// feature-118) : la PREMIÈRE version recopiait l'implémentation dans ce
// fichier. Une divergence entre la copie et `data/app.js` serait restée
// invisible — exactement le défaut relevé sur la première version de
// test_native_dose_banner_silence (feature-148) : un miroir qui RÉIMPLÉMENTE
// au lieu d'EXTRAIRE teste du code mort dès que l'original change. Ce fichier
// EXTRAIT désormais le texte réel de `const SHA256_K = …` et
// `function createSha256() { … }` depuis `data/app.js` (recherche d'ancre +
// comptage de profondeur `[]`/`{}` qui ignore les chaînes et commentaires,
// patron déjà utilisé par les sentinelles natives `test_native_ws_config_
// parity` / `test_native_dose_banner_silence`, transposé ici en JS), puis
// ÉVALUE ce texte extrait — jamais une recopie. Toute divergence future entre
// l'implémentation livrée et ce qui est testé est donc STRUCTURELLEMENT
// impossible : ce qui est haché ICI est le code qui tourne dans le navigateur.
//
// Exécution : `node test/sha256_vectors.js` (zéro dépendance npm, sort avec un
// code non nul si un vecteur échoue — utilisable en CI et en pre-commit local
// via `./tools/check_sha256_vectors.sh`).
//
// Couverture demandée par security-reviewer :
//   - chaîne vide
//   - "abc"
//   - messages de 55 / 56 / 64 / 65 octets (bourrage : bascule 1 ou 2 blocs)
//   - un bloc > 1 Mo, en un seul appel ET fragmenté (comme sha256OfFile()
//     qui tranche en 256 Kio) — les deux doivent produire le même résultat.
//
// Référence de vérité : `node:crypto`, PAS des constantes recopiées à la main.

"use strict";

const crypto = require("crypto");
const assert = require("assert");
const fs = require("fs");
const path = require("path");

// ---------------------------------------------------------------------
// Extraction du code RÉELLEMENT livré depuis data/app.js (voir commentaire
// d'en-tête). Deux ancres, chacune doit apparaître EXACTEMENT une fois :
//   - "const SHA256_K = new Uint32Array([" → table des 64 constantes K
//   - "function createSha256() {"          → fabrique du hacheur incrémental
// ---------------------------------------------------------------------

const APP_JS_PATH = path.join(__dirname, "..", "data", "app.js");
const appJsSource = fs.readFileSync(APP_JS_PATH, "utf8");

// Retourne l'index du caractère de FERMETURE correspondant à `openChar` situé
// à `openIdx` dans `src` (qui doit être `openChar`), en ignorant tout
// `openChar`/`closeChar` situé à l'intérieur d'une chaîne ('...'/"..."/`...`)
// ou d'un commentaire (// ou /* */) — un texte extrait doit rester du CODE,
// jamais une citation en commentaire (même piège que bug-098 / check_fopen_
// guard.sh / test_native_dose_banner_silence).
function findMatchingClose(src, openIdx, openChar, closeChar) {
  if (src[openIdx] !== openChar) {
    throw new Error(`findMatchingClose: src[${openIdx}] n'est pas '${openChar}'`);
  }
  let depth = 0;
  let i = openIdx;
  while (i < src.length) {
    const c = src[i];
    if (c === '"' || c === "'" || c === "`") {
      const quote = c;
      i++;
      while (i < src.length && src[i] !== quote) {
        i += src[i] === "\\" ? 2 : 1;
      }
      i++;
      continue;
    }
    if (c === "/" && src[i + 1] === "/") {
      i += 2;
      while (i < src.length && src[i] !== "\n") i++;
      continue;
    }
    if (c === "/" && src[i + 1] === "*") {
      i += 2;
      while (i < src.length && !(src[i] === "*" && src[i + 1] === "/")) i++;
      i += 2;
      continue;
    }
    if (c === openChar) {
      depth++;
    } else if (c === closeChar) {
      depth--;
      if (depth === 0) return i;
    }
    i++;
  }
  throw new Error(`findMatchingClose: aucune fermeture '${closeChar}' trouvée pour l'ouverture à l'index ${openIdx}`);
}

// Localise `anchorText` dans `src`, exige UNE SEULE occurrence (sinon
// l'extraction serait ambiguë — même garde que les sentinelles natives),
// et retourne la portion de `src` depuis le début de l'ancre jusqu'au
// caractère `closeChar` correspondant au `openChar` qui termine l'ancre
// (inclus).
function extractBalancedFromAnchor(src, anchorText, openChar, closeChar) {
  const firstIdx = src.indexOf(anchorText);
  if (firstIdx === -1) {
    throw new Error(`Ancre introuvable dans data/app.js : ${JSON.stringify(anchorText)}`);
  }
  if (src.indexOf(anchorText, firstIdx + 1) !== -1) {
    throw new Error(`Ancre AMBIGUË (plusieurs occurrences) dans data/app.js : ${JSON.stringify(anchorText)}`);
  }
  const openIdx = firstIdx + anchorText.length - 1;
  const closeIdx = findMatchingClose(src, openIdx, openChar, closeChar);
  return { text: src.slice(firstIdx, closeIdx + 1), closeIdx };
}

// --- SHA256_K --------------------------------------------------------------
// L'ancre se termine par "[" (ouverture du littéral de tableau) ; on extrait
// jusqu'au "]" correspondant, puis on VÉRIFIE que le source continue bien par
// ");" juste après (la forme `new Uint32Array([ ... ])`) — si cette
// hypothèse de syntaxe ne tient plus, on échoue fort plutôt que de
// reconstruire un extrait syntaxiquement faux en silence.
const sha256KAnchor = "const SHA256_K = new Uint32Array([";
const sha256KExtract = extractBalancedFromAnchor(appJsSource, sha256KAnchor, "[", "]");
const afterSha256K = appJsSource.slice(sha256KExtract.closeIdx + 1, sha256KExtract.closeIdx + 3);
if (afterSha256K !== ");") {
  throw new Error(
    `data/app.js : la forme de SHA256_K a changé (attendu ");" juste après le tableau, trouvé ${JSON.stringify(afterSha256K)}) — adapter l'extraction`
  );
}
const sha256KSource = sha256KExtract.text + ");";

// --- createSha256 -----------------------------------------------------------
// L'ancre se termine par "{" (ouverture du corps de fonction) ; la fonction
// entière (y compris compress() imbriquée et l'objet retourné) est extraite
// jusqu'à l'accolade fermante correspondante.
const createSha256Anchor = "function createSha256() {";
const createSha256Source = extractBalancedFromAnchor(appJsSource, createSha256Anchor, "{", "}").text;

// --- Évaluation du code EXTRAIT (jamais une recopie) ------------------------
const buildLiveCreateSha256 = new Function(`
  "use strict";
  ${sha256KSource}
  ${createSha256Source}
  return createSha256;
`);
const createSha256 = buildLiveCreateSha256();

// Référence de vérité : node:crypto, jamais une constante recopiée à la main.
function referenceHex(buf) {
  return crypto.createHash("sha256").update(buf).digest("hex");
}

// Hache en un seul appel.
function hashWhole(buf) {
  const hasher = createSha256();
  hasher.update(new Uint8Array(buf));
  return hasher.hex();
}

// Hache en fragments de CHUNK octets, comme sha256OfFile() tranche le fichier
// réel en 256 Kio — c'est le chemin que le navigateur emprunte vraiment.
function hashFragmented(buf, chunk) {
  const hasher = createSha256();
  const bytes = new Uint8Array(buf);
  for (let off = 0; off < bytes.length; off += chunk) {
    hasher.update(bytes.subarray(off, Math.min(off + chunk, bytes.length)));
  }
  return hasher.hex();
}

let failures = 0;

function check(label, buf, fragmentSizes) {
  const expected = referenceHex(buf);
  const whole = hashWhole(buf);
  try {
    assert.strictEqual(whole, expected, `${label} (appel unique)`);
  } catch (e) {
    failures++;
    console.error("ÉCHEC", e.message);
  }
  for (const frag of fragmentSizes) {
    const got = hashFragmented(buf, frag);
    try {
      assert.strictEqual(got, expected, `${label} (fragmenté par ${frag})`);
    } catch (e) {
      failures++;
      console.error("ÉCHEC", e.message);
    }
  }
  console.log(`OK  ${label.padEnd(28)} ${expected}`);
}

// 1. Chaîne vide — bourrage dans un unique bloc de 64 octets.
check("chaîne vide (0 o)", Buffer.alloc(0), [262144, 7]);

// 2. "abc" — vecteur de test canonique du NIST (FIPS 180-4).
check('"abc" (3 o)', Buffer.from("abc", "utf8"), [262144, 1]);

// 3. Tailles de bord du bourrage : 55/56/64/65 octets.
//    55 o : (55+1) <= 56 -> bourrage tient dans le même bloc de 64.
//    56 o : (56+1) >  56 -> bascule sur un second bloc de bourrage.
//    64 o : un bloc complet, exactement, + bourrage sur un second bloc.
//    65 o : un bloc complet + 1 octet, + bourrage sur un second bloc.
for (const size of [55, 56, 64, 65]) {
  const buf = Buffer.alloc(size);
  for (let i = 0; i < size; i++) buf[i] = i & 0xff;  // contenu déterministe, non nul
  check(`${size} octets`, buf, [262144, 64, 17]);
}

// 4. Bloc > 1 Mo, en un seul appel ET fragmenté comme le ferait sha256OfFile()
//    avec CHUNK=262144 (256 Kio) sur un vrai fichier .bin OTA.
{
  const size = 1024 * 1024 + 777;  // > 1 Mo, taille non alignée sur le chunk
  const big = Buffer.alloc(size);
  for (let i = 0; i < size; i++) big[i] = (i * 2654435761) & 0xff;  // motif pseudo-aléatoire
  check(`> 1 Mo (${size} o)`, big, [262144, 65536]);
}

if (failures > 0) {
  console.error(`\n${failures} vecteur(s) en échec.`);
  process.exit(1);
}
console.log("\nTous les vecteurs SHA-256 JS sont conformes à node:crypto (code EXTRAIT de data/app.js, pas une copie).");
