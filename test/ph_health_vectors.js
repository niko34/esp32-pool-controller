// Vecteurs de test de la logique PURE de classement de santé pH
// (feature-154), extraite de `data/app.js`.
//
// Pourquoi ce fichier existe : l'écran « État sonde pH » affiche désormais
// trois pastilles (pente acide, pente base, décalage zéro) jugées par
// `classifyPhCriterion(value, minInclusive, maxInclusive)` — MIROIR exact
// d'`isPhProbeHealthy()` (`src/ph_guard_logic.h:138`), qui décide la bascule
// de sécurité pH côté firmware. Un bug de classement ici n'est pas cosmétique
// : il affiche un diagnostic qui diverge de celui qui a réellement coupé
// (ou laissé tourner) la régulation — c'est exactement le défaut n°2 du
// contexte de la spec (deux jugements de santé différents dans la même
// application). Un faux vert sur un critère hors plage INNOCENTE une sonde.
//
// CE QUE CE FICHIER VISE (patron `test/sha256_vectors.js`, feature-118 ;
// `test/trace_logic_vectors.js`, feature-153) : il EXTRAIT le texte réel de
// `classifyPhCriterion` depuis `data/app.js` (ancre + comptage de profondeur
// `{}` via `test/js_extract.js`, ADR-0099 amendé feature-154 — ce fichier
// est le TROISIÈME cas qui a motivé la factorisation du scanner), puis
// ÉVALUE ce texte extrait — jamais une recopie. Toute divergence future
// entre l'implémentation livrée et ce qui est testé est donc
// structurellement impossible : ce qui juge ICI est le code qui tourne dans
// le navigateur.
//
// ⚠️ ASSERTION STRUCTURELLE (ADR-0100, architect feature-154) : le texte
// extrait ne doit contenir AUCUN littéral numérique hors 0 et 1. C'est ce
// qui rend opposable le critère « les seuils de santé sont SERVIS, jamais
// réécrits côté client » — sans cette assertion, rien n'empêcherait un futur
// commit de recopier silencieusement 90/110/15 en dur dans la fonction tout
// en gardant son nom et sa signature, ce que le reste de ce harnais ne
// pourrait pas détecter (il fournit lui-même ces bornes en paramètres).
//
// Exécution : `node test/ph_health_vectors.js` (zéro dépendance npm, sort
// avec un code non nul si un vecteur échoue — utilisable en CI et en
// pre-commit local via `./tools/check_ph_health_vectors.sh`).
//
// Couverture demandée (spec feature-154, items D1/D2) :
//   - bornes exactes 90,0 / 110,0 / 15,0 incluses comme SAINES (comparaison
//     stricte `<`/`>`, miroir d'isPhProbeHealthy) ;
//   - juste au-delà des bornes (89,9 / 110,1) et le faux vert historique
//     (115 %, jamais testé avant ce lot) ⇒ 'hors' ;
//   - zéro NÉGATIF : le critère s'évalue sur (-zeroMax, +zeroMax), jamais
//     par Math.abs côté appelant pour les pentes (déjà des pourcentages
//     positifs) ;
//   - valeurs non exploitables (null/NaN/undefined/chaîne/Infinity)
//     ⇒ 'indisponible', jamais 'sain' (le vert ne doit pas être atteignable
//     par défaut) ;
//   - seuils absents/non finis/min>max ⇒ 'indisponible' AVANT tout autre
//     test ;
//   - cas réel utilisateur du 2026-10-11 (100,2 / 100,0 / 128,16) ⇒
//     sain · sain · hors.

"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const { extractBalancedFromAnchor } = require("./js_extract");

// ---------------------------------------------------------------------
// Extraction du code RÉELLEMENT livré depuis data/app.js (voir commentaire
// d'en-tête). Une seule ancre, qui doit apparaître EXACTEMENT une fois.
// ---------------------------------------------------------------------

const APP_JS_PATH = path.join(__dirname, "..", "data", "app.js");
const appJsSource = fs.readFileSync(APP_JS_PATH, "utf8");

const classifyPhCriterionSource = extractBalancedFromAnchor(
  appJsSource,
  "function classifyPhCriterion(value, minInclusive, maxInclusive) {",
  "{",
  "}"
).text;

// --- Évaluation du code EXTRAIT (jamais une recopie) ------------------------
const buildLive = new Function(`
  "use strict";
  ${classifyPhCriterionSource}
  return { classifyPhCriterion };
`);
const { classifyPhCriterion } = buildLive();

let failures = 0;

function check(label, actual, expected) {
  try {
    assert.strictEqual(actual, expected, label);
    console.log(`OK  ${label}`);
  } catch (e) {
    failures++;
    console.error("ÉCHEC", label, "—", e.message);
  }
}

// =============================================================================
// Assertion structurelle (ADR-0100) : aucun littéral numérique hors 0 et 1
// dans le texte EXTRAIT — les seuils doivent venir des paramètres, jamais
// être recopiés en dur dans le corps de la fonction.
// =============================================================================
{
  // Repère tout littéral numérique entier/décimal qui n'est PAS collé à une
  // lettre/underscore voisine (évite de confondre avec un identifiant) :
  // on retire d'abord les chaînes et commentaires pour ne juger QUE du code,
  // comme le fait l'extraction elle-même.
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

  const codeOnly = stripStringsAndComments(classifyPhCriterionSource);
  const numberRe = /\b\d+(?:\.\d+)?\b/g;
  const forbidden = [];
  let m;
  while ((m = numberRe.exec(codeOnly)) !== null) {
    if (m[0] !== "0" && m[0] !== "1") forbidden.push(m[0]);
  }
  check(
    "classifyPhCriterion : aucun littéral numérique hors 0 et 1 dans le code extrait",
    forbidden.length === 0 ? "aucun" : `trouvés: ${forbidden.join(",")}`,
    "aucun"
  );
}

// =============================================================================
// Bornes exactes des PENTES : plage servie (90,0 ; 110,0), comparaison
// stricte miroir d'isPhProbeHealthy (slope < min || slope > max -> hors).
// =============================================================================

const SLOPE_MIN = 90.0;
const SLOPE_MAX = 110.0;
const ZERO_MAX = 15.0;

check("pente : borne basse 90,0 incluse -> sain", classifyPhCriterion(90.0, SLOPE_MIN, SLOPE_MAX), "sain");
check("pente : borne haute 110,0 incluse -> sain", classifyPhCriterion(110.0, SLOPE_MIN, SLOPE_MAX), "sain");
check("zéro : borne haute 15,0 incluse -> sain", classifyPhCriterion(15.0, -ZERO_MAX, ZERO_MAX), "sain");

check("pente : juste sous la borne basse (89,9) -> hors", classifyPhCriterion(89.9, SLOPE_MIN, SLOPE_MAX), "hors");
check("pente : juste au-dessus de la borne haute (110,1) -> hors", classifyPhCriterion(110.1, SLOPE_MIN, SLOPE_MAX), "hors");
check("pente : 115 % (faux vert historique, jamais testé avant ce lot) -> hors", classifyPhCriterion(115, SLOPE_MIN, SLOPE_MAX), "hors");

// =============================================================================
// Zéro NÉGATIF : le critère s'évalue sur (-zeroMax, +zeroMax).
// =============================================================================

check("zéro : -15,0 (borne négative incluse) -> sain", classifyPhCriterion(-15.0, -ZERO_MAX, ZERO_MAX), "sain");
check("zéro : -15,1 (juste hors borne négative) -> hors", classifyPhCriterion(-15.1, -ZERO_MAX, ZERO_MAX), "hors");
check("zéro : -128,16 -> hors", classifyPhCriterion(-128.16, -ZERO_MAX, ZERO_MAX), "hors");

// =============================================================================
// Valeurs non exploitables -> 'indisponible', JAMAIS 'sain'.
// =============================================================================

check("valeur null -> indisponible", classifyPhCriterion(null, SLOPE_MIN, SLOPE_MAX), "indisponible");
check("valeur NaN -> indisponible", classifyPhCriterion(NaN, SLOPE_MIN, SLOPE_MAX), "indisponible");
check("valeur undefined -> indisponible", classifyPhCriterion(undefined, SLOPE_MIN, SLOPE_MAX), "indisponible");
check("valeur chaîne -> indisponible", classifyPhCriterion("100.2", SLOPE_MIN, SLOPE_MAX), "indisponible");
check("valeur Infinity -> indisponible", classifyPhCriterion(Infinity, SLOPE_MIN, SLOPE_MAX), "indisponible");

// =============================================================================
// Seuils absents / non finis / min > max -> 'indisponible', jamais 'sain'
// (le vert doit être STRUCTURELLEMENT inatteignable sans seuils reçus).
// =============================================================================

check("seuils absents (undefined) -> indisponible", classifyPhCriterion(100.0, undefined, undefined), "indisponible");
check("borne min NaN -> indisponible", classifyPhCriterion(100.0, NaN, SLOPE_MAX), "indisponible");
check("borne max NaN -> indisponible", classifyPhCriterion(100.0, SLOPE_MIN, NaN), "indisponible");
check("borne min Infinity -> indisponible", classifyPhCriterion(100.0, -Infinity, SLOPE_MAX), "indisponible");
check("borne max Infinity -> indisponible", classifyPhCriterion(100.0, SLOPE_MIN, Infinity), "indisponible");
check("min > max -> indisponible (jamais sain)", classifyPhCriterion(100.0, SLOPE_MAX, SLOPE_MIN), "indisponible");

// =============================================================================
// Cas réel utilisateur (2026-10-11) : pente acide 100,2 %, pente base
// 100,0 %, zéro 128,16 mV -> sain · sain · hors. Les deux pentes sont
// parfaites, c'est la jonction de référence (zéro) qui est en cause — le
// défaut corrigé par cette spec (le verdict global amalgamait les trois).
// =============================================================================

check("cas réel : pente acide 100,2 % -> sain", classifyPhCriterion(100.2, SLOPE_MIN, SLOPE_MAX), "sain");
check("cas réel : pente base 100,0 % -> sain", classifyPhCriterion(100.0, SLOPE_MIN, SLOPE_MAX), "sain");
check("cas réel : zéro 128,16 mV -> hors", classifyPhCriterion(128.16, -ZERO_MAX, ZERO_MAX), "hors");

if (failures > 0) {
  console.error(`\n${failures} vecteur(s) en échec.`);
  process.exit(1);
}
console.log("\nTous les vecteurs de santé pH sont conformes (code EXTRAIT de data/app.js, pas une copie).");
