#ifndef DISCOVERY_REPORT_LOGIC_H
#define DISCOVERY_REPORT_LOGIC_H

// =============================================================================
// discovery_report_logic — Synthèse PURE d'une rafale de découverte HA
// =============================================================================
// feature-109 : les 54 publications de `publishDiscovery()` émettaient 54 lignes
// INFO (~98 o chacune, ≈ 5,3 Ko), soit 65 % d'un journal persistant de 8 Ko
// (kMaxLogFileBytes, ADR-0033) et 54 des 100 entrées du tampon RAM. Une seule
// reconnexion MQTT évinçait donc la trace du redémarrage précédent.
// Ce module remplace ce flot par UNE ligne de synthèse, sans masquer d'échec :
//   - comptage à l'exécution (jamais une constante d'entités : le commentaire
//     « 52 publish » de mqtt_manager.cpp a démontré qu'une constante dérive) ;
//   - premier topic fautif mémorisé NOMMÉMENT (queue de chaîne conservée) ;
//   - distinction « rafale interrompue » / « échecs épars » par un prédicat
//     calculé, pas par un drapeau posé à la main.
// La coquille impure (millis(), safePublish(), systemLogger) vit dans
// mqtt_manager.cpp.
//
// CONTRAINTE FORTE : formatage par snprintf() dans un buffer fourni par
// l'appelant — JAMAIS de String Arduino. Le String du harnais natif est un stub
// qui ne stocke rien : un formateur à base de String compilerait, les tests
// passeraient au vert et ne prouveraient RIEN. Ici `bufLen` est réellement
// respecté et vérifiable.
//
// CONTRAINTE : pas d'Arduino.h, pas de FreeRTOS — <stdio.h>/<stddef.h>/<stdint.h>
// uniquement, fonctions inline (header-only, rien à ajouter au build natif ni à
// build_src_filter).
//
// bug-121 : la rafale n'est plus republiée à chaque reconnexion MQTT (voir
// discovery_fingerprint_logic.h) — seulement sur changement d'empreinte, 1re
// connexion, rafale précédente incomplète, ou redémarrage HA détecté. Cette
// synthèse porte donc désormais le MOTIF (`DiscoveryTally.reason`) et le COÛT
// RÉEL mesuré (octets offerts, plancher heap 8 bits avant/après la boucle de
// publication) — pas seulement sa durée : une rafale plus rare doit être plus
// informative, pour ne jamais reproduire l'hypothèse fausse de bug-121 (« la
// rafale épuise les tampons TX ») sans preuve chiffrée à l'appui.
// =============================================================================

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "constants.h"  // kDiscoveryFailTopicMax (self-contained : stddef/stdint/time.h)
#include "discovery_fingerprint_logic.h"  // bug-121 : DiscoveryReason + discoveryReasonLabel (self-contained : stdint.h)

/**
 * @brief Compteurs d'une rafale de découverte Home Assistant.
 *
 * Vit sur la pile de mqttTask (≈ 64 o) — aucune allocation tas. Les compteurs
 * sont alimentés à l'exécution par discoveryTallyRecord(), un appel par entité.
 */
struct DiscoveryTally {
  uint16_t attempted;       // Entités effectivement tentées (compté, jamais supposé)
  uint16_t published;       // Publications acceptées par safePublish()
  uint16_t failed;          // Publications refusées (lien tombé, buffer plein…)
  uint16_t firstFailIndex;  // Rang 1-based du 1er échec ; 0 = aucun échec
  uint32_t burstSeq;        // Numéro de rafale (distingue deux reconnexions rapprochées)
  uint32_t durationMs;      // Durée totale de la rafale (ms)
  char     firstFailTopic[kDiscoveryFailTopicMax];  // Topic du 1er échec (queue conservée)
  // bug-121 (D2) : coût RÉEL d'une rafale — pas seulement sa durée. Accumulé
  // par l'appelant dans la lambda publishConfig(), JAMAIS déclaré/estimé.
  uint32_t bytesOffered;    // Σ(len(topic) + len(payload) + 2) sur TOUTES les tentatives
  uint32_t maxBloc8Before;  // heapLargestFreeBlock8Bit() juste AVANT la boucle de publication
  uint32_t maxBloc8After;   // heapLargestFreeBlock8Bit() juste APRÈS la boucle de publication
  // bug-121 (A4) : motif de la rafale — None ne doit jamais atteindre le
  // formateur (l'appelant filtre en amont, silence = cas nominal).
  DiscoveryReason reason;
};

/**
 * @brief Remet une synthèse à zéro pour une nouvelle rafale.
 * @param t Synthèse à initialiser.
 * @param burstSeq Numéro de rafale (monotone côté appelant).
 */
inline void discoveryTallyInit(DiscoveryTally& t, uint32_t burstSeq) {
  t.attempted = 0;
  t.published = 0;
  t.failed = 0;
  t.firstFailIndex = 0;
  t.burstSeq = burstSeq;
  t.durationMs = 0;
  t.firstFailTopic[0] = '\0';
  t.bytesOffered = 0;
  t.maxBloc8Before = 0;
  t.maxBloc8After = 0;
  t.reason = DiscoveryReason::None;
}

/**
 * @brief Copie un topic dans `dst` en conservant sa QUEUE si troncature.
 *
 * Le préfixe `homeassistant/<classe>/` est du remplissage identique pour toutes
 * les entités ; le discriminant (`..._orp_sensor_problem/config`) est en fin de
 * chaîne. Tronquer par la tête préserve donc l'information utile au diagnostic.
 *
 * @param dst Buffer de destination (toujours terminé par NUL si dstLen > 0).
 * @param dstLen Taille du buffer, NUL compris.
 * @param topic Topic source (nullptr toléré → chaîne vide).
 */
inline void discoveryCopyTopicTail(char* dst, size_t dstLen, const char* topic) {
  if (dst == nullptr || dstLen == 0) return;
  if (topic == nullptr) {
    dst[0] = '\0';
    return;
  }
  size_t len = 0;
  while (topic[len] != '\0') len++;
  const size_t keep = dstLen - 1;
  const size_t n = (len > keep) ? keep : len;
  const char* src = topic + (len - n);  // queue de la chaîne
  for (size_t i = 0; i < n; i++) dst[i] = src[i];
  dst[n] = '\0';
}

/**
 * @brief Enregistre le résultat d'UNE publication d'entité.
 *
 * Aucune allocation, aucun log : c'est le seul travail fait dans la boucle des
 * 54 publications. Seul le PREMIER topic fautif est mémorisé — le pire cas doit
 * rester une ligne de journal, pas 42.
 *
 * @param t Synthèse en cours.
 * @param topic Topic de configuration publié.
 * @param ok Résultat de safePublish().
 */
inline void discoveryTallyRecord(DiscoveryTally& t, const char* topic, bool ok) {
  t.attempted++;
  if (ok) {
    t.published++;
    return;
  }
  t.failed++;
  if (t.firstFailIndex == 0) {
    t.firstFailIndex = t.attempted;
    discoveryCopyTopicTail(t.firstFailTopic, sizeof(t.firstFailTopic), topic);
  }
}

/**
 * @brief La rafale a-t-elle été interrompue (lien tombé) plutôt qu'entachée
 *        d'échecs épars ?
 *
 * Prédicat CALCULÉ, pas un drapeau : `publishDiscovery()` ne s'arrête jamais
 * (aucun break, aucun retour anticipé), donc un lien tombé à la Ne entité
 * produit un échec pour TOUTES les suivantes. La signature d'une interruption
 * est donc « tous les échecs sont contigus jusqu'à la fin » :
 *   failed == attempted - firstFailIndex + 1
 *
 * @return true si la queue de la rafale est intégralement en échec.
 */
inline bool discoveryTallyInterrupted(const DiscoveryTally& t) {
  if (t.failed == 0 || t.firstFailIndex == 0) return false;
  return t.failed == (uint16_t)(t.attempted - t.firstFailIndex + 1);
}

/**
 * @brief Suffixe ordinal français : 1re, 2e, 13e…
 */
inline const char* discoveryOrdinalSuffix(uint16_t n) {
  return (n == 1) ? "re" : "e";
}

/**
 * @brief Formate la ligne de synthèse d'une rafale terminée.
 *
 * Formes émises (une seule ligne, quel que soit le nombre d'échecs) :
 *   Découverte HA #3 (1re connexion) : 54/54 entités, 0 échec, 9214 o, maxBloc8 21492→21236, 1240 ms
 *   Découverte HA #4 (empreinte changée) : 12/54 entités, 42 échecs, interrompue à la 13e (…), 2048 o, maxBloc8 21000→20800, 310 ms
 *   Découverte HA #7 (rafale précédente incomplète) : 52/54 entités, 2 échecs épars, 1er à la 9e (…), 8900 o, maxBloc8 21400→21100, 1180 ms
 * (exemples repris tels quels de test_discovery_report.cpp — fixtures de test
 * figées à 54 entités avant la migration à 60, PAS un relevé réel ; voir la
 * même note dans docs/subsystems/mqtt-manager.md.)
 *
 * bug-121 (A4/D2) : le motif (`t.reason`) et le coût mesuré (octets offerts,
 * plancher heap 8 bits avant/après, ADR-0038 — jamais la mémoire libre)
 * s'ajoutent à la synthèse feature-109. `t.reason` ne doit JAMAIS valoir
 * `DiscoveryReason::None` ici : l'appelant ne déclenche une rafale (et donc cet
 * appel) que pour un motif réel — le silence du cas nominal est géré en amont.
 *
 * @param buf Buffer de destination (kDiscoverySummaryLineMax recommandé).
 * @param bufLen Taille du buffer, NUL compris.
 * @return Nombre de caractères qu'il aurait fallu écrire (convention snprintf) :
 *         ≥ bufLen signale une troncature.
 */
inline int discoveryFormatSummary(char* buf, size_t bufLen, const DiscoveryTally& t) {
  if (buf == nullptr || bufLen == 0) return 0;
  const unsigned seq = (unsigned)t.burstSeq;
  const unsigned pub = (unsigned)t.published;
  const unsigned att = (unsigned)t.attempted;
  const unsigned fail = (unsigned)t.failed;
  const unsigned dur = (unsigned)t.durationMs;
  const unsigned bytesOffered = (unsigned)t.bytesOffered;
  const unsigned before = (unsigned)t.maxBloc8Before;
  const unsigned after = (unsigned)t.maxBloc8After;
  const char* reasonLabel = discoveryReasonLabel(t.reason);

  if (fail == 0) {
    return snprintf(buf, bufLen,
                    "Découverte HA #%u (%s) : %u/%u entités, 0 échec, %u o, maxBloc8 %u\xE2\x86\x92%u, %u ms",
                    seq, reasonLabel, pub, att, bytesOffered, before, after, dur);
  }
  const char* plural = (fail > 1) ? "s" : "";
  const unsigned idx = (unsigned)t.firstFailIndex;
  if (discoveryTallyInterrupted(t)) {
    return snprintf(buf, bufLen,
                    "Découverte HA #%u (%s) : %u/%u entités, %u échec%s, interrompue à la %u%s (%s), "
                    "%u o, maxBloc8 %u\xE2\x86\x92%u, %u ms",
                    seq, reasonLabel, pub, att, fail, plural, idx, discoveryOrdinalSuffix(t.firstFailIndex),
                    t.firstFailTopic, bytesOffered, before, after, dur);
  }
  return snprintf(buf, bufLen,
                  "Découverte HA #%u (%s) : %u/%u entités, %u échec%s épars, 1er à la %u%s (%s), "
                  "%u o, maxBloc8 %u\xE2\x86\x92%u, %u ms",
                  seq, reasonLabel, pub, att, fail, plural, idx, discoveryOrdinalSuffix(t.firstFailIndex),
                  t.firstFailTopic, bytesOffered, before, after, dur);
}

/**
 * @brief Formate la ligne d'abandon (rafale non tentée : lien MQTT absent).
 *
 * Cas limite exigé par feature-109 : « aucune entité publiée » doit produire une
 * ligne, jamais le silence.
 *
 * @param buf Buffer de destination.
 * @param bufLen Taille du buffer, NUL compris.
 * @param burstSeq Numéro de rafale consommé par la tentative.
 * @return Convention snprintf (≥ bufLen = troncature).
 */
inline int discoveryFormatAbort(char* buf, size_t bufLen, uint32_t burstSeq) {
  if (buf == nullptr || bufLen == 0) return 0;
  return snprintf(buf, bufLen, "Découverte HA #%u : abandonnée avant publication (lien MQTT absent)",
                  (unsigned)burstSeq);
}

#endif  // DISCOVERY_REPORT_LOGIC_H
