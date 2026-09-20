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

// =============================================================================
// Réarmement du watchdog de connectivité — logique PURE (bug-134, v2.53.1)
// =============================================================================
// L'incident du 2026-09-19 a montré que réarmer l'horloge de stall sur TOUTE
// preuve de vie ouvre deux boucles infinies : une IP DHCP obtenue déclenche un
// soft recover qui obtient une IP qui réarme... (boucle A), et une sonde ICMP
// recréée sur silence réarme, retombe en silence 90 s plus tard, se recrée...
// (boucle B). Dans les deux cas la preuve de vie est FABRIQUÉE PAR LA
// RÉCUPÉRATION ELLE-MÊME, pas par un tiers. Règle retenue : un signe de vie que
// la récupération fabrique elle-même ne peut pas financer indéfiniment son
// propre report.
enum class NetLifeSign : uint8_t {
  IcmpEcho,    // écho ICMP passerelle reçu — preuve EXTERNE
  AppTraffic,  // trafic applicatif servi (HTTP/WS/MQTT/NTP) — preuve EXTERNE
  IpAcquired,  // IP obtenue par DHCP (GOT_IP) — preuve AUTO-PRODUITE
  ProbeStarted // sonde ICMP (re)démarrée — preuve AUTO-PRODUITE
};

// Entrées de la décision de réarmement.
struct NetRearmInputs {
  NetLifeSign sign;            // nature de la preuve de vie reçue
  uint8_t     selfRearmCount;  // crédit déjà consommé par des preuves auto-produites
  uint8_t     maxSelfRearms;   // plafond du crédit (kNetworkSelfRearmMax)
};

// Sortie de la décision de réarmement.
struct NetRearmOutcome {
  bool    rearmClock;        // true = la coquille doit réarmer lastNetworkOkMs
  uint8_t newSelfRearmCount; // nouvelle valeur du crédit à mémoriser
};

// Décide si une preuve de vie réarme l'horloge de stall, et met à jour le
// crédit de réarmements auto-produits. Règle EXACTE (conditions C1/C2 du gate
// pool-chemistry, bloquantes) :
//   - preuve EXTERNE (IcmpEcho, AppTraffic) : réarme TOUJOURS ET remet le
//     crédit à 0 — seule une preuve externe revalide toute la confiance
//     (C2 : le crédit n'est JAMAIS remis à zéro par IpAcquired ni ProbeStarted) ;
//   - preuve AUTO-PRODUITE (IpAcquired, ProbeStarted) : réarme seulement si
//     `selfRearmCount < maxSelfRearms`, puis incrémente le crédit consommé
//     (C1 : ProbeStarted passe TOUJOURS par ce crédit, jamais de réarmement
//     inconditionnel — c'est ce qui ferme la boucle B). Crédit épuisé → aucun
//     réarmement : l'horloge court jusqu'au reboot, qui reste alors inévitable.
NetRearmOutcome evaluateNetRearm(const NetRearmInputs& in);

// Sonde ICMP jugée morte si aucun écho reçu depuis `silenceMs` (typiquement
// kNetworkProbeSilenceMs, trois échos manqués à kNetworkProbeIntervalMs).
// Logique pure : compare deux durées, ne lit ni ne modifie l'état de la sonde.
bool netProbeSeemsDead(uint32_t msSinceLastEcho, uint32_t silenceMs);

#endif // NET_HEALTH_LOGIC_H
