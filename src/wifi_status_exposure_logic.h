#ifndef WIFI_STATUS_EXPOSURE_LOGIC_H
#define WIFI_STATUS_EXPOSURE_LOGIC_H

// =============================================================================
// wifi_status_exposure_logic — qui voit le SSID dans GET /wifi/status
// (bug-181, livrable A2), PURE
// =============================================================================
// Module sans dépendance Arduino (testable en natif, ADR-0017), SANS allocation
// et sans journalisation (ADR-0082).
//
// RÈGLE : expose = !authEnabled || tokenValid || apBit.
//   `apBit` est DÉRIVÉ ICI du mode Wi-Fi BRUT (voir wifiModeHasAp) : le site
//   d'appel ne porte aucune expression booléenne de décision.
//
// FAIL-CLOSED, vérifié et non recopié : une `WifiSsidExposureInput{}` rend
// MASQUÉ (authEnabled = true, tokenValid = false, wifiMode = 0 = NULL, donc ni
// jeton ni bit AP). Un champ oublié chez un appelant ne peut pas ouvrir.
//
// ⛔ Ce prédicat n'est PAS wifiConfigAllowed() : cette route DIVULGUE, elle n'AGIT
// pas. Voir le commentaire du handler dans web_routes_config.cpp.
// =============================================================================

// Valeurs de wifi_mode_t (IDF 5), déclarées ici car le module ne peut pas inclure
// d'en-tête Arduino. Toute autre valeur (dont 0 = NULL, 4 = NAN) est INCONNUE
// et se replie sur « pas de bit AP ».
constexpr int kWifiModeApValue = 2;       // WIFI_MODE_AP
constexpr int kWifiModeApStaValue = 3;    // WIFI_MODE_APSTA

// Bit AP : vrai UNIQUEMENT pour AP (2) et AP+STA (3). Repli fermé sur le reste.
inline bool wifiModeHasAp(int wifiMode) {
  return wifiMode == kWifiModeApValue || wifiMode == kWifiModeApStaValue;
}

struct WifiSsidExposureInput {
  bool authEnabled = true;     // authentification activée (défaut : OUI, donc fermé)
  bool tokenValid = false;     // X-Auth-Token valide (prédicat silencieux)
  int wifiMode = 0;            // wifi_mode_t BRUT (0 = NULL : pas de bit AP)
  // ⚠️ `staConnected` est IGNORÉ par le verdict, DÉLIBÉRÉMENT, et ce n'est PAS du
  // code mort : sans lui, la mutation « réintroduire wifiConfigAllowed() »
  // (apBit || !staConnected) ne peut pas s'exprimer dans le module, donc aucun
  // test ne peut prouver qu'on ne l'a pas écrite. ⛔ NE PAS LE SUPPRIMER.
  bool staConnected = false;
};

// Vrai = SSID en clair ; faux = ssid "" (la clé reste présente).
inline bool shouldExposeWifiSsid(const WifiSsidExposureInput& in) {
  if (!in.authEnabled) return true;
  if (in.tokenValid) return true;
  return wifiModeHasAp(in.wifiMode);
}

#endif  // WIFI_STATUS_EXPOSURE_LOGIC_H
