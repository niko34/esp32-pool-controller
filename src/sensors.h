#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "atlas_ezo.h"
#include "constants.h"
#include "sensor_filter.h"

// Rôle attribué à une sonde DS18B20 (feature-020)
enum class SondeRole : uint8_t {
  Unknown = 0, // Sonde détectée mais non identifiée par l'utilisateur
  Water,       // Sonde eau de la piscine (consommée par compensation pH/ORP)
  Circuit      // Sonde de circuit interne (T° boîtier électronique)
};

// Informations d'une sonde DS18B20 détectée sur le bus OneWire (feature-020)
struct SondeInfo {
  uint8_t addr[kSondeAddrLen]; // Adresse ROM 1-Wire 64 bits (unique par sonde)
  float lastTempRaw;           // Dernière température brute lue (°C, NaN si erreur)
  bool present;                // true si sonde détectée et lue avec succès
  SondeRole role;              // Rôle attribué (eau, circuit, ou inconnu)
};

// =============================================================================
// SensorManager — Pilotage capteurs (DS18B20 + Atlas EZO pH/ORP)
// =============================================================================
//
// Architecture (feature-021) :
//   - DS18B20 : multi-sondes (eau + circuit), inchangé feature-020
//   - pH / ORP : modules Atlas EZO Embedded I²C (kEzoPhAddress / kEzoOrpAddress)
//     Calibration stockée DANS le module EZO (NVS interne), pas en NVS ESP32.
//     Compensation T° envoyée via "RT,<temp>" avant chaque "R" (cf. AtlasEzoSensor).
//
// Concurrence :
//   - update() est appelé depuis loopTask (core 1).
//   - getPh() / getOrp() peuvent être appelés depuis loopTask (pump_controller)
//     ou des handlers async (core 0). Les caches `_lastPh` / `_lastOrp` sont
//     des `float` 32 bits alignés : lecture/écriture atomique sur ESP32 (Xtensa
//     LX6) — pas de mutex dédié nécessaire pour ces variables scalaires.
//   - Les commandes longues (calibration, ~1-2 s par appel I²C) sont
//     sérialisées via une queue FreeRTOS (`_ezoQueue`) traitée dans update().
//     Les handlers async appellent `enqueue*()` (< 1 ms) et l'UI observe la
//     transition via WS.
// =============================================================================

class SensorManager {
public:
  // Type de commande EZO différée (file `_ezoQueue`)
  enum class EzoCmdKind {
    CalibratePhMid,    // Cal,mid,7.00
    CalibratePhLow,    // Cal,low,4.00
    CalibrateOrp,      // Cal,<referenceMv>
    ClearPhCal,        // Cal,clear sur pH
    ClearOrpCal,       // Cal,clear sur ORP
    QueryPhSlope       // Slope,? sur pH (feature-024 — re-query auto 24h + post-cal)
  };

  // Requête de commande EZO posée par les handlers async, traitée par update()
  struct EzoCmdRequest {
    EzoCmdKind kind;
    float arg;  // Pour CalibrateOrp : valeur de référence en mV. Sinon ignoré.
  };

  SensorManager();
  ~SensorManager();

  void begin();
  void update(); // Appelé dans loop() — non bloquant côté HTTP

  // ===== API capteurs Atlas EZO (pH / ORP) =====
  // NaN si lecture stale (dernière lecture valide > kSensorStaleTimeoutMs).
  // Atomique (float aligné, ESP32 Xtensa).
  // feature-067 : valeur CORRIGÉE (brut EZO + mqttCfg.phOffset) — cf. contrat ci-dessous.
  float getPh() const;
  // NaN si lecture stale (dernière lecture valide > kSensorStaleTimeoutMs).
  float getOrp() const;

  // ===== feature-025 : chaîne de filtrage pH/ORP (médiane + EMA + rejet pics) =====
  // Le PID auto consomme getPhFiltered()/getOrpFiltered() et exige isPhFilterReady()/
  // isOrpFilterReady() avant tout dosage (fail-closed warmup / EZO injoignable / instable).
  // Tous les getters sont lock-free (lecture dans le SEUL contexte loopTask, comme _lastPh).
  //
  // ===== feature-067 — CONTRAT OFFSET pH (règle mnémotechnique) =====
  //   ⇒ SEULS les accesseurs suffixés `*Raw` renvoient du BRUT capteur.
  // Tous les autres accesseurs pH renvoient la valeur CORRIGÉE
  // (brut + mqttCfg.phOffset, cf. src/ph_offset.h) : getPh(), getPhMedian(),
  // getPhFiltered(). L'offset est appliqué EN SORTIE — le filtre pH travaille
  // toujours sur le brut (latch EMI et seuils de rejet inchangés) et un
  // changement d'offset est visible IMMÉDIATEMENT, sans purge de la fenêtre.
  // NaN in → NaN out : l'offset ne fabrique jamais une mesure.
  float getPhRaw() const;             // Dernière brute pH EZO, SANS offset (NaN si stale)
  float getPhMedian() const;          // Médiane courante pH CORRIGÉE (NaN si pas de donnée)
  float getPhFiltered() const;        // pH filtré EMA CORRIGÉ — valeur PID (NaN si non amorcé)
  // pH filtré EMA BRUT (SANS offset). Réservé aux filets de sécurité chimique
  // qui doivent raisonner sur la mesure réelle du capteur, indépendamment d'un
  // offset éventuellement erroné (planchers de dosage pool-chemistry,
  // kPhRawDosingFloor / kPhRawDosingFloorWide / kPhRawOrpInhibit).
  float getPhFilteredRaw() const;
  bool isPhFilterReady() const;       // true après warmup + dernière mesure valide récente
  bool isPhFilterUnstable() const;    // true si trop de rejets consécutifs
  uint8_t getPhRejectedCount() const; // Compteur glissant de rejets pH

  // v2.20.6 : amplitude (max−min) de la fenêtre médiane PLEINE, NaN sinon.
  // C'est la grandeur qui DÉCIDE du drapeau d'instabilité (comparée à
  // kPhFilterMaxDispersion / kOrpFilterMaxDispersion, latch après
  // kSensorFilterDispersionLatchCycles fenêtres consécutives en dépassement).
  // Elle n'était exposée par AUCUNE route : on pilotait donc une inhibition de
  // dosage sur une valeur que personne ne pouvait observer. Échelle BRUTE, mais
  // une amplitude est de toute façon insensible à l'offset (constante additive).
  float getPhDispersion() const;
  float getOrpDispersion() const;
  // Fenêtres pleines et propres consécutives : lisibilité de l'auto-levée du latch.
  uint8_t getPhDispersionCleanStreak() const;
  uint8_t getOrpDispersionCleanStreak() const;

  // ===== feature-067 : offset logiciel pH =====
  // Recharge la copie locale `_phOffset` depuis `mqttCfg.phOffset` et journalise
  // le changement. À appeler au boot (begin()) et après TOUT enregistrement de la
  // configuration (route HTTP, commande MQTT) — la prise en compte est immédiate.
  // Contexte : loopTask ou handler async (écriture d'un float aligné = atomique).
  // Une valeur hors bornes est neutralisée à 0.00 (fail-safe, cf. isPhOffsetValid).
  void refreshPhOffsetCache();
  // Offset actuellement APPLIQUÉ aux mesures (copie locale, pas la config).
  float getPhOffset() const { return _phOffset; }

  float getOrpRaw() const;
  float getOrpMedian() const;
  float getOrpFiltered() const;
  bool isOrpFilterReady() const;
  bool isOrpFilterUnstable() const;
  uint8_t getOrpRejectedCount() const;

  // ===== feature-022 Passe 2 : détection capteur figé (variance nulle) =====
  // true si kSensorFrozenSamples lectures ACCEPTÉES consécutives sont contenues
  // dans une bande < ½ LSB (kSensorFrozenEpsilonPh/Orp). Un capteur figé rend
  // is*FilterReady() = false → dosage bloqué par la garde FilterNotReady existante.
  bool isPhSensorFrozen() const;
  bool isOrpSensorFrozen() const;
  // true si kTempFrozenSamples lectures DS18B20 valides de la sonde "eau" sont
  // contenues dans une bande < kTempFrozenEpsilonC. Warning-only : AUCUN impact
  // dosage (effets réels : compensation EZO pH + planning auto filtration).
  bool isTemperatureFrozen() const;

  // Reset manuel des filtres (appelé après calibration EZO réussie, ou debug).
  // Repasse le filtre en warmup → dosage auto bloqué tant que la fenêtre centrale
  // (kSensorFilterCentralWindow mesures valides) n'est pas de nouveau pleine.
  // Lève aussi le latch EMI (feature-058).
  // ⚠️ CONTEXTE loopTask UNIQUEMENT (contrat SensorFilter, sensor_filter.h) : ces
  // méthodes écrivent dans le filtre. Depuis un handler async (route HTTP), MQTT
  // ou toute autre tâche → utiliser requestFilterReset().
  void resetPhFilter();
  void resetOrpFilter();

  // Demande de réinitialisation DIFFÉRÉE, par capteur (v2.20.3). Pose un drapeau
  // `volatile` consommé en TÊTE de update() (contexte loopTask) — même pattern que
  // enqueuePhSlopeQuery() pour les commandes EZO. Appelable depuis N'IMPORTE QUEL
  // contexte (async_tcp, mqttTask). Retourne false si aucun filtre n'est demandé.
  // Sélectif : réparer le pH ne sacrifie plus le filtre ORP sain.
  // `origin` : libellé de provenance journalisé (D4). DOIT être un littéral de
  // chaîne / une constante statique — seul le POINTEUR est mémorisé (aucune
  // copie, aucune allocation dans un contexte async).
  bool requestFilterReset(bool ph, bool orp, const char* origin = "inconnue");

  // Latch EMI posé sur le filtre (feature-058). Exposé pour la journalisation de
  // l'état DÉTRUIT par une réinitialisation manuelle (verdict pool-chemistry P6).
  bool isPhFilterLatched() const { return _phFilter.unstableLatched(); }
  bool isOrpFilterLatched() const { return _orpFilter.unstableLatched(); }

  // ===== v2.20.3 (C1) : alarme « filtre non prêt durablement » =====
  // true quand le filtre est resté NON PRÊT pendant plus de
  // kSensorFilterNotReadyAlarmMs alors que la filtration tournait ET que les
  // mesures étaient déclarées REPRÉSENTATIVES — c'est-à-dire quand il n'y a plus
  // AUCUNE régulation sur ce paramètre sans raison légitime. La purge de la
  // chambre de mesure (measuresValid == false) et l'arrêt de circulation ne sont
  // JAMAIS comptés : ce sont des états normaux qui bloquent le dosage par
  // conception. Consommé par /data, le WebSocket, MQTT et l'UI.
  bool isPhFilterNotReadyAlarm() const { return _phNotReadyAlarmed; }
  bool isOrpFilterNotReadyAlarm() const { return _orpNotReadyAlarmed; }

  // Nombre de points de calibration mémorisés dans l'EZO.
  // -1 si module injoignable (erreur I²C ou bus indisponible),
  // 0..3 sinon (Atlas pH supporte 1pt/2pts/3pts, ORP 1pt seul).
  // ⚠️ Lecture I²C bloquante (~900 ms) — réservé aux routes HTTP de diagnostic
  // et à mqttTask. Pour les chemins chauds (loopTask: ws_manager, pump_controller),
  // utiliser `getPhCalibrationPointsCached()` qui retourne `_phCalCachedPoints`
  // sans accès au bus.
  int getPhCalibrationPoints();
  int getOrpCalibrationPoints();

  // Variantes "cache only" — pas d'accès I²C, retourne la dernière valeur connue
  // (mise à jour en begin() puis à chaque calibration via _processEzoQueue).
  // Utilisable depuis n'importe quel contexte temps réel.
  int getPhCalibrationPointsCached() const { return _phCalCachedPoints; }
  int getOrpCalibrationPointsCached() const { return _orpCalCachedPoints; }

  // ===== Enqueue de commandes longues (handlers async safe, < 1 ms) =====
  // Renvoient true si la commande a été placée dans la queue, false sinon
  // (queue pleine ou non initialisée).
  bool enqueueCalibratePhMid();         // Calibration point milieu (pH 7.00)
  bool enqueueCalibratePhLow();         // Calibration point bas (pH 4.00)
  bool enqueueCalibrateOrp(float referenceMv);  // Calibration ORP (référence en mV)
  bool enqueueClearPhCalibration();
  bool enqueueClearOrpCalibration();

  // ===== feature-024 : pente sonde pH =====
  // Acide / base : % de la pente Nernst théorique (idéal 100%).
  // Zero offset : décalage du point isopotentiel en mV (idéal 0).
  // Tous renvoient NaN si :
  //   - aucune query Slope,? n'a encore réussi depuis le boot, OU
  //   - le bus I²C est dégradé (fail streak ≥ kEzoBusFailMaxConsecutive), OU
  //   - pour getPhSlopeZero() uniquement : firmware EZO ne rapporte pas le zéro.
  float getPhSlopeAcid() const;
  float getPhSlopeBase() const;
  float getPhSlopeZero() const;
  // Renvoie l'âge de la dernière query Slope,? réussie (millis() - timestamp).
  // UINT32_MAX si jamais lue avec succès depuis le boot.
  uint32_t getPhSlopeAgeMs() const;
  // Pose une demande de query Slope,? dans la file EZO (dédoublonnée via
  // _phSlopeQueryPending : un appel répété tant que la précédente n'a pas été
  // traitée renverra true mais n'enfilera qu'une seule fois). Retourne false
  // si la queue est pleine ou non initialisée.
  bool enqueuePhSlopeQuery();

  // ===== Diagnostic / état =====
  // True si au moins un EZO a répondu au boot ET qu'au moins une lecture pH
  // ou ORP valide est disponible (cohérent avec gestion fallback EZO débranché).
  bool isInitialized() const;

  // ===== Trace capteur (ring buffer RAM, feature-057) =====
  // Capture chaque cycle EZO (pH brut + pH corrigé + pH lissé + ORP brut + ORP
  // lissé + T° eau). Sert au diagnostic d'oscillation / de lissage (comparaison
  // brut vs filtré) et, depuis feature-067, à distinguer ce qui vient de l'EZO
  // de ce qui vient de l'offset logiciel : les champs pH de la trace sont
  // TOUJOURS bruts (membres internes), sauf `phCorrected`.
  // Buffer glissant ~25 min à kPhOrpSensorIntervalMs = 5 s.
  //
  // Concurrence (MÊME contrat que le cache EZO `_lastPh`/`_lastOrp`) :
  //   - Écrit UNIQUEMENT par loopTask (core 1) via _recordSensorTrace(), à la fin
  //     de _readEzoSensors().
  //   - Lu par un handler AsyncTCP (core 0) via appendSensorTraceJson().
  //   - AUCUN mutex : les champs sont des scalaires alignés. Pire cas = un handler
  //     lit 1 échantillon partiellement mis à jour pendant l'écriture (valeurs
  //     mixées d'un cycle) OU rate le tout dernier échantillon. Pour un outil de
  //     diagnostic non temps-réel, cette incohérence ponctuelle est acceptable.
  struct SensorTraceSample {
    uint32_t ms;          // millis() au moment de la lecture
    float    phRaw;       // pH brut Atlas EZO, SANS offset (NaN si lecture échouée)
    float    phCorrected; // feature-067 : pH brut + offset logiciel (NaN si brut NaN)
    float    phFiltered;  // pH lissé BRUT (médiane + EMA, feature-025 ; NaN si filtre vide)
    float    orpRaw;      // ORP brut Atlas EZO (NaN si lecture échouée)
    float    orpFiltered; // ORP lissé (NaN si filtre vide)
    float    tempEau;     // T° eau calibrée envoyée à l'EZO pour compensation (NaN si sonde absente)
  };
  // Nombre d'échantillons valides actuellement dans le ring buffer.
  size_t getSensorTraceCount() const;
  // Ajoute une PAGE d'échantillons (ordre chronologique, plus ancien d'abord)
  // à `out` : les échantillons [offset, offset+count) — offset 0 = le plus
  // ancien. L'appelant (web_routes_debug.cpp) clamp via computeTracePage() ;
  // un re-clamp défensif interne borne quand même sur le snapshot local de
  // _traceCount (le buffer peut avoir glissé). `out` doit être un JsonArray
  // d'un JsonDocument déjà alloué. Sérialise NaN → null. Arrondis : pH ×1000,
  // ORP ×10, T° ×10.
  void appendSensorTraceJson(JsonArray out, size_t offset, size_t count) const;
  // Vide le ring buffer (démarre une fenêtre d'observation propre).
  void clearSensorTrace();

  // ===== API DS18B20 — Température (feature-020, inchangé) =====
  // Alias rétrocompat de la T° eau, avec fallback gracieux sur la 1ʳᵉ sonde
  // présente tant que l'identification utilisateur n'a pas été faite.
  float getTemperature() const;
  // Retourne la T° calibrée de la sonde "eau" (offset utilisateur appliqué).
  // NaN si sonde "eau" non identifiée OU non détectée.
  float getWaterTemperature() const;
  // Retourne la T° BRUTE de la sonde "eau" (sans `mqttCfg.tempCalibrationOffset`).
  // Utile pour l'UI de calibration T° qui doit calculer un nouvel offset à partir
  // d'une référence externe — évite la formule fragile `calibrated - offset_précédent`.
  // NaN si sonde "eau" non identifiée OU non détectée.
  float getWaterTemperatureRaw() const;
  // Retourne la T° brute de la sonde "circuit" (PAS d'offset, calibration usine).
  float getCircuitTemperature() const;
  bool areSondesIdentified() const;
  int getDetectedSondeCount() const { return _detectedCount; }
  void getDetectedSondeAddresses(uint8_t addrs[kMaxDs18b20Sondes][kSondeAddrLen],
                                 bool matched[kMaxDs18b20Sondes]) const;
  bool identifySonde(const uint8_t addr[kSondeAddrLen], bool isWater);
  void resetSondeIdentification();
  SondeRole getSondeRole(uint8_t index) const {
    return (index < kMaxDs18b20Sondes) ? _sondes[index].role : SondeRole::Unknown;
  }
  float getSondeTempRaw(uint8_t index) const {
    return (index < kMaxDs18b20Sondes) ? _sondes[index].lastTempRaw : NAN;
  }

private:
  // ===== Capteurs DS18B20 =====
  OneWire oneWire;
  DallasTemperature tempSensor;

  SondeInfo _sondes[kMaxDs18b20Sondes];
  uint8_t _detectedCount = 0;

  // Cache rétrocompat eau (alimenté par readDs18b20s())
  float tempValue = NAN;
  float tempRawValue = NAN;

  // ===== Capteurs Atlas EZO =====
  AtlasEzoSensor _phEzo{kEzoPhAddress, "EZO pH"};
  AtlasEzoSensor _orpEzo{kEzoOrpAddress, "EZO ORP"};

  // Cache des dernières lectures valides — accédés sans mutex.
  // Atomique CHAMP PAR CHAMP (float 32 bits aligné, instructions L32I/S32I single-cycle
  // sur Xtensa LX6) mais PAS atomique sur la paire (_lastPh, _lastPhMs) : un getter peut
  // lire la valeur récente avec l'horodatage précédent (ou inverse) pendant 1 cycle si
  // _readEzoSensors() écrit entre les 2 lectures. Impact maximal : 1 cycle (~5 s) de
  // fausse alerte stale ou inverse — fail-safe acceptable pour la régulation chimique.
  float _lastPh = NAN;
  float _lastOrp = NAN;
  uint32_t _lastPhMs = 0;
  uint32_t _lastOrpMs = 0;

  // feature-067 : COPIE locale de mqttCfg.phOffset, rafraîchie à l'enregistrement
  // de la config (refreshPhOffsetCache). Lue dans les getters CHAUDS (loopTask +
  // handlers async/MQTT) : jamais de xSemaphoreTake dans un getter — la copie est
  // un float 32 bits aligné, donc lecture/écriture atomique sur Xtensa LX6
  // (même contrat que _lastPh). Toujours dans [kPhOffsetMin, kPhOffsetMax].
  float _phOffset = 0.0f;

  // ===== feature-025 : filtres pH / ORP =====
  // Alimentés dans _readEzoSensors() à chaque lecture EZO valide (contexte loopTask).
  // Lus par les getters get*Filtered()/is*FilterReady() depuis loopTask uniquement
  // (pump_controller, ws_manager côté loop). Pas de mutex : cf. contrat SensorFilter.
  // feature-058 : fenêtre médiane LONGUE + dispersion (drapeau découplé de la valeur).
  // Ordre positionnel = SensorFilter::Config :
  //   minValue, maxValue, spikeReject, emaAlpha, centralWindow,
  //   maxConsecutiveRejects, maxAgeMs, maxDispersion, frozenSamples, frozenEpsilon,
  //   rearmCoherence, rearmMin, rearmMax (v2.20.3 : sortie de secours).
  SensorFilter _phFilter{SensorFilter::Config{
      kPhFilterMin, kPhFilterMax, kPhFilterSpikeReject, kPhEmaAlpha,
      kSensorFilterCentralWindow, kSensorFilterMaxConsecutiveRejects,
      kSensorFilterMaxAgeMs, kPhFilterMaxDispersion,
      kSensorFrozenSamples, kSensorFrozenEpsilonPh,
      kPhFilterRearmCoherence, kPhFilterRearmMin, kPhFilterRearmMax}};
  SensorFilter _orpFilter{SensorFilter::Config{
      kOrpFilterMin, kOrpFilterMax, kOrpFilterSpikeReject, kOrpEmaAlpha,
      kSensorFilterCentralWindow, kSensorFilterMaxConsecutiveRejects,
      kSensorFilterMaxAgeMs, kOrpFilterMaxDispersion,
      kSensorFrozenSamples, kSensorFrozenEpsilonOrp,
      kOrpFilterRearmCoherence, kOrpFilterRearmMin, kOrpFilterRearmMax}};

  // ===== feature-022 Passe 2 : détecteur figé dédié température =====
  // Alimenté par les lectures DS18B20 VALIDES (brutes, NON arrondies) de la
  // sonde "eau" dans _readDs18b20s(). 900 lectures à 2 s = 30 min.
  FrozenDetector _waterTempFrozen{kTempFrozenSamples, kTempFrozenEpsilonC};

  // Flags edge-triggered pour les logs SENSOR_FROZEN (une seule transition loggée)
  bool _phFrozenLogged = false;
  bool _orpFrozenLogged = false;
  bool _tempFrozenLogged = false;

  // v2.20.3 — flags edge-triggered des refus de ré-amorçage (le module pur ne peut
  // pas logger : rearmDenied() est COLLANT, la coquille ne journalise qu'à la
  // transition et réarme son flag quand le motif retombe à 0).
  bool _phRearmDeniedLogged = false;
  bool _orpRearmDeniedLogged = false;

  // v2.20.3 — demandes de reset différées (posées hors loopTask, consommées en
  // tête de update()). `volatile` : écrites par une autre tâche, lues par loopTask.
  volatile bool _phFilterResetRequested = false;
  volatile bool _orpFilterResetRequested = false;
  // Origine de la demande (D4). Pointeur vers un littéral statique uniquement :
  // pas de String (allocation) ni de copie depuis un contexte async_tcp.
  const char* volatile _phResetOrigin = "inconnue";
  const char* volatile _orpResetOrigin = "inconnue";

  // v2.20.3 (C1) — alarme « filtre non prêt durablement », PAR CAPTEUR.
  // `_xxNotReadySinceMs` = millis() du début de l'épisode continu (0 = pas
  // d'épisode en cours ; toute interruption — filtration coupée, purge, filtre
  // redevenu prêt — remet le compteur à 0, l'épisode doit être CONTINU).
  // `_xxNotReadyAlarmed` = alarme posée (edge-triggered : un seul critical à la
  // pose, une répétition toutes les kSensorFilterNotReadyRepeatMs, un info à la
  // levée). `_xxNotReadyLastAlarmMs` cadence la répétition.
  uint32_t _phNotReadySinceMs = 0;
  uint32_t _orpNotReadySinceMs = 0;
  bool _phNotReadyAlarmed = false;
  bool _orpNotReadyAlarmed = false;
  uint32_t _phNotReadyLastAlarmMs = 0;
  uint32_t _orpNotReadyLastAlarmMs = 0;

  // Compteurs d'échecs I²C consécutifs (pool-chemistry condition #5)
  int _phI2cFailStreak = 0;
  int _orpI2cFailStreak = 0;

  // Flags pour ne logger critical qu'à la transition vers stale
  bool _phStaleLogged = false;
  bool _orpStaleLogged = false;

  // Flags pour ne logger critical qu'une seule fois quand le bus I²C franchit
  // le seuil `kEzoBusFailMaxConsecutive` (pool-chemistry condition #5).
  // Remis à false dès la 1ʳᵉ lecture réussie suivante.
  bool _phI2cDegradedLogged = false;
  bool _orpI2cDegradedLogged = false;

  // Cache des points de calibration EZO (mis à jour en begin() puis après chaque
  // calibration). Permet à getPhCalibrationPointsCached() / getOrpCalibrationPointsCached()
  // d'être appelés sans bloquer le bus I²C dans le chemin chaud (loopTask, WS).
  // Valeur -1 : EZO injoignable ou bus I²C dégradé (fail-streak ≥ kEzoBusFailMaxConsecutive).
  // canDose(int) bloque le dosage tant que la valeur reste -1 (pool-chemistry condition #2/#5).
  // Le cache est rafraîchi opportunément à la 1ʳᵉ lecture réussie suivante.
  int _phCalCachedPoints = -1;   // -1 = bus down/inconnu, 0..3 sinon
  int _orpCalCachedPoints = -1;  // -1 = bus down/inconnu, 0..1 sinon

  // True si au moins un EZO a répondu (au moins une fois) — utilisé par isInitialized()
  bool _ezoEverResponded = false;

  // ===== feature-024 : cache pente sonde pH (Slope,?) =====
  // Mis à jour au boot puis : (a) toutes les kPhSlopeQueryIntervalMs (24h),
  //                            (b) après chaque calibration pH (post-cal),
  //                            (c) sur demande POST /debug/ph_slope_refresh.
  float _phSlopeAcid = NAN;     // % pente acide ; NaN si jamais lu OU bus dégradé
  float _phSlopeBase = NAN;     // % pente base
  float _phSlopeZero = NAN;     // mV zéro (NaN si firmware EZO ne le rapporte pas)
  uint32_t _phSlopeQueriedMs = 0;     // 0 = jamais ; sinon millis() de la dernière query OK
  bool _phSlopeQueryPending = false;  // anti-doublon enqueue → handler lève le flag
  int _phSlopeFailStreak = 0;         // ≥ kEzoBusFailMaxConsecutive → invalider cache à NaN

  // v2.21.2 (bug-slope-boot-perdue) : retentative bornée de la Slope,? de boot.
  // Décision pure dans slope_retry_logic.h (shouldRetryBootSlope), consommée en
  // fin d'update(). `_phSlopeBootAttempts` est incrémenté à CHAQUE tentative,
  // MÊME si l'enqueue échoue, et n'est JAMAIS remis à zéro (gate C1 pool-chemistry :
  // borne dure de kPhSlopeBootRetryMaxAttempts transactions I²C ~900 ms).
  bool _phEzoDetected = false;             // Gate C2 : readInfo() pH OK au boot
  uint8_t _phSlopeBootAttempts = 0;        // Retentatives décomptées (jamais réarmé)
  uint32_t _phSlopeBootLastAttemptMs = 0;  // millis() de la dernière retentative
  // Flags one-shot de journalisation (jamais réarmés) :
  bool _phSlopeBootFirstFailLogged = false;  // warning au 1ᵉʳ échec sans pente jamais lue
  bool _phSlopeBootExhaustedLogged = false;  // warning UNIQUE à l'épuisement des retentatives

  // ===== Queue FreeRTOS pour commandes longues =====
  static constexpr UBaseType_t kEzoQueueLen = 4;
  QueueHandle_t _ezoQueue = nullptr;

  // ===== Trace capteur (ring buffer RAM, feature-057) =====
  // Voir contrat de concurrence dans la section publique SensorTraceSample.
  SensorTraceSample _traceBuffer[kSensorTraceBufferSize];
  size_t _traceIdx = 0;    // Prochaine position d'écriture
  size_t _traceCount = 0;  // Nombre d'échantillons valides (≤ kSensorTraceBufferSize)

  // ===== Helpers privés =====
  void _readEzoSensors(float tempC);   // Lecture pH puis ORP, mise à jour caches
  // Enregistre un échantillon de trace (appelé en fin de _readEzoSensors, loopTask).
  // feature-067 : `phCorrected` = brut + offset (NaN si brut NaN) ; `phRaw` et
  // `phFiltered` restent BRUTS (membres internes, jamais corrigés).
  void _recordSensorTrace(float phRaw, float phCorrected, float phFiltered,
                          float orpRaw, float orpFiltered, float tempEau);
  void _readDs18b20s();                // Lecture multi-sondes DS18B20
  void _processEzoQueue();             // Dépile au plus 1 commande par cycle
  void _executeEzoCmd(const EzoCmdRequest& req);
  void _checkStaleAndLog();            // Détection stale → log critical (1 fois)
  void _checkFrozenAndLog();           // feature-022 : logs SENSOR_FROZEN edge-triggered
  // v2.20.3 : applique le contexte de représentativité aux DEUX filtres (porte
  // d'alimentation) et journalise les transitions gel / réouverture.
  void _updateFilterGates(uint32_t nowMs);
  // v2.20.3 : journalise le ré-amorçage (warning) ou son REFUS (critical + alerte
  // MQTT) pour un filtre donné. `isPh` sélectionne le filtre et les libellés.
  void _checkRearmAndLog(bool isPh);
  // v2.20.3 (C1) : arme/désarme l'alarme « filtre non prêt durablement » pour les
  // DEUX capteurs (lit le snapshot measure_validity une seule fois).
  void _checkFilterNotReadyAlarm(uint32_t nowMs);
  // Machine d'état d'un capteur. `blocked` = « aucune régulation possible sans
  // raison légitime » (eau présente + mesures représentatives + filtre non prêt).
  void _evalNotReadyAlarm(bool isPh, bool blocked, uint32_t nowMs);
  // v2.20.3 (D4) : exécute une réinitialisation DEMANDÉE et journalise l'état
  // DÉTRUIT (médiane avant/après, rejets, latch EMI, sonde figée). Contexte
  // loopTask exclusivement (appelé en tête de update()).
  void _executeRequestedReset(bool isPh, const char* origin);

  // Helpers DS18B20 (feature-020) — inchangés
  void _loadSondeIdentificationFromNvs();
  bool _saveSondeAddrToNvs(const char* nvsKey, const uint8_t addr[kSondeAddrLen]);
  int _findSondeIndexByRole(SondeRole role) const;
  int _findSondeIndexByAddr(const uint8_t addr[kSondeAddrLen]) const;
};

extern SensorManager sensors;

#endif // SENSORS_H
