# Bug — Le seuil d'inhibition ORP (`kPhRawOrpInhibit`) reste calé sur le pH BRUT sans asservissement à l'offset

- **Type** : bug de sécurité chimique (format minimal `specs/modele.md`)
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : à définir
- **Créée** : 2026-08-25 (relevé par le gate `pool-chemistry` pendant le cycle
  `bug-plancher-ph-offset-legitime-et-securite-desarmee`, v2.25.0 — signalé comme
  **même classe de défaut**, côté chlore, et laissé hors périmètre)
- **Domaine** : Firmware — régulation ORP, filets de sécurité pH brut
- **Specs liées** : `bug-plancher-ph-offset-legitime-et-securite-desarmee` (v2.25.0),
  feature-067 (offset pH), [ADR-0048](../../../docs/adr/0048-plancher-ph-continu-et-bornes-de-consigne.md),
  [ADR-0039](../../../docs/adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)

## Symptôme

`isOrpInhibitedByPhRaw()` (`src/pump_controller.cpp`) suspend le dosage chlore dès
que le **pH filtré BRUT** dépasse `kPhRawOrpInhibit = 7,60` (`src/constants.h`). Ce
seuil est **fixe**, alors que le pH brut d'exploitation dépend directement de
l'offset utilisateur (`pH_réel ≈ pH_brut + offset`).

Avec un offset **négatif légitime** — le cas symétrique de celui traité en v2.25.0,
et parfaitement atteignable puisque `kPhOffsetMin = −0,50` — le brut d'exploitation
vaut :

| Offset | pH réel visé (consigne) | pH BRUT correspondant | Inhibition ORP ? |
|---:|---:|---:|---|
| `0,00` | 7,40 | 7,40 | non |
| `−0,30` | 7,40 | **7,70** | **OUI, en permanence** |
| `−0,50` | 7,40 | **7,90** | **OUI, en permanence** |
| `−0,50` | 7,60 (borne haute) | **8,10** | **OUI, en permanence** |

Autrement dit : sur une installation à offset négatif, la régulation ORP peut être
**inhibée en permanence** alors que le pH réel du bassin est parfaitement nominal.
Le chlore n'est plus régulé du tout, et la seule trace est une alerte
`orp_ph_raw_inhibit` émise **une fois** (edge-triggered) au franchissement — donc
potentiellement invisible des semaines plus tard.

## Pourquoi c'est la même classe de bug que celui corrigé en v2.25.0

Côté acide, le plancher d'injection souffrait exactement du même défaut : un seuil
**fixe sur le brut** alors que la grandeur à protéger est le pH **réel** du bassin.
La v2.25.0 l'a corrigé en **asservissant le seuil à l'offset** tout en conservant
le raisonnement sur le brut (propriété structurante de l'[ADR-0039](../../../docs/adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md)) :

```
phRawFloorFor(offset) = max(kPhRawFloorAbsolute, kPhRealFloorTarget − offset)
```

`kPhRawOrpInhibit` n'a **pas** reçu le même traitement (périmètre v2.25.0 = pH).
Il reste donc, côté chlore, un seuil qui punit une correction d'offset légitime —
avec une conséquence inverse mais tout aussi grave : là où le plancher acide
**bloquait le pH−**, ce seuil **bloque la chloration**.

## Piste de correctif (à arbitrer par `pool-chemistry`)

1. **Asservir le seuil comme le plancher** (option symétrique, cohérente avec
   l'ADR-0048) :
   `orpInhibitRawFor(offset) = min(kPhRawOrpInhibitAbsolute, kPhRealOrpInhibitTarget − offset)`
   avec un plafond **RÉEL** constant (7,60 affiché) et une **butée haute dure** sur
   le brut, symétrique de la butée basse à 6,50 — elle protège d'un offset
   **négatif** saisi à tort (offset trop négatif ⇒ plafond réel visé trop haut).
   Choisir la valeur de butée est le vrai arbitrage : trop haute, elle laisse le
   chlore surdoser dans une eau réellement basique.
2. **Butée haute seule**, sans asservissement — plus simple, mais ne résout le cas
   nominal que partiellement.
3. **Ne rien faire et documenter** — à écarter : l'inhibition permanente est
   silencieuse et le défaut est démontré atteignable dans la plage d'offset
   autorisée.

Points d'attention :

- Le filet **fail-closed sur pH brut indisponible** (`NaN` ⇒ chloration automatique
  suspendue) ne doit pas être touché : `canDose(1)` ne consulte jamais le pH, ce
  filet est l'unique garde du couplage pH/chlore.
- Vérifier la cohérence avec `kPhTargetMax = 7,60`, dont le `static_assert`
  (`kPhTargetMax <= kPhRawOrpInhibit`, `src/constants.h`) s'appuie aujourd'hui sur
  la valeur brute fixe : si le seuil devient une fonction, l'invariant doit être
  réécrit sur la **branche la plus contraignante**.
- L'alerte `orp_ph_raw_inhibit` étant edge-triggered, envisager une **répétition
  périodique** tant que l'inhibition dure (patron `ph_filter_not_ready`, répétition
  toutes les 30 min) — une chloration suspendue depuis des semaines doit rester
  visible.

## Critères d'acceptation

- [ ] Avec un offset de `−0,50` et une consigne de `7,40`, la régulation ORP
      **n'est pas** inhibée (le pH réel du bassin étant nominal)
- [ ] Un pH réel réellement supérieur au plafond (offset nul, brut 7,80) inhibe
      toujours le dosage chlore
- [ ] Un offset négatif **aberrant** ne peut pas repousser indéfiniment le seuil :
      butée dure vérifiée
- [ ] Invariant `kPhTargetMax` ↔ seuil d'inhibition revu et verrouillé par
      `static_assert`
- [ ] Logique extraite en fonction pure testable en natif (patron `ph_guard_logic.h`,
      [ADR-0017](../../../docs/adr/0017-logique-metier-pure-humble-object-testabilite.md)),
      cas limites couverts (`NaN`, égalité au seuil, bornes d'offset)
- [ ] Gate `pool-chemistry` : GO amont **et** contre-visite
- [ ] `pio run` 0 warning, `pio test -e native` sans régression
- [ ] `docs/subsystems/pump-controller.md` mis à jour (la limitation y est
      actuellement documentée comme connue) + ADR si la décision retenue s'écarte
      de la symétrie avec l'ADR-0048

## Hors périmètre

- La valeur de l'offset elle-même et la correction physique des courants de fuite
- Le plancher acide (traité en v2.25.0)
- Le mécanisme d'alerte edge-triggered générique

## Note de clôture (2026-08-27)

**Résolue intégralement par la refonte des seuils pH (v2.25.0)**, sans cycle
dédié : le sujet de cette spec faisait partie du périmètre refondu. Les six
critères d'acceptation sont satisfaits — vérifiés un par un dans le code livré :

| Critère | Ce qui le satisfait |
|---|---|
| offset −0,50, consigne 7,40 : pas d'inhibition | l'inhibition porte désormais sur le pH **CORRIGÉ** (`kPhOrpInhibit` = 7,60), plus sur le brut |
| brut 7,80 à offset nul : inhibition | inchangé, et désormais exprimé dans le bon référentiel |
| un offset négatif aberrant ne repousse pas le seuil | **butée brute à 8,10** (`kPhCeilingRawHard` = `kPhOrpInhibit + kPhOffsetMax`), exigée par le gate en contre-visite |
| invariant consigne ↔ seuil verrouillé | `static_assert(kPhTargetMax + kPhOrpInhibitMargin <= kPhOrpInhibit)` — une consigne qui inhiberait le chlore en permanence ne compile plus |
| logique extraite en fonction pure | `src/ph_guard_logic.h` (`phGuardBlocksAbove`), 71 tests natifs dans `test_native_ph_guard` |
| gate GO amont **et** contre-visite | quatre avis `pool-chemistry` sur le cycle, dont deux ayant modifié une borne visible |

Deux durcissements non demandés par cette spec ont été livrés au passage :
- une **hystérésis par seuil** (le seuil corrigé et la butée brute ont chacun leur
  mémoire) — un drapeau partagé rendait inopérante l'hystérésis du seuil le moins
  souvent armé et produisait des alertes contradictoires ;
- l'alerte d'inhibition est désormais évaluée **hors de la conjonction
  d'éligibilité** : quota chlore atteint, elle part quand même. Avant, la seule
  alerte désignant la vraie cause était muette précisément dans ce cas.

Voir `specs/features/done/refonte-seuils-ph-fixes-derives.md`,
`docs/adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md`.
