# ADR-0052 — Levée de `Ki = 0` sur la régulation pH : terme intégral dimensionné par l'AUTORITÉ, intégré en signé sur le seuil d'arrêt

- **Statut** : Accepté — **lève le `Ki = 0` de l'[ADR-0016](0016-regulation-p-temporisee-vs-pid.md), pour le SEUL pH** ; `Ki_ORP = 0` et `Kd = 0` **partout** restent inchangés, la temporisation (pause mélange, zone morte, anti-cycling) et l'anti-windup strict de l'ADR-0016 restent intégralement en vigueur.
- **Date** : 2026-08-28
- **Décideurs** : gate `pool-chemistry` (**GO SOUS CONDITIONS C1-C5**, 28/08/2026, + contre-visite sur la correction d'intégration signée), utilisateur (principe de non-biais de la consigne), architect
- **Spec(s) liée(s)** : [feature-078-rendement-des-impulsions-et-lenteur-regulation-ph](../../specs/features/todo/feature-078-rendement-des-impulsions-et-lenteur-regulation-ph.md) (cause n°1 : « la loi P plafonne la demande »)

## Contexte

### Le symptôme

La régulation pH **ne rejoint jamais sa consigne**. Terrain d'août 2026 : ~1 000 mL
d'acide injectés en une journée, pH **tenu à 7,50-7,66** pour une consigne à **7,30**.
Ni le produit, ni la pompe, ni les limites de dosage n'étaient en cause — la pompe n'est
utilisée qu'à **5 %** de sa capacité.

### Le diagnostic : ce n'est pas un défaut, c'est une propriété du correcteur

Un correcteur **proportionnel pur** soumis à une **charge permanente** ne peut
structurellement **pas** annuler une erreur statique : il se stabilise sur son *droop*

```
e* = besoin / (kp × T)
```

où `besoin` est le débit moyen nécessaire pour tenir le pH et `T` la fraction de temps
où la régulation est active (filtration en marche). Le terme proportionnel **est** la
seule source de débit : pour délivrer un débit non nul, il lui faut une erreur non nulle.
La consigne n'est donc atteignable qu'à charge nulle.

**Vérification numérique** (kp = 8, vitesse `normal`) : l'écart observé 0,358 pH donne
2,86 mL/min, soit 1 800 mL/j pour 628 min ≈ **10,5 h de filtration par jour** — exactement
le régime de l'installation. **Le système était à son équilibre théorique.** Il n'y avait
rien à réparer dans le firmware : il fallait changer de correcteur.

### La charge, mesurée

| Source de la charge basique | Part |
|---|---|
| Dégazage du **CO₂** (équilibre carbonate) | ~80 % |
| **Chlore** (hypochlorite, basique) | ~20 % |

Besoin ≈ **1 800 mL/j pour TENIR** le pH, ≈ **2 700 mL/j** pour le faire descendre de 0,2.

### Pourquoi trancher maintenant

L'[ADR-0016](0016-regulation-p-temporisee-vs-pid.md) (feature-025) posait `Ki = 0` comme
**non négociable**, et le code le répétait : *« Ki/Kd toujours forcés à 0 (P temporisée
pure — pool-chemistry feature-025) »*. Cette décision était **juste à l'époque** : la
régulation venait de basculer sur une mesure **filtrée**, la pause mélange n'avait pas de
recul terrain, l'anti-windup n'était pas éprouvé et le risque documenté — *« l'intégrale
s'emballe et provoque un surdosage avant que la mesure ne reflète l'injection déjà
faite »* — était réel et non maîtrisé. L'ADR-0016 avait d'ailleurs prévu la sortie :
l'infrastructure `PIDController` est **conservée pour permettre une réactivation
progressive de `Ki` après validation terrain**, et `integralMax` portait le commentaire
« conservé pour réactivation future ». La levée n'est donc pas une infraction : c'est le
**scénario de sortie prévu par l'ADR-0016**, déclenché par la validation terrain.

Ce qui a changé depuis, et qui rend la levée **sûre aujourd'hui** :

1. **L'anti-windup existe, est complet et testé.** `computePidPure`
   ([`src/dosing_logic.cpp`](../../src/dosing_logic.cpp)) : dans la bande morte, la sortie
   est forcée à 0 **et** l'intégrale n'est pas modifiée ; `allowIntegration =
   !freezeIntegral` ; bornage **symétrique** `±integralMax`. Couverture native 100 %.
2. **Six barrières de sécurité ont été ajoutées depuis la feature-025**, toutes en amont
   ou en aval du correcteur : seuils pH **fixes** dérivés de la plage de consigne avec
   `static_assert` ([ADR-0048](0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md)),
   deux **butées de dernier recours sur le pH BRUT** (6,20 / 8,10), **hystérésis** de
   relâche par seuil, **surveillance hors conjonction** d'éligibilité, **escalade
   fail-closed** à 3 refus / 24 h, **inhibition du chlore** sur pH haut — auxquelles
   s'ajoutent les limites journalière et horaire déjà présentes.

## Décision

### 1. `Ki` est levé sur le SEUL pH

`Kd = 0` reste **impératif sur les deux chimies** (un dérivé n'amplifierait que le bruit
résiduel du filtre) et `Ki_ORP = 0` reste **inchangé** : l'ORP dépend du pH, de la
température et du stabilisant, et son droop n'est pas le défaut diagnostiqué.

### 2. Le plafond se dimensionne par l'AUTORITÉ, jamais « en pH·s »

On pose d'abord le **débit maximal que l'intégrale seule est autorisée à demander**, puis
on en déduit le plafond :

```
kPhIntegralAuthorityMlPerMin = 6,0 mL/min      (2 × la charge d'équilibre mesurée 2,86)
ki          = kp / Ti
integralMax = U_I,max / ki  =  U × Ti / kp
```

| Vitesse | kp | Ti | ki (mL/min par pH·s) | `integralMax` (pH·s) |
|---|---:|---:|---:|---:|
| `slow` | 4 | 8 h | 1,389e-4 | **43 200** |
| `normal` (défaut) | 8 | 6 h | 3,704e-4 | **16 200** |
| `fast` | 12 | 4 h | 8,333e-4 | **7 200** |

`integralMax` **varie donc avec la vitesse de régulation**, exactement comme `kp` :
l'autorité est constante (6 mL/min), seule la **vitesse de charge** change. Neuf
`static_assert` verrouillent l'ensemble, dont **le verrou central** :
`ki × integralMax ≈ U` pour les trois vitesses.

`Ti` = 2 à 3 × le temps de brassage du bassin (4-6 h). Le retard de mélange
(`kPhMixingDelayMs` = 15 min) est **négligeable devant Ti** — verrouillé lui aussi par un
`static_assert` (`Ti_fast ≥ 4 × pause mélange`).

> **Le piège évité — à retenir.** L'ancien `integralMax = 50` (inerte tant que `Ki = 0`)
> aurait plafonné la contribution intégrale à `3,704e-4 × 50 = 0,0185 mL/min` pour un
> besoin de 2,86 — soit **0,6 %**, saturé en **140 s**. Réactiver `Ki` sans redimensionner
> `integralMax` aurait été un **échec silencieux** : une régulation « avec intégrale » sur
> le papier, un droop intact dans le bassin. C'est précisément cet invariant que le
> `static_assert` `ki × integralMax ≈ U` rend désormais impossible à violer.
>
> **Corollaire à ne jamais perdre de vue** : à l'équilibre l'erreur tend vers 0, donc le
> terme **P disparaît** et **l'intégrale porte TOUTE la charge**. Son autorité doit couvrir
> la charge, pas une correction d'appoint.

### 3. L'intégration est SIGNÉE, sur `erreur − phStopThreshold`, par quanta d'une seconde

```
dI/dt = (erreur − phStopThreshold)
```

évaluée à **chaque tick de la branche automatique**, que l'on dose ou non. La **sortie**,
elle, reste gatée par `shouldDose` et par la bande morte : entretenir l'intégrale
n'autorise aucune injection.

Trois propriétés de mise en œuvre, toutes justifiées :

- **Décharge structurelle** : dès que l'erreur passe sous le seuil d'arrêt, l'intégrande
  devient négatif. Le point d'équilibre s'établit à **`consigne + phStopThreshold`**,
  c'est-à-dire **au point exact où l'hystérésis de dosage coupe** — correcteur et
  ordonnanceur visent enfin le même point de fonctionnement. Résidu attendu : **+0,01 pH**.
- **Quanta d'une seconde** (`kPhIntegralChargeQuantumMs`) : à 16 200 pH·s, l'*ulp* du
  flottant simple précision vaut ~2e-3. Un incrément de 5 ms × 0,01 pH (5e-5) serait
  **purement et simplement perdu à l'addition** — la décharge n'aurait pas existé. Le
  reliquat sous le quantum est reporté, aucun temps n'est perdu.
- **`dt > 10 s` rejeté** (`kPhIntegralMaxTickDtMs`), jamais crédité d'un bloc : une pause
  mélange de 15 min chargerait sinon l'intégrale au plafond en un tour.
- **Gel ASYMÉTRIQUE** : l'anti-windup bloque la charge (delta positif), **jamais** la
  décharge. C'est l'anti-windup correct, et strictement plus sûr que l'ancien gel total.

### 4. Défaut préexistant corrigé au passage : bande morte alignée sur l'hystérésis

La bande morte passée à `computePidPure` valait **toujours** `phStartThreshold` (0,05),
y compris pendant une session **déjà ouverte**, alors que `shouldContinueDosing` déclare
poursuivre jusqu'à `phStopThreshold` (0,01). La plage `]0,01 ; 0,05]` était donc une
**session vivante à débit nul** : le seuil d'arrêt réellement appliqué était 0,05, pas
0,01. Conséquence directe sur l'intégrale : le régime établi se parquant à 0,05,
l'intégrande `(erreur − 0,01)` restait **positif en permanence** et le point fixe visé
était **inatteignable par la sortie**.

La bande morte suit désormais l'hystérésis : démarrage à `phStartThreshold`, poursuite
jusqu'à `phStopThreshold`. À l'équilibre, le volume total injecté est **inchangé** (bilan
de matière : il vaut la charge du bassin, quelle que soit la position de la bande morte).

### 5. La logique du terme intégral est un module PUR

`src/ph_integral_logic.h` ([ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)) :
charge signée, oubli exponentiel, reset sec, péremption 24 h, hystérésis de saturation,
paliers et rappel. **Motif** : les conditions C2-C5 du gate vivaient dans la coquille
ESP32 et **aucune mutation ne les tuait** — les mutations « restaurer le reset à −0,01 »
et « supprimer la décroissance » laissaient la suite native intégralement verte. Le défaut
trouvé (l'intégrale sans voie de décharge) était **exactement** dans la garde de C2.

### 6. Garde-fous (conditions C1-C5 du gate)

| Garde | Règle |
|---|---|
| **Gel sur saturation d'actionneur** | `demande ≥ ampFlow × budget_horaire / 60`. Le prédicat porte sur le **débit moyen soutenable** (~2 mL/min à 5 min/h), **pas** sur le débit d'amplitude (~25 mL/min) : comparé à l'amplitude, le gel ne se déclenchait **jamais** et ne protégeait de rien. |
| **Gel sur quota journalier** | Accumulation gelée au-delà de **80 %** du quota (`kPhIntegralFreezeDailyFraction`). Couvre la sonde bloquée sur une valeur haute, que rien d'autre ne borne : le volume déjà injecté est le seul témoin fiable de la divergence. Le terme acquis reste servi, on n'en ajoute plus. |
| **Reset sec** | Excursion **franche** de l'autre côté de la consigne : `erreur < −(startThreshold + 0,10)`, soit −0,15 par défaut — trois fois le bruit résiduel du filtre. |
| **Oubli exponentiel** | Demi-vie 1 h sous la consigne, par pas fixes (pas de `powf()` : 2,3 Ko de flash sur une ressource à 97 %), facteur vérifié par `static_assert`. |
| **Péremption** | Purge après **24 h** sans évaluation (mode `off`/`manual`, garde durable, filtration coupée). Appelée **hors de toute conjonction d'éligibilité**. |
| **Aucune persistance NVS** | Un reboot repart **de zéro**. Une valeur stockée — a fortiori corrompue — serait une **autorisation de dosage pré-chargée au démarrage**. |
| **Surveillance** | Anomalie = demande **saturée** ET erreur **encore au-dessus du seuil d'arrêt**. Tenue comptée en **temps de branche automatique** (un passage en `manual` suspend l'épisode). Hystérésis **95 / 85 %**, `warning` à 2 h, `critical` à **6 h** (= un Ti entier à autorité pleine), **rappel toutes les 12 h** tant que l'anomalie dure. |

### 7. Observabilité

Trois grandeurs publiées **à l'identique** sur `/data`, sur la trame WebSocket et sur MQTT
(mêmes noms, mêmes arrondis, mêmes règles `null`) : `ph_integral` (pH·s),
`ph_integral_term` (mL/min), `ph_integral_pct` (% du plafond). Trois entités Home Assistant
(le **pourcentage** en tête : seule grandeur lisible sans connaître un `integralMax` qui
varie), une alerte `ph_integral_saturated`, et un affichage en pied de la carte
« Régulation pH » (mode automatique seulement, libellés « correction lente » /
« rattrapage accumulé »).

## Alternatives considérées

- **Monter `kp` de 8 à 12 (`fast`)** (rejetée) — l'écart résiduel tomberait de 0,358 à
  **0,24** : le pH se parquerait vers **7,54** au lieu de 7,66. Insuffisant. Atteindre 7,35
  en P pur exigerait **kp ≈ 86**, soit un gain dix fois supérieur au plus agressif du
  projet, sur un procédé à retard long — inacceptable côté sécurité chimique.
- **Biaiser la consigne** (rejetée, **motif de principe**) — régler 7,05 pour obtenir 7,30
  *fonctionne*. Trois raisons de refus : (1) la consigne **cesserait de signifier ce
  qu'elle affiche** ; (2) le biais dépend d'une charge qui **varie** (bâche, saison,
  fréquentation) ; (3) il exigerait une **révision manuelle saisonnière** — intenable sur
  une flotte livrée à des tiers. Principe posé par l'utilisateur : *« les règles de
  protection pour limiter les risques ne doivent pas empêcher la bonne régulation »*.
- **Augmenter l'amplitude de la pompe ou la période d'impulsion** (rejetée) — **sans
  effet sur la droop**, qui est une propriété du correcteur et non de l'actionneur. La
  pompe n'est utilisée qu'à **5 %** de sa capacité : ni la puissance ni la durée ne sont
  la contrainte.
- **Décroissance exponentielle de l'intégrale dans la bande morte** (rejetée) — envisagée
  comme alternative à l'intégration signée. À demi-vie d'une heure, le rapport
  accumulation/décharge est d'environ **30 en défaveur de la charge** : l'intégrale ne
  pourrait plus tenir la charge basique mesurée (2,86 mL/min), et lever `Ki` deviendrait
  **inutile**.
- **Réactiver `Ki` en conservant `integralMax = 50`** (rejetée — c'est le piège) — 0,6 %
  du besoin, saturé en 140 s : un échec **silencieux**. Voir l'encadré ci-dessus.
- **Dimensionner `integralMax` « en pH·s » à l'intuition** (rejetée) — une valeur en pH·s
  n'a pas de sens physique isolément : seul le produit `ki × integralMax` (un débit) en a
  un. Le dimensionnement par l'autorité est la seule formulation vérifiable à la
  compilation.
- **Lever `Ki` aussi sur l'ORP** (rejetée, hors périmètre) — l'ORP dépend du pH, de la
  température et du stabilisant ; son droop n'est pas le défaut diagnostiqué et sa charge
  n'a pas été mesurée. Un ADR distinct sera requis.

## Conséquences

### Positives

- La régulation pH peut **enfin rejoindre sa consigne** — à `+0,01` près, résidu qui est
  exactement le seuil d'arrêt de l'hystérésis, donc le point où le dosage cesse par
  conception.
- Le dimensionnement est **vérifiable à la compilation** : neuf `static_assert`, dont le
  verrou `ki × integralMax ≈ U` qui rend l'échec silencieux impossible.
- L'anti-windup devient **asymétrique**, donc strictement plus sûr que le gel total qu'il
  remplace : la décharge n'est jamais bloquée.
- Le gel « sortie saturée » **protège réellement** (débit soutenable au lieu du débit
  d'amplitude) — il ne se déclenchait jamais auparavant.
- Un défaut latent est corrigé : la plage `]0,01 ; 0,05]` n'est plus une session vivante à
  débit nul.
- Les conditions C2-C5 sont **testables** : 823 → **849** tests, et **cinq mutations tuent
  désormais** — dont trois qui n'en tuaient aucune avant l'extraction du module pur.
- Une intégrale saturée n'est plus silencieuse : trois transports, trois entités HA, une
  alerte à rappel périodique.

### Négatives / dette assumée

- ⚠️ **Le résultat n'est pas vérifié sur le terrain.** La correction est **théoriquement
  fondée** (droop, bilan de matière, boucle fermée simulée) mais la v2.26.0 **n'a pas
  encore tourné sur le bassin**. Si le pH ne rejoint pas sa consigne, l'hypothèse de
  charge (~1 800 mL/j) est à réexaminer, pas le correcteur.
- Le terme intégral **peut demander jusqu'à 6 mL/min en permanence** si la mesure est
  fausse en restant plausible. C'est le risque que les six barrières, le gel sur quota à
  80 % et la péremption 24 h encadrent — pas un risque nul.
- L'intégrale est **perdue à chaque reboot**, à chaque changement de vitesse de régulation,
  à chaque bascule `ph_minus` ↔ `ph_plus` et après 24 h d'inactivité : la charge est alors
  ré-identifiée en un `Ti` (4 à 8 h). Coût accepté — c'est le prix de « jamais
  d'autorisation de dosage pré-chargée ».
- Le seuil d'affichage de saturation (95 %) est **recopié dans `data/app.js`** — miroir
  assumé, purement cosmétique : l'autorité reste l'alerte firmware, seule à tenir compte
  de la durée.

### Ce que ça verrouille

- **`Kd = 0` reste un invariant de sécurité** sur les deux chimies. Inchangé par cet ADR.
- **`Ki_ORP = 0` reste en vigueur** : lever `Ki` sur l'ORP exige un **nouvel ADR** et une
  mesure de la charge oxydo-réductrice.
- **`integralMax` ne se pose plus jamais à la main** : il se **dérive** de l'autorité et de
  `Ti`. Tout `Ki` posé sans reposer `integralMax` (`setPhPID()` en est le seul chemin
  restant, sans appelant à ce jour) reproduit l'échec silencieux.
- **`kPhIntegralAuthorityMlPerMin` est une constante de SÉCURITÉ CHIMIQUE** : elle fixe le
  débit qu'une sonde figée haut pourrait demander en permanence. Toute retouche exige un
  visa `pool-chemistry`.
- **L'intégrale ne sera jamais persistée en NVS**, quelle que soit la commodité apparente.
- **Le point d'équilibre de la régulation est `consigne + phStopThreshold`**, pas la
  consigne exacte : correcteur et hystérésis doivent rester alignés sur le même seuil. Les
  désaligner reproduirait le défaut de la bande morte.
- **Ti doit rester ≥ 4 × la pause mélange** : intégrer plus vite que le bassin ne
  s'homogénéise, c'est intégrer sur une mesure qui ne reflète pas les doses déjà passées.

## Références

- Code : [`src/ph_integral_logic.h`](../../src/ph_integral_logic.h) (module pur : charge
  signée, oubli, reset sec, péremption, hystérésis de saturation),
  [`src/constants.h`](../../src/constants.h) (section « RÉGULATION pH — TERME INTÉGRAL » :
  `kPhIntegralAuthorityMlPerMin`, `kPhIntegralTi*`, `kPhKp*`, `kPhKi*`, `kPhIntegralMax*`,
  les 9 `static_assert`), [`src/pump_controller.cpp`](../../src/pump_controller.cpp)
  (`applyRegulationSpeed`, `tickPhIntegralExpiry`, `applyPhIntegralVerdict`, tick unique
  dans la branche automatique, bande morte alignée sur l'hystérésis),
  [`src/pump_controller.h`](../../src/pump_controller.h) (`PIDController`, accesseurs
  `getPhIntegral*`), [`src/dosing_logic.cpp`](../../src/dosing_logic.cpp)
  (`computePidPure`, anti-windup préexistant)
- Tests : [`test/test_native_ph_integral/`](../../test/test_native_ph_integral/) — dont le
  test de droop (un P pur ne converge pas, un PI converge)
- Spec : [feature-078](../../specs/features/todo/feature-078-rendement-des-impulsions-et-lenteur-regulation-ph.md)
  (cause n°1 : « la loi P plafonne la demande — et ce n'est pas un défaut »)
- ADR liés : [ADR-0016](0016-regulation-p-temporisee-vs-pid.md) (`Ki = 0` levé sur le pH,
  le reste — `Kd = 0`, temporisation, anti-windup strict — inchangé),
  [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (module pur),
  [ADR-0043](0043-dosage-proportionnel-de-temps-amplitude-calibree.md) /
  [ADR-0046](0046-accumulateur-demande-sous-pouls-minimal.md) (l'ordonnanceur qui réalise
  la demande du correcteur, inchangé),
  [ADR-0048](0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md) (les barrières qui
  rendent la levée sûre),
  [ADR-0051](0051-pouls-minimal-double-rendement-avant-comptage.md) (levier de rendement
  appliqué en v2.25.3 sur la même spec)
- Doc : [pump-controller.md](../subsystems/pump-controller.md#terme-intégral-ph--levée-de-ki--0-v2260),
  [API.md](../API.md), [MQTT.md](../MQTT.md), [page-ph.md](../features/page-ph.md)
