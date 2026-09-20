#include "net_health_logic.h"

// =============================================================================
// net_health_logic — implémentation PURE (v2.9.2)
// =============================================================================
// Aucune dépendance runtime : logique déterministe testable en natif.

// NOTE ORDRE DES RÈGLES (v2.9.2) : ici l'uptime minimal (règle 2) est testé AVANT
// l'anti-boucle (règle 3), alors que evaluateSelfHeal() teste l'anti-boucle avant
// l'uptime. Cette divergence est SANS IMPACT en production : dans checkSystemHealth(),
// le bloc self-heal (heap) s'exécute AVANT ce bloc réseau et, s'il atteint le plafond
// de reboots, pose maintenanceMode = true. Or le bloc réseau est gardé par
// `if (!maintenanceMode ...)` → il ne s'exécute pas dans ce cas. Ne PAS réaligner
// l'ordre : les 25 tests natifs valident ce comportement précis.
NetHealthDecision evaluateNetHealth(const NetHealthInputs& in) {
  // 1. Pas de lien WiFi : impossible de juger la pile lwIP. La coquille gèle
  //    l'horloge de stall (lastNetworkOkMs = millis()) tant que WiFi est absent.
  if (!in.wifiAssociated) {
    return NetHealthDecision::Continue;
  }

  // 2. Uptime minimal non atteint : on laisse le démarrage/DHCP/mDNS se stabiliser
  //    avant d'armer le watchdog de connectivité.
  if (in.uptimeMs < in.minUptimeMs) {
    return NetHealthDecision::Continue;
  }

  // 3. Anti-boucle PRIORITAIRE : plafond de reboots atteint dans la fenêtre →
  //    maintenance, on NE redémarre plus (budget partagé heap+réseau).
  if (in.rebootCountInWindow >= in.maxReboots) {
    return NetHealthDecision::EnterMaintenance;
  }

  // 4. Stall sévère confirmé : pile réseau figée trop longtemps → reboot propre.
  if (in.msSinceLastNetOk >= in.hardStallMs) {
    return NetHealthDecision::Restart;
  }

  // 5. Stall léger et soft recover pas encore tenté : reconnexion WiFi (une fois).
  if (in.msSinceLastNetOk >= in.softStallMs && !in.softRecoverAttempted) {
    return NetHealthDecision::SoftRecover;
  }

  // 6. Réseau sain (ou soft recover déjà tenté, en attente du seuil hard).
  return NetHealthDecision::Continue;
}

// =============================================================================
// Réarmement du watchdog de connectivité — implémentation PURE (bug-134)
// =============================================================================
NetRearmOutcome evaluateNetRearm(const NetRearmInputs& in) {
  const bool isExternalProof =
      (in.sign == NetLifeSign::IcmpEcho || in.sign == NetLifeSign::AppTraffic);

  if (isExternalProof) {
    // Preuve EXTERNE : confiance totale revalidée, réarme sans limite et remet
    // le crédit d'auto-réarmement à 0 (C2).
    return NetRearmOutcome{true, 0};
  }

  // Preuve AUTO-PRODUITE (IpAcquired, ProbeStarted) : passe TOUJOURS par le
  // crédit (C1), qu'elle qu'en soit la nature — c'est ce qui ferme la boucle B
  // (ProbeStarted réarmait jusqu'ici sans condition).
  if (in.selfRearmCount < in.maxSelfRearms) {
    return NetRearmOutcome{true, (uint8_t)(in.selfRearmCount + 1)};
  }

  // Crédit épuisé : pas de réarmement, l'horloge poursuit sa course vers le
  // reboot — c'est le comportement voulu quand la passerelle est réellement
  // injoignable malgré des tentatives de récupération répétées.
  return NetRearmOutcome{false, in.selfRearmCount};
}

bool netProbeSeemsDead(uint32_t msSinceLastEcho, uint32_t silenceMs) {
  return msSinceLastEcho >= silenceMs;
}
