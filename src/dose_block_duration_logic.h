#ifndef DOSE_BLOCK_DURATION_LOGIC_H
#define DOSE_BLOCK_DURATION_LOGIC_H

#include <stdint.h>

// feature-138 — Durée (en secondes) depuis laquelle un code de refus de
// dosage est inchangé, par voie (pH/ORP). Module PUR (ADR-0017), header-only,
// testable nativement sans FreeRTOS ni Arduino.
//
// Signature volontairement SPÉCIFIQUE à ce besoin (pas de refactor des cinq
// autres occurrences du patron « depuis » du projet, hors périmètre de
// feature-138) — mais assez générale pour qu'une spec ultérieure puisse les y
// lever : `nowMs`/`sinceMs` en millis() bruts, `blocked` sépare « pas de
// refus » de « refus qui vient de commencer » (sinceMs == nowMs), `capS` est
// le clamp appliqué par l'appelant (kDoseBlockedForMaxS).
//
// `blocked` = false : il n'y a PAS de refus en cours. La fonction renvoie 0,
// mais ⚠️ CE N'EST PAS la sentinelle qui décide de publier `null` côté JSON —
// c'est au code de refus (`code != kDoseBlockedCodeNone`) de porter cette
// décision, jamais une comparaison sur la durée elle-même. Ne pas hériter du
// défaut de `_sensorFaultSinceMs` (pump_controller.h) où `0 = pas d'épisode`
// collisionne avec la milliseconde 0 après boot et après chaque repliement.
//
// Arithmétique NON SIGNÉE, exacte à travers le repliement de millis() (24,8 j,
// le projet a déjà été mordu par ce repliement) : `(uint32_t)(nowMs - sinceMs)`
// reste juste même si `nowMs` a repliement par rapport à `sinceMs`. AUCUNE
// comparaison `now < since`, aucune branche de rattrapage — ce sont elles qui
// ont produit les défauts passés.
//
// `capS` borne la valeur retournée (kDoseBlockedForMaxS = 86400, 24h) : c'est
// ce CLAMP qui fixe le budget d'octets de la sérialisation côté JSON (5
// chiffres décimaux), PAS le type C sous-jacent — à ne pas reperdre de vue au
// prochain recomptage du pire cas de trame.
inline uint32_t doseBlockElapsedS(uint32_t nowMs, uint32_t sinceMs, bool blocked,
                                   uint32_t capS) {
  if (!blocked) {
    return 0;
  }
  const uint32_t elapsedMs = (uint32_t)(nowMs - sinceMs);
  const uint32_t elapsedS = elapsedMs / 1000UL;
  return (elapsedS > capS) ? capS : elapsedS;
}

#endif  // DOSE_BLOCK_DURATION_LOGIC_H
