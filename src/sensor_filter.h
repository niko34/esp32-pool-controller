#ifndef SENSOR_FILTER_H
#define SENSOR_FILTER_H

// NOTE : PAS d'include Arduino.h ici — logique 100 % pure, testable en env:native
// (ADR-0017). N'utiliser aucun type/fonction Arduino ni String dans ce module.
// Uniquement des headers C standard (types entiers, NAN/isnan/fabsf).
#include <math.h>    // NAN
#include <stdint.h>  // uint8_t, uint16_t, uint32_t, UINT16_MAX

#include "constants.h"

// =============================================================================
// SensorFilter — Chaîne de mesure pH/ORP à 5 ÉTAGES (feature-152, refonte)
// =============================================================================
//
// Le filtre LISSE, il ne JUGE pas — c'est la couche de dosage (dosing_logic.h,
// pump_controller.cpp) qui juge, avec des BORNES qui comparent sans délibérer
// (butées de plausibilité, plafonds de dose). Ce module ne connaît ni verrou,
// ni ré-amorçage, ni latch : aucune séquence d'entrées ne peut le laisser sans
// valeur plus longtemps que la traversée normale de la fenêtre.
//
//   1. lecture du module (coquille : sensors.cpp)
//   2. rejet de l'absurde : NaN / hors plage PHYSIQUE, comparaison STRICTE
//   3. médiane glissante (kSensorFilterMedianWindow échantillons) PUIS
//      lissage exponentiel de constante de temps FIXE (kSensorFilterEmaTauMs,
//      en millisecondes) — α recalculé à CHAQUE échantillon depuis le temps
//      réellement écoulu : indépendant de la cadence de lecture (D3)
//   4. offset utilisateur — appliqué EN SORTIE d'accesseur par la coquille
//      (ph_offset.h), jamais dans ce module (ADR-0039)
//   5. fraîcheur : pas de mesure ACCEPTÉE depuis kSensorFilterMaxAgeMs ⇒
//      vidage de la fenêtre, même primitive que la porte ci-dessous — jamais
//      de médiane bâtie à cheval sur une absence (ADR-0072 : NaN = pas de
//      valeur, jamais une sentinelle)
//
// 🔑 LA MÉDIANE EST LE REJET D'ABERRATIONS : une médiane de N écarte PAR
// CONSTRUCTION jusqu'à (N−1)/2 valeurs extrêmes, sans aucun seuil réglable sur
// une installation. Un échelon légitime (sonde replongée après étalonnage,
// remise en eau) traverse la fenêtre en (N+1)/2 échantillons, QUOI QU'IL
// ARRIVE : il n'existe donc aucune séquence d'entrées qui verrouille ce
// module — contrairement à l'ancien rejet de spike + sortie de secours, qui
// pouvait se verrouiller indéfiniment sur une médiane périmée (incident
// v2.20.3, dosage pH ET chlore à l'arrêt ≥ 3 h).
//
// Ce qui reste, et pourquoi (verdict pool-chemistry P1, feature-152) :
//   - détecteur de sonde figée (FrozenDetector) : la médiane ne détecte PAS
//     une sonde morte qui rend une constante plausible — c'est le seul
//     mécanisme qui protège ce cas, sans verrou (sortie immédiate dès une
//     lecture hors bande) ;
//   - fraîcheur : un fait (pas de lecture récente), pas un jugement ;
//   - porte d'alimentation : SIMPLIFIÉE (vider / remplir), plus de plancher
//     de gel ni de ré-amorçage explicite — le prochain échantillon ACCEPTÉ
//     redate lui-même l'horloge.
//
// Ce qui disparaît (aucun cas trouvé où ces mécanismes ont démontrablement
// évité un dommage, verdict pool-chemistry P1) : rejet de spike isolé, sortie
// de secours de ré-amorçage et ses trois gardes cumulatives, cooldown,
// dispersion de fenêtre et son latch EMI collant, alarme « filtre non prêt
// durablement ».
//
// Contraintes :
//   - ZÉRO allocation dynamique : buffer FIXE float[kSensorFilterMedianWindowMax].
//   - Pas de membre statique : 1 instance par capteur.
//   - Lecture/écriture dans le SEUL contexte loopTask (comme _lastPh/_lastOrp).
//     addSample()/setContext() écrivent, les getters lisent — pas de mutex
//     interne nécessaire si l'appelant respecte ce contrat (cf. SensorManager).
// =============================================================================

// =============================================================================
// FrozenDetector — Détection de variance nulle sur N échantillons (feature-022,
// conservé tel quel par feature-152 — condition C2 du gate)
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
//     lecture hors bande (ré-ancrage du run sur cette lecture) — SANS VERROU.
//   - samples = 0 → détection désactivée (frozen() toujours false).
//   - ZÉRO allocation, pas de String — compile en env:native.
class FrozenDetector {
public:
  // samples : nb d'échantillons dans la bande avant déclaration "figé" (0 = désactivé)
  // epsilon : largeur de bande (½ LSB du capteur, strictement < étendue vivante)
  FrozenDetector(uint16_t samples, float epsilon)
      : _samples(samples), _epsilon(epsilon) {}

  // Soumet un échantillon ACCEPTÉ (l'appelant filtre NaN/hors-plage en amont).
  // Si l'échantillon reste dans la bande courante → étend les bornes, runCount++.
  // Sinon → ré-ancre le run sur x (runMin=runMax=x, runCount=1) : sortie immédiate.
  void addSample(float x);

  // true si le run courant atteint `samples` échantillons dans la bande.
  bool frozen() const { return (_samples > 0) && (_runCount >= _samples); }

  // Réinitialise le run (appelé par SensorFilter::reset()).
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
  // 7 champs (contre 13 avant feature-152) : chacun correspond à un étage ou
  // un mécanisme CONSERVÉ de la chaîne cible. Aucun champ de réglage de rejet
  // (spike/dispersion/rearm) : ces mécanismes n'existent plus.
  struct Config {
    float minValue;          // Étage 2 : borne basse plage PHYSIQUE/matérielle
    float maxValue;           // Étage 2 : borne haute plage PHYSIQUE/matérielle
    uint8_t medianWindow;     // Étage 3a : largeur de la médiane (<= kSensorFilterMedianWindowMax)
    float emaTauMs;           // Étage 3b : constante de temps du lissage (ms, PAS un compteur)
    uint32_t maxAgeMs;        // Étage 5 : âge max d'une mesure acceptée pour rester valide
    uint16_t frozenSamples;   // Détecteur figé : N échantillons acceptés dans la bande (0 = off)
    float frozenEpsilon;      // Détecteur figé : largeur de bande (½ LSB capteur)
  };

  explicit SensorFilter(const Config& config);

  // Soumet une mesure brute. Retourne true si elle a été ACCEPTÉE (filtre
  // alimenté), false si rejetée (NaN / hors plage / porte fermée).
  // nowMs = millis() de la lecture.
  bool addSample(float raw, uint32_t nowMs);

  // Porte d'alimentation SIMPLIFIÉE (A4, feature-152) — à appeler AVANT
  // addSample(), à chaque cycle de lecture, avec le verdict de
  // représentativité des mesures (measure_validity). Fermée si
  // !waterPresent || !measuresValid. À la FERMETURE : vidage immédiat de la
  // fenêtre (même effet que la fraîcheur ci-dessous). À l'OUVERTURE : AUCUNE
  // action — la fenêtre, déjà vide, se remplit naturellement au fil des
  // échantillons acceptés qui suivent. Ni plancher de gel, ni ré-amorçage
  // explicite : le mécanisme n'a plus d'état à gérer entre les deux.
  //
  // Trou de fraîcheur (C4, pool-chemistry) : si aucune mesure n'a été acceptée
  // depuis plus de maxAgeMs, vidage IMMÉDIAT, que la porte soit ouverte ou
  // fermée — jamais de médiane bâtie à cheval sur une absence. N'efface PAS
  // l'horloge de la dernière mesure acceptée : un silence qui perdure continue
  // de se signaler (stale()), il ne se re-présente pas en warmup à chaque cycle.
  void setContext(bool waterPresent, bool measuresValid, uint32_t nowMs);

  // Réinitialise complètement le filtre (warmup, buffer, détecteur figé).
  // Appelé après une calibration EZO réussie, ou par le bouton de reset manuel.
  void reset();

  float raw() const;        // Dernière valeur brute soumise (NaN si aucune)
  float median() const;     // Médiane COURANTE (diagnostic, NaN si buffer vide)

  // VALEUR OFFICIELLE (A5) : NaN si la fenêtre n'est pas pleine (warmup), si
  // le capteur est figé, ou si la dernière mesure acceptée est périmée.
  // JAMAIS figée par un jugement d'instabilité — il n'y en a plus : la médiane
  // fait déjà le travail de rejet, sans seuil et sans verrou.
  float value(uint32_t nowMs) const;

  bool gateClosed() const { return _gateClosed; }
  bool frozen() const { return _frozenDetector.frozen(); }
  // Périmé : aucune mesure jamais acceptée, OU dernière mesure acceptée plus
  // vieille que maxAgeMs.
  bool stale(uint32_t nowMs) const;
  // Fenêtre médiane incomplète (warmup, cold start ou suite d'un vidage).
  bool warmingUp() const { return _bufCount < _cfg.medianWindow; }
  uint32_t ageMs(uint32_t nowMs) const;  // Âge dernière mesure acceptée (UINT32_MAX si aucune)

  // Compteur glissant de rejets (étage 2 : NaN / hors plage) — DIAGNOSTIC
  // SEUL, ne décide plus rien (plus de seuil d'instabilité consécutif).
  uint8_t rejectedCount() const { return _rejectedCount; }

private:
  Config _cfg;

  // Buffer circulaire FIXE pour la médiane (taille physique =
  // kSensorFilterMedianWindowMax, taille logique = _cfg.medianWindow bornée
  // à la capacité physique).
  float _buffer[kSensorFilterMedianWindowMax];
  uint8_t _bufIdx = 0;        // Prochain slot d'écriture
  uint8_t _bufCount = 0;      // Nombre d'échantillons valides dans le buffer

  float _raw = NAN;           // Dernière brute soumise
  float _filtered = NAN;      // Estimateur central (médiane lissée), NaN tant que fenêtre non pleine
  uint8_t _rejectedCount = 0; // Compteur de rejets (diagnostic, sature à 255)
  uint32_t _lastValidMs = 0;  // millis() de la dernière mesure ACCEPTÉE
  bool _hasValid = false;     // true dès la 1ʳᵉ mesure acceptée

  bool _gateClosed = false;   // Porte d'alimentation (false par défaut : tests natifs existants
                              // qui n'appellent pas setContext() gardent le comportement antérieur)

  // Détecteur de sonde figée (C2, feature-152) — alimenté UNIQUEMENT par les
  // échantillons bruts ACCEPTÉS.
  FrozenDetector _frozenDetector;

  // Calcule la médiane des _bufCount premiers échantillons par tri sur copie locale.
  float _computeMedian() const;
  // Vide la fenêtre (buffer + estimateur) — A4 : même primitive pour la porte
  // qui se ferme et pour la fraîcheur qui expire.
  void _clearWindow();
};

#endif // SENSOR_FILTER_H
