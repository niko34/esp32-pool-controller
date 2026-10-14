// Vecteurs de test de la décision PURE « quel mode AFFICHER sur le sélecteur
// de régulation pH/ORP après une tentative de bascule ? » (feature-161,
// correctif bloquant du code-reviewer), extraite de `data/app.js`.
//
// Pourquoi ce fichier existe : la revue a rendu « Request changes » avec DEUX
// bloquants liés. Le bloquant [1] — annuler la confirmation C4 (« Interrompre
// la chloration de secours ? ») laissait le sélecteur ORP affiché sur
// « Arrêt » alors que la régulation continuait de tourner, jusqu'au prochain
// rafraîchissement — est la conséquence DIRECTE du bloquant [2] : ce chemin
// de retour à l'état réel n'était pas une décision extraite, donc pas
// testable. Un mutant qui le supprimait ne mourait pas.
//
// `resolveSelectorMode(requested, lastConfirmed, outcome)` est cette décision,
// désormais PURE et extraite : elle ne touche jamais le DOM, seulement le
// mode à afficher. Elle est appelée depuis `applyRegulationMode()` à CHAQUE
// sortie (optimiste, succès, refus serveur, panne réseau, annulation C4), et
// c'est ce chemin UNIQUE — partagé par la carte du tableau de bord ET la page
// de réglage depuis l'extension C4 du 2026-10-13 — qui garantit que le
// sélecteur ne peut plus afficher un mode que l'appareil n'a pas adopté.
//
// CE QUE CE FICHIER VISE (patron `test/cal_points_acquired_vectors.js`,
// feature-159 ; `test/ph_health_vectors.js`, feature-154) : il EXTRAIT le
// texte réel de `resolveSelectorMode` depuis `data/app.js` (ancre + comptage
// de profondeur `{}` via `test/js_extract.js`, ADR-0099) et l'ÉVALUE — jamais
// une recopie. Toute divergence future entre l'implémentation livrée et ce
// qui est testé est donc structurellement impossible.
//
// Exécution : `node test/resolve_selector_mode_vectors.js` (zéro dépendance
// npm, sort avec un code non nul si un vecteur échoue — utilisable en CI et
// en pre-commit local via `./tools/check_resolve_selector_mode_vectors.sh`).
//
// Couverture demandée (code-reviewer, feature-161) :
//   - succès ('ok')                              ⇒ mode DEMANDÉ ;
//   - refus serveur (409/400)                     ⇒ dernier mode CONFIRMÉ ;
//   - annulation (confirmation C4 refusée)        ⇒ dernier mode CONFIRMÉ ;
//   - panne réseau (contrôleur injoignable)       ⇒ dernier mode CONFIRMÉ
//     (sendConfigDetailed traite déjà panne réseau et refus serveur de
//     façon IDENTIQUE — ok:false — donc les deux vecteurs partagent le même
//     outcome 'refus' : c'est exactement le comportement à verrouiller) ;
//   - lastConfirmed INCONNU (undefined/null)      ⇒ restitué TEL QUEL, sans
//     repli inventé par cette fonction générique.
//
// Mutants, et leur sort :
//   - rendre `requested` sur un refus            ⇒ tué par le vecteur refus ;
//   - rendre `requested` sur une annulation       ⇒ tué par le vecteur annule ;
//   - confondre « refus » et « succès » (inverser
//     la condition : `outcome === "refus"` au lieu
//     de `outcome === "ok"`)                      ⇒ tué par le vecteur 'ok'
//     (bascule à tort sur lastConfirmed) ET par les vecteurs refus/annule
//     (basculent à tort sur requested).

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

const resolveSelectorModeSource = extractBalancedFromAnchor(
  appJsSource,
  "function resolveSelectorMode(requested, lastConfirmed, outcome) {",
  "{",
  "}"
).text;

// --- Évaluation du code EXTRAIT (jamais une recopie) ------------------------
const buildLive = new Function(`
  "use strict";
  ${resolveSelectorModeSource}
  return { resolveSelectorMode };
`);
const { resolveSelectorMode } = buildLive();

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

// =============================================================================
// Succès serveur ('ok') ⇒ le sélecteur affiche le mode DEMANDÉ, qu'il
// s'agisse de pH ou d'ORP, et quel que soit le dernier mode confirmé.
// =============================================================================
check("ph: succès (automatic -> manual) -> manual", resolveSelectorMode("manual", "automatic", "ok"), "manual");
check("orp: succès (automatic -> scheduled) -> scheduled", resolveSelectorMode("scheduled", "automatic", "ok"), "scheduled");
check("succès même si requested === lastConfirmed -> requested", resolveSelectorMode("automatic", "automatic", "ok"), "automatic");

// =============================================================================
// Refus serveur (409/400, sendConfigDetailed ok:false) ⇒ dernier mode
// CONFIRMÉ — jamais le mode demandé, même s'il a été affiché de façon
// optimiste entre-temps.
// =============================================================================
check("refus serveur 409 (ph, gate sécurité) -> dernier confirmé", resolveSelectorMode("manual", "automatic", "refus"), "automatic");
check("refus serveur 400 (orp) -> dernier confirmé", resolveSelectorMode("manual", "scheduled", "refus"), "scheduled");

// =============================================================================
// Panne réseau (contrôleur injoignable) ⇒ MÊME outcome 'refus' que le refus
// serveur (sendConfigDetailed ne distingue pas les deux, status:0 vs
// status:409/400, mais ok:false dans les deux cas) ⇒ même verdict : dernier
// mode CONFIRMÉ.
// =============================================================================
check("panne réseau (contrôleur injoignable) -> dernier confirmé", resolveSelectorMode("manual", "automatic", "refus"), "automatic");

// =============================================================================
// Annulation de la confirmation C4 (« Interrompre la chloration de secours ? »
// refusée par l'utilisateur) ⇒ RIEN n'a été envoyé au serveur ⇒ dernier mode
// CONFIRMÉ. C'est le bloquant [1] : AVANT ce correctif, le sélecteur restait
// affiché sur `requested` ("manual"/Arrêt) après une annulation.
// =============================================================================
check("annulation C4 (orp, secours en cours) -> dernier confirmé", resolveSelectorMode("manual", "automatic", "annule"), "automatic");
check("annulation C4, dernier confirmé = scheduled -> scheduled", resolveSelectorMode("manual", "scheduled", "annule"), "scheduled");

// =============================================================================
// lastConfirmed INCONNU (undefined/null, ex. tout premier chargement avant
// /get-config) ⇒ restitué TEL QUEL par cette fonction générique — c'est à
// l'appelant (qui connaît le repli par kind) de combler ce trou, pas à elle.
// =============================================================================
check("refus, lastConfirmed undefined -> undefined (restitué tel quel)", resolveSelectorMode("manual", undefined, "refus"), undefined);
check("annulation, lastConfirmed null -> null (restitué tel quel)", resolveSelectorMode("manual", null, "annule"), null);

if (failures > 0) {
  console.error(`\n${failures} vecteur(s) en échec.`);
  process.exit(1);
}
console.log("\nTous les vecteurs resolveSelectorMode sont conformes.");
