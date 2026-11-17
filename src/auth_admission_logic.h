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
//   authentification désactivée -> route publique -> quota -> exemption de
//   l'assistant -> jeton -> refus.
// Le quota passe AVANT l'exemption : l'exemption n'échappe plus au rate limiting.
// =============================================================================

enum class AuthVerdict { kAllow, kDenyUnauth, kDenyRateLimited };

// ⚠️ Chaque champ porte son initialiseur : un champ AJOUTÉ plus tard et oublié
// chez un appelant vaudra false, donc n'ouvrira rien. Fail-closed PAR
// CONSTRUCTION, sans dépendre de la discipline du site d'appel.
struct AuthAdmissionInput {
  bool authEnabled = false;      // authentification activée
  bool publicRoute = false;      // RouteProtection::NONE
  bool firstBoot = false;        // assistant de configuration non terminé
  bool isSaveConfigUrl = false;  // URL EXACTEMENT égale à /save-config (pas de préfixe)
  bool rateLimitOk = false;      // quota respecté (ignoré si authEnabled == false ou route publique)
  bool tokenValid = false;       // X-Auth-Token présent et égal au jeton (temps constant)
};

inline AuthVerdict decideAdmission(const AuthAdmissionInput& in) {
  if (!in.authEnabled) return AuthVerdict::kAllow;
  if (in.publicRoute) return AuthVerdict::kAllow;
  if (!in.rateLimitOk) return AuthVerdict::kDenyRateLimited;
  if (in.firstBoot && in.isSaveConfigUrl) return AuthVerdict::kAllow;
  if (in.tokenValid) return AuthVerdict::kAllow;
  return AuthVerdict::kDenyUnauth;
}

#endif  // AUTH_ADMISSION_LOGIC_H
