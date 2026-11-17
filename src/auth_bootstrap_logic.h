#ifndef AUTH_BOOTSTRAP_LOGIC_H
#define AUTH_BOOTSTRAP_LOGIC_H

#include <cstring>

// =============================================================================
// auth_bootstrap_logic — état « vierge » / « possédé » et décisions associées
// (bug-185), PURE
// =============================================================================
// Module sans dépendance Arduino (testable en natif, ADR-0017), SANS allocation
// et sans journalisation (ADR-0082).
//
// INVARIANT UNIQUE : un droit anonyme n'existe que si aucun mot de passe n'a
// jamais été choisi, et il se limite à en choisir un.
//
// ⚠️ DEUX NOTIONS DISTINCTES, ⛔ NE JAMAIS LES CONFONDRE :
//   · `factoryPassword` / `virginState` — « aucun mot de passe n'a jamais été
//     choisi ». C'est LUI qui commande les droits anonymes et le refus de
//     session. Un inconnu ne peut pas le poser.
//   · `firstBoot` — « l'assistant doit encore être montré ». Sert à l'INTERFACE
//     et à la fermeture de la fenêtre, ⛔ JAMAIS à accorder un droit : il reste
//     vrai sur un appareil déjà possédé dont l'assistant a été interrompu.
//
// ⛔ L'état se déduit du MOT DE PASSE, jamais du seul drapeau NVS wizardCompleted :
// c'est précisément le bit qu'un inconnu pouvait poser (POST /auth/complete-wizard
// anonyme). Un état de sécurité ne dépend pas d'un bit que l'attaquant contrôle.
//
// Chaque struct porte ses initialiseurs `= false`.
//
// ⚠️ MAIS « = false » N'EST PAS PARTOUT L'ÉTAT FERMÉ — ⛔ affirmation corrigée le
// 2026-11-17 (relevée par security-reviewer pendant bug-181, même défaut que celui
// d'auth_admission_logic.h : un commentaire qui promettait plus que le code).
//   · `PwdChangeInput` : OUI, fail-closed. Une entrée tout-à-faux rend kDenyUnauth.
//     (⚠️ sauf `newEqualsCurrent`, dont l'oubli saute un contrôle d'HYGIÈNE — pas
//     d'autorisation.)
//   · ⛔ `AuthEffectPlan` : NON. Tout-à-faux = pas de rotation du jeton, pas de
//     révocation des WebSockets, fenêtre non refermée — c'est l'état OUVERT. Un
//     plan vide ne protège rien.
//     ⇒ `planAfterPasswordChange()` est la SEULE fabrique légitime, elle pose les
//     quatre champs, et test_effect_plan la couvre. ⛔ NE JAMAIS construire un
//     AuthEffectPlan à la main : passer par la fabrique.
// =============================================================================

// Mot de passe d'usine : "admin", OU VIDE. Le vide est atteignable (une lecture
// NVS vide peut écraser adminPassword) et /auth/login compare par égalité : un
// mot de passe vide serait sinon accepté par un corps {"password":""}.
inline bool isFactoryPasswordLiteral(const char* pwd) {
  return pwd == nullptr || pwd[0] == '\0' || std::strcmp(pwd, "admin") == 0;
}

// Premier démarrage = assistant non terminé OU mot de passe encore celui d'usine.
// Le second terme est la migration auto-réparatrice : un appareil dont un tiers a
// refermé la fenêtre (wizardCompleted vrai, mot de passe admin) rouvre l'assistant.
inline bool computeFirstBoot(bool wizardCompleted, bool factoryPassword) {
  return !wizardCompleted || factoryPassword;
}

// ----------------------------------------------------------------------------
// /auth/login
// ----------------------------------------------------------------------------
enum class LoginVerdict { kAllow, kDenyCredentials, kDenyMustChangePassword };

// Identifiants faux -> 401 ; mot de passe d'usine -> 403. ⛔ ET RIEN D'AUTRE.
//
// ⚠️ `firstBoot` N'EST PAS un paramètre, DÉLIBÉRÉMENT. Le tester ici enfermerait
// dehors le propriétaire d'un appareil dont l'assistant a été INTERROMPU après
// qu'il a choisi son mot de passe (wizardCompleted faux + mot de passe réel) :
// il connaît son mot de passe, et l'appareil le refuserait. ⛔ Or l'OTA est le
// SEUL canal de déploiement — pas de port série : un tel appareil serait perdu.
// ⇒ Ce qui refuse la session, c'est « le mot de passe est encore celui d'usine »,
// pas « l'assistant n'est pas fini ». C'est aussi ce qui ferme le fait 5 (un
// tiers refermait la fenêtre et admin/admin valait pour toujours).
inline LoginVerdict decideLogin(bool userMatches, bool passwordMatches, bool factoryPassword) {
  if (!userMatches || !passwordMatches) return LoginVerdict::kDenyCredentials;
  if (factoryPassword) return LoginVerdict::kDenyMustChangePassword;
  return LoginVerdict::kAllow;
}

// ----------------------------------------------------------------------------
// /auth/change-password
// ----------------------------------------------------------------------------
enum class PwdChangeVerdict { kAllow, kDenyUnauth, kDenyBadCurrent, kDenyTooShort, kDenySame };

struct PwdChangeInput {
  // ⛔ « VIERGE » = le mot de passe est encore celui d'usine, donc AUCUN mot de
  // passe n'a jamais été choisi. ⚠️ CE N'EST PAS `firstBoot` : un assistant
  // interrompu après le choix du mot de passe laisse firstBoot vrai alors que
  // l'appareil est DÉJÀ possédé — y accorder le droit anonyme laisserait un
  // inconnu écraser le mot de passe du propriétaire.
  bool virginState = false;             // état vierge (seul cas de droit anonyme)
  bool hasCurrentPassword = false;      // currentPassword fourni et non vide
  bool currentMatches = false;          // currentPassword == mot de passe en place
  bool currentIsFactoryLiteral = false; // currentPassword fourni vaut "admin" (état vierge)
  bool tokenValid = false;              // X-Auth-Token valide
  bool newLenOk = false;                // nouveau mot de passe >= longueur minimale
  bool newEqualsCurrent = false;        // nouveau == currentPassword fourni
};

// ⛔ ORDRE IMPOSÉ : l'AUTHENTIFICATION se teste AVANT la longueur. L'inverse
// répondrait 400 au lieu de 401 à un anonyme et FUIRAIT l'existence d'un compte
// (et l'état de l'appareil) à qui n'a aucune identité.
inline PwdChangeVerdict decidePasswordChange(const PwdChangeInput& in) {
  if (in.virginState) {
    // État vierge : droit anonyme. Si un currentPassword est fourni il doit être
    // celui d'usine.
    if (in.hasCurrentPassword && !in.currentIsFactoryLiteral) return PwdChangeVerdict::kDenyBadCurrent;
  } else if (in.hasCurrentPassword) {
    if (!in.currentMatches) return PwdChangeVerdict::kDenyBadCurrent;
  } else {
    if (!in.tokenValid) return PwdChangeVerdict::kDenyUnauth;
  }
  if (!in.newLenOk) return PwdChangeVerdict::kDenyTooShort;
  if (in.hasCurrentPassword && in.newEqualsCurrent) return PwdChangeVerdict::kDenySame;
  return PwdChangeVerdict::kAllow;
}

// Effets d'un changement réussi. Le jeton tourne TOUJOURS (un jeton pris pendant
// la fenêtre ne survit pas au choix du mot de passe) et le NOUVEAU est rendu
// (les quatre clients stockent response.token ; la page de secours OTA échoue
// sans jeton).
struct AuthEffectPlan {
  bool rotateToken = false;
  bool closeFirstBootWindow = false;
  bool returnRotatedToken = false;
  bool revokeWebsockets = false;
};

inline AuthEffectPlan planAfterPasswordChange(bool firstBoot) {
  AuthEffectPlan p;
  p.rotateToken = true;
  p.closeFirstBootWindow = firstBoot;
  p.returnRotatedToken = true;
  p.revokeWebsockets = true;
  return p;
}

// ----------------------------------------------------------------------------
// /auth/complete-wizard — devenue un filet idempotent
// ----------------------------------------------------------------------------
enum class WizardCloseVerdict { kClose, kAlreadyClosed, kDenyUnauth, kDenyFactoryPassword };

// ORDRE : identité d'abord (401), puis mot de passe d'usine (409), puis idempotence.
inline WizardCloseVerdict decideWizardClose(bool firstBoot, bool tokenValid, bool factoryPassword) {
  if (!tokenValid) return WizardCloseVerdict::kDenyUnauth;
  if (factoryPassword) return WizardCloseVerdict::kDenyFactoryPassword;
  if (!firstBoot) return WizardCloseVerdict::kAlreadyClosed;
  return WizardCloseVerdict::kClose;
}

#endif  // AUTH_BOOTSTRAP_LOGIC_H
