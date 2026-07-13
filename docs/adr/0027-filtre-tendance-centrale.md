# ADR-0027 — Filtre pH/ORP recalé sur la tendance centrale (estimateur robuste + découplage valeur/latch)

- **Statut** : Accepté
- **Date** : 2026-07-11
- **Décideurs** : architect, pool-chemistry (double passage GO), code-reviewer
- **Spec(s) liée(s)** : feature-058 (successeur de feature-025 pour la stratégie de filtrage ; diagnostic feature-057)

## Contexte

Le filtre de mesure pH/ORP (`sensor_filter.*`) est l'**entrée du PID** de régulation. La chaîne d'origine (feature-025 : rejet de saut `maxStep` → médiane(7) → EMA, plus re-sync anti latch-up feature-033) souffrait de trois défauts révélés par la trace feature-057, sous forte oscillation (bruit CEM/pompe, ±0,3 pH, période ~20 s) :

1. le **rejet de saut** traitait les transitions de l'oscillation comme des aberrations → l'EMA n'intégrait qu'**un seul côté** de l'enveloppe → la valeur **collait** en haut ou en bas (bas hier, haut aujourd'hui) ;
2. la fenêtre médiane (7) était **plus courte que la période** → pas de moyenne symétrique ;
3. sur rejets saturés + latch instable, la valeur était **figée** sur un nombre trompeur (ex. 7,13 alors que le vrai pH ≈ 7,6–7,7).

La cause racine du bruit est **matérielle** (isolation/pompe, traitée séparément par l'utilisateur). En attendant ce correctif, une valeur filtrée **honnête et centrée** est **plus sûre** qu'une valeur collée ou gelée : coller au bas de l'enveloppe pH ferait sur-doser l'acide.

## Décision

On remplace la stratégie « rejet de saut → colle à l'enveloppe » par un **estimateur de tendance centrale robuste**, et on **découple la valeur du drapeau d'instabilité** :

- **Chaîne** : brut → rejet des **vraies aberrations uniquement** (NaN / hors-plage / spike DUR isolé `|raw − médiane| > spikeReject`) → **médiane sur fenêtre LONGUE** (13 échantillons ≈ 65 s, > 1 période) → **EMA** (α pH 0,10 / ORP 0,08).
- `filtered()` est **recalculée en continu** : c'est l'estimateur central courant. Elle n'est **jamais figée** ni ré-amorcée par une instabilité. Elle vaut `NaN` **uniquement** tant que la fenêtre centrale n'est pas pleine (warmup).
- `unstable()` (drapeau d'**inhibition de la régulation**) est **découplé** de la valeur : il vaut `true` si (a) la dispersion `max−min` de la fenêtre brute pleine dépasse le cap, **OU** (b) latch EMI (dispersion > cap sur N fenêtres pleines consécutives, levé uniquement par `reset()`), **OU** (c) rejets NaN/hors-plage consécutifs ≥ `maxConsecutiveRejects`.

L'oscillation **légitime soutenue** n'est plus rejetée : elle alimente le filtre et la médiane longue la moyenne sur ≥ 1 période → valeur centrée. Seule la **vraie aberration** (spike dur isolé, NaN, hors-plage) est écartée.

Paramètres arbitrés par pool-chemistry (dans `constants.h`) : `kSensorFilterCentralWindow=13` (buffer physique 16), `kPhFilterMaxDispersion=0,5` / `kOrpFilterMaxDispersion=100`, `kPhFilterSpikeReject=1,0` / `kOrpFilterSpikeReject=300`, `kSensorFilterDispersionLatchCycles=3`, α EMA inchangés, `kSensorFilterMaxConsecutiveRejects=10` conservé.

## Alternatives considérées

- **Assouplir le rejet de saut** (augmenter `maxStep`) *(rejetée)* — ne règle pas le collage d'enveloppe : soit on continue de rejeter un côté de l'oscillation, soit on laisse passer les vraies aberrations. Compromis instable, sensible au réglage.
- **Moyenne tronquée** (écarter k% extrêmes puis moyenner) *(rejetée)* — plus robuste qu'une moyenne simple, mais plus complexe à borner et à raisonner sur signal fortement corrompu ; la médiane longue donne le même centrage avec une garantie d'appartenance `[min,max]` triviale et un coût O(n log n) sur fenêtre fixe.
- **Conserver la mécanique re-sync/latch figeant la valeur** *(rejetée)* — c'est précisément la source du gel sur valeur trompeuse. On préfère toujours publier le meilleur estimateur central et signaler séparément qu'on ne peut pas doser.
- **Médiane longue + EMA + dispersion/latch découplés** *(retenue)* — centre honnête, robustesse aberrations conservée, inhibition de dosage préservée, module 100 % pur testable en natif (ADR-0017).

## Conséquences

### Positives
- La valeur filtrée reflète la **tendance centrale** réelle, ne colle plus à une enveloppe, ne gèle plus.
- Robustesse aux aberrations conservée (spike dur isolé, NaN, hors-plage écartés).
- Inhibition de la régulation **préservée** (drapeau dispersion + latch EMI + rejets), donc iso-sécurité.
- Module `sensor_filter.*` rendu **100 % pur** → couverture native complète (218 tests).

### Négatives / dette assumée
- **Latence +15 s** (fenêtre médiane 7 → 13, retard ≈30 s). Négligeable devant la pause mélange hydraulique post-injection (15–20 min) ; pH/ORP varient lentement.
- Sur oscillation **asymétrique**, le centre reste légèrement biaisé vers le côté dominant — inévitable sur signal corrompu, mais borné et plus sûr qu'un collage d'enveloppe ; le drapeau instable s'active si la dispersion est extrême.
- Le filtre **ne corrige pas** la cause du bruit : le vrai correctif reste **matériel** (isolation/pompe).

### Ce que ça verrouille
- **Invariant iso-sécurité** : `filtered() ∈ [min, max]` de la fenêtre observée, jamais tirée vers la consigne, jamais figée. Toute évolution future doit préserver l'inhibition de dosage sous bruit extrême et l'appartenance de la valeur à l'intervalle des mesures.
- Séparation stricte **valeur ≠ drapeau** : aucune instabilité ne doit refiger ou fausser `filtered()`.
- Suppression définitive de la mécanique `maxStep` / re-sync / warmup à seuil court : constantes retirées (`kSensorFilterMedianWindow`, `kPhFilterMaxStep`/`kOrpFilterMaxStep`, `kSensorFilterWarmupSamples`, `kSensorFilterResyncRejects`, `kSensorFilterMaxResyncPerWindow`, `kSensorFilterResyncWindowMs`).

## Références

- Code : `src/sensor_filter.h`, `src/sensor_filter.cpp`, `src/constants.h` (bloc feature-058)
- Spec : `specs/features/doing/feature-058-filtre-tendance-centrale.md`
- Tests : `test/test_native_sensor_filter/` (218 tests natifs)
- Doc : [`docs/subsystems/sensors.md`](../subsystems/sensors.md#filtrage-des-mesures-phorp--feature-025--feature-058), diagnostic [feature-057](../../specs/features/done/), ADR précédent implicite feature-025
