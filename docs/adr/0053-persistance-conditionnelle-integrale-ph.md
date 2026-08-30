# ADR-0053 — Persistance conditionnelle de l'intégrale pH : ce qui est restauré n'est pas une autorisation de doser, c'est la vitesse d'une correction déjà décidée par la mesure

- **Statut** : Accepté — **amende la clause « Aucune persistance NVS » de l'[ADR-0052](0052-terme-integral-ph-dimensionne-par-autorite.md)**. Tout le reste de l'ADR-0052 est **inchangé** : autorité `kPhIntegralAuthorityMlPerMin` = 6,0 mL/min, `integralMax` dérivé de `U × Ti / kp`, intégration signée sur `erreur − phStopThreshold`, gel d'anti-windup asymétrique, `Kd = 0` sur les deux chimies, `Ki_ORP = 0`.
- **Date** : 2026-08-30
- **Décideurs** : gate `pool-chemistry` (**GO**, 30/08/2026, après réexamen du plafond de restauration), `architect`, `code-reviewer` (trois défauts fail-open relevés puis corrigés), utilisateur
- **Spec(s) liée(s)** : [feature-084-integrale-ph-cadence-et-memoire](../../specs/features/doing/feature-084-integrale-ph-cadence-et-memoire.md)

## Contexte

### Ce que l'ADR-0052 interdisait, et pourquoi

> « **Aucune persistance NVS** — un reboot repart de zéro. Une valeur stockée — a fortiori
> corrompue — serait une **autorisation de dosage pré-chargée au démarrage**. »
> *(ADR-0052, garde-fous C1-C5 ; repris dans « Ce que ça verrouille » : « L'intégrale ne
> sera jamais persistée en NVS, quelle que soit la commodité apparente. »)*

L'objection était juste et le reste : servir au boot une valeur relue d'une mémoire non
vérifiée revient à armer une demande de dosage que **rien dans le bassin n'a demandée**.

### Ce qui a changé et impose de rouvrir la question

1. **Le coût du redémarrage est devenu le facteur limitant.** Avec le tick d'entretien de
   la v2.26.0 (gardé par `canDose(0)`, rapport cyclique ≈ 25 %), la convergence de
   l'intégrale demandait **2,4 jours de filtration**. Chaque redémarrage repartait de zéro,
   c'est-à-dire effaçait deux jours et demi d'apprentissage.
2. **Le PANIC du 29/08 (`bug-080`) n'est pas élucidé.** Une régulation dont l'acquis est
   détruit par un redémarrage inexpliqué n'atteint jamais son régime.
3. Le remède au problème de fond relève de la
   [feature-085](../../specs/features/todo/feature-085-dose-de-base-apprise-feedforward.md)
   (dose de base apprise) et de la
   [feature-086](../../specs/features/todo/feature-086-limite-actionneur-dosage-ph.md)
   (l'installation est **limitée par l'actionneur** : ~911 mL/jour délivrables pour
   ~1 800 mL/jour de besoin). La feature-084 est une feature de **robustesse**, pas de
   vitesse : elle ramène le coût d'un redémarrage de **2,4 jours à environ une heure**.

## Décision

**L'intégrale pH est persistée en NVS**, sous **sept garanties cumulatives R1-R7**. Aucune
n'est facultative : retirée isolément, chacune rétablit l'objection de l'ADR-0052 intacte.

### Pourquoi l'objection tombe — et seulement de cette façon

L'objection de l'ADR-0052 se décompose en **deux volets**, qui appellent deux réponses
différentes.

**Volet « corrompue » — traité par le SCELLEMENT.** L'enregistrement porte un numéro de
schéma, une empreinte FNV-1a de ses octets amont, et n'est accepté que si la NVS en rend
**exactement** la taille attendue. Une taille inattendue, un schéma différent ou une
empreinte discordante valent **rejet net** : il n'y a jamais de réinterprétation partielle
d'un enregistrement à demi écrit. Le même scellement protège l'anneau de charge.

**Volet « pré-chargée » — annulé par R4, PAS par R1/R2.** C'est le cœur de l'amendement, et
il ne repose ni sur le plafond ni sur le contrôle d'intégrité :

> **La valeur relue n'est jamais appliquée au démarrage.** Elle reste *candidate* jusqu'à ce
> que le bassin démontre **lui-même**, pendant **5 minutes de temps de branche**, une erreur
> **au-dessus du seuil de démarrage** de l'hystérésis. Or dans cet état, la boucle aurait de
> toute façon **ouvert une session de dosage sur le seul terme proportionnel**.
>
> **Ce qui est restauré n'est donc pas une autorisation de doser : c'est la VITESSE d'une
> correction déjà décidée par la mesure courante.**

Un pH déjà à sa consigne au retour du courant ⇒ la candidate n'est **jamais** appliquée. Le
qualificatif « pré-chargée » ne décrit plus le mécanisme livré.

### ⚠️ Condition posée par le gate, à ne pas relire comme un détail d'implémentation

Le plafond de restauration est le **minimum de deux termes** :

```
plafond = min( 0,30 × integralMax ,
               0,50 × (charge_OBSERVÉE − kp × phStopThreshold) / ki )
```

Le gate a posé deux conditions explicites sur ce calcul.

**(a) Le second terme est LE PORTEUR de l'argument de sûreté.** Le forfait `0,30 ×
integralMax` est une butée absolue, utile mais **muette sur la chimie du bassin** ; c'est le
terme dérivé de la **charge observée** qui garantit que la valeur restaurée reste dans la
moitié convergente de l'équilibre. Par conséquent :

> **L'intégrité de la charge observée — fraîcheur de l'historique, plausibilité du
> dénominateur — est une PROPRIÉTÉ DE SÉCURITÉ, pas un confort de diagnostic.**

Toute retouche de l'anneau de charge (`recordPhLoadDay`, comptage des minutes de
circulation, fenêtre d'âge, filet de vraisemblance) touche donc au raisonnement de sûreté et
exige un visa `pool-chemistry`.

**(b) Le forfait seul NE SUFFIT PAS sous 1,88 mL/min de charge.** Écrit en fraction du
plafond, l'équilibre vaut `f* = (charge − kp × stop) / autorité`, soit `(charge − 0,08) / 6`.
L'égalité `f* = 30 %` se produit exactement à **charge = 1,88 mL/min**. Tout bassin
**moins chargé que celui de référence** — plus petit, bâché, TAC bas, acide plus concentré,
hors saison — verrait la butée forfaitaire passer **au-dessus de son propre équilibre**, et
l'affirmation « aucune restauration ne peut produire un surdosage net » s'effondrerait. À
1,5 mL/min, `f* = 23,7 %` : un forfait à 30 % restaurerait **1,27 × l'équilibre**. Aggravant :
un bassin deux fois plus petit a un gain pH double, donc des excursions doublées.

Sur l'installation de référence (charge délivrée observée), le terme de charge rend
**3 753 pH·s ≈ 23 %** là où le forfait autoriserait 30 % — c'est bien lui qui gouverne.

### Les sept garanties

| # | Garantie | Mise en œuvre |
|---|---|---|
| **R1** | **Plafond chimique de restauration** | `min(0,30 × integralMax ; 0,50 × (charge observée − kp × stop) / ki)`. **Fail-closed** : moins de 3 jours d'historique exploitable, `ki` inerte, charge non finie ou équilibre ≤ 0 ⇒ plafond **nul** ⇒ aucune restauration. Un appareil neuf ne restaure rien — comportement voulu. Le plafond est **réappliqué à l'instant de l'application**, jamais celui photographié au boot. |
| **R2** | **Invalidation contextuelle** | Empreinte FNV-1a sur (schéma, vitesse de régulation, type de correction `ph_minus`/`ph_plus`, consigne pH, `integralMax`). Toute différence ⇒ zéro. Une candidate en attente est **abandonnée** au changement de vitesse, à la bascule de produit, au passage en mode Programmée, au passage en `manual` et **à toute purge sèche**. |
| **R3** | **Péremption 6 h** (`kPhIntegralPersistMaxAgeS`), pas 24 h | 6 h = un `Ti` à vitesse `normale`. Mesurée sur l'**horloge murale** (c'est le reboot qu'il faut traverser) et **réévaluée à chaque tick** : une candidate peut mourir de vieillesse *pendant* son attente de confirmation. Horloge non synchronisée, horodatage nul ou situé dans le futur ⇒ périmé (un âge incalculable vaut un âge périmé). |
| **R4** | **Confirmation par la mesure** | 5 minutes **continues** de temps de branche avec `erreur > phStartThreshold`. Toute interruption (mode manuel, garde bloquante, mesure invalide, filtration coupée) remet la tenue à zéro. C'est la garantie qui annule le volet « pré-chargée ». |
| **R5** | **Cadence d'écriture CONJONCTIVE** | Au plus **une écriture par 30 min** **ET** seulement au-delà de **5 % du plafond** de variation. Ordre de grandeur : ~2 écritures par jour de filtration en régime établi, contre 100 000 cycles garantis par secteur NVS. Sans heure valide, **on n'écrit pas** (l'enregistrement serait de toute façon rejeté par R3). |
| **R6** | **Journalisation** | `info` à la retenue d'un instantané, `info` à la restauration effective (avec charge observée, profondeur d'historique, plafond et **origine du plafond**), `warning` à tout rejet motivé, `info` sur le cas nominal « plafond nul, historique trop court ». Un rejet muet rendrait la persistance indébogable. |
| **R7** | **Anti-boucle de démarrage** | Au-delà de **3 restaurations sans écriture normale intercalée**, la persistance se **désarme**. Le compteur vit **dans l'instantané NVS** (il doit survivre au reboot qu'il compte), est incrémenté **avant** toute confirmation (c'est le nombre de démarrages qui *tentent* qu'il faut borner) et son incrément est **saturé** — un repliement à 255 réarmerait silencieusement la persistance. |

### Les trois défauts fail-open trouvés en revue, et corrigés

Un ADR qui tait ses quasi-accidents ne prévient pas leur reprise. Les trois défauts
ci-dessous étaient présents dans la première implémentation ; tous allaient dans le **sens
fail-open** (surévaluer la charge observée, donc le plafond de restauration).

**1. Fenêtre d'âge absente sur l'anneau de charge.** L'anneau garde les **sept dernières
journées ARCHIVÉES**, ce qui n'est pas les sept derniers **jours** : un appareil arrêté deux
mois retrouve au démarrage des journées d'été intactes. Quatre journées estivales à
2,86 mL/min et trois journées d'automne à 1,00 mL/min donnent une médiane de **2,86** — la
valeur d'une saison révolue — soit un plafond de **3 753 pH·s** contre **2 484 pH·s**
d'équilibre réel : **1,5 × l'équilibre courant**, exactement ce que le gate déclare
inacceptable. Corrigé par une fenêtre d'âge de 7 jours **calendaires**
(`phLoadDayWithinWindow`, conversion Howard Hinnant — sans elle, « 20260301 − 20260228 »
vaudrait 73).

**2. Dénominateur perdu sur tous les chemins sans flush.** Le flush périodique des compteurs
journaliers (30 s) vivait en **fin** d'`update()`, derrière **quatre `return`** : OTA, mode
maintenance, capteurs non initialisés, stabilisation / assistant de débit. Pendant ces
épisodes l'eau **circule** et les minutes s'accumulent, mais `_dailyCountersDirty` n'était
jamais consommé — et l'OTA comme le mode maintenance se terminent par un **redémarrage**.
Chiffrage : une journée de 480 min de circulation dont 360 en maintenance et 120 en
régulation à 5 mL était relue **5/120 = 0,042 mL/min** au lieu de **0,0104**, soit une charge
observée **quatre fois trop grande**. Corrigé en remontant le flush **en amont de toutes les
sorties anticipées**, et en posant explicitement les compteurs à l'entrée en maintenance
ainsi qu'avant les redémarrages **OTA, mode AP, web et MQTT**.

**3. La purge sèche n'abandonnait pas la candidate.** Séquence atteignable **dans la fenêtre
R3 de 6 h** : candidate en attente → pluie légèrement acide → purge sèche (l'excursion
déclare l'accumulation caduque) → le pH remonte dans l'après-midi → la candidate se confirme
et **recharge l'autorité que la purge venait précisément d'invalider**. Une restauration ne
doit jamais survivre à la purge de ce qu'elle restaure. Corrigé : `kDryReset` abandonne la
candidate, au même titre que le changement de vitesse et la bascule de produit.

## Alternatives considérées

- **Confirmer l'interdiction de l'ADR-0052** (rejetée) — position défendable jusqu'au 29/08.
  Elle laisse chaque redémarrage détruire 2,4 jours d'apprentissage, sur un appareil dont le
  dernier PANIC n'est pas élucidé. La régulation n'atteindrait jamais son régime.
- **Persister et restaurer la valeur brute au boot** (rejetée) — c'est exactement
  l'« autorisation de dosage pré-chargée » que l'ADR-0052 décrit. Aucun des sept garde-fous
  ne remplace R4 sur ce point précis.
- **Plafond forfaitaire seul (`0,30 × integralMax`)** (rejetée — **c'est le piège de
  flotte**) — vrai sur le bassin de développement, faux dès que la charge tombe sous
  1,88 mL/min. Le `static_assert` correspondant compare la butée à **une** charge, celle de
  l'installation de référence ; il ne dit rien d'un autre bassin. La vérification qui vaut
  pour tous est faite **à l'exécution**, sur la charge observée.
- **Terme de charge seul, sans butée absolue** (rejetée) — la médiane peut être temporairement
  surévaluée (mise en service, purge, assistant de mesure de débit). La butée reste le
  plafond dur qui ne dépend d'aucune observation.
- **Restaurer 100 % de l'équilibre observé** (rejetée) — `I*` est la frontière convergence /
  cycle limite : au-dessus, la décharge ne vaut plus que 0,013 mL/min par heure, soit six
  jours sans retour. La fraction retenue (0,50) laisse 50 % de marge ; le `static_assert`
  borne le réglage à 0,80.
- **Péremption alignée sur les 24 h d'inactivité de l'ADR-0052** (rejetée) — 24 h laissent le
  bassin changer d'état. 6 h = un `Ti` : au-delà, la valeur ne décrit plus la charge courante.
- **Écrire l'instantané à chaque tick, ou à chaque variation** (rejetée) — usure NVS. La
  cadence R5 est **conjonctive** (temps **et** amplitude) précisément pour qu'aucune des deux
  conditions ne puisse être satisfaite en rafale.
- **Ranger la valeur dans la configuration générale** (rejetée) — une réécriture complète de
  la config à chaque flush. L'instantané a son **namespace dédié** (`pool-phint`, 20 octets),
  l'anneau de charge vit dans le namespace existant `pool-daily` (clé `ph_load7`, 64 octets,
  une écriture par jour).
- **Laisser les décisions R1-R7 dans la coquille ESP32** (rejetée) — ni `config.cpp` ni
  `pump_controller.cpp` ne sont dans le `build_src_filter` de `env:native` : trois mutations
  survivaient (supprimer l'écrêtage R1, supprimer la vérification de CRC, rendre le prédicat
  permissif). C'est la situation exacte qui avait précédé l'extraction de
  `ph_integral_logic.h`, où le défaut réel se trouvait **dans la garde de C2**. D'où
  [`src/ph_persist_logic.h`](../../src/ph_persist_logic.h)
  ([ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)).

## Conséquences

### Positives

- Un redémarrage coûte **~1 h de reconvergence au lieu de 2,4 jours**.
- L'argument de sûreté est **explicite et vérifiable** : la restauration ne s'applique que
  dans un état où la boucle aurait de toute façon ouvert une session de dosage.
- Le plafond est vérifié **à l'exécution sur la charge du bassin concerné**, ce qu'aucun
  `static_assert` ne peut faire — la flotte est couverte, pas seulement l'installation de
  développement.
- La charge acide observée (médiane sur 7 jours, en mL/min) **existe enfin** comme grandeur
  de premier ordre, réutilisable telle quelle par la dose de base apprise (feature-085).
- Toutes les décisions sont dans un module **pur** testable en natif : les trois mutations
  qui survivaient meurent désormais.

### Négatives / dette assumée

- ⚠️ **L'usure NVS des compteurs journaliers augmente d'un facteur 5 à 6** :
  `_dailyCountersDirty` est désormais posé **une fois par minute de circulation**, donc le
  flush 30 s se déclenche tant que l'eau tourne — de l'ordre de **420 à 600 transactions
  `pool-daily` par jour contre 70 à 100 auparavant**. Le choix est **délibéré** : numérateur
  (mL) et dénominateur (minutes) sont persistés **ensemble**, si bien qu'une coupure ne perd
  au plus qu'**une minute de chacun** et que le rapport mL/min est préservé. L'ordre de
  grandeur reste très en deçà des 100 000 cycles garantis d'un secteur, la NVS étant à
  nivellement d'usure. Ce coût doit être **présenté**, pas minimisé.
- Un appareil neuf, ou dont l'anneau a été vidé, **ne restaure rien pendant 3 jours**. C'est
  le fail-closed voulu, mais il rend la fonctionnalité invisible sur une mise en service.
- La restauration reste **plafonnée** : elle ne rend pas l'intégrale d'avant le redémarrage,
  seulement la moitié basse de l'équilibre observé. La reconvergence est raccourcie, pas
  supprimée.
- La persistance ajoute **deux structures NVS** dont le schéma doit être maintenu
  (`PhIntegralSnapshot`, `PhLoadHistory`). Toute retouche impose d'incrémenter le schéma
  correspondant, sous peine de réinterpréter des octets d'une autre version.
- ⚠️ **Non vérifié sur le terrain** : la v2.27.0 n'a pas été déployée au moment de la
  rédaction.

### Ce que ça verrouille

- **L'intégrité de la charge observée est une propriété de SÉCURITÉ.** Fenêtre d'âge, filet
  de vraisemblance, critère de jour exploitable, comptage des minutes de circulation et
  points de flush associés : toute modification exige un visa `pool-chemistry`. En
  particulier, **aucune sortie anticipée d'`update()` ne doit repasser devant le flush des
  compteurs journaliers**.
- **R1-R7 sont cumulatives.** Retirer une seule garantie rétablit l'objection de l'ADR-0052 ;
  il faudrait alors un nouvel ADR, pas un correctif.
- **La valeur relue n'est jamais appliquée au boot.** Court-circuiter R4 « pour aller plus
  vite » reproduit exactement la classe de défaut que cet ADR écarte.
- **Le plafond COURANT fait foi** à l'instant de l'application. Réutiliser celui photographié
  au boot rouvrirait la fenêtre changement-de-vitesse / changement-de-charge.
- **Une candidate ne survit à aucune rupture de sens** : changement de vitesse, bascule
  `ph_minus` ↔ `ph_plus`, mode `manual`, fin de mode Programmée, purge sèche. Ajouter un
  nouvel état de rupture impose d'ajouter l'abandon correspondant.
- **Le fail-closed est toujours du côté qui SOUS-estime la charge.** Tout nouveau critère
  d'exploitabilité d'une journée doit écarter **par le haut** seulement : une journée à
  volume nul reste **conservée**, l'écarter tirerait la médiane vers le haut.
- **Toute retouche de `PhIntegralSnapshot` ou de `PhLoadHistory` incrémente son schéma.** Un
  enregistrement d'un autre schéma est rejeté, jamais réinterprété.
- L'ADR-0052 reste en vigueur sur **tout le reste** : autorité 6 mL/min, `integralMax`
  dérivé, intégration signée, gel asymétrique, `Kd = 0`, `Ki_ORP = 0`, point d'équilibre à
  `consigne + phStopThreshold`, `Ti ≥ 4 ×` la pause mélange.

## Références

- Code : [`src/ph_persist_logic.h`](../../src/ph_persist_logic.h) (module **pur** : scellement
  et empreinte, empreinte de contexte, plafond R1, péremption R3, machine de confirmation R4,
  cadence R5, anti-boucle R7, anneau de charge et médiane),
  [`src/constants.h`](../../src/constants.h) (section « PERSISTANCE NVS de l'intégrale pH » :
  `kPhIntegralRestoreMaxFraction`, `kPhIntegralRestoreLoadFraction`,
  `kPhIntegralRestoreHistoryWindowDays`, `kPhIntegralRestoreMinHistoryDays`,
  `kPhIntegralPersistMaxAgeS`, `kPhIntegralRestoreConfirmMs`,
  `kPhIntegralRestoreArmWindowMs`, `kPhIntegralPersistMinIntervalMs`,
  `kPhIntegralPersistMinDeltaFrac`, `kPhIntegralRestoreMaxStreak`,
  `kPhIntegralPersistSchema`, `kPhLoadHistorySchema` + les `static_assert` (xi) à (xiv)),
  [`src/config.cpp`](../../src/config.cpp) / [`src/config.h`](../../src/config.h) (transport
  NVS seul : `loadPhIntegralSnapshot`, `savePhIntegralSnapshot`, `loadPhLoadHistory`,
  `savePhLoadHistory`, `recordPhLoadDay`, `readPhLoadObservation`,
  `phSustainableFlowFromConfig`, champ `SafetyLimits::dailyFiltrationMinutes`),
  [`src/pump_controller.cpp`](../../src/pump_controller.cpp) (câblage :
  `loadPhIntegralSnapshotDeferred`, `tickPhIntegralPersistence`, `phIntegralRestoreCap`,
  `refreshPhLoadObservation`, `tickDailyFiltrationMinutes`, `setMaintenanceInhibit`),
  [`src/web_server.cpp`](../../src/web_server.cpp) et
  [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp) (flush avant redémarrage)
- Tests : [`test/test_native_ph_persist/`](../../test/test_native_ph_persist/)
- Spec : [feature-084](../../specs/features/doing/feature-084-integrale-ph-cadence-et-memoire.md)
- ADR liés : [ADR-0052](0052-terme-integral-ph-dimensionne-par-autorite.md) (amendé sur le
  seul point de la persistance), [ADR-0054](0054-seuil-de-vraisemblance-fonction-de-la-configuration.md)
  (le filet de vraisemblance de l'anneau suit la configuration),
  [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (module pur),
  [ADR-0008](0008-persistance-cumuls-journaliers-nvs.md) (compteurs journaliers NVS, dont
  l'enregistrement porte désormais le dénominateur de la charge),
  [ADR-0016](0016-regulation-p-temporisee-vs-pid.md) (temporisation et anti-windup, inchangés)
- Doc : [pump-controller.md](../subsystems/pump-controller.md#cadence-mémoire-et-charge-observée-de-lintégrale-ph-v2270)
