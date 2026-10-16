# ADR-0113 — Les deux filets pH sur le brut sont des constantes chimiques absolues, indépendantes de toute borne de réglage de l'offset

- **Statut** : Accepté
- **Date** : 2026-10-16
- **Décideurs** : `pool-chemistry` (GO sous conditions, 2 passes), `architect` (trou d'asymétrie C5bis concédé par le gate), utilisateur (décision explicite du geste de correction, voir [ADR-0114](0114-correction-mesure-ph-sans-borne-de-reglage.md))
- **Spec(s) liée(s)** : [`feature-168`](../../specs/features/done/feature-168-correction-ph-sans-borne.md) · supersède [ADR-0048](0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md) · lié à [ADR-0039](0039-offset-ph-applique-en-sortie-des-accesseurs.md), [ADR-0098](0098-toute-constante-de-mesure-declare-sa-source.md), [ADR-0100](0100-un-seuil-de-sante-est-servi-jamais-reecrit-cote-client.md)

## Contexte

[ADR-0048](0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md) posait deux « butées de dernier recours » sur le pH **brut** (`kPhFloorRawHard`, `kPhCeilingRawHard`), dont la valeur **dérivait** de la borne de réglage de l'offset :

```
kPhFloorRawHard   = kPhFloorCorrected − kPhOffsetMax;   // 6,20
kPhCeilingRawHard = kPhOrpInhibit     + kPhOffsetMax;   // 8,10
```

feature-168 lève la borne de réglage de l'offset (`kPhOffsetMin`/`kPhOffsetMax` disparaissent, voir [ADR-0114](0114-correction-mesure-ph-sans-borne-de-reglage.md)) à la demande explicite de l'utilisateur. Cette dérivation met au jour un défaut structurel non identifié par ADR-0048 : **la correction commandait les gardes**. Élargir ou supprimer la borne d'offset aurait desserré ces deux filets de sécurité **sans qu'aucune décision explicite ne le dise** — un chemin par lequel une décision produit (« je ne veux pas de borne sur ma correction ») aurait silencieusement affaibli une garde chimique.

Un second défaut, latent dans ADR-0048 et jamais corrigé, apparaît au même moment : `kPhCeilingRawHard` protégeait contre un offset **négatif** erroné mais se calculait avec la borne **positive** (`kPhOffsetMax`). La coïncidence ne tenait que par la symétrie des bornes min/max — supprimer cette symétrie l'aurait rendue fausse silencieusement.

## Décision

**Les deux filets bruts deviennent des constantes absolues, à justification chimique PROPRE, et ne dérivent plus de rien lié à l'offset.**

```cpp
constexpr float kPhRawFloorAbsolute   = 5.50f;
constexpr float kPhRawCeilingAbsolute = 8.50f;

constexpr float kPhFloorRawHard   = kPhRawFloorAbsolute;    // identité, pas approximation
constexpr float kPhCeilingRawHard = kPhRawCeilingAbsolute;
```

Justification chimique ([`src/constants.h:1820-1825`](../../src/constants.h)) :

- **Plancher 5,50** : sous ~5,0, l'acide (pH−) rencontrant l'hypochlorite encore présent libère du **chlore gazeux** — danger direct, pas seulement chimique pour l'eau. 0,5 pH de marge avant ce seuil.
- **Plafond 8,50** : à 8,50, il ne reste que ~9 % de chlore actif (HOCl) pour un pKa de 7,5 (Henderson-Hasselbalch) : au-delà, alcaliniser encore n'a plus de sens sanitaire, quelle que soit la correction appliquée.

**L'inversion de dérivation** est le cœur de la décision : avant, `offset → filet`. Désormais, le filet est fixe et **c'est l'écart entre le filet et le seuil corrigé qui définit une zone d'impasse** (`kPhOffsetImpasseThreshold`, `kPhOffsetImpasseThresholdHigh`, [ADR-0114](0114-correction-mesure-ph-sans-borne-de-reglage.md)) — l'appareil le **dit** plutôt que de se fermer en silence.

**Conséquence sur l'invariant de compilation.** L'ancien invariant « la butée brute ne mord jamais avant le seuil corrigé, pour tout offset du domaine » n'est **plus vérifiable** : le domaine de l'offset n'est plus borné. Il est remplacé par un invariant d'**ordre absolu**, qui ne dépend plus de l'offset :

```cpp
static_assert(kPhRawFloorAbsolute < kPhFloorCorrected &&
              kPhCeilingCorrected < kPhRawCeilingAbsolute, …);
```

Il garantit qu'à offset nul (ou raisonnable), la régulation opère normalement entre les deux filets — quelle que soit la zone d'impasse que de grandes corrections peuvent ouvrir.

**Le défaut d'asymétrie est corrigé en même temps** : le filet haut protège désormais explicitement **deux** points d'application (inhibition ORP **et** refus d'alcaliniser pour `ph_plus`), documentés ensemble plutôt que par coïncidence arithmétique.

## Alternatives considérées

- **Conserver la dérivation et juste élargir `kPhOffsetMax`** (ce qu'aurait été une application naïve de la demande utilisateur) — *rejetée*. C'est exactement le défaut identifié : la correction continuerait de commander les gardes, et toute élargissement futur de la correction desserrerait les filets sans décision dédiée.
- **Rendre les filets continus, asservis à l'offset** (reprendre l'idée déjà abandonnée par ADR-0048 pour les seuils corrigés) — *non reconsidérée*. ADR-0048 a déjà tranché cette question pour la même classe de problème : un seuil que l'utilisateur ne peut pas réciter n'est pas un seuil de sécurité.
- **Butée haute dérivée séparément pour ses deux usages** (inhibition ORP d'un côté, refus d'alcaliniser de l'autre, avec deux constantes distinctes) — *rejetée*. Les deux usages protègent la même réalité chimique (eau trop basique) ; deux constantes auraient pu diverger sans qu'aucun invariant ne le remarque.

## Conséquences

### Positives
- Les deux filets bruts sont désormais des valeurs **que l'utilisateur peut vérifier au réactif** et qui ne bougent plus avec aucun réglage de l'appareil.
- Le défaut d'asymétrie (plafond calculé avec la mauvaise borne) est éliminé structurellement, pas seulement corrigé ponctuellement.
- Toute correction, même extrême, laisse les deux filets strictement à leur place : la seule chose qui change est si la régulation peut ou non les atteindre avant eux (zone d'impasse, [ADR-0114](0114-correction-mesure-ph-sans-borne-de-reglage.md)).

### Négatives / dette assumée
- **La marge de protection contre un offset réellement faux n'est plus bornée dans un sens précis** : avec un offset extrême, le filet brut peut mordre bien avant le seuil corrigé (zone d'impasse) — ce n'est plus l'exception calibrée d'ADR-0048, c'est une conséquence mécanique assumée de la levée de la borne de réglage.
- Les trois `static_assert` « ne mord jamais avant » d'ADR-0048 (iv, ix) sont retirés : toute tentative future de les restaurer doit d'abord relire que le domaine de l'offset n'est plus borné.

### Ce que ça verrouille
- **Aucun seuil de sécurité pH sur le brut ne peut à nouveau dériver d'une borne de réglage de l'offset** sans repasser par `pool-chemistry` et une nouvelle décision explicite — c'est précisément le défaut que cet ADR corrige.
- **Les deux filets bruts sont un contrat d'API public** : `ph_raw_floor_absolute` / `ph_raw_ceiling_absolute` sont servis par `/get-config` ([ADR-0100](0100-un-seuil-de-sante-est-servi-jamais-reecrit-cote-client.md)) ; tout texte d'interface qui les cite doit les lire là, jamais en dur ([ADR-0098](0098-toute-constante-de-mesure-declare-sa-source.md)).

## Références

- Code : [`src/constants.h`](../../src/constants.h) (`kPhRawFloorAbsolute`, `kPhRawCeilingAbsolute`, `kPhFloorRawHard`, `kPhCeilingRawHard`, invariant d'ordre absolu), [`src/pump_controller.cpp`](../../src/pump_controller.cpp) (`tickPhGuardSurveillance`)
- Tests : `test/test_native_ph_guard/test_ph_guard.cpp` (`test_derives_valeurs_numeriques_exactes`, `test_filet_brut_independant_de_l_offset_sur_tout_le_domaine`)
- Doc : [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md#gardes-de-sécurité-ph-refonte-v2250), [`docs/API.md`](../API.md), [`docs/features/page-ph.md`](../features/page-ph.md)
- ADR liés : [ADR-0048](0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md) (superseded), [ADR-0098](0098-toute-constante-de-mesure-declare-sa-source.md), [ADR-0100](0100-un-seuil-de-sante-est-servi-jamais-reecrit-cote-client.md), [ADR-0114](0114-correction-mesure-ph-sans-borne-de-reglage.md)
