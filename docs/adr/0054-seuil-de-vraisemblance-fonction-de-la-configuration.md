# ADR-0054 — Un seuil de vraisemblance dérivé d'un paramètre configurable est une FONCTION de ce paramètre, jamais une constante

- **Statut** : Accepté
- **Date** : 2026-08-30
- **Décideurs** : gate `pool-chemistry` (30/08/2026), `architect`, `code-reviewer`
- **Spec(s) liée(s)** : [feature-084-integrale-ph-cadence-et-memoire](../../specs/features/done/feature-084-integrale-ph-cadence-et-memoire.md)

## Contexte

### Le filet dont il est question

La [feature-084](../../specs/features/done/feature-084-integrale-ph-cadence-et-memoire.md)
introduit un **anneau de charge acide** : sept journées `{date, mL de correcteur pH,
minutes de circulation}`, dont on tire une **médiane en mL/min**. Cette médiane borne le
plafond de restauration de l'intégrale et porte, à ce titre, l'argument de sûreté de
l'[ADR-0053](0053-persistance-conditionnelle-integrale-ph.md).

Le dénominateur (les minutes de circulation) peut être **tronqué** : minutes perdues par un
flush NVS inatteignable, compteur remis à zéro en cours de journée, enregistrement à demi
écrit. Un dénominateur tronqué **surévalue** la charge mL/min, donc le plafond de
restauration — c'est un défaut **fail-open**. D'où un filet : une journée dont le rapport
dépasse ce que l'actionneur peut **physiquement délivrer** n'est pas l'observation d'un
bassin très chargé, c'est la **preuve** qu'il manque des minutes. Elle est **écartée**,
jamais rapiécée.

```
journée retenue  ⟺  ml / minutes  ≤  1,10 × débit_moyen_soutenable
```

### Le défaut de la première rédaction

Le seuil a d'abord été codé **en dur** à **2,17 mL/min** — c'est-à-dire `5/60 × 26`, les
réglages **par défaut** de l'installation de référence (limite horaire 5 min/h, débit
d'amplitude calibré ~26 mL/min).

Or le remède recommandé par le gate au déficit de dosage constaté est **précisément de
relever `phInjectionLimitMinutes`** (cf. « L'installation est limitée par l'actionneur »
ci-dessous). À **15 min/h**, le délivré légitime atteint **~6,5 mL/min**.

Conséquence d'un seuil figé à 2,17, dans le scénario **le plus probable** :

1. l'utilisateur applique la recommandation et passe la limite horaire à 15 min/h ;
2. **toutes** les journées deviennent « invraisemblables » ;
3. `days` retombe à 0 ;
4. le plafond de restauration devient nul, **la restauration ne s'arme plus jamais** ;
5. **sans un mot dans le journal.**

La fonctionnalité mourrait **au moment exact** où l'utilisateur suit le conseil donné.

## Décision

**Le seuil de vraisemblance est une fonction des réglages courants**, pas une constante :

```cpp
seuil = kPhLoadDayMaxRateFactor × phSustainableAvgFlow(ampFlow_calibré, budget_min_par_heure)
```

- `phSustainableAvgFlow()` est la **fonction pure déjà existante** utilisée par le gel
  d'anti-windup de l'intégrale ([ADR-0052](0052-terme-integral-ph-dimensionne-par-autorite.md)).
  `phSustainableFlowFromConfig()` ([`config.h`](../../src/config.h)) en est la **source
  unique** côté firmware : le filet de l'anneau et le prédicat de saturation ne peuvent pas
  diverger.
- `kPhLoadDayMaxRateFactor = 1,10` absorbe l'arrondi du volume au mL et le décalage d'une
  minute entre les deux compteurs. Elle ne rattrape **pas** un dénominateur tronqué : dans le
  scénario de référence (360 min de maintenance perdues sur 480), le rapport observé vaut
  4 × la vérité, très au-delà du filet.
- L'ancienne constante est **renommée** `kPhLoadSustainableFallbackMlPerMin` et devient une
  **valeur de repli**, employée uniquement quand le débit soutenable n'est **pas calculable**
  (pompe non caractérisée, configuration pas encore chargée, valeur non finie). Le
  commentaire l'énonce : *« Ne JAMAIS s'en servir comme seuil nominal. »*
- Le filet n'écarte **que par le haut** : une journée à volume nul et longue circulation
  reste **conservée** — elle tire la médiane vers le bas, donc du bon côté.

### La sentinelle verrouille les DEUX sens

Un test de seuil dérivé n'a de valeur que s'il meurt **quand le seuil redevient une
constante**. La suite native vérifie donc, sur la **même** journée à 6 mL/min :

| Limite horaire configurée | Débit soutenable | Verdict attendu |
|---|---:|---|
| 5 min/h | ~2,17 mL/min | journée **écartée** |
| 15 min/h | ~6,5 mL/min | journée **retenue** |

Un seuil figé — à 2,17 comme à 6,5 — casse **l'une des deux lignes**. C'est ce qui
distingue cette sentinelle d'un test qui se contenterait de vérifier un rejet.

### La règle générale, à appliquer au-delà de ce cas

> **Un seuil de vraisemblance dérivé d'un paramètre configurable doit être une FONCTION de ce
> paramètre, jamais une constante.** Le figer transforme un garde-fou en piège silencieux :
> il ne signale pas qu'il est devenu faux, il écarte simplement toutes les données — et la
> fonctionnalité qu'il protège meurt sans trace.

Corollaire : quand un seuil dépend d'un réglage que la documentation **recommande de
changer**, la question n'est pas hypothétique.

## Alternatives considérées

- **Garder la constante 2,17 mL/min** (rejetée — c'est le défaut) — juste tant que personne ne
  touche à la limite horaire, c'est-à-dire tant que personne ne suit la recommandation du
  gate. Échec **silencieux**, la pire des classes de défaut de ce projet.
- **Une constante généreuse couvrant le pire cas** (ex. 10 mL/min pour 30 min/h) (rejetée) — le
  filet ne filtrerait plus rien à 5 min/h, c'est-à-dire **dans la configuration par défaut**,
  celle où un dénominateur tronqué est le plus probable (journées courtes, part fixe des
  transitoires plus grande). On échangerait un faux positif silencieux contre un faux négatif
  silencieux.
- **Supprimer le filet** (rejetée) — le dénominateur tronqué est un défaut **réel et observé**
  (cf. le second défaut fail-open de l'[ADR-0053](0053-persistance-conditionnelle-integrale-ph.md)),
  et son sens d'erreur est celui qui **surévalue** le plafond de restauration.
- **Rendre le seuil configurable par l'utilisateur** (rejetée) — un second réglage à tenir
  cohérent avec le premier, sans information supplémentaire : le seuil se **déduit**
  entièrement de la limite horaire et du débit calibré.
- **Recalculer le seuil à l'écriture de la journée et le figer dans l'échantillon** (rejetée) —
  un changement de réglage postérieur rendrait l'historique incohérent, et l'échantillon
  passerait de 8 à 12 octets pour un gain nul : le seuil courant est disponible à chaque
  lecture.

## Conséquences

### Positives

- Relever `phInjectionLimitMinutes` — la recommandation explicite du gate — **ne casse plus**
  l'anneau de charge ni la restauration d'intégrale.
- Le filet et le gel d'anti-windup parlent du **même** débit soutenable, calculé par la **même**
  fonction pure : ils ne peuvent pas diverger.
- La sentinelle à deux sens rend la régression **détectable** : figer le seuil casse un test.

### Négatives / dette assumée

- Le seuil devient **dépendant d'une calibration** : une pompe mal caractérisée (débit
  d'amplitude erroné) déplace le filet. Le repli couvre l'absence de calibration, pas une
  calibration fausse.
- Le paramètre traverse trois appels (`refreshPhLoadObservation` →
  `readPhLoadObservation` → `phLoadObservation`) : trois signatures à maintenir cohérentes
  plutôt qu'une constante lue sur place.

### Ce que ça verrouille

- **Aucun futur seuil de vraisemblance de ce projet ne sera une constante** dès lors qu'il
  dérive d'un paramètre réglable. Si un tel seuil doit rester constant, l'ADR correspondant
  devra le justifier explicitement.
- **`kPhLoadSustainableFallbackMlPerMin` est un repli, pas un seuil.** L'employer comme valeur
  nominale reproduit le défaut corrigé ici.
- **`phSustainableAvgFlow()` reste la source unique** du débit moyen soutenable. Toute
  seconde formule de « ce que la pompe peut délivrer en moyenne » est à proscrire.
- Un seuil dérivé doit être couvert par une sentinelle qui **verrouille les deux sens**
  (au moins un cas retenu et un cas écarté, en faisant varier le paramètre) — un test à sens
  unique ne tue pas la mutation « seuil figé ».

## Références

- Code : [`src/ph_persist_logic.h`](../../src/ph_persist_logic.h)
  (`phLoadDayRatePlausible`, `phLoadSustainableOrFallback`, `phLoadDayUsable`,
  `phLoadObservation`), [`src/ph_integral_logic.h`](../../src/ph_integral_logic.h)
  (`phSustainableAvgFlow`, fonction pure partagée),
  [`src/config.cpp`](../../src/config.cpp) / [`src/config.h`](../../src/config.h)
  (`phSustainableFlowFromConfig`, source unique),
  [`src/constants.h`](../../src/constants.h) (`kPhLoadSustainableFallbackMlPerMin`,
  `kPhLoadDayMaxRateFactor` + `static_assert` associés)
- Tests : [`test/test_native_ph_persist/`](../../test/test_native_ph_persist/) — sentinelle
  à deux sens (même journée écartée à 5 min/h, retenue à 15 min/h)
- Spec : [feature-084](../../specs/features/done/feature-084-integrale-ph-cadence-et-memoire.md)
- ADR liés : [ADR-0053](0053-persistance-conditionnelle-integrale-ph.md) (l'intégrité de la
  charge observée est une propriété de sécurité — c'est ce qui rend ce filet nécessaire),
  [ADR-0052](0052-terme-integral-ph-dimensionne-par-autorite.md) (`phSustainableAvgFlow`, gel
  d'anti-windup sur le débit soutenable),
  [ADR-0050](0050-debit-pleine-puissance-constante-unique-de-flotte.md) (aucun miroir d'une
  valeur de sécurité — même famille de raisonnement : une valeur de sûreté a **une** source)
- Doc : [pump-controller.md](../subsystems/pump-controller.md#cadence-mémoire-et-charge-observée-de-lintégrale-ph-v2270)
