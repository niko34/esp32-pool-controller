# ADR-0114 — La correction de mesure pH n'a plus de borne de réglage, seulement une salubrité de domaine

- **Statut** : Accepté
- **Date** : 2026-10-16
- **Décideurs** : utilisateur (décision explicite, tranchée en connaissance de cause), `pool-chemistry` (GO sous conditions, 2 passes, alternative chiffrée proposée et refusée)
- **Spec(s) liée(s)** : [`feature-168`](../../specs/features/done/feature-168-correction-ph-sans-borne.md) · lié à [ADR-0113](0113-filets-ph-bruts-absolus-independants-de-loffset.md), [ADR-0048](0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md) (seuils dont la dérivation change), [ADR-0100](0100-un-seuil-de-sante-est-servi-jamais-reecrit-cote-client.md)

## Contexte

La correction de mesure pH (`ph_offset`, [ADR-0039](0039-offset-ph-applique-en-sortie-des-accesseurs.md)) était bornée à ±0,50 pH depuis feature-067. L'utilisateur mesure un décalage **réel et établi** de 1,0 pH sur son installation — eau à pH 8, sonde lisant 7 à cause des courants de fuite de la pompe de filtration (phénomène documenté de cette installation, pas une suspicion de sonde). La borne lui interdisait d'enregistrer la correction dont il a besoin.

`pool-chemistry` a présenté deux options, chiffrées et justifiées chimiquement :

1. Élargir la borne à une fenêtre large mais toujours finie, **−0,90 / +1,20**, calée sur la chimie de l'eau (marge avant que le filet brut ne morde avant la consigne).
2. Retirer toute borne de réglage : la correction accepte **toute valeur finie de l'échelle pH**.

**L'utilisateur a choisi explicitement la seconde, en connaissance de cause** (« je ne veux pas de limite… le message d'avertissement peut rester, mais je ne veux pas bloquer »). La décision est la sienne et n'est pas à rediscuter.

## Décision

**La correction de mesure pH accepte toute valeur finie de l'échelle pH. Il ne reste qu'une salubrité de domaine, pas une borne de réglage.**

```cpp
// src/ph_offset.h
inline bool isPhOffsetValid(float offset) {
  return isfinite(offset) && (fabsf(offset) <= kPhOffsetScaleSpan);  // 14,0 = échelle pH entière
}
```

`kPhOffsetMin`/`kPhOffsetMax` sont supprimés. `roundPhOffsetToStep()` n'écrête plus — elle **arrondit seulement**, sous la précondition que l'appelant a déjà validé l'entrée par `isPhOffsetValid()` (vérifié : les trois appelants du projet le font tous).

**Ce qui remplace la protection perdue, par trois mécanismes distincts, aucun bloquant :**

| Mécanisme | Rôle | Constante |
|---|---|---|
| **Avertissement non bloquant** | Signal à l'écran dès que la correction dépasse l'ancienne borne | `kPhOffsetUnusualAbs = 0,50` |
| **Zone d'impasse** | L'appareil **dit** quand le filet brut (ADR-0113, fixe) va mordre avant la consigne | `kPhOffsetImpasseThreshold` / `…High` (calculées) |
| **Péremption à 30 jours** | Une correction inhabituelle non reconfirmée depuis 30 j est signalée | `kPhOffsetConfirmIntervalDays = 30` |

**Contrepartie assumée et non édulcorée.** Au-delà d'une correction de +1,20 (ou −0,90 côté haut), le filet brut mord **avant** la consigne : la régulation est en **impasse**. Ce n'est pas un défaut — c'est la conséquence assumée de la levée de la borne, et l'appareil le dit au lieu de se fermer en silence. Et si la correction elle-même est fausse, le plancher sur le pH **corrigé** ne protège plus (il est calculé à partir de la correction suspecte) : le seul filet qui reste indépendant de l'erreur de correction est le filet **brut** (ADR-0113).

**Une mémoire altérée doit être visible, pas seulement contenue.** L'écrêtage retiré, une valeur NVS corrompue mais dans l'échelle (ex. +9,30) est désormais **appliquée** plutôt que neutralisée à 0 — décision cohérente avec le choix utilisateur. Elle est signalée à chaque démarrage (bandeau + `critical` + MQTT) jusqu'à confirmation explicite, jamais bloquée ni effacée.

## Alternatives considérées

- **Borne large −0,90 / +1,20, calée sur la chimie** — *présentée, chiffrée, explicitement refusée par l'utilisateur*. Elle aurait laissé une marge avant la zone d'impasse en toute circonstance. Rejetée en connaissance de cause : l'utilisateur a choisi de ne pas réintroduire de plafond, même large.
- **Garder la dérivation existante et juste repousser `kPhOffsetMax` à 1,00** — *rejetée*. C'est la demande initiale littérale, mais elle aurait laissé les deux filets de sécurité chimiques (ADR-0113) dériver encore de cette borne — exactement le défaut structurel que ce cycle corrige ensemble avec ADR-0113. Un futur élargissement aurait de nouveau desserré les gardes sans décision dédiée.
- **Bloquer l'injection au-delà du seuil d'impasse plutôt que la signaler** — *rejetée*. L'appareil n'a aucune façon de distinguer une correction extrême légitime (installation à fort couplage électrique) d'une erreur de saisie ; fermer la régulation retirerait une protection réelle (le filet brut mord déjà) sans en ajouter une nouvelle — exactement le raisonnement qui exclut ces refus du compteur des 3 refus / 24 h.

## Conséquences

### Positives
- L'utilisateur peut enregistrer la correction dont il a besoin (+1,00), sans contournement ni édition manuelle de la NVS.
- Les trois signaux (avertissement, impasse, péremption) rendent visible ce que l'ancienne borne masquait par simple refus : une correction large reste un geste **informé**, jamais une boîte noire.
- Une mémoire altérée (NVS corrompue) devient **visible** au lieu d'être silencieusement neutralisée — l'utilisateur sait qu'il doit vérifier, au lieu de croire que « tout va bien » parce que l'appareil a corrigé tout seul.

### Négatives / dette assumée
- **Aucun garde-fou logiciel n'empêche plus une erreur de saisie de grande amplitude** (ex. taper 10 au lieu de 1,0). Seule la salubrité (NaN/inf/±14) est refusée. C'est le prix explicite de la décision utilisateur — il est amorti par l'avertissement et par le filet brut, jamais supprimé.
- **La marge de protection contre un offset faux se réduit à mesure que la correction grandit**, jusqu'à devenir négative au-delà du seuil d'impasse (la régulation n'atteint plus sa consigne). Documenté, pas corrigé : c'est la définition même de la zone d'impasse.
- Le message HTTP 400 change de **nature** : ce n'est plus « hors plage de réglage », c'est un refus de salubrité (valeur non finie ou hors échelle pH). Tout client qui interprétait ce message comme « corrigez dans la fourchette ±0,50 » doit être mis à jour.

### Ce que ça verrouille
- **Aucune borne de réglage ne peut être réintroduite sur `ph_offset` sans une nouvelle décision utilisateur explicite** — cette décision a été prise en connaissance de cause après une alternative chiffrée refusée ; elle n'est pas à rouvrir sur la base d'un confort d'implémentation.
- **`roundPhOffsetToStep()` ne doit plus jamais clamper** : réintroduire un écrêtage « par prudence » romprait silencieusement cette décision (une correction de +1,00 redeviendrait +0,50 au premier redémarrage, sans refus ni log). Un test de mutation dédié verrouille cette propriété.
- **`kPhOffsetScaleSpan` (salubrité) reste une garde d'API/MQTT, jamais servie au client** ([ADR-0100](0100-un-seuil-de-sante-est-servi-jamais-reecrit-cote-client.md)) — ce n'est pas un seuil de santé à afficher, c'est une garde de domaine interne.

## Références

- Code : [`src/ph_offset.h`](../../src/ph_offset.h) (`isPhOffsetValid`, `isPhOffsetStale`, `isPhOffsetExpired`), [`src/ph_guard_logic.h`](../../src/ph_guard_logic.h) (`roundPhOffsetToStep` sans clamp, `isPhRegulationImpasseLow/High`), [`src/config.cpp`](../../src/config.cpp) (`setPhOffsetStamped`, `loadMqttConfig`, signal de mémoire altérée), [`src/constants.h`](../../src/constants.h) (`kPhOffsetUnusualAbs`, `kPhOffsetScaleSpan`, `kPhOffsetConfirmIntervalDays`, `kPhOffsetImpasseThreshold(High)`)
- Tests : `test/test_native_ph_offset/`, `test/test_native_ph_guard/`
- Doc : [`docs/features/page-ph.md`](../features/page-ph.md), [`docs/API.md`](../API.md), [`docs/MQTT.md`](../MQTT.md), [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md)
- ADR liés : [ADR-0113](0113-filets-ph-bruts-absolus-independants-de-loffset.md), [ADR-0048](0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md), [ADR-0039](0039-offset-ph-applique-en-sortie-des-accesseurs.md), [ADR-0100](0100-un-seuil-de-sante-est-servi-jamais-reecrit-cote-client.md)
