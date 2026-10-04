# feature-139 — Le battement de la porte de mesure au seuil

## Métadonnées
- **ID** : feature-139
- **Slug** : battement-de-la-porte-de-mesure-au-seuil
- **Type** : feature — **assouplissement d'une garde de dosage**, donc à instruire avec prudence
- **Statut** : todo
- **Priorité** : **à rétablir après le livrable A1** — le 52 % de temps porte fermée est réel,
  mais il n'a coûté aucun millilitre le jour mesuré (plafond journalier déjà atteint).
  La priorité dépend du blocage **mordant**, qui reste à mesurer.
- **Version cible** : 2.55+
- **Dernière mise à jour** : 2026-10-04
- **Spec(s) liée(s)** : `specs/features/done/feature-091-…` (porte adaptative, C1/C2) · `specs/features/done/feature-097-…` (estimateur de bloc, planchers de bruit) · feature-138 (observabilité du blocage) · feature-137 (cause physique du bruit) · ADR-0059

## Domaines touchés
- [x] **Capteurs (pH / ORP / température)** — critère de stabilité
- [x] **Régulation (pump_controller, PID)** → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry — OBLIGATOIRE et BLOQUANT, avant toute écriture de code et après**
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

La porte de validité des mesures **bat au seuil** : elle s'ouvre avec quelques
millièmes de marge, puis se referme pour 20 minutes sur une seule fenêtre à
peine au-dessus du seuil de re-fermeture. Sur une après-midi mesurée, le dosage a
été bloqué **52 % du temps** alors que l'eau n'avait pas changé d'état.

Cette spec cherche à **supprimer le battement**, pas à assouplir le critère de
représentativité.

## Contexte — les mesures

### L'origine : l'incident du 2026-10-02

Pluies diluviennes, boue au fond du bassin, eau trouble. Les deux voies ont été
déclarées instables et tout dosage automatique s'est arrêté — **chlore compris,
au moment où la demande en chlore était maximale**. Deux jours plus tard l'ORP
était encore à **262 mV pour une consigne de 530**, filtres sains et dosage
pourtant autorisé : la demande n'était toujours pas satisfaite.

### Le relevé qui fonde cette spec

Journal de production, captures `/root/pool-captures/journal_2026100*.txt` :

```
2026-10-03 14:41:25  Re-purge n°2 — étendue pH 0.320 / ORP 20.9 mV
2026-10-03 15:06:27  Fin de re-purge n°2 (25 min, blocs 0.141)
2026-10-03 15:57:30  Re-purge n°3 — étendue pH 0.309 / ORP 29.2 mV
2026-10-03 16:20:31  Fin de re-purge n°3 (23 min, blocs 0.102)
2026-10-03 16:30:32  Re-purge n°4 — étendue pH 0.331 / ORP 29.7 mV
2026-10-03 16:49:33  Fin de re-purge n°4 (19 min, blocs 0.145)
```

**De 14:41 à 16:49 — 128 minutes, dont 67 porte fermée : 52 %.** La re-purge n°4
est survenue **10 minutes** après l'ouverture précédente.

### ⚠️ MAIS — ce 52 % n'a coûté AUCUN millilitre ce jour-là

Vérification faite le 2026-10-04 sur les mêmes journaux, **avant** d'ouvrir le
cycle. Le plafond journalier de chlore a été atteint **trois jours sur quatre** :

```
2026-10-01 00:00:01  Reset journalier — pH 0/2000 mL, ORP 1501/1500 mL   ← plafond
2026-10-02 00:00:00  Reset journalier — pH 0/2000 mL, ORP  950/1500 mL
2026-10-02 17:13:10  Alerte orp_limit — Limite journalière chlore atteinte
2026-10-03 00:00:00  Reset journalier — pH 0/2000 mL, ORP 2000/2000 mL   ← plafond
2026-10-03 15:14:50  Alerte orp_limit — Limite journalière chlore atteinte
2026-10-04 00:00:00  Reset journalier — pH 0/2000 mL, ORP 2000/2000 mL   ← plafond
```

Le 03/10, **le plafond journalier était atteint à 15:14:50** — soit AVANT les
re-purges n°3 (15:57) et n°4 (16:30). Pendant tout le battement chiffré
ci-dessus, le dosage chlore était **déjà** bloqué par la limite journalière. Et
le pH était à **0/2000 mL** : aucun acide n'était dû, le pH étant bas.

**Le battement de porte n'a donc coûté aucun millilitre cette après-midi-là.**

### Ce que cela change pour cette spec

1. **Le chiffrage A1 n'est plus une formalité, c'est le livrable qui décide si la
   spec a lieu d'être.** Il faut mesurer le temps de blocage **qui mord
   réellement** — c'est-à-dire porte fermée ALORS QUE le budget journalier et le
   budget horaire étaient encore disponibles. Tout le reste est du blocage
   gratuit, sans coût.
2. **La valeur de cette spec se joue sur les jours NORMAUX**, pas sur l'épisode
   d'eau boueuse où la limite journalière lie de toute façon. C'est exactement
   l'inverse de l'intuition qui a motivé l'ouverture de la spec.
3. **Si A1 conclut que le blocage mordant est négligeable, cette spec doit être
   fermée sans implémentation.** Ce serait un résultat, pas un échec.

### Et le vrai problème de l'épisode n'est pas logiciel

Deux litres de chlore par jour pendant trois jours, et l'ORP reste à **261 mV**
pour une consigne de 530. C'est la signature d'une **demande en chlore non
satisfaite** : la charge organique (boue) consomme l'oxydant aussi vite qu'il est
injecté, et l'ORP ne montera qu'une fois le breakpoint franchi. Aucun
assouplissement de garde ne résout cela — seul le retrait mécanique de la boue le
fait. **Relever encore le plafond journalier serait la mauvaise réponse** : on
formerait des chloramines sans faire monter l'ORP.

### Les marges réelles

| Événement | Valeur mesurée | Seuil | Marge |
|---|---|---|---|
| Ouvertures (stats de bloc) | 0.139 · 0.141 · 0.102 · 0.145 | `kPurgeStablePhRange` 0.15 | **5 à 9 millièmes** |
| Re-fermetures (étendue) | 0.320 · 0.309 · 0.331 | `kPurgeRecloseRangePh` 0.30 | **9 à 31 millièmes** |

`constants.h` annonce explicitement, pour le réglage actuel :

> ⚠️ Une seule trace, porte ouverte, mesurée le 20/09 ; marge retenue
> (`kPurgeStablePhRange` / `kPurgeNoiseFloorPh` = 0,15 / 0,10) : **1,5×
> SEULEMENT**.

En eau chargée, le bruit des statistiques de bloc est monté de **0,089 à ~0,14**.
**La marge annoncée de 1,5× est tombée à ~1,05×.** Le réglage n'est pas en
défaut : il est hors de son domaine de validité, et le projet avait écrit
d'avance que la marge était mince.

### L'asymétrie à instruire

Deux mécanismes du même firmware jugent la même chose et ne se comportent pas
pareil :

| Mécanisme | Fenêtres en dépassement avant d'agir |
|---|---|
| Latch EMI (`sensor_filter.cpp`) | **3** (`kSensorFilterDispersionLatchCycles`) |
| Re-fermeture STAB (`measure_validity_logic.cpp`, règle 4) | **1** |

La règle 4 est `if (st.valid && st.recloseArmed && in.unstable)` : **une seule**
fenêtre au-dessus du seuil referme la porte pour au moins un plancher complet.
Rien n'indique que cette asymétrie ait été décidée — elle est peut-être
simplement non remarquée.

### Ce que le plafond ne fait PAS

⚠️ **Le plafond `kPurgeCapMs` (90 min) n'a jamais déclenché** pendant l'incident.
Toutes les ouvertures se sont faites par stabilité, bien avant. Une première
version de cette idée visait à abaisser le plafond : **c'est écarté**, le plafond
n'est pas le mécanisme en cause. L'instruire quand même reviendrait à élargir une
garde qui ne s'est jamais exprimée.

## Comportement attendu

**Objectif : réduire le temps de blocage sans réduire la protection contre les
mesures non représentatives.** Trois pistes à instruire, non exclusives — c'est
`architect` + `pool-chemistry` qui tranchent, la spec ne présume pas :

1. **Persistance avant re-fermeture.** Exiger N fenêtres consécutives au-dessus
   du seuil de re-fermeture, par symétrie avec le latch EMI. Coût : jusqu'à
   (N−1) × durée de bloc de dosage sur des mesures possiblement mauvaises — **à
   chiffrer en millilitres**, c'est la grandeur qui compte pour `pool-chemistry`.
2. **Borne au nombre de re-purges par cycle d'eau.** Au-delà de N re-purges,
   adopter le comportement déjà existant du plafond : ouvrir avec alerte et
   **désarmer la re-fermeture** pour le reste du cycle. `repurgeCount` existe
   déjà et n'est aujourd'hui borné que par son type (255).
3. **Hystérésis dépendante du bruit observé.** Le facteur ×2 est figé ; le
   plancher de bruit, non. À instruire **avec réserve** : un seuil calculé
   pendant la purge serait gonflé par le transitoire lui-même — piste déjà
   écartée dans feature-097 pour cette raison exacte, ne pas la réintroduire
   sans traiter ce point.

### Cas limites
- Eau réellement instable (injection en amont, air dans le circuit) : la
  protection doit **tenir**. C'est le cas qui interdit d'assouplir bêtement.
- Sonde qui dérive lentement : la re-fermeture est l'un des filets. Vérifier que
  la piste retenue ne l'ouvre pas en grand (cf. limite connue de feature-091 :
  une dérive ≤ 0,017 pH/min est déjà jugée stable).
- Cycle d'eau très court (`computeAutoWindow` clampe à 1,0 h, bug-126) : une
  borne au nombre de re-purges ne doit pas rendre un cycle court inexploitable.
- Repliement de `millis()` sur un cycle d'eau long.
- Première ouverture du cycle : la re-fermeture n'est armée qu'après.

## Comportement actuel

`measureValidityTick()` règle 4 : une fenêtre dont l'étendue dépasse
`kPurgeRecloseRangePh` (0.30) referme la porte immédiatement,
`repurgeCount++`, et la réouverture exige de nouveau **plancher (10 min) +
fenêtre stable (10 min)**. `repurgeCount` n'est borné que par son type.

## Contraintes techniques
- Logique **pure** (ADR-0017) : `measure_validity_logic` et
  `purge_stability_logic` sont déjà des modules purs, testables nativement.
  **Y garder toute la décision** — aucune logique nouvelle dans la coquille.
- Ne toucher **ni** `kPurgeStablePhRange`, **ni** `kPurgeStableOrpRange`, **ni**
  le plancher `kPurgeFloorMs`, sauf verdict explicite de `pool-chemistry`.
- `static_assert` existants reliant plancher, fenêtre et cadence : les tenir à
  jour, et **ne pas en écrire un dont l'énoncé dit l'inverse de ce qu'il garantit**
  (défaut déjà rencontré sur `kPurgeMaxUndetectedDriftPhPerMin`).
- Aucune allocation sur chemin périodique (ADR-0082) ; `esp_task_wdt_reset()`.
- Toute constante nouvelle dans `constants.h`, **avec la mesure qui la justifie**
  et sa date (ADR-0073 : jamais un chiffre sans sa méthode).

## Fichiers potentiellement concernés
- `src/measure_validity_logic.{h,cpp}` — règle 4, `repurgeCount`
- `src/purge_stability_logic.{h,cpp}` — si le critère de persistance y loge
- `src/constants.h` — constante de persistance et/ou borne de re-purges
- `src/sensors.cpp` — journalisation des événements STAB
- `test/test_native_*` — suites existantes de la porte
- `docs/subsystems/`, `docs/adr/`, `CHANGELOG.md`

## Livrables — check-list
- [ ] A1 — **Chiffrage préalable, et il DÉCIDE de la suite** : temps de blocage **mordant**
      (porte fermée ALORS QUE les budgets journalier ET horaire étaient encore disponibles)
      sur les journaux disponibles, AVANT toute modification. Si le blocage mordant est
      négligeable, **fermer la spec sans implémenter** et le consigner.
- [ ] A2 — Piste retenue, et les deux autres **explicitement écartées avec leur raison**
- [ ] A3 — Implémentation dans le module pur, coquille inchangée
- [ ] A4 — Constante(s) nouvelle(s) documentée(s) avec mesure, date et marge
- [ ] A5 — Journalisation : dire **pourquoi** la porte ne s'est pas refermée quand la persistance l'a retenue
- [ ] D1 — Tests natifs : battement au seuil rejoué depuis les valeurs réelles du 03/10 ; eau franchement instable (0.918) toujours refermée ; cycle court
- [ ] D2 — **Mutation** : ramener la persistance à 1 fenêtre ⇒ un test doit mourir ; supprimer la borne de re-purges ⇒ un autre
- [ ] D3 — Non-régression intégrale des suites de feature-091 et feature-097
- [ ] C1 — `docs/subsystems/` du composant touché
- [ ] C2 — `CHANGELOG.md`
- [ ] C3 — ADR si l'asymétrie 1 vs 3 fenêtres est tranchée comme décision structurante

## Sécurité chimique
- **Dosage impliqué : OUI, indirectement mais réellement.** Cette spec **élargit
  la fenêtre pendant laquelle le dosage automatique est autorisé**. Elle va donc
  dans la direction « autoriser plus », pas « refuser plus ».
- Produits concernés : **chlore** (le manque est le problème constaté) et
  **pH− acide** (le risque, si une mesure non représentative passait la porte).
- **Risque à instruire** : une mesure transitoire franchit la porte et déclenche
  une injection d'acide sur un pH faussement haut. À chiffrer en millilitres sur
  la durée exacte d'exposition ajoutée, pas en appréciation.
- ⚠️ **Fait chimique à porter au dossier** : au moment de l'incident le pH était
  **bas** (brut 6,09–6,51, filtré ≈ 6,83 pour une cible de 7,3) — pluie acide et
  charge organique. Le blocage de la voie pH ne coûtait rien. **C'est le chlore
  qui a manqué.** Une piste qui rouvrirait les deux voies symétriquement doit
  être comparée à une piste qui traite d'abord l'ORP.
- ⚠️ **Et le contre-argument, à instruire sérieusement** : en eau chargée de
  matière organique, l'ORP **ne monte pas** tant que le breakpoint n'est pas
  atteint. Une régulation asservie à l'ORP peut alors injecter en poursuivant une
  consigne inatteignable, former des chloramines et vider un bidon sans résultat.
  **La garde qui bloque protège aussi de cela.** Toute ouverture doit être bornée
  en **volume**, pas seulement en ORP.
- Limites journalières / horaires, garde de présence d'eau, PID : **non touchés**.
- **Gate `pool-chemistry` bloquant avant code, et second passage sur le diff réel.**

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1 (contrainte imposée : tout futur critère de
  représentativité devra se positionner sur l'axe réactivité/persistance) ·
  2 (décision invisible : le choix du nombre de fenêtres encode un arbitrage
  exposition/blocage) · 3 (trois pistes crédibles, deux écartées)
- **Sujet pressenti** : « Combien de fois faut-il se tromper avant de fermer la porte »

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- Aucun

### Champs WebSocket
- Aucun — l'observabilité du blocage est l'objet de **feature-138**

### Topics MQTT
- Aucun

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/subsystems/<composant>.md` — critère de re-fermeture, constantes

#### Docs transverses
- `CHANGELOG.md` — comportement utilisateur modifié : la porte se referme moins souvent
- `docs/user/src/guide-regulation.html` — **si** le comportement de régulation visible change, puis `./tools/build_user_pdfs.sh`

#### Docs immuables
- `docs/adr/NNNN-*.md` — si l'asymétrie est tranchée

## Critères d'acceptation
- [ ] Le battement du 03/10 rejoué sur les valeurs réelles **ne referme plus** la porte à répétition
- [ ] L'étendue de 0.918 du 02/10 referme **toujours** la porte
- [ ] Le temps de blocage sur la période mesurée est réduit, et la réduction est **chiffrée**
- [ ] L'exposition ajoutée est chiffrée **en millilitres**, pas en minutes seules
- [ ] Aucun seuil d'ouverture ni plancher modifié sans verdict écrit de `pool-chemistry`
- [ ] `pio run` sans nouveau warning ; `pio test -e native` vert ; mutants tués
- [ ] Les suites de feature-091 et feature-097 passent sans modification

## Plan de test manuel
1. Rejouer les blocs réels du 03/10 (journaux versionnés) et comparer le temps porte ouverte avant/après.
2. Provoquer une instabilité franche (arrêt de circulation) : la porte doit se refermer.
3. Cycle de filtration court : vérifier qu'une borne de re-purges ne fige pas le cycle.
4. Observer une après-midi complète en production et compter les re-purges.

## Notes pour agent

⚠️ **Cette spec assouplit une garde de dosage.** Contrairement à bug-135 et
feature-137 qui corrigent des gardes refusant à tort sans danger, celle-ci
autorise à doser plus souvent. C'est la direction qui demande le plus de rigueur.

⚠️ **Mesurer AVANT de modifier (A1).** Le projet s'est déjà trompé en recopiant
un chiffre sans sa méthode (ADR-0073) et en mesurant la mauvaise grandeur (cause
racine du 15/08/2026). Les journaux de `/root/pool-captures/` contiennent les
valeurs réelles : s'en servir, ne pas estimer.

⚠️ **Ne pas conclure d'une seule après-midi.** Les données disponibles couvrent
un épisode d'eau boueuse exceptionnel. Le réglage retenu doit rester correct en
eau normale — où le plancher de bruit mesuré est 0,089, pas 0,14.

⚠️ **Le plafond 90 min n'est pas le sujet** et n'a jamais déclenché. Ne pas
dériver vers lui.

⚠️ **feature-137 est la vraie cause.** Si le bruit de mode commun était corrigé à
la source, le battement disparaîtrait sans toucher à aucune garde. Cette spec est
un palliatif assumé, et doit le dire dans son ADR.

## Hors périmètre
- Le plafond `kPurgeCapMs` et sa valeur.
- L'observabilité du blocage : **feature-138**.
- La cause physique du bruit : **feature-137**.
- La butée brute pH : **bug-135**.
- Un mode « récupération » autorisant le dosage porte fermée : non couvert ici,
  et à n'ouvrir que si cette spec échoue à supprimer le battement.
