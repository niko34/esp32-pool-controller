#ifndef SENSOR_FILTER_H
#define SENSOR_FILTER_H

#include <Arduino.h>
#include "constants.h"

// =============================================================================
// SensorFilter — Lissage robuste d'une mesure scalaire (pH ou ORP) (feature-025)
// =============================================================================
//
// Chaîne de filtrage déterministe, testable hors matériel Atlas :
//   mesure brute → rejet aberrant (NaN / hors plage / saut) → médiane courte → EMA
//
// Contraintes :
//   - ZÉRO allocation dynamique : buffer FIXE float[kSensorFilterMedianWindow].
//   - Pas de membre statique : 1 instance par capteur.
//   - Lecture/écriture dans le SEUL contexte loopTask (comme _lastPh/_lastOrp).
//     addSample() écrit, les getters lisent — pas de mutex interne nécessaire si
//     l'appelant respecte ce contrat (cf. SensorManager).
//
// Logique de rejet (une mesure n'alimente PAS le filtre si) :
//   - NaN ;
//   - hors [minValue, maxValue] ;
//   - |raw - filtered| > maxStep, SAUF pendant le warmup (le filtre doit pouvoir
//     converger vers une valeur initiale même éloignée de 0).
//
// ready()    = warmup atteint ET dernière mesure récente valide (< maxAgeMs).
// unstable() = consecutiveRejects >= maxConsecutiveRejects OU latch anti-boucle posé.
//
// Re-synchronisation (anti latch-up) :
//   Un saut > maxStep qui PERSISTE n'est pas un pic mais un vrai changement (ex. sonde
//   plongée en solution de calibration pendant le warmup, puis retour en bassin). Si le
//   filtre rejetait à vie, _filtered resterait figé. Donc, après kSensorFilterResyncRejects
//   rejets consécutifs par "saut excessif", on ré-amorce le filtre sur la MÉDIANE des
//   derniers bruts rejetés (et non l'échantillon courant seul, pour ignorer un pic final).
//   Pendant le re-warmup, ready()=false → dosage bloqué (invariant préservé).
//
// Anti-boucle latché :
//   Un capteur qui re-sync en boucle = défaut EMI. Au-delà de kSensorFilterMaxResyncPerWindow
//   re-sync sur kSensorFilterResyncWindowMs → latch _unstableLatched, levé uniquement par reset().
//
// Détection capteur figé (feature-022 Passe 2) :
//   Un FrozenDetector est composé dans SensorFilter, alimenté UNIQUEMENT par les
//   échantillons bruts ACCEPTÉS (les rejets NaN/hors-plage/saut n'alimentent PAS
//   le détecteur → l'état "figé" PERSISTE pendant une rafale de rejets,
//   pool-chemistry condition #4). Si frozen() → ready() = false (fail-closed,
//   garde FilterNotReady existante). reset() réinitialise aussi le détecteur
//   (pool-chemistry condition #2).
// =============================================================================

// =============================================================================
// FrozenDetector — Détection de variance nulle sur N échantillons (feature-022)
// =============================================================================
//
// Classe pure réutilisable (pH, ORP, température). Détecte un capteur qui
// répond sans erreur mais retourne indéfiniment la même valeur : un run de
// `samples` échantillons consécutifs tous contenus dans une bande < `epsilon`.
//
// Invariants :
//   - epsilon = ½ LSB du capteur : un capteur vivant bruite ≥ ±1 LSB, donc un
//     toggle de 1 LSB (même en float32) CASSE le run (pool-chemistry cond. #1).
//   - Latence exactement `samples` échantillons ; sortie IMMÉDIATE dès une
//     lecture hors bande (ré-ancrage du run sur cette lecture).
//   - samples = 0 → détection désactivée (frozen() toujours false).
//   - ZÉRO allocation, pas de String — compile en env:native.
class FrozenDetector {
public:
  // samples : nb d'échantillons dans la bande avant déclaration "figé" (0 = désactivé)
  // epsilon : largeur de bande (½ LSB du capteur, strictement < étendue vivante)
  FrozenDetector(uint16_t samples, float epsilon)
      : _samples(samples), _epsilon(epsilon) {}

  // Soumet un échantillon ACCEPTÉ (l'appelant filtre NaN/hors-plage/sauts en amont).
  // Si l'échantillon reste dans la bande courante → étend les bornes, runCount++.
  // Sinon → ré-ancre le run sur x (runMin=runMax=x, runCount=1) : sortie immédiate.
  void addSample(float x);

  // true si le run courant atteint `samples` échantillons dans la bande.
  bool frozen() const { return (_samples > 0) && (_runCount >= _samples); }

  // Réinitialise le run (appelé par SensorFilter::reset() — condition #2).
  void reset() { _runCount = 0; }

private:
  uint16_t _samples;        // Seuil N (0 = détection désactivée)
  float _epsilon;           // Largeur de bande
  float _runMin = 0.0f;     // Borne basse du run courant (valide si _runCount > 0)
  float _runMax = 0.0f;     // Borne haute du run courant
  uint16_t _runCount = 0;   // Longueur du run courant (saturé à UINT16_MAX)
};

class SensorFilter {
public:
  struct Config {
    float minValue;                  // Borne basse plage plausible
    float maxValue;                  // Borne haute plage plausible
    float maxStep;                   // Saut max toléré entre 2 lectures (hors warmup)
    float emaAlpha;                  // Coefficient EMA (0..1, plus petit = plus lent)
    uint8_t medianWindow;            // Taille fenêtre médiane (<= kSensorFilterMedianWindow)
    uint8_t warmupSamples;           // Mesures valides avant ready()
    uint8_t maxConsecutiveRejects;   // Seuil de déclaration "instable"
    uint32_t maxAgeMs;               // Âge max dernière mesure valide pour ready()
    // feature-022 : détection capteur figé. Champs en FIN de struct pour préserver
    // l'init positionnelle existante (tests natifs) : une init agrégat plus courte
    // value-initialise ces champs à 0 → détection désactivée. Pas d'initialiseur
    // par défaut ici (gnu++11 côté ESP32 : la struct doit rester un agrégat).
    uint16_t frozenSamples;          // N échantillons acceptés dans la bande → figé (0 = off)
    float frozenEpsilon;             // Largeur de bande (½ LSB capteur)
  };

  explicit SensorFilter(const Config& config);

  // Soumet une mesure brute. Retourne true si elle a été ACCEPTÉE (filtre alimenté),
  // false si rejetée (NaN / hors plage / saut excessif). nowMs = millis() de la lecture.
  bool addSample(float raw, uint32_t nowMs);

  // Réinitialise complètement le filtre (warmup, buffers, compteurs).
  void reset();

  float raw() const;        // Dernière valeur brute soumise (NaN si aucune)
  float median() const;     // Médiane courante (NaN si pas encore de donnée)
  float filtered() const;   // Valeur filtrée EMA (NaN tant que warmup pas amorcé)
  bool ready(uint32_t nowMs) const;   // Warmup atteint ET mesure récente valide ET non figé
  bool unstable() const;    // Trop de rejets consécutifs → capteur instable
  bool frozen() const;      // Capteur figé : N échantillons acceptés dans une bande < epsilon (feature-022)
  uint8_t rejectedCount() const;        // Compteur glissant (8 bits, sature à 255)
  uint8_t consecutiveRejects() const;   // Rejets consécutifs courants
  uint8_t resyncCount() const;          // Nb re-sync dans la fenêtre glissante courante
  bool unstableLatched() const;         // Latch anti-boucle EMI posé (jusqu'à reset())
  uint32_t ageMs(uint32_t nowMs) const; // Âge dernière mesure valide (UINT32_MAX si aucune)

private:
  Config _cfg;

  // Buffer circulaire FIXE pour la médiane (taille physique = kSensorFilterMedianWindow,
  // taille logique = _cfg.medianWindow bornée à la capacité physique).
  float _buffer[kSensorFilterMedianWindow];
  uint8_t _bufIdx = 0;        // Prochain slot d'écriture
  uint8_t _bufCount = 0;      // Nombre d'échantillons valides dans le buffer

  float _raw = NAN;           // Dernière brute soumise
  float _filtered = NAN;      // Valeur EMA
  uint8_t _validCount = 0;    // Nombre de mesures valides depuis reset (sature à 255)
  uint8_t _rejectedCount = 0; // Compteur de rejets (sature à 255)
  uint8_t _consecutiveRejects = 0;
  uint32_t _lastValidMs = 0;  // millis() de la dernière mesure acceptée
  bool _hasValid = false;     // true dès la 1ʳᵉ mesure acceptée

  // --- Re-synchronisation (anti latch-up) ---
  // Mini-buffer FIXE des derniers bruts rejetés par "saut excessif" (réutilise la
  // capacité physique kSensorFilterMedianWindow → aucune alloc dynamique). Sert à
  // calculer la médiane d'amorçage lors d'une re-sync (ignore un éventuel pic final).
  float _rejBuffer[kSensorFilterMedianWindow];
  uint8_t _rejIdx = 0;        // Prochain slot d'écriture
  uint8_t _rejCount = 0;      // Nombre d'échantillons valides dans _rejBuffer

  // --- Anti-boucle latché ---
  // Timestamps des re-sync (fenêtre glissante kSensorFilterResyncWindowMs).
  uint32_t _resyncTimestamps[kSensorFilterMaxResyncPerWindow];
  uint8_t _resyncCount = 0;       // Nb de re-sync valides dans la fenêtre courante
  bool _unstableLatched = false;  // Latch EMI, levé uniquement par reset()

  // --- Détection capteur figé (feature-022 Passe 2) ---
  // Alimenté UNIQUEMENT par les échantillons bruts ACCEPTÉS dans addSample()
  // (condition #4 : les rejets ne l'alimentent pas → frozen persiste).
  FrozenDetector _frozenDetector;

  // Calcule la médiane des _bufCount premiers échantillons par tri sur copie locale.
  float _computeMedian() const;

  // Médiane du mini-buffer de bruts rejetés (amorçage de re-sync). NAN si vide.
  float _computeRejectedMedian() const;
};

#endif // SENSOR_FILTER_H
