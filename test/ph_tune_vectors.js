// Vecteurs de test de la logique PURE de l'APERÇU d'accord pH sur un test
// manuel (feature-155), extraite de `data/app.js`.
//
// Pourquoi ce fichier existe : l'écran « Correction manuelle de la mesure »
// (anciennement « offset pH ») laisse l'utilisateur AMENER un nombre à la
// valeur lue sur son test, et affiche un APERÇU en direct (ce que l'appareil
// lit, ce que dit le test, l'écart qui en résulte) AVANT que la décision
// réelle soit prise — côté SERVEUR, par `evaluatePhTune()`
// (`src/ph_tune_logic.h`), seule autorité sur le dosage (condition 1 du gate
// pool-chemistry). Un bug dans l'aperçu n'est pas cosmétique : c'est
// précisément un écart entre aperçu et serveur qui a révélé, dans ce lot
// même, que le refus OutOfRange serveur était MORT (roundPhOffsetToStep()
// clampe avant d'arrondir) — trouvé par web-ui-developer en câblant
// l'aperçu, PAS par la relecture du firmware. Ce harnais existe pour que
// cette classe de défaut soit détectée par une machine, pas par hasard.
//
// CE QUE CE FICHIER VISE (patron `test/sha256_vectors.js`, feature-118 ;
// `test/trace_logic_vectors.js`, feature-153 ; `test/ph_health_vectors.js`,
// feature-154) : il EXTRAIT le texte réel d'`evaluatePhTunePreview` depuis
// `data/app.js` (ancre + comptage de profondeur `{}` via `test/js_extract.js`,
// ADR-0099 amendé), puis ÉVALUE ce texte extrait — jamais une recopie.
//
// ⚠️ ASSERTION STRUCTURELLE (ADR-0100, reconduite depuis feature-154) : le
// texte extrait ne doit contenir AUCUN littéral numérique hors 0 et 1 — les
// seuils (pas, bornes) sont SERVIS par /get-config (ph_offset_step,
// ph_offset_min, ph_offset_max), jamais réécrits en dur côté client.
//
// Exécution : `node test/ph_tune_vectors.js` (zéro dépendance npm, sort avec
// un code non nul si un vecteur échoue — utilisable en CI et en pre-commit
// local via `./tools/check_ph_tune_vectors.sh`).
//
// Couverture demandée (spec feature-155) :
//   - mesure indisponible (phValueRaw non fini) ⇒ mesureIndisponible=true,
//     aucun autre champ signifiant ;
//   - bornes EXACTES ±0,50 (acceptées, horsBornes=false) et ±0,51 (refusées,
//     horsBornes=true) — en NOMBRE DE PAS ENTIER, pas en comparaison flottante ;
//   - 🔴 écart +0,80 : horsBornes=true, ET valeurAtteinte annonce bien la
//     valeur demandée NON écrêtée (l'aperçu ne doit RIEN masquer : contrat
//     différent du serveur, qui refuse — ici on vérifie que l'aperçu
//     SIGNALE le dépassement sans l'absorber) ;
//   - valeurAtteinte = phValueRaw + ecart, jamais valeurAmenee (Piège 1) ;
//   - 🔴 COHÉRENCE APERÇU ↔ SERVEUR : pour un même jeu d'entrées (dans le
//     domaine commun aux deux — hors filtration/calPoints, qui n'existent
//     que côté serveur), le verdict "accordé / hors bornes" de l'aperçu et
//     celui d'evaluatePhTune() (réimplémenté ici en JS, MIROIR fidèle,
//     uniquement pour cette comparaison) doivent coïncider. C'est cet écart,
//     laissé non vérifié, qui a caché le refus mort.

"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const { extractBalancedFromAnchor } = require("./js_extract");

// ---------------------------------------------------------------------
// Extraction du code RÉELLEMENT livré depuis data/app.js.
// ---------------------------------------------------------------------

const APP_JS_PATH = path.join(__dirname, "..", "data", "app.js");
const appJsSource = fs.readFileSync(APP_JS_PATH, "utf8");

const previewSource = extractBalancedFromAnchor(
  appJsSource,
  "function evaluatePhTunePreview(valeurAmenee, phValueRaw, offsetStep, offsetMin, offsetMax) {",
  "{",
  "}"
).text;

// --- Évaluation du code EXTRAIT (jamais une recopie) ------------------------
const buildLive = new Function(`
  "use strict";
  ${previewSource}
  return { evaluatePhTunePreview };
`);
const { evaluatePhTunePreview } = buildLive();

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

function checkBool(label, actual, expected) {
  try {
    assert.strictEqual(actual, expected, label);
    console.log(`OK  ${label}`);
  } catch (e) {
    failures++;
    console.error("ÉCHEC", label, "—", e.message);
  }
}

function checkFloat(label, actual, expected, eps) {
  const e = eps || 1e-6;
  try {
    assert.ok(
      Math.abs(actual - expected) <= e,
      `${label} — attendu ${expected}, obtenu ${actual}`
    );
    console.log(`OK  ${label}`);
  } catch (e2) {
    failures++;
    console.error("ÉCHEC", label, "—", e2.message);
  }
}

// =============================================================================
// Assertion structurelle (ADR-0100) : aucun littéral numérique hors 0 et 1
// dans le texte EXTRAIT.
// =============================================================================
{
  function stripStringsAndComments(src) {
    let out = "";
    let i = 0;
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
      out += c;
      i++;
    }
    return out;
  }

  const codeOnly = stripStringsAndComments(previewSource);
  const numberRe = /\b\d+(?:\.\d+)?\b/g;
  const forbidden = [];
  let m;
  while ((m = numberRe.exec(codeOnly)) !== null) {
    if (m[0] !== "0" && m[0] !== "1") forbidden.push(m[0]);
  }
  checkBool(
    "evaluatePhTunePreview : aucun littéral numérique hors 0 et 1 dans le code extrait",
    forbidden.length === 0,
    true
  );
  if (forbidden.length > 0) {
    console.error("  littéraux trouvés :", forbidden.join(","));
  }
}

// =============================================================================
// Mesure indisponible — aucun autre champ n'est alors signifiant.
// =============================================================================

check(
  "phValueRaw NaN -> mesureIndisponible",
  evaluatePhTunePreview(7.4, NaN, 0.1, -0.5, 0.5),
  { mesureIndisponible: true, ecart: null, horsBornes: false, valeurAtteinte: null }
);
check(
  "phValueRaw undefined -> mesureIndisponible",
  evaluatePhTunePreview(7.4, undefined, 0.1, -0.5, 0.5),
  { mesureIndisponible: true, ecart: null, horsBornes: false, valeurAtteinte: null }
);
check(
  "phValueRaw chaîne -> mesureIndisponible",
  evaluatePhTunePreview(7.4, "7.13", 0.1, -0.5, 0.5),
  { mesureIndisponible: true, ecart: null, horsBornes: false, valeurAtteinte: null }
);

// =============================================================================
// Bornes EXACTES ±0,50 acceptées (horsBornes=false), ±0,51 refusées
// (horsBornes=true) — en nombre de pas entier (pas = 0,10).
// =============================================================================

{
  const r = evaluatePhTunePreview(7.5, 7.0, 0.1, -0.5, 0.5);
  checkFloat("borne +0,50 : ecart", r.ecart, 0.5);
  checkBool("borne +0,50 : horsBornes=false", r.horsBornes, false);
}
{
  const r = evaluatePhTunePreview(6.5, 7.0, 0.1, -0.5, 0.5);
  checkFloat("borne -0,50 : ecart", r.ecart, -0.5);
  checkBool("borne -0,50 : horsBornes=false", r.horsBornes, false);
}
{
  const r = evaluatePhTunePreview(7.51, 7.0, 0.1, -0.5, 0.5);
  checkBool("borne +0,51 : horsBornes=true", r.horsBornes, true);
}
{
  const r = evaluatePhTunePreview(6.49, 7.0, 0.1, -0.5, 0.5);
  checkBool("borne -0,51 : horsBornes=true", r.horsBornes, true);
}

// =============================================================================
// 🔴 Écart +0,80 : horsBornes=true, ET l'aperçu ne MASQUE rien — ecart et
// valeurAtteinte reflètent la demande NON écrêtée (contrat de l'aperçu,
// différent de celui du serveur qui refuse d'appliquer).
// =============================================================================
{
  const r = evaluatePhTunePreview(7.8, 7.0, 0.1, -0.5, 0.5);
  checkFloat("écart +0,80 : ecart NON écrêté", r.ecart, 0.8);
  checkBool("écart +0,80 : horsBornes=true", r.horsBornes, true);
  checkFloat("écart +0,80 : valeurAtteinte = phValueRaw + ecart", r.valeurAtteinte, 7.8);
}

// =============================================================================
// valeurAtteinte = phValueRaw + ecart, JAMAIS valeurAmenee (Piège 1).
// Référence 7,45 / lu 7,13 ⇒ ecart arrondi au pas le plus proche de 0,32,
// soit 0,30 (pasAmene = round(0,32/0,1) = round(3,2) = 3 -> 0,30).
// =============================================================================
{
  const r = evaluatePhTunePreview(7.45, 7.13, 0.1, -0.5, 0.5);
  checkFloat("7,45/7,13 : ecart arrondi au pas", r.ecart, 0.3, 1e-4);
  checkBool("7,45/7,13 : horsBornes=false", r.horsBornes, false);
  checkFloat("7,45/7,13 : valeurAtteinte = 7,43 (PAS 7,45)", r.valeurAtteinte, 7.43, 1e-4);
}

// =============================================================================
// 🔴 COHÉRENCE APERÇU ↔ SERVEUR : pour un même jeu d'entrées (hors
// filtration/calPoints, absents de l'aperçu), le verdict accordé/hors-bornes
// doit coïncider entre evaluatePhTunePreview (JS, aperçu) et evaluatePhTune
// (C++, serveur). On réimplémente ici un MIROIR MINIMAL de la partie bornes
// d'evaluatePhTune (pas de filtration/calPoints : testés côté natif C++,
// test/test_native_ph_tune) pour comparer les deux verdicts sur le même
// domaine. C'est exactement cet écart qui, non vérifié, a laissé vivre le
// refus serveur mort pendant que l'aperçu, lui, affichait déjà "hors bornes".
// =============================================================================
function serverVerdictOutOfRange(phReference, phValueRaw, offsetMin, offsetMax) {
  // Miroir de ph_tune_logic.h après le correctif : le refus porte sur
  // l'écart DEMANDÉ, jamais sur la sortie de l'arrondi.
  const offsetDemande = phReference - phValueRaw;
  return offsetDemande < offsetMin || offsetDemande > offsetMax;
}

const parityVectors = [
  { ref: 7.5, raw: 7.0 },    // borne exacte +0,50
  { ref: 6.5, raw: 7.0 },    // borne exacte -0,50
  { ref: 7.51, raw: 7.0 },   // juste hors +0,50
  { ref: 6.49, raw: 7.0 },   // juste hors -0,50
  { ref: 7.8, raw: 7.0 },    // 🔴 le vecteur qui a révélé l'écart
  { ref: 7.45, raw: 7.13 },  // cas nominal du gate
  { ref: 7.0, raw: 7.0 },    // écart nul
];

for (const v of parityVectors) {
  const preview = evaluatePhTunePreview(v.ref, v.raw, 0.1, -0.5, 0.5);
  const serverHorsBornes = serverVerdictOutOfRange(v.ref, v.raw, -0.5, 0.5);
  checkBool(
    `parité aperçu/serveur (ref=${v.ref}, raw=${v.raw}) : horsBornes identique`,
    preview.horsBornes,
    serverHorsBornes
  );
}

if (failures > 0) {
  console.error(`\n${failures} vecteur(s) en échec.`);
  process.exit(1);
}
console.log("\nTous les vecteurs d'accord pH sont conformes (code EXTRAIT de data/app.js, pas une copie).");
