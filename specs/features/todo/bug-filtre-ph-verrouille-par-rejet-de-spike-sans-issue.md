# Bug — le filtre pH se verrouille définitivement sur un rejet de spike, et coupe TOUTE la régulation

- **Type** : bug critique (mesure / sécurité chimique / disponibilité)
- **Priorité** : critique — a laissé le bassin **sans régulation pH NI chlore** pendant ≥ 3 h
- **Statut** : todo
- **Version cible** : à définir
- **Créée** : 2026-08-16, après incident de production constaté et débloqué en direct
- **Domaine** : Capteurs (`sensor_filter`), Régulation pH/ORP, UI
- **Specs liées** : feature-058 (rejet de spike dur), feature-067 (offset pH + filets de sécurité), ADR-0017

## Incident constaté (mesures on-target, firmware 2.20.0)

```
/debug/sensor_filter_state
ph : raw=6.957  median=5.293(corrigé) filtered=5.282(corrigé) ready=false  rejected=255
orp: raw=556    median=561            filtered=568            ready=true   rejected=0
```

La médiane **interne** valait donc `5.293 − 0.50 = 4.793` (l'offset est appliqué en
sortie d'accesseur, ADR-0039). Écart avec le brut : **2,164 pH**.

## Cause racine — un piège sans issue, déterministe

`src/sensor_filter.cpp:98-108` : un échantillon dont `|raw − médiane| > spikeReject`
(`kPhFilterSpikeReject = 1.0`, `src/constants.h:495`) est classé « spike dur » et
**écarté avant d'alimenter le buffer**.

Or la médiane ne peut évoluer QUE si des échantillons entrent dans le buffer. Dès que
la valeur vraie s'éloigne durablement de plus de `spikeReject` de la médiane courante,
**tout échantillon suivant est rejeté à son tour** : la médiane est figée pour toujours.

Les deux échappatoires existantes ne s'appliquent pas :
1. `_consecutiveRejects` n'est **volontairement pas** incrémenté sur ce chemin
   (commentaire l.92-93) → le garde `maxConsecutiveRejects` ne se déclenche jamais.
2. `_dispersionCleanStreak` est remis à 0 sur tout spike écarté (condition
   `pool-chemistry` de feature-058) → l'auto-levée du latch EMI
   (`kSensorFilterLatchClearCycles`) ne se déclenche jamais non plus.

Le compteur `rejectedCount` sature à 255 et n'est qu'un indicateur : il ne pilote rien.
**Seul `reset()` sort de cet état.**

Le comportement est documenté comme un fail-closed volontaire (`sensor_filter.h:49-53` :
« un spike répété rend le filtre non prêt »), pensé pour un capteur défaillant. Il n'a
pas été pensé pour un **échelon légitime** de la grandeur mesurée.

## Conséquence — c'est le point grave

`ready() == false` en permanence, donc :
- dosage **pH** automatique bloqué (garde FilterNotReady) ;
- dosage **chlore** inhibé par le filet de la feature-067, l'appareil l'ayant écrit :
  `[Dosage ORP] pH brut indisponible (capteur muet ou filtre non amorcé) — dosage
  chlore automatique suspendu par sécurité`.

Le bassin n'était donc plus régulé **du tout**, sans alarme, sans notification, et sans
aucun moyen de reprise depuis l'interface : la seule porte de sortie est
`POST /debug/sensor_filter_reset`, une route de debug absente de l'UI.

L'utilisateur n'a détecté l'anomalie que parce que la **valeur affichée** lui semblait
incohérente — pas parce que le système l'a signalée.

## Ce qui a été fait en direct (déblocage, pas correctif)

`POST /debug/sensor_filter_reset` puis `ph_regulation_mode → automatic`. Les deux
filtres sont repartis et prêts en ~60 s ; `ph_dosing` et `orp_dosing` de nouveau actifs.
**Le défaut firmware reste entier.**

Note : la route de reset réinitialise **les deux** filtres (`{"reset":["ph","orp"]}`),
donc elle sacrifie le filtre ORP qui était sain — à revoir aussi.

## Le DÉCLENCHEUR — le filtre est empoisonné par des mesures que le système sait fausses

Le verrouillage décrit ci-dessus est le **mécanisme**. Son **déclencheur** est distinct, et
c'est lui qui rend le défaut RÉCURRENT plutôt qu'accidentel.

Chronologie relevée dans le journal, à la seconde :

```
06:07:59  Reprise circulation — stabilisation des mesures pH/ORP 5 min
06:12:59  [Dosage ORP] pH brut indisponible — dosage chlore automatique suspendu
06:13:00  Stabilisation terminée — mesures pH/ORP de nouveau représentatives
```

À l'instant précis où les mesures redevenaient représentatives, le filtre était **déjà**
verrouillé sur une médiane construite pendant la stagnation.

Le module `measure_validity` (`src/measure_validity.cpp:25-46`) détecte le front montant
d'eau et déclare les mesures **non représentatives** pendant `stabilizationDelayMin`. Il
suspend l'injection — correctement. Mais :

**`resetPhFilter()` / `resetOrpFilter()` ne sont appelés QUE après calibration réussie
(`src/sensors.cpp:656,671,701`) et depuis `POST /debug/sensor_filter_reset`. JAMAIS à la
reprise de circulation.**

Le filtre continue donc d'ingérer les échantillons de stagnation, sa médiane converge vers
la valeur de l'eau stagnante, et cette médiane acquiert ensuite **autorité sur les
échantillons représentatifs** qui la suivent — qu'elle rejette un à un comme aberrants.

En une phrase, le défaut de conception :
> **Le filtre ingère des échantillons que le système lui-même a déclarés non
> représentatifs, puis leur donne autorité sur les échantillons représentatifs suivants.**

Hypothèse sur l'excursion à ~4,8 : eau stagnante dans la chambre de mesure au voisinage du
point d'injection d'acide, donc localement très acide — **plausible et cohérente avec la
chronologie, mais NON VÉRIFIÉE**. Le cycle doit soit la confirmer (relevé du brut pendant un
arrêt de filtration), soit instruire une défaillance de sonde. Tant qu'elle n'est pas
tranchée, on ne sait pas si le correctif filtre suffit ou s'il masque un problème matériel.

Conséquence de périmètre : **corriger uniquement le verrouillage ne suffit pas.** Sans
traiter l'ingestion des mesures non représentatives, le filtre sera empoisonné à chaque
reprise de filtration ; la sortie de secours re-convergerait à répétition, après avoir
produit une médiane fausse entre-temps. Les deux volets sont nécessaires.

Bonne nouvelle : l'information requise **existe déjà** et est publiée
(`MeasureValidityResult.valid`). Le correctif consiste à la consommer, pas à la produire.

## Correctif attendu (pistes, à arbitrer par `architect` + `pool-chemistry`)

0. **Ne pas alimenter le filtre avec des mesures déclarées non représentatives**, ou
   ré-amorcer le filtre à la fin de la stabilisation. À arbitrer : geler l'alimentation
   pendant la purge (le filtre garde alors une médiane pré-arrêt, potentiellement périmée)
   ou réinitialiser à la transition purge → valide (warmup à chaque reprise, donc dosage
   retardé d'autant). Le second est plus sûr, le premier plus disponible — c'est
   `pool-chemistry` qui tranche.

1. **Sortie de secours obligatoire** : au-delà de N spikes consécutifs écartés, cesser de
   croire la médiane et re-converger (ré-amorçage du buffer sur les échantillons récents).
   Un capteur qui crache des aberrations ISOLÉES doit rester filtré ; une grandeur qui a
   franchement changé doit être suivie. Le critère qui distingue les deux est la
   **cohérence entre échantillons rejetés** : N rejets successifs *proches les uns des
   autres* décrivent une nouvelle valeur, pas du bruit.
2. **Alarme** : un filtre non prêt depuis plus de X minutes alors que la filtration tourne
   doit produire un `CRITICAL` et une alerte visible (UI + MQTT), pas un simple INFO noyé.
   L'absence totale de régulation ne doit jamais être silencieuse.
3. **Reprise depuis l'UI** : exposer la réinitialisation du filtre hors des routes de debug,
   et **par capteur** (ne pas sacrifier l'ORP pour réparer le pH).
4. Vérifier le même piège sur le filtre **ORP** (`kOrpFilterSpikeReject = 300 mV`) : la
   logique est partagée, donc le défaut l'est aussi.
5. Réexaminer `kPhFilterSpikeReject = 1.0` : un échelon de 1 pH est-il vraiment une
   aberration, sachant qu'une correction de calibration ou un ajout de produit peut le
   produire légitimement ?

## Points d'attention pour le cycle

- `pool-chemistry` est **obligatoire** : toute modification change ce que consomme la
  régulation, et la sortie de secours doit être fail-closed si le capteur est réellement HS.
- Ne PAS supprimer le rejet de spike : il protège d'aberrations réelles (feature-058).
- Les conditions bloquantes de feature-058 sur `_dispersionCleanStreak` doivent être
  préservées ou explicitement réarbitrées.

## Observations annexes du même incident

- Sonde de température d'eau signalée **figée** :
  `[SENSOR_FROZEN] Sonde T° eau figée : 900 lectures dans une bande < 0.03 °C autour de
  26.6 °C — compensation pH et planning filtration sur valeur figée`. À instruire à part.
- `ph_regulation_mode` était sur `manual` / `ph_enabled=false` sans trace dans le journal
  restant : impossible de dire si c'est l'escalade automatique de feature-067 (3 refus de
  plancher acide en 24 h) ou une action manuelle. **La bascule automatique en manuel doit
  être journalisée en `CRITICAL` et persistée**, sans quoi elle est indiagnosticable.

## Critères d'acceptation

- [ ] **Test de non-régression rejouant l'incident** à partir des valeurs réelles relevées :
      médiane établie à 4,79 puis échelon soutenu à 6,96 → le filtre doit redevenir `ready`
      et suivre, en un nombre d'échantillons **borné et asserté**. C'est le critère central :
      sans lui, rien ne garantit que le défaut ne réapparaisse pas.
- [ ] Une reprise de circulation ne laisse PAS la médiane de stagnation faire autorité sur
      les mesures représentatives qui suivent (test dédié sur la transition purge → valide)
- [ ] Une aberration isolée reste rejetée (non-régression feature-058), et la condition
      bloquante de feature-058 sur `_dispersionCleanStreak` est préservée ou réarbitrée
- [ ] Un filtre non prêt durablement, filtration active, déclenche une alerte visible
      (`CRITICAL` + UI + MQTT) — l'absence totale de régulation ne doit jamais être silencieuse
- [ ] La bascule automatique de la régulation pH en `manual` (escalade feature-067) est
      journalisée en `CRITICAL` et persistée
- [ ] La réinitialisation est accessible hors debug et agit **par capteur**
- [ ] Le même piège est instruit sur le filtre ORP (logique partagée)
- [ ] L'origine de l'excursion à ~4,8 est tranchée : stagnation acide confirmée, ou
      défaillance de sonde instruite à part
- [ ] Verdict `pool-chemistry` GO explicite
- [ ] `pio run` bi-core verts, `pio test -e native` sans régression (suite `sensor_filter`)
