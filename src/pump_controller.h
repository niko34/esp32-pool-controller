#ifndef PUMP_CONTROLLER_H
#define PUMP_CONTROLLER_H

#include <Arduino.h>
#include <atomic>
#include "config.h"
#include "constants.h"
#include "pulse_dosing_logic.h"  // itération 2 : proportionnel de temps (option B+C)
#include "dosing_logic.h"        // v2.25.1 : DoseRefusal (code de cause exposé)
#include "ph_integral_logic.h"   // v2.26.0 : terme intégral pH (logique PURE)
#include "ph_persist_logic.h"    // feature-084 : persistance de l'intégrale (PURE)
#include "ph_feedforward_logic.h"  // feature-085 : dose de base apprise (PURE)
#include "ph_session_logic.h"      // feature-094 : porte de session pH (PURE)

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
  // feature-025 (pool-chemistry) : régulation P temporisée.
  //   - ORP : Kp=0.3, Ki=0, Kd=0 (override en construction PumpController)
  // v2.26.0 (gate pool-chemistry) : Ki LEVÉ sur le SEUL pH — un P pur sous charge
  // basique permanente ne peut structurellement PAS rejoindre sa consigne (il se
  // stabilise sur son droop e* = besoin/(kp×T) : 7,50-7,66 mesurés pour une
  // consigne à 7,30, cf. constants.h « RÉGULATION pH — TERME INTÉGRAL »).
  //   - pH  : Kp/Ki/integralMax posés par applyRegulationSpeed() selon la vitesse
  //           (kPhKp*/kPhKi*/kPhIntegralMax*), Kd=0.
  //   - ORP : Ki=0 INCHANGÉ (hors périmètre du gate v2.26.0).
  // Kd=0 IMPÉRATIF sur les DEUX chimies (pas d'amplification du bruit résiduel).
  //
  // AUCUN champ de cette structure n'est persisté en NVS. `integral` n'est ici
  // qu'un MIROIR de PumpControllerClass::_phIntegral.integral, dont la
  // persistance — autorisée par feature-084, ADR-0052 amendé — passe par un
  // instantané dédié soumis à sept garanties cumulatives (plafond de
  // restauration à 30 % du plafond, CRC + contexte, péremption 6 h,
  // confirmation par la mesure, cadence d'écriture, traçabilité, anti-boucle de
  // boot). Cf. constants.h, section « PERSISTANCE NVS de l'intégrale pH ».
  float kp = 8.0f;     // Proportionnel : réaction à l'erreur actuelle
  float ki = 0.0f;     // Intégral : 0 = inerte (défaut ; le pH est armé par
                       // applyRegulationSpeed, l'ORP reste à 0)
  float kd = 0.0f;     // Dérivé : désactivé (bruit)
  float integral = 0.0f;
  float lastError = 0.0f;
  unsigned long lastTime = 0;
  // Anti-windup. Défaut VOLONTAIREMENT inerte (Ki=0 par défaut) ; côté pH la
  // valeur réelle est dimensionnée PAR L'AUTORITÉ (U_I,max / ki) et posée par
  // applyRegulationSpeed(). Ne jamais réactiver un Ki sans redimensionner ce
  // plafond : 50 pH·s ne représentent que 0,6 % de la charge d'équilibre pH,
  // soit une régulation « avec intégrale » qui n'en a que le nom.
  float integralMax = 50.0f;
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
  // feature-075 : suspension du dosage AUTOMATIQUE et PROGRAMMÉ pendant
  // l'assistant de mesure du débit. Posé/levé par le handler web async
  // (setFlowAssistSuspend) — y compris par le timeout serveur
  // kFlowAssistSuspendTimeoutMs ; consommé par update() (loopTask) qui referme
  // les états de dosage par le chemin existant de la perte de filtration.
  // Le mode MANUEL (véhicule de l'assistant) reste pleinement fonctionnel.
  // volatile : écrit en contexte web async, lu par loopTask — patron identique
  // à maintenanceInhibit (visibilité inter-tâches ; l'écriture d'un octet est
  // atomique sur Xtensa, course bénigne : prise en compte au pire différée
  // d'un tour de loop, fail-closed côté suspension).
  volatile bool _flowAssistSuspend = false;
  // État précédent vu par update() : log info one-shot par TRANSITION
  // (pose/levée), jamais de spam par tour. Écrit/lu en loopTask uniquement.
  bool _flowAssistSuspendPrev = false;

  // feature-063, prescription P1 pool-chemistry : pompe utilisable. Passé à
  // false UNIQUEMENT dans begin() si ledcAttachChannel() échoue — écrit AVANT le
  // démarrage du serveur web, jamais réarmé à chaud (reboot requis). Fail-closed :
  // applyPumpDuty() refuse toute mutation d'une pompe inutilisable (pumpDuty
  // reste 0 à vie).
  // volatile : écrit par loopTask (begin), lu en contexte web async
  // (setManualPump → applyPumpDuty) — patron identique à maintenanceInhibit.
  volatile bool pumpUsable[2] = {true, true};

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
  // feature-110 — accesseur UNIQUE du drapeau de suivi produit d'une chimie
  // (isPh true = pH, false = ORP). CONST, sans effet de bord, sans log, sans
  // verrou : c'est ce qui rend le couplage volume/démarrages STRUCTUREL. Les
  // deux crédits (creditInjectedVolume et le compteur de démarrages
  // d'accountPumpVolume) le lisent, donc un mutant sur cet accesseur gèle V ET
  // N ensemble — impossible de faire avancer l'un sans l'autre.
  bool productTrackingEnabled(bool isPh) const;

  // Fonctions anti-cycling
  bool shouldStartDosing(float error, float startThreshold, DosingState& state, unsigned long now);
  // feature-094 : effets de bord journaliers de la porte de démarrage (rollover
  // 24 h de cyclesToday + warning throttlé). Extraits de shouldStartDosing pour
  // rester atteignables par le pH, dont la porte est désormais phSessionGate().
  // loopTask uniquement.
  void tickDosingCycleDay(DosingState& state, unsigned long now);
  // Itération 2 : la garde de temps minimum d'injection raisonne en temps
  // POMPE-ON cumulé de la session (sessionOnMs), plus en temps mural depuis
  // lastStartTime — cohérence avec le proportionnel de temps (flaggé pour
  // contre-visite du gate pool-chemistry).
  bool shouldContinueDosing(float error, float stopThreshold, unsigned long sessionOnMs);

  // ===== v2.26.0 : FERMETURE de session de dosage pH (branche AUTOMATIQUE) =====
  // POINT DE PASSAGE UNIQUE de toute fin de session pH automatique. Y sont
  // réunies les trois choses qui doivent aller ENSEMBLE et qui, avant ce cycle,
  // ne vivaient que dans la branche automatique :
  //   1. `phDosingState.active = false` + datation de `lastStopTime` ;
  //   2. ARMEMENT DE LA PAUSE MÉLANGE (notifyPhDose) — homogénéisation post-dose ;
  //   3. le log d'arrêt de session (durée, pompe-ON, volume, cumul du jour).
  //
  // POURQUOI CE POINT UNIQUE. La branche automatique est court-circuitée par la
  // limite horaire, la limite journalière, une garde pH ou canDose() : la session
  // se refermait alors SILENCIEUSEMENT en fin d'update() (`active = phActive`),
  // sans pause mélange ni trace. Le produit déjà injecté n'avait donc jamais son
  // temps d'homogénéisation, et le journal ne montrait aucun arrêt.
  //
  // No-op (aucune écriture, aucun log, aucune allocation) si aucune session pH
  // n'est ouverte — appelable donc inconditionnellement depuis un chemin de
  // suspension. `reason` doit être un littéral (durée de vie statique).
  // Appelée UNIQUEMENT depuis update() (loopTask) — pas de mutex.
  void closePhDosingSession(unsigned long now, const char* reason);

  // Timer de stabilisation par pompe (index 0 = pH, 1 = ORP).
  // La cinétique chimique différente justifie une fenêtre indépendante par sonde
  // (cf. kStabilizationDurationPhMs / kStabilizationDurationOrpMs et
  // pool-chemistry condition #3).
  uint32_t _stabilizationEndMs[2] = {0, 0};

  // Cache de la dernière cause de refus pour log "edge-triggered" (1 seule entrée
  // par transition de cause). Évite le spam quand canDose() est appelé chaque cycle.
  // feature-025 : également exposé via copyPhDoseBlockedReason()/
  // copyOrpDoseBlockedReason() pour /data et le WS (chaîne vide = dosage autorisé).
  //
  // TAMPON FIXE ET NON `String` (revue v2.25.0) : cette chaîne est écrite par
  // loopTask (surveillance, escalade, canDose) et LUE depuis `async_tcp` (route
  // /data), sans aucune exclusion mutuelle. Copier une `String` réassignée par
  // une autre tâche lit `buffer`/`len` pendant que l'écrivain peut la libérer et
  // la réallouer : lecture de tas libéré, potentiellement fatale. Un tampon
  // membre de l'objet ne peut PAS être libéré — le pire cas dégénère en lecture
  // DÉCHIRÉE, soit un message d'affichage partiellement à jour, sans
  // conséquence. Dimensionnement et politique de troncature UTF-8 :
  // cf. kDoseBlockedReasonMaxLen (constants.h) et clampReasonUtf8()
  // (pump_controller.cpp). Écrit en loopTask UNIQUEMENT.
  char _lastRefusalCause[2][kDoseBlockedReasonMaxLen] = {{0}, {0}};

  // v2.25.1 — CODE de la cause, tenu STRICTEMENT en phase avec le tampon texte
  // ci-dessus (mêmes quatre points d'écriture : canDose, garde, relâche, reset).
  // Cf. getPhDoseBlockedCode() pour la sémantique des valeurs.
  uint8_t _lastRefusalCode[2] = {kDoseBlockedCodeNone, kDoseBlockedCodeNone};

  // v2.25.1 — escalade des causes de PANNE DE MESURE (sonde figée / plus aucune
  // mesure acceptée) en `warning` + alerte MQTT dédiée, sous condition de
  // PERSISTANCE (kDoseSensorFaultAlertDelayMs) : le transitoire de ré-amorçage à
  // la reprise de circulation ne doit pas crier au loup.
  //
  // UN ÉPISODE = une période de panne CONTINUE, quelle que soit la NATURE de la
  // panne pendant cette période (revue : la fenêtre était redatée sur tout
  // changement de code, donc aussi sur panne → panne ; un EZO agonisant qui
  // alterne figé ↔ périmé par phases < 60 s n'alertait alors JAMAIS).
  //   `_sensorFaultActive`  = une panne était en cours au tour précédent — SEUL
  //                           front non-panne → panne redate la fenêtre ;
  //   `_sensorFaultSinceMs` = millis() du début de l'épisode (0 = pas d'épisode) ;
  //   `_sensorFaultAlerted` = alerte déjà émise pour CET épisode (une seule, même
  //                           si la nature de la panne change ensuite).
  // loopTask uniquement (patron _stabilizationEndMs).
  uint32_t _sensorFaultSinceMs[2] = {0, 0};
  bool _sensorFaultAlerted[2] = {false, false};
  bool _sensorFaultActive[2] = {false, false};

  // Écriture BORNÉE d'une cause dans _lastRefusalCause[pumpIndex] (troncature
  // sûre en UTF-8). Seul point d'écriture du tampon, loopTask uniquement.
  void storeRefusalCause(int pumpIndex, const char* cause);

  // ===== D1 (v2.25.0) : PROPRIÉTÉ de la chaîne de cause ======================
  // Deux écrivains alimentent `_lastRefusalCause` — la SURVEILLANCE pH/ORP
  // (tick*Surveillance, inconditionnelle) et la DÉCISION canDose() (sous
  // conjonction `&&`). Sans arbitrage, la cause posée par une garde restait
  // FIGÉE après le lever de la butée dans tous les modes où canDose() n'est
  // jamais appelé — en particulier le mode `manual`, le seul où l'UI affiche
  // l'injection manuelle, et l'état exact où l'escalade laisse l'appareil.
  // L'UI annonçait alors un blocage révolu, indéfiniment.
  //
  // ARBITRAGE : un seul propriétaire à la fois, mémorisé ici.
  //   - true  = la chaîne courante a été posée par une GARDE de sécurité
  //             (surveillance ou escalade) → seule la garde peut l'effacer,
  //             et elle le fait à la RELÂCHE, qui tourne à chaque tick ;
  //   - false = la chaîne a été posée (ou vidée) par canDose() → la garde n'y
  //             touche pas, sinon la relâche effacerait un refus RÉEL et
  //             toujours actif (« filtration arrêtée », quota, anti-rafale…).
  // Conséquence : quand une garde bloque réellement, la cause reste celle de la
  // garde (canDose n'est pas atteint) ; quand canDose refuse, elle reste celle
  // de canDose ; quand ni l'un ni l'autre ne refuse, la chaîne est vide.
  // Écrit/lu en loopTask uniquement (patron _stabilizationEndMs).
  bool _refusalCauseFromGuard[2] = {false, false};

  // Pose une cause au nom d'une GARDE (prend la propriété de la chaîne).
  void setGuardRefusalCause(int pumpIndex, const String& cause);
  // Efface la cause SI ET SEULEMENT SI une garde en est propriétaire. No-op
  // (aucune écriture, aucune allocation) dans tous les autres cas — appelable
  // donc à chaque tick depuis les branches de relâche.
  void clearGuardRefusalCause(int pumpIndex);

  // feature-025 : pause mélange hydraulique post-injection (pool-chemistry).
  // _mixingEndMs[0]=pH, [1]=ORP. Armé par notifyPhDose()/notifyOrpDose() à l'ARRÊT
  // d'une injection (homogénéisation post-dose) — bloque ainsi le cycle suivant sans
  // interrompre l'injection en cours. Gate indépendante (OR) dans canDose(), distincte du timer post-cal.
  // Écrits/lus en loopTask uniquement → pas de mutex (cohérent avec _stabilizationEndMs).
  uint32_t _mixingEndMs[2] = {0, 0};

  // Helpers internes pour le log de refus (canDose). `code` est mémorisé pour
  // /data + WS et pilote la SÉVÉRITÉ du journal : v2.25.1 escalade en `warn` +
  // alerte MQTT dédiée les causes de PANNE de mesure (capteur figé, mesure
  // absente/périmée) — sur une installation surveillée à distance, un `info`
  // dédoublonné reste invisible. Les autres causes restent en `info`.
  void logRefusalOnce(int pumpIndex, const String& cause, DoseRefusal code);
  void resetRefusalLogState(int pumpIndex);

  // ===== Gardes de sécurité pH (pool-chemistry, refonte v2.25.0) =============
  // Les seuils CHIMIQUES portent sur le pH CORRIGÉ (brut + offset) : une fois
  // l'offset appliqué, la valeur EST le pH de l'eau, et un seuil chimique doit
  // porter sur le pH réel. Seules les deux BUTÉES de dernier recours portent sur
  // le pH BRUT (getPhFilteredRaw()), pour rester valides si l'offset est FAUX.
  // Le pH corrigé est RECALCULÉ ici — applyPhOffset(getPhFilteredRaw(), offset) —
  // et non lu via getPhFiltered() : getPhFilteredRaw() est l'unique accesseur qui
  // porte la garde d'âge du filtre (pool-chemistry P2). Aucune garde d'âge n'est
  // ajoutée à getPhFiltered(), dont le contrat est celui de /data, du WS et de MQTT.
  // Appelées UNIQUEMENT depuis update() (loopTask) : état et logs sans mutex.

  // ===== SÉPARATION SURVEILLANCE / DÉCISION (contre-visite C1, v2.25.0) ======
  // Les fonctions de DÉCISION ci-dessous sont appelées dans une conjonction
  // d'éligibilité (`phMode == "automatic" && phLimitOk && … && !isPhGuardBlocking`).
  // Le court-circuit `&&` de C++ fait qu'elles ne sont PAS appelées dès qu'un
  // terme amont est faux : quota journalier épuisé, mode `manual` (l'état exact
  // où l'escalade laisse l'appareil), régulation désactivée… Y laisser la
  // SURVEILLANCE, c'est promettre des alertes qui ne partiront jamais quand elles
  // sont le plus nécessaires — une panne silencieuse.
  //
  // La surveillance vit donc dans les deux `tick*Surveillance()` ci-dessous,
  // appelées INCONDITIONNELLEMENT une fois par tick d'update(), hors de toute
  // conjonction. Elles portent : les franchissements de seuil, l'hystérésis de
  // relâche, les logs warn/critical, les alertes MQTT et la cause exposée au WS.
  // Elles ne comptent RIEN et n'escaladent JAMAIS (un appareil laissé en manuel
  // s'auto-escaladerait sinon). Elles ne lisent que des CACHES capteur
  // (getPhFilteredRaw / getPhOffset) : aucune transaction I²C supplémentaire.

  // Surveillance pH inconditionnelle — met à jour les 5 drapeaux d'hystérésis, le
  // verdict `_phGuardBlockingNow` et le latch `_phGuardRefusalPending`, journalise
  // et alerte. Une mesure périmée (getPhFilteredRaw() == NaN, garde d'âge du
  // filtre) ne produit NI refus NI alerte : tous les drapeaux sont réarmés.
  // D1 : la RELÂCHE (et le cas mesure périmée) EFFACE la cause de refus quand une
  // garde en est propriétaire — cf. _refusalCauseFromGuard.
  void tickPhGuardSurveillance();

  // Surveillance de l'inhibition ORP inconditionnelle — met à jour
  // `_orpPhInhibitActive` / `_orpPhInhibitKind` et le verdict `_orpPhInhibitNow`.
  // R1 (v2.25.0) : SEUL point d'écriture des DEUX mémoires d'hystérésis de seuil
  // `_orpPhCorrThresholdActive` (seuil CORRIGÉ) et `_orpPhRawThresholdActive`
  // (butée BRUTE haute) — chacune écrite depuis le résultat de SA SEULE
  // comparaison, jamais depuis l'autre ni depuis une branche NaN.
  // R2 (contre-visite v2.25.0) : porte aussi l'anti-rafale d'alertes —
  // re-qualification du motif admise UNIQUEMENT en montée de gravité, et une
  // seule alerte par gravité et par ÉPISODE (`_orpPhAlertedSeverity`, remis à
  // zéro à la seule LEVÉE de l'inhibition).
  // La politique `requireKnownPh` (fail-closed sur pH inconnu) est dérivée ICI du
  // mode ORP courant, plus passée par l'appelant : la branche automatique n'est
  // atteignable que si le mode EST "automatic", donc le verdict rendu à la
  // décision est celui de sa propre politique.
  // D1 : la RELÂCHE efface la cause de refus ORP d'origine garde (idem pH).
  void tickOrpPhInhibitSurveillance();

  // ===== v2.26.0 : TERME INTÉGRAL pH — la coquille du module PUR ==============
  // Toute la LOGIQUE (charge signée sur `erreur − stopThreshold`, oubli, reset
  // sec, péremption, hystérésis de saturation et rappel) vit dans
  // src/ph_integral_logic.h, testable en natif. Ici : le temps, les entrées, et
  // l'application du verdict. État et logs en loopTask, sans mutex.

  // C4 — PÉREMPTION : purge l'intégrale pH quand la branche automatique n'a pas
  // tourné depuis kPhIntegralStaleResetMs (mode `off`/`manual`, garde bloquante
  // durable, filtration arrêtée tout l'hiver). Appelée UNE fois par tick
  // d'update(), hors de toute conjonction d'éligibilité — c'est justement dans
  // les états où l'intégrale est laissée à l'abandon qu'elle doit être purgée.
  void tickPhIntegralExpiry(unsigned long now);

  // Applique le verdict d'un tick du module pur : journalisation de la purge
  // sèche, des paliers de saturation (warning / critical + RAPPEL périodique) et
  // de la levée d'alerte, plus la publication MQTT associée. Aucune décision.
  void applyPhIntegralVerdict(const PhIntegralVerdict& verdict, float error);

  // ===== feature-084 (sujet 1) : CADENCE — le tick sort de la conjonction =====
  // Jusqu'à la v2.26.0, phIntegralTick() n'était appelé QUE dans la branche
  // automatique, elle-même gardée par `canDose(0)`. Rapport cyclique mesuré : à
  // peine 25 % du temps — l'intégrale n'apprenait pas pendant les pauses de
  // mélange ni pendant les suspensions de limite horaire, précisément les états
  // où la charge du bassin continue pourtant d'agir sur la mesure. `Ti` = 6 h de
  // branche valait donc ~24 h réelles.
  //
  // Précédent suivi : tickPhGuardSurveillance() (v2.25.0), sortie de la même
  // conjonction pour la même raison — « surveillance ≠ décision ».
  //
  // LE PRÉDICAT D'ENTRETIEN, état par état (validé par le gate du 30/08) — en
  // clair : `canDose(0)` MOINS {pause mélange, limite horaire, quota journalier,
  // anti-cycling}, PLUS `phMode == "automatic"`.
  //   ACCUMULE   : pause mélange (l'eau circule, la mesure est valide ; le
  //                sur-crédit est borné à 15 min × 0,15 = 135 pH·s, 0,8 % du
  //                plafond), limite horaire épuisée, quota, anti-cycling,
  //                plafond de session — ce sont des suspensions RYTHMIQUES du
  //                dosage, pas des interdits ; les gels d'anti-windup
  //                (`demande ≥ soutenable`, quota 80 %) restent actifs et
  //                bornent l'accumulation exactement comme avant.
  //   N'ACCUMULE PAS : filtration coupée / eau absente, stabilisation post-cal,
  //                OTA, maintenance, assistant débit — update() sort AVANT le
  //                point d'appel, la garantie est structurelle ;
  //                porte de validité fermée, warmup, sonde figée/périmée, NaN,
  //                calibration insuffisante — REJET EXPLICITE ici, jamais hérité
  //                du `isfinite(error)` interne au module pur ;
  //                mode `manual`/`off` (dosage durablement impossible : charger
  //                serait armer une autorité libérée d'un bloc au retour) ;
  //                garde pH bloquante ou escalade des 3 refus (fail-closed : on
  //                ne pré-arme jamais une autorité pendant une inhibition de
  //                sécurité) ; watchdog inactif.
  //
  // Lit le VERDICT `_phGuardBlockingNow` posé par tickPhGuardSurveillance() et
  // non isPhGuardBlocking(true) : ce dernier COMPTE les refus et peut escalader.
  // Le prédicat doit rester sans aucun effet de bord.
  // loopTask uniquement (il lit les caches capteur et l'état de garde du tick).
  bool phIntegralAccumulationAllowed();

  // UN tick d'entretien de l'intégrale, hors de toute conjonction de dosage.
  // No-op si le prédicat ci-dessus est faux (l'absence de tick nourrit alors la
  // péremption C4, inchangée). Fournit aussi la base de temps de la confirmation
  // de restauration NVS (R4). loopTask uniquement.
  // feature-085 : `ffMlPerMin` = débit du feedforward appliqué CE tour, retranché
  // du soutenable MOYEN vu par le PI (anti-windup sur « demande − FF », condition
  // bloquante n°4 de la spec — la dimension correcte est traitée dans
  // PhIntegralInputs::ffMlPerMin, ph_integral_logic.h).
  void tickPhIntegralAuto(unsigned long now, float error, float ampFlow,
                          int limitMin, bool allowed, float ffMlPerMin);

  // ===== feature-085 : DOSE DE BASE APPRISE (feedforward) — la coquille =======
  // Toute la LOGIQUE (plafond d'actionneur, quota, historique minimal, latch et
  // réarmement) vit dans src/ph_feedforward_logic.h, testable en natif. Ici : le
  // câblage (erreur signée, validité de mesure, empreinte de contexte, charge
  // observée, session) et la journalisation des transitions. Rend le débit FF
  // (mL/min) à ADDITIONNER au PI dans la branche automatique — c'est le SEUL
  // terme rendu, il passe ensuite par le chemin de dosage existant (computePID
  // borné à `ampFlow − ff`, somme clampée, planPulse, comptabilité) : un seul
  // chemin vers la pompe, une seule comptabilité (condition n°1 du gate).
  // À appeler AVANT tickPhIntegralAuto (le tick intégral consomme le FF).
  // loopTask uniquement (lit les caches capteur et l'état de session).
  float tickPhFeedforward(unsigned long now, float signedError,
                          float sustainableMlPerMin);

  // État du feedforward (module pur) — loopTask uniquement.
  PhFfState _phFf;

  // ===== feature-094 (v2.36.0) : PORTE DE SESSION pH ROBUSTE AU BRUIT ========
  // Signal LENT de décision (moyenne glissante de 10 min de la filtrée). Tické
  // HORS branche, à côté du tick intégral et après la garde de présence d'eau :
  // la porte doit continuer de mûrir pendant les pauses de mélange et les
  // suspensions de limite horaire, où l'eau circule et la mesure est valide.
  // loopTask uniquement — écrivain unique, aucun mutex (patron _phIntegral).
  PhDecisionFilter _phDecision;
  // Erreur SIGNÉE portée par le signal lent, recalculée à chaque tick (NAN tant
  // que la fenêtre n'est pas pleine). Consommée par la seule branche automatique.
  float _phDecisionError = NAN;
  // Un cycle a-t-il déjà été comptabilisé pour la session pH EN COURS ? Depuis
  // que la session vit sur le verdict de PORTE et non plus sur le débit
  // instantané, l'ouverture peut précéder de plusieurs tours le premier débit
  // réel (computePID rejette dt > 10 s au premier tick). Sans ce témoin, le
  // cycle ne serait comptabilisé NI à l'ouverture (aucun débit) NI ensuite
  // (`startDecided` est déjà retombé) : la comptabilité de sécurité
  // (cyclesToday, ring anti-rafale) deviendrait aveugle. Remis à false à toute
  // fermeture de session. loopTask uniquement.
  bool _phCycleCounted = false;
  // Instantané servi à /data + WS (écrit par tickPhFeedforward en loopTask, lu
  // en contexte web async — course bénigne, patron isAnyPumpEnergized).
  // feature-092 : le débit FF servi vit désormais dans l'instantané de
  // composition ci-dessous ; il ne reste ici que le CODE d'état.
  uint8_t _ffStatus = 0;  // PhFfStatus (contrat figé, cf. ph_feedforward_logic.h)
  // ===== feature-092 : INSTANTANÉS DE COMPOSITION DU DÉBIT ===================
  // Écrits par la branche automatique (loopTask) au point EXACT où la demande de
  // débit est calculée, lus par les huit accesseurs de projection en contexte web
  // async. Copie sous spinlock portMUX (patron feature-096) : les cinq flottants
  // d'une même composition sont servis COHÉRENTS entre eux — sans lui, la carte
  // pourrait afficher un terme P d'un tour et une demande du suivant, donc une
  // équation qui ne s'additionne pas.
  //
  // FRAÎCHEUR — À SAVOIR : ces instantanés ne sont rafraîchis QUE lorsque la
  // branche automatique évalue une demande (même contrat que l'instantané
  // `_ffStatus`). Hors session, ils portent la dernière composition connue ; la
  // porte de validité des mesures (measure_validity) les traduit en `null` dès
  // que la filtration s'arrête ou qu'une purge s'ouvre, c'est-à-dire dans tous
  // les cas où ils cesseraient de décrire le bassin.
  DoseComposition _phDoseComp;
  DoseComposition _orpDoseComp;
  mutable portMUX_TYPE _doseCompMux = portMUX_INITIALIZER_UNLOCKED;
  // Publie une composition dans l'instantané de la voie (0 = pH, 1 = ORP).
  // loopTask uniquement ; une seule section critique, aucune allocation.
  void publishDoseComposition(int chemIndex, const DoseComposition& c);
  // Copie cohérente de l'instantané d'une voie (0 = pH, 1 = ORP) — une seule
  // section critique. Appelable depuis un contexte web async.
  DoseComposition doseCompositionSnapshot(int chemIndex) const;
  // Compose ET publie la composition d'une voie (0 = pH, 1 = ORP) : SOURCE
  // UNIQUE de lecture de kp et du terme intégral, pour que les deux points
  // d'appel de la branche automatique (avec dosage / sans dosage) ne puissent
  // pas décrire l'équation différemment. Rend la composition, dont le filet de
  // non-divergence (C2) a besoin. loopTask uniquement.
  DoseComposition composeDose(int chemIndex, float error, float deadband,
                              float ffServed, float ampFlow,
                              DoseFlowRegime regime);
  // Anti-spam : premier tick actif de la session déjà journalisé.
  bool    _ffAnnounced = false;
  // Recommandation du gate — TENUE PRODUIT : au changement de phCorrectionType,
  // le latch FF est tenu jusqu'à ce qu'au moins UNE journée ENTIÈRE ait été
  // archivée sous le nouveau régime (l'anneau contient des mL de l'autre
  // produit, la médiane ne décrit pas la charge courante). Deux temps : la
  // première clôture de journée après le changement archive une journée MIXTE
  // (_ffProductHoldFreshDay la saute), la suivante lève la tenue. RAM seulement
  // (pas de NVS — périmètre du plan) : un reboot la lève, le FF restant borné
  // par le plafond d'actionneur et le gel de quota (fail-open borné, assumé).
  uint8_t _ffCorrectionKind = 255;      // 255 = pas encore observé (boot)
  bool    _ffProductChangeHold = false;
  bool    _ffProductHoldFreshDay = false;
  // C2 du visa feature-092 : témoin d'anti-spam du filet de non-divergence (un
  // warn par session). Regroupé AVEC les booléens voisins (et non entre les deux
  // instantanés de composition, où il coûtait du remplissage). RAM seulement,
  // remis à false hors session par tickPhFeedforward — ni persistance, ni
  // compteur, ni champ servi (contrainte explicite du visa).
  bool    _phCompDivergenceLogged = false;

  // ===== feature-084 (sujet 3) : PERSISTANCE NVS (R1-R7) =====================
  // La coquille ne DÉCIDE de rien : elle fournit le temps, l'heure, la mesure et
  // les réglages à `ph_persist_logic.h` (module PUR, testable en natif), puis
  // applique et journalise le verdict. Toute règle ajoutée ici et non là serait,
  // par construction, hors d'atteinte des tests.
  //
  // Lecture DIFFÉRÉE de l'instantané (attend une heure valide, R3). Une seule
  // tentative aboutie ; abandon définitif après kPhIntegralRestoreArmWindowMs.
  void loadPhIntegralSnapshotDeferred(unsigned long now);
  // Flush NVS borné (R5), appelé au point de sauvegarde différée de fin
  // d'update(). No-op si la persistance est désarmée (R7) ou si les conditions
  // de cadence / d'amplitude ne sont pas réunies.
  void tickPhIntegralPersistence(unsigned long now);
  // Empreinte de CONTEXTE (R2) : vitesse de régulation, type de correction,
  // consigne pH et plafond d'anti-windup courants. Toute différence avec celle
  // de l'instantané invalide la restauration.
  uint32_t phIntegralContextHash() const;
  // R1 — CHARGE ACIDE OBSERVÉE du bassin : médiane des volumes pH journaliers sur
  // kPhIntegralRestoreHistoryWindowDays, rapportée aux minutes de circulation du
  // jour. v2.27.0 : l'anneau NVS qui manquait existe (readPhLoadObservation) ;
  // ce que l'on rend ici est son CACHE, rafraîchi au démarrage et à chaque
  // clôture de journée — jamais une lecture NVS depuis update().
  // Accesseur PUBLIC volontairement réutilisable : la dose de base apprise
  // (feature-085) consomme exactement la même médiane sur 7 jours.
  PhPersistLoadObservation phIntegralObservedLoad() const;
  // Relit l'anneau NVS et recalcule le cache ci-dessus. RARE par construction
  // (démarrage + une fois par jour) : c'est la seule lecture NVS du chemin.
  void refreshPhLoadObservation();
  // Cumule les minutes de circulation EFFECTIVES de la journée courante
  // (dénominateur de la charge). Appelé à chaque tour d'update(), hors de toute
  // conjonction d'éligibilité — c'est du comptage, pas une décision de dosage.
  void tickDailyFiltrationMinutes(unsigned long now);
  // R1 — plafond de restauration COURANT, min(butée absolue, moitié de
  // l'équilibre observé). Recalculé à chaque usage : le plafond d'anti-windup et
  // la charge observée changent tous les deux en cours de vie.
  PhPersistCapVerdict phIntegralRestoreCap() const;

  // État persistant du terme intégral pH (source de vérité ; phPID.integral en
  // est le miroir consommé par computePidPure et les accesseurs /data + WS).
  PhIntegralState _phIntegral;

  // --- feature-084 : état de la persistance (loopTask uniquement) ------------
  bool     _phIntegralPersistLoaded = false;  // tentative de lecture soldée
  // R3/R4 : candidate en attente de confirmation par la mesure (machine PURE).
  PhPersistConfirmState _phIntegralConfirm;
  float    _phIntegralSavedValue = 0.0f;      // R5 : dernière valeur persistée
  uint32_t _phIntegralSaveMs = 0;             // R5 : millis du dernier flush
  // v2.34.0 (feature-093) : compteur de filtration à la dernière écriture (P3,
  // échéance en temps de filtration) et demande de réécriture C5 (l'instantané
  // relu au boot a été rejeté : la NVS porte un enregistrement inutile).
  uint32_t _phIntegralSavedFiltMin = 0;
  bool     _phIntegralRewriteWanted = false;

  // --- v2.27.0 (R1) : historique de charge (loopTask uniquement) -------------
  PhPersistLoadObservation _phLoadObs;        // cache de la médiane sur 7 jours
  uint32_t _waterMinuteTickMs = 0;            // millis du dernier tick de comptage
  uint32_t _waterMinuteAccumMs = 0;           // résidu sous la minute

  // --- feature-086 : sous-dimensionnement de l'actionneur pH ------------------
  // CACHE de la série de journées saturées (ph_actuator_limit_logic.h), rafraîchi
  // par refreshPhLoadObservation() — c'est-à-dire aux SEULS points existants où
  // l'anneau NVS est relu (démarrage différé + clôture de journée) : JAMAIS de
  // lecture NVS dans update() ni dans un handler. Écrit en loopTask, lu aussi en
  // contexte web async (isPhUndersizedSuspected) — lecture 8 bits atomique sur
  // Xtensa, course bénigne (patron isAnyPumpEnergized).
  uint8_t _phLimitStreakDays = 0;
  // État annoncé de l'alerte : log par TRANSITION (montée ET retombée), jamais
  // par tour. Écrit/lu en loopTask uniquement (tickPhUndersizedAlert).
  bool _phUndersizedActive = false;
  // Compare le verdict courant à l'état annoncé et journalise le franchissement
  // (warning à la montée, info à la retombée). Le verdict est celui de
  // l'accesseur publié isPhUndersizedSuspected() — garde mesure-pH-valide
  // comprise — pour que journal et champ /data+WS ne divergent jamais. Coût par
  // tour : trois lectures de cache et une comparaison — aucune NVS, aucune
  // allocation. Appelé une fois par tick d'update(), hors de toute conjonction
  // d'éligibilité (l'alerte doit retomber même quand la branche automatique
  // n'est pas atteinte). loopTask uniquement.
  void tickPhUndersizedAlert();

  // Conditions #2 + #7 : DÉCISION de refus du dosage pH. True → dosage pH REFUSÉ.
  // Ne détecte plus rien : rend le verdict de tickPhGuardSurveillance() et n'y
  // ajoute que ce qui relève d'un refus RÉEL (comptage + escalade en automatique,
  // signalement périodique en programmé). Seuils appliqués par la surveillance :
  //   - `ph_minus` : refus d'ACIDIFIER sous kPhFloorCorrected [CORRIGÉ], plus la
  //     butée kPhFloorRawHard [BRUT] contre un offset positif saisi à tort ;
  //   - `ph_plus`  : refus d'ALCALINISER au-dessus de kPhCeilingCorrected
  //     [CORRIGÉ] (durcissement v2.25.0 : le pH+ n'était pas gardé du tout),
  //     plus la butée kPhCeilingRawHard [BRUT] contre un offset négatif saisi à
  //     tort (contre-visite C3 : symétrique de la butée brute basse — sans elle
  //     un offset −0,50 erroné laissait le pH+ alcaliniser jusqu'à 8,20 brut).
  // Alerte NON BLOQUANTE en plus, à kPhLowWarn [CORRIGÉ], dans les deux cas.
  //
  // `countRefusal` (condition du gate) : true depuis la branche AUTOMATIQUE
  // seulement. Un refus y alimente le compteur 24 h glissantes → au-delà de
  // kPhGuardRefusalMax, bascule fail-closed de la régulation pH en manuel. La
  // branche PROGRAMMÉE passe false : elle refuse tout autant (c'est un filet
  // chimique, pas une boucle de régulation) mais ne compte ni n'escalade — elle
  // signale à la place, en warn + alerte MQTT à débit limité.
  bool isPhGuardBlocking(bool countRefusal);

  // Condition #3 : DÉCISION d'inhibition du dosage ORP sur pH élevé. True →
  // dosage ORP SUSPENDU (branches automatique ET programmée). Rend le verdict de
  // tickOrpPhInhibitSurveillance(), sans effet de bord. Deux seuils, OU logique :
  // kPhOrpInhibit sur le pH CORRIGÉ (chlore peu actif → la régulation ORP
  // surdoserait) et la butée kPhCeilingRawHard sur le pH BRUT, inconditionnelle,
  // contre un offset négatif saisi à tort qui masquerait une eau réellement haute.
  //
  // Le paramètre `requireKnownPh` a disparu (pool-chemistry P2 inchangé sur le
  // fond) : la politique est désormais dérivée du mode ORP dans la surveillance —
  // AUTOMATIQUE → un pH INDISPONIBLE (capteur muet, filtre non amorcé) inhibe
  // aussi, car canDose(1) ne consulte jamais le pH ; PROGRAMMÉ → aveugle à la
  // mesure par conception (comportement historique préservé).
  bool isOrpInhibitedByPh() const { return _orpPhInhibitNow; }

  // État edge-triggered des gardes ci-dessus : 1 seul log critical + 1 seule
  // alerte MQTT par transition, et hystérésis de relâche portée par ces drapeaux.
  // Écrits par la SURVEILLANCE uniquement (tick*Surveillance) — jamais par la
  // décision : c'est ce qui garantit UNE alerte par franchissement, la
  // surveillance étant le seul point d'évaluation et n'étant appelée qu'une fois
  // par tick.
  //
  // CINQ drapeaux pH INDÉPENDANTS, et non un seul : ils vivent dans deux
  // référentiels (corrigé / brut) et dans deux directions (bas / haut). Un
  // drapeau unique ferait relâcher une garde sur le franchissement d'une autre.
  bool     _phLowBlockActive     = false;  // Refus d'acidifier  [CORRIGÉ, bas]
  bool     _phHighBlockActive    = false;  // Refus d'alcaliniser [CORRIGÉ, haut]
  bool     _phRawHardBlockActive = false;  // Butée acide         [BRUT, bas]
  // Contre-visite C3 : butée brute HAUTE appliquée au refus d'ALCALINISER,
  // symétrique de _phRawHardBlockActive. Sans elle, kPhCeilingRawHard n'agissait
  // que sur l'inhibition ORP : un offset −0,50 erroné laissait le pH+ pousser
  // l'eau au-delà de 8,10 BRUT pendant que le chlore était inhibé — l'appareil
  // conduisait lui-même l'eau dans l'impasse sanitaire. Depuis le plafond à 7,60,
  // les deux gardes mordent au même point au pire cas d'offset (cf. constants.h).
  bool     _phRawHardHighBlockActive = false;  // Butée base      [BRUT, haut]
  bool     _orpPhInhibitActive   = false;  // Inhibition ORP  [CORRIGÉ + BRUT, haut]
  // ===== R1 (revue v2.25.0) : DÉCOUPLAGE DES DEUX RÔLES DU DRAPEAU ORP =======
  // `_orpPhInhibitActive` cumulait deux rôles incompatibles :
  //   (a) MÉMOIRE D'HYSTÉRÉSIS des franchissements de seuil (paramètre
  //       `wasBlocking` de phGuardBlocksAbove) ;
  //   (b) LATCH « inhibition en cours », qui porte `_orpPhInhibitKind` et le
  //       edge-trigger de log/alerte.
  // Or les branches NaN arment (b) pour des motifs ÉTRANGERS À TOUT SEUIL
  // (kind 1 = pH indisponible, kind 2 = stabilisation des mesures). Résultat
  // observé : reprise de filtration → mesures non représentatives → latch posé
  // (kind 2) ; le pH revient à 7,57, JAMAIS monté au-dessus de 7,60 — mais
  // l'hystérésis, croyant sortir d'un blocage, exigeait ≤ 7,55 et MAINTENAIT
  // l'inhibition du chlore, indéfiniment, à chaque démarrage de filtration, en
  // annonçant « stabilisation des mesures » (kind figé). Panne silencieuse de
  // chloration + cause publiée trompeuse.
  //
  // Les drapeaux ci-dessous ne portent QUE le rôle (a) : ils sont armés/désarmés
  // EXCLUSIVEMENT par les comparaisons de seuil de
  // tickOrpPhInhibitSurveillance(). AUCUNE branche NaN ne les écrit — le
  // fail-closed sur pH inconnu reste entier, il agit sur le latch (b), pas sur la
  // mémoire de franchissement.
  //
  // ===== R2 (contre-visite v2.25.0) : UNE MÉMOIRE PAR SEUIL ==================
  // Ces deux seuils PARTAGEAIENT un drapeau unique, écrit par `(raw || corr)`.
  // Le seuil le moins souvent armé perdait alors toute hystérésis : tant que le
  // seuil CORRIGÉ maintenait le drapeau à true, la butée BRUTE se comportait en
  // simple comparateur à 8,05 (= 8,10 − hystérésis). Cas physique observé —
  // bassin à pH 8,03-8,12 avec offset 0,00 : chaque mesure franchissant 8,05
  // re-qualifiait le motif (3 ↔ 0) et produisait un critical + une alerte MQTT,
  // en alternant DEUX messages contradictoires (« offset négatif probablement
  // erroné » alors que l'offset vaut 0,00, puis « pH > 7,60 »). Deux seuils, deux
  // référentiels : une mémoire par seuil, comme les quatre drapeaux pH ci-dessus.
  bool     _orpPhCorrThresholdActive = false;  // kPhOrpInhibit     [CORRIGÉ, haut]
  bool     _orpPhRawThresholdActive  = false;  // kPhCeilingRawHard [BRUT, haut]
  // ===== R2 : anti-rafale — une alerte par gravité et par ÉPISODE ============
  // Gravité du motif d'inhibition DÉJÀ signalé dans l'épisode courant :
  //   0 = épisode vierge (rien signalé) — sentinelle, toute gravité la dépasse
  //   1 = mesure absente  (`_orpPhInhibitKind` 1 ou 2)
  //   2 = seuil CORRIGÉ franchi (kind 0)
  //   3 = butée BRUTE haute    (kind 3)
  // Une alerte n'est émise QUE si la gravité du motif courant dépasse cette
  // valeur : la re-qualification ne va donc jamais en DÉGRADATION (la butée brute
  // qui se relâche à 8,05 pendant que le seuil corrigé tient ne réémet rien), et
  // une gravité déjà signalée ne réémet pas non plus. Remise à 0 au SEUL point de
  // LEVÉE de l'inhibition — jamais avant, sinon l'épisode se scinderait et la
  // rafale reviendrait.
  uint8_t  _orpPhAlertedSeverity = 0;
  // Motif dont le LIBELLÉ de cause est actuellement exposé (mêmes valeurs que
  // `_orpPhInhibitKind`, 255 = aucun). Sert d'edge-trigger au rafraîchissement du
  // libellé quand la mesure devient indisponible sous un latch de motif de seuil :
  // sans lui, la cause publiée continuait d'afficher l'ancienne valeur pH, et le
  // rafraîchir à chaque tick construirait un String par tour de loop.
  uint8_t  _orpPhCauseKind = 255;
  // Alerte NON BLOQUANTE d'eau acide (kPhLowWarn) : edge-trigger séparé, parce
  // qu'elle n'est pas une garde — elle ne refuse rien et ne compte rien.
  bool     _phLowWarnActive      = false;
  // Type de correction observé au tick précédent (0 = ph_minus, 1 = ph_plus).
  // Snapshot lu UNE FOIS par tick : changer de produit change de seuil ET de
  // direction, donc les trois drapeaux pH ci-dessus deviennent caducs et sont
  // réarmés. Le compteur de refus, lui, n'est PAS réinitialisé : une anomalie
  // durable ne doit pas s'effacer d'un changement de réglage.
  uint8_t  _phCorrectionKind = 0;
  // v2.20.3 (C6) : nature de l'inhibition ORP en cours, pour que le message de
  // LEVÉE corresponde au message de POSE.
  //   0 = pH corrigé trop élevé (kPhOrpInhibit franchi)
  //   1 = pH indisponible (capteur muet / filtre non amorcé) → anomalie
  //   2 = mesures non représentatives (purge chambre de mesure) → état NORMAL
  //   3 = butée BRUTE haute (kPhCeilingRawHard) → offset négatif suspect
  // Le cas 2 était auparavant journalisé comme le cas 1 : à chaque reprise de
  // filtration l'appareil annonçait un « capteur muet » alarmant alors qu'il
  // s'agissait de la stabilisation qu'il venait lui-même de déclencher.
  uint8_t  _orpPhInhibitKind = 0;

  // ===== Verdicts du tick, posés par la surveillance, lus par la décision =====
  // Écrits/lus en loopTask uniquement (aucun mutex, patron _stabilizationEndMs).
  // Verdict pH du tick courant : true si une garde pH bloque le dosage.
  bool     _phGuardBlockingNow = false;
  // Latch « épisode de blocage pas encore comptabilisé ». Posé par la
  // surveillance à la TRANSITION vers blocage (au plus une fois par tick, comme
  // l'ex-`newRefusal`), consommé UNE seule fois par la décision automatique.
  // Sans lui, un franchissement survenu pendant une inéligibilité amont (quota
  // épuisé) ne serait jamais compté : la surveillance ayant déjà posé les
  // drapeaux d'hystérésis, la décision ne verrait plus aucune transition au
  // retour de l'éligibilité. Remis à false dès que le blocage retombe : un
  // épisode terminé sans refus RÉEL ne doit rien compter.
  bool     _phGuardRefusalPending = false;
  // Verdict ORP du tick courant : true si le dosage chlore est inhibé.
  bool     _orpPhInhibitNow = false;

  uint8_t  _phGuardRefusals = 0;         // Refus AUTOMATIQUES dans la fenêtre
  uint32_t _phGuardWindowStart = 0;      // millis() du 1er refus de la fenêtre
  // Débit limité de l'alerte « dosage pH programmé refusé » (pas de compteur ni
  // d'escalade dans cette branche : sans réémission, un refus permanent serait
  // un no-op silencieux après la première transition).
  uint32_t _phScheduledAlertMs = 0;
  bool     _phScheduledAlertSent = false;

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

  // ===== v2.26.0 (revue, bloquant n°1) : CHANGEMENT DE RÉGLAGE PID DIFFÉRÉ =====
  // `applyRegulationSpeed()` est appelée depuis le contexte web async (route de
  // config). Elle écrivait alors directement `phPID` et `_phIntegral`, que
  // loopTask lit ET réécrit dans le même tick (phIntegralTick). L'entrelacement
  // « loopTask lit I → la tâche web purge tout et pose un Ki plus grand → loopTask
  // réécrit I » ressuscite jusqu'à 16 200 pH·s servis avec le nouveau Ki, soit
  // 13,5 mL/min pour une autorité de 6 — et le clamp sur le nouveau plafond ne
  // s'exécute pas (accumulation gelée, delta positif non appliqué).
  //
  // Patron du projet (resetPhPauseGuard) : la tâche web pose un drapeau ATOMIQUE,
  // loopTask résout en tête d'update(). AUCUNE écriture de phPID/_phIntegral hors
  // loopTask. `_regulationSpeedCode` porte la vitesse demandée sous forme de CODE
  // 8 bits (0 = slow, 1 = normal, 2 = fast) : loopTask n'a ainsi jamais à lire la
  // String mqttCfg.regulationSpeed, qu'une autre tâche peut réassigner.
  std::atomic<bool> _phTuningChangeRequested{false};
  std::atomic<uint8_t> _regulationSpeedCode{1};

  // Application EFFECTIVE des coefficients (loopTask uniquement) : begin() et
  // update() après consommation de `_phTuningChangeRequested`.
  void applyRegulationSpeedNow();

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
  // v2.26.0 : instant d'OUVERTURE de la session pH (millis, 0 = aucune session).
  // Base du critère MURAL du plafond de session (kPhSessionMaxWallMs). Distinct de
  // `phDosingState.lastStartTime`, qui n'est daté qu'à la COMPTABILISATION d'un
  // cycle : une reprise après suspension hors-branche rouvre bien une session sans
  // compter de cycle, et laisserait donc une base de temps périmée. Scalaire (pH
  // seul) : le plafond de session est une réponse au point de fonctionnement du PI
  // pH — l'ORP, dont la bande morte n'a pas été alignée, referme ses sessions
  // normalement et reste hors périmètre.
  uint32_t _phSessionStartMs = 0;
  // v2.26.0 (revue, bloquant n°2) : PROPRIÉTÉ de la session pH ouverte —
  // true = ouverte par la branche AUTOMATIQUE, false = par le mode PROGRAMMÉ (ou
  // aucune session). La garde de fermeture hors branche s'excluait auparavant sur
  // `phMode != "scheduled"`, c'est-à-dire sur le mode COURANT : une bascule
  // automatique → programmée en cours de session s'auto-excluait, et la session
  // ouverte par l'automatique se refermait en silence, sans pause mélange. Le
  // critère correct est QUI a ouvert la session, pas le mode du moment.
  // Écrit/lu en loopTask uniquement (patron _stabilizationEndMs).
  bool _phSessionFromAuto = false;
  // feature-073 : accumulateur de dosage SOUS le pouls minimal (ex-seuil C),
  // par CHIMIE ([0]=pH, [1]=ORP). _pulseCarryMl = volume demandé cumulé (mL),
  // borné structurellement à < V_pouls (volume kick-inclus d'une impulsion
  // minimale — invariant de planPulseWithCarry). NON persistant : reset VOULU
  // au reboot, à l'ouverture ET à la fermeture de séance, et sur tout refus
  // (gate, entrées invalides, retour ≥ plancher). _carryPulseLogged = premier
  // tir d'accumulateur de la séance déjà journalisé (anti-spam, log A3).
  // Écrits/lus en loopTask UNIQUEMENT (seul écrivain, patron _stabilizationEndMs
  // — aucun mutex requis).
  float _pulseCarryMl[2] = {0.0f, 0.0f};
  bool _carryPulseLogged[2] = {false, false};
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
  // v2.27.0 (gate pool-chemistry) : le mode maintenance est de durée NON BORNÉE
  // et se termine le plus souvent par un REDÉMARRAGE. Les compteurs journaliers
  // sont donc posés en NVS À L'ENTRÉE — les minutes de circulation, elles,
  // continuent de courir pendant l'épisode, et les perdre laisserait au prochain
  // démarrage un numérateur complet (mL) sur un dénominateur amputé, c'est-à-dire
  // une charge observée SURÉVALUÉE et un plafond de restauration trop généreux.
  void setMaintenanceInhibit(bool inhibit);
  bool isMaintenanceInhibited() const { return maintenanceInhibit; }

  // feature-075 : suspend/rétablit le dosage automatique ET programmé pendant
  // l'assistant de mesure du débit (appelable depuis un handler web async —
  // patron maintenanceInhibit). Contrairement au mode maintenance, la levée
  // est immédiate (pas de reboot requis) et le mode MANUEL reste fonctionnel.
  // update() referme les états de dosage par le chemin de la perte de
  // filtration et journalise la pose/levée (log info one-shot par transition).
  void setFlowAssistSuspend(bool suspend) { _flowAssistSuspend = suspend; }
  bool isFlowAssistSuspended() const { return _flowAssistSuspend; }

  // feature-064 (ADR-0034, prescription pool-chemistry P1) : lecture seule
  // STRICTE de l'utilisabilité de la pompe (0 = pH, 1 = ORP/chlore), posée une
  // seule fois par begin() (feature-063). Ne doit JAMAIS servir à réactiver une
  // pompe (aucun setter — reboot requis). Reflète pumpUsable[] : false dès que
  // ledcAttachChannel() a échoué au boot pour cette pompe.
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

  // DEMANDE l'application des paramètres PID de mqttCfg.regulationSpeed.
  // Appelable depuis N'IMPORTE QUEL contexte, y compris un handler web async :
  // photographie la vitesse en code 8 bits puis pose un drapeau atomique.
  // L'écriture de phPID / de l'intégrale a lieu dans update(), sur loopTask
  // (cf. _phTuningChangeRequested) — jamais depuis async.
  void applyRegulationSpeed();

  // Setters pour PID tuning
  void setPhPID(float kp, float ki, float kd);
  void setOrpPID(float kp, float ki, float kd);

  // Reset des états
  void resetDosingStates();

  // Réinitialise la garde anti-cycling du pH (ex: changement de type de correction)
  void resetPhPauseGuard();

  // Arme le timer de stabilisation pour une pompe donnée (0 = pH, 1 = ORP) :
  // bloque le dosage pendant kStabilizationDurationPhMs / kStabilizationDurationOrpMs
  // (après calibration EZO). v2.34.0 (feature-091) : le réglage utilisateur
  // `stabilizationDelayMin` est RETIRÉ de ce chemin — la stabilisation après
  // reprise de circulation est portée par la purge adaptative de measureValidity.
  void armStabilizationTimer(int pumpIndex);

  // Surcharge sans paramètre : arme les 2 pompes avec leurs durées fixes
  // respectives. Seul site d'appel restant : le passage de minuit (double quota).
  // Les armings « filtration » et « boot mode continu » ont été retirés (feature-091).
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

  // ===== feature-025 : raison de blocage dosage exposée au WS + /data =====
  // Chaîne vide si dosage autorisé, sinon la dernière cause de refus EN COURS
  // (garde de sécurité pH/ORP ou canDose() — cf. _refusalCauseFromGuard).
  // D1 (v2.25.0) : la chaîne est désormais effacée par la RELÂCHE de la
  // surveillance, qui tourne à chaque tick quel que soit le mode de régulation —
  // elle reflète donc l'état COURANT y compris en mode `manual`, où canDose()
  // n'est jamais appelé.
  //
  // R2 (revue v2.25.0) : COPIE BORNÉE vers un tampon de l'APPELANT, en
  // remplacement des ex-getters `String getPhDoseBlockedReason()`. Renvoyer une
  // `String` copiait, depuis `async_tcp`, un objet réassigné par loopTask :
  // lecture de tas potentiellement libéré. Ici, `out` reçoit au plus `outSize-1`
  // octets + NUL, la lecture du tampon source est bornée par sa capacité, et la
  // troncature ne coupe jamais au milieu d'un caractère UTF-8 (accents des
  // messages français → JSON non décodable côté client sinon).
  // Sortie garantie NUL-terminée (chaîne vide si `out`/`outSize` invalides), ce
  // qui préserve le contrat « chaîne vide ⇒ null en JSON » des deux côtés.
  // Course bénigne assumée (patron isAnyPumpEnergized) : pire cas, une lecture
  // déchirée = message partiellement à jour ; le tampon source ne peut pas
  // disparaître. Aucun effet de bord, aucune décision de dosage.
  void copyPhDoseBlockedReason(char* out, size_t outSize) const;
  void copyOrpDoseBlockedReason(char* out, size_t outSize) const;

  // ===== v2.25.1 : CODE de la cause de refus, à côté du texte ================
  // Le texte reste publié (journal, message par défaut, causes de garde portant
  // des valeurs runtime), mais il ne doit plus servir à IDENTIFIER la cause : le
  // client cherchait des fragments de phrase française, si bien qu'un simple
  // « indisponible » de warmup matchait « EZO indisponible » et affichait une
  // panne inexistante. Le code est le seul identifiant stable.
  //   0 (kDoseBlockedCodeNone) = aucun refus en cours → publié `null` ;
  //   1..22                    = valeur de DoseRefusal (contrat figé, dosing_logic.h) ;
  //   250 (kDoseBlockedCodeGuard) = cause posée par une garde de sécurité pH/ORP
  //       ou l'escalade → le client se rabat sur le TEXTE.
  // Course bénigne assumée, identique au tampon de texte : écrit par loopTask, lu
  // en contexte async ; lecture 8 bits atomique sur Xtensa. Pire cas, un code en
  // retard d'un tour de loop sur le texte (ou l'inverse) — un affichage corrigé
  // au rafraîchissement suivant, jamais une décision de dosage.
  uint8_t getPhDoseBlockedCode() const { return _lastRefusalCode[0]; }
  uint8_t getOrpDoseBlockedCode() const { return _lastRefusalCode[1]; }

  // ===== D2 (v2.25.0) : « la butée BRUTE mord » — LECTURE SEULE ==============
  // Aucun client ne peut déduire ces deux états du pH CORRIGÉ publié : c'est
  // tout leur objet. Un offset saisi à tort rend le corrigé rassurant alors que
  // la mesure capteur, elle, est hors limite — seule la butée brute le voit.
  //
  // SANS LATCH : ces getters rendent les drapeaux d'hystérésis recalculés à
  // chaque tick par la surveillance (posés au franchissement, effacés à la
  // relâche). Un latch reproduirait exactement le défaut D1.
  //
  // Course bénigne assumée (patron isAnyPumpEnergized / getPulseMs) : drapeaux
  // écrits par loopTask, lus ici en contexte web async ; lectures 8 bits
  // atomiques sur Xtensa, pire cas un instantané en retard d'un tour de loop.
  // Aucun setter, aucune influence sur une décision de dosage.

  // Butée pH BRUTE applicable au produit DÉCLARÉ : basse (butée acide) en
  // `ph_minus`, haute (butée base) en `ph_plus`. Le produit est lu via le
  // snapshot `_phCorrectionKind` (uint8_t posé par la surveillance) et non via
  // mqttCfg.phCorrectionType : on ne copie pas une String écrite par loopTask
  // depuis un handler async.
  bool isPhRawGuardBlocked() const {
    return (_phCorrectionKind == 1) ? _phRawHardHighBlockActive
                                    : _phRawHardBlockActive;
  }

  // Inhibition ORP due à la BUTÉE BRUTE HAUTE uniquement — pas aux autres motifs
  // (pH corrigé élevé, pH indisponible, mesures non représentatives), qui ont
  // leurs propres champs.
  //
  // Lit le DRAPEAU DE SEUIL et non `_orpPhInhibitKind` (revue v2.25.0, 2ᵉ passe) :
  // le kind est LATCHÉ et re-qualifié en montée de gravité seulement, donc il
  // reste à 3 après le retour du pH brut sous la butée tant que le seuil corrigé
  // maintient l'inhibition. Scénario : bassin à 8,03-8,12 avec offset 0,00 — la
  // butée brute retombe sous 8,05, le seuil corrigé tient, et l'encart
  // d'injection manuelle citerait un pH brut périmé. Le drapeau de seuil, lui,
  // dit l'état courant : c'est ce que le client doit voir.
  bool isOrpPhRawGuardBlocked() const {
    return _orpPhInhibitNow && _orpPhRawThresholdActive;
  }

  // ===== v2.26.0 (C5) : observabilité du TERME INTÉGRAL pH — LECTURE SEULE =====
  // Trois grandeurs, publiées à l'IDENTIQUE sur /data ET sur la trame WS (mêmes
  // règles null, mêmes arrondis) : le motif « champ servi d'un seul côté » a déjà
  // récidivé trois fois dans ce projet (pentes de sonde v2.21.2, champs de
  // calibration v2.23.0, état de blocage de dosage v2.25.0), et il produit pire
  // qu'une absence : un clignotement, `latestSensorData` étant intégralement
  // remplacé par les charges /data sur plusieurs chemins côté client.
  //
  // Course bénigne assumée (patron isAnyPumpEnergized) : l'état PID est écrit par
  // loopTask et lu ici en contexte web async ; les lectures 32 bits sont atomiques
  // sur Xtensa, le pire cas est un instantané en retard d'un tour de loop. Aucun
  // setter, aucune influence sur une décision de dosage.

  // Intégrale accumulée (pH·s). NAN si non finie (→ null en JSON).
  float getPhIntegral() const;
  // Contribution du terme intégral au débit moyen demandé (mL/min) = ki × intégrale.
  // NAN si non finie. Vaut 0 tant que Ki est nul (ORP, ou pH avant armement).
  float getPhIntegralTerm() const;
  // Remplissage de l'anti-windup, en % de integralMax (valeur ABSOLUE : une
  // saturation négative est tout aussi anormale). NAN si integralMax ≤ 0.
  float getPhIntegralPct() const;

  // ===== v2.26.0 (revue, mineure n°1) : VERDICT de saturation, PAS un seuil =====
  // Épisode de saturation OUVERT au sens du module pur : hystérésis 95 % / 85 %
  // sur le rapport `demande / débit soutenable` — JAMAIS sur le remplissage.
  //
  // POURQUOI UN BOOLÉEN ET PAS UN SEUIL CÔTÉ CLIENT. Le client comparait 95 % au
  // REMPLISSAGE (`ph_integral_pct`) alors que les 95 % du firmware portent sur la
  // DEMANDE : deux grandeurs distinctes derrière un seul nombre. Une intégrale
  // proche de son plafond qui tient la charge sans forcer est une régulation
  // SAINE — le client annonçait « vérifiez le bidon » alors qu'aucun épisode
  // n'était ouvert côté firmware. Le seuil et son hystérésis restent donc au
  // FIRMWARE ; le client ne consomme qu'un verdict.
  // Servi à l'IDENTIQUE sur /data et sur la trame WS (parité stricte).
  //
  // Course bénigne assumée (patron isAnyPumpEnergized) : drapeau écrit par
  // loopTask, lu ici en contexte web async ; lecture 8 bits atomique sur Xtensa.
  bool isPhIntegralSaturated() const { return _phIntegral.satActive; }

  // ===== feature-086 : LIMITE D'ACTIONNEUR pH — LECTURE SEULE (diagnostic) ====
  // Quatre accesseurs à publier à l'IDENTIQUE sur /data ET sur la trame WS
  // (parité stricte — 5 récidives du motif « champ servi d'un seul côté »).
  // AUCUN setter, AUCUNE décision de dosage n'en dépend : ils rendent VISIBLE la
  // conséquence de la limite horaire (plafond de débit moyen → maximum
  // journalier), ils ne la modifient pas. Course bénigne assumée (patron
  // isAnyPumpEnergized) : état écrit par loopTask, lu en contexte web async ;
  // lectures 8/32 bits atomiques sur Xtensa, pire cas un instantané en retard
  // d'un tour de loop.

  // Débit moyen SOUTENABLE de la voie pH (mL/min) = débit d'amplitude calibré ×
  // limite horaire configurée (phSustainableFlowFromConfig, source unique,
  // recalcul à la demande — coût nul, pas de cache). NAN si la pompe n'est pas
  // caractérisée ou la valeur non calculable (→ null en JSON : on affiche
  // l'indisponibilité, on n'invente rien).
  float getPhSustainableMlPerMin() const;

  // Maximum journalier DÉLIVRABLE (mL) = soutenable × minutes PROGRAMMÉES de
  // filtration (scheduledMinutesToday, modes `auto` et `manual` — les deux
  // suivent la plage start/end, cf. decideFiltrationRun). NAN si le soutenable
  // est indisponible ou si le planning ne fait pas foi (mode `off`, plage
  // invalide) — jamais de maximum inventé.
  float getPhDailyMaxMl() const;

  // La régulation pH est-elle BORNÉE par la limite horaire EN CE MOMENT ?
  // = épisode de saturation C5 ouvert (hystérésis du module intégral sur la
  // demande, jamais sur le remplissage) ET mesure pH valide — la conjonction
  // évite de servir un verdict FIGÉ sur panne de sonde : sans mesure, la
  // saturation affichée ne décrirait plus rien (distinction demandée par la
  // spec : borné par la limite ≠ bidon vide ≠ panne de sonde).
  bool isPhLimitedByHourlyBudget() const;

  // Sous-dimensionnement de l'actionneur SUSPECTÉ : kPhActuatorLimitConsecutiveDays
  // journées calendaires consécutives saturées (cache _phLimitStreakDays, anneau
  // ph_load7) ET épisode C5 encore ouvert (l'erreur pH reste positive) ET mesure
  // pH valide — même garde exactement qu'isPhLimitedByHourlyBudget : la tenue
  // d'anomalie GÈLE quand la branche d'entretien s'arrête sur mesure invalide,
  // et l'alerte ne doit jamais s'émettre sur un épisode figé par une panne de
  // sonde (condition bloquante n°1 du visa pool-chemistry). Verdict du module
  // PUR phActuatorUndersized — information de diagnostic, pas une anomalie de
  // fonctionnement (spec, livrable A3).
  bool isPhUndersizedSuspected() const;

  // ===== feature-085 : FEEDFORWARD pH — LECTURE SEULE (observabilité) =========
  // Deux grandeurs, à publier à l'IDENTIQUE sur /data ET sur la trame WS (parité
  // stricte — 5 récidives du motif « champ servi d'un seul côté » dans ce
  // projet). AUCUN setter, aucune influence sur une décision de dosage.
  //
  // L'instantané `_ffStatus` n'est rafraîchi que lorsque update() atteint le
  // tick FF (eau présente, hors OTA/maintenance) :
  // les deux accesseurs les CONDITIONNENT donc à l'état de session courant, pour
  // ne jamais servir un « actif » périmé après une sortie anticipée d'update()
  // (filtration coupée, par exemple). Course bénigne assumée (patron
  // isAnyPumpEnergized) : écrits par loopTask, lus en contexte web async ;
  // lectures 8/32 bits atomiques sur Xtensa, pire cas un instantané en retard
  // d'un tour de loop.

  // Débit de base appliqué à la session pH AUTOMATIQUE en cours (mL/min).
  // 0 hors session — le FF ne vit que sous `shouldDose` (A8, structurel).
  //
  // feature-092 — SÉMANTIQUE AJUSTÉE (correction, pas rupture) : la valeur vient
  // désormais de l'instantané de composition (`ff` du RÉGIME APPLIQUÉ) et non
  // plus du verdict brut du tick. Conséquence unique : pendant une prolongation
  // de session à débit NUL (régime `HoldZero`, bassin repassé sous la consigne),
  // ce champ rapporte 0,00 au lieu de la dose de base — ce qui est justement ce
  // que le champ documente (« appliqué »). C'est aussi ce qui rend exacte la
  // reconstitution client `PI servi = demande − FF`. La dose de base APPRISE,
  // elle, reste lisible en permanence par getPhFeedforwardLearnedFlow().
  float getPhFeedforwardFlow() const;

  // Code d'état PhFfStatus (contrat FIGÉ, cf. ph_feedforward_logic.h) :
  //   0 = inactif (aucune session automatique) ; 1 = actif ; 2 = retenu par le
  //   latch de sécurité ; 3 = historique insuffisant (< 3 jours exploitables) ;
  //   4 = quota journalier (≥ 60 % de la limite consommés).
  // Un statut « actif » n'est servi que si une session automatique est ouverte
  // (sinon il est traduit en « inactif » — instantané périmé possible après une
  // sortie anticipée d'update()). Les états 2-4, eux, restent servis hors
  // session : l'UI peut dire POURQUOI le FF ne s'appliquera pas.
  // feature-092 (A2) : les DEUX traductions vivent désormais dans le module pur
  // phFeedforwardServedStatus() — l'ancienne (actif hors session ⇒ inactif) et la
  // nouvelle (historique insuffisant alors que l'anneau porte ≥ 3 journées
  // exploitables ⇒ inactif). Le second cas est le défaut du 03/09 : entre
  // l'archivage de minuit et le premier tick du matin, l'interface annonçait
  // « en apprentissage » pendant sept heures alors que la dose ÉTAIT apprise.
  uint8_t getPhFeedforwardStatus() const;

  // ===== feature-092 : COMPOSITION DU DÉBIT DEMANDÉ — LECTURE SEULE ==========
  // Huit accesseurs de projection, publiés à l'IDENTIQUE sur /data ET sur la
  // trame WS (parité stricte : un champ servi d'un seul côté ne disparaît pas,
  // il CLIGNOTE — `latestSensorData` est intégralement remplacé par les charges
  // /data toutes les ~30 s). AUCUN setter, AUCUNE décision de dosage n'en
  // dépend : ils rendent lisible l'équation que la régulation applique déjà.
  //
  // COURSE BÉNIGNE — patron `isAnyPumpEnergized` : l'instantané est écrit par
  // loopTask (branche automatique) et lu ici en contexte web async. La copie se
  // fait sous spinlock portMUX (patron feature-096, timeToTarget.getSnapshot) :
  // les cinq flottants d'une même composition sont donc COHÉRENTS entre eux, le
  // pire cas étant un instantané en retard d'un tour de loop.
  //
  // AUCUN de ces accesseurs n'appelle getPhDecisionValue() (réservée à loopTask,
  // condition C2 du gate feature-096), ni l'I²C, ni LittleFS, ni la NVS : ce sont
  // des lectures de cache et de l'arithmétique — sûrs dans un handler async.

  // Dose de base APPRISE (mL/min) : min(charge médiane 7 j, 60 % du soutenable).
  // Calculée À LA LECTURE depuis le cache `_phLoadObs` et non depuis un
  // instantané du tick — c'est tout l'objet du champ : le tick ne tourne que sous
  // filtration hors purge, alors que la valeur apprise est vraie en permanence.
  // NAN (→ null) si l'historique porte moins de kPhIntegralRestoreMinHistoryDays
  // journées exploitables ou si le débit soutenable est indisponible. Une charge
  // médiane NULLE avec 3 journées exploitables rend 0,00 : c'est une observation
  // légitime, pas une indisponibilité.
  float getPhFeedforwardLearnedFlow() const;

  // Termes de la composition du débit pH demandé (mL/min), tels que le tick les a
  // calculés au dernier tour où la branche automatique a évalué une demande :
  //   getPhPTermFlow()   = kp·erreur, SIGNÉ (négatif = pH sous la consigne), 0
  //                        dans la bande morte, JAMAIS borné ;
  //   getPhITermFlow()   = ki·intégrale SERVI (0 dans la bande morte) — distinct
  //                        de getPhIntegralTerm(), qui est le terme BRUT et reste
  //                        le contrat externe historique ;
  //   getPhDemandFlow()  = demande totale bornée = clamp(PI servi + FF, 0, ampFlow).
  // NAN (→ null) dès que la porte de validité des mesures est fermée (filtration
  // arrêtée, purge en cours) : sans mesure, l'écart courant n'EXISTE pas — on
  // affiche une absence, jamais un 0 qui se lirait « pH à la consigne ».
  // getPhPTermFlow() rend également NAN si kp vaut 0 (régulateur inerte) : un
  // 0,00 y annoncerait « pH à la consigne » sur une régulation morte.
  float getPhPTermFlow() const;
  float getPhITermFlow() const;
  float getPhDemandFlow() const;

  // Mêmes règles pour l'ORP, régulation P-PURE (Ki forcé à 0, aucun feedforward) :
  // la demande VAUT le terme proportionnel borné. Les deux champs sont servis
  // malgré la redondance apparente — c'est ce qui rend la carte ORP lisible avec
  // le même composant que la carte pH, et ce qui montre POURQUOI le chlore est
  // régulé plus simplement.
  float getOrpPTermFlow() const;
  float getOrpDemandFlow() const;

  // Débit moyen SOUTENABLE de la voie ORP (mL/min) = débit d'amplitude calibré de
  // la pompe ORP × limite horaire ORP configurée (orpSustainableFlowFromConfig,
  // source unique, recalcul à la demande). NAN si la pompe n'est pas caractérisée.
  // Miroir exact de getPhSustainableMlPerMin().
  float getOrpSustainableMlPerMin() const;

  // La régulation ORP est-elle BORNÉE par sa limite horaire EN CE MOMENT ?
  // Homologue d'isPhLimitedByHourlyBudget(), qui n'avait pas d'équivalent chlore
  // alors que c'est la voie qui rencontre le cas TOUS LES JOURS sur cet appareil.
  // VERDICT, jamais une comparaison laissée au client — vrai dès que l'une des
  // deux faces du MÊME phénomène est constatée :
  //   - la fenêtre horaire est ÉPUISÉE (usedMs ≥ limite : le dosage est suspendu
  //     jusqu'au prochain cycle, c'est le warn « Limite horaire ORP atteinte ») ;
  //   - ou la DEMANDE atteint le débit moyen soutenable (elle videra la fenêtre :
  //     même cause, constatée plus tôt).
  // Faux si la limite est désactivée (≤ 0 ou ≥ 60 min/h) : sans limite, rien ne
  // borne. Faux aussi quand la porte de validité des mesures est fermée — même
  // fail-closed que le pH : la demande est alors GELÉE, et servir un verdict figé
  // ferait annoncer « bridé » sur une filtration à l'arrêt.
  // Il n'existe PAS de terme intégral ORP : la demande est P-pure, donc aucune
  // hystérésis d'épisode ici (contrairement au pH, dont le verdict vient du
  // module ph_integral_logic). Au voisinage exact du plafond, la note « plafonnée »
  // peut donc apparaître et disparaître d'un rafraîchissement à l'autre.
  bool isOrpLimitedByHourlyBudget() const;

  // ===== feature-096 : grandeur de DÉCISION pH, en lecture seule ==============
  // Valeur du signal lent qui décide de l'ouverture de session pH (moyenne
  // glissante de kPhDecisionWindowMs de la filtrée). NAN tant que la fenêtre
  // n'est pas pleine — « absence », jamais 0 ni « à la consigne ».
  //
  // POURQUOI LA VALEUR ET NON L'ERREUR : _phDecisionError est calculée par le
  // produit (`ph_plus` ⇒ consigne − valeur), son signe basculerait donc avec le
  // produit employé. L'instrument qui la consomme calcule lui-même
  // e0 = valeur − consigne, définition stable quel que soit le produit.
  //
  // loopTask UNIQUEMENT (elle lit l'état vivant du filtre de décision, écrit par
  // update()) : INTERDITE aux handlers AsyncWebServer, qui lisent l'instantané
  // mémorisé par l'indicateur (condition C2 du gate pool-chemistry).
  //
  // SENS UNIQUE : cet accesseur SORT de la régulation. Aucune sortie de
  // l'indicateur ne doit jamais entrer ici — la garde est un test de grep
  // bidirectionnel sur ce fichier.
  float getPhDecisionValue() const {
    return _phDecision.primed ? _phDecision.value : NAN;
  }

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
