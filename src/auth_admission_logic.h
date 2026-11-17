#ifndef AUTH_ADMISSION_LOGIC_H
#define AUTH_ADMISSION_LOGIC_H

// =============================================================================
// auth_admission_logic — décision d'admission d'une requête (bug-184), PURE
// =============================================================================
// Module sans dépendance Arduino (testable en natif, ADR-0017). Il ne journalise
// RIEN (ADR-0082) : le journal borné reste chez l'appelant (AuthManager).
//
// Une SEULE preuve d'identité : le jeton aléatoire X-Auth-Token. La voie HTTP
// Basic est retirée (comparaison insensible à la casse, aveugle à isFirstBoot).
//
// ORDRE IMPOSÉ (ne pas permuter) :
//   authentification désactivée -> route publique -> quota -> jeton -> refus.
// bug-185 : l'exemption anonyme du premier démarrage (/save-config) est SUPPRIMÉE ;
// le seul droit anonyme restant est /auth/change-password, route publique par
// déclaration (voir auth_bootstrap_logic.h).
// =============================================================================

enum class AuthVerdict { kAllow, kDenyUnauth, kDenyRateLimited };

// ⚠️ Chaque champ porte son initialiseur, et une AuthAdmissionInput{} construite
// par défaut REFUSE : un champ oublié chez un appelant ne peut pas ouvrir.
// Fail-closed PAR CONSTRUCTION, sans dépendre de la discipline du site d'appel.
// ⇒ test_entree_par_defaut_refuse affirme « ce n'est PAS kAllow », et rien de
// plus : c'est la propriété de SÉCURITÉ. (Le verdict tombe aujourd'hui en
// kDenyRateLimited — rateLimitOk vaut false — mais ⛔ ne pas épingler ce détail
// dans le test : il casserait sur un réordonnancement légitime des gardes, pour
// une raison qui n'aurait rien à voir avec la sécurité.)
//
// ⛔ CORRIGÉ LE 2026-11-17 (relevé par security-reviewer pendant bug-181) : ce
// commentaire AFFIRMAIT cette propriété alors que le code faisait l'INVERSE.
// `authEnabled` valait false par défaut, et la première ligne de decideAdmission
// est `if (!authEnabled) return kAllow` ⇒ une structure par défaut AUTORISAIT.
// ⚠️ Non exploitable alors (checkAuth affectait toujours le champ), mais c'était
// un piège armé pour le prochain appelant — sur le chemin d'admission de TOUTES
// les routes, DOSAGE COMPRIS. Le balayage des 16 combinaisons ne pouvait pas le
// voir : son helper remplit les quatre champs explicitement.
// ⇒ `authEnabled` est le SEUL champ dont la valeur fermée est `true` (« oui,
// l'authentification est active »). Même forme que wifi_status_exposure_logic.h.
struct AuthAdmissionInput {
  bool authEnabled = true;       // authentification activée (défaut : OUI, donc fermé)
  bool publicRoute = false;      // RouteProtection::NONE
  bool rateLimitOk = false;      // quota respecté (ignoré si authEnabled == false ou route publique)
  bool tokenValid = false;       // X-Auth-Token présent et égal au jeton (temps constant)
};

inline AuthVerdict decideAdmission(const AuthAdmissionInput& in) {
  if (!in.authEnabled) return AuthVerdict::kAllow;
  if (in.publicRoute) return AuthVerdict::kAllow;
  if (!in.rateLimitOk) return AuthVerdict::kDenyRateLimited;
  if (in.tokenValid) return AuthVerdict::kAllow;
  return AuthVerdict::kDenyUnauth;
}

#endif  // AUTH_ADMISSION_LOGIC_H
