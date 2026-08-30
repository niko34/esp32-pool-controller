# ADR-0051 — Pouls minimal doublé (5 s → 10 s) : améliorer le rendement d'abord, corriger le comptage ensuite

- **Statut** : Accepté — **amende le pouls minimal de 5 s (condition B3) de l'[ADR-0043](0043-dosage-proportionnel-de-temps-amplitude-calibree.md)** ; le reste de l'ADR-0043 (proportionnel de temps, amplitude calibrée, kick, période fixe, aucune interpolation) et la totalité de l'[ADR-0046](0046-accumulateur-demande-sous-pouls-minimal.md) sont **inchangés** — seuls leurs ordres de grandeur chiffrés se déplacent.
- **Date** : 2026-08-28
- **Décideurs** : gate `pool-chemistry` (**GO SOUS CONDITIONS**, 28/08/2026), utilisateur, architect
- **Spec(s) liée(s)** : [feature-078-rendement-des-impulsions-et-lenteur-regulation-ph](../../specs/features/todo/feature-078-rendement-des-impulsions-et-lenteur-regulation-ph.md)

## Contexte

Constat de terrain du 28/08/2026 : **968 mL comptés en une journée** et le pH reste à
**7,50** pour une consigne à **7,30**. Le besoin réel est de **800-900 mL pour ΔpH 0,2**
sur 40 m³ — chiffre établi par **deux voies indépendantes et concordantes** : la
composition du produit (pH Moins Ultra, 12 % H₂SO₄ + 13 % HCl ≈ **6,9 éq/L**) et le
dosage constructeur (0,2 L/10 m³ pour 0,1-0,2 unité). Près d'un litre d'acide a donc été
**compté** sans effet mesurable sur le bassin : le dimensionnement du produit n'est pas
en cause, c'est le **volume réellement délivré** qui est inférieur au volume compté.

Hypothèse retenue : la **perte fixe par démarrage** de la pompe péristaltique —
relaxation puis ré-occlusion du tube, reprise d'amorçage — représente **0,1 à 0,5 mL**
sur une pompe dont un tour vaut 1-2 mL. `pulseDeltaVolumeMl` compte comme si le débit
s'établissait **instantanément**. Facteur aggravant décisif : **toutes** les injections
observées sont au pouls minimal (`ph_pulse_ms = 5000` en permanence), donc l'erreur porte
sur **100 % du volume**, pas sur une fraction.

L'ADR-0043 verrouillait pourtant : « toute évolution des constantes `kPulse*` exige une
nouvelle mesure au banc + gate `pool-chemistry` ». La mesure de la perte fixe (M2 de la
feature-078) **n'est pas encore faite** — il fallait donc trancher : attendre la mesure,
ou appliquer d'abord le levier qui ne change pas la dose demandée.

## Décision

`kPulseMinMs` passe de **5 000 à 10 000 ms** ([`constants.h`](../../src/constants.h)).
Rien d'autre ne change : ni la période, ni l'amplitude, ni le modèle de comptage.

Ce que ce changement fait, et **surtout** ce qu'il ne fait pas :

- il **n'augmente PAS la dose demandée**. `planPulseWithCarry` accumule
  `meanFlow × period` et retire **exactement** `V_pouls` au tir : la moyenne restituée
  est **mathématiquement indépendante** de cette constante (vérifié par le gate dans
  [`pulse_dosing_logic.cpp`](../../src/pulse_dosing_logic.cpp)). Les impulsions
  deviennent **deux fois plus grosses et deux fois moins fréquentes** ;
- il améliore le **rendement** : deux fois moins de démarrages pour le même volume, donc
  la perte fixe est amortie sur deux fois plus de volume utile.

Conséquences chiffrées (débit à l'amplitude **mesuré** 25,75 mL/min, `flow255` 74,0) :

| Grandeur | Avant (pouls 5 s) | Après (pouls 10 s) |
|---|---:|---:|
| Volume d'une impulsion minimale `V_pouls` | 2,47 mL | **4,61 mL** |
| Seuil de bascule vers le régime proportionnel direct | 2,15 mL/min | **4,29 mL/min** |
| Cadence à demande 1,76 mL/min | ~1 impulsion / 1,4 min | **~1 / 2,7 min** |

**Innocuité du bolus unitaire** : 4,6 mL = **32 méq**, soit **ΔpH < 0,002** même sur
15 m³ ; négligeable jusqu'à ~5 m³. Réserve explicite du gate : un spa de moins de 2 m³
sortirait de ce raisonnement — hors périmètre de la flotte.

**`kPulsePeriodMs` reste à 60 s** (décision explicite) : la fraction de temps pompe
maximale est inchangée, et le refus antérieur de l'utilisateur d'allonger la période
(cycle feature-073, [ADR-0046](0046-accumulateur-demande-sous-pouls-minimal.md)) reste
fondé — allonger la période dégraderait la latence de régulation sans rien gagner sur la
moyenne.

**Effets de bord instruits, tous inchangés** :

- cycles/jour (`maxCyclesPerDay`) et anti-rafale sont comptés par **séance**, pas par
  impulsion ;
- la **limite horaire** est réévaluée à chaque tick : un dépassement reste borné par un
  tour de boucle, pas par 10 s ;
- la garde de **temps minimum d'injection** raisonne en temps **pompe-ON cumulé**
  ([ADR-0044](0044-budget-horaire-et-temps-min-en-temps-pompe-on.md)) — la durée murale
  d'une séance est inchangée ;
- la **pause mélange** est armée en **fin de séance** seulement.

## Alternatives considérées

- **Corriger d'abord le modèle de comptage** (rejetée **pour ce cycle**, pas sur le
  fond) — c'est la vraie correction, mais elle exige la mesure M2 de la perte fixe
  (24 pouls de 5 s contre 12 pouls de 10 s, à temps de pompe égal ⇒ `V₁₀ − V₅ = 12 × L`)
  et elle constitue une **hausse assumée de la dose réellement injectée**. On ne
  l'applique pas sur une estimation.
- **Allonger la période à 120 s** (rejetée, cohérente avec le refus utilisateur de
  l'ADR-0046) — abaisserait aussi le plancher de demande, mais au prix de la latence de
  toute la régulation. Le pouls minimal suffit.
- **Monter `kp` de 8 à 12 (`fast`)** (rejetée) — P pur sur un procédé à retard long
  (mélange 15 min, brassage 4-6 h) : dépassement sous pH 7,0 probable. Déplace le
  symptôme, ne touche pas au rendement.
- **Augmenter l'amplitude au-delà de 50 %** (rejetée, contre-productive) — exige une
  nouvelle pesée et, à moyenne constante, **raccourcit** les pouls : exactement l'inverse
  de l'objectif.
- **Relever la limite journalière** (sans objet) — la limite n'est pas atteinte.
- **Pouls minimal > 10 s** (écartée) — le bolus unitaire croît linéairement alors que le
  gain de rendement décroît ; 10 s conserve une marge d'un facteur ~3 sur l'innocuité du
  bolus (ΔpH < 0,002 sur 15 m³) et une latence de régulation acceptable.

## Conséquences

### Positives
- Deux fois moins de démarrages pour un même volume : la perte fixe par démarrage est
  amortie sur deux fois plus de volume utile, et le débit d'une impulsion de 10 s est
  **plus reproductible** que celui d'une impulsion de 5 s (transitoires moins dominants).
- **Aucune modification de la dose demandée** par la régulation : la moyenne restituée
  par l'accumulateur est indépendante de la constante — pas de « hausse de dose » cachée.
- Aucune garde, aucun seuil, aucun contrat externe touché : un seul entier change.

### Négatives / dette assumée
- ⚠️ **Le compteur reste FAUX** (condition n°1 du gate). En divisant les démarrages par
  deux à volume **compté** constant, on augmente le volume **réel** délivré sous le même
  plafond journalier : la limite journalière devient légèrement **moins conservatrice**
  (quelques dizaines de mL/jour si la perte vaut ~0,5 mL). Cet allongement **atténue** le
  défaut de comptage, **il ne le corrige pas**.
- **Exigences du gate, dues** : (1) surveiller le plafond journalier sur **3 jours** ;
  (2) la **mesure de la perte fixe** reste due (M2), après quoi le modèle de comptage
  devra être corrigé — voir
  [feature-078](../../specs/features/todo/feature-078-rendement-des-impulsions-et-lenteur-regulation-ph.md).
- La restitution des très faibles demandes est **plus grossièrement quantifiée** (paquets
  de 4,61 mL au lieu de 2,47 mL) et différée de deux fois plus de périodes.
- L'amélioration attendue du pH est une **hypothèse en cours de vérification**, pas un
  résultat mesuré : si le pH ne descend pas, la perte fixe n'était pas la cause dominante
  et la feature-078 reprend l'enquête (cause n°1 : la loi P plafonne la demande).

### Ce que ça verrouille
- Le pouls minimal **ne redescend pas sous 10 s** sans nouveau gate `pool-chemistry` :
  ce serait revenir au régime de démarrages dont on cherche à sortir.
- Toute nouvelle évolution de `kPulseMinMs` **ou** du modèle `pulseDeltaVolumeMl` exige
  la mesure M2 : on n'empile pas deux atténuations non mesurées.
- La correction du comptage, quand elle viendra, devra être traitée **explicitement comme
  une hausse de dose** (visa `pool-chemistry`), et **pas** comme un correctif neutre.
- `kPulsePeriodMs` reste à 60 s ; l'allonger exige un ADR distinct.

## Références

- Code : [`src/constants.h`](../../src/constants.h) (`kPulseMinMs`, commentaire de
  justification), [`src/pulse_dosing_logic.h`](../../src/pulse_dosing_logic.h) /
  [`.cpp`](../../src/pulse_dosing_logic.cpp) (`planPulseWithCarry`, `planPulse`,
  invariance de la moyenne), [`src/pump_controller.cpp`](../../src/pump_controller.cpp)
  (log de tir d'accumulateur, durée reprise de `kPulseMinMs`)
- Spec : [feature-078](../../specs/features/todo/feature-078-rendement-des-impulsions-et-lenteur-regulation-ph.md)
  (analyse `pool-chemistry` du 28/08, 7 leviers, protocoles de mesure M1 / M2)
- ADR liés : [ADR-0043](0043-dosage-proportionnel-de-temps-amplitude-calibree.md)
  (pouls minimal amendé — le reste inchangé),
  [ADR-0046](0046-accumulateur-demande-sous-pouls-minimal.md) (l'accumulateur est le
  mécanisme qui rend la moyenne indépendante du pouls minimal ; décision inchangée, ses
  ordres de grandeur chiffrés se déplacent),
  [ADR-0044](0044-budget-horaire-et-temps-min-en-temps-pompe-on.md) (temps pompe-ON — le
  rapport d'impulsion maximal passe de ×12 à ×6),
  [ADR-0050](0050-debit-pleine-puissance-constante-unique-de-flotte.md) (le sur-crédit du
  kick par volume compté est divisé par deux)
- Doc : [pump-controller.md](../subsystems/pump-controller.md#pouls-minimal-porté-à-10-s-v2253)
