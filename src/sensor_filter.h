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
//   - la PORTE est fermée (cf. setContext ci-dessous) : sortie en tête, l'échantillon
//     est mémorisé pour le diagnostic mais ne touche NI le buffer, NI le détecteur
//     figé, NI les compteurs ;
//   - NaN → rejet, incrémente consecutiveRejects (signal d'instabilité) ;
//   - hors [minValue, maxValue] → idem ;
//   - spike DUR isolé : |raw − médiane| > spikeReject → ÉCARTÉ, EXCLU avant le
//     calcul de dispersion, NE compte PAS comme instabilité (n'incrémente pas
//     consecutiveRejects), NE met PAS à jour lastValid → l'âge grandit (C4).
//     Un spike ISOLÉ maintient le filtre non-alimenté → âge > maxAge → non prêt →
//     dosage fail-closed (C4, invariant feature-022 préservé).
//
// PORTE D'ALIMENTATION + SORTIE DE SECOURS (v2.20.3, bug-filtre-ph-verrouille) :
//   L'ancien contrat présentait le verrouillage sur spike répété comme un
//   fail-closed VOLONTAIRE. C'était le bug : la médiane ne peut évoluer QUE si des
//   échantillons entrent dans le buffer, donc dès que la grandeur VRAIE s'éloigne
//   durablement de plus de spikeReject de la médiane courante, chaque échantillon
//   suivant est rejeté à son tour et la médiane est figée POUR TOUJOURS (incident
//   de production : dosage pH ET chlore à l'arrêt ≥ 3 h, seul reset() en sortait).
//   Le mécanisme n'avait jamais été pensé pour un ÉCHELON LÉGITIME de la grandeur.
//   Deux ajouts, tous deux dans ce module PUR pour rester testables (ADR-0017) :
//
//   (1) setContext(waterPresent, measuresValid, nowMs) — PORTE d'alimentation.
//       Le filtre n'ingère plus les échantillons que le système lui-même a
//       déclarés NON REPRÉSENTATIFS (eau à l'arrêt, purge de la chambre de mesure
//       en cours) : sans cela, la médiane converge vers la valeur de l'eau
//       STAGNANTE, puis acquiert autorité sur les échantillons représentatifs
//       suivants qu'elle rejette un à un. Porte FERMÉE si !waterPresent ||
//       !measuresValid, plus un plancher de gel de kSensorFilterGateFloorMs après
//       tout front montant d'eau. À la réouverture (fermé → ouvert) le filtre est
//       RÉ-AMORCÉ : la médiane d'avant la fermeture n'a aucune autorité sur les
//       mesures représentatives qui suivent.
//
//   (2) Sortie de secours sur rejets de spike SOUTENUS ET COHÉRENTS. Au bout de
//       kSensorFilterSpikeRearmCount rejets consécutifs contenus dans une bande
//       <= rearmCoherence (donc décrivant une NOUVELLE valeur, pas du bruit), le
//       filtre cesse de croire sa médiane et se ré-amorce — sous TROIS gardes
//       cumulatives (pool-chemistry) : cooldown kSensorFilterRearmCooldownMs
//       depuis le dernier ré-amorçage, valeur dans [rearmMin, rearmMax], et
//       cohérence de la série. Si une garde refuse → rearmDenied() != 0 (collant,
//       à journaliser en critical par la coquille) et le filtre RESTE fail-closed :
//       une sonde réellement HS ne ré-amorce jamais. Les échantillons rejetés sont
//       JETÉS, jamais ré-injectés dans le buffer (garantie : aucune valeur
//       aberrante n'entre par cette porte).
//
//   rearm() ≠ reset() : rearm() ne touche NI le détecteur figé (feature-022) NI le
//   latch EMI (feature-058) → on ne masque jamais une sonde figée ou une ambiance
//   EMI en re-convergeant. Une init agrégat sans les 3 champs de fin
//   (rearmCoherence = 0) DÉSACTIVE la sortie de secours (rétrocompatible).
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
    // v2.20.3 (sortie de secours) : MÊME contrainte de position que ci-dessus —
    // champs en FIN de struct pour préserver toutes les initialisations agrégées
    // existantes (une init plus courte laisse rearmCoherence = 0 → sortie de
    // secours DÉSACTIVÉE, comportement strictement antérieur).
    float rearmCoherence;            // Amplitude max de la série rejetée (0 = sortie de secours off)
    float rearmMin;                  // Borne basse de la bande admissible pour ré-amorcer
    float rearmMax;                  // Borne haute de la bande admissible pour ré-amorcer
  };

  explicit SensorFilter(const Config& config);

  // Soumet une mesure brute. Retourne true si elle a été ACCEPTÉE (filtre alimenté),
  // false si rejetée (NaN / hors plage / spike dur isolé). nowMs = millis() de la lecture.
  bool addSample(float raw, uint32_t nowMs);

  // PORTE d'alimentation (v2.20.3) — à appeler AVANT addSample(), à chaque cycle
  // de lecture, avec le verdict de représentativité des mesures (measure_validity).
  // Ferme la porte si !waterPresent || !measuresValid, et maintient la fermeture
  // pendant kSensorFilterGateFloorMs après tout front montant d'eau. La transition
  // fermé → ouvert déclenche rearm() : la médiane construite avant/pendant la
  // stagnation ne fait JAMAIS autorité sur les mesures représentatives suivantes.
  void setContext(bool waterPresent, bool measuresValid, uint32_t nowMs);

  // Ré-amorce l'ESTIMATEUR (buffer, EMA, streak de dispersion propre) sans toucher
  // au détecteur figé ni au latch EMI — c'est ce qui la distingue de reset() et ce
  // qui garantit qu'on ne masque pas une sonde HS en re-convergeant.
  // Arme aussi une GRÂCE d'observabilité de maxAgeMs sur stale() (cf.
  // _staleGraceArmed) : l'âge d'avant le ré-amorçage ne doit pas être présenté
  // comme une panne de sonde. Aucun effet sur ready() ni sur le dosage.
  void rearm();

  // true si la porte est fermée : addSample() sort en tête (aucune alimentation).
  bool gateClosed() const;

  // feature-139 (v2.62.0, B1 gate pool-chemistry 2e passage) : millisecondes
  // restantes avant la fin du PLANCHER DE GEL (kSensorFilterGateFloorMs) armé
  // au dernier front montant d'eau — 0 si non armé ou déjà écoulé. LECTURE
  // SEULE, aucune décision nouvelle : le calcul et l'armement du plancher
  // restent entièrement dans setContext() ci-dessus. Sert à corriger
  // l'AFFICHAGE du délai avant dosage (l'ancien calcul ignorait ce second
  // plancher et annonçait jusqu'à 10x le délai réel après un arrêt d'eau
  // court) — le mécanisme lui-même, jugé bénin, n'est pas modifié.
  uint32_t gateFloorRemainingMs(uint32_t nowMs) const {
    if (!_gateFloorArmed) return 0;
    const int32_t diff = (int32_t)(_gateFloorUntilMs - nowMs);
    return diff > 0 ? (uint32_t)diff : 0;
  }

  // feature-139 (M1, code-reviewer) : snapshot BRUT des deux champs du
  // plancher de gel, à copier par l'appelant SOUS SON PROPRE VERROU avant de
  // les combiner — cette classe reste pure/loopTask-only par conception (cf.
  // en-tête de fichier) et n'a donc PAS de mutex interne. Un appelant qui lit
  // armed/untilMs par DEUX accès séparés (ou via gateFloorRemainingMs()
  // directement) depuis un AUTRE contexte que loopTask pendant que
  // setContext() les écrit s'expose à une paire incohérente (armé avec
  // l'échéance d'un front différent). gateFloorRemainingMs() ci-dessus reste
  // correct et testé pour un usage loopTask-only ; ce snapshot est le point
  // d'entrée pour un usage cross-contexte (cf. SensorManager::
  // getFilterGateRemainingMs()).
  void gateFloorSnapshot(bool& armed, uint32_t& untilMs) const {
    armed = _gateFloorArmed;
    untilMs = _gateFloorUntilMs;
  }

  // Consomme le drapeau « un ré-amorçage a eu lieu depuis le dernier appel ».
  // Destiné à la journalisation edge-triggered par la coquille (module pur : ni
  // logger ni String ici, ADR-0017 — même pattern que le latch EMI).
  bool rearmedSinceLastPoll();

  // Motif du DERNIER refus de ré-amorçage (collant jusqu'au prochain ré-amorçage
  // ou reset) : 0 = aucun, 1 = cooldown non écoulé, 2 = valeur hors bande
  // admissible. Un refus = le filtre reste non prêt → dosage fail-closed : la
  // coquille DOIT l'alarmer.
  uint8_t rearmDenied() const;

  // Réinitialise complètement le filtre (warmup, buffers, compteurs, latch).
  void reset();

  float raw() const;        // Dernière valeur brute soumise (NaN si aucune)
  float median() const;     // Médiane courante (NaN si pas encore de donnée)
  float filtered() const;   // EMA de la médiane-longue, estimateur central COURANT.
                            // JAMAIS figée ; NaN tant que la fenêtre n'est pas pleine (C2/C5).
  float dispersion() const; // Amplitude (max−min) de la fenêtre brute PLEINE (NaN sinon). Diagnostic + drapeau.
  bool ready(uint32_t nowMs) const;   // Fenêtre pleine ET mesure récente ET non figé

  // --- v2.25.1 : décomposition de ready() pour l'OBSERVABILITÉ ---------------
  // ready() est faux pour quatre raisons physiquement distinctes ; la chaîne de
  // refus de dosage les confondait dans un seul libellé, présentant une panne de
  // sonde comme une stabilisation normale. Ces trois prédicats exposent l'ÉTAT
  // INTERNE du filtre (jamais un cache brut extérieur, qui reste frais sur une
  // sonde figée) et sont composés par resolveFilterState() (dosing_logic.h).
  // Aucun effet sur la décision de dosage : lecture seule, const.
  bool warmingUp() const;             // Fenêtre centrale incomplète (_bufCount < centralWindow)
  // Aucune mesure acceptée, OU âge > maxAgeMs — SAUF pendant la grâce de
  // ré-amorçage (cf. _staleGraceArmed) : juste après un rearm(), l'âge porté par
  // le filtre est celui d'AVANT le ré-amorçage et n'incrimine pas la sonde.
  bool stale(uint32_t nowMs) const;
  bool unstable() const;    // Drapeau d'inhibition : dispersion > cap OU latch EMI OU rejets consécutifs
  bool frozen() const;      // Capteur figé : N échantillons acceptés dans une bande < epsilon (feature-022)
  uint8_t rejectedCount() const;        // Compteur glissant (8 bits, sature à 255)
  // Valeur de rejectedCount() juste avant le dernier rearm() — le compteur étant
  // remis à zéro par le ré-amorçage, c'est ce qu'il faut journaliser pour tracer
  // l'ampleur du verrouillage qui vient d'être levé.
  uint8_t rejectedBeforeRearm() const { return _rejectedBeforeRearm; }
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

  // --- Grâce « périmé » après ré-amorçage (revue v2.25.1) ---
  // rearm() vide l'estimateur mais NE FABRIQUE PAS de mesure : _hasValid et
  // _lastValidMs restent ceux d'avant (invariant : ils ne décrivent QUE des
  // échantillons réellement acceptés — c'est ce qui rend ready() et ageMs()
  // honnêtes). Conséquence non voulue : juste après un ré-amorçage de porte, le
  // filtre était simultanément warmingUp ET stale, et la priorité pannes-d'abord
  // de resolveFilterState() affichait « sonde muette » sur une sonde SAINE, au
  // démarrage de la filtration.
  // On ne falsifie donc PAS la datation : on mémorise que l'âge courant est
  // IMPUTABLE AU RÉ-AMORÇAGE, pendant maxAgeMs au plus, et seul stale() lit ce
  // drapeau. Aucun chemin de DÉCISION (ready(), unstable(), dosage) ne le
  // consulte : la garde d'âge de ready() reste inchangée au bit près.
  // Une sonde réellement muette redevient stale à l'expiration (≤ maxAgeMs, soit
  // 20 s) : le retard est intrinsèque — on ne peut pas conclure avant d'avoir
  // laissé sa chance à la première lecture qui suit le ré-amorçage.
  //
  // EXPIRATION EFFECTIVE, PAS SEULEMENT OBSERVÉE (revue v2.25.1, correctif de repliement) — 3ᵉ défaut
  // de repliement du projet, après la retentative de pente (v2.21.2) et
  // tickPulse. La v2.25.1 n'évaluait l'échéance QUE dans stale(), qui est const :
  // sur une sonde restée muette, le drapeau ne redescendait jamais et la
  // comparaison de deadline `(int32_t)(now - until) < 0`, juste au premier tour,
  // se retournait 2³¹ ms plus tard (24,8 j) → la panne était masquée à nouveau
  // pendant 24,8 j. Deux mesures cumulatives :
  //   (1) le drapeau est RÉELLEMENT désarmé par _expireStaleGrace(), appelé en
  //       tête de setContext() ET de addSample() — les deux seuls chemins
  //       non-const qui reçoivent l'horloge ;
  //   (2) la fenêtre est exprimée en ÉCOULÉ DEPUIS L'ARMEMENT (intervalle fermé
  //       [from, from+maxAgeMs[) et non en deadline (demi-droite) : le prédicat
  //       n'est plus monotone en `now` mais en `now − from`, donc même un drapeau
  //       resté armé ne peut re-rentrer dans la grâce qu'au repliement COMPLET de
  //       millis() (49,7 j = la limite intrinsèque du projet), au lieu de 24,8 j.
  bool _staleGraceArmed = false;     // grâce en cours (levée par expiration, par la 1ʳᵉ mesure acceptée, ou par reset())
  uint32_t _staleGraceFromMs = 0;    // instant d'ARMEMENT (durée = _cfg.maxAgeMs)
  // Dernier nowMs vu par setContext()/addSample(). rearm() n'a pas d'argument de
  // temps (API publique consommée par les tests natifs et la coquille) : cette
  // observation d'horloge en tient lieu. Sur les DEUX chemins de ré-amorçage réels
  // (réouverture de porte dans setContext, sortie de secours dans addSample) elle
  // vaut exactement le nowMs du cycle en cours, l'affectation étant faite en tête.
  uint32_t _lastSeenMs = 0;

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

  // --- Porte d'alimentation (v2.20.3) ---
  // _gateClosed = false par défaut : un appelant qui n'utilise PAS setContext()
  // (tests natifs existants) conserve le comportement antérieur, porte ouverte.
  bool _gateClosed = false;
  bool _lastWaterPresent = false;   // Pour le front montant d'eau
  bool _gateFloorArmed = false;     // Plancher de gel en cours après front montant
  uint32_t _gateFloorUntilMs = 0;   // Fin du plancher de gel (millis, wrap-safe)

  // --- Sortie de secours sur rejets cohérents (v2.20.3) ---
  uint8_t _spikeCount = 0;          // Longueur de la série de rejets COHÉRENTS courante
  float _spikeMin = NAN;            // Borne basse de la série rejetée
  float _spikeMax = NAN;            // Borne haute de la série rejetée
  // Tolérance aux TROUS de lecture (NaN / hors plage) dans une série de rejets
  // cohérents — condition BLOQUANTE du gate pool-chemistry (v2.20.3). Sans elle,
  // une sonde ratant un cycle sur douze pendant un échelon légitime ne
  // ré-amorcerait JAMAIS : soit exactement l'incident qu'on corrige.
  // Règle chiffrée par le gate : au plus 2 trous par série, JAMAIS 2 consécutifs.
  uint8_t _spikeGaps = 0;           // Trous tolérés déjà consommés par la série
  bool _lastWasGap = false;         // Le dernier échantillon de la série était un trou
  uint8_t _rejectedBeforeRearm = 0; // rejectedCount() juste avant le dernier rearm()
  void _noteSpikeSeriesGap();       // Applique la règle de tolérance ci-dessus

  // Désarme la grâce « périmé » si sa durée est écoulée (cf. _staleGraceArmed).
  // À appeler en TÊTE de tout chemin non-const recevant l'horloge : c'est ce qui
  // rend l'expiration EFFECTIVE et interdit tout ré-armement spontané au
  // repliement de millis(). Idempotent, sans effet si aucune grâce n'est armée.
  void _expireStaleGrace(uint32_t nowMs);
  uint32_t _lastRearmMs = 0;        // millis() du dernier ré-amorçage par sortie de secours
  bool _hasRearmed = false;         // false = jamais ré-amorcé (cooldown inapplicable)
  bool _rearmedFlag = false;        // Drapeau drainé par rearmedSinceLastPoll()
  uint8_t _rearmDenied = 0;         // 0 aucun / 1 cooldown / 2 hors bande (collant)

  // Calcule la médiane des _bufCount premiers échantillons par tri sur copie locale.
  float _computeMedian() const;

  // Amplitude (max−min) de la fenêtre. NAN tant que la fenêtre n'est pas PLEINE (C2).
  float _computeDispersion() const;
};

#endif // SENSOR_FILTER_H
