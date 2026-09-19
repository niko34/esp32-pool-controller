#include "web_routes_data.h"
#include "web_helpers.h"
#include "auth.h"
#include "config.h"
#include "constants.h"
#include "sensors.h"
#include "filtration.h"
#include "measure_validity.h"
#include "pump_controller.h"
#include "time_to_target.h"  // feature-096 : instantané + mémoire 7 jours (/get-kpi)
#include "web_routes_control.h"
#include "logger.h"
#include "logs_page.h"
#include "logs_export_chunk.h"
#include "history.h"
#include "history_export_chunk.h"
#include "history_import_logic.h"  // bug-101 : mode "replace"/"append" (logique pure)
#include "heap_metrics.h"  // heapLargestFreeBlock8Bit — garde heap /get-logs (bug-080)
#include "fs_guard.h"     // bug-100 : point d'admission unique des fopen internes (ADR-0070)
#include "json_compat.h"
#include <AsyncJson.h>
#include <time.h>
#include <memory>
#include <stdarg.h>  // va_list (historyCarryLoadf)
#include <atomic>

// bug-100 : ouvertures du journal persistant refusées pour famine mémoire
// (GET /download-logs, tâche async_tcp). L'export est alors TRONQUÉ à la
// session courante, et le dit dans le fichier téléchargé.
static std::atomic<uint32_t> sLogsExportFopenDeferrals{0};

uint32_t logsExportFopenDeferralCount() {
  return sLogsExportFopenDeferrals.load(std::memory_order_relaxed);
}

// Signal réseau secondaire (v2.9.2, défini dans main.cpp) : /data est polled toutes
// les ~30 s par l'appli web → une requête servie prouve la pile HTTP vivante et
// rafraîchit le watchdog de connectivité (jamais de reboot pendant une session active).
void notifyNetworkActivity();

namespace {
bool isTimeValid(time_t t) {
  return t >= kMinValidEpoch;
}
}  // namespace

static void handleGetData(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);
  notifyNetworkActivity();
  // Buffer statique pour éviter la fragmentation du heap
  // Taille estimée : ~16 champs × 30 bytes + overhead (feature-020 ajoute 3 champs)
  // feature-025 : +14 champs filtre pH/ORP → bump du buffer.
  // feature-061 : +3 champs validité mesures → bump du buffer.
  // v2.20.3 (C2) : +2 booléens ph/orpFilterAlarm.
  //   "phFilterAlarm":false,  = 22 o  |  "orpFilterAlarm":false, = 23 o
  //   → 45 o au pire, arrondi à 64 o de marge → bump 1280 → 1344.
  // Itération 2 (bug-dosage-fantome, W2) : +4 champs de caractérisation pompes
  // (~134 o au pire) → bump 1344 → 1536.
  // Itération 3 (bug-dosage-fantome, défaut n°3) : +7 champs calendrier
  // d'impulsion (~169 o sérialisés au pire + 7 nœuds de pool ≈ 112 o)
  // → bump 1536 → 1792.
  // bug-slope-boot-perdue (B1) : +4 champs pente sonde pH (parité WS)
  // "phSlopeAcid":101.5, + "phSlopeBase":100.5, + "phSlopeZero":-12.34, +
  // "phSlopeAgeMs":4294967294, ≈ 87 o au pire ; clés const char* liées (non
  // copiées dans le pool), 4 nœuds ≈ 64 o → bump 1792 → 1920.
  // feature-075 : +1 objet flow_assist (9 champs dont tableau flow255[2],
  // ~165 o sérialisés au pire + 13 nœuds de pool ≈ 208 o) → bump 1920 → 2304.
  // D2 (v2.25.0) : +4 champs de parité avec le WS — ph/orpDoseBlockedReason
  // (chaînes, ~62/64 o au pire) + ph/orpRawGuardBlocked (booléens, 26+29 o).
  // TAILLE RÉELLE MESURÉE de la charge (harnais hôte ArduinoJson 7.4.3, valeurs
  // PIRE CAS) : 1591 o AVANT, 1823 o APRÈS — soit 481 o de marge sous le
  // gabarit, qui reste donc à 2304. NB : en ArduinoJson 7, StaticJson<N> dérive
  // de JsonDocument (N purement documentaire) et sendJsonResponse sérialise dans
  // une String dimensionnée à la demande : ce chemin n'a AUCUN plafond dur, donc
  // aucun risque de troncature silencieuse (contrairement à sWsFrameBuf côté WS).
  // v2.25.1 : +2 champs de CODE de cause de refus (parité WS, même passage).
  // Octets SÉRIALISÉS au pire cas (`null`, plus long que le code « 250 ») :
  //   "phDoseBlockedCode":null, = 25 o | "orpDoseBlockedCode":null, = 26 o → 51 o.
  // Charge pire cas : 1823 o (mesure v2.25.0) + 51 = 1874 o, soit 430 o de marge
  // sous le gabarit, qui reste donc à 2304.
  // v2.26.0 (C5) : +3 champs du terme intégral pH (parité WS, même passage).
  // Octets SÉRIALISÉS au pire cas (flottants arrondis, signe compris) :
  //   "ph_integral":-43200.0, = 23 o | "ph_integral_term":-6.000, = 26 o |
  //   "ph_integral_pct":100.0, = 24 o  → 73 o.
  // v2.26.0 (revue, mineure n°1) : +1 booléen de VERDICT de saturation (parité WS)
  //   "ph_integral_saturated":false, = 30 o → charge pire cas 1977 o, marge 327 o.
  // Charge pire cas : 1874 + 73 = 1947 o, soit 357 o de marge sous le gabarit,
  // qui reste donc à 2304. Rappel : ce chemin n'a AUCUN plafond dur
  // (sendJsonResponse dimensionne la String à la demande), le gabarit est
  // documentaire — contrairement à sWsFrameBuf côté WS.
  // feature-085 (v2.28.0) : +2 champs feedforward pH (parité WS, même passage).
  // Octets SÉRIALISÉS au pire cas (flottant 2 décimales signé, code 1 chiffre) :
  //   "ph_ff_ml_per_min":123.45, = 26 o | "ph_ff_status":4, = 17 o → 43 o.
  // Charge pire cas : 1977 (mesure v2.26.0) + 43 = 2020 o, marge 284 o sous le
  // gabarit, qui reste donc à 2304.
  // feature-086 (v2.29.0) : +4 champs limite d'actionneur pH (parité WS, même
  // passage). Octets SÉRIALISÉS au pire cas (flottant 2 décimales, entier 7
  // chiffres, booléens `false`) :
  //   "ph_sustainable_ml_per_min":123.45,  = 35 o | "ph_daily_max_ml":1439986, = 26 o
  //   "ph_limited_by_hourly_budget":false, = 36 o | "ph_undersized":false,     = 22 o
  //   → 119 o.
  // Charge pire cas : 2020 (v2.28.0) + 119 = 2139 o, marge 165 o sous le
  // gabarit, qui reste donc à 2304.
  // feature-091 (v2.34.0) : purge adaptative — `measures_stab_remaining_s` est
  // REMPLACÉ par `measures_stab_elapsed_s` et 3 champs sont ajoutés (parité WS,
  // même passage). Octets SÉRIALISÉS au pire cas :
  //   retiré  "measures_stab_remaining_s":4294967295, = 39 o
  //   ajouté  "measures_stab_elapsed_s":4294967295,         = 37 o
  //           "measures_purge_last_duration_s":4294967295,  = 44 o
  //           "measures_purge_last_end":4,                  = 28 o
  //           "measures_purge_count":255,                   = 27 o
  //   → +136 − 39 = +97 o (même calcul que ws_manager.cpp, mêmes champs).
  // Charge pire cas : 2139 (v2.29.0) + 97 = 2236 o, marge 68 o sous le gabarit,
  // qui reste donc à 2304. ATTENTION : la marge devient étroite — tout nouveau
  // champ de `/data` exige de recompter ici ET dans ws_manager.cpp (parité).
  // feature-096 (v2.37.0, 05/09/2026) : +8 champs « temps d'atteinte de cible »
  // (parité /data ↔ WS, MÊME passage). Octets SÉRIALISÉS au pire cas, comptés
  // clé par clé (guillemets + deux-points + valeur + virgule) :
  //   "ph_ttt_state":3,     → 1+12+1+1+1+1 = 17 o   (état 0-3, 1 chiffre)
  //   "ph_ttt_min":1440,    → 1+10+1+1+4+1 = 18 o   (borné à 1440 par le module pur)
  //   "ph_ttt_e0":-14.00,   → 1+ 9+1+1+6+1 = 19 o   (pH, 2 décimales signées ; `null` = 4 o, plus court)
  //   "ph_ttt_flag":255,    → 1+11+1+1+3+1 = 18 o   (CHAMP DE BITS uint8, jusqu'à 3 chiffres)
  //   "orp_ttt_state":3,    → 1+13+1+1+1+1 = 18 o
  //   "orp_ttt_min":1440,   → 1+11+1+1+4+1 = 19 o
  //   "orp_ttt_e0":-1000.0, → 1+10+1+1+7+1 = 21 o   (mV, 1 décimale signée)
  //   "orp_ttt_flag":255,   → 1+12+1+1+3+1 = 20 o
  //   → 150 o (mêmes clés, mêmes largeurs que le décompte de ws_manager.cpp).
  // Charge pire cas : 2236 (v2.34.0) + 150 = 2386 o, AU-DESSUS du gabarit 2304
  // → gabarit porté 2304 → 2432 (marge 46 o).
  // feature-092 (v2.46.0, 07/09/2026) : +8 champs de COMPOSITION du débit
  // d'injection (parité /data ↔ WS, MÊME passage, marqueurs PARITE-DATA) et,
  // côté /data seulement, +4 champs de PAUSE MÉLANGE qui étaient WS-seuls.
  //
  // MÉTHODE DE CHIFFRAGE — À LIRE AVANT DE TOUCHER À CE NOMBRE. On compte la
  // valeur au PIRE CAS STRUCTUREL, c'est-à-dire la borne du tampon ou du type
  // qui la porte, JAMAIS une valeur relevée sur une trame vivante. C'est
  // exactement l'erreur qui a rendu le chiffre précédent faux de 115 o : les
  // deux `*DoseBlockedReason` avaient été comptés ~62/64 o « au pire » alors
  // qu'ils sont bornés par kDoseBlockedReasonMaxLen = 96, soit 95 caractères
  // + 2 guillemets = 97 o de VALEUR chacun (contre ~8 o en fonctionnement
  // normal). Le décompte corrigé de ces deux clés :
  //   "phDoseBlockedReason":"<95 car.>",  → 1+19+1+1+97+1 = 120 o (comptée 62)
  //   "orpDoseBlockedReason":"<95 car.>", → 1+20+1+1+97+1 = 121 o (comptée 64)
  //   → correction +115 o sur le pire cas de la trame, sans qu'aucun champ soit
  //     ajouté. Ce pire cas n'est PAS théorique : les causes de refus les plus
  //     longues sont celles de l'ESCALADE DE SÉCURITÉ pH — le moment précis où
  //     perdre la trame coûte le plus cher.
  // Rappel du mode de défaillance côté WS : le dépassement SAUTE le push (garde
  // `measureJson(doc) + 1 > cap`), il ne TRONQUE jamais.
  //
  // Décompte des 8 nouvelles clés, une par une (1 guillemet + nom + guillemet +
  // deux-points + valeur + virgule). Valeurs signées comptées sur 8 caractères
  // (« -1234.56 ») : les termes p et i ne sont PAS bornés, c'est leur raison
  // d'être (grandeurs pédagogiques signées) ; les grandeurs ≥ 0 sur 6
  // (« 123.45 ») comme les champs mL/min existants ; `null` = 4 o, plus court,
  // donc jamais dimensionnant.
  //   "ph_ff_learned_ml_per_min":123.45,   → 1+24+1+1+6+1 = 34 o
  //   "ph_p_term_ml_per_min":-1234.56,     → 1+20+1+1+8+1 = 32 o
  //   "ph_i_term_ml_per_min":-1234.56,     → 1+20+1+1+8+1 = 32 o
  //   "ph_demand_ml_per_min":123.45,       → 1+20+1+1+6+1 = 30 o
  //   "orp_p_term_ml_per_min":-1234.56,    → 1+21+1+1+8+1 = 33 o
  //   "orp_demand_ml_per_min":123.45,      → 1+21+1+1+6+1 = 31 o
  //   "orp_sustainable_ml_per_min":123.45, → 1+26+1+1+6+1 = 36 o
  //   "orp_limited_by_hourly_budget":false, → 1+28+1+1+5+1 = 37 o
  //   → 265 o.
  // Décompte des 4 clés de pause mélange (uint32 comptés sur 10 chiffres — borne
  // du TYPE, pas la borne fonctionnelle de 900 s) :
  //   "phMixingDelayActive":false,      → 1+19+1+1+ 5+1 = 28 o
  //   "orpMixingDelayActive":false,     → 1+20+1+1+ 5+1 = 29 o
  //   "ph_mix_remaining_s":4294967295,  → 1+18+1+1+10+1 = 32 o
  //   "orp_mix_remaining_s":4294967295, → 1+19+1+1+10+1 = 33 o
  //   → 122 o (déjà comptés côté WS depuis v2.19.1 : ils y existaient déjà).
  // Pire cas de charge : 2386 (décompte v2.37.0) + 115 (correction ci-dessus)
  // + 265 (8 champs neufs) + 122 (4 champs de pause mélange, nouveaux ICI) =
  // 2888 o, AU-DESSUS du gabarit 2432 → gabarit porté 2432 → 2944 (marge 56 o).
  // Les 2 booléens `*_pump_flow_uncharacterized` ne s'AJOUTENT pas ici : ils
  // étaient déjà servis par /data (ils ne font que MIGRER dans le bloc de
  // parité) — ce sont les 75 o que la trame WS, elle, doit désormais porter.
  // Contrairement au WebSocket, ce chemin n'a AUCUN plafond dur : la marge n'est
  // qu'un indicateur d'hygiène, aucune troncature n'est possible ici.
  // RAPPEL, et ce n'est pas une formalité : ce gabarit est PUREMENT DOCUMENTAIRE.
  // En ArduinoJson 7, StaticJson<N> dérive de JsonDocument (N n'est plus qu'une
  // valeur de capacity(), le pool est alloué dynamiquement) et sendJsonResponse
  // sérialise dans une String dimensionnée à la demande : ce chemin n'a AUCUN
  // plafond dur et AUCUNE troncature n'y est possible — contrairement à
  // sWsFrameBuf côté WebSocket (kWsFrameBufBytes, seul plafond DUR du projet).
  //
  // feature-068 (A4) : +1 booléen `tempFrozenAlarm` (sonde de température d'eau
  // figée), servi dans le MÊME passage que la trame WS, entre les mêmes
  // marqueurs PARITE-DATA. MÉTHODE DE CHIFFRAGE (ADR-0073), refaite et non
  // recopiée : borne STRUCTURELLE du type — `bool`, dont la sérialisation la
  // plus longue est `false` (5 o) et non `true` (4 o) ; clé de 15 caractères
  // comptés un à un (t-e-m-p-F-r-o-z-e-n-A-l-a-r-m).
  //   "tempFrozenAlarm":false, → 1+15+1+1+5+1 = 24 o
  // Contre-mesure au harnais hôte : 24 o avec `false`, 23 o avec `true` (cf. le
  // même décompte dans src/ws_manager.cpp).
  // Pire cas de charge : 2888 (décompte feature-092) + 24 = 2912 o, sous le
  // gabarit 2944 (marge 32 o) → AUCUN bump. Rappel : ce gabarit est PUREMENT
  // documentaire, ce chemin n'a aucun plafond dur. Le budget CONTRAIGNANT de
  // cet ajout est celui de la trame WS (kWsFrameBufBytes), décompté dans
  // src/ws_manager.cpp : marge portée de 67 à 43 o.
  StaticJson<2944> doc;

  // feature-025 : la "valeur courante" (champs ph/orp) reflète la valeur FILTRÉE
  // (médiane + EMA), alignée sur le WS. Le brut reste exposé séparément (phRaw/orpRaw).
  // Fallback sur le brut si le filtre n'est pas encore amorcé (warmup).
  // feature-067 : `ph` = valeur CORRIGÉE (brut + offset logiciel), comme le WS et
  // MQTT. `phRaw` reste le BRUT EZO (diagnostic), `phMedian`/`phFiltered` sont
  // corrigés. Le repli de warmup passe par getPh() (corrigé), jamais par le brut.
  float orpRaw = sensors.getOrpRaw();
  float phRaw  = sensors.getPhRaw();      // BRUT EZO (sans offset)
  float orpMed = sensors.getOrpMedian();
  float phMed  = sensors.getPhMedian();   // médiane CORRIGÉE
  float orpFil = sensors.getOrpFiltered();
  float phFil  = sensors.getPhFiltered(); // lissé CORRIGÉ
  float orpVal = !isnan(orpFil) ? orpFil : orpRaw;
  float phVal  = !isnan(phFil)  ? phFil  : sensors.getPh();

  // ORP (valeur courante = filtrée)
  if (!isnan(orpVal)) {
    doc["orp"] = round(orpVal);
  } else {
    doc["orp"] = nullptr;
  }

  // pH (feature-021 : 3 décimales fiables EZO — alignement WS / MQTT / REST).
  if (!isnan(phVal)) {
    doc["ph"] = round(phVal * 1000.0f) / 1000.0f;
  } else {
    doc["ph"] = nullptr;
  }

  // feature-025 : chaîne de filtrage exposée pour la chip d'état + ligne brute UI.
  if (!isnan(phRaw)) doc["phRaw"] = round(phRaw * 1000.0f) / 1000.0f; else doc["phRaw"] = nullptr;
  if (!isnan(phMed)) doc["phMedian"] = round(phMed * 1000.0f) / 1000.0f; else doc["phMedian"] = nullptr;
  if (!isnan(phFil)) doc["phFiltered"] = round(phFil * 1000.0f) / 1000.0f; else doc["phFiltered"] = nullptr;
  doc["phFilterReady"]    = sensors.isPhFilterReady();
  doc["phFilterUnstable"] = sensors.isPhFilterUnstable();
  doc["phRejectedCount"]  = sensors.getPhRejectedCount();
  // v2.20.3 (C2) : alarme « filtre non prêt durablement » (≥ 5 min alors que la
  // filtration tourne ET que les mesures sont représentatives) = plus AUCUNE
  // régulation. Exposée pour le chip d'état et la bannière du tableau de bord.
  doc["phFilterAlarm"]    = sensors.isPhFilterNotReadyAlarm();
  if (!isnan(orpRaw)) doc["orpRaw"] = round(orpRaw); else doc["orpRaw"] = nullptr;
  if (!isnan(orpMed)) doc["orpMedian"] = round(orpMed); else doc["orpMedian"] = nullptr;
  if (!isnan(orpFil)) doc["orpFiltered"] = round(orpFil); else doc["orpFiltered"] = nullptr;
  doc["orpFilterReady"]    = sensors.isOrpFilterReady();
  doc["orpFilterUnstable"] = sensors.isOrpFilterUnstable();
  doc["orpRejectedCount"]  = sensors.getOrpRejectedCount();
  doc["orpFilterAlarm"]    = sensors.isOrpFilterNotReadyAlarm();  // v2.20.3 (C2)

  // feature-034 : points de calibration EZO (déjà publiés en WS, mais ABSENTS de /data —
  // nécessaires au polling de calibration qui interroge /data activement, Safari mettant
  // le WebSocket en pause sur un onglet non focalisé).
  doc["phCalPoints"]  = sensors.getPhCalibrationPointsCached();
  doc["orpCalPoints"] = sensors.getOrpCalibrationPointsCached();

  // bug-slope-boot-perdue (B1) : pente sonde pH — parité de transport avec la
  // trame WS (ws_manager.cpp, feature-024). Lecture CACHE uniquement, ZÉRO I²C
  // dans ce chemin (contexte async_tcp). Mêmes règles que le WS :
  // pentes arrondies à 1 décimale (résolution EZO), zéro à 2 décimales (mV),
  // null si jamais lu OU invalidé (NaN) ; ageMs null si jamais lu (UINT32_MAX).
  {
    float slopeAcid = sensors.getPhSlopeAcid();
    float slopeBase = sensors.getPhSlopeBase();
    float slopeZero = sensors.getPhSlopeZero();
    if (!isnan(slopeAcid)) doc["phSlopeAcid"] = round(slopeAcid * 10.0f) / 10.0f;   else doc["phSlopeAcid"] = nullptr;
    if (!isnan(slopeBase)) doc["phSlopeBase"] = round(slopeBase * 10.0f) / 10.0f;   else doc["phSlopeBase"] = nullptr;
    if (!isnan(slopeZero)) doc["phSlopeZero"] = round(slopeZero * 100.0f) / 100.0f; else doc["phSlopeZero"] = nullptr;
    uint32_t slopeAge = sensors.getPhSlopeAgeMs();
    if (slopeAge == UINT32_MAX) doc["phSlopeAgeMs"] = nullptr;
    else                        doc["phSlopeAgeMs"] = slopeAge;
  }

  // Température (offset utilisateur appliqué dans getTemperature())
  if (!isnan(sensors.getTemperature())) {
    doc["temperature"] = sensors.getTemperature();
  } else {
    doc["temperature"] = nullptr;
  }

  // feature-020 : T° circuit (2ᵉ sonde DS18B20) + indicateurs identification
  float tCircuit = sensors.getCircuitTemperature();
  if (!isnan(tCircuit)) {
    doc["temperature_circuit"] = round(tCircuit * 10.0f) / 10.0f;
  } else {
    doc["temperature_circuit"] = nullptr;
  }
  doc["sondes_identified"] = sensors.areSondesIdentified();
  doc["sondes_detected"]   = sensors.getDetectedSondeCount();

  doc["filtration_running"] = filtration.isRunning();

  // feature-061 : validité des mesures pH/ORP — lue depuis le CACHE de
  // measure_validity (snapshot cohérent sous spinlock). NE JAMAIS appeler
  // filtration.resolveWaterPresence() ici : callback en contexte async_tcp.
  // NB : les champs live ph/orp/phRaw/... restent publiés tels quels (page
  // calibration + onglet Avancé) — l'UI arbitre l'affichage via measures_valid.
  // feature-091 (v2.34.0) : purge ADAPTATIVE — plus de « restant » (la fin n'est
  // pas datée à l'avance), mais l'écoulé, la dernière durée et son motif de fin.
  // PARITÉ /data ↔ WS obligatoire : mêmes clés dans ws_manager.cpp.
  {
    const MeasureValiditySnapshot mv = measureValidity.getSnapshot();
    doc["water_present"] = mv.waterPresent;
    doc["measures_valid"] = mv.measuresValid;
    // Nom distinct de stabilization_remaining_s (WS = stabilisation post-
    // calibration des pompes) : ici purge de la chambre de mesure.
    doc["measures_stab_elapsed_s"] = mv.stabElapsedS;
    doc["measures_purge_last_duration_s"] = mv.lastPurgeDurationS;
    doc["measures_purge_last_end"] = mv.lastPurgeEnd;  // 0 aucune/1 stabilité/2 plafond/3 dispensée/4 re-purge
    doc["measures_purge_count"] = mv.purgeCount;
  }
  doc["ph_dosing"] = PumpController.isPhDosing();
  doc["orp_dosing"] = PumpController.isOrpDosing();
  // Itération 2 (bug-dosage-fantome, W2) : état de caractérisation par CHIMIE
  // (les getters résolvent l'affectation mqttCfg.phPump/orpPump) + amplitude du
  // plateau d'impulsion (duty commandé pendant un pouls). Alimente le badge
  // « Débit non caractérisé — pesée requise » de l'UI. Getters lecture seule,
  // sûrs en contexte async_tcp (cf. pump_controller.h).
  // feature-092 : `ph/orp_pump_flow_uncharacterized` ont MIGRÉ dans le bloc
  // `PARITE-DATA` plus bas — ils étaient /data-SEULS, donc effacés de
  // `latestSensorData` par chaque push WebSocket (pied de carte clignotant).
  doc["ph_pump_amplitude_duty"]  = PumpController.getPumpAmplitudeDuty(pumpIndexFromNumber(mqttCfg.phPump));
  doc["orp_pump_amplitude_duty"] = PumpController.getPumpAmplitudeDuty(pumpIndexFromNumber(mqttCfg.orpPump));
  // Itération 3 (bug-dosage-fantome, défaut n°3) : calendrier d'impulsion —
  // distingue « session active » (ph/orp_dosing = commandé) de « pompe alimentée »
  // (pouls effectif : phase < pulse_ms). Getters LECTURE SEULE (pump_controller.h,
  // sûrs en contexte async_tcp). La phase est RELATIVE au début de période (jamais
  // un millis absolu) : le client l'ancre sur SA propre horloge à la réception.
  // Noms de champs IDENTIQUES au WS (_buildSensorJson) — contrat architect.
  {
    uint32_t pulseNowMs = millis();
    doc["pulse_period_ms"]    = kPulsePeriodMs;
    doc["ph_pulse_session"]   = PumpController.isPulseSessionActive(0);
    doc["orp_pulse_session"]  = PumpController.isPulseSessionActive(1);
    doc["ph_pulse_ms"]        = PumpController.getPulseMs(0);
    doc["orp_pulse_ms"]       = PumpController.getPulseMs(1);
    doc["ph_pulse_phase_ms"]  = PumpController.getPulsePhaseMs(0, pulseNowMs);
    doc["orp_pulse_phase_ms"] = PumpController.getPulsePhaseMs(1, pulseNowMs);
  }

  // D2 (v2.25.0) : PARITÉ /data ↔ WS de l'état de blocage du dosage. Troisième
  // récidive du même motif après les pentes de sonde (v2.21.2) et les champs de
  // calibration de la trame de config (v2.23.0) : ces champs n'existaient QUE sur
  // le WebSocket. Or `latestSensorData` est intégralement REMPLACÉ par les
  // charges /data sur plusieurs chemins côté client — l'information ne
  // disparaissait donc pas seulement pour un client sans WS, elle clignotait par
  // INTERMITTENCE, ce qui est pire qu'une absence franche.
  //
  // Règles null/arrondi STRICTEMENT identiques à ws_manager::_buildSensorJson
  // (modèle : le précédent v2.21.2) : chaîne vide → null explicite (absent ≠ 0),
  // booléens publiés tels quels. Getters LECTURE SEULE, sûrs en contexte
  // async_tcp (cf. pump_controller.h) — aucun I²C, aucune décision de dosage.
  //
  // R2 (revue v2.25.0) : COPIE BORNÉE dans un tampon de PILE, et non copie d'une
  // `String` du contrôleur. Cette route s'exécute sur `async_tcp` alors que
  // loopTask réassigne la cause (surveillance, escalade, canDose) : copier une
  // `String` lisait `buffer`/`len` pendant que l'écrivain pouvait libérer et
  // réallouer le tas. Le tampon source est désormais fixe, donc indéboulonnable ;
  // le pire cas est une lecture déchirée (message partiellement à jour).
  // ArduinoJson DUPLIQUE un `char[]` non const dans le document (seuls les
  // littéraux `const char(&)[N]` sont stockés par pointeur) : le tampon de pile
  // peut donc mourir à la sortie du bloc.
  {
    char phBlocked[kDoseBlockedReasonMaxLen];
    char orpBlocked[kDoseBlockedReasonMaxLen];
    PumpController.copyPhDoseBlockedReason(phBlocked, sizeof(phBlocked));
    PumpController.copyOrpDoseBlockedReason(orpBlocked, sizeof(orpBlocked));
    if (phBlocked[0]  != '\0') doc["phDoseBlockedReason"]  = phBlocked;  else doc["phDoseBlockedReason"]  = nullptr;
    if (orpBlocked[0] != '\0') doc["orpDoseBlockedReason"] = orpBlocked; else doc["orpDoseBlockedReason"] = nullptr;
    // v2.25.1 : CODE de cause à côté du texte — le client ne doit plus deviner la
    // cause en cherchant des fragments dans une phrase française (un « indispo »
    // de warmup matchait « EZO indisponible » : panne affichée sur sonde saine).
    // MÊME règle null que le texte, et publié dans le MÊME passage que le WS
    // (ws_manager::_buildSensorJson) : 0 = aucun refus → null explicite.
    const uint8_t phCode  = PumpController.getPhDoseBlockedCode();
    const uint8_t orpCode = PumpController.getOrpDoseBlockedCode();
    if (phCode  != kDoseBlockedCodeNone) doc["phDoseBlockedCode"]  = phCode;  else doc["phDoseBlockedCode"]  = nullptr;
    if (orpCode != kDoseBlockedCodeNone) doc["orpDoseBlockedCode"] = orpCode; else doc["orpDoseBlockedCode"] = nullptr;
    // « La butée BRUTE mord » — indéductible du pH corrigé publié : c'est tout
    // l'objet de ces deux booléens. Recalculés à chaque tick de la surveillance,
    // SANS latch (un latch reproduirait le défaut D1 de la cause collante).
    doc["phRawGuardBlocked"]    = PumpController.isPhRawGuardBlocked();
    doc["orpPhRawGuardBlocked"] = PumpController.isOrpPhRawGuardBlocked();
  }
  // v2.26.0 (C5) : TERME INTÉGRAL pH — observabilité de la levée de Ki. Publié
  // dans le MÊME passage que le WS (ws_manager::_buildSensorJson), avec les MÊMES
  // règles null (NaN → null explicite) et les MÊMES arrondis : quatrième
  // application du contrat de parité /data ↔ WS après les pentes de sonde
  // (v2.21.2), les champs de calibration (v2.23.0) et l'état de blocage (v2.25.0).
  // Une intégrale saturée en silence est LA classe de défaut de ce projet : ces
  // trois champs sont le seul moyen de la voir de l'extérieur.
  // Getters LECTURE SEULE, sûrs en contexte async_tcp (cf. pump_controller.h).
  {
    const float phIntegral    = PumpController.getPhIntegral();
    const float phIntegralTrm = PumpController.getPhIntegralTerm();
    const float phIntegralPct = PumpController.getPhIntegralPct();
    if (!isnan(phIntegral))    doc["ph_integral"]      = round(phIntegral * 10.0f) / 10.0f;         else doc["ph_integral"]      = nullptr;
    if (!isnan(phIntegralTrm)) doc["ph_integral_term"] = round(phIntegralTrm * 1000.0f) / 1000.0f;  else doc["ph_integral_term"] = nullptr;
    if (!isnan(phIntegralPct)) doc["ph_integral_pct"]  = round(phIntegralPct * 10.0f) / 10.0f;      else doc["ph_integral_pct"]  = nullptr;
    // v2.26.0 (revue, mineure n°1) : VERDICT de saturation, jamais un seuil à
    // recalculer côté client. Les 95 % du firmware portent sur `demande / débit
    // soutenable` ; le client les comparait au REMPLISSAGE (`ph_integral_pct`) —
    // deux grandeurs derrière un seul nombre. Une intégrale au plafond qui tient
    // la charge sans forcer est une régulation SAINE : la comparaison locale
    // annonçait « vérifiez le bidon » sans qu'aucun épisode ne soit ouvert.
    doc["ph_integral_saturated"] = PumpController.isPhIntegralSaturated();
    // feature-085 (v2.28.0) : FEEDFORWARD pH — dose de base apprise. Publié dans
    // le MÊME passage que le WS (ws_manager::_buildSensorJson), MÊMES clés, MÊMES
    // arrondis (2 décimales), MÊME règle NaN → null (parité stricte /data ↔ WS —
    // un champ servi d'un seul côté CLIGNOTE dans l'UI, 5 récidives du motif).
    //   ph_ff_ml_per_min = débit de base appliqué à la session auto (mL/min, 0 hors session)
    //   ph_ff_status     = code PhFfStatus (0 inactif, 1 actif, 2 latch sécurité,
    //                      3 historique < 3 j, 4 quota ≥ 60 %) — contrat FIGÉ,
    //                      cf. pump_controller.h. Entier toujours servi (jamais null).
    const float phFfFlow = PumpController.getPhFeedforwardFlow();
    if (!isnan(phFfFlow)) doc["ph_ff_ml_per_min"] = round(phFfFlow * 100.0f) / 100.0f; else doc["ph_ff_ml_per_min"] = nullptr;
    doc["ph_ff_status"] = PumpController.getPhFeedforwardStatus();
    // feature-086 (v2.29.0) : LIMITE D'ACTIONNEUR pH — capacités et verdicts de
    // diagnostic. Accesseurs LECTURE SEULE (aucun chemin nouveau vers les
    // pompes, cf. pump_controller.cpp). Publié dans le MÊME passage que le WS
    // (ws_manager::_buildSensorJson), MÊMES clés, MÊMES arrondis, MÊME règle
    // NaN → null (parité stricte /data ↔ WS — un champ servi d'un seul côté
    // CLIGNOTE dans l'UI, 5 récidives du motif).
    //   ph_sustainable_ml_per_min   = plafond de débit moyen imposé par la
    //       limite horaire (mL/min, 2 décimales ; null si pompe non caractérisée)
    //   ph_daily_max_ml             = maximum injectable sur la journée de
    //       filtration PROGRAMMÉE (mL/j, entier — la décimale serait du bruit ;
    //       null si soutenable null ou planning indisponible)
    //   ph_limited_by_hourly_budget = épisode de saturation C5 ouvert ET mesure
    //       pH valide (booléen, jamais null)
    //   ph_undersized               = série ≥ 3 journées saturées ET anomalie C5
    //       ouverte (booléen, jamais null)
    const float phSust     = PumpController.getPhSustainableMlPerMin();
    const float phDailyMax = PumpController.getPhDailyMaxMl();
    if (!isnan(phSust))     doc["ph_sustainable_ml_per_min"] = round(phSust * 100.0f) / 100.0f; else doc["ph_sustainable_ml_per_min"] = nullptr;
    if (!isnan(phDailyMax)) doc["ph_daily_max_ml"] = (uint32_t)lroundf(phDailyMax);             else doc["ph_daily_max_ml"] = nullptr;
    doc["ph_limited_by_hourly_budget"] = PumpController.isPhLimitedByHourlyBudget();
    doc["ph_undersized"]               = PumpController.isPhUndersizedSuspected();
  }
  // >>> PARITE-DATA:DEBUT
  // CONVENTION NORMATIVE (lue telle quelle par la sentinelle de parité, sur le
  // patron des marqueurs PARITE-CONFIG de web_routes_config.cpp / ws_manager.cpp) :
  // entre ces deux marqueurs, TOUTE clé s'écrit `doc["<clé>"]` — un seul
  // receveur. L'autre côté du contrat porte les MÊMES clés entre les MÊMES
  // marqueurs. Une clé posée hors des marqueurs est INVISIBLE pour la sentinelle :
  // la parité cesserait d'être garantie sans que rien ne casse.
  //
  // POURQUOI CE BLOC EXISTE (feature-092). Un champ servi d'un seul côté ne
  // disparaît pas, il CLIGNOTE : `latestSensorData` est alimenté par le WS, puis
  // intégralement REMPLACÉ par les charges /data toutes les ~30 s. Trois
  // récidives documentées en trois versions ; la pause mélange était encore dans
  // ce cas (WS-seule) — d'où sa migration DANS ce bloc.
  //
  // PAUSE MÉLANGE — les quatre champs qui manquaient à /data. La pastille d'état
  // de la carte « Débit d'injection » les consomme : sans eux, elle basculerait
  // de « Pause mélange · 12 min » à « En attente » à chaque charge /data.
  const uint32_t mixNowMs = millis();
  doc["phMixingDelayActive"]  = PumpController.isPhMixingDelayActive(mixNowMs);
  doc["orpMixingDelayActive"] = PumpController.isOrpMixingDelayActive(mixNowMs);
  doc["ph_mix_remaining_s"]   = PumpController.getMixingRemainingS(0);
  doc["orp_mix_remaining_s"]  = PumpController.getMixingRemainingS(1);
  //
  // COMPOSITION DU DÉBIT D'INJECTION (feature-092) — l'équation que la régulation
  // applique déjà, rendue lisible : demande = dose de base apprise + écart
  // courant + rattrapage accumulé, bornée par le débit soutenable.
  //   ph_ff_learned_ml_per_min   = dose de base APPRISE (mL/min, 2 décimales),
  //       min(charge médiane 7 j, 60 % du soutenable) — calculée À LA LECTURE :
  //       elle reste vraie hors session, contrairement à `ph_ff_ml_per_min` qui
  //       vaut 0 hors session (les deux coexistent, aucun n'est renommé).
  //       null si < 3 journées exploitables ou pompe non caractérisée. 0,00 avec
  //       3 journées sans injection est une OBSERVATION, pas une indisponibilité.
  //   ph_p_term_ml_per_min       = écart courant kp·e, SIGNÉ (négatif = pH sous
  //       la consigne), 0 dans la bande morte, JAMAIS borné ; null si la mesure
  //       n'est pas représentative ou si kp vaut 0 (régulateur inerte : un 0,00
  //       s'y lirait « pH à la consigne » sur une régulation morte).
  //   ph_i_term_ml_per_min       = rattrapage SERVI (0 dans la bande morte) —
  //       distinct de `ph_integral_term`, terme BRUT et contrat externe
  //       historique, qui reste inchangé. Deux champs, deux sémantiques.
  //   ph_demand_ml_per_min       = demande TOTALE bornée = clamp(PI servi + FF,
  //       0, débit d'amplitude). La borne basse porte sur le COUPLE p + i, jamais
  //       sur la somme des trois termes (cf. doseComposition, dosing_logic.h).
  //   orp_p_term_ml_per_min      = idem ORP ; orp_demand_ml_per_min = demande
  //       (régulation P-PURE : Ki = 0, aucun feedforward — la demande VAUT le
  //       terme proportionnel borné, et la carte ORP montre ainsi POURQUOI le
  //       chlore est régulé plus simplement).
  //   orp_sustainable_ml_per_min = plafond de débit moyen de la voie ORP imposé
  //       par sa limite horaire (miroir de `ph_sustainable_ml_per_min`).
  //   orp_limited_by_hourly_budget = VERDICT firmware « le chlore est borné par
  //       sa limite horaire EN CE MOMENT » (booléen, jamais null) — homologue
  //       d'`ph_limited_by_hourly_budget`, qui n'existait pas côté ORP alors que
  //       c'est la voie qui rencontre le cas TOUS LES JOURS sur cet appareil
  //       (journal du 07/09 09:15 : « Limite horaire ORP atteinte : 10 min/h
  //       consommées »). Verdict, jamais une comparaison à refaire côté client.
  // Règle NaN → null EXPLICITE (absent ≠ 0) et arrondi 2 décimales pour les
  // sept — IDENTIQUES de l'autre côté, servis dans le MÊME passage.
  // Accesseurs LECTURE SEULE : aucune I²C, aucun LittleFS, aucune NVS, aucun
  // appel à getPhDecisionValue() (réservée à loopTask, condition C2 du gate
  // feature-096) — sûrs dans un handler AsyncWebServer.
  const float phFfLearned  = PumpController.getPhFeedforwardLearnedFlow();
  const float phPTerm      = PumpController.getPhPTermFlow();
  const float phITerm      = PumpController.getPhITermFlow();
  const float phDemand     = PumpController.getPhDemandFlow();
  const float orpPTerm     = PumpController.getOrpPTermFlow();
  const float orpDemand    = PumpController.getOrpDemandFlow();
  const float orpSust      = PumpController.getOrpSustainableMlPerMin();
  if (!isnan(phFfLearned)) doc["ph_ff_learned_ml_per_min"] = round(phFfLearned * 100.0f) / 100.0f; else doc["ph_ff_learned_ml_per_min"] = nullptr;
  if (!isnan(phPTerm))     doc["ph_p_term_ml_per_min"]     = round(phPTerm * 100.0f) / 100.0f;     else doc["ph_p_term_ml_per_min"]     = nullptr;
  if (!isnan(phITerm))     doc["ph_i_term_ml_per_min"]     = round(phITerm * 100.0f) / 100.0f;     else doc["ph_i_term_ml_per_min"]     = nullptr;
  if (!isnan(phDemand))    doc["ph_demand_ml_per_min"]     = round(phDemand * 100.0f) / 100.0f;    else doc["ph_demand_ml_per_min"]     = nullptr;
  if (!isnan(orpPTerm))    doc["orp_p_term_ml_per_min"]    = round(orpPTerm * 100.0f) / 100.0f;    else doc["orp_p_term_ml_per_min"]    = nullptr;
  if (!isnan(orpDemand))   doc["orp_demand_ml_per_min"]    = round(orpDemand * 100.0f) / 100.0f;   else doc["orp_demand_ml_per_min"]    = nullptr;
  if (!isnan(orpSust))     doc["orp_sustainable_ml_per_min"] = round(orpSust * 100.0f) / 100.0f;   else doc["orp_sustainable_ml_per_min"] = nullptr;
  doc["orp_limited_by_hourly_budget"] = PumpController.isOrpLimitedByHourlyBudget();
  //
  // CARACTÉRISATION DE POMPE — deux booléens qui étaient /data-SEULS, donc le
  // piège n° 1 du projet dans son sens INVERSE : ce n'est pas la charge /data
  // qui écrasait un champ WS, c'est le push WebSocket qui effaçait un champ
  // /data de `latestSensorData`. Le pied « pompe non caractérisée » de la carte
  // « Débit d'injection » s'affichait donc une seconde puis disparaissait
  // jusqu'au rafraîchissement suivant. Même mécanisme, même correctif : la
  // PARITÉ, pas une mémorisation côté client. Booléens d'état STABLE (une pesée
  // au banc, un réglage de puissance), jamais null.
  doc["ph_pump_flow_uncharacterized"]  = PumpController.isPhPumpFlowUncharacterized();
  doc["orp_pump_flow_uncharacterized"] = PumpController.isOrpPumpFlowUncharacterized();
  //
  // SONDE DE TEMPÉRATURE D'EAU FIGÉE (feature-068, A4) — drapeau INFORMATIF, et
  // c'est tout ce qu'il est : le verdict pool-chemistry du 07/09/2026 a chiffré
  // l'erreur de pH induite par une compensation de température fausse à
  // ≈ 0,7 millième de pH par °C d'écart, soit ≈ 0,001 pH sur le cas relevé —
  // deux à trois ORDRES DE GRANDEUR sous la bande morte la plus serrée (0,01).
  // La compensation, la régulation et le planning de filtration sont donc
  // INCHANGÉS : on instrumente, on ne corrige pas. Ce booléen ne doit jamais
  // servir à inhiber quoi que ce soit côté client.
  // Booléen STRICT, jamais null (l'absence de sonde n'est pas « figé » : elle se
  // lit sur `temperature` / `temperature_raw`, déjà servis plus haut, et l'UI
  // fait primer « sonde absente ou invalide » sur « figée », plus sévère).
  // Accesseur LECTURE SEULE d'un compteur de run (aucune I²C, aucun OneWire) :
  // sûr dans un handler AsyncWebServer.
  doc["tempFrozenAlarm"] = sensors.isTemperatureFrozen();
  // <<< PARITE-DATA:FIN
  doc["ph_daily_ml"] = safetyLimits.dailyPhInjectedMl;
  doc["orp_daily_ml"] = safetyLimits.dailyOrpInjectedMl;
  doc["ph_limit_reached"] = safetyLimits.phLimitReached;
  doc["orp_limit_reached"] = safetyLimits.orpLimitReached;
  doc["ph_inject_remaining_s"]  = manualInjectRemainingS(manualInjectPh);
  doc["orp_inject_remaining_s"] = manualInjectRemainingS(manualInjectOrp);

  // feature-075 : session de l'assistant de mesure du débit — LECTURE SEULE,
  // resynchronisation du compte à rebours client à l'échéance (pas de champ WS,
  // aucun nouveau rythme réseau : l'UI n'interroge /data qu'aux échéances).
  // Snapshot sans mutex (getFlowAssistStatus, patron manualInject*).
  {
    FlowAssistStatus fa = getFlowAssistStatus();
    JsonObject faObj = doc["flow_assist"].to<JsonObject>();
    faObj["active"]      = fa.active;
    faObj["pump"]        = fa.pumpNumber;     // 1|2 (0 = aucune session)
    faObj["phase"]       = fa.phase;          // "purge"|"measure"|"" (à l'arrêt)
    faObj["remaining_s"] = fa.remainingS;
    // Revue feature-075 : dernière marche interrompue par le SERVEUR (perte de
    // filtration) — l'UI ne doit JAMAIS router une mesure avortée vers la
    // saisie du volume (débit sous-estimé → sur-injection).
    faObj["aborted"]     = fa.aborted;
    faObj["purges_left"] = fa.purgesLeft;
    faObj["purge_s"]     = kFlowAssistPurgeDurationS;    // durées servies : l'UI
    faObj["measure_s"]   = kFlowAssistMeasureDurationS;  // paramètre ses libellés
    // Débit majorant de flotte à duty 255 — servi pour que l'UI annonce le coût
    // budget SANS miroir en dur (revue). Tableau [0] = pompe 1, [1] = pompe 2
    // CONSERVÉ tel quel (contrat UI inchangé) : depuis v2.25.2 la valeur est
    // unique pour les deux pompes (kPumpFlow255MlPerMin), mais casser la forme
    // du tableau ferait tomber l'UI sur son repli générique.
    JsonArray f255 = faObj["flow255"].to<JsonArray>();
    f255.add(kPumpFlow255MlPerMin);
    f255.add(kPumpFlow255MlPerMin);
  }

  doc["ph_tracking_enabled"]  = productCfg.phTrackingEnabled;
  doc["ph_remaining_ml"]      = max(0.0f, productCfg.phContainerVolumeMl  - productCfg.phTotalInjectedMl);
  doc["ph_alert_threshold_ml"]= productCfg.phAlertThresholdMl;
  doc["orp_tracking_enabled"] = productCfg.orpTrackingEnabled;
  doc["orp_remaining_ml"]     = max(0.0f, productCfg.orpContainerVolumeMl - productCfg.orpTotalInjectedMl);
  doc["orp_alert_threshold_ml"]= productCfg.orpAlertThresholdMl;

  // feature-096 (v2.37.0) : TEMPS D'ATTEINTE DE CIBLE — 8 champs servis depuis
  // l'instantané mémorisé de timeToTarget (copie sous spinlock portMUX, patron
  // measure_validity, sûre en contexte async_tcp). Ce chemin n'appelle JAMAIS
  // PumpController::getPhDecisionValue() : cet accesseur est réservé à loopTask
  // (condition C2 du gate feature-096).
  // PARITÉ /data ↔ WS obligatoire : MÊMES clés, MÊMES arrondis, MÊME règle
  // NaN → null que ws_manager::_buildSensorJson, publiées dans le MÊME passage
  // (un champ servi d'un seul côté ne disparaît pas, il CLIGNOTE :
  // `latestSensorData` est alimenté par le WS puis intégralement REMPLACÉ par
  // les charges /data toutes les ~30 s — 3 récidives documentées du motif).
  //   *_ttt_state = 0 pas de départ aujourd'hui / 1 en cours / 2 atteinte /
  //                 3 non atteinte (4-255 réservés)
  //   *_ttt_min   = minutes de FILTRATION (état 1 : écoulées ; 2 : temps
  //                 d'atteinte ; 3 : durée totale), bornées à 1440
  //   *_ttt_e0    = écart initial SIGNÉ à t0 (null si aucun départ) — pH à
  //                 2 décimales, ORP à 1 décimale
  //   *_ttt_flag  = CHAMP DE BITS, jamais une énumération : 0x01 consigne
  //                 modifiée, 0x02 boost, 0x04 purge au plafond, 0x08 reprise
  //                 après coupure secteur, 0x10 régulation non automatique.
  //                 Plusieurs causes peuvent être présentes SIMULTANÉMENT et
  //                 les bits 5-7 restent libres : le client teste des bits, il
  //                 ne compare jamais le champ à une valeur unique.
  {
    const TttSnapshot ttt = timeToTarget.getSnapshot();
    doc["ph_ttt_state"] = ttt.ph.state;
    doc["ph_ttt_min"]   = ttt.ph.minutes;
    if (!isnan(ttt.ph.e0)) doc["ph_ttt_e0"] = round(ttt.ph.e0 * 100.0f) / 100.0f; else doc["ph_ttt_e0"] = nullptr;
    doc["ph_ttt_flag"]  = ttt.ph.flags;
    doc["orp_ttt_state"] = ttt.orp.state;
    doc["orp_ttt_min"]   = ttt.orp.minutes;
    if (!isnan(ttt.orp.e0)) doc["orp_ttt_e0"] = round(ttt.orp.e0 * 10.0f) / 10.0f; else doc["orp_ttt_e0"] = nullptr;
    doc["orp_ttt_flag"]  = ttt.orp.flags;
  }

  time_t nowEpoch = time(nullptr);
  doc["time_synced"] = isTimeValid(nowEpoch);

  sendJsonResponse(request, doc);
}

// =============================================================================
// GET /get-kpi — mémoire 7 jours du temps d'atteinte de cible (feature-096)
// =============================================================================
// PROTECTION : RouteProtection::WRITE, la MÊME classe que /data. `NONE`
// exposerait sans authentification le comportement du bassin (heures de
// filtration, réactivité de la régulation, jours de maintenance) et rejouerait
// exactement bug-081.
//
// NON BLOQUANT : aucune lecture NVS, aucune E/S. copyHistory() recopie 188 o
// depuis un miroir RAM sous spinlock portMUX ; le reste est du tri sur un
// tableau de 14 dates en pile. Aucune allocation récurrente hors le document
// JSON (~800 o au pire cas).
//
// FORME : { "days": [ {"d":AAAAMMJJ, "ph":{...}, "orp":{...}}, ... ] }
//   - le PLUS RÉCENT EN TÊTE, au plus kTimeToTargetHistoryDays entrées ;
//   - clés courtes (s/m/e0/f) : la charge est lue par la machine, pas par un
//     humain, et 7 jours × 2 voies multiplient chaque octet de nom par 14 ;
//   - une voie SANS entrée pour la date est OMISE (absent ≠ « état 0 ») ;
//   - `e0` vaut null si la journée n'a pas de départ exploitable.
// Les deux voies sont archivées indépendamment : leurs dates peuvent différer.
// L'union des dates est donc calculée, triée décroissant, puis tronquée.
static void handleGetKpi(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  // C4 : NVS `pool-kpi` refusée ⇒ l'indicateur est suspendu. On le DIT (503 +
  // message), on ne rend JAMAIS un tableau vide qui se lirait « aucun jour
  // enregistré » alors que la mémoire est en panne.
  if (timeToTarget.isMemoryUnavailable()) {
    sendErrorResponse(request, 503, "Mémoire des indicateurs indisponible");
    return;
  }

  TttHistory h;
  timeToTarget.copyHistory(h);

  // Union des dates des deux voies (au plus 2 × 7), triée par ordre décroissant.
  // Tri par insertion : 14 éléments au maximum, aucune allocation, coût nul
  // devant le budget de 50 ms d'un handler async.
  uint32_t dates[kTttChannelCount * kTimeToTargetHistoryDays];
  uint8_t  nDates = 0;
  for (uint8_t c = 0; c < kTttChannelCount; ++c) {
    for (uint8_t i = 0; i < kTimeToTargetHistoryDays; ++i) {
      const uint32_t d = h.days[c][i].date;
      if (d == 0) continue;  // case vide
      bool seen = false;
      for (uint8_t k = 0; k < nDates; ++k) {
        if (dates[k] == d) { seen = true; break; }
      }
      if (seen) continue;
      uint8_t pos = nDates;
      while (pos > 0 && dates[pos - 1] < d) { dates[pos] = dates[pos - 1]; --pos; }
      dates[pos] = d;
      ++nDates;
    }
  }
  if (nDates > kTimeToTargetHistoryDays) nDates = kTimeToTargetHistoryDays;

  // Pire cas : 7 jours × 2 voies × ~48 o + enveloppe ≈ 800 o.
  StaticJson<1024> doc;
  JsonArray days = doc["days"].to<JsonArray>();
  const char* chanKey[kTttChannelCount] = {"ph", "orp"};
  // pH à 2 décimales, ORP à 1 : MÊMES arrondis que /data et le WS.
  const float e0Scale[kTttChannelCount] = {100.0f, 10.0f};
  for (uint8_t i = 0; i < nDates; ++i) {
    JsonObject day = days.add<JsonObject>();
    day["d"] = dates[i];
    for (uint8_t c = 0; c < kTttChannelCount; ++c) {
      const TttDay* e = tttHistoryFind(h, c, dates[i]);
      if (e == nullptr) continue;  // voie sans entrée ce jour-là : clé ABSENTE
      JsonObject o = day[chanKey[c]].to<JsonObject>();
      o["s"] = e->state;
      o["m"] = e->minutes;
      if (!isnan(e->e0)) o["e0"] = round(e->e0 * e0Scale[c]) / e0Scale[c];
      else               o["e0"] = nullptr;
      o["f"] = e->flags;  // CHAMP DE BITS (cf. /data) — jamais une énumération
    }
  }

  sendJsonResponse(request, doc);
}

// État partagé de l'export chunked /download-logs (capturé en shared_ptr par le
// callback — détruit à la fin de la réponse OU sur déconnexion client, ce qui
// ferme le File via son destructeur).
struct LogsDownloadState {
  enum Phase : uint8_t { kHeader, kFileOpen, kFileData, kFileEnd, kRamHeader, kRamEntries, kDone };
  Phase phase = kHeader;
  File file;                 // /system.log (partition history via systemLogger.getPersistenceFs())
  size_t ramIdx = 0;         // Index chronologique dans le buffer RAM du logger
  bool hasAbsoluteTime = false;
  long bootEpoch = 0;        // Epoch de démarrage (reconstruction des timestamps absolus)
  time_t nowEpoch = 0;
  unsigned long nowMs = 0;
  LogLineCarry<kLogExportLineMaxBytes> carry;  // Reliquat de ligne entre deux chunks
  // feature-129 (phase C) : tampon de copie pour Logger::copyEntryByIndex()
  // (le message ne pointe plus jamais dans sLogArena — condition C3). Champ de
  // LogsDownloadState (shared_ptr alloué UNE fois par requête) : jamais sur la
  // pile d'async_tcp, jamais par chunk. Dimension : kLogExportLineMaxBytes
  // (256, la constante qui borne déjà la ligne d'export).
  char buf[kLogExportLineMaxBytes];
};

// Charge une chaîne C (marqueurs de section) dans le carry.
static void logsCarryLoad(LogLineCarry<kLogExportLineMaxBytes>& c, const char* s) {
  size_t n = strlen(s);
  if (n > sizeof(c.line)) n = sizeof(c.line);
  memcpy(c.line, s, n);
  c.len = n;
  c.pos = 0;
}

// Formate le timestamp d'une entrée RAM (absolu si l'heure est synchronisée,
// sinon +HH:MM:SS depuis le boot).
static void logsFormatTime(const LogsDownloadState& st, unsigned long entryMs,
                           char* timeBuf, size_t timeBufLen) {
  if (st.hasAbsoluteTime) {
    time_t entryEpoch = st.bootEpoch + (long)(entryMs / 1000);
    struct tm t;
    localtime_r(&entryEpoch, &t);
    strftime(timeBuf, timeBufLen, "%Y-%m-%d %H:%M:%S", &t);
  } else {
    unsigned long totalSec = entryMs / 1000;
    snprintf(timeBuf, timeBufLen, "+%02lu:%02lu:%02lu",
             totalSec / 3600, (totalSec % 3600) / 60, totalSec % 60);
  }
}

static void handleDownloadLogs(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  // bug-crash-getlogs-badalloc : réponse CHUNKED — le callback remplit le buffer
  // TCP borné fourni par AsyncWebServer, jamais de cbuf contigu proportionnel à
  // la taille de l'export (l'ancien AsyncResponseStream accumulait tout : resize()
  // = new[] throwant → std::bad_alloc → abort). Aucune garde heap nécessaire.
  auto st = std::make_shared<LogsDownloadState>();
  st->nowEpoch = time(nullptr);
  st->nowMs = millis();
  st->hasAbsoluteTime = (st->nowEpoch > 1609459200L);
  st->bootEpoch = st->hasAbsoluteTime ? (long)(st->nowEpoch - st->nowMs / 1000) : 0;

  AsyncWebServerResponse* res = request->beginChunkedResponse(
    "text/plain; charset=utf-8",
    [st](uint8_t* buf, size_t maxLen, size_t /*index*/) -> size_t {
      // Machine à états : chaque appel émet au plus maxLen octets (< 50 ms :
      // écritures mémoire + lectures LittleFS par blocs). Retourne 0 UNIQUEMENT
      // quand tout est émis (0 = fin de réponse pour AsyncWebServer).
      size_t total = 0;
      while (total < maxLen && st->phase != LogsDownloadState::kDone) {
        // 1) Vider d'abord le reliquat de ligne en cours (garantie de progression)
        if (st->carry.pos < st->carry.len) {
          total += emitCarry(st->carry, buf + total, maxLen - total);
          continue;  // re-tester la place restante
        }
        // 2) Carry épuisé : produire le contenu suivant selon la phase
        switch (st->phase) {
          case LogsDownloadState::kHeader: {
            char exportTime[32];
            if (st->hasAbsoluteTime) {
              struct tm t;
              localtime_r(&st->nowEpoch, &t);
              strftime(exportTime, sizeof(exportTime), "%Y-%m-%d %H:%M:%S", &t);
            } else {
              snprintf(exportTime, sizeof(exportTime), "boot+%lus (heure non sync.)",
                       st->nowMs / 1000);
            }
            int n = snprintf(st->carry.line, sizeof(st->carry.line),
                             "# Pool Controller — Journal système\n"
                             "# Exporté le : %s\n"
                             "# Format: [YYYY-MM-DD HH:MM:SS] NIVEAU : message\n",
                             exportTime);
            st->carry.len = (n > 0) ? ((size_t)n < sizeof(st->carry.line)
                                           ? (size_t)n : sizeof(st->carry.line) - 1)
                                    : 0;
            st->carry.pos = 0;
            st->phase = LogsDownloadState::kFileOpen;
            break;
          }
          case LogsDownloadState::kFileOpen: {
            // Ouverture paresseuse du journal persistant (boots précédents).
            // bug-100 / ADR-0070 — CLASSE A (8 192) : on est sur `async_tcp`,
            // une réponse chunked est en vol, donc des pbuf lwip aussi. Le repli
            // existait déjà (→ kRamHeader = journal RAM seul) mais il était
            // SILENCIEUX : l'export ne disait pas qu'il était tronqué. Il le dit
            // désormais dans le fichier téléchargé — c'est là que le lecteur est.
            fs::FS* pfs = systemLogger.getPersistenceFs();
            if (pfs) {
              st->file = openIfHeapAllows(pfs, "/system.log", "r",
                                          kStaticFileMinAllocBytes);
            }
            if (st->file && st->file.size() > 0) {
              logsCarryLoad(st->carry, "\n# === Historique persistant ===\n");
              st->phase = LogsDownloadState::kFileData;
            } else {
              if (st->file) {
                st->file.close();
              } else if (pfs) {
                // Ni fichier absent ni fichier vide ne se distinguent d'un refus
                // ici : on annonce la troncature possible plutôt que de laisser
                // croire à un journal complet.
                sLogsExportFopenDeferrals.fetch_add(1, std::memory_order_relaxed);
                logsCarryLoad(st->carry,
                              "\n# === Historique persistant INDISPONIBLE ===\n"
                              "# (fichier absent, ou memoire insuffisante pour l'ouvrir :\n"
                              "#  export TRONQUE a la session courante)\n");
              }
              st->phase = LogsDownloadState::kRamHeader;
            }
            break;
          }
          case LogsDownloadState::kFileData: {
            // Copie par blocs directement dans le buffer TCP (hors mutex logger)
            size_t n = st->file.available()
                           ? st->file.read(buf + total, maxLen - total) : 0;
            if (n > 0) {
              total += n;
            } else {
              st->file.close();
              st->phase = LogsDownloadState::kFileEnd;
            }
            break;
          }
          case LogsDownloadState::kFileEnd:
            logsCarryLoad(st->carry, "# === Fin historique persistant ===\n");
            st->phase = LogsDownloadState::kRamHeader;
            break;
          case LogsDownloadState::kRamHeader:
            logsCarryLoad(st->carry, "\n# === Session courante ===\n");
            st->phase = LogsDownloadState::kRamEntries;
            break;
          case LogsDownloadState::kRamEntries: {
            // Une entrée RAM par itération, copiée sous mutex DANS st->buf
            // (feature-129 : le message ne pointe plus jamais dans sLogArena,
            // condition C3). false = fin du buffer OU timeout mutex (export
            // best-effort) ; le ring peut tourner entre deux chunks
            // (doublon/saut tolérés).
            LogRecord entry;
            if (systemLogger.copyEntryByIndex(st->ramIdx, entry, st->buf, sizeof(st->buf))) {
              st->ramIdx++;
              char timeBuf[24];
              logsFormatTime(*st, entry.timestamp, timeBuf, sizeof(timeBuf));
              int n = snprintf(st->carry.line, sizeof(st->carry.line),
                               "[%s] %s : %s\n", timeBuf,
                               logLevelName(entry.level),
                               entry.message);
              if (n < 0) {
                st->carry.len = 0;
              } else if ((size_t)n >= sizeof(st->carry.line)) {
                // Ligne trop longue : tronquée avec « … » (3 octets UTF-8) + \n.
                // Une séquence UTF-8 du message peut être coupée — cosmétique.
                size_t len = sizeof(st->carry.line) - 1;
                memcpy(&st->carry.line[len - 4], "\xE2\x80\xA6\n", 4);
                st->carry.len = len;
              } else {
                st->carry.len = (size_t)n;
              }
              st->carry.pos = 0;
            } else {
              st->phase = LogsDownloadState::kDone;
            }
            break;
          }
          default:
            st->phase = LogsDownloadState::kDone;
            break;
        }
      }
      return total;
    });
  res->addHeader("Content-Disposition", "attachment; filename=\"pool_logs.txt\"");
  res->addHeader("Cache-Control", "no-cache");
  request->send(res);
}

static void handleGetLogs(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  // Support paramètre optionnel ?since=TIMESTAMP pour récupération incrémentale
  unsigned long sinceTimestamp = 0;
  if (request->hasParam("since")) {
    String sinceParam = request->getParam("since")->value();
    sinceTimestamp = sinceParam.toInt();
  }

  // bug-crash-getlogs-badalloc : page BORNÉE construite en JsonDocument (une seule
  // passe sous le mutex logger), puis réponse pré-dimensionnée. Contrat réel du
  // cbuf d'AsyncResponseStream : resize() = new char[] THROWANT (2 coredumps le
  // prouvent — le commentaire v2.8.2 « realloc, jamais d'abort » était faux),
  // d'où pagination (~4 Ko max) + measureJson + garde heap AVANT toute allocation.
  // Le client boucle avec ?since=<dernier timestamp> tant que truncated == true.
  JsonDocument doc;
  doc["uptime_ms"] = millis();
  JsonArray arr = doc["logs"].to<JsonArray>();

  bool truncated = false;
  size_t included = 0;
  unsigned long lastTs = 0;
  systemLogger.forEachRecentLog(kMaxLogEntries, sinceTimestamp,
    [&arr, &truncated, &included, &lastTs](const LogArenaView& entry) -> bool {
      LogsPageDecision d = logsPageAdmit(included, entry.timestamp, lastTs,
                                         kLogsMaxEntriesPerPage, kLogsPageHardCap);
      if (d.include) {
        JsonObject o = arr.add<JsonObject>();
        o["timestamp"] = entry.timestamp;
        // feature-130 (ADR-0082) : littéral statique stocké par pointeur, pas
        // de String temporaire (voir logLevelName()).
        o["level"] = logLevelName(entry.level);
        // COPIE OBLIGATOIRE (feature-129, danger silencieux) : entry.message
        // pointe DANS sLogArena, valide SOUS CE MUTEX SEULEMENT (on est dans
        // le visiteur de forEachRecentLog). ArduinoJson stocke un `const
        // char*` PAR RÉFÉRENCE — `o["message"] = entry.message` lirait un
        // pointeur périmé au moment de serializeJson(), APRÈS la libération
        // du mutex. JsonString(ptr, len) avec isStatic=false (défaut) force
        // la copie IMMÉDIATE, pendant que le pointeur est encore valide.
        o["message"] = JsonString(entry.message, entry.len);
        o["trunc"] = (entry.flags & kLogFlagTruncated) != 0;
        included++;
        lastTs = entry.timestamp;
      }
      if (d.stop) {
        truncated = true;  // il reste des entrées au-delà de cette page
        return false;      // arrêt anticipé de l'itération
      }
      return true;
    });
  doc["truncated"] = truncated;

  // ORDRE IMPÉRATIF : measureJson → garde heap → beginResponseStream pré-dimensionné
  // → serializeJson → send. Ne RIEN écrire d'autre dans le stream.
  // bug-080 (v2.33.0) : la garde compare au plus gros bloc RÉELLEMENT allouable
  // (8 bits) — la métrique brute rendait le 503 préventif inatteignable.
  size_t sz = measureJson(doc);
  if (sz + kHttpHeapMarginBytes > heapLargestFreeBlock8Bit()) {
    request->send(503, "application/json", "{\"error\":\"heap fragmenté, réessayez\"}");
    return;
  }
  AsyncResponseStream* res = request->beginResponseStream("application/json", sz);
  serializeJson(doc, *res);
  request->send(res);
}

// État partagé de l'export chunked /get-history (bug-heap-toctou A3, patron
// LogsDownloadState) : capturé en shared_ptr par le callback, détruit à la fin de
// la réponse OU sur déconnexion client. Contrat JSON INCHANGÉ octet pour octet :
// {"range":"…","count":0,"history":[…],"count":N} — la clé "count" est émise DEUX
// fois (0 dans l'en-tête, valeur réelle dans le pied), comme l'ancienne route.
struct HistoryDownloadState {
  enum Phase : uint8_t { kHeader, kPoints, kFooter, kDone };
  Phase phase = kHeader;
  size_t idx = 0;             // Index dans l'instantané `pts`
  unsigned long cutoff = 0;   // Borne basse ?range= (0 = tout) — historyRangeCutoff
  unsigned long since = 0;    // Borne basse ?since= (0 = désactivé)
  size_t count = 0;           // Points réellement émis (pied de réponse)
  bool first = true;          // Virgule de séparation entre points

  // bug-history-doublons-troncature (itération 2) : INSTANTANÉ des points, copié
  // en UNE prise de mutex à l'ouverture de la requête. Le callback chunked ne
  // touche plus jamais le mutex → plus de timeouts en cours de flux (les
  // micro-chunks de maintien d'un octet interagissaient mal avec la couche
  // d'envoi de la lib : pied perdu, JSON tronqué — démontré en production),
  // plus de glissement d'index pendant une consolidation. Coût borné :
  // ≤ 339 points × sizeof(DataPoint) — 28 o depuis feature-095 (2 floats de plus)
  // contre 20 auparavant, soit ~9,3 Ko d'un SEUL bloc contigu (reserve dimensionné
  // sur la taille du buffer, donc majorant : aucune réallocation possible),
  // libérés à la fin de la réponse (l'ancienne route épinglait ~40 Ko de String
  // non bornée — cf. ADR-0032).
  std::vector<DataPoint> pts;
  // Comptabilité d'émission, loggée en DEBUG à la clôture — instrumentation
  // née du diagnostic de la cause C (pied jamais émis) et conservée : c'est
  // elle qui permet de confronter l'émis serveur aux octets reçus client.
  size_t sentBytes = 0;
  // feature-095 : points écartés parce que leur JSON dépasse la capacité du
  // carry (kHistoryPointJsonMaxBytes). Comptés ici, journalisés UNE fois à la
  // clôture — sans cela, la perte est totalement muette (JSON valide, trou de
  // courbe indiscernable d'une panne de sonde).
  size_t skipped = 0;
  bool endLogged = false;
  String range;               // Écho du paramètre ?range= (en-tête)
  LogLineCarry<kHistoryPointJsonMaxBytes> carry;  // Reliquat en-tête/point/pied entre deux chunks
};

// Charge une chaîne formatée bornée dans le carry (en-tête / pied de réponse).
static void historyCarryLoadf(LogLineCarry<kHistoryPointJsonMaxBytes>& c,
                              const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(c.line, sizeof(c.line), fmt, args);
  va_end(args);
  c.len = (n > 0) ? ((size_t)n < sizeof(c.line) ? (size_t)n : sizeof(c.line) - 1) : 0;
  c.pos = 0;
}

static void handleGetHistory(AsyncWebServerRequest* request) {
  REQUIRE_AUTH(request, RouteProtection::WRITE);

  // Support paramètre optionnel ?range=24h|3d|7d|30d|all
  String rangeParam = "all";
  if (request->hasParam("range")) {
    rangeParam = request->getParam("range")->value();
  }
  // Revue 2.14.0 : seul le TOKEN RECONNU est échoé dans l'en-tête JSON (jamais
  // la valeur brute — une valeur longue ou avec guillemet malformerait le JSON
  // dans le carry borné). Valeur inconnue → traitée et échoée comme "all".
  const char* range = "all";
  int hours = 0;  // 0 = "all" (aucun filtre temporel)
  if (rangeParam == "24h")      { range = "24h"; hours = 24; }
  else if (rangeParam == "3d")  { range = "3d";  hours = 24 * 3; }
  else if (rangeParam == "7d")  { range = "7d";  hours = 24 * 7; }
  else if (rangeParam == "30d") { range = "30d"; hours = 24 * 30; }

  // Réponse CHUNKED (bug-heap-toctou A3, ADR-0032 mécanisme b) streamée depuis
  // un INSTANTANÉ filtré copié en UNE prise de mutex (snapshotFiltered, cf.
  // bug-history-doublons-troncature itération 2) : ~11 Ko bornés libérés en fin
  // de réponse, contre ~40 Ko de String non bornée avant v2.14. Le callback ne
  // prend AUCUN mutex. NOTE : migrateLegacyHistory n'est plus appelé sur ce
  // chemin (l'ancien getLastHours le faisait) — couvert par update()/load/import.
  // Epoch : time(nullptr) si valide, sinon 0 → cutoff 0 = tout renvoyer (l'estimation
  // lastKnownEpoch de getLastHours n'est pas accessible ici ; superset toléré).
  auto st = std::make_shared<HistoryDownloadState>();
  st->range = range;
  time_t nowT = time(nullptr);
  st->cutoff = historyRangeCutoff(hours, isTimeValid(nowT) ? (unsigned long)nowT : 0);
  if (request->hasParam("since")) {
    st->since = request->getParam("since")->value().toInt();
  }

  // bug-history-doublons-troncature (itération 2) : instantané filtré en UNE
  // prise de mutex — le callback ne verrouille plus rien ensuite (cf. état).
  if (!history.snapshotFiltered(st->pts, st->cutoff, st->since)) {
    request->send(503, "application/json",
                  "{\"error\":\"historique occupé, réessayez\"}");
    return;
  }

  AsyncWebServerResponse* res = request->beginChunkedResponse(
    "application/json",
    [st](uint8_t* buf, size_t maxLen, size_t /*index*/) -> size_t {
      // Machine à états : chaque appel émet au plus maxLen octets, depuis
      // l'instantané st->pts (aucun mutex, aucun timeout possible). Retourne 0
      // UNIQUEMENT quand carry vide ET phase kDone (fin de réponse).
      size_t total = 0;
      // bug-history-doublons-troncature (cause racine C, v2.14.0) : la boucle
      // doit continuer tant que le carry n'est pas vide MÊME en phase kDone —
      // le cas kFooter charge le pied dans le carry ET passe en kDone dans la
      // même itération ; avec l'ancienne condition (phase != kDone seul), la
      // boucle sortait AVANT d'émettre le carry : le pied ],"count":N} n'était
      // JAMAIS envoyé → JSON tronqué sur TOUTES les réponses depuis v2.14.0
      // (prouvé au flux brut : chunks = exactement l'émis SANS pied).
      while (total < maxLen &&
             (st->carry.pos < st->carry.len ||
              st->phase != HistoryDownloadState::kDone)) {
        // 1) Vider d'abord le reliquat en cours (garantie de progression)
        if (st->carry.pos < st->carry.len) {
          total += emitCarry(st->carry, buf + total, maxLen - total);
          continue;  // re-tester la place restante
        }
        // 2) Carry épuisé : produire le contenu suivant selon la phase
        switch (st->phase) {
          case HistoryDownloadState::kHeader:
            historyCarryLoadf(st->carry, "{\"range\":\"%s\",\"count\":0,\"history\":[",
                              st->range.c_str());
            st->phase = HistoryDownloadState::kPoints;
            break;
          case HistoryDownloadState::kPoints: {
            // Streaming depuis l'INSTANTANÉ (filtres déjà appliqués à la copie) :
            // aucun mutex, aucun timeout, aucun glissement d'index possible.
            if (st->idx < st->pts.size()) {
              const DataPoint& p = st->pts[st->idx];
              st->idx++;
              size_t n = formatHistoryPointJson(
                  st->carry.line, sizeof(st->carry.line), p.timestamp, p.ph, p.orp,
                  p.temperature, p.filtrationActive, p.doseMask,
                  static_cast<uint8_t>(p.granularity), st->first,
                  p.phFiltered, p.orpFiltered);
              if (n > 0) {
                st->carry.len = n;
                st->carry.pos = 0;
                st->first = false;
                st->count++;
              }
              // n == 0 : point plus long que kHistoryPointJsonMaxBytes (pire cas
              // absolu re-mesuré en bug-098 : 161 o pour 192 de capacité, et
              // 157 o pour le pire cas atteignable — décompte de constants.h) →
              // point SAUTÉ, jamais de JSON partiel. Compté (jamais journalisé
              // ici : un export en sauterait des centaines) — un unique WARNING
              // throttlé est émis à la clôture.
              else {
                st->skipped++;
              }
              break;
            }
            st->phase = HistoryDownloadState::kFooter;  // fin de l'instantané
            break;
          }
          case HistoryDownloadState::kFooter:
            historyCarryLoadf(st->carry, "],\"count\":%u}", (unsigned)st->count);
            st->phase = HistoryDownloadState::kDone;
            break;
          default:
            st->phase = HistoryDownloadState::kDone;
            break;
        }
      }
      // Trace de clôture (DEBUG) : l'émis réel de la machine à états, à
      // confronter aux octets reçus côté client en cas de doute.
      st->sentBytes += total;
      if (st->phase == HistoryDownloadState::kDone &&
          st->carry.pos >= st->carry.len && !st->endLogged) {
        st->endLogged = true;
        // feature-095 : perte de points rendue AUDIBLE. Un seul WARNING par
        // réponse, throttlé entre réponses (kHistorySkipWarnThrottleMs) —
        // message actionnable : combien, et la constante à relever. snprintf
        // sur tampon de pile, pas de String construite dans le callback chunké.
        if (st->skipped > 0) {
          static unsigned long sWarnSkipMs = 0;
          unsigned long nowMs = millis();
          if (sWarnSkipMs == 0 || nowMs - sWarnSkipMs >= kHistorySkipWarnThrottleMs) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "get-history : %u point(s) omis de la réponse — JSON du point "
                     "au-dessus de kHistoryPointJsonMaxBytes (%u o) ; courbes trouées, "
                     "relever la constante",
                     (unsigned)st->skipped, (unsigned)kHistoryPointJsonMaxBytes);
            systemLogger.warning(msg);
            sWarnSkipMs = nowMs;
          }
        }
        systemLogger.debug("get-history clos: emis=" + String((unsigned)st->sentBytes) +
                           " o, points=" + String((unsigned)st->count) +
                           ", idx=" + String((unsigned)st->idx) +
                           ", dernierAppel=" + String((unsigned)total) + " o");
      }
      return total;
    });
  // Aucun header additionnel : l'ancienne route (sendRawJsonResponse) n'en posait
  // pas — seul le Transfer-Encoding change (chunked, transparent pour fetch()).
  request->send(res);
}

void setupDataRoutes(AsyncWebServer* server) {
  server->on("/data", HTTP_GET, handleGetData);
  server->on("/get-kpi", HTTP_GET, handleGetKpi);  // feature-096 (v2.37.0)
  server->on("/get-logs", HTTP_GET, handleGetLogs);
  server->on("/download-logs", HTTP_GET, handleDownloadLogs);
  server->on("/logs", HTTP_DELETE, [](AsyncWebServerRequest* request) {
    REQUIRE_AUTH(request, RouteProtection::WRITE);
    systemLogger.clearAll();
    systemLogger.info("Logs effacés (RAM + fichier persistant)");
    JsonDocument doc;
    doc["success"] = true;
    sendJsonResponse(request, doc);
  });
  server->on("/get-history", HTTP_GET, handleGetHistory);

  auto* importHandler = new AsyncCallbackJsonWebHandler(
    "/history/import",
    [](AsyncWebServerRequest* request, JsonVariant& json) {
      REQUIRE_AUTH(request, RouteProtection::WRITE);

      JsonObject root = json.as<JsonObject>();
      if (!root["history"].is<JsonArray>()) {
        sendErrorResponse(request, 400, "Format invalide: champ history manquant");
        return;
      }

      JsonArray items = root["history"].as<JsonArray>();
      if (items.isNull() || items.size() == 0) {
        sendErrorResponse(request, 400, "Historique vide");
        return;
      }

      std::vector<DataPoint> imported;
      imported.reserve(items.size());

      for (JsonObject item : items) {
        if (!item["timestamp"].is<unsigned long>()) {
          continue;
        }
        unsigned long ts = item["timestamp"] | 0;
        if (ts == 0) {
          continue;
        }

        DataPoint dp;
        dp.timestamp = ts;
        dp.ph = item["ph"].isNull() ? NAN : item["ph"].as<float>();
        dp.orp = item["orp"].isNull() ? NAN : item["orp"].as<float>();
        dp.temperature = item["temperature"].isNull() ? NAN : item["temperature"].as<float>();
        // feature-095 : clés LONGUES, celles de l'export /get-history (le fichier
        // persisté, lui, utilise les clés courtes pf/of — ne pas confondre les
        // deux formats). Sans cette relecture, restaurer une sauvegarde perdrait
        // la filtrée en silence. Absent ou null → NaN (absence), jamais 0.
        dp.phFiltered = item["phFiltered"].isNull() ? NAN : item["phFiltered"].as<float>();
        dp.orpFiltered = item["orpFiltered"].isNull() ? NAN : item["orpFiltered"].as<float>();
        dp.filtrationActive = item["filtration"] | false;
        // bug-098 — PIEGE VERIFIE : `item["dosing"] | false` rend `false` sur
        // un ENTIER (l'operateur | d'ArduinoJson exige la CONCORDANCE de type),
        // il aurait donc efface tout masque relu d'un export neuf. Lecture
        // TYPEE, meme discriminant que loadFromFile : booleen = export <= 2.46
        // (dosage, voie indeterminee), entier = masque, absent = aucun dosage.
        // MEME LIMITE ASSUMEE qu'en relecture du fichier : un `"dosing"`
        // flottant ou 64 bits hors plage `int` retombe sur 0 (cf. history.cpp).
        JsonVariant dosingVar = item["dosing"];
        if (dosingVar.is<bool>()) {
          dp.doseMask = dosingVar.as<bool>() ? kHistDoseUnknownChannel : 0;
        } else if (dosingVar.is<int>()) {
          dp.doseMask = (uint8_t)(dosingVar.as<unsigned int>() &
                                  (kHistDosePh | kHistDoseOrp |
                                   kHistDoseUnknownChannel));
        } else {
          dp.doseMask = 0;
        }
        uint8_t granularity = item["granularity"] | 0;
        dp.granularity = granularity <= DAILY ? static_cast<Granularity>(granularity) : RAW;

        imported.push_back(dp);
      }

      if (imported.empty()) {
        sendErrorResponse(request, 400, "Aucune donnée valide à importer");
        return;
      }

      // bug-101 — le champ "mode" décide, et son ABSENCE vaut "replace" : c'est
      // la rétrocompatibilité stricte de la route (tout client antérieur à
      // 2.43.0 postait un remplacement total sans le savoir). Toute valeur
      // inconnue vaut également "replace" — jamais une fusion silencieuse d'un
      // client qu'on n'a pas compris. Cf. historyImportModeIsReplace().
      JsonVariant modeVar = root["mode"];
      const char* mode = modeVar.is<const char*>() ? modeVar.as<const char*>() : nullptr;
      // `total` en mode remplacement = le nombre de points SOUMIS. La fusion de
      // doublons opérée par importData() peut le réduire ; on ne le mesure pas
      // ici parce qu'il faudrait copier tout le tampon (~11 Ko) sur la tâche
      // async_tcp, exactement là où bug-080 documente la famine. Le client
      // conclut de toute façon sur le DERNIER lot, qui est un "append" — donc
      // sur un total authentique.
      size_t total = imported.size();
      size_t dropped = 0;
      bool ok = historyImportModeIsReplace(mode)
                  ? history.importData(imported)
                  : history.appendData(imported, &total, &dropped);
      if (!ok) {
        sendErrorResponse(request, 500, "Impossible d'importer l'historique");
        return;
      }

      JsonDocument doc;
      doc["status"] = "success";
      doc["count"] = imported.size();
      // `total` = points détenus par l'appareil après ce lot.
      //
      // ⚠️ `dropped` = points ÉVINCÉS DE L'HISTORIQUE par trimToCaps() pendant
      // cet appel — les PLUS ANCIENS de chaque granularité au-delà du plafond.
      // Ce n'est PAS « les points de ce lot qui ont été refusés », et les deux
      // ne coïncident pas par construction : importer 20 points récents dans un
      // historique déjà plein donne `dropped = 20`, mais ce sont 20 points
      // ANCIENS DÉJÀ PRÉSENTS qui partent, pas les entrants. La règle de
      // collision d'appendData() étant l'écrasement, un entrant n'est pas
      // écarté par la FUSION : il entre, puis le plafond décide qui sort.
      // (⚠️ à ne pas lire comme « aucun entrant n'est jamais écarté » : la
      // boucle de désérialisation ci-dessus fait `continue` sur un timestamp
      // invalide, et ces points ne sont comptés nulle part — `count` est le
      // nombre de points RETENUS. Défaut préexistant, hors périmètre.)
      // Définition de référence : doxygen d'HistoryManager::appendData
      // dans history.h.
      //
      // En mode remplacement il vaut 0 parce qu'importData() n'évince pas :
      // il substitue le tampon et fusionne les doublons agrégés.
      //
      // Rapporté TOUJOURS, même à zéro : la perte muette est le défaut
      // d'origine de bug-101.
      doc["total"] = total;
      doc["dropped"] = dropped;
      sendJsonResponse(request, doc);
    });
  importHandler->setMethod(HTTP_POST);
  // bug-101 / bug-113 — ORDRE SIGNIFICATIF, ne pas permuter ces deux lignes.
  // AsyncWebServer::_handleRequest retient le PREMIER handler dont filter() et
  // canHandle() passent (WebServer.cpp:147) : le garde doit donc être enregistré
  // AVANT importHandler pour intercepter les corps hors gabarit et rendre un JSON
  // maison (413/503) au lieu du 413 nu de la bibliothèque.
  registerJsonBodyGuard(server, "/history/import", kHistoryImportMaxBodyBytes);
  importHandler->setMaxContentLength(kHistoryImportMaxBodyBytes);
  server->addHandler(importHandler);

  server->on("/history/clear", HTTP_POST, [](AsyncWebServerRequest* request) {
    REQUIRE_AUTH(request, RouteProtection::WRITE);
    // feature-027 : clearHistory() peut échouer si le mutex historique est occupé
    if (!history.clearHistory()) {
      sendErrorResponse(request, 503, "Historique occupé — réessayer");
      return;
    }

    JsonDocument doc;
    doc["status"] = "success";
    sendJsonResponse(request, doc);
  });
}
