#ifndef PH_STALE_MESSAGE_H
#define PH_STALE_MESSAGE_H

// =============================================================================
// Texte de l'alerte « correction à vérifier après étalonnage » (feature-170 (C32))
// — module PUR, header-only (ADR-0017, patron de auth_messages.h / bug-200 et de
// ota_refusal_messages.h / feature-151) : aucune dépendance Arduino, aucun
// String, testable en natif. Le tampon est FOURNI par l'appelant.
//
// ⚠️ CONTRAINTE DE CANAL, pas un style : MQTT publishAlert() tronque à ~46 octets
// utiles (kMaxPayloadLen = 128, enveloppe JSON ~62 o, queue de troncature 19 o).
// ⭐ Le texte nomme l'ÉTAT, ⛔ jamais le chemin (ADR-0139, arbitrage Q14) :
// markPhCalibrationStamped() est aussi appelée sur un EFFACEMENT d'étalonnage
// (ClearPhCal, sensors.cpp) — « refait » / « vient d'avoir lieu » y seraient
// FAUX. ⛔ Ces deux formules sont bannies de ce fichier et de data/app.js.
// La TÊTE (« Correction de mesure +1,20 à vérifier : ») fait 41 octets : le
// verdict ET la valeur doivent survivre à la troncature. ⛔ Ne pas rallonger la
// tête. Le firmware notifie un CHANGEMENT D'ÉTAT au moment où il se produit,
// il ne reproduit pas les quatre rendus datés de l'écran.
// ⛔ Le mot « sonde » ne met en cause la sonde dans aucun texte.
// =============================================================================

#include <math.h>
#include <stddef.h>
#include <stdio.h>

// La phrase d'ACTE est la seule partagée avec data/app.js : une sentinelle la
// compare octet pour octet entre les deux fichiers (feature-170, parité de texte).
// ⛔ Ne pas retirer ni déplacer les deux ancres.
// >>> PH-STALE-ACTE:DEBUT
#define PH_STALE_ACTE "Refaites un test manuel et accordez la mesure dessus, ou remettez la correction à zéro."
// <<< PH-STALE-ACTE:FIN

/**
 * @brief Écrit le texte de l'alerte dans `out` (toujours terminé par '\0' si cap > 0).
 * @param out    Tampon fourni par l'appelant.
 * @param cap    Taille du tampon en octets.
 * @param offset Correction de mesure en vigueur (pH), rendue « +1,20 » (2 décimales
 *               signées, virgule décimale).
 * @return Longueur voulue hors '\0' (patron snprintf) ; > cap-1 si tronqué.
 */
inline size_t phStaleCalibrationMessage(char* out, size_t cap, float offset) {
  char val[16];
  snprintf(val, sizeof(val), "%+.2f", (double)offset);
  for (char* p = val; *p; ++p) {
    if (*p == '.') *p = ',';
  }
  const int n = snprintf(out, cap,
      "Correction de mesure %s à vérifier : l'étalonnage vient de changer, "
      "après sa pose — il a déplacé la mesure sur laquelle elle avait été "
      "calculée. " PH_STALE_ACTE,
      val);
  return n < 0 ? 0 : (size_t)n;
}

#endif // PH_STALE_MESSAGE_H
