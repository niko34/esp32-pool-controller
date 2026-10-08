# bug-142 — Le détecteur de sonde température figée produit un faux positif quotidien

## Métadonnées
- **ID** : bug-142
- **Slug** : detecteur-de-temperature-figee-faux-positif-quotidien
- **Type** : bug — **réglage de seuil**, pas de refonte
- **Statut** : todo
- **Priorité** : normale — aucun risque chimique, mais l'alarme se déclenche tous les jours et érode la confiance dans TOUS les avertissements
- **Version cible** : 2.56.1
- **Dernière mise à jour** : 2026-10-07
- **Spec(s) liée(s)** : feature-022 (origine du `FrozenDetector`) · `docs/subsystems/sensors.md`

## Domaines touchés
- [x] **Capteurs (pH / ORP / température)** — seuil du détecteur figé de la voie température
- [ ] Régulation (pump_controller, PID)
- [x] **Filtration** — non modifiée, mais CONSOMMATRICE : `isTemperatureFrozen()` gate
      le recalcul du planning (`filtration.cpp:162`) et l'éligibilité des échantillons (`:405`)
- [ ] Web — routes HTTP / WebSocket
- [x] **MQTT / Home Assistant** — `temp_sensor_frozen` change de fréquence, pas de contrat
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] **Frontend (`data/`)** — badge « Sonde température figée » (affichage seul, aucun code à changer)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry** — la température alimente la compensation pH ; le gate doit confirmer qu'allonger la fenêtre ne masque rien d'utile à la régulation
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Le badge « Sonde température figée » et le topic MQTT associé se déclenchent sur
une sonde **parfaitement saine**. La fenêtre de détection (30 min) est plus courte
que le temps pendant lequel un bassin sain reste légitimement sur un même pas de
quantification du DS18B20. Porter la fenêtre à **24 heures**.

## Contexte — mesuré sur l'appareil le 2026-10-07

### La preuve

Journal système de l'appareil, même journée :

```
16:39:21  WARN  [SENSOR_FROZEN] Sonde T° eau figée : 900 lectures dans une bande < 0.03 °C autour de 20.3 °C
16:58:58  INFO  [SENSOR_FROZEN] Sonde T° eau à nouveau vivante — détection figée levée
17:59:18  WARN  [SENSOR_FROZEN] Sonde T° eau figée : 900 lectures dans une bande < 0.03 °C autour de 20.3 °C
18:01:08  INFO  [SENSOR_FROZEN] Sonde T° eau à nouveau vivante — détection figée levée
```

Deux épisodes, dont un de **1 min 50**. Une panne de sonde ne guérit pas en deux
minutes.

L'historique de la même journée montre une sonde qui suit normalement la
journée : **20,5 → 20,7 → 20,8 → 20,9** (`/get-history?range=24h`).

### L'arithmétique du défaut

| Grandeur | Valeur | Source |
|---|---|---|
| Résolution DS18B20 | **12 bits** ⇒ pas de **0,0625 °C** | `sensors.cpp` — `g_ds18b20ResolutionBits = 12` |
| Bande du détecteur | **0,03 °C** | `kTempFrozenEpsilonC` |
| Fenêtre | **900 lectures × 2 s = 30 min** | `kTempFrozenSamples` |
| Dérive mesurée (phase la plus rapide) | **0,05 °C/h** (0,4 °C en 8 h) | historique 2026-10-07 |
| Temps pour franchir un pas | **1 h 15** | 0,0625 ÷ 0,05 |

**La bande (0,03) est plus étroite qu'un pas (0,0625).** Elle ne peut donc
contenir qu'**une seule valeur possible** : le détecteur ne dit pas « la mesure
varie peu », il dit « 900 lectures sont tombées exactement sur le même seizième de
degré ».

Or franchir un pas demande **1 h 15 au meilleur moment de la journée** — deux fois
et demie la fenêtre. La nuit, à l'équilibre thermique, la dérive tend vers zéro et
le pas peut tenir indéfiniment. Le déclenchement est donc **structurellement
quotidien**.

### Le commentaire du code dit l'inverse de la réalité

`src/sensors.cpp` justifie le réglage ainsi :

> *« l'epsilon 0.03 °C est < ½ LSB DS18B20 0.0625, le bruit de quantification
> d'une sonde vivante doit casser le run »*

Le raisonnement est retourné : **le bruit de quantification n'existe que si la
valeur vraie se trouve au voisinage d'une frontière de pas.** Loin d'une
frontière, le convertisseur sort une constante parfaite — cas normal pour une
masse d'eau de plusieurs dizaines de m³.

Le `FrozenDetector` est partagé avec le pH et l'ORP, où il est pertinent : ces
signaux bougent en permanence. Il a été appliqué à la température sans retenir
que l'inertie thermique change complètement l'échelle de temps.

### Conséquence annexe : `kTempFrozenEpsilonC` est une constante inerte

Tant qu'elle reste **sous 0,0625**, la valeur `0.03` et la valeur `0.001` donnent
**rigoureusement le même comportement** (un seul pas tient dans la bande). Elle a
l'apparence d'un bouton de calibrage, elle n'en est pas un. **Le seul vrai levier
est la fenêtre.** Ce point doit être écrit à côté de la constante, sinon le
prochain réglage portera sur le mauvais paramètre.

## Comportement attendu

- La fenêtre de détection passe à **24 heures** (`43 200` lectures à 2 s).
- Sur un bassin extérieur sain, l'alarme ne se déclenche **plus**.
- Une sonde réellement bloquée sur une valeur plausible reste détectée, en moins
  d'une journée.
- Le libellé du journal, du badge et du topic MQTT dit à l'utilisateur **quoi
  faire** (vérifier la sonde) plutôt que d'énumérer ce qui dérive.

### Pourquoi 24 h et pas 12 h

12 h a été envisagé puis **écarté sur données** : le 2026-10-07, le bassin est
resté affiché à 20,5 °C **de 08:00 à 13:00 locales**, soit six heures. Une
fenêtre de 12 h peut être avalée par une nuit calme suivie d'une matinée calme —
le faux positif reviendrait.

24 h est le seul seuil adossé à une **garantie physique** plutôt qu'à une marge
choisie à vue : sur 24 h un bassin extérieur traverse son cycle jour/nuit et
franchit plusieurs pas (six le 2026-10-07). Une sonde qui rend la même valeur sur
un cycle complet est réellement en panne — il n'y a plus de scénario sain.

### Cas limites
- **Lectures invalides** : seules les lectures VALIDES alimentent le détecteur
  (comportement actuel, inchangé). `frozen` persiste pendant une rafale
  d'invalides — cohérent, à ne pas toucher.
- **Redémarrage** : `_runCount` repart de zéro. Énoncé complet, tel que relevé par
  le gate `pool-chemistry` — ce n'est pas « 24 h de plus » : **un appareil qui
  redémarre plus souvent que toutes les 24 h ne détectera JAMAIS une sonde figée.**
  Accepté (aucun effet dosage, et le projet vise justement à réduire les
  redémarrages, cf. bug-134), mais **à écrire** dans la spec ET dans la doc — pas à
  corriger ici.
- **Sortie de l'état figé** : immédiate dès qu'une lecture quitte la bande
  (comportement actuel, inchangé).

## Comportement actuel

`kTempFrozenSamples = 900` (30 min). L'alarme se déclenche sur sonde saine,
plusieurs fois par jour, avec le message :

> `Sonde T° eau figée : 900 lectures dans une bande < 0.03 °C autour de 20.3 °C
> — compensation pH et planning filtration sur valeur figée`

## Contraintes techniques

- ⚠️ **`_runCount` est un `uint16_t` qui SATURE à 65 535** (`src/sensor_filter.h`).
  À 2 s par lecture, cela **plafonne la fenêtre à ~36 heures**. Au-delà, le seuil
  n'est jamais atteint et le détecteur **se désactive EN SILENCE** — il ne devient
  pas plus strict, il s'éteint. 43 200 passe, mais la marge n'est que d'un facteur
  1,5. **À documenter noir sur blanc à côté de la constante** : le prochain qui
  voudra « être prudent » avec 48 h cassera la fonction en croyant la renforcer.
- `kTempFrozenSamples` est un `uint16_t` : 43 200 tient.
- ⚠️ **« warning-only » ne vaut que pour le DOSAGE.** Le commentaire de `sensors.cpp`
  induit en erreur, et la première rédaction de cette spec l'a recopié. Vérifié :
  `isTemperatureFrozen()` n'atteint aucun chemin de `pump_controller`, mais il a
  **deux effets réels sur la filtration** (depuis bug-126) :
    * `src/filtration.cpp:162` — `if (sensors.isTemperatureFrozen()) return;`
      **bloque tout recalcul du planning** ;
    * `src/filtration.cpp:405` — `isSampleEligible(..., frozen)` **rejette les
      échantillons de température**.
  Le faux positif quotidien **dégrade donc le planning aujourd'hui**. Le correctif
  supprime cette dégradation : c'est un gain, pas seulement un badge en moins.
  Invariant à préserver : **aucun chemin de DOSAGE** ne doit dépendre de ce drapeau.
- Ne pas toucher au `FrozenDetector` lui-même : il est partagé avec pH et ORP, où
  le réglage actuel est correct.
- Ne pas toucher à `kTempFrozenEpsilonC` : inerte sous 0,0625, la modifier
  donnerait l'illusion d'un réglage. **Documenter, pas changer.**

## Fichiers potentiellement concernés
- `src/constants.h` — `kTempFrozenSamples` 900 → 43 200 ; commentaire sur le
  plafond `uint16_t` et sur l'inertie de `kTempFrozenEpsilonC`
- `src/sensors.cpp` — commentaire erroné à corriger (ligne du `addSample`) +
  libellé du message `[SENSOR_FROZEN]`
- `data/app.js` — libellé du badge « Sonde température figée » si reformulé
- `test/test_native_sensor_filter/test_sensor_filter.cpp` — valeurs épinglées
- `docs/subsystems/sensors.md` — seuil et justification physique
- `CHANGELOG.md`

## Livrables — check-list
- [x] A1 — `kTempFrozenSamples` = **43 200** (24 h à 2 s)
- [x] A2 — Commentaire de `constants.h` : **plafond `uint16_t` ≈ 36 h**, et au-delà le détecteur s'éteint en silence
- [x] A3 — Commentaire de `constants.h` : `kTempFrozenEpsilonC` est **inerte** sous 0,0625 ; le levier est la fenêtre
- [x] A4 — Commentaire erroné de `sensors.cpp` corrigé (le bruit de quantification ne casse PAS le run sur une masse d'eau)
- [x] A5 — Libellé `[SENSOR_FROZEN]` orienté action (vérifier la sonde) : retirer la
      dramatisation « compensation pH » (≈ 0,001 pH, ADR-0074), **conserver** la
      mention du planning de filtration, qui est le seul effet réel
- [x] A6 — **`static_assert(kTempFrozenSamples < UINT16_MAX)`** — rendu **BLOQUANT**
      par le gate `pool-chemistry` : un commentaire n'arrête pas ce piège
- [x] A7 — Commentaire de `src/sensors.h:410` (« 900 lectures à 2 s = 30 min ») mis à jour
- [x] A8 — Corriger partout l'affirmation « warning-only / aucun impact » :
      `src/sensors.h:163`, commentaire de `sensors.cpp`, et `docs/subsystems/sensors.md`.
      Le drapeau gate le planning de filtration (`filtration.cpp:162` et `:405`)
- [x] B1 — Badge UI aligné sur le nouveau libellé si A5 le change
- [x] D1 — Test épinglant `kTempFrozenSamples == 43200`
- [x] D2 — Test « dérive réaliste » : un échantillon franchissant un pas toutes les 1 h 15 ne déclenche JAMAIS
- [x] D3 — Test « sonde morte » : valeur strictement constante ⇒ figé à 43 200, pas avant
- [x] D4 — **Mutation** : remettre 900 ⇒ **la compilation échoue** sur le `static_assert` de
      borne basse, avant même que D2 tourne (mutant tué plus tôt que prévu ; D2 le tue aussi
      quand l'assert est neutralisé) · écrire 86 400 ⇒ replié en 20 864 par le `uint16_t`,
      **compilation refusée** par la même borne basse · 65 000 ⇒ refusé par la borne haute
- [x] C1 — `docs/subsystems/sensors.md`
- [x] C3 — Propager 43 200 / 24 h partout où 900 / 30 min est écrit : `docs/MQTT.md:691` ·
      `docs/API.md:413` et `:1890` · `docs/features/page-temperature.md:45,85,100,108` ·
      `docs/features/page-dashboard.md:235` · `docs/subsystems/ws-manager.md:759` ·
      `docs/subsystems/sensors.md:169,844,915`
- [x] C4 — Note de suite datée dans `docs/adr/0074-*.md` : la décision warning-only est
      CONFIRMÉE pour le dosage, mais « il n'y avait pas de seuil à durcir » (ligne 99) et
      « le drapeau ne déclenche rien » sont infirmés par bug-142. Ne rien inverser.
- [x] E1 — `src/version.h` : 2.56.0 → **2.56.1**
- [x] C2 — `CHANGELOG.md`

## Sécurité chimique
- Dosage impliqué : **non** — vérifié, `isTemperatureFrozen()` n'atteint aucun
  chemin de `pump_controller`.
- ⚠️ **Mais le drapeau n'est PAS inerte** : il gate le planning de filtration
  (`filtration.cpp:162` et `:405`). Voir Contraintes techniques.
- Limites journalières / horaires concernées : **aucune**
- Filtration requise avant action : **sans objet**
- Calibration capteur modifiée : **non** — seul un seuil de diagnostic bouge
- Algorithme PID touché : **non**
- **Risque identifié** : allonger la fenêtre retarde la détection d'une sonde
  réellement bloquée de 30 min à 24 h. La température alimente la **compensation
  pH** et le **planning de filtration**. Le gate `pool-chemistry` doit confirmer
  que ce retard est sans conséquence chimique — l'erreur de pH induite par une
  compensation figée a déjà été chiffrée (cf. `data/app.js`, commentaire du badge,
  qui la qualifie de WARNING et non DANGER).

## ADR attendu ?
- **Probable** : non
- **Critère(s) candidat(s)** : aucun — c'est une correction de seuil adossée à une
  mesure, pas un arbitrage entre options équivalentes.
- **Sujet pressenti** : aucun

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- Aucun. Le champ `tempFrozenAlarm` de `/data` existe déjà et ne change pas de forme.

### Champs WebSocket
- Aucun nouveau.

### Topics MQTT
- `temp_sensor_frozen` — **contrat inchangé**, seule la fréquence de passage à
  `ON` change (de quotidienne à quasi nulle). Pas de nouvelle auto-discovery.

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/subsystems/sensors.md` — nouveau seuil, et **surtout** la justification
  physique (pas de 0,0625 °C vs dérive de 0,05 °C/h) ; sans elle le prochain
  lecteur reprendra 30 min en croyant bien faire.

#### Docs transverses
- `CHANGELOG.md` — entrée 2.56.1 : l'avertissement « Sonde température figée » ne
  se déclenche plus à tort.

## Critères d'acceptation
- [x] Une dérive réaliste (0,05 °C/h) ne déclenche **jamais** le détecteur
- [x] Une valeur strictement constante déclenche à 43 200 lectures, **pas avant**
- [x] Le plafond `uint16_t` est documenté à côté de la constante
- [x] L'inertie de `kTempFrozenEpsilonC` est documentée
- [x] Les libellés utilisateur sont en français
- [x] `pio run` compile sans nouveau warning
- [x] `pio test -e native` vert, zéro régression
- [x] Les mutants de D4 sont tués

## Plan de test manuel
1. Déployer, puis relever `/download-logs` après 48 h : **aucun** `[SENSOR_FROZEN]`
   sur la voie température.
2. Vérifier que `tempFrozenAlarm` reste `false` dans `/data` sur la même fenêtre.
3. Contrôler que le badge n'apparaît plus sur le tableau de bord.

## Notes pour agent

⚠️ **Ne pas « améliorer » l'epsilon.** C'est le réflexe naturel devant un
détecteur trop sensible, et c'est faux ici : **élargir la bande rend le
déclenchement PLUS facile** (plus de lectures tiennent dans la bande, le run
s'allonge). Et la rétrécir ne change rien, puisqu'elle est déjà sous un pas. Le
seul levier est la fenêtre.

⚠️ **Ne pas toucher au `FrozenDetector`.** Il est partagé avec pH et ORP, où
`kSensorFrozenSamples = 30` est correct — ces signaux bougent en permanence. Le
défaut est dans l'application du patron à la température, pas dans le patron.

⚠️ Les chiffres de cette spec sont **mesurés le 2026-10-07 sur l'appareil**, pas
estimés : journal système pour les déclenchements, `/get-history?range=24h` pour
la dérive, `src/sensors.cpp` pour la résolution. Les reprendre tels quels dans la
doc plutôt que les recalculer.

## Hors périmètre
- Le `FrozenDetector` lui-même et ses réglages pH / ORP.
- L'arrondi de `lastTempRaw` à 0,1 °C (`sensors.cpp`) et la perte de résolution
  qui en découle pour l'historique et les courbes — **sujet réel, mais distinct**,
  à traiter dans une spec propre si l'utilisateur le souhaite.
- Le champ WS `temperature_raw`, arrondi à 2 décimales alors que sa source est
  déjà quantifiée à 0,1 — même sujet que ci-dessus.

---

## Notes d'implémentation (2026-10-07, v2.56.1)

### Ce que le cycle a révélé et que la spec initiale ignorait

1. **« warning-only » était faux.** La spec a été rédigée en recopiant le commentaire de
   `sensors.cpp`. Le gate `pool-chemistry` a relevé — et vérification directe du code l'a
   confirmé — que `isTemperatureFrozen()` **gate le planning de filtration** :
   `filtration.cpp:162` bloque tout recalcul, `filtration.cpp:405` rejette les échantillons.
   Le faux positif quotidien **dégradait donc réellement le planning** depuis bug-126. Le
   correctif supprime une dégradation, il ne fait pas que retirer un badge. Commentaires
   firmware et doc corrigés en conséquence (A8, C5).

2. **Le `static_assert` demandé était inopérant.** Le gate exigeait
   `static_assert(kTempFrozenSamples < 65535)`. L'architect a montré que `kTempFrozenSamples`
   étant un `uint16_t`, écrire `86400` le replie **modulo 2^16 en 20864** (11 h 35) *avant*
   d'atteindre l'assert, qui verrait `20864 < 65535` et passerait — un retour pire que le bug
   corrigé. La garde réelle est une **borne BASSE sur la durée dérivée**
   (`kTempFrozenWindowSec >= 20 h`). La borne haute a été conservée : la revue a vérifié
   qu'elle attrape bien la bande 64 801-65 535 (65 000 ⇒ compilation refusée), mais elle est
   **aveugle au-delà**, où le repli la court-circuite.

3. **Le texte proposé pour A5 était faux.** L'architect, briefé avant que l'effet sur la
   filtration soit établi, proposait de terminer le message par « Aucune régulation n'est
   suspendue. » Corrigé en « Le dosage n'est pas affecté ; le planning de filtration n'est
   plus recalculé. » — sans quoi ce lot aurait planté dans le firmware exactement le type
   d'affirmation fausse qu'il corrige.

4. **Effet de bord heureux des mutations.** Remettre `kTempFrozenSamples = 900` ne fait pas
   échouer un test : **ça ne compile plus**, la borne basse l'attrape. Le réglage fautif
   d'origine est désormais impossible à réintroduire en silence.

### Résultats mesurés
- `pio run -e esp32dev_core3` : SUCCESS, zéro nouveau warning (RAM 29,5 %, Flash 50,1 %)
- `pio test -e native` : **2 043 cas / 2 043**, **57 suites** (56 avant + `test_native_temp_frozen`)
- Message `[SENSOR_FROZEN]` : 239 o UTF-8, sous `kLogMessageMaxBytes = 768`
- `src/sensor_filter.{h,cpp}` : **strictement inchangés** (`git diff --stat` vide)
- Revue de code : **Approved**

### Reste à faire
- **Déploiement** : non fait. L'appareil tourne en 2.55.3 ; 2.56.0 et 2.56.1 sont en attente.
- **Validation terrain** : relever `/download-logs` 48 h après déploiement — aucun
  `[SENSOR_FROZEN]` sur la voie température ne doit apparaître.
