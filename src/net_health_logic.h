#ifndef NET_HEALTH_LOGIC_H
#define NET_HEALTH_LOGIC_H

// =============================================================================
// net_health_logic — Décision du watchdog de CONNECTIVITÉ PURE (v2.9.2)
// =============================================================================
// Logique de décision du watchdog de connectivité (détection d'un gel de la pile
// réseau lwIP : web/mDNS/ping ICMP morts alors que le WiFi reste associé, heap
// sain, sans reboot WDT car le thread tcpip lwIP n'est pas surveillé). Extraite de
// main.cpp pour être testable en natif (sans ESP32, sans FreeRTOS, sans Arduino).
// Style ADR-0017, comme dosing_logic / schedule_logic / self_heal_logic.
//
// INVARIANT : ce module ne fait QUE décider à partir de valeurs déjà collectées.
// La collecte (sonde ICMP esp_ping vers la passerelle, millis, RTC RAM) et les
// effets (WiFi.reconnect, performFailSafeRestart, enterMaintenanceMode) restent
// dans la coquille impure main.cpp.
//
// CONTRAINTE : pas d'Arduino.h, pas de FreeRTOS, pas de <String>/<FS.h>. On utilise
// l'en-tête C <stdint.h> (et non <cstdint>) pour compiler en natif.
// =============================================================================

#include <stdint.h>

// Verdict de l'évaluation du watchdog de connectivité.
enum class NetHealthDecision : uint8_t {
  Continue = 0,     // rien à faire, réseau sain (ou horloge de stall gelée)
  SoftRecover,      // stall léger → tentative de reconnexion WiFi (une seule fois)
  Restart,          // stall sévère confirmé → redémarrage propre fail-safe
  EnterMaintenance  // trop de reboots dans la fenêtre → maintenance (anti-boucle)
};

// Entrées de la décision — toutes fournies par l'appelant (aucune API Arduino).
struct NetHealthInputs {
  bool     wifiAssociated;       // WiFi.isConnected() : lien WiFi associé à l'AP
  uint32_t msSinceLastNetOk;     // ms écoulées depuis le dernier signe de vie réseau
  uint32_t softStallMs;          // seuil stall léger → soft recover (kNetworkStallSoftMs)
  uint32_t hardStallMs;          // seuil stall sévère → reboot (kNetworkStallRebootMs)
  bool     softRecoverAttempted; // un soft recover a déjà été tenté sur ce stall
  uint32_t uptimeMs;             // uptime courant (millis)
  uint32_t minUptimeMs;          // uptime minimal avant tout restart (kAutoHealMinUptimeMs)
  uint8_t  rebootCountInWindow;  // reboots auto déjà effectués dans la fenêtre (partagé heap+réseau)
  uint8_t  maxReboots;           // plafond de reboots auto (kAutoHealMaxReboots)
};

// Décision pure du watchdog de connectivité. Ordre des règles (priorité
// décroissante) — EXACT :
//   1. !wifiAssociated → Continue (l'horloge de stall est gelée par la coquille :
//      impossible de juger la pile lwIP sans lien WiFi).
//   2. uptimeMs < minUptimeMs → Continue (on laisse le système finir son démarrage).
//   3. rebootCountInWindow >= maxReboots → EnterMaintenance (anti-boucle PRIORITAIRE :
//      on n'enchaîne plus de reboot une fois le plafond atteint).
//   4. msSinceLastNetOk >= hardStallMs → Restart (gel confirmé, redémarrage propre).
//   5. msSinceLastNetOk >= softStallMs ET !softRecoverAttempted → SoftRecover.
//   6. sinon → Continue.
NetHealthDecision evaluateNetHealth(const NetHealthInputs& in);

#endif // NET_HEALTH_LOGIC_H
