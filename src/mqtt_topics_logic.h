#ifndef MQTT_TOPICS_LOGIC_H
#define MQTT_TOPICS_LOGIC_H

// =============================================================================
// mqtt_topics_logic — Normalisation PURE du préfixe de topics MQTT
// =============================================================================
// Logique de normalisation du préfixe (`mqttCfg.topic`) extraite de
// MqttManager::refreshTopics() pour être testable en natif (sans ESP32, sans
// FreeRTOS, sans Arduino). Style ADR-0017, comme net_health_logic / dosing_logic.
//
// INVARIANT : ce module ne fait QUE transformer une chaîne déjà collectée.
// La lecture de la config (snapshot sous configMutex) et la reconstruction des
// topics (sous topicsMutex) restent dans la coquille impure mqtt_manager.cpp.
//
// CONTRAINTE : pas d'Arduino.h, pas de String (le shim natif n'en fournit qu'un
// stub no-op) — API sur buffers C, en-têtes C uniquement (<stddef.h>).
// =============================================================================

#include <stddef.h>

// Préfixe de repli si la base normalisée est vide (défaut historique du projet).
constexpr char kMqttDefaultBase[] = "pool/sensors";

// Normalise un préfixe de topics MQTT dans `out` (toujours NUL-terminé) :
//   1. trim des espaces/blancs (espace, \t, \n, \v, \f, \r) en tête et en queue
//      — mêmes caractères que String::trim() Arduino ;
//   2. suppression des '/' finaux, y compris multiples ("pool///" → "pool") ;
//   3. chaîne vide ou réduite à rien après 1-2 → fallback kMqttDefaultBase.
// Comportement identique à l'ancien code inline de refreshTopics() : pas de
// re-trim après la suppression des '/' ("pool /" → "pool " espace conservé).
// Tronque silencieusement si le résultat dépasse outLen-1. `raw` peut être NULL
// (traité comme vide). No-op si out == NULL ou outLen == 0.
void normalizeMqttBase(const char* raw, char* out, size_t outLen);

#endif  // MQTT_TOPICS_LOGIC_H
