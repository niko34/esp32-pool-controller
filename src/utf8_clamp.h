#ifndef UTF8_CLAMP_H
#define UTF8_CLAMP_H

// =============================================================================
// utf8_clamp — Troncature PURE au dernier caractère UTF-8 COMPLET (ADR-0017)
// =============================================================================
// feature-129 (phase A) : extraction de clampReasonUtf8() (pump_controller.cpp),
// la meilleure des deux implémentations qui coexistaient dans le firmware — elle
// valide l'octet de tête ET la longueur attendue de la séquence, là où la boucle
// de recul de publishAlert() (mqtt_manager.cpp:766) se contente de reculer sur
// les octets de continuation sans vérifier que l'octet de tête est valide.
//
// Motif : les messages sont en français (accents = séquences UTF-8 multi-octets).
// Couper à l'octet laisserait une séquence incomplète, donc un JSON NON
// décodable côté client — symptôme déjà rencontré sur les charges MQTT
// (mqtt_manager.cpp, publishAlert, refus v2.25.0).
//
// UN SEUL module pour ce besoin dans tout le firmware (consigne pool-chemistry,
// feature-129 §C1-C4) : ne pas en créer une troisième implémentation.
// ⚠️ mqtt_manager.cpp:766 N'EST PAS migré vers ce module dans ce lot — sa boucle
// est imbriquée dans une re-sérialisation JSON validée par pool-chemistry en
// v2.25.0, sur le chemin d'alerte de sécurité. Dette délibérée, à traiter dans
// un lot suivant (voir ADR feature-129).
//
// CONTRAINTE : pas d'Arduino.h, pas de FreeRTOS, pas de String — <stddef.h> et
// <string.h> (strlen, fonction standard C) uniquement.
// =============================================================================

#include <stddef.h>

/**
 * @brief Longueur ≤ `len` à laquelle couper `s` sans scinder un caractère
 *        UTF-8 multi-octets.
 *
 * Ne suppose PAS `s` NUL-terminé au-delà de `len` : opère strictement sur les
 * `len` premiers octets fournis (utilisable directement sur un message qui vit
 * dans une arène circulaire, sans terminaison).
 *
 * Algorithme : reculer depuis `len - 1` jusqu'au premier octet du DERNIER
 * caractère (les octets de continuation valent `10xxxxxx`), puis vérifier que
 * ce caractère est ENTIER dans les `len` octets fournis. Si l'octet de tête est
 * invalide (lecture déchirée) OU si la séquence dépasserait `len`, le dernier
 * caractère est retiré ; sinon `len` est retourné inchangé.
 *
 * @param s   octets sources (peut être un fragment d'arène, pas nécessairement
 *            NUL-terminé au-delà de `len`).
 * @param len nombre d'octets à considérer (borne haute du résultat).
 * @return    longueur ≤ `len`, se terminant toujours sur un caractère COMPLET.
 */
size_t utf8ClampLen(const char* s, size_t len);

/**
 * @brief Tronque `dst` (déjà écrit, NUL-terminé) au dernier caractère UTF-8
 *        COMPLET. Ne fait rien si `dst` se termine déjà sur un caractère
 *        complet. Ex-`clampReasonUtf8()` (pump_controller.cpp), réécrite sur
 *        `utf8ClampLen()`.
 *
 * @param dst chaîne NUL-terminée, modifiée en place.
 */
void utf8ClampInPlace(char* dst);

#endif  // UTF8_CLAMP_H
