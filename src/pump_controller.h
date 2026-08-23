#ifndef PUMP_CONTROLLER_H
#define PUMP_CONTROLLER_H

#include <Arduino.h>
#include <atomic>
#include "config.h"
#include "constants.h"
#include "pulse_dosing_logic.h"  // itération 2 : proportionnel de temps (option B+C)

struct PumpDriver {
  int pwmPin;    // Pin PWM Gate MOSFET (IRLZ44N)
  int channel;   // Canal LEDC pour PWM
};

struct DosingState {
  unsigned long windowStart = 0;
  unsigned long usedMs = 0;
  bool active = false;
  unsigned long lastTimestamp = 0;
  unsigned long lastSafetyTimestamp = 0; // suivi ml injectés (sécurité)
  
  // Variables pour protection anti-cycling
  unsigned long lastStartTime = 0;      // Moment du dernier démarrage
  unsigned long lastStopTime = 0;       // Moment du dernier arrêt
  unsigned int cyclesToday = 0;         // Nombre de démarrages aujourd'hui
  unsigned long cyclesDayStart = 0;     // Timestamp du début du jour des cycles
};

struct PIDController {
  // feature-025 (pool-chemistry) : régulation P temporisée par défaut.
  //   - pH  : Kp=8, Ki=0, Kd=0
  //   - ORP : Kp=0.3, Ki=0, Kd=0 (override en construction PumpController)
  // Kd=0 IMPÉRATIF (pas d'amplification du bruit résiduel). Ki=0 → intégrale inerte
  // (integralMax conservé mais sans effet). Les valeurs ci-dessous sont les défauts pH ;
  // l'ORP est ajusté dans le constructeur de PumpControllerClass.
  float kp = 8.0f;     // Proportionnel : réaction à l'erreur actuelle
  float ki = 0.0f;     // Intégral : désactivé (anti-windup strict, P pure)
  float kd = 0.0f;     // Dérivé : désactivé (bruit)
  float integral = 0.0f;
  float lastError = 0.0f;
  unsigned long lastTime = 0;
  float integralMax = 50.0f; // Anti-windup (inerte avec Ki=0, conservé pour réactivation future)
};

class PumpControllerClass {
private:
  PumpDriver pumps[2];
  uint8_t pumpDuty[2] = {0, 0};
  bool manualMode[2] = {false, false};  // Mode test manuel par pompe
  // volatile : écrit par loopTask, lu aussi en contexte web async / OTA.
  volatile bool otaInProgress = false;
  // Auto-guérison v2.9.1 (condition pool-chemistry C2) : quand true, tout dosage
  // (auto, manuel, scheduled) est inhibé dans update(). Posé UNIQUEMENT par le mode
  // maintenance de main.cpp (trop de reboots auto) ; reste actif jusqu'au prochain boot.
  // volatile : écrit par loopTask (setMaintenanceInhibit), lu en contexte web async
  // (setManualPump, isMaintenanceInhibited) → garantit la visibilité inter-tâches.
  volatile bool maintenanceInhibit = false;

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  // Migration core 3 (feature-063, prescription P1 pool-chemistry) : pompe
  // utilisable. Passé à false UNIQUEMENT dans begin() (chemin core 3) si
  // ledcAttachChannel() échoue — écrit AVANT le démarrage du serveur web, jamais
  // réarmé à chaud (reboot requis). Fail-closed : applyPumpDuty() refuse toute
  // mutation d'une pompe inutilisable (pumpDuty reste 0 à vie).
  // volatile : écrit par loopTask (begin), lu en contexte web async
  // (setManualPump → applyPumpDuty) — patron identique à maintenanceInhibit.
  volatile bool pumpUsable[2] = {true, true};
#endif

  DosingState phDosingState;
  DosingState orpDosingState;

  PIDController phPID;
  PIDController orpPID;

  void applyPumpDuty(int index, uint8_t duty);
  // feature-006 (condition pool-chemistry #2) : budget horaire usedMs PARTAGÉ
  // auto + manuel. Itération 2 (bug-dosage-fantome) : le prédicat d'accumulation
  // devient POMPE-ON (pompe physiquement alimentée — pouls auto, scheduled OU
  // manuel), plus la session : entre deux pouls la pompe ne consomme pas le
  // budget horaire. UN seul point d'accumulation → pas de double comptage.
  void refreshDosingState(DosingState& state, unsigned long now, bool pumpOn);

  // feature-025 : `deadband` = seuil de démarrage existant (phStartThreshold /
  // orpStartThreshold) ; `freezeIntegral` gèle l'accumulation intégrale (anti-windup
  // strict : filtre non prêt, canDose==false, pause mélange, saturation, mesure
  // rejetée/instable, erreur dans deadband). Le gel est appliqué AVANT le calcul.
  // feature-037 : le cœur du calcul (proportionnel + anti-windup + bornage final)
  // est délégué à computePidPure() (src/dosing_logic.*, testable en natif). La
  // coquille gère le temps (dt depuis millis), l'état PID et renvoie le débit
  // FINAL déjà borné [minFlow, maxFlow] (plus de constrain externe chez l'appelant).
  float computePID(PIDController& pid, float error, unsigned long now,
                   float deadband, bool freezeIntegral,
                   float minFlow, float maxFlow);
  // Itération 2 (bug-dosage-fantome) : dutyToFlow()/flowToDuty() SUPPRIMÉS —
  // le modèle linéaire duty↔débit est mort (mesure au banc 2026-08-23). Le
  // débit moyen est réalisé par proportionnel de TEMPS (pulse_dosing_logic).

  bool checkSafetyLimits(bool isPhPump);
  // Reset journalier (RTC/NTP local minuit, ou fallback millis() 24h).
  // Doit être appelé en permanence depuis update(), AVANT le check de filtration,
  // pour que les compteurs se réinitialisent même quand la filtration est arrêtée.
  void tickDailyRollover();
  // Crédite un VOLUME (mL) aux compteurs de sécurité journaliers + suivi produit.
  // Remplace updateSafetyTracking(flow, delta) : le volume est désormais calculé
  // en amont (pulseDeltaVolumeMl pour l'auto/scheduled, majorant pour le manuel).
  void creditInjectedVolume(bool isPhPump, float volumeMl);

  // Fonctions anti-cycling
  bool shouldStartDosing(float error, float startThreshold, DosingState& state, unsigned long now);
  // Itération 2 : la garde de temps minimum d'injection raisonne en temps
  // POMPE-ON cumulé de la session (sessionOnMs), plus en temps mural depuis
  // lastStartTime — cohérence avec le proportionnel de temps (flaggé pour
  // contre-visite du gate pool-chemistry).
  bool shouldContinueDosing(float error, float stopThreshold, unsigned long sessionOnMs);

  // Timer de stabilisation par pompe (index 0 = pH, 1 = ORP).
  // La cinétique chimique différente justifie une fenêtre indépendante par sonde
  // (cf. kStabilizationDurationPhMs / kStabilizationDurationOrpMs et
  // pool-chemistry condition #3).
  uint32_t _stabilizationEndMs[2] = {0, 0};

  // Cache de la dernière cause de refus pour log "edge-triggered" (1 seule entrée
  // par transition de cause). Évite le spam quand canDose() est appelé chaque cycle.
  // feature-025 : également exposé via getPhDoseBlockedReason()/getOrpDoseBlockedReason()
  // pour le WS (chaîne vide si dosage autorisé). Écrit/lu en loopTask uniquement.
  String _lastRefusalCause[2];

  // feature-025 : pause mélange hydraulique post-injection (pool-chemistry).
  // _mixingEndMs[0]=pH, [1]=ORP. Armé par notifyPhDose()/notifyOrpDose() à l'ARRÊT
  // d'une injection (homogénéisation post-dose) — bloque ainsi le cycle suivant sans
  // interrompre l'injection en cours. Gate indépendante (OR) dans canDose(), distincte du timer post-cal.
  // Écrits/lus en loopTask uniquement → pas de mutex (cohérent avec _stabilizationEndMs).
  uint32_t _mixingEndMs[2] = {0, 0};

  // Helpers internes pour le log de refus (canDose).
  void logRefusalOnce(int pumpIndex, const String& cause);
  void resetRefusalLogState(int pumpIndex);

  // ===== feature-067 : filets de sécurité sur le pH BRUT (pool-chemistry) =====
  // Un offset logiciel pH erroné décale la consigne EFFECTIVE de régulation.
  // Ces deux gardes raisonnent sur getPhFilteredRaw() (mesure capteur SANS
  // offset), donc restent valides même si l'offset est faux. Inertes à offset 0.
  // Appelées UNIQUEMENT depuis update() (loopTask) : état et logs sans mutex.

  // Condition #2 : plancher dur d'injection d'acide. True → dosage pH REFUSÉ
  // (branches automatique ET programmée). Palier kPhRawDosingFloor si
  // |offset| <= kPhOffsetWideThreshold, kPhRawDosingFloorWide au-delà.
  // Ne s'applique pas au produit basique (phCorrectionType == "ph_plus").
  // Condition #7 : compte les refus sur 24 h glissantes → au-delà de
  // kPhRawFloorRefusalMax, bascule la régulation pH en manuel (fail-closed).
  bool isPhRawFloorBlocking();

  // Condition #3 : inhibition du dosage ORP quand le pH BRUT dépasse
  // kPhRawOrpInhibit (chlore peu actif → la régulation ORP surdoserait).
  // True → dosage ORP SUSPENDU (branches automatique ET programmée).
  //
  // `requireKnownPh` (pool-chemistry P2) : true en régulation AUTOMATIQUE → un pH
  // brut INDISPONIBLE (capteur muet, filtre non amorcé) inhibe aussi, car
  // canDose(1) ne consulte jamais le pH : ce filet est l'unique garde du couplage
  // pH/chlore, et doser à l'aveugle avec une eau possiblement au-dessus de 7,60
  // reviendrait à surdoser. false pour le dosage PROGRAMMÉ, aveugle à la mesure
  // par conception (comportement historique préservé, hors périmètre).
  bool isOrpInhibitedByPhRaw(bool requireKnownPh);

  // État edge-triggered des deux gardes ci-dessus (1 seul log critical + 1 seule
  // alerte MQTT par transition) + compteur de refus sur fenêtre glissante 24 h.
  bool     _phRawFloorActive = false;
  bool     _orpPhRawInhibitActive = false;
  // v2.20.3 (C6) : nature de l'inhibition ORP en cours, pour que le message de
  // LEVÉE corresponde au message de POSE.
  //   0 = pH brut trop élevé (seuil franchi)
  //   1 = pH brut indisponible (capteur muet / filtre non amorcé) → anomalie
  //   2 = mesures non représentatives (purge chambre de mesure) → état NORMAL
  // Le cas 2 était auparavant journalisé comme le cas 1 : à chaque reprise de
  // filtration l'appareil annonçait un « capteur muet » alarmant alors qu'il
  // s'agissait de la stabilisation qu'il venait lui-même de déclencher.
  uint8_t  _orpPhRawInhibitKind = 0;
  uint8_t  _phRawFloorRefusals = 0;      // Refus dans la fenêtre courante
  uint32_t _phRawFloorWindowStart = 0;   // millis() du 1er refus de la fenêtre

  // Ring buffer anti-rafale (pool-chemistry Pass 3.5) — timestamps de start
  // de cycle (millis) par pompe. Index circulaire, 0 = pH, 1 = ORP.
  // Capacité 20 entrées : couvre largement la fenêtre 15 min à raison de
  // kMaxDosingCyclesPer15Min = 20 cycles max.
  uint32_t _dosingCycleHistory[2][kDosingCycleHistorySize] = {{0}, {0}};
  size_t _dosingCycleHistoryIdx[2] = {0, 0};

  // Enregistre un nouveau timestamp de démarrage de cycle dans le ring buffer.
  // À appeler EXACTEMENT au moment où le PWM passe de 0 → >0 (pas dans canDose,
  // qui ne fait que valider).
  void recordDosingCycleStart(int pumpIndex);

  // Compte les cycles dans la fenêtre [now - windowMs, now] pour la pompe.
  int countRecentDosingCycles(int pumpIndex, uint32_t windowMs) const;

  // Flags de reset demandés depuis des tâches externes (web handlers)
  // Résolus au début de update() pour éviter les races inter-core
  std::atomic<bool> _resetRequested{false};
  std::atomic<bool> _phPauseResetRequested{false};

  // feature-006 : démarrages de cycle MANUELS demandés depuis les handlers web
  // async (requestManualCycleRecord). Consommés en tête d'update() (loopTask,
  // AVANT les branches de régulation) : recordDosingCycleStart + cyclesToday++
  // + compteurs NVS dirty. Le ring anti-rafale et cyclesToday restent ainsi
  // écrits par UNE seule tâche (pas de mutex). Index 0 = pH, 1 = ORP.
  std::atomic<uint8_t> _manualCycleStartPending[2] = {{0}, {0}};

  // ===== Itération 2 (bug-dosage-fantome, option B+C) : train d'impulsions ====
  // Index par CHIMIE ([0]=pH, [1]=ORP). Écrits/lus en loopTask uniquement
  // (cohérent avec _stabilizationEndMs — aucun mutex requis).
  PulseState _pulseState[2] = {};        // état du train (période/pouls courant)
  uint32_t _pulseMs[2] = {0, 0};         // durée du pouls planifié pour la période
  bool _pulseSession[2] = {false, false}; // session de dosage en cours (auto OU scheduled)
  unsigned long _sessionOnMs[2] = {0, 0}; // temps POMPE-ON cumulé de la session (garde temps min)
  float _sessionVolMl[2] = {0.0f, 0.0f};  // volume crédité de la session (log d'arrêt)
  // Comptabilité par delta de tick : état pompe au tour PRÉCÉDENT (le volume du
  // delta [t0, now] est celui de la commande posée à t0, attribution arrière).
  bool _acctPumpOn[2] = {false, false};   // pompe alimentée au tour précédent
  bool _acctManual[2] = {false, false};   // ... via le mode manuel (crédit majorant)
  uint8_t _acctManualDuty[2] = {0, 0};    // duty manuel du tour précédent (crédit exact si == calibré)

  // Helpers de caractérisation — index de pompe PHYSIQUE (0 = pompe 1, 1 = pompe 2).
  // Crédit du volume du delta [lastSafetyTimestamp, now] pour une chimie
  // (chem 0 = pH, 1 = ORP) : pouls auto/scheduled → pulseDeltaVolumeMl (exact,
  // kick au débit 255 mesuré + plateau au débit calibré) ; manuel → MAJORANT
  // flow255, sauf duty == calibratedDuty → débit calibré. Jamais zéro sur une
  // pompe alimentée (P4). Met aussi à jour _sessionOnMs/_sessionVolMl.
  void accountPumpVolume(int chem, int physIdx, unsigned long now, bool manualActive);

  // feature-011 : état de la répartition scheduled par fenêtres de 15 min
  // (kScheduledWindowMinutes). windowIdx = fenêtre courante (nowMin/15, -1 =
  // aucune → recalcul forcé) ; stopTargetMl = cumul journalier cible d'arrêt
  // de la fenêtre courante ; plannedFlow = débit moyen planifié restant
  // (diagnostic WS ; NAN hors mode scheduled / hors plage / heure invalide).
  // Écrits/lus en loopTask uniquement (cohérent avec _stabilizationEndMs).
  int _phSchedWindowIdx = -1;
  float _phSchedStopTargetMl = 0.0f;
  float _phSchedPlannedFlow = NAN;
  int _orpSchedWindowIdx = -1;
  float _orpSchedStopTargetMl = 0.0f;
  float _orpSchedPlannedFlow = NAN;

  // Persistance des compteurs journaliers en NVS
  static bool _dailyLoaded;          // Chargement différé effectué (attend NTP)
  static bool _dailyCountersDirty;   // Indique qu'une sauvegarde est en attente
  static unsigned long _lastDailySaveMs; // Timestamp dernier flush NVS
  static bool _phWasActive;          // État pH précédent (détection démarrage injection)
  static bool _orpWasActive;         // État ORP précédent

public:
  PumpControllerClass();

  void begin();
  void update();
  void stopAll();
  void setOtaInProgress(bool inProgress);
  // Lecture seule du flag OTA (v2.9.2) : consommé par checkSystemHealth() pour
  // inhiber les watchdogs heap/connectivité pendant un flash (évite tout reboot
  // parasite au milieu d'une OTA). Ne touche PAS la logique de dosage.
  bool isOtaInProgress() const { return otaInProgress; }
  // Auto-guérison v2.9.1 (condition pool-chemistry C2) : inhibe/réarme tout dosage.
  // En mode maintenance, main.cpp appelle setMaintenanceInhibit(true) : update()
  // coupe alors les 2 pompes à chaque cycle tant que le flag est actif.
  void setMaintenanceInhibit(bool inhibit) { maintenanceInhibit = inhibit; }
  bool isMaintenanceInhibited() const { return maintenanceInhibit; }

  // feature-064 (ADR-0034, prescription pool-chemistry P1) : lecture seule
  // STRICTE de l'utilisabilité de la pompe (0 = pH, 1 = ORP/chlore), posée une
  // seule fois par begin() (feature-063). Ne doit JAMAIS servir à réactiver une
  // pompe (aucun setter — reboot requis). Sémantique bi-core :
  //   - core 3 : reflète pumpUsable[] (échec ledcAttachChannel possible au boot),
  //   - core 2 : toujours true (pas de flag, l'échec d'attache n'existe pas).
  // Index hors bornes → false (fail-closed).
  bool isPumpUsable(int index) const;

  // Getters pour l'état
  bool isPhDosing() const { return phDosingState.active; }
  bool isOrpDosing() const { return orpDosingState.active; }

  // bug-parachute-oom-jamais-rearme (condition pool-chemistry C1) : LECTURE
  // SEULE STRICTE — true dès qu'une pompe est énergisée (PWM > 0), QUELLE QUE
  // SOIT l'origine. isPhDosing()/isOrpDosing() ne reflètent QUE la régulation
  // AUTO : une injection manuelle ou un test pompe est armé par setManualPump()
  // depuis la tâche web et ne met JAMAIS phDosingState.active à true. Ce getter
  // sert de garde CUMULATIVE au contrôle d'intégrité du tas (main.cpp) et
  // n'influence AUCUNE décision de dosage (aucun setter associé).
  // Course bénigne assumée : pumpDuty[] est écrit par loopTask et par la tâche
  // web (setManualPump) ; la lecture d'un octet est atomique sur Xtensa, le pire
  // cas est un contrôle d'intégrité différé d'un tour de loop (fail-closed).
  bool isAnyPumpEnergized() const { return pumpDuty[0] > 0 || pumpDuty[1] > 0; }
  unsigned long getPhUsedMs() const { return phDosingState.usedMs; }
  unsigned long getOrpUsedMs() const { return orpDosingState.usedMs; }

  // ===== feature-053 : valeurs ORP EFFECTIVES (surcouche Mode Boost) =====
  // Surcouche NON destructive : renvoie la cible / limite journalière consommées
  // par la régulation. Effet STRICTEMENT réservé au mode ORP "automatic" et au
  // boost actif (isBoostActive) ; sinon renvoie la valeur normale de config.
  // Consommées par la régulation ORP automatic ET exposées à l'UI/WS/MQTT.
  float getEffectiveOrpTarget() const;
  float getEffectiveMaxChlorineMlPerDay() const;

  // Applique les paramètres PID selon mqttCfg.regulationSpeed
  void applyRegulationSpeed();

  // Setters pour PID tuning
  void setPhPID(float kp, float ki, float kd);
  void setOrpPID(float kp, float ki, float kd);

  // Reset des états
  void resetDosingStates();

  // Réinitialise la garde anti-cycling du pH (ex: changement de type de correction)
  void resetPhPauseGuard();

  // Arme le timer de stabilisation pour une pompe donnée (0 = pH, 1 = ORP).
  // Bloque le dosage pendant la durée appropriée :
  //   - kStabilizationDurationPhMs / kStabilizationDurationOrpMs après calibration EZO,
  //   - mqttCfg.stabilizationDelayMin (override utilisateur) pour les armings legacy
  //     (filtration, mode continu, passage de minuit) — applique la même durée aux 2 pompes.
  void armStabilizationTimer(int pumpIndex);

  // Surcharge legacy (sans paramètre) : conservée pour les sites d'arming "globaux"
  // (filtration, boot mode continu, rollover minuit). Arme les 2 pompes simultanément
  // avec la durée utilisateur `mqttCfg.stabilizationDelayMin`.
  void armStabilizationTimer();

  // True si le timer de stabilisation est encore actif pour la pompe donnée.
  bool isStabilizationTimerActive(int pumpIndex) const;

  // Réinitialise le timer de stabilisation des deux pompes (ex: arrêt filtration)
  void clearStabilizationTimer();

  // Retourne les secondes restantes de stabilisation max sur les 2 pompes
  // (0 si les deux sont expirés). Utilisé par l'UI WS pour afficher le compte à rebours.
  unsigned long getStabilizationRemainingS() const;

  // feature-006 : secondes restantes de stabilisation pour UNE pompe (0 si expirée
  // ou index invalide). ASYMÉTRIE ASSUMÉE (condition pool-chemistry #5) : la boucle
  // AUTO suspend les 2 pompes dès qu'UNE stabilisation est active (garde globale
  // d'update()), alors que l'injection MANUELLE n'est bloquée que par la fenêtre
  // de SA pompe — les fenêtres sont indépendantes par sonde
  // (kStabilizationDurationPhMs / kStabilizationDurationOrpMs, cinétiques différentes).
  unsigned long getStabilizationRemainingS(int pumpIndex) const;

  // ===== feature-006 : compteurs exposés aux gardes d'injection manuelle =====
  // Appelés depuis le handler web async (hors loopTask). Lecture SNAPSHOT sans
  // mutex : les compteurs sont écrits en loopTask uniquement et les lectures
  // 32 bits sont atomiques sur ESP32 → course BÉNIGNE (au pire une valeur en
  // retard d'un tour de loop). Les deux getters AJOUTENT le pending
  // _manualCycleStartPending non encore consommé : deux starts manuels
  // rapprochés se voient mutuellement (fail-closed).

  // Cycles du jour (auto + manuel), pending manuel inclus. 0 si index invalide.
  unsigned int getCyclesToday(int pumpIndex) const;

  // Cycles dans la fenêtre glissante [now - windowMs, now] (ring anti-rafale
  // PARTAGÉ avec l'auto), pending manuel inclus. 0 si index invalide.
  int getRecentCycles(int pumpIndex, uint32_t windowMs) const;

  // Demande l'enregistrement d'un démarrage de cycle MANUEL (thread-safe,
  // appelable depuis un handler async). Consommé en tête d'update() :
  // recordDosingCycleStart (ring anti-rafale partagé) + cyclesToday++ + NVS dirty.
  void requestManualCycleRecord(int pumpIndex);

  // Crédit plancher du registre journalier à la fin NATURELLE d'une injection
  // manuelle volumée (bug sous-compte v2.9.1). `idx` logique : 0 = pH, 1 = ORP.
  // À appeler en loopTask uniquement (updateManualInject).
  // Propriété : le registre de sécurité ne sous-compte jamais un volume promis
  // injecté en entier ; peut sur-compter de quelques dixièmes de mL
  // (conservateur) ; jamais appelé sur arrêt anticipé (stop manuel /
  // interruption filtration : l'intégration réelle fait foi).
  void creditManualInjectionFloor(int idx, float startCumulMl, float creditMl);

  // Vérifie si le dosage est autorisé pour la pompe `pumpIndex` (0 = pH, 1 = ORP).
  // Ordre des gardes (validé pool-chemistry feature-021) — fail-closed strict :
  //   1. Watchdog actif
  //   2. Filtration en marche (ou mode continu)
  //   3. Lecture pH/ORP FILTRÉE non NaN (feature-025 ; cond #1 stale + cond #5 bus dégradé)
  //   3b. Filtre capteur prêt (feature-025 : warmup / EZO injoignable → fail-closed)
  //   3c. Capteur non instable (feature-025 : rejets consécutifs → fail-closed)
  //   4. EZO calibré (cond #2 : pH ≥ 2 points, ORP ≥ 1 point ; -1 → bloqué)
  //   5. Pas de stabilisation post-cal en cours (cond #3)
  //   5b. Pas de pause mélange hydraulique active (feature-025 — gate indépendante)
  //   6. Mode régulation = automatic
  //   7. Limite journalière non atteinte
  //   8. Limite horaire non atteinte
  //   9. Anti-cycling : cyclesToday < maxCyclesPerDay
  //  10. Anti-rafale court terme (ring buffer 1 min / 15 min, Pass 3.5)
  // Log "edge-triggered" : 1 entrée info par transition de cause de refus.
  bool canDose(int pumpIndex);

  // ===== feature-025 : pause mélange hydraulique post-injection =====
  // À appeler au démarrage d'une injection (à côté de recordDosingCycleStart).
  // Arme _mixingEndMs[pompe] = nowMs + kPhMixingDelayMs / kOrpMixingDelayMs.
  // Géré uniquement par timestamps (AUCUN delay()).
  void notifyPhDose(uint32_t nowMs);
  void notifyOrpDose(uint32_t nowMs);
  bool isPhMixingDelayActive(uint32_t nowMs) const;
  bool isOrpMixingDelayActive(uint32_t nowMs) const;
  // Secondes restantes de pause mélange par pompe ([0]=pH, [1]=ORP) — observabilité UI.
  unsigned long getMixingRemainingS(int pumpIndex) const;

  // ===== feature-025 : raison de blocage dosage exposée au WS =====
  // Chaîne vide si dosage autorisé, sinon la dernière cause de refus de canDose().
  String getPhDoseBlockedReason() const { return _lastRefusalCause[0]; }
  String getOrpDoseBlockedReason() const { return _lastRefusalCause[1]; }

  // ===== feature-011 : débit moyen planifié du mode scheduled (mL/min) =====
  // Débit moyen restant de la répartition (remaining / horizonMinutes).
  // NAN hors mode scheduled, hors plage de filtration, ou heure locale invalide
  // (le WS traduit NAN en null). Rafraîchi à chaque tour d'update() (loopTask).
  float getPhScheduledPlannedFlow() const { return _phSchedPlannedFlow; }
  float getOrpScheduledPlannedFlow() const { return _orpSchedPlannedFlow; }

  // ===== Itération 2 : état de caractérisation exposé à l'UI/WS (agent web) ====
  // Lecture SEULE, appelable depuis les handlers async (lit mqttCfg — octets et
  // float 32 bits, lectures atomiques Xtensa, course bénigne d'un tour de loop).

  // Amplitude courante du plateau d'impulsion pour la pompe PHYSIQUE
  // (0 = pompe 1, 1 = pompe 2) : pumpNMaxDutyPct × 255 / 100. 0 si index invalide.
  uint8_t getPumpAmplitudeDuty(int pumpIndex) const;

  // True si la pompe PHYSIQUE ne peut PAS doser en connaissance de débit :
  // jamais caractérisée (calibratedDuty == 0), débit implausible, amplitude
  // différente du duty calibré, ou amplitude sous MIN_ACTIVE_DUTY (B2).
  // = evaluatePulseGate(...) != None (le seuil C, lui, dépend de la demande).
  // Index invalide → true (fail-closed). Sert au badge « débit non caractérisé ».
  bool isPumpFlowUncharacterized(int pumpIndex) const;

  // Raccourcis par CHIMIE (résolvent l'affectation mqttCfg.phPump/orpPump).
  bool isPhPumpFlowUncharacterized() const {
    return isPumpFlowUncharacterized(pumpIndexFromNumber(mqttCfg.phPump));
  }
  bool isOrpPumpFlowUncharacterized() const {
    return isPumpFlowUncharacterized(pumpIndexFromNumber(mqttCfg.orpPump));
  }

  // ===== Itération 3 : calendrier d'impulsion exposé à l'UI/WS (LECTURE SEULE) =
  // Index par CHIMIE (chem 0 = pH, 1 = ORP). AUCUN setter associé : ces getters
  // n'influencent aucune décision de dosage, ils servent uniquement à distinguer
  // à l'UI « session active » (commandé) vs « pompe alimentée » (pouls effectif).
  // Course bénigne assumée (patron isAnyPumpEnergized/getPumpAmplitudeDuty) :
  // _pulseSession/_pulseMs/_pulseState sont écrits par loopTask et lus ici en
  // contexte web async (async_tcp) ; les lectures 8/32 bits sont atomiques sur
  // Xtensa, le pire cas est un instantané en retard d'un tour de loop.

  // Session de dosage en cours (auto OU scheduled) pour la chimie. false si index invalide.
  bool isPulseSessionActive(int chem) const;

  // Durée du pouls planifié pour la période courante (ms). 0 si index invalide.
  uint32_t getPulseMs(int chem) const;

  // Position dans la période d'impulsion : nowMs − periodStartMs (soustraction
  // uint32 non signée, wrap-safe), clampée [0..kPulsePeriodMs]. 0 si session
  // inactive ou index invalide.
  uint32_t getPulsePhaseMs(int chem, uint32_t nowMs) const;

  // Test manuel des pompes (à utiliser avec précaution)
  void setManualPump(int pumpIndex, uint8_t duty);
};

extern PumpControllerClass PumpController;

#endif // PUMP_CONTROLLER_H
