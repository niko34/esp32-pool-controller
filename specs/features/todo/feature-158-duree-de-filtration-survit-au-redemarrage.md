## Métadonnées
- **ID** : feature-158
- **Slug** : duree-de-filtration-survit-au-redemarrage
- **Type** : bug
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : 2.74.0
- **Dernière mise à jour** : 2026-10-12
- **Spec(s) liée(s)** : feature-155 (garde des 10 min sur la correction pH), feature-139
  (purge adaptative et chemin de dispense unique), feature-091 (retrait du minuteur fixe au
  démarrage de la filtration), feature-157 (lot où le défaut a été constaté en production)

## Domaines touchés
- [x] Capteurs (pH / ORP / température) — la garde protège la justesse d'une mesure
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [x] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`) — seulement si le libellé de refus doit changer

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry** — ⚠️ OBLIGATOIRE : la grandeur corrigée est l'entrée d'une **garde de
      dosage**. Assouplir une garde n'est jamais anodin, même quand elle est manifestement trop
      stricte.
- [ ] ux-ui-designer — *a priori* non : aucune surface nouvelle. **À confirmer** si le texte de
      refus doit être reformulé.
- [x] embedded-developer
- [ ] web-ui-developer — seulement si le frontend doit changer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

La durée de filtration servie par l'appareil (`filtration_elapsed_s`) **repart de zéro à chaque
redémarrage**, alors même que la pompe n'a pas cessé de tourner. Conséquence directe : la
correction manuelle de la mesure pH réclame 10 minutes d'attente après chaque déploiement OTA.

## Contexte

### Constaté en production le 2026-10-12, signalé par l'utilisateur

Après le déploiement de la v2.73.0 : « *J'ai toujours un message qui dit d'attendre 10 min sur
la correction de la mesure.* » Puis l'intuition, exacte : « *Je pense que lors d'un redémarrage
de quelques secondes, les mesures ne démarrent pas au bout de 10 min.* »

### ⚠️ LA PREUVE EST DANS LE JOURNAL DE L'APPAREIL LUI-MÊME

À 7,0 s après le démarrage, le firmware écrit :

```
[STAB] Arrêt d'eau 9 s (reset logiciel, eau présente avant) → purge de reprise DISPENSÉE
```

**L'appareil SAIT que l'eau n'a pas cessé de circuler.** Il l'a établi, tracé, et il en a tiré
la conséquence pour la validité des mesures. **Et la garde du pH l'ignore.**

### Le défaut, en trois lignes de code

`src/filtration.cpp:78-83`, au démarrage :

```cpp
digitalWrite(kFiltrationRelayPin, initialState ? HIGH : LOW);
...
state.startedAtMs = millis();
systemLogger.info("Gestionnaire de filtration initialisé (filtration maintenue active)");
```

⚠️ **Les deux dernières lignes se contredisent.** Le message dit « **maintenue** active » — le
firmware retrouve la filtration dans sa fenêtre et la garde allumée. La ligne au-dessus remet le
chronomètre à **zéro**, comme s'il s'agissait d'un démarrage.

### Mesuré sur l'appareil, le 2026-10-12

| grandeur | valeur |
|---|---|
| `filtration_elapsed_s` | 1 619 s |
| `uptime_seconds` | 1 644 s |
| écart | **25 s** — le délai d'initialisation |

⇒ **La « durée de filtration » du firmware EST l'uptime.** Elle ne mesure pas la filtration,
elle mesure l'appareil. Fenêtre configurée ce jour-là : 08:42 – 17:18 en mode auto, pompe en
marche — durée réelle de circulation ≈ **5 h**. Le compteur se trompait d'un facteur **onze**.

### ⚠️ LE REMÈDE EXISTE DÉJÀ DANS CET APPAREIL — c'est un raccordement, pas une invention

`src/measure_validity.cpp` (feature-139) porte l'horodatage de l'arrêt d'eau **à travers le
reboot**, en **mémoire RTC scellée**, dans le référentiel `millis()` post-boot (soustraction non
signée, wrap-safe). `evaluateOutageContinuity()` décide ensuite, à partir de la **cause du
reset** (`esp_reset_reason()`) et de la grâce `kWaterStopGraceS = 180 s`, si la purge est
dispensée. Le commentaire du code insiste : **un seul chemin de dispense** (arbitrage
`pool-chemistry` de feature-139).

⇒ Le projet a **déjà résolu** « faire survivre la continuité de l'eau à un redémarrage ». La
filtration n'en profite pas. Et feature-091 avait déjà posé le principe : *« un arrêt/redémarrage
fugace est traité par la logique pure »* — la garde du pH n'a jamais reçu ce traitement.

### Pourquoi ce n'est pas qu'un confort

- `filtration_elapsed_s` est servi à **`/data`** (`web_routes_data.cpp:400`) **et** à la trame
  **WebSocket** (`ws_manager.cpp:987`). Tout futur consommateur hériterait de la même erreur.
- `kPhTuneMinFiltrationMs` (600 000 ms) et `kPurgeFloorMs` (600 000 ms) valent **le même
  chiffre**. Deux attentes de 10 min coexistent : l'une sait franchir un reboot, l'autre non.
  Un utilisateur qui voit l'accueil redevenir normal pendant que la correction reste bloquée
  conclut, légitimement, que l'appareil se contredit.

## Comportement attendu

- **Après un redémarrage logiciel court pendant lequel l'eau était présente avant et après**, la
  durée de filtration servie **tient compte du temps écoulé avant le redémarrage**. La garde des
  10 min de la correction pH n'est donc pas réarmée.
- **Après un démarrage à froid** (coupure secteur, mémoire RTC absente), ou après un arrêt d'eau
  **plus long que la grâce**, le comportement actuel est **conservé** : la durée repart de zéro,
  la garde s'applique pleinement.
- Le verdict reste calculé par le **FIRMWARE** et servi (ADR-0100) — le navigateur ne recalcule
  rien. `/data` et la trame WS restent en **parité stricte**, expressions comprises (le grep ne
  suffit plus, cf. la 4ᵉ forme de récidive du 11/10).

### Cas limites — à trancher explicitement
- ⚠️ **Mode « Marche » (forcé) et Boost** : il n'y a pas de fenêtre horaire pour ancrer le
  calcul. Proposition : **conserver le comportement actuel** — un démarrage forcé EST un
  démarrage. À confirmer.
- **Reset provoqué par un plantage** (panique, watchdog) et non par un OTA : la dispense
  doit-elle s'appliquer ? `evaluateOutageContinuity()` tranche déjà cette question pour la
  purge — ⚠️ **réutiliser son verdict, ne pas en écrire un second**.
- **Repliement de `millis()` à 24,8 j** : tout calcul de durée doit être wrap-safe. Ce défaut
  est **déjà survenu dans ce projet** et n'a été trouvé que par mutation.
- **Champ absent** (firmware antérieur) : le client doit **BLOQUER**, jamais lever la garde sur
  une information qu'il n'a pas. Repli déjà en place, à **ne pas casser**.

## Contraintes techniques
- ⛔ **Ne PAS créer un second chemin de dispense.** L'arbitrage `pool-chemistry` de feature-139
  en impose **un seul**. Si la filtration a besoin du verdict de continuité, elle le **consomme**
  — elle ne le recalcule pas.
- ⛔ **Ne PAS élargir `kWaterStopGraceS` ni `kPhTuneMinFiltrationMs`** au passage. Ce lot corrige
  un ANCRAGE, pas un SEUIL.
- ⚠️ **ADR-0102 reste opposable** : l'ordre des gestes se décide par COMPTEUR, jamais par
  l'horloge. Ici on calcule une DURÉE, pas un ordre — mais toute solution qui ferait dépendre la
  garde de l'heure NTP/RTC synchronisée doit être justifiée, et se dégrader proprement quand
  l'heure est inconnue.
- **Logique pure extractible** (ADR-0017) : la décision doit être testable sans matériel, pour
  être **validable par MUTATION** — refuser à la limite basse, accepter juste au-dessus.
- Watchdog, mutex, `kConstantName`, logs via `systemLogger`, français.

## Livrables — check-list
- [ ] A1 — La durée servie tient compte de l'interruption dispensée.
- [ ] A2 — Démarrage à froid et arrêt long : comportement **inchangé**, prouvé par test.
- [ ] A3 — **Un seul chemin de dispense** : le verdict de `measure_validity` est consommé, pas
      dupliqué.
- [ ] A4 — Parité `/data` ↔ WS vérifiée **sur les expressions**, pas seulement sur les clés.
- [ ] A5 — Wrap-safe à 24,8 j, prouvé par vecteur de test.
- [ ] A6 — Le message de refus côté écran reste exact dans tous les cas.
- [ ] B1 — Tests natifs sur la logique pure, **validés par mutation** (ADR-0017).
- [ ] C1 — `docs/subsystems/filtration.md`, `docs/subsystems/measure-validity.md` si touché,
      `CHANGELOG.md`.

## Sécurité chimique
- **Dosage impliqué : OUI.** La grandeur corrigée conditionne une garde qui protège la pose d'une
  correction de mesure, laquelle oriente l'injection d'acide.
- **Sens de l'assouplissement** : la garde devient *moins* stricte dans un cas précis. ⚠️ C'est
  exactement le type de changement qui mérite un examen, même quand il est manifestement juste.
- **Argument en faveur** : la garde existe parce que l'écart corrigé vient des **courants de
  fuite de la pompe de filtration** — il faut un régime établi. Une interruption de 9 secondes
  ne défait pas ce régime. La garde actuelle ne protège donc rien dans ce cas, elle **bloque** —
  ce que le principe « les plafonds protègent, le reste informe » désigne comme un défaut.
- **Questions au gate** : (a) la grâce de 180 s est-elle le bon critère pour la filtration, ou
  faut-il un seuil propre ? (b) faut-il distinguer un reset OTA d'un reset par panique ?
  (c) le comportement en mode forcé/Boost doit-il vraiment rester inchangé ?

## ADR attendu ?
- **Possible.** Si le lot établit que la continuité de l'eau est une notion **partagée** entre
  `measure_validity` et `filtration` — avec un producteur unique et des consommateurs — c'est une
  contrainte qui verrouille les choix futurs, donc un ADR.

## Éléments visibles (pour doc-writer)
- `CHANGELOG.md` — comportement utilisateur changé.
- `docs/subsystems/filtration.md` — sémantique de `filtration_elapsed_s`.
- ⚠️ **Aucun endpoint nouveau**, aucun champ nouveau attendu : c'est la **valeur** d'un champ
  existant qui devient juste.

## Critères d'acceptation
- [ ] Après un déploiement OTA, la correction pH est utilisable **immédiatement** si la
      filtration tournait depuis plus de 10 min avant le redémarrage.
- [ ] Après une coupure secteur, la garde des 10 min s'applique **pleinement**.
- [ ] `filtration_elapsed_s` **cesse d'être égal à l'uptime** quand la dispense s'applique.
- [ ] Le journal reste lisible : on doit pouvoir constater *a posteriori* quelle décision a été
      prise et pourquoi.
- [ ] `pio run`, `pio test -e native` zéro régression (compteurs **mesurés**, ADR-0073),
      `build_fs.sh` dans les 512 Ko, contrôles d'hygiène verts.

## Plan de test manuel
1. Filtration en marche depuis > 10 min → déployer en OTA → la correction pH est utilisable
   **tout de suite**.
2. Couper l'alimentation, rallumer → la garde des 10 min s'applique.
3. Arrêter la filtration > 3 min, la relancer → la garde s'applique.
4. Vérifier le journal dans les trois cas.

## Hors périmètre
- Les **seuils** eux-mêmes (`kPhTuneMinFiltrationMs`, `kWaterStopGraceS`, `kPurgeFloorMs`).
- Les **textes imposés** par `pool-chemistry` sur la carte de correction.
- L'écart **matériel** entre la sonde et la mesure de référence de l'utilisateur — ce n'est pas
  un défaut logiciel (décision du 2026-10-12), aucun bug ouvert pour cela.
