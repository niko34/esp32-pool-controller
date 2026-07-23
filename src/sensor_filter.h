#ifndef SENSOR_FILTER_H
#define SENSOR_FILTER_H

// NOTE : PAS d'include Arduino.h ici — logique 100 % pure, testable en env:native
// (ADR-0017). N'utiliser aucun type/fonction Arduino ni String dans ce module.
// Uniquement des headers C standard (types entiers, NAN/isnan/fabsf).
#include <math.h>    // NAN
#include <stdint.h>  // uint8_t, uint16_t, uint32_t, UINT16_MAX

#include "constants.h"

// =============================================================================
// SensorFilter — Estimateur de tendance centrale pH/ORP (feature-025 → feature-058)
// =============================================================================
//
// Chaîne de filtrage déterministe, testable hors matériel Atlas :
//   brut → rejet vraie aberration (NaN / hors-plage / spike DUR isolé)
//        → médiane sur fenêtre LONGUE → EMA lente
//
// NOUVEAU CONTRAT (feature-058) — VALEUR ≠ DRAPEAU :
//   * filtered() renvoie TOUJOURS l'EMA de la médiane-longue recalculée : c'est
//     l'estimateur central COURANT. Elle n'est JAMAIS figée, JAMAIS ré-amorcée en
//     cours de route par une instabilité. NaN uniquement tant que la fenêtre
//     centrale n'est pas PLEINE (warmup, C2).
//   * unstable() (le DRAPEAU d'inhibition dosage) est DÉCOUPLÉ de la valeur. Il ne
//     modifie jamais filtered(). Il vaut true si l'un de ces trois signaux est posé :
//       (a) dispersion > cap : la fenêtre brute PLEINE a une amplitude (max−min)
//           au-dessus du cap → oscillation de niveau EMI (C1/C3) ;
//       (b) latch EMI : dispersion > cap SOUTENUE (N fenêtres consécutives) → latch
//           collant, levé UNIQUEMENT par reset()/recalibration ;
//       (c) rejets consécutifs : NaN/hors-plage répétés >= maxConsecutiveRejects.
//
//   Motivation (diagnostic feature-057) : l'ancien rejet de « saut » (maxStep)
//   traitait les transitions de l'oscillation comme aberrantes → n'intégrait qu'un
//   côté de l'enveloppe → la valeur collait en haut/en bas, puis gelait sur latch.
//   On SUPPRIME ce rejet du chemin valeur : l'oscillation légitime alimente le
//   filtre, la médiane longue la moyenne sur ≥ 1 période → valeur CENTRÉE honnête.
//
// Contraintes :
//   - ZÉRO allocation dynamique : buffer FIXE float[kSensorFilterCentralWindowMax].
//   - Pas de membre statique : 1 instance par capteur.
//   - Lecture/écriture dans le SEUL contexte loopTask (comme _lastPh/_lastOrp).
//     addSample() écrit, les getters lisent — pas de mutex interne nécessaire si
//     l'appelant respecte ce contrat (cf. SensorManager).
//
// Logique de rejet — une mesure n'alimente PAS le filtre si :
//   - NaN → rejet, incrémente consecutiveRejects (signal d'instabilité) ;
//   - hors [minValue, maxValue] → idem ;
//   - spike DUR isolé : |raw − médiane| > spikeReject → ÉCARTÉ, EXCLU avant le
//     calcul de dispersion, NE compte PAS comme instabilité (n'incrémente pas
//     consecutiveRejects), NE met PAS à jour lastValid → l'âge grandit (C4).
//   Un spike répété maintient donc le filtre non-alimenté → âge > maxAge → non
//   prêt → dosage fail-closed (C4, invariant feature-022 préservé).
//
// ready()      = fenêtre centrale PLEINE (>= centralWindow échantillons acceptés,
//                C2) ET dernière mesure récente (< maxAgeMs) ET non figé (frozen()).
//                Tant que la fenêtre n'est pas pleine → filtered() = NaN, ready()=false.
// dispersion() = amplitude (max−min) de la fenêtre brute acceptée quand elle est
//                PLEINE (NaN sinon). Diagnostic + moteur du drapeau d'instabilité.
//
// Détection capteur figé (feature-022 Passe 2) :
//   Un FrozenDetector est composé dans SensorFilter, alimenté UNIQUEMENT par les
//   échantillons bruts ACCEPTÉS (les rejets NaN/hors-plage/spike n'alimentent PAS
//   le détecteur → l'état "figé" PERSISTE pendant une rafale de rejets,
//   pool-chemistry condition #4). Si frozen() → ready() = false (fail-closed,
//   garde FilterNotReady existante). reset() réinitialise aussi le détecteur.
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
    float spikeReject;               // Seuil spike DUR isolé (|raw − médiane|), feature-058
    float emaAlpha;                  // Coefficient EMA (0..1, plus petit = plus lent)
    uint8_t centralWindow;           // Taille fenêtre médiane LONGUE (<= kSensorFilterCentralWindowMax)
    uint8_t maxConsecutiveRejects;   // Rejets NaN/hors-plage consécutifs → instable
    uint32_t maxAgeMs;               // Âge max dernière mesure valide pour ready()
    float maxDispersion;             // Cap dispersion (max−min fenêtre pleine) → unstable (C1/C3)
    // feature-022 : détection capteur figé. Champs en FIN de struct pour préserver
    // l'init positionnelle : une init agrégat plus courte value-initialise ces
    // champs à 0 → détection désactivée. Pas d'initialiseur par défaut ici
    // (gnu++11 côté ESP32 : la struct doit rester un agrégat).
    uint16_t frozenSamples;          // N échantillons acceptés dans la bande → figé (0 = off)
    float frozenEpsilon;             // Largeur de bande (½ LSB capteur)
  };

  explicit SensorFilter(const Config& config);

  // Soumet une mesure brute. Retourne true si elle a été ACCEPTÉE (filtre alimenté),
  // false si rejetée (NaN / hors plage / spike dur isolé). nowMs = millis() de la lecture.
  bool addSample(float raw, uint32_t nowMs);

  // Réinitialise complètement le filtre (warmup, buffers, compteurs, latch).
  void reset();

  float raw() const;        // Dernière valeur brute soumise (NaN si aucune)
  float median() const;     // Médiane courante (NaN si pas encore de donnée)
  float filtered() const;   // EMA de la médiane-longue, estimateur central COURANT.
                            // JAMAIS figée ; NaN tant que la fenêtre n'est pas pleine (C2/C5).
  float dispersion() const; // Amplitude (max−min) de la fenêtre brute PLEINE (NaN sinon). Diagnostic + drapeau.
  bool ready(uint32_t nowMs) const;   // Fenêtre pleine ET mesure récente ET non figé
  bool unstable() const;    // Drapeau d'inhibition : dispersion > cap OU latch EMI OU rejets consécutifs
  bool frozen() const;      // Capteur figé : N échantillons acceptés dans une bande < epsilon (feature-022)
  uint8_t rejectedCount() const;        // Compteur glissant (8 bits, sature à 255)
  uint8_t consecutiveRejects() const;   // Rejets NaN/hors-plage consécutifs courants
  bool unstableLatched() const;         // Latch EMI posé (jusqu'à auto-levée ou reset())
  uint8_t dispersionCleanStreak() const;// Fenêtres pleines propres consécutives (diagnostic auto-levée latch)
  uint32_t ageMs(uint32_t nowMs) const; // Âge dernière mesure valide (UINT32_MAX si aucune)

private:
  Config _cfg;

  // Buffer circulaire FIXE pour la médiane longue (taille physique =
  // kSensorFilterCentralWindowMax, taille logique = _cfg.centralWindow bornée
  // à la capacité physique). Contient les bruts ACCEPTÉS (base de la dispersion).
  float _buffer[kSensorFilterCentralWindowMax];
  uint8_t _bufIdx = 0;        // Prochain slot d'écriture
  uint8_t _bufCount = 0;      // Nombre d'échantillons valides dans le buffer

  float _raw = NAN;           // Dernière brute soumise
  float _filtered = NAN;      // EMA (estimateur central) — NaN tant que fenêtre non pleine
  uint8_t _validCount = 0;    // Nombre de mesures valides depuis reset (sature à 255)
  uint8_t _rejectedCount = 0; // Compteur de rejets (sature à 255)
  uint8_t _consecutiveRejects = 0;  // Rejets NaN/hors-plage consécutifs (le spike ne compte pas)
  uint32_t _lastValidMs = 0;  // millis() de la dernière mesure acceptée
  bool _hasValid = false;     // true dès la 1ʳᵉ mesure acceptée

  // --- Latch EMI (anti bruit soutenu) ---
  // Compteur de fenêtres PLEINES consécutives en dépassement de dispersion.
  // Au-delà de kSensorFilterDispersionLatchCycles → _unstableLatched (collant).
  uint8_t _dispersionBreachStreak = 0;
  // Compteur de fenêtres PLEINES consécutives PROPRES (disp <= cap ET zéro rejet).
  // Au-delà de kSensorFilterLatchClearCycles → auto-levée du latch EMI collant
  // (feature v2.8.4). Remis à 0 sur TOUT rejet (NaN / hors-plage / spike) ET sur
  // toute fenêtre en dépassement → anti-flapping : fenêtre pleine ET zéro rejet
  // pendant TOUTE la streak.
  uint8_t _dispersionCleanStreak = 0;
  bool _unstableLatched = false;  // Latch EMI, posé par bruit soutenu, auto-levé après N fenêtres propres ou reset()

  // --- Détection capteur figé (feature-022 Passe 2) ---
  // Alimenté UNIQUEMENT par les échantillons bruts ACCEPTÉS dans addSample()
  // (condition #4 : les rejets ne l'alimentent pas → frozen persiste).
  FrozenDetector _frozenDetector;

  // Calcule la médiane des _bufCount premiers échantillons par tri sur copie locale.
  float _computeMedian() const;

  // Amplitude (max−min) de la fenêtre. NAN tant que la fenêtre n'est pas PLEINE (C2).
  float _computeDispersion() const;
};

#endif // SENSOR_FILTER_H
