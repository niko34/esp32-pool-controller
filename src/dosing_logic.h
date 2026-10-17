#ifndef DOSING_LOGIC_H
#define DOSING_LOGIC_H

// =============================================================================
// dosing_logic — Décision de dosage PURE (feature-036, characterization refactor)
// =============================================================================
// Logique de décision « peut-on doser ? » extraite de pump_controller pour la
// rendre testable en natif (sans ESP32, sans I²C, sans FreeRTOS).
//
// INVARIANT DIRECTEUR : ce module ne change AUCUN comportement de dosage. Il
// reproduit EXACTEMENT l'ordre des gardes 2→15 de canDose(), fail-closed strict.
// Toute la collecte des entrées (globals, I²C, millis(), historique anti-rafale)
// et le formatage français des causes de refus restent dans la coquille
// pump_controller.cpp.
//
// CONTRAINTE : pas d'Arduino.h, pas de FreeRTOS, pas de <vector>/<FS.h>/<String>.
// On utilise les en-têtes C <stdint.h>/<math.h> (et non <cstdint>/<cmath>) : la
// libc++ n'est pas disponible sur l'hôte de CI/dev (cf. test/native_shim/Arduino.h),
// or le module doit compiler en natif. isnan() suffit pour le test NaN.
// =============================================================================

#include <stdint.h>
#include <math.h>
#include <string.h>

// =============================================================================
// Mode d'installation (feature-056) — décrit le câblage réel et pilote de façon
// cohérente : la présence d'eau (garde dosage), le pilotage du relais filtration
// et l'horizon de répartition scheduled. Enum PUR partagé par config.h (aucun
// Arduino.h ici). Sérialisation JSON/WS/UART STABLE (ne JAMAIS renuméroter) :
//   ManagedFiltration   -> "managed"   (ancien "pilote" + filtrationCfg.enabled)
//   PoweredByFiltration -> "powered"   (ancien "continu")
//   ExternalFiltration  -> "external"  (NOUVEAU : filtration tierce signalée)
// =============================================================================
enum class InstallMode : uint8_t {
  ManagedFiltration = 0,    // PoolController pilote le relais filtration
  PoweredByFiltration = 1,  // PC alimenté par la phase filtration → eau présumée présente
  ExternalFiltration = 2    // filtration tierce, état signalé via HTTP/MQTT (fail-safe OFF)
};

// Source retenue pour la présence d'eau (diagnostic / WS).
enum class WaterSource : uint8_t {
  FiltrationCommanded,  // Managed : état commandé par PoolController
  PoweredAssumed,       // Powered : présumé par câblage
  ExternalSignal        // External : dernier signal externe
};

// Entrées POD de la résolution « eau présente ? ». Tous les champs collectés par
// la coquille (filtration.*) AVANT l'appel. age = (now - lastMs) non signé,
// calculé wrap-safe dans la coquille (jamais ici).
struct WaterPresenceInputs {
  InstallMode mode;
  bool filtrationCommandedOn;    // filtration.isRunning() (Managed)
  bool externalSignalOn;         // dernier état signalé (External)
  bool externalSignalKnown;      // au moins un signal reçu depuis le boot (External)
  uint32_t externalSignalAgeMs;  // âge du dernier signal externe (now - lastMs)
  uint32_t externalStaleMs;      // fraîcheur max tolérée (kExternalFiltrationStaleMs)
};

// Verdict de présence d'eau. `stale` : mode External, signal connu mais périmé.
struct WaterPresence {
  bool waterPresent;
  WaterSource source;
  bool stale;
};

// Résout la présence d'eau selon le mode d'installation. Fail-closed strict
// (condition pool-chemistry #2 : doute = refus).
//   Managed  → waterPresent = filtrationCommandedOn ;
//   Powered  → waterPresent = true (eau présumée présente) ;
//   External → waterPresent = externalSignalOn && externalSignalKnown &&
//              externalSignalAgeMs <= externalStaleMs (on teste `known` AVANT
//              l'âge), sinon false ;
//   mode inconnu → false.
WaterPresence resolveWaterPresent(const WaterPresenceInputs& in);

// Migration de l'ancien schéma (regulationMode + filtrationCfg.enabled) vers
// InstallMode : "continu" → PoweredByFiltration ; sinon → ManagedFiltration.
// Ne produit JAMAIS ExternalFiltration (mode nouveau, jamais migré).
InstallMode migrateInstallMode(const char* regMode, bool filtrationEnabled);

// =============================================================================
// État INTERNE du filtre capteur (v2.25.1) — discriminant des causes de refus
// =============================================================================
// SensorFilter::ready() est faux pour QUATRE raisons physiquement distinctes
// (fenêtre incomplète, aucune mesure acceptée / trop ancienne, capteur figé,
// porte d'alimentation fermée). La chaîne d'observabilité les confondait dans un
// seul libellé « warmup / EZO injoignable » : une VRAIE panne de sonde était donc
// présentée à l'utilisateur comme une stabilisation normale.
//
// Ce discriminant porte sur l'ÉTAT INTERNE DU FILTRE, jamais sur `getPhRaw()` :
// le cache brut est alimenté par TOUTE lecture (y compris hors plage), et il
// reste donc frais sur une sonde figée ou un EZO débitant du garbage — il
// conclurait « warmup » sur une panne, soit exactement le défaut corrigé.
//
// Enum d'ENTRÉE (non sérialisé — seul DoseRefusal est un contrat de transport).
enum class DoseFilterState : uint8_t {
  Unknown = 0,     // état non collecté → AUCUNE attribution de cause (voir Note)
  Ready = 1,       // filtre prêt (ready() == true)
  Warmup = 2,      // fenêtre centrale incomplète (_bufCount < centralWindow)
  Frozen = 3,      // capteur figé (frozen(), feature-022) — PANNE
  Stale = 4,       // aucune mesure acceptée, ou dernière trop ancienne (ageMs > maxAgeMs) — PANNE
  GateClosed = 5   // porte d'alimentation fermée : mesures déclarées NON représentatives
};

// Note (fail-closed d'OBSERVABILITÉ) : `Unknown` ne prétend RIEN. Un site de
// collecte qui oublierait de renseigner l'état retombe sur les causes génériques
// ReadingNaN / FilterNotReady, dont les libellés disent « cause indéterminée ».
// Le pire cas est donc une cause imprécise, JAMAIS une panne matérielle
// maquillée en état normal.

// Compose l'état de filtre depuis les quatre primitives de SensorFilter.
// PRIORITÉ (verrouillée ici, unique lieu de décision) : les PANNES d'abord, les
// états normaux ensuite — un défaut ne doit jamais être masqué par un état
// bénin concomitant :
//   frozen → Frozen ; sinon gateClosed → GateClosed ; sinon stale → Stale ;
//   sinon warmingUp → Warmup ; sinon Ready.
// `gateClosed` passe AVANT `stale` : porte fermée, l'âge croît par conception
// (aucun échantillon n'est ingéré) — l'annoncer « périmé » accuserait la sonde
// d'un blocage voulu par le système.
DoseFilterState resolveFilterState(bool frozen, bool gateClosed, bool stale,
                                   bool warmingUp);

// Cause de refus de dosage (énum pur). L'ordre des valeurs suit l'ordre des
// gardes de canDose() ; 15-18 sont des QUALIFICATIONS de la garde 3, ajoutées
// en v2.25.1.
//
// ⚠️ CONTRAT DE SÉRIALISATION (v2.25.1) : ces valeurs sont exposées telles
// quelles par `phDoseBlockedCode` / `orpDoseBlockedCode` (/data + WebSocket) et
// traduites par une table côté client. Valeurs FIXÉES EXPLICITEMENT :
// ne JAMAIS renuméroter, ne JAMAIS insérer au milieu — toute nouvelle cause
// s'AJOUTE À LA FIN (prochaine valeur disponible : 24). Un décalage ferait
// afficher une mauvaise cause, soit précisément la classe de défaut que ce
// code corrige (précédent : InstallMode). La complétude est verrouillée
// valeur par valeur par les static_assert de pump_controller.cpp (switch
// énum → libellé français).
//
// feature-152 (refonte de la chaîne de mesure) : FilterNotReady(4),
// FilterUnstable(5), FilterWarmup(19), FilterFrozen(20), FilterStale(21) et
// FilterGateClosed(22) sont RETIRÉS — la garde séparée « filtre prêt » / «
// capteur instable » (3b/3c de l'ancien canDose()) a disparu : la VALEUR
// OFFICIELLE de sensor_filter encapsule désormais directement warmup, sonde
// figée, péremption et porte fermée (NaN dans tous ces cas). Une seule garde
// (isnan(reading)) suffit, qualifiée par ReadingNaN* ci-dessous. Ces six
// numéros ne sont JAMAIS recyclés — un appareil non mis à jour afficherait
// l'ancien libellé sur un code réattribué.
enum class DoseRefusal : uint8_t {
  None = 0,
  WatchdogInactive = 1,        // watchdog inactif
  FiltrationOff = 2,           // pas de présence d'eau (resolveWaterPresent, selon le mode d'installation)
  ReadingNaN = 3,              // valeur officielle NaN, état de filtre indéterminé
  // 4, 5 : RETIRÉS (feature-152), ne jamais recycler — voir note ci-dessus.
  CalibrationInsufficient = 6, // calibration insuffisante (cal=X, requis=Y)
  StabilizationActive = 7,     // stabilisation post-calibration en cours
  MixingActive = 8,            // pause mélange en cours
  ModeNotAutomatic = 9,        // mode régulation != automatic (X)
  DailyLimit = 10,             // limite journalière atteinte
  HourlyLimit = 11,            // limite horaire atteinte
  CyclesPerDay = 12,           // limite cycles/jour atteinte
  BurstPerMinute = 13,         // anti-rafale : N cycles dans la dernière minute
  BurstPer15Min = 14,          // anti-rafale : N cycles dans les 15 dernières minutes
  // --- v2.25.1 : qualification de la garde 3 (valeur officielle NaN) --------
  ReadingNaNWarmup = 15,       // fenêtre de lissage en cours de remplissage
  ReadingNaNFrozen = 16,       // capteur figé (PANNE)
  ReadingNaNStale = 17,        // aucune mesure acceptée / trop ancienne (PANNE)
  ReadingNaNGateClosed = 18,  // mesures déclarées non représentatives (purge/eau à l'arrêt)
  // 19, 20, 21, 22 : RETIRÉS (feature-152), ne jamais recycler.
  // 23 (CalibrationHold) : RETIRÉ bug-160 (volet 2, REFONTE 2026-10-13) — le
  // mécanisme de bail (cal_hold_logic) qui le justifiait est ABANDONNÉ au
  // profit d'une bascule explicite de régulation (mqttCfg.ph/orpRegulationMode
  // == "manual"). JAMAIS recyclé.
  // feature-163 (gate pool-chemistry G2) : AJOUTÉ EN FIN (valeur 24, cf.
  // commentaire ci-dessus) — ⛔ aucune renumérotation des valeurs existantes.
  // Évaluée au RANG 3 dans evaluateDose() (immédiatement après la présence
  // d'eau, AVANT la garde NaN) : le rang d'évaluation et l'ordinal de l'énum
  // sont deux axes INDÉPENDANTS, même patron que ManualInjectRefusal ci-dessous.
  CalibrationSessionOpen = 24  // session de calibration OUVERTE (mqttCfg.calSessionOpen)
};

// Entrées POD de la décision de dosage. Tous les champs sont collectés par la
// coquille canDose() depuis les globals AVANT l'appel à evaluateDose().
struct DoseInputs {
  bool watchdogActive;          // esp_task_wdt_status(NULL) == ESP_OK
  bool waterPresent;            // feature-056 : source UNIQUE resolveWaterPresent()
                                // (Managed=commandé, Powered=présumé, External=signal frais)
  // feature-163 (gate G2/G6) : session d'étalonnage OUVERTE (mqttCfg.calSessionOpen,
  // PERSISTÉ — cf. config.h). INITIALISEUR DE MEMBRE (même patron que filterState
  // ci-dessous) : les fixtures de test existantes construisent `DoseInputs in;`
  // sans valeur-initialiser l'agrégat et ne renseignent pas ce champ — sans ce
  // défaut à false, elles liraient une valeur INDÉTERMINÉE et pourraient refuser
  // le dosage de façon aléatoire. La collecte réelle (canDose) l'écrase toujours
  // explicitement.
  bool calSessionOpen = false;
  // feature-152 : VALEUR OFFICIELLE de sensor_filter — sensors.getPhFiltered() /
  // getOrpFiltered(). NaN encapsule déjà warmup, sonde figée, péremption et
  // porte fermée : une seule garde (isnan) suffit désormais (les anciennes
  // gardes 3b « filtre prêt » et 3c « capteur instable » ont disparu avec les
  // champs filterReady/filterUnstable qui les alimentaient).
  float reading;
  // v2.25.1 — état interne du filtre, UNIQUEMENT pour QUALIFIER la cause de
  // refus de la garde 3 (aucune condition ajoutée : le verdict ne le lit
  // jamais). INITIALISEUR DE MEMBRE OBLIGATOIRE : `DoseInputs in;` (canDose) ne
  // value-initialise PAS l'agrégat — sans ce défaut, un site de collecte ou une
  // fixture de test lirait une valeur indéterminée. `Unknown` = aucune
  // attribution de cause (fail-closed d'observabilité, cf. DoseFilterState).
  DoseFilterState filterState = DoseFilterState::Unknown;
  int calPoints;                // points de calibration cachés (-1 si EZO injoignable)
  int requiredPoints;           // 2 (pH) ou 1 (ORP)
  bool stabilizationActive;     // isStabilizationTimerActive(pumpIndex)
  bool mixingActive;            // isPhMixingDelayActive / isOrpMixingDelayActive
  bool modeAutomatic;           // phRegulationMode / orpRegulationMode == "automatic"
  float dailyInjectedMl;        // safetyLimits.dailyPhInjectedMl / dailyOrpInjectedMl
  float maxDailyMl;             // maxPhMlPerDay / maxChlorineMlPerDay
  unsigned long usedMs;         // phDosingState.usedMs / orpDosingState.usedMs
  unsigned long hourlyLimitMs;  // limitMin*60000 ; 0 = pas de limite horaire
  unsigned int cyclesToday;     // phDosingState.cyclesToday / orpDosingState.cyclesToday
  unsigned int maxCyclesPerDay; // pumpProtection.maxCyclesPerDay
  int cyclesLastMin;            // countRecentDosingCycles(pumpIndex, 60000)
  int maxCyclesPerMin;          // kMaxDosingCyclesPerMinute
  int cyclesLast15Min;          // countRecentDosingCycles(pumpIndex, 900000)
  int maxCyclesPer15Min;        // kMaxDosingCyclesPer15Min
};

// Verdict de dosage : allowed=true ssi cause==None.
struct DoseDecision {
  bool allowed;
  DoseRefusal cause;
};

// Reproduit EXACTEMENT l'ordre des gardes 2→15 de canDose() (la garde 1,
// pumpIndex invalide, reste dans la coquille). Première garde en échec →
// cause correspondante ; sinon { true, None }. Fail-closed strict.
DoseDecision evaluateDose(const DoseInputs& in);

// Hystérésis de démarrage (extrait pur de shouldStartDosing).
// true ssi cyclesToday < maxCyclesPerDay ET error > startThreshold.
bool shouldStartDosingPure(float error, float startThreshold,
                           unsigned int cyclesToday, unsigned int maxCyclesPerDay);

// Hystérésis de poursuite (extrait pur de shouldContinueDosing).
// true si runTimeMs < minInjectionTimeMs (force poursuite — temps minimum
// d'injection) OU error > stopThreshold.
bool shouldContinueDosingPure(float error, float stopThreshold,
                              unsigned long runTimeMs, unsigned long minInjectionTimeMs);

// =============================================================================
// Comptabilisation d'un démarrage de cycle — PURE (bug-dosage-fantome, défaut n°2)
// =============================================================================
// Seule sortie utile d'un tour de régulation automatique : faut-il comptabiliser
// un démarrage de cycle ?
//
// Un champ `latchActive` a existé ici et a été RETIRÉ (revue 2.20.5) : il valait
// trivialement `flowActive`, la production ne le lisait pas (elle latche sur son
// propre `phActive`/`orpActive`), et seul le test s'en servait. Un test qui
// s'appuie sur un champ mort en production donne l'illusion de couvrir le latch
// tout en pouvant en diverger silencieusement.
struct CycleStartAccounting {
  bool countCycleStart;  // cyclesToday++, ring anti-rafale, log « Démarrage dosage »
};

// INVARIANT VERROUILLÉ (bug-dosage-fantome, défaut n°2, prescription
// pool-chemistry P5) : un cycle est comptabilisé sur le MÊME prédicat que celui
// qui latche `active` — donc « cycle compté ⇔ cycle réellement démarré ».
//
// Pourquoi : computePID rend TOUJOURS 0 au premier tour d'un cycle quand le
// cycle précédent est loin (dt > 10 s → delta jugé aberrant, rejeté). L'ancien
// code comptabilisait sur la seule DÉCISION de doser : `active` ne se latchait
// pas (débit nul), le tour suivant repassait par shouldStartDosing et comptait un
// SECOND cycle — d'où deux lignes de journal, deux entrées dans le ring
// anti-rafale et un budget cycles/jour réellement divisé par deux.
// On ne synthétise DÉLIBÉRÉMENT aucun dt (piège latent si ki devenait non nul) :
// c'est la comptabilisation qui attend le débit, pas le PID qu'on force.
//
//   wasActive    : DosingState.active à l'entrée du tour
//   startDecided : shouldStartDosing a dit oui CE tour (faux si déjà actif)
//   flowActive   : un duty non nul a réellement été commandé ce tour
CycleStartAccounting evaluateCycleStartAccounting(bool wasActive, bool startDecided,
                                                  bool flowActive);

// =============================================================================
// Comptabilisation d'un DÉMARRAGE de pompe — PURE (feature-110)
// =============================================================================
// Invariant JUMEAU de evaluateCycleStartAccounting ci-dessus, mais un cran plus
// bas : on ne compte pas des CYCLES de régulation, on compte des DÉMARRAGES
// PHYSIQUES de pompe (transitions OFF→ON du duty), c'est-à-dire exactement les
// événements auxquels se paie la perte fixe d'amorçage (relaxation du tube,
// ré-occlusion, reprise d'amorçage — feature-078).
//
// POURQUOI ICI ET PAS SUR L'IMPULSION : le modèle de biais s'écrit
// `V_compté = V_réel + N·δ`. Il n'a de sens que si `N` porte sur le MÊME
// ensemble d'événements que `V`. Le seul endroit où cette identité est
// STRUCTURELLE est la branche de accountPumpVolume() qui crédite le volume :
// un démarrage compté ⇔ un volume crédité par ce démarrage. Compter
// `plan.carryFired` aurait compté des SÉANCES (drapeau levé une seule fois par
// séance, et jamais en régime proportionnel direct) — `N` massivement
// sous-estimé, donc `δ` absurdement grand, donc un correctif calé de travers
// dans le sens qui RELÂCHE les quotas.
//
// HYGIÈNE DE LA MESURE (countAnomaly) : le volume d'un tick est crédité par
// attribution ARRIÈRE sur l'intervalle [lastSafetyTimestamp, now]. Un tick
// anormalement long (loop bloquée, OTA, I²C en souffrance) crédite donc un
// volume calculé sur un intervalle dont on ne sait pas si la pompe a réellement
// tourné tout du long. Ces ticks ne sont NI rejetés NI corrigés (la
// comptabilité du volume est intouchée par feature-110) : ils sont COMPTÉS à
// part, avec leur volume, pour que le relevé de bidon puisse être disqualifié
// s'ils pèsent trop. Sans cet indicateur, une campagne polluée serait
// indiscernable d'une campagne propre.
struct PumpStartAccounting {
  bool countStart;    // front OFF→ON : incrémenter le compteur de démarrages
  bool countAnomaly;  // le tick crédité était anormalement long (hygiène)
};

// Décide, pour UN tour de accountPumpVolume(), s'il faut compter un démarrage
// et/ou un tick anormal.
//   prevOn          : pompe alimentée au tour PRÉCÉDENT (_acctPumpOn[chem])
//   onNow           : pompe alimentée au tour COURANT (pumpDuty > 0)
//   creditedDeltaMs : durée du delta réellement CRÉDITÉ ce tour (0 = aucun
//                     crédit : premier tour, pompe éteinte, delta nul)
//   anomalyDeltaMs  : seuil d'anomalie (kAcctDeltaAnomalyMs) ; 0 désactive
//
// countStart   = !prevOn && onNow          (front strict, un seul par session)
// countAnomaly = anomalyDeltaMs != 0 && creditedDeltaMs > anomalyDeltaMs
//                (frontière > STRICTE : un delta pile au seuil est normal)
//
// Aucun débordement possible ici : la saturation est la responsabilité de
// saturatingInc(), appliquée par la coquille au moment du crédit.
PumpStartAccounting evaluatePumpStartAccounting(bool prevOn, bool onNow,
                                                uint32_t creditedDeltaMs,
                                                uint32_t anomalyDeltaMs);

// Incrément SATURANT sur 32 bits : à UINT32_MAX, la valeur reste à UINT32_MAX.
// Elle ne repart JAMAIS à 0 — un compteur qui reboucle donnerait un rapport
// V/N silencieusement faux (et, à 4,29 milliards de démarrages, un compteur
// figé est un aveu lisible, pas une erreur de mesure).
uint32_t saturatingInc(uint32_t v);

// =============================================================================
// Sanitization config (feature-060) — HORS des gardes de dosage
// =============================================================================
// Sanitization config (PAS une garde de dosage) : borne maxCyclesPerDay dans
// [kCyclesPerDayMin, kCyclesPerDayMax]. wasClamped (optionnel) → true si la
// valeur demandée a été modifiée (les appelants loggent le warn).
unsigned int clampCyclesPerDay(long requested, bool* wasClamped = nullptr);

// Résultat pur d'un pas PID : débit final borné + nouvel état PID (intégrale,
// dernière erreur) renvoyés explicitement, jamais lus/écrits en global.
struct PidResult {
  float flow;       // débit FINAL borné [minFlow, maxFlow], 0 dans la zone morte / sortie négative
  float integral;   // intégrale après mise à jour anti-windup (inchangée si gelée ou deadband)
  float lastError;  // erreur courante (= error), à recopier dans pid.lastError
};

// Cœur PID PUR extrait de PumpControllerClass::computePID (feature-037,
// characterization refactor — AUCUN changement de comportement).
//
// Reproduit EXACTEMENT, dans cet ordre :
//   1. deadband STRICT : inDeadband = fabsf(error) < deadband ;
//   2. si inDeadband → { flow:0, integral inchangée, lastError:error } ;
//   3. sinon allowIntegration = !freezeIntegral → integral += error*dtSec puis
//      bornage ±integralMax (clamp haut ET bas) ; gelée si freezeIntegral ;
//   4. output = kp*error + ki*integral + kd*(error - prevError)/dtSec ;
//   5. if (output < 0) output = 0 ;
//   6. flow = clampf(output, minFlow, maxFlow) (bornage final, déplacé ici
//      depuis la coquille — feature-037 Option Y ; équivalent au constrain()
//      Arduino externe d'origine, réécrit en clampf() car constrain indispo en natif).
//
// L'état PID (integral, prevError) est passé en paramètre et renvoyé ; la
// coquille fournit dtSec (calculé depuis millis) et le flag freezeIntegral.
PidResult computePidPure(float kp, float ki, float kd,
                         float error, float prevError, float integral,
                         float dtSec, float integralMax, float deadband,
                         float minFlow, float maxFlow, bool freezeIntegral);

// =============================================================================
// feature-092 — COMPOSITION DU DÉBIT DEMANDÉ (observabilité, logique PURE)
// =============================================================================
// À QUOI CE MODULE SERT — ET À QUOI IL NE SERT PAS. La carte « Débit
// d'injection » doit pouvoir dire, sans rien deviner : « la régulation demande
// tant, composé de la dose de base apprise + l'écart courant + le rattrapage
// accumulé, borné par ça ». Les trois termes étaient calculés à l'intérieur du
// tick, dans des expressions non réutilisables ; les reconstituer côté client
// aurait exigé de recopier l'ordre des bornages — c'est-à-dire de recopier une
// règle de DOSAGE hors du firmware, avec une divergence garantie à la première
// évolution.
//
// ORDRE INTERNE : MIROIR LITTÉRAL DU TICK, NON RÉORDONNABLE.
// La borne basse à zéro porte sur le COUPLE (p + i), JAMAIS sur la somme des
// trois termes : le tick réduit d'abord le plafond du PI de la part servie au
// feedforward (`phPiMaxFlow = ampFlow − ff`, borné ≥ 0), borne le PI dans
// [0 ; ce plafond], PUIS ajoute le feedforward et clampe à `ampFlow`.
// Déplacer la borne basse APRÈS l'addition changerait le dosage réel : avec
// ff = 1,41, p = −3,0, i = 0 et régime `Full`, le tick injecte 1,41 mL/min,
// tandis que clamp(p + i + ff, 0, ampFlow) donnerait 0. Ce piège a été attrapé
// en revue ; ne pas le réintroduire.
//
// LA BANDE MORTE SE LIT, ELLE NE SE RECALCULE PAS. `deadband` doit venir du
// verdict de porte de session (phSessionGate, feature-094) : session ouverte
// elle vaut phStopThreshold (0,01), fermée phStartThreshold (0,05). La
// reconstruire ici ou côté appelant depuis les seuils mentirait dans la bande
// ]0,01 ; 0,05] — c'est le mécanisme exact du défaut v2.26.0.
// =============================================================================

// Régime de service du feedforward, traduit par la coquille depuis le verdict de
// porte de session (`kPhGate*`). Ces codes-là restent un CONTRAT INTERNE : ils ne
// sont jamais sérialisés — cet enum est le seul point d'entrée du régime ici.
enum class DoseFlowRegime : uint8_t {
  Full = 0,                 // kPhGateOpened / kPhGateHoldFull : PI + FF servis
  CappedToFeedforward = 1,  // kPhGateHoldCapped : PI annulé, FF seul (prolongation)
  Zero = 2,                 // kPhGateHoldZero : débit NUL (bassin sous la consigne)
};

// Décomposition du débit MOYEN demandé (mL/min). Les termes p et i sont des
// grandeurs PÉDAGOGIQUES signées et NON bornées : c'est ce qui permet à
// l'interface d'afficher « −0,35 mL/min » quand le pH est sous la consigne, là
// où la demande servie, elle, reste ≥ 0.
struct DoseComposition {
  float p          = 0.0f;  // kp·erreur, SIGNÉ, 0 dans la bande morte — NON borné
  float i          = 0.0f;  // ki·intégrale,      0 dans la bande morte — NON borné
  float piServed   = 0.0f;  // clamp(p + i, 0, fmaxf(0, ampFlow − ff)) puis régime
  float ff         = 0.0f;  // contribution FF réellement servie (régime appliqué)
  float demand     = 0.0f;  // clamp(piServed + ff, 0, ampFlow)
  bool  inDeadband = false; // |erreur| < deadband (comparaison STRICTE)
};

// Composition du débit demandé — miroir du tick, à résultat identique.
//   kp, error     : gain proportionnel et erreur SIGNÉE courante (positive =
//                   il faut doser), les mêmes que celles passées à computePidPure ;
//   kiIntegral    : terme intégral DÉJÀ multiplié par ki (mL/min) ;
//   deadband      : bande morte LUE du verdict de porte (jamais recalculée) ;
//   ffServed      : débit de base apprise rendu par le tick feedforward ;
//   ampFlow       : débit d'amplitude calibré de la pompe (plafond dur) ;
//   regime        : régime de service du FF (cf. DoseFlowRegime).
//
// FAIL-CLOSED : `error` non finie ⇒ les cinq flottants à NAN et inDeadband =
// false. L'interface affichera « — » ; elle n'affichera jamais un 0 qui se
// lirait « pH à la consigne » sur une mesure absente.
DoseComposition doseComposition(float kp, float error, float kiIntegral,
                                float deadband, float ffServed, float ampFlow,
                                DoseFlowRegime regime);

// =============================================================================
// Anti-rafale + rollover journalier PURS (feature-039, characterization refactor)
// =============================================================================
// Logique pure extraite de pump_controller (countRecentDosingCycles,
// recordDosingCycleStart, déclencheurs de tickDailyRollover). AUCUN changement
// de comportement : seuils/fenêtres/frontières strictement préservés. La
// coquille fournit millis()/time()/les buffers membres.

// Compte les démarrages de cycle dans la fenêtre [now-windowMs, now].
// Copie exacte de countRecentDosingCycles : slot 0 ignoré (jamais utilisé),
// comptage si (now - ts) <= windowMs.
// frontière <= inclusive volontaire ; wrap millis uint32 (arithmétique non
// signée → fenêtre cohérente au passage 0xFFFFFFFF) ; size doit valoir
// kDosingCycleHistorySize.
int countCyclesInWindow(const uint32_t* history, size_t size, uint32_t now, uint32_t windowMs);

// Écrit `now` au slot idx du ring buffer et renvoie le prochain index circulaire.
size_t recordCycleTimestamp(uint32_t* history, size_t idx, size_t size, uint32_t now);

// Déclencheur rollover par changement de date NTP : vrai ssi une date est déjà
// connue (non vide) ET diffère de la date du jour.
bool shouldRolloverByDate(const char* currentDayDate, const char* todayStr);

// Déclencheur rollover fallback 24 h (heure non synchronisée).
// frontière >= inclusive volontaire ; wrap millis uint32 (arithmétique non signée).
bool shouldRolloverByMillis(uint32_t dayStartMs, uint32_t now);

// =============================================================================
// Injection manuelle gardée — décision PURE (feature-006)
// =============================================================================
// Gardes de sécurité appliquées à `POST /ph|orp/inject/start` UNIQUEMENT.
// Les routes test `/pumpN/on` ne passent PAS par evaluateManualInject : elles
// ne sont protégées que par la garde filtration ; seul le budget horaire
// (usedMs, comptabilisé dans PumpController) couvre aussi ces pompes test.
// Le manuel est volontairement AVEUGLE à la mesure (pas de garde NaN/filtre/
// calibration/mode/mélange) : l'opérateur assume la décision chimique, mais ne
// peut pas dépasser le budget (journalier, horaire, cycles, anti-rafale) ni
// injecter sans eau / pendant une stabilisation post-calibration.
// Ordre des gardes et frontières validés pool-chemistry feature-006 (GO sous
// conditions) — NE PAS réordonner.

// Cause de refus d'injection manuelle. None = injection autorisée.
// ⚠️ bug-160 (volet 2, REFONTE 2026-10-13) — CalibrationHold (ex-RANG 4,
// immédiatement après StabilizationActive, devant AlreadyInjecting) est
// RETIRÉ : le mécanisme de bail qui le justifiait est ABANDONNÉ au profit
// d'une bascule explicite de régulation en "manual" (mqttCfg.ph/
// orpRegulationMode), qui ferme nativement le dosage AUTOMATIQUE/PROGRAMMÉ via
// ModeNotAutomatic (evaluateDose) — l'injection MANUELLE, elle, est interrompue
// à l'entrée en calibration par la coquille (web_routes_calibration.cpp,
// gate 5 C6), pas par une garde évaluée ici. JAMAIS recyclé.
// ⚠️ CONTRAT DE NUMÉROTATION (bug-161, gate pool-chemistry C1) : valeurs
// EXPLICITES verrouillées par les static_assert de dosing_logic.cpp — ne
// JAMAIS renuméroter, ne JAMAIS insérer au milieu. Toute nouvelle cause
// s'AJOUTE À LA FIN, quel que soit son RANG D'ÉVALUATION dans
// evaluateManualInject() (le rang d'évaluation est documenté par les
// commentaires numérotés de la fonction, indépendant de l'ordinal ci-dessous).
enum class ManualInjectRefusal {
  None = 0,
  WatchdogInactive = 1,      // watchdog inactif (règle absolue : aucun dosage sans watchdog)
  FiltrationOff = 2,         // filtration arrêtée (hors mode continu — exemption côté collecte)
  StabilizationActive = 3,   // stabilisation post-calibration en cours (par pompe)
  AlreadyInjecting = 4,      // une injection manuelle est déjà en cours (double start)
  DailyLimit = 5,            // cumul + volume demandé dépasserait la limite journalière
  HourlyLimit = 6,           // usedMs + durée demandée dépasserait la limite horaire
  MaxCyclesPerDay = 7,       // limite cycles/jour atteinte (compteur partagé avec l'auto)
  BurstPerMinute = 8,        // anti-rafale : fenêtre 1 min (ring partagé avec l'auto)
  BurstPer15Min = 9,         // anti-rafale : fenêtre 15 min (ring partagé avec l'auto)
  // bug-161 (gate C1) : AJOUTÉ EN FIN D'ÉNUMÉRATION (⛔ aucune renumérotation
  // des valeurs ci-dessus) — évalué au RANG 4 dans evaluateManualInject()
  // (immédiatement après StabilizationActive, avant AlreadyInjecting) : le
  // rang d'évaluation et l'ordinal de l'énum sont deux axes INDÉPENDANTS ici.
  AutomaticInjectInProgress = 10, // une injection AUTOMATIQUE est en cours sur cette pompe
  // feature-163 (gate pool-chemistry G2) : AJOUTÉ EN FIN D'ÉNUMÉRATION (⛔
  // aucune renumérotation des valeurs ci-dessus) — évalué au RANG 3 dans
  // evaluateManualInject() (AVANT StabilizationActive, immédiatement après la
  // présence d'eau) : même patron d'indépendance rang/ordinal qu'au-dessus.
  CalibrationSessionOpen = 11    // session de calibration OUVERTE (mqttCfg.calSessionOpen)
};

// Entrées POD collectées par la coquille (web_routes_control) AVANT l'appel.
struct ManualInjectInputs {
  bool watchdogActive;             // esp_task_wdt_status(NULL) == ESP_OK
  bool waterPresent;               // feature-056 : resolveWaterPresent() (source UNIQUE)
  // feature-163 (gate G2/G6) : session d'étalonnage OUVERTE (mqttCfg.calSessionOpen,
  // PERSISTÉ). INITIALISEUR DE MEMBRE : les fixtures existantes construisent
  // `ManualInjectInputs in;` SANS valeur-initialiser l'agrégat (cf. test natif) —
  // sans ce défaut à false, elles liraient une valeur INDÉTERMINÉE. La collecte
  // réelle (web_routes_control.cpp) l'écrase toujours explicitement.
  bool calSessionOpen = false;
  bool stabilizationActive;        // isStabilizationTimerActive(pumpIndex)
  uint32_t stabilizationRemainingS;// secondes restantes (formatage 409 uniquement,
                                   // non utilisé par la décision — recopié tel quel)
  bool alreadyInjecting;           // injection manuelle déjà active sur cette pompe
  float requestedMl;               // volume demandé (déjà borné ≤ 2000 mL côté route)
  float dailyInjectedMl;           // cumul journalier (safetyLimits.daily*InjectedMl)
  float maxDailyMl;                // limite journalière ; ≤ 0 = illimité
  uint32_t usedMs;                 // budget horaire consommé (PARTAGÉ avec l'auto)
  uint32_t hourlyLimitMs;          // limite horaire ; 0 = illimité (convention auto)
  uint32_t requestedDurationMs;    // durée de l'injection demandée
  unsigned int cyclesToday;        // cycles du jour (auto + manuel, pending inclus)
  unsigned int maxCyclesPerDay;    // pumpProtection.maxCyclesPerDay
  int cyclesLastMin;               // getRecentCycles(pumpIndex, 60000)
  int maxCyclesPerMin;             // kMaxDosingCyclesPerMinute
  int cyclesLast15Min;             // getRecentCycles(pumpIndex, 900000)
  int maxCyclesPer15Min;           // kMaxDosingCyclesPer15Min
  // bug-161 (gate C1) : AJOUTÉ EN FIN DE STRUCTURE. Une injection AUTOMATIQUE
  // était auparavant ATTEIGNABLE par-dessus l'injection manuelle sans aucune
  // garde — inatteignable en pratique tant que le bouton manuel restait
  // masqué hors du mode `manual` (app.js), mais ce lot lève ce masquage.
  // PumpController.isPh/OrpDosing() : LECTURE SEULE de phDosingState.active /
  // orpDosingState.active — ne reflète QUE la régulation AUTO (jamais une
  // injection manuelle ni un test pompe, cf. commentaire d'isAnyPumpEnergized
  // dans pump_controller.h).
  bool automaticInjectInProgress;  // PumpController.isPhDosing() / isOrpDosing()
};

// Verdict : allowed=true ssi cause==None. remainingMl = reliquat journalier
// (max(0, maxDailyMl - dailyInjectedMl)) renseigné pour DailyLimit ET pour le
// cas autorisé ; 0 pour les autres causes de refus.
struct ManualInjectDecision {
  bool allowed;
  ManualInjectRefusal cause;
  float remainingMl;
};

// Évalue les gardes d'injection manuelle dans l'ORDRE EXACT validé
// pool-chemistry (condition #1 : watchdog EN PREMIER). Première garde en
// échec → cause correspondante. Fail-closed strict.
ManualInjectDecision evaluateManualInject(const ManualInjectInputs& in);

// =============================================================================
// Répartition scheduled — décision PURE (feature-011)
// =============================================================================
// Répartit le volume quotidien du mode "scheduled" (pH/ORP) par fenêtres de
// `windowMinutes` (kScheduledWindowMinutes = 15 en prod) sur l'horizon de
// filtration restant, borné à minuit. Le volume de fenêtre est RECALCULÉ à
// chaque nouvelle fenêtre depuis l'état courant (auto-correcteur : changement
// de cible en cours de journée, injection manuelle comptée dans
// dailyInjectedMl, retard subi par la limite horaire, redémarrage ESP32).
// Fail-closed strict.
//
// Conditions pool-chemistry feature-011 (bloquantes — NE PAS altérer) :
//   n°2 : horizonMinutes <= 0 → aucune injection (le plancher nWin >= 1 ne
//         s'applique qu'à un horizon strictement positif, jamais via max(1,0)) ;
//   n°3 : doseNow réévalué à CHAQUE tick contre min(stopTargetMl, cible
//         effective) — une baisse de cible/plafond en cours de fenêtre arrête
//         l'injection immédiatement ; remaining et v clampés >= 0 ;
//   n°4 : watchdogActive transmis en entrée et vérifié DANS la fonction pure ;
//   n°5 : le MÊME effectiveFlowMlPerMin sert au bornage horaire, à la durée
//         d'injection et au duty (la coquille fournit une seule variable).
// (Condition n°1 — consultation du ring anti-rafale avant tout démarrage —
// appliquée par la coquille pump_controller, qui possède le ring.)

// Entrées POD collectées par la coquille pump_controller AVANT l'appel.
struct ScheduledDoseInputs {
  int nowMin;                  // minutes locales depuis minuit (heure valide requise)
  int horizonMinutes;          // minutes restantes de la plage (remainingRangeMinutes)
                               // ou 1440-nowMin hors mode Managed ; <=0 → refus
  int windowMinutes;           // taille de fenêtre (kScheduledWindowMinutes = 15)
  float dailyTargetMl;         // mqttCfg.ph/orpDailyTargetMl (cible utilisateur)
  float maxDailyMl;            // plafond sécurité (maxPhMlPerDay / maxChlorineMlPerDay) ; <=0 = sans plafond
  float dailyInjectedMl;       // cumul journalier (auto + manuel, safetyLimits)
  float effectiveFlowMlPerMin; // débit effectif pompe (UNIQUE — condition n°5)
  uint32_t usedMs;             // budget horaire consommé (PARTAGÉ, ADR-0020)
  uint32_t hourlyLimitMs;      // limite horaire ; 0 = illimité (convention auto)
  uint32_t minInjectionTimeMs; // durée min d'injection (anti short-cycling, 30 s)
  bool watchdogActive;         // esp_task_wdt_status(NULL) == ESP_OK (condition n°4)
  // feature-163 (gate pool-chemistry G3/G6) : session d'étalonnage OUVERTE
  // (mqttCfg.calSessionOpen, PERSISTÉ) — placée À CÔTÉ de watchdogActive dans
  // l'entrée fail-closed d'evaluateScheduledDose (même traitement : aucun log
  // dédié, cf. canDose()/evaluateDose() pour la cause détaillée). INITIALISEUR
  // DE MEMBRE : les fixtures existantes construisent `ScheduledDoseInputs in;`
  // SANS valeur-initialiser l'agrégat — sans ce défaut à false, elles liraient
  // une valeur INDÉTERMINÉE. La collecte réelle (pump_controller.cpp) l'écrase
  // toujours explicitement.
  bool calSessionOpen = false;
  int prevWindowIndex;         // index de fenêtre du tick précédent (-1 = aucun)
  float prevStopTargetMl;      // cible d'arrêt cumulée de la fenêtre courante
};

// Verdict de répartition. En refus fail-closed (watchdog/horizon/débit) :
// { false, -1, 0, NAN } — windowIndex=-1 force le recalcul au retour en plage.
struct ScheduledDoseDecision {
  bool doseNow;              // vrai ssi dailyInjectedMl < min(stopTargetMl, cible effective)
  int windowIndex;           // nowMin / windowMinutes (fenêtre absolue du jour)
  float stopTargetMl;        // cumul journalier cible d'arrêt de la fenêtre courante
  float plannedFlowMlPerMin; // débit moyen planifié restant (diagnostic WS) ; NAN si rien à injecter
};

// Décision de répartition scheduled (voir bloc de doc ci-dessus).
ScheduledDoseDecision evaluateScheduledDose(const ScheduledDoseInputs& in);

// =============================================================================
// Mode Boost — cible ORP et limite journalière effectives PURES (feature-053)
// =============================================================================
// Surcouche NON destructive : ces fonctions renvoient les valeurs *effectives*
// consommées par la régulation ORP quand le Boost est actif, sans jamais
// modifier la config persistée. L'effet chimique est STRICTEMENT réservé au mode
// ORP "automatic" (paramètre orpModeAutomatic) — le mode scheduled/manual ne voit
// JAMAIS la valeur boostée (condition pool-chemistry #1).

// Cible ORP effective (mV). Si boost actif ET mode automatic :
//   max(orpTarget, fminf(orpTarget + deltaMv, ceilingMv))
// Le max() protège une cible déjà supérieure au plafond : la cible n'est JAMAIS
// abaissée par le boost (condition pool-chemistry #5). Sinon → orpTarget inchangé.
float effectiveOrpTargetPure(float orpTarget, bool boostActive, bool orpModeAutomatic,
                             float deltaMv, float ceilingMv);

// Limite journalière chlore effective (mL). Si boost actif ET mode automatic :
//   fminf(maxChlorine * factor, hardCapMl)  (borné en dur, condition #1)
// Sinon → maxChlorine inchangée.
float effectiveMaxChlorinePure(float maxChlorine, bool boostActive, bool orpModeAutomatic,
                               float factor, float hardCapMl);

#endif // DOSING_LOGIC_H
