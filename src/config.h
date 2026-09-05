#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "dosing_logic.h"  // feature-056 : enum InstallMode (logique pure partagée)
#include "constants.h"     // feature-074 : kPumpTypePulseFlowMlPerMin (calibration de type)
#include "ph_persist_logic.h"  // feature-084 : PhIntegralSnapshot (logique PURE)
#include "regulation_defaults.h"  // feature-088 : valeurs d'usine de régulation (source UNIQUE)
#include "time_to_target_logic.h"  // feature-096 : TttHistory (logique PURE)

// ==== Configuration des canaux PWM pompes ====
// MOSFET IRLZ44N - PWM sur Gate, pins définis dans constants.h (PCB v2)
#define PUMP1_CHANNEL 0
#define PUMP2_CHANNEL 1
#define PUMP_PWM_FREQ 20000   // 20kHz PWM
#define PUMP_PWM_RES_BITS 8  // 8-bit resolution (0-255)

// ==== Constantes ====
constexpr float PH_DEADBAND = 0.01f;      // Zone morte réduite : 7.2 à 7.21 (±0.01)
constexpr float ORP_DEADBAND = 2.0f;      // Zone morte réduite : ±2mV
constexpr uint8_t MAX_PWM_DUTY = (1 << PUMP_PWM_RES_BITS) - 1;
// feature-074 : amplitude d'impulsion par défaut (gelée à 50 % — l'UI ne la
// modifie plus, le champ reste vivant côté API pour cas exceptionnel).
constexpr uint8_t kPumpAmpDefaultPct = 50;
// Duty de la calibration DE TYPE. Condition C2 du gate pool-chemistry : MÊME
// expression ENTIÈRE que getPumpAmplitudeDuty ((pct * MAX_PWM_DUTY) / 100) pour
// que l'égalité exacte amplitude == duty calibré du gate d'impulsion soit
// garantie par construction (pas de divergence float/arrondi). = 127 à 50 %.
constexpr uint8_t kPumpTypeCalDuty = (kPumpAmpDefaultPct * MAX_PWM_DUTY) / 100;
// Plancher de rotation (bug-dosage-fantome itération 2) : Dmin MESURÉ au banc
// le 2026-08-23 = 82 (plus petit duty à rotation continue et franche), + 16
// counts de marge (prescription P2 du gate pool-chemistry — couple de décollage
// à froid observé au terrain à 82). Toute amplitude sous ce plancher est
// REFUSÉE (condition B2, PulseRefusal::AmplitudeBelowMinActive), jamais clampée.
constexpr uint8_t MIN_ACTIVE_DUTY = 98;
// Verrous compile-time (test-engineer, feature-074) : la calibration de type
// vaut EXACTEMENT 127 (sinon l'égalité exacte du gate d'impulsion casserait au
// premier flash d'un appareil vierge) et reste au-dessus du plancher de rotation.
static_assert(kPumpTypeCalDuty == 127,
              "kPumpTypeCalDuty doit valoir 127 (50 % de 255, division entiere)");
static_assert(kPumpTypeCalDuty >= MIN_ACTIVE_DUTY,
              "kPumpTypeCalDuty sous le plancher de rotation MIN_ACTIVE_DUTY");

// ==== Home Assistant ====
const char* const HA_DEVICE_ID = "poolcontroller";
const char* const HA_DEVICE_NAME = "Pool Controller";
const char* const HA_DISCOVERY_PREFIX = "homeassistant";

// ==== Structures de configuration ====
struct MqttConfig {
  String server = "192.168.1.10";
  int port = 1883;
  String topic = "pool/sensors";
  String username = "";
  String password = "";
  bool enabled = false;
  // feature-088 : les VALEURS D'USINE de régulation vivent dans
  // regulation_defaults.h (source UNIQUE, avec leurs justifications v2.30.0) —
  // partagées entre ces initialiseurs, restoreRegulationDefaults() et l'objet
  // `regulation_defaults` de /get-config.
  float phTarget = kDefaultPhTarget;
  float orpTarget = kDefaultOrpTarget;
  bool phEnabled = false;
  bool orpEnabled = false;
  int phPump = 1;
  int orpPump = 2;
  // bug-dosage-fantome itération 2 (option B+C) : pumpNMaxDutyPct définit
  // désormais l'AMPLITUDE du pouls (duty du plateau = pct × 255 / 100) — la
  // pompe ne tourne qu'à cette amplitude, le débit moyen est réalisé par la
  // fraction de temps de marche (proportionnel de temps, condition B1 : ce
  // n'est plus un clamp PWM appliqué en sortie de régulation).
  uint8_t pump1MaxDutyPct = kPumpAmpDefaultPct;  // Amplitude pompe 1 (% → duty plateau, 50 % = 127)
  uint8_t pump2MaxDutyPct = kPumpAmpDefaultPct;  // Amplitude pompe 2 (% → duty plateau)
  // Caractérisation des pompes. feature-074 (contexte flotte) : les défauts
  // passent de « non caractérisé » (0, fail-closed) à la CALIBRATION DE TYPE
  // (valeur usine du modèle de pompe, kPumpTypeCalDuty/kPumpTypePulseFlowMlPerMin)
  // — un appareil NEUF dose dès le premier flash. calibratedDuty = duty auquel
  // le débit s'applique ; 0 = effacement explicite / NVS héritée jamais calibrée
  // → TOUT dosage automatique/programmé REFUSÉ (fail-closed,
  // PulseRefusal::FlowUncharacterized) — cette machinerie 2.21.0 est conservée.
  // La pesée individuelle (raffinement PAR pompe) reste possible et pose le
  // drapeau pumpNFlowWeighed.
  uint8_t pump1CalibratedDuty = kPumpTypeCalDuty;  // Duty calibré pompe 1 (0 = non caractérisé)
  uint8_t pump2CalibratedDuty = kPumpTypeCalDuty;  // Duty calibré pompe 2 (0 = non caractérisé)
  float pump1PulseFlowMlPerMin = kPumpTypePulseFlowMlPerMin;  // Débit (ml/min) au duty calibré, pompe 1
  float pump2PulseFlowMlPerMin = kPumpTypePulseFlowMlPerMin;  // Débit (ml/min) au duty calibré, pompe 2
  // feature-074 : distinction VALEUR USINE (calibration de type, false) /
  // PESÉE INDIVIDUELLE (l'utilisateur a pesé CETTE pompe, true). Purement
  // informatif (affichage « usine » / « pesé ») — ne participe à aucun gate.
  bool pump1FlowWeighed = false;   // true = débit pompe 1 issu d'une pesée individuelle
  bool pump2FlowWeighed = false;   // true = débit pompe 2 issu d'une pesée individuelle
  // feature-088 : justification du 10 min/h (v2.30.0, feature-087) déménagée
  // avec les valeurs dans regulation_defaults.h.
  int phInjectionLimitMinutes = kDefaultPhInjectionLimitMin;    // Max min d'injection pH / fenêtre d'1 h
  int orpInjectionLimitMinutes = kDefaultOrpInjectionLimitMin;  // Max min d'injection ORP / fenêtre d'1 h
  // feature-056 : mode d'installation (remplace regulationMode ET
  // filtrationCfg.enabled). Décrit le câblage réel et pilote la présence d'eau,
  // le relais filtration et l'horizon de répartition. Défaut = Managed (ancien
  // "pilote" + filtration gérée).
  InstallMode installMode = InstallMode::ManagedFiltration;
  int stabilizationDelayMin = kDefaultStabilizationDelayMin;  // Délai de stabilisation avant dosage (minutes)
  String regulationSpeed = kDefaultRegulationSpeed;  // Vitesse de correction PID : "slow", "normal", "fast"
  // feature-090 : taille du bassin. INVARIANT ABSOLU : elle ne sert qu'à
  // PRÉSÉLECTIONNER les défauts de régulation (regulation_defaults.h) — elle
  // n'entre JAMAIS dans un calcul de dosage, de débit ou de quota. Défaut
  // Medium = colonne des défauts v2.30.0 (clé NVS pool_size absente sur la
  // flotte existante ⇒ zéro changement de comportement à la mise à jour ;
  // AUCUNE application de défauts au boot, la lecture ne fait que charger le champ).
  PoolSize poolSize = PoolSize::Medium;
  String phCorrectionType = "ph_minus";  // "ph_minus" (acide) ou "ph_plus" (base)
  String phRegulationMode = "automatic"; // "automatic" / "scheduled" / "manual"
  int phDailyTargetMl = kDefaultPhDailyTargetMl;      // Volume quotidien cible (mL) pour le mode programmée
  String orpRegulationMode = "automatic"; // "automatic" / "scheduled" / "manual"
  int orpDailyTargetMl = kDefaultOrpDailyTargetMl;    // Volume quotidien cible (mL) pour le mode Programmée ORP
  bool timeUseNtp = true;
  String ntpServer = "pool.ntp.org";
  String manualTimeIso = "";
  String timezoneId = "europe_paris";

  // feature-021 (Pass 4a) : calibration pH/ORP entièrement déléguée aux modules
  // Atlas EZO (commande Cal,? + cache _phCalCachedPoints / _orpCalCachedPoints
  // dans SensorManager). Aucun champ de calibration pH/ORP n'est plus persisté
  // en NVS côté ESP32. Les anciens champs phCalibrationDate/Temp,
  // orpCalibrationOffset/Slope/Date/Reference/Temp ont été supprimés.

  // Calibration Température DS18B20
  // Formule appliquée: Temp_final = Temp_brut + offset
  float tempCalibrationOffset = 0.0f; // Offset de calibration température (°C)
  String tempCalibrationDate = "";    // Date de calibration température (ISO 8601)
  bool temperatureEnabled = true;     // Fonction température activée/désactivée

  // v2.20.3 (bug-filtre-ph-verrouille, C4) : TRACE PERSISTÉE de la bascule
  // AUTOMATIQUE de la régulation pH en `manual` (escalade feature-067 : N refus
  // de plancher acide en 24 h). Sans cette trace, l'incident du 2026-08-16 était
  // INDIAGNOSTICABLE : le mode était sur `manual` sans qu'aucun journal résiduel
  // ne permette de dire si c'était l'escalade automatique ou une action humaine.
  //   * phAutoManualEpoch  : epoch UNIX de la bascule (0 = aucune bascille auto
  //     en cours ; c'est aussi le défaut sur une NVS existante sans la clé →
  //     rétrocompatible, lecture d'un firmware antérieur inchangée).
  //   * phAutoManualReason : cause en clair (français), vide si epoch == 0.
  // Posés par pump_controller au moment de la bascule, EFFACÉS dès que
  // l'utilisateur reprend la main sur le mode (route HTTP ou commande MQTT).
  // INVARIANT INCHANGÉ : `phEnabled` reste le miroir de `phRegulationMode != "manual"`.
  uint32_t phAutoManualEpoch = 0;     // 0 = pas de bascule automatique enregistrée
  String phAutoManualReason = "";     // Cause de la bascule (vide si epoch == 0)

  // feature-067 : offset logiciel de mesure pH (unités pH, borné
  // [kPhOffsetMin, kPhOffsetMax]). Formule : pH_corrigé = pH_brut_EZO + phOffset.
  // Appliqué EN SORTIE des accesseurs de SensorManager (cf. src/ph_offset.h) —
  // la calibration électrochimique du module Atlas EZO n'est PAS touchée.
  // Toute écriture doit être suivie de sensors.refreshPhOffsetCache().
  float phOffset = 0.0f;              // Offset de mesure pH (défaut 0.00 = inerte)
};

struct FiltrationConfig {
  // feature-056 : le champ `enabled` a été absorbé par mqttCfg.installMode
  // (relais piloté ssi installMode == ManagedFiltration).
  String mode = "auto"; // auto, manual, off
  String start = "08:00";
  String end = "20:00";
  bool forceOn = false;  // Forçage ON temporaire (non persisté, remis à false au redémarrage)
  bool forceOff = false; // Forçage OFF temporaire (non persisté, remis à false au redémarrage)
};

struct LightingConfig {
  bool featureEnabled = true;          // Fonction éclairage activée/désactivée (masque l'UI)
  bool enabled = false;                // Éclairage ON/OFF manuel
  uint8_t brightness = 255;            // Luminosité PWM (0-255)
  bool scheduleEnabled = false;        // Programmation activée/désactivée
  String startTime = "20:00";          // Heure de début (HH:MM)
  String endTime = "23:00";            // Heure de fin (HH:MM)
};

struct AuthConfig {
  bool enabled = true;            // Authentification activée/désactivée
  String adminPassword = "admin"; // Mot de passe administrateur (HTTP Basic Auth)
  String apiToken = "";           // Token API pour intégrations (généré au boot si vide)
  String apPassword = "";         // Mot de passe réseau WiFi AP (généré au premier boot si vide)
  bool forceWifiConfig = false;   // Forcer l'affichage du bouton Wi-Fi sur l'écran login
  bool wizardCompleted = false;   // Wizard de configuration initiale complété (persiste au redémarrage)
  bool disableApOnBoot = false;   // Désactiver le mode AP au prochain redémarrage (pour transition WiFi)
  bool sensorLogsEnabled = false; // Logs détaillés des sondes (pH, ORP, Temp) activés/désactivés
  bool debugLogsEnabled = false;  // Logs DEBUG (firmware + UI) activés/désactivés
};

struct PumpControlParams {
  float minFlowMlPerMin;
  float maxFlowMlPerMin;
  float maxError;
};

struct ProductConfig {
  bool phTrackingEnabled = false;         // Suivi volume pH activé
  float phContainerVolumeMl = 20000.0f;   // Volume bidon pH (ml), défaut 20L
  float phTotalInjectedMl = 0.0f;         // Total injecté depuis dernier reset (ml)
  float phAlertThresholdMl = 2000.0f;     // Seuil d'alerte volume restant (ml), défaut 2L

  bool orpTrackingEnabled = false;        // Suivi volume chlore activé
  float orpContainerVolumeMl = 20000.0f;  // Volume bidon chlore (ml)
  float orpTotalInjectedMl = 0.0f;        // Total injecté depuis dernier reset (ml)
  float orpAlertThresholdMl = 2000.0f;    // Seuil d'alerte volume restant (ml)
};

struct SafetyLimits {
  float maxPhMlPerDay = 300.0f;
  float maxChlorineMlPerDay = 500.0f;
  float dailyPhInjectedMl = 0.0f;
  float dailyOrpInjectedMl = 0.0f;
  unsigned long dayStartTimestamp = 0;
  bool phLimitReached = false;
  bool orpLimitReached = false;
  char currentDayDate[9] = {};  // YYYYMMDD\0 — date locale du dernier reset journalier
  // v2.27.0 (feature-084, R1) — DÉNOMINATEUR de la charge acide observée :
  // minutes de circulation EFFECTIVES cumulées sur la journée courante (présence
  // d'eau au sens de resolveWaterPresence(), tous modes d'installation). Persisté
  // avec les compteurs de volume dans `pool-daily` : sans persistance, un
  // redémarrage en cours de journée repartirait de zéro et SOUS-estimerait les
  // minutes, donc SUR-estimerait la charge mL/min — soit un plafond de
  // restauration trop généreux (fail-open). Archivé puis remis à zéro au
  // basculement de date, au même point que les volumes.
  uint32_t dailyFiltrationMinutes = 0;
  // v2.34.0 (feature-093, R3) — compteur MONOTONE de minutes de filtration
  // cumulées depuis la première mise en service : JAMAIS remis à zéro au
  // basculement de date, incrémenté avec `dailyFiltrationMinutes`. C'est la base
  // de temps de la fraîcheur de l'instantané d'intégrale pH (`filtTotalMin` de
  // PhIntegralSnapshot) : l'âge en filtration = compteur courant − compteur écrit.
  // Persisté dans `pool-daily` (clé `filt_total`) dans la même transaction que les
  // volumes, relu quelle que soit la date. Déborde après ~8 000 ans.
  uint32_t filtTotalMin = 0;
};

struct PumpProtection {
  // Protection anti-cycling pour prolonger la durée de vie des pompes
  unsigned long minInjectionTimeMs = 30000;      // 30s minimum par injection
  // feature-094 (C9) : initialisés depuis constants.h — ces deux seuils ne sont
  // ni relus depuis la NVS ni exposés par l'API, ce sont de facto des constantes.
  // Leur cohérence avec le bruit résiduel du signal lent de décision est
  // verrouillée par static_assert là-bas, pas ici.
  float phStartThreshold = kPhStartThresholdDefault;  // Démarre dosage si erreur pH > 0.05
  float phStopThreshold = kPhStopThreshold;           // Continue dosage si erreur pH > 0.01
  float orpStartThreshold = 15.0f;               // Démarre dosage si erreur ORP > 15mV (feature-025 : deadband effectif ORP ±15 mV)
  float orpStopThreshold = 2.0f;                 // Continue dosage si erreur ORP > 2mV
  unsigned int maxCyclesPerDay = 24;             // Démarrages max/jour (feature-060). Configurable, borné
                                                 // [kCyclesPerDayMin, kCyclesPerDayMax].
                                                 // 48 → 24 en v2.20.5, condition P5 du gate pool-chemistry :
                                                 // le double comptage de cycle (premier tick à débit nul, cf.
                                                 // bug-dosage-fantome-duty-non-calibre) faisait que l'enveloppe
                                                 // RÉELLEMENT vécue valait la moitié de la valeur configurée, soit 24.
                                                 // Corriger le comptage sans abaisser cette limite aurait DOUBLÉ le
                                                 // nombre d'injections quotidiennes sous couvert de correctif.
                                                 // 24 reste dans le régime d'entretien sain (20-40/j).
};

// feature-053 : état du Mode Boost (surchloration temporaire du jour).
// Surcouche NON destructive : ne modifie PAS la config de régulation. Persisté
// en NVS dédié (saveBoostState/loadBoostState) pour survivre à un reboot dans
// la journée ; auto-expirant au prochain minuit local (tickBoostExpiry).
struct BoostState {
  bool active = false;      // Boost actif
  time_t untilEpoch = 0;    // Instant d'expiration (prochain minuit local)
};

struct TimezoneInfo {
  const char* id;
  const char* label;
  const char* posix;
};

// ==== Fuseaux horaires disponibles ====
static const TimezoneInfo TIMEZONES[] = {
  {"europe_paris", "Europe/Paris (UTC+1/UTC+2)", "CET-1CEST,M3.5.0/02:00:00,M10.5.0/03:00:00"},
  {"utc", "UTC", "UTC0"},
  {"america_new_york", "America/New_York (UTC-5/UTC-4)", "EST+5EDT,M3.2.0/02:00:00,M11.1.0/02:00:00"},
  {"america_los_angeles", "America/Los_Angeles (UTC-8/UTC-7)", "PST+8PDT,M3.2.0/02:00:00,M11.1.0/02:00:00"},
  {"asia_tokyo", "Asia/Tokyo (UTC+9)", "JST-9"},
  {"australia_sydney", "Australia/Sydney (UTC+10/UTC+11)", "AEST-10AEDT,M10.1.0/02:00:00,M4.1.0/03:00:00"}
};

// feature-056 : sérialisation STABLE du mode d'installation (JSON/WS/UART/NVS).
const char* installModeToString(InstallMode mode);
InstallMode installModeFromString(const char* s, InstallMode fallback);

const TimezoneInfo* findTimezoneById(const String& id);
const TimezoneInfo* defaultTimezone();
const TimezoneInfo* currentTimezone();
void ensureTimezoneValid();
void applyTimezoneEnv();

// ==== Variables globales de configuration ====
extern MqttConfig mqttCfg;
extern FiltrationConfig filtrationCfg;
extern LightingConfig lightingCfg;
extern AuthConfig authCfg;
extern PumpControlParams phPumpControl;
extern PumpControlParams orpPumpControl;
extern SafetyLimits safetyLimits;
extern PumpProtection pumpProtection;
extern ProductConfig productCfg;
extern bool productConfigDirty;
extern BoostState boostState;

void saveProductConfig();
void loadProductConfig();
void saveDailyCounters();
void loadDailyCounters();

// ==== feature-084 : instantané NVS de l'intégrale pH (ADR-0052 amendé) ====
// Namespace NVS DÉDIÉ (`pool-phint`, clé unique `snap`) : aucune interaction
// avec la config générale ni avec les compteurs journaliers, et une seule
// écriture d'un bloc de 20 octets par flush (usure minimale, cf. R5).
//
// ICI, IL N'Y A QUE DU TRANSPORT. La structure `PhIntegralSnapshot`, son schéma,
// son empreinte d'intégrité et TOUTES les décisions (R1-R7) vivent dans
// `ph_persist_logic.h`, module PUR compilable en natif — sans quoi rien de tout
// cela ne serait atteignable par les tests (ni config.cpp ni pump_controller.cpp
// ne sont dans le `build_src_filter` de `env:native`).
//
// AUCUN configMutex n'est pris par ces deux fonctions, et c'est VOULU : la
// charge utile appartient exclusivement à loopTask (`PumpControllerClass::
// _phIntegral`), il n'y a donc aucune donnée partagée à sérialiser — le mutex ne
// protégerait rien tout en introduisant une prise de verrou dans update(). Le
// pilote NVS est lui-même réentrant.

// Relit l'instantané. false si absent, de taille inattendue ou de CRC invalide —
// dans tous ces cas `out` est laissé à ses valeurs par défaut (fail-closed).
bool loadPhIntegralSnapshot(PhIntegralSnapshot& out);
// Écrit l'instantané en le SCELLANT au préalable (`reserved` normalisé, `crc`
// posé) : le champ `crc` fourni par l'appelant est ignoré.
void savePhIntegralSnapshot(PhIntegralSnapshot& s);

// ==== v2.27.0 (feature-084, R1) : anneau NVS de l'historique de charge ====
// 7 journées {date, mL pH, minutes de filtration} dans UN blob de 64 octets
// (`pool-daily`, clé `ph_load7`) — UNE écriture par jour, au basculement de date.
// Même répartition que ci-dessus : ici le transport, toutes les décisions
// (schéma, intégrité, insertion, exploitabilité, médiane) dans ph_persist_logic.h.
//
// Relit l'anneau. false si absent, de taille inattendue, d'un autre schéma ou de
// CRC invalide — `out` est alors laissé VIDE, donc `days = 0` et aucune
// restauration (fail-closed, rétrocompatible avec une NVS qui l'ignore).
bool loadPhLoadHistory(PhLoadHistory& out);
// Écrit l'anneau en le SCELLANT au préalable.
void savePhLoadHistory(PhLoadHistory& h);
// CLÔTURE d'une journée : ajoute l'échantillon à l'anneau, le persiste et
// journalise (info) le volume et les minutes retenus. No-op si la date est
// absente ou aberrante (horloge non synchronisée : une journée non datable
// n'est pas archivable). Idempotent pour une même date.
// À n'appeler QUE depuis les points EXISTANTS de basculement de date.
// `sustainableMlPerMin` : débit moyen soutenable COURANT, pour le filet de
// vraisemblance du journal (0 = inconnu ⇒ repli documenté).
void recordPhLoadDay(const char* date8, float phMl, uint32_t filtrationMinutes,
                     float sustainableMlPerMin);
// Charge acide observée du bassin, relue depuis l'anneau NVS : médiane des mL/min
// des journées exploitables. Prévu pour être appelé RAREMENT (au démarrage et à
// chaque clôture de journée) — l'appelant met le résultat en cache. Réutilisable
// tel quel par la dose de base apprise (feature-085), qui exige la même médiane.
//
// `sustainableMlPerMin` = phSustainableAvgFlow(débit d'amplitude CALIBRÉ, limite
// horaire CONFIGURÉE). C'est un PARAMÈTRE et non une constante : le seuil de
// vraisemblance doit suivre les réglages, sans quoi relever la limite horaire
// (recommandation du gate) écarterait toutes les journées en silence.
PhPersistLoadObservation readPhLoadObservation(float sustainableMlPerMin);
// feature-086 : série de journées calendaires consécutives SATURÉES par
// l'actionneur pH (délivré ≥ kPhActuatorLimitDayFillRatio × maximum possible),
// relue depuis le MÊME anneau — LECTURE SEULE STRICTE (schéma, CRC et
// recordPhLoadDay intouchés). Toute la décision vit dans
// ph_actuator_limit_logic.h (module PUR) ; ici, le transport uniquement.
// Prévu pour être appelé RAREMENT (démarrage + clôture de journée) — l'appelant
// met le résultat en cache, JAMAIS de lecture NVS depuis update().
// `sustainableMlPerMin` ≤ 0 ou non fini ⇒ 0 (pompe non caractérisée : pas de
// maximum calculable, donc pas de série — fail-closed côté alerte).
uint8_t readPhActuatorLimitStreak(float sustainableMlPerMin);
// SOURCE UNIQUE du débit moyen soutenable de la voie pH (mL/min) : débit
// d'amplitude CALIBRÉ de la pompe pH × limite horaire CONFIGURÉE. Rend 0 quand la
// pompe n'est pas caractérisée — l'appelant retombe alors sur le repli documenté
// (kPhLoadSustainableFallbackMlPerMin). Centralisée ici pour que le filet de
// vraisemblance de l'anneau et le prédicat de saturation de l'intégrale ne
// puissent pas diverger.
float phSustainableFlowFromConfig();

// Date du jour au format AAAAMMJJ, ou 0 si l'horloge n'est pas exploitable
// (time() < kMinValidEpoch). SOURCE UNIQUE de la date civile pour tout ce qui
// archive une journée : l'anneau de charge acide (feature-084) comme
// l'indicateur de temps d'atteinte (feature-096).
uint32_t currentLoadDateNumber();

// ==== feature-096 : anneau NVS du temps d'atteinte de cible ====
// Namespace NVS `pool-kpi` (clé unique `ttt7`), DISJOINT de `pool-daily`. Le
// namespace n'isole pas la flash (les pages sont partagées) mais le DROIT DE
// DESTRUCTION EN MASSE : il est l'unité de `clear()`. Mutualiser obligerait cet
// instrument à détenir un handle en ÉCRITURE sur le namespace des compteurs de
// dosage (dailyPhInjectedMl, dailyOrpInjectedMl, filtTotalMin, drapeaux de
// limite) — un `clear()` ou une clé mal nommée y effacerait les quotas
// journaliers. Moindre privilège : un non-garde-fou ne détient pas la clé
// d'écriture des garde-fous. `filt_total` reste lu en LECTURE SEULE, jamais
// écrit par ce module.
//
// Comme pour l'instantané d'intégrale, ICI IL N'Y A QUE DU TRANSPORT : schéma,
// intégrité, insertion et machine d'états vivent dans time_to_target_logic.h
// (config.cpp n'est pas dans le `build_src_filter` de `env:native`).
// À n'appeler que depuis loopTask : la NVS est bloquante, jamais depuis un
// callback AsyncWebServer.

// Relit l'anneau. false si absent, de taille inattendue, d'un autre schéma ou
// d'empreinte discordante — `out` est alors laissé VIDE (fail-closed).
bool loadTttHistory(TttHistory& out);
// Écrit l'anneau en le SCELLANT au préalable (`serieStamp` = estampille de
// série, condition C6 du gate). false = NVS INDISPONIBLE (namespace refusé,
// partition pleine ou corrompue) : l'appelant passe alors en « mémoire
// indisponible » et n'insiste PAS (aucune boucle de retry, aucun repli
// d'écriture vers un autre namespace).
bool saveTttHistory(TttHistory& h, uint32_t serieStamp);

// ==== Mode Boost (feature-053) ====
// Persistance NVS DÉDIÉE (namespace poolctrl, clés boost_active/boost_until) —
// PAS via saveMqttConfig, pour éviter l'usure NVS d'une réécriture complète.
void saveBoostState();
void loadBoostState();  // À appeler au boot ; si expiré au chargement → inactif.
// Active le boost jusqu'au prochain minuit local. REFUSE si l'heure n'est pas
// synchronisée (time() < kMinValidEpoch, condition pool-chemistry #4) : sans
// heure valide, l'expiration à minuit ne peut être calculée.
void startBoost();
// Désactive le boost et persiste immédiatement.
void stopBoost();
// True si le boost est actif à l'instant `now`. Si `now` < kMinValidEpoch
// (heure non synchro) → renvoie boostState.active SANS expirer (condition #4).
bool isBoostActive(time_t now);
// Expire le boost au passage de minuit. À appeler EN TÊTE de tickDailyRollover,
// AVANT tout reset de compteur (condition pool-chemistry #3).
void tickBoostExpiry(time_t now);

// ==== Mutex pour protection concurrence ====
// Protège l'accès aux configurations partagées entre loop() et handlers async
extern SemaphoreHandle_t configMutex;
// Protège l'accès au bus I2C partagé entre sensors.update() et calibrations
extern SemaphoreHandle_t i2cMutex;

// ==== Fonctions de gestion ====
void initConfigMutexes();  // Initialise les mutex (à appeler dans setup())
void saveMqttConfig();
void loadMqttConfig();

// feature-088 : restaure les 8 réglages de TUNING de la régulation aux valeurs
// d'usine (regulation_defaults.h) puis persiste (saveMqttConfig). Depuis la
// feature-090, les valeurs restaurées sont celles de la TAILLE DE BASSIN
// COURANTE (mqttCfg.poolSize). Écrit sous configMutex ; sur timeout du mutex,
// RIEN n'est modifié (journalisé en error).
// Journal avant → après pour chaque champ modifié — WARNING si la restauration
// AUGMENTE la capacité de dosage (consigne ORP ou limite horaire qui monte),
// info sinon. Ne touche JAMAIS : modes de régulation, ph/orpEnabled,
// phCorrectionType, phOffset, trace phAutoManual*, calibration/sélection des
// pompes, SafetyLimits (périmètre arbitré par pool-chemistry, feature-088).
bool restoreRegulationDefaults();  // false = timeout mutex, RIEN modifié

// v2.20.3 (C4) — trace de la bascule AUTOMATIQUE pH → manual.
// setPhAutoManualTrace() : à appeler JUSTE AVANT saveMqttConfig() par le chemin
// d'escalade (pump_controller) ; l'epoch vaut 0 si l'heure n'est pas synchronisée
// (la raison reste alors journalisée et exposée, seule la date manque).
// clearPhAutoManualTrace() : à appeler dès qu'un HUMAIN reprend la main sur
// `ph_regulation_mode` (route HTTP /save-config, commande MQTT) — la trace ne
// décrit que la DERNIÈRE bascule automatique non acquittée.
void setPhAutoManualTrace(const String& reason);
void clearPhAutoManualTrace();
void applyMqttConfig();
void sanitizePumpSelection();
int sanitizePumpNumber(int pumpNumber, int defaultValue);
int pumpIndexFromNumber(int pumpNumber);

// feature-021 : la calibration pH 2 points est gérée par le module Atlas EZO.
// Les fonctions calculatePhCalibration() / isPhCalibrationValid() ont été
// supprimées (utiliser sensors.getPhCalibrationPointsCached() à la place).

#endif // CONFIG_H
