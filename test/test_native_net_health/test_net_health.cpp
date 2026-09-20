// =============================================================================
// Tests unitaires natifs — Watchdog de connectivité (net_health_logic, v2.9.2)
// =============================================================================
// Tournent sur PC (env:native, Unity), HORS ESP32 / FreeRTOS / Arduino.
// On teste le COMPORTEMENT observable de la décision PURE extraite dans
// src/net_health_logic.{h,cpp} (ADR-0017, comme self_heal_logic / dosing_logic
// / schedule_logic / sensor_filter) :
//   - evaluateNetHealth(NetHealthInputs) →
//       { Continue, SoftRecover, Restart, EnterMaintenance }
//
// Ordre des règles (priorité décroissante), verrouillé par ces tests :
//   1. !wifiAssociated → Continue (horloge de stall gelée, rien à juger).
//   2. uptimeMs < minUptimeMs → Continue (démarrage/DHCP/mDNS en cours).
//   3. rebootCountInWindow >= maxReboots → EnterMaintenance (anti-boucle,
//      PRIORITÉ HAUTE : l'emporte même si un Restart serait sinon dû).
//   4. msSinceLastNetOk >= hardStallMs → Restart (gel confirmé).
//   5. msSinceLastNetOk >= softStallMs ET !softRecoverAttempted → SoftRecover.
//   6. sinon → Continue.
//
// FRONTIÈRES figées par le code (>= vs <) :
//   - stall soft atteint ssi msSinceLastNetOk >= softStallMs (== → atteint).
//   - stall hard atteint ssi msSinceLastNetOk >= hardStallMs (== → atteint).
//   - uptime atteint ssi uptimeMs >= minUptimeMs (== → atteint, watchdog armé).
//   - maintenance ssi rebootCountInWindow >= maxReboots (== → maintenance).
// =============================================================================

#include <unity.h>
#include <stdint.h>
#include "net_health_logic.h"

void setUp(void) {}
void tearDown(void) {}

// Valeurs de référence (répliquées ici, module pur sans include Arduino).
// Toute divergence firmware sera détectée par la revue.
static const uint32_t kSoftStallMs = 60000UL;   // kNetworkStallSoftMs (indicatif)
static const uint32_t kHardStallMs = 180000UL;  // kNetworkStallRebootMs (indicatif)
static const uint32_t kMinUptimeMs = 120000UL;  // kAutoHealMinUptimeMs (indicatif)
static const uint8_t  kMaxReboots  = 3;          // kAutoHealMaxReboots (indicatif)

// Helper : entrées « saines » — WiFi associé, réseau vivant (stall nul), uptime
// largement écoulé, aucun reboot, aucun soft recover tenté. Chaque test part de
// cette base et ne perturbe que le(s) champ(s) étudié(s).
static NetHealthInputs healthyInputs(void) {
  NetHealthInputs in;
  in.wifiAssociated      = true;
  in.msSinceLastNetOk    = 0;                 // signe de vie tout récent
  in.softStallMs         = kSoftStallMs;
  in.hardStallMs         = kHardStallMs;
  in.softRecoverAttempted = false;
  in.uptimeMs            = kMinUptimeMs * 10;  // démarré depuis longtemps
  in.minUptimeMs         = kMinUptimeMs;
  in.rebootCountInWindow = 0;
  in.maxReboots          = kMaxReboots;
  return in;
}

// Helper : entrées où un Restart serait dû (WiFi associé, uptime écoulé, stall
// au-delà du seuil hard, sous le plafond de reboots).
static NetHealthInputs restartReadyInputs(void) {
  NetHealthInputs in = healthyInputs();
  in.msSinceLastNetOk = kHardStallMs;  // gel confirmé
  return in;
}

// =============================================================================
// 1. !wifiAssociated → Continue (même si stall énorme)
// =============================================================================
void test_continue_when_wifi_not_associated(void) {
  NetHealthInputs in = healthyInputs();
  in.wifiAssociated   = false;
  in.msSinceLastNetOk = kHardStallMs * 100;  // stall gigantesque : ignoré
  TEST_ASSERT_EQUAL(NetHealthDecision::Continue, evaluateNetHealth(in));
}

// WiFi absent l'emporte même quand le plafond de reboots est atteint : la
// règle 1 court-circuite tout (l'horloge de stall est gelée par la coquille).
void test_continue_wifi_absent_even_with_reboots_maxed(void) {
  NetHealthInputs in = healthyInputs();
  in.wifiAssociated      = false;
  in.msSinceLastNetOk    = kHardStallMs * 10;
  in.rebootCountInWindow = kMaxReboots;
  TEST_ASSERT_EQUAL(NetHealthDecision::Continue, evaluateNetHealth(in));
}

// =============================================================================
// 2. uptimeMs < minUptimeMs → Continue (même si stall > hardStall)
// =============================================================================
void test_continue_when_uptime_below_min(void) {
  NetHealthInputs in = restartReadyInputs();  // stall au-delà du hard
  in.uptimeMs = kMinUptimeMs - 1;             // watchdog pas encore armé
  TEST_ASSERT_EQUAL(NetHealthDecision::Continue, evaluateNetHealth(in));
}

// Uptime insuffisant l'emporte sur un stall gigantesque.
void test_continue_uptime_below_min_even_huge_stall(void) {
  NetHealthInputs in = healthyInputs();
  in.uptimeMs         = kMinUptimeMs - 1;
  in.msSinceLastNetOk = kHardStallMs * 100;
  TEST_ASSERT_EQUAL(NetHealthDecision::Continue, evaluateNetHealth(in));
}

// =============================================================================
// 3. EnterMaintenance PRIORITAIRE : plafond de reboots atteint l'emporte même
//    quand un Restart serait sinon dû (anti-boucle).
// =============================================================================
void test_maintenance_priority_over_restart(void) {
  NetHealthInputs in = restartReadyInputs();  // Restart aurait été renvoyé…
  in.rebootCountInWindow = kMaxReboots;        // …mais plafond atteint
  TEST_ASSERT_EQUAL(NetHealthDecision::EnterMaintenance, evaluateNetHealth(in));
}

// Maintenance même sur un réseau parfaitement sain dès que le plafond est
// atteint : la règle 3 court-circuite Restart/SoftRecover.
void test_maintenance_even_when_network_healthy(void) {
  NetHealthInputs in = healthyInputs();
  in.rebootCountInWindow = kMaxReboots;
  TEST_ASSERT_EQUAL(NetHealthDecision::EnterMaintenance, evaluateNetHealth(in));
}

// Reboots au-delà du plafond → maintenance (>= et non ==).
void test_maintenance_when_reboots_exceed_max(void) {
  NetHealthInputs in = restartReadyInputs();
  in.rebootCountInWindow = kMaxReboots + 5;
  TEST_ASSERT_EQUAL(NetHealthDecision::EnterMaintenance, evaluateNetHealth(in));
}

// =============================================================================
// 4. msSinceLastNetOk >= hardStallMs → Restart (WiFi associé, uptime OK, sous
//    le plafond de reboots)
// =============================================================================
void test_restart_when_hard_stall(void) {
  NetHealthInputs in = restartReadyInputs();
  TEST_ASSERT_EQUAL(NetHealthDecision::Restart, evaluateNetHealth(in));
}

// Stall bien au-delà du hard → toujours Restart (sous le plafond).
void test_restart_when_stall_far_beyond_hard(void) {
  NetHealthInputs in = restartReadyInputs();
  in.msSinceLastNetOk = kHardStallMs * 3;
  TEST_ASSERT_EQUAL(NetHealthDecision::Restart, evaluateNetHealth(in));
}

// Restart l'emporte sur SoftRecover même si le soft n'a jamais été tenté :
// une fois le seuil hard franchi, on redémarre (règle 4 avant règle 5).
void test_restart_over_soft_when_soft_not_attempted(void) {
  NetHealthInputs in = restartReadyInputs();
  in.softRecoverAttempted = false;
  TEST_ASSERT_EQUAL(NetHealthDecision::Restart, evaluateNetHealth(in));
}

// Juste sous le plafond de reboots + hard stall → Restart (la règle 3 ne
// s'active pas encore).
void test_restart_reboots_one_below_max(void) {
  NetHealthInputs in = restartReadyInputs();
  in.rebootCountInWindow = (uint8_t)(kMaxReboots - 1);
  TEST_ASSERT_EQUAL(NetHealthDecision::Restart, evaluateNetHealth(in));
}

// =============================================================================
// 5. msSinceLastNetOk >= softStallMs ET !softRecoverAttempted → SoftRecover ;
//    si softRecoverAttempted → Continue (pas de 2e soft, on attend le hard).
// =============================================================================
void test_soft_recover_when_soft_stall_and_not_attempted(void) {
  NetHealthInputs in = healthyInputs();
  in.msSinceLastNetOk     = kSoftStallMs;  // stall léger atteint
  in.softRecoverAttempted = false;
  TEST_ASSERT_EQUAL(NetHealthDecision::SoftRecover, evaluateNetHealth(in));
}

// Stall entre soft et hard, soft déjà tenté → Continue (attente du seuil hard).
void test_continue_when_soft_already_attempted(void) {
  NetHealthInputs in = healthyInputs();
  in.msSinceLastNetOk     = kSoftStallMs;  // au-dessus du soft, sous le hard
  in.softRecoverAttempted = true;
  TEST_ASSERT_EQUAL(NetHealthDecision::Continue, evaluateNetHealth(in));
}

// Stall à mi-chemin (bien au-dessus du soft, sous le hard) sans soft tenté →
// SoftRecover.
void test_soft_recover_mid_range(void) {
  NetHealthInputs in = healthyInputs();
  in.msSinceLastNetOk     = (kSoftStallMs + kHardStallMs) / 2;
  in.softRecoverAttempted = false;
  TEST_ASSERT_EQUAL(NetHealthDecision::SoftRecover, evaluateNetHealth(in));
}

// Même stall à mi-chemin mais soft déjà tenté → Continue.
void test_continue_mid_range_soft_attempted(void) {
  NetHealthInputs in = healthyInputs();
  in.msSinceLastNetOk     = (kSoftStallMs + kHardStallMs) / 2;
  in.softRecoverAttempted = true;
  TEST_ASSERT_EQUAL(NetHealthDecision::Continue, evaluateNetHealth(in));
}

// =============================================================================
// 6. Réseau sain (msSinceLastNetOk < softStallMs) → Continue
// =============================================================================
void test_continue_when_network_healthy(void) {
  NetHealthInputs in = healthyInputs();  // stall = 0
  TEST_ASSERT_EQUAL(NetHealthDecision::Continue, evaluateNetHealth(in));
}

// Stall non nul mais sous le seuil soft → Continue (même si soft jamais tenté).
void test_continue_when_stall_below_soft(void) {
  NetHealthInputs in = healthyInputs();
  in.msSinceLastNetOk     = kSoftStallMs - 1;
  in.softRecoverAttempted = false;
  TEST_ASSERT_EQUAL(NetHealthDecision::Continue, evaluateNetHealth(in));
}

// =============================================================================
// 7. Frontières exactes aux seuils (== )
// =============================================================================

// Soft : msSinceLastNetOk == softStallMs → atteint (>=) → SoftRecover.
void test_boundary_soft_stall_equals_soft_recover(void) {
  NetHealthInputs in = healthyInputs();
  in.msSinceLastNetOk     = kSoftStallMs;
  in.softRecoverAttempted = false;
  TEST_ASSERT_EQUAL(NetHealthDecision::SoftRecover, evaluateNetHealth(in));
}

// Soft : msSinceLastNetOk == softStallMs - 1 → non atteint → Continue.
void test_boundary_soft_stall_one_below_continue(void) {
  NetHealthInputs in = healthyInputs();
  in.msSinceLastNetOk     = kSoftStallMs - 1;
  in.softRecoverAttempted = false;
  TEST_ASSERT_EQUAL(NetHealthDecision::Continue, evaluateNetHealth(in));
}

// Hard : msSinceLastNetOk == hardStallMs → atteint (>=) → Restart.
void test_boundary_hard_stall_equals_restart(void) {
  NetHealthInputs in = healthyInputs();
  in.msSinceLastNetOk = kHardStallMs;
  TEST_ASSERT_EQUAL(NetHealthDecision::Restart, evaluateNetHealth(in));
}

// Hard : msSinceLastNetOk == hardStallMs - 1 → sous le hard → SoftRecover
// (soft jamais tenté, stall bien au-dessus du soft).
void test_boundary_hard_stall_one_below_soft_recover(void) {
  NetHealthInputs in = healthyInputs();
  in.msSinceLastNetOk     = kHardStallMs - 1;
  in.softRecoverAttempted = false;
  TEST_ASSERT_EQUAL(NetHealthDecision::SoftRecover, evaluateNetHealth(in));
}

// Uptime : uptimeMs == minUptimeMs → watchdog armé (>=). Avec hard stall → Restart.
void test_boundary_uptime_equals_min_armed(void) {
  NetHealthInputs in = restartReadyInputs();
  in.uptimeMs = kMinUptimeMs;
  TEST_ASSERT_EQUAL(NetHealthDecision::Restart, evaluateNetHealth(in));
}

// Uptime : uptimeMs == minUptimeMs - 1 → non armé → Continue.
void test_boundary_uptime_one_below_min_continue(void) {
  NetHealthInputs in = restartReadyInputs();
  in.uptimeMs = kMinUptimeMs - 1;
  TEST_ASSERT_EQUAL(NetHealthDecision::Continue, evaluateNetHealth(in));
}

// Maintenance : rebootCountInWindow == maxReboots → atteint (>=) → maintenance.
void test_boundary_reboots_equals_max_maintenance(void) {
  NetHealthInputs in = restartReadyInputs();
  in.rebootCountInWindow = kMaxReboots;
  TEST_ASSERT_EQUAL(NetHealthDecision::EnterMaintenance, evaluateNetHealth(in));
}

// Maintenance : rebootCountInWindow == maxReboots - 1 → sous le plafond → pas
// de maintenance (ici hard stall → Restart).
void test_boundary_reboots_one_below_max_restart(void) {
  NetHealthInputs in = restartReadyInputs();
  in.rebootCountInWindow = (uint8_t)(kMaxReboots - 1);
  TEST_ASSERT_EQUAL(NetHealthDecision::Restart, evaluateNetHealth(in));
}

// =============================================================================
// 8. evaluateNetRearm() — crédit de réarmement auto-produit (bug-134)
// =============================================================================
// Constantes RÉELLES de src/constants.h (contrairement aux sections 1-7
// ci-dessus qui utilisent des valeurs indicatives) : les scénarios T3/T4 et la
// séquence chaînée reproduisent fidèlement les seuils de production. Toute
// divergence avec kNetworkStallSoftMs / kNetworkStallRebootMs /
// kNetworkSelfRearmMax / kNetworkProbeSilenceMs sera détectée en revue.
static const uint32_t kRealSoftStallMs    = 120000UL;  // kNetworkStallSoftMs
static const uint32_t kRealHardStallMs    = 300000UL;  // kNetworkStallRebootMs
static const uint8_t  kRealSelfRearmMax   = 2;          // kNetworkSelfRearmMax
static const uint32_t kRealProbeSilenceMs = 90000UL;   // kNetworkProbeSilenceMs

// ---- Rôle des 4 NetLifeSign, un par un (C1/C2) ----

// IcmpEcho : preuve EXTERNE — réarme toujours, remet le crédit à 0, même si le
// crédit était déjà consommé.
void test_role_icmp_echo_always_rearms_and_resets_credit(void) {
  NetRearmInputs in;
  in.sign = NetLifeSign::IcmpEcho;
  in.selfRearmCount = kRealSelfRearmMax;  // crédit déjà épuisé
  in.maxSelfRearms = kRealSelfRearmMax;
  NetRearmOutcome out = evaluateNetRearm(in);
  TEST_ASSERT_TRUE(out.rearmClock);
  TEST_ASSERT_EQUAL_UINT8(0, out.newSelfRearmCount);
}

// AppTraffic : même rôle qu'IcmpEcho — preuve EXTERNE.
void test_role_app_traffic_always_rearms_and_resets_credit(void) {
  NetRearmInputs in;
  in.sign = NetLifeSign::AppTraffic;
  in.selfRearmCount = kRealSelfRearmMax;
  in.maxSelfRearms = kRealSelfRearmMax;
  NetRearmOutcome out = evaluateNetRearm(in);
  TEST_ASSERT_TRUE(out.rearmClock);
  TEST_ASSERT_EQUAL_UINT8(0, out.newSelfRearmCount);
}

// Preuve externe : remet à 0 même si le crédit dépassait déjà le plafond
// (défense en profondeur contre un appelant qui n'aurait pas clampé en amont).
void test_role_external_proof_resets_credit_even_above_max(void) {
  NetRearmInputs in;
  in.sign = NetLifeSign::IcmpEcho;
  in.selfRearmCount = (uint8_t)(kRealSelfRearmMax + 5);
  in.maxSelfRearms = kRealSelfRearmMax;
  NetRearmOutcome out = evaluateNetRearm(in);
  TEST_ASSERT_TRUE(out.rearmClock);
  TEST_ASSERT_EQUAL_UINT8(0, out.newSelfRearmCount);
}

// IpAcquired : preuve AUTO-PRODUITE — sous le plafond, réarme et INCRÉMENTE le
// crédit (ne le remet JAMAIS à 0). Mutant tué : reclasser IpAcquired en externe
// (T2/M3).
void test_role_ip_acquired_gated_below_max(void) {
  NetRearmInputs in;
  in.sign = NetLifeSign::IpAcquired;
  in.selfRearmCount = 0;
  in.maxSelfRearms = kRealSelfRearmMax;
  NetRearmOutcome out = evaluateNetRearm(in);
  TEST_ASSERT_TRUE(out.rearmClock);
  TEST_ASSERT_EQUAL_UINT8(1, out.newSelfRearmCount);  // incrémenté, pas remis à 0
}

// IpAcquired : crédit déjà au plafond → AUCUN réarmement, crédit INCHANGÉ (pas
// remis à 0 non plus). Mutant tué : reclasser IpAcquired en externe (T2/M3).
void test_role_ip_acquired_refused_at_max(void) {
  NetRearmInputs in;
  in.sign = NetLifeSign::IpAcquired;
  in.selfRearmCount = kRealSelfRearmMax;
  in.maxSelfRearms = kRealSelfRearmMax;
  NetRearmOutcome out = evaluateNetRearm(in);
  TEST_ASSERT_FALSE(out.rearmClock);
  TEST_ASSERT_EQUAL_UINT8(kRealSelfRearmMax, out.newSelfRearmCount);
}

// ProbeStarted : même rôle qu'IpAcquired — AUTO-PRODUITE, gated, jamais de
// remise à 0. Mutant tué : permettre à ProbeStarted de remettre le crédit à 0
// (T2/M2), ou lui retirer le plafond (T1/T3/M1).
void test_role_probe_started_gated_below_max(void) {
  NetRearmInputs in;
  in.sign = NetLifeSign::ProbeStarted;
  in.selfRearmCount = 1;
  in.maxSelfRearms = kRealSelfRearmMax;
  NetRearmOutcome out = evaluateNetRearm(in);
  TEST_ASSERT_TRUE(out.rearmClock);
  TEST_ASSERT_EQUAL_UINT8(2, out.newSelfRearmCount);  // incrémenté, pas remis à 0
}

void test_role_probe_started_refused_at_max(void) {
  NetRearmInputs in;
  in.sign = NetLifeSign::ProbeStarted;
  in.selfRearmCount = kRealSelfRearmMax;
  in.maxSelfRearms = kRealSelfRearmMax;
  NetRearmOutcome out = evaluateNetRearm(in);
  TEST_ASSERT_FALSE(out.rearmClock);
  TEST_ASSERT_EQUAL_UINT8(kRealSelfRearmMax, out.newSelfRearmCount);
}

// ---- T1 : crédit épuisé ⇒ pas de réarmement, et le refus se propage jusqu'au
// Restart si le silence réseau atteint le seuil hard. Mutant tué : plafond
// rendu infini (ProbeStarted/IpAcquired réarment toujours, T1/T3/M1).
void test_T1_credit_exhausted_refuses_rearm(void) {
  NetRearmInputs in;
  in.sign = NetLifeSign::ProbeStarted;
  in.selfRearmCount = kRealSelfRearmMax;
  in.maxSelfRearms = kRealSelfRearmMax;
  NetRearmOutcome out = evaluateNetRearm(in);
  TEST_ASSERT_FALSE(out.rearmClock);
  TEST_ASSERT_EQUAL_UINT8(kRealSelfRearmMax, out.newSelfRearmCount);
}

void test_T1_credit_exhausted_leads_to_restart(void) {
  // Le refus de réarmer (ci-dessus) laisse l'horloge de stall courir : on
  // vérifie qu'evaluateNetHealth() en tire la conséquence attendue une fois le
  // seuil hard atteint — Restart, pas Continue.
  NetHealthInputs in;
  in.wifiAssociated = true;
  in.msSinceLastNetOk = kRealHardStallMs;
  in.softStallMs = kRealSoftStallMs;
  in.hardStallMs = kRealHardStallMs;
  in.softRecoverAttempted = true;
  in.uptimeMs = kRealHardStallMs * 10;
  in.minUptimeMs = 0;
  in.rebootCountInWindow = 0;
  in.maxReboots = 255;
  TEST_ASSERT_EQUAL(NetHealthDecision::Restart, evaluateNetHealth(in));
}

// ---- T5 : bornes exactes du crédit — exactement kNetworkSelfRearmMax
// réarmements consentis, le suivant est refusé. Le crédit est PARTAGÉ entre
// IpAcquired et ProbeStarted (un seul compteur, quelle que soit la preuve).
void test_T5_exactly_max_rearms_then_refused(void) {
  uint8_t credit = 0;
  for (uint8_t i = 0; i < kRealSelfRearmMax; ++i) {
    NetRearmInputs in;
    in.sign = NetLifeSign::ProbeStarted;
    in.selfRearmCount = credit;
    in.maxSelfRearms = kRealSelfRearmMax;
    NetRearmOutcome out = evaluateNetRearm(in);
    TEST_ASSERT_TRUE(out.rearmClock);
    credit = out.newSelfRearmCount;
  }
  TEST_ASSERT_EQUAL_UINT8(kRealSelfRearmMax, credit);

  NetRearmInputs in;
  in.sign = NetLifeSign::ProbeStarted;
  in.selfRearmCount = credit;
  in.maxSelfRearms = kRealSelfRearmMax;
  NetRearmOutcome out = evaluateNetRearm(in);
  TEST_ASSERT_FALSE(out.rearmClock);
  TEST_ASSERT_EQUAL_UINT8(kRealSelfRearmMax, out.newSelfRearmCount);
}

void test_T5_credit_shared_across_self_produced_signs(void) {
  uint8_t credit = 0;

  NetRearmInputs in1;
  in1.sign = NetLifeSign::IpAcquired;
  in1.selfRearmCount = credit;
  in1.maxSelfRearms = kRealSelfRearmMax;
  NetRearmOutcome out1 = evaluateNetRearm(in1);
  TEST_ASSERT_TRUE(out1.rearmClock);
  credit = out1.newSelfRearmCount;  // 1, consommé par IpAcquired

  NetRearmInputs in2;
  in2.sign = NetLifeSign::ProbeStarted;
  in2.selfRearmCount = credit;
  in2.maxSelfRearms = kRealSelfRearmMax;
  NetRearmOutcome out2 = evaluateNetRearm(in2);
  TEST_ASSERT_TRUE(out2.rearmClock);
  credit = out2.newSelfRearmCount;  // 2, le même compteur continue de monter

  NetRearmInputs in3;
  in3.sign = NetLifeSign::ProbeStarted;
  in3.selfRearmCount = credit;
  in3.maxSelfRearms = kRealSelfRearmMax;
  NetRearmOutcome out3 = evaluateNetRearm(in3);
  TEST_ASSERT_FALSE(out3.rearmClock);  // crédit épuisé par la somme des deux signes
}

// =============================================================================
// 9. netProbeSeemsDead() — bornes exactes (89999 / 90000 / 90001)
// =============================================================================
// Mutant tué : rendre netProbeSeemsDead() toujours faux (« doit tuer le test de
// recréation sur silence ») — ces bornes sont exactement ce test : si la
// fonction est neutralisée, l'assertion "dead" à 90000/90001 échoue directement.
void test_probe_not_dead_one_ms_below_silence(void) {
  TEST_ASSERT_FALSE(netProbeSeemsDead(kRealProbeSilenceMs - 1, kRealProbeSilenceMs));
}

void test_probe_dead_exactly_at_silence(void) {
  TEST_ASSERT_TRUE(netProbeSeemsDead(kRealProbeSilenceMs, kRealProbeSilenceMs));
}

void test_probe_dead_one_ms_above_silence(void) {
  TEST_ASSERT_TRUE(netProbeSeemsDead(kRealProbeSilenceMs + 1, kRealProbeSilenceMs));
}

// =============================================================================
// 10. Simulation de la coquille impure (main.cpp::checkSystemHealth, bloc
//     réseau, l. ~1464-1546) — T3, T4 et la séquence chaînée du 19/09
// =============================================================================
// Ces scénarios ne réimplémentent pas la logique de la coquille : ils rejouent
// EXACTEMENT la même séquence d'appels aux fonctions PURES (evaluateNetRearm,
// netProbeSeemsDead, evaluateNetHealth) que src/main.cpp, tick après tick — pour
// prouver un comportement observable (reboot ou non) sur toute une fenêtre
// temporelle, pas sur un appel isolé. C'est le niveau exigé par le gate
// pool-chemistry pour T3/T4/la séquence chaînée.
//
// LIMITE DOCUMENTÉE (relevée par pool-chemistry, à ne pas perdre) : dans ces
// scénarios, WiFi reste associé en permanence et AUCUNE preuve externe
// (IcmpEcho/AppTraffic) n'arrive jamais tant qu'on ne le simule pas
// explicitement. En production, un simple client HTTP qui interroge /data
// réarme SANS LIMITE (AppTraffic est externe) : le reboot peut donc être
// différé indéfiniment si un client sonde régulièrement le serveur web. Ce
// n'est PAS une régression de ce correctif — c'est déjà vrai sur HEAD, un OU
// plat sur les preuves externes — mais cela signifie que "la passerelle
// injoignable ⇒ reboot toujours" n'est vrai QUE dans le cas "aucune preuve
// externe", qui est précisément celui que T3 isole ci-dessous.
struct NetSim {
  uint32_t now = 0;
  uint32_t lastNetworkOkMs = 0;
  uint32_t lastIcmpEchoMs = 0;
  uint8_t  selfRearmCount = 0;
  bool     softRecoverAttempted = false;
  bool     probeExists = false;
  bool     netProbeDeclaredDead = false;
  bool     netProbeRestartRequested = false;
  int      restartCount = 0;
  int      softRecoverCount = 0;
};

// Système déjà en fonctionnement (pas un boot à froid) : la sonde ICMP existe
// déjà, crédit d'auto-réarmement inutilisé (des échos réels l'ont régulièrement
// remis à 0 jusqu'ici).
static NetSim netSimAlreadyRunning(void) {
  NetSim s;
  s.probeExists = true;
  return s;
}

// Équivalent de notifyNetworkLifeSign() (main.cpp) : applique evaluateNetRearm
// et répercute son verdict sur l'état simulé.
static void simNotify(NetSim& s, NetLifeSign sign) {
  NetRearmInputs in;
  in.sign = sign;
  in.selfRearmCount = s.selfRearmCount;
  in.maxSelfRearms = kRealSelfRearmMax;
  NetRearmOutcome out = evaluateNetRearm(in);
  s.selfRearmCount = out.newSelfRearmCount;
  if (out.rearmClock) s.lastNetworkOkMs = s.now;
}

// Équivalent de ensureNetworkProbe(force) (main.cpp) : ne (re)crée que si la
// sonde est absente OU si `force` est posé — dans les deux cas, notifie
// ProbeStarted (preuve AUTO-PRODUITE, passe par le crédit).
static void simEnsureProbe(NetSim& s, bool force) {
  if (!s.probeExists || force) {
    s.probeExists = true;
    s.lastIcmpEchoMs = s.now;
    simNotify(s, NetLifeSign::ProbeStarted);
  }
}

// Un tick de checkSystemHealth() (bloc réseau). WiFi supposé associé en
// permanence dans ces scénarios : la perte WiFi elle-même est hors périmètre
// bug-134 (déjà couverte section 1-7 via wifiAssociated=false).
static void simTick(NetSim& s) {
  simEnsureProbe(s, s.netProbeRestartRequested);
  s.netProbeRestartRequested = false;

  if (s.probeExists) {
    uint32_t msSinceLastEcho = s.now - s.lastIcmpEchoMs;
    bool probeDead = netProbeSeemsDead(msSinceLastEcho, kRealProbeSilenceMs);
    if (probeDead && !s.netProbeDeclaredDead) {
      s.netProbeRestartRequested = true;
      s.netProbeDeclaredDead = true;
    } else if (!probeDead && s.netProbeDeclaredDead) {
      s.netProbeDeclaredDead = false;
    }
  }

  uint32_t msSinceNetOk = s.now - s.lastNetworkOkMs;
  if (msSinceNetOk < kRealSoftStallMs) s.softRecoverAttempted = false;

  NetHealthInputs in;
  in.wifiAssociated = true;
  in.msSinceLastNetOk = msSinceNetOk;
  in.softStallMs = kRealSoftStallMs;
  in.hardStallMs = kRealHardStallMs;
  in.softRecoverAttempted = s.softRecoverAttempted;
  in.uptimeMs = s.now + kRealHardStallMs * 10;  // uptime jamais limitant ici
  in.minUptimeMs = 0;
  in.rebootCountInWindow = 0;
  in.maxReboots = 255;  // isole du plafond de reboots (hors périmètre bug-134)

  NetHealthDecision d = evaluateNetHealth(in);
  if (d == NetHealthDecision::Restart) {
    s.restartCount++;
  } else if (d == NetHealthDecision::SoftRecover) {
    s.softRecoverAttempted = true;
    s.netProbeRestartRequested = true;
    s.softRecoverCount++;
  }
}

static void simAdvance(NetSim& s, uint32_t dt) {
  s.now += dt;
  simTick(s);
}

// ---- T3 (test CENTRAL) : passerelle RÉELLEMENT injoignable, WiFi associé,
// recréations périodiques de la sonde ⇒ reboot A LIEU QUAND MÊME, sous
// (max+1) × kNetworkStallRebootMs. Aucune preuve externe n'arrive JAMAIS dans
// ce scénario (cf. limite documentée ci-dessus). Mutant tué : réarmement
// inconditionnel de ProbeStarted (l'ancien `lastNetworkOkMs = millis()` sans
// crédit) — c'est la boucle B de la spec bug-134.
void test_T3_gateway_unreachable_no_external_proof_restarts_eventually(void) {
  NetSim s = netSimAlreadyRunning();
  const uint32_t dt = 30000UL;  // ~kNetworkProbeIntervalMs
  const uint32_t bound = (uint32_t)(kRealSelfRearmMax + 1) * kRealHardStallMs;
  uint32_t elapsed = 0;
  while (s.restartCount == 0 && elapsed < bound) {
    simAdvance(s, dt);
    elapsed += dt;
  }
  TEST_ASSERT_TRUE_MESSAGE(s.restartCount >= 1,
      "passerelle injoignable sans preuve externe doit finir par reboot");
  TEST_ASSERT_TRUE_MESSAGE(elapsed < bound,
      "le reboot doit survenir avant (max+1) x kNetworkStallRebootMs");
  TEST_ASSERT_TRUE(s.selfRearmCount <= kRealSelfRearmMax);  // le plafond tient
}

// ---- T4 : rejeu de la séquence RÉELLE du 2026-09-19 (silence primaire →
// SoftRecover → GOT_IP → recréation de sonde → la passerelle est EN RÉALITÉ
// saine, les échos ICMP reprennent) ⇒ ZÉRO reboot sur toute la fenêtre. C'est
// le comportement attendu du correctif : la panne n'était qu'un signal
// auto-produit stagnant (sonde locale morte), pas une vraie coupure réseau.
// Vérifie aussi la fermeture de la boucle n°1 (softRecoverCount reste à 1).
void test_T4_replay_19_09_incident_zero_reboot(void) {
  NetSim s = netSimAlreadyRunning();

  // Silence primaire : ni écho ICMP ni trafic applicatif ne réarment plus rien.
  simAdvance(s, 60000);   // ~15:42:33
  simAdvance(s, 60000);   // ~15:43:33 — stall soft atteint (124s dans le journal)
  TEST_ASSERT_EQUAL(1, s.softRecoverCount);

  // ~15:43:38 : IP DHCP obtenue (GOT_IP), preuve AUTO-PRODUITE, crédit 0→1.
  s.now += 5000;
  simNotify(s, NetLifeSign::IpAcquired);
  TEST_ASSERT_EQUAL_UINT8(1, s.selfRearmCount);

  // Tick suivant : la coquille consomme netProbeRestartRequested (posé par le
  // SoftRecover) et recrée la sonde ICMP → ProbeStarted, crédit 1→2 (plafond).
  simAdvance(s, 55000);  // ~15:44:37 "WiFi reconnecté avec succès"
  TEST_ASSERT_EQUAL_UINT8(2, s.selfRearmCount);

  // À partir d'ici la passerelle EST joignable en réalité (contrairement à T3) :
  // les échos ICMP reprennent, preuve EXTERNE, remettent le crédit à 0 à chaque
  // fois, indéfiniment.
  for (int i = 0; i < 30; ++i) {
    s.now += 30000;
    s.lastIcmpEchoMs = s.now;
    simNotify(s, NetLifeSign::IcmpEcho);
    simTick(s);
  }

  TEST_ASSERT_EQUAL(0, s.restartCount);
  TEST_ASSERT_EQUAL(1, s.softRecoverCount);  // un seul, jamais de reboucle
}

// ---- Séquence chaînée (critère d'acceptation n°3) : silence primaire →
// SoftRecover → IpAcquired (crédit 1) → ProbeStarted (crédit 2) → silence
// PROLONGÉ (cette fois la passerelle ne répond plus JAMAIS, aucune preuve
// externe) → le Restart doit malgré tout avoir lieu. Distingue ce cas de T4 :
// mêmes trois premiers événements, issue opposée selon que le réseau guérit
// réellement ou pas ensuite.
void test_chained_states_credit_exhausted_then_restart(void) {
  NetSim s = netSimAlreadyRunning();

  simAdvance(s, 60000);
  simAdvance(s, 60000);  // silence primaire → SoftRecover
  TEST_ASSERT_EQUAL(1, s.softRecoverCount);

  s.now += 5000;
  simNotify(s, NetLifeSign::IpAcquired);  // crédit 0→1
  TEST_ASSERT_EQUAL_UINT8(1, s.selfRearmCount);

  simAdvance(s, 55000);  // recréation de sonde → ProbeStarted, crédit 1→2
  TEST_ASSERT_EQUAL_UINT8(2, s.selfRearmCount);

  // Silence PROLONGÉ : plus aucune preuve, ni externe ni auto-produite (crédit
  // épuisé), ne peut plus réarmer. Le reboot doit finir par avoir lieu.
  const uint32_t dt = 30000UL;
  const uint32_t bound = (uint32_t)(kRealSelfRearmMax + 1) * kRealHardStallMs;
  uint32_t elapsed = 0;
  while (s.restartCount == 0 && elapsed < bound) {
    simAdvance(s, dt);
    elapsed += dt;
  }
  TEST_ASSERT_TRUE_MESSAGE(s.restartCount >= 1,
      "critere d'acceptation n.3 : le reboot doit avoir lieu malgre le credit epuise");
}

int main(int, char**) {
  UNITY_BEGIN();
  // 1. WiFi non associé
  RUN_TEST(test_continue_when_wifi_not_associated);
  RUN_TEST(test_continue_wifi_absent_even_with_reboots_maxed);
  // 2. Uptime insuffisant
  RUN_TEST(test_continue_when_uptime_below_min);
  RUN_TEST(test_continue_uptime_below_min_even_huge_stall);
  // 3. Maintenance prioritaire
  RUN_TEST(test_maintenance_priority_over_restart);
  RUN_TEST(test_maintenance_even_when_network_healthy);
  RUN_TEST(test_maintenance_when_reboots_exceed_max);
  // 4. Restart (hard stall)
  RUN_TEST(test_restart_when_hard_stall);
  RUN_TEST(test_restart_when_stall_far_beyond_hard);
  RUN_TEST(test_restart_over_soft_when_soft_not_attempted);
  RUN_TEST(test_restart_reboots_one_below_max);
  // 5. SoftRecover / pas de 2e soft
  RUN_TEST(test_soft_recover_when_soft_stall_and_not_attempted);
  RUN_TEST(test_continue_when_soft_already_attempted);
  RUN_TEST(test_soft_recover_mid_range);
  RUN_TEST(test_continue_mid_range_soft_attempted);
  // 6. Réseau sain
  RUN_TEST(test_continue_when_network_healthy);
  RUN_TEST(test_continue_when_stall_below_soft);
  // 7. Frontières
  RUN_TEST(test_boundary_soft_stall_equals_soft_recover);
  RUN_TEST(test_boundary_soft_stall_one_below_continue);
  RUN_TEST(test_boundary_hard_stall_equals_restart);
  RUN_TEST(test_boundary_hard_stall_one_below_soft_recover);
  RUN_TEST(test_boundary_uptime_equals_min_armed);
  RUN_TEST(test_boundary_uptime_one_below_min_continue);
  RUN_TEST(test_boundary_reboots_equals_max_maintenance);
  RUN_TEST(test_boundary_reboots_one_below_max_restart);
  // 8. evaluateNetRearm() — rôle des 4 NetLifeSign, T1, T5
  RUN_TEST(test_role_icmp_echo_always_rearms_and_resets_credit);
  RUN_TEST(test_role_app_traffic_always_rearms_and_resets_credit);
  RUN_TEST(test_role_external_proof_resets_credit_even_above_max);
  RUN_TEST(test_role_ip_acquired_gated_below_max);
  RUN_TEST(test_role_ip_acquired_refused_at_max);
  RUN_TEST(test_role_probe_started_gated_below_max);
  RUN_TEST(test_role_probe_started_refused_at_max);
  RUN_TEST(test_T1_credit_exhausted_refuses_rearm);
  RUN_TEST(test_T1_credit_exhausted_leads_to_restart);
  RUN_TEST(test_T5_exactly_max_rearms_then_refused);
  RUN_TEST(test_T5_credit_shared_across_self_produced_signs);
  // 9. netProbeSeemsDead() — bornes
  RUN_TEST(test_probe_not_dead_one_ms_below_silence);
  RUN_TEST(test_probe_dead_exactly_at_silence);
  RUN_TEST(test_probe_dead_one_ms_above_silence);
  // 10. Simulation coquille — T3 (central), T4 (rejeu 19/09), séquence chaînée
  RUN_TEST(test_T3_gateway_unreachable_no_external_proof_restarts_eventually);
  RUN_TEST(test_T4_replay_19_09_incident_zero_reboot);
  RUN_TEST(test_chained_states_credit_exhausted_then_restart);
  return UNITY_END();
}
