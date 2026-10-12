## Métadonnées
- **ID** : feature-158
- **Slug** : duree-de-filtration-survit-au-redemarrage
- **Type** : bug
- **Statut** : **livré et clôturé** — gate GO sous 13 conditions (12 appliquées d'emblée, la 13e après revue), revue levée, doc et ADR-0104 livrés. ⚠️ Recette sur cible restant à passer (voir Notes de clôture)
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

## ⚠️ CORRECTION DE CETTE SPEC — la lacune trouvée par `spec-reader` (2026-10-12)

**Ce que la spec affirmait** : « le remède existe déjà dans cet appareil — raccordement, pas
invention ». **C'est à moitié faux, et la moitié fausse est celle qui coûte du travail.**

Vérifié dans `src/measure_validity_logic.h:219-223`, le verdict de continuité porte :

```cpp
struct OutageContinuityVerdict {
  bool      waive;     // l'arrêt d'eau au boot peut être traité COURT
  uint32_t  outageS;   // durée de la COUPURE
  OutageDeny deny;
};
```

⇒ Il dit **« cet arrêt a duré 9 s et il est dispensé »**. Il ne dit **PAS** « la filtration
tournait depuis 5 heures ». **Cette seconde donnée n'existe nulle part et ne survit à aucun
redémarrage** : `state.startedAtMs` est de la RAM pure (`src/filtration.h`), contrairement à
`measure_validity` qui porte son état en `RTC_NOINIT_ATTR` scellé.

### Ce que cela change pour le lot

- **La DÉCISION reste unique** — l'interdit de la spec tient entièrement : la filtration
  **consomme** le verdict de dispense, elle n'en calcule pas un second. ⛔ Inchangé.
- **Mais une DONNÉE nouvelle doit être rendue persistante** : « depuis quand la filtration
  tournait-elle ». Deux voies, à trancher par `architect` :
  - **(a)** une mémoire `RTC_NOINIT_ATTR` propre à la filtration, sur le **même patron** que
    `OutageRtcRecord` (magie + scellement FNV-1a, *fail-closed* si absente ou corrompue) ;
  - **(b)** étendre `OutageRtcRecord` lui-même.
  ⚠️ Dans les deux cas, **ajouter une donnée n'est pas ajouter un verdict** — c'est la frontière
  à ne pas franchir, et elle doit être explicite dans le code.
- ⚠️ *Fail-closed* obligatoire : mémoire absente, corrompue, ou non intacte ⇒ on retombe sur le
  comportement actuel (garde pleine). **Jamais** l'inverse : on ne lève pas une garde de dosage
  sur une mémoire dont on doute.

### Un point que l'analyse d'impact a confirmé, et qui allège le lot

`web_routes_data.cpp:400` et `ws_manager.cpp:987` portent l'expression **strictement
identique**, lisant le **même getter**. Corriger la source corrige les deux, et la parité est
**garantie par construction**. ⚠️ Le critère A4 reste à vérifier **formellement** après coup —
c'est précisément une divergence d'EXPRESSIONS sous des clés identiques qui a échappé au grep le
2026-10-11.

### Leçon, pour les specs suivantes

Écrire « le mécanisme existe déjà » sans vérifier **ce que le mécanisme porte réellement** est le
même travers que celui déjà consigné dans ce projet : décrire la PORTÉE de ce qu'on a regardé,
pas le jugement qu'on en tire. Le verdict de dispense avait été lu ; sa **structure de données**
ne l'avait pas été.

## ✅ VERDICT `pool-chemistry` — GO SOUS 13 CONDITIONS (2026-10-12)

Le plan est validé : persister en RAM RTC scellée la **durée mesurée** du cycle, et la recréditer
au boot **si et seulement si** le verdict de continuité de `measure_validity` accorde la dispense.
L'assouplissement est **justifié chimiquement** — `constants.h:1544` fonde les 10 min sur **deux**
composantes, et le gate les a séparées :

- **composante hydraulique** (régime de la pompe, courants de fuite) : **survit** à une coupure de
  9 s — c'est exactement ce qu'affirme déjà `kWaterStopGraceS` avec sa justification physique
  (71 s de renouvellement au droit des sondes, ΔpH ≈ 0 sur 180 s) ;
- **composante instrumentale** (fenêtre de médiane 9 × 5 s) : **ne survit PAS** au redémarrage —
  mais elle est **déjà couverte** par le refus n°1 de `evaluatePhTune` (`isnan(phValueRaw)`), qui
  reste fermé tant que la fenêtre n'est pas remplie.

⇒ Le gate n'impose **pas** de plafond « 180 s d'uptime d'abord » : il coûterait le bénéfice
demandé sans rien protéger. ⚠️ **Mais cette conclusion repose ENTIÈREMENT sur l'ordre des refus**
— d'où la condition 8, qui est la plus fragile du lot.

### Les 13 conditions

1. **La coupure n'est JAMAIS créditée.** `carryMs` ne porte que des secondes réellement mesurées,
   pompe commandée ON et appareil vivant. ⛔ Ne jamais y ajouter `outageS`. Commentaire **imposé
   mot pour mot** dans le code : *« La coupure n'est JAMAIS créditée : ce compteur ne porte que des
   secondes RÉELLEMENT mesurées, pompe commandée ON et appareil vivant. Sous-estimer est sans
   danger, sur-estimer lève une garde de dosage. »*
2. **Preuve PROPRE à la filtration.** L'enregistrement porte `filtrationRunning` et
   `installMode == ManagedFiltration`. ⛔ **Ne pas déduire** « la filtration tournait » du
   `waterPresent` du verdict : en modes externes il est présumé et ne dit rien du relais.
3. **Consommation UNE SEULE FOIS**, dans `begin()`, et seulement si `initialState == true`. Sinon
   le record est invalidé et `carry = 0`. ⛔ Aucun démarrage ultérieur du même uptime ne peut
   consommer le report. **C'est cette condition qui exclut le Boost, par construction.**
4. **Ordres d'écriture imposés** (lecture déchirée en `async_tcp`) — un lecteur qui voit
   `running == true` doit **toujours** lire un couple qui sous-estime. Démarrage : `carryMs = 0`
   → `startedAtMs` → `running = true`. Arrêt (**les DEUX chemins**, `:401` et `:296`) :
   `running = false` → `startedAtMs = 0; carryMs = 0` → invalidation du record.
5. **Borne de vraisemblance : retour à ZÉRO, jamais écrêtage.** ⛔ Écrêter à 24 h laisserait passer
   la garde avec une valeur dont on vient de dire qu'on n'y croit pas. La borne est un **détecteur
   de doute**, pas un plafond de confort.
6. **Aucun seuil de continuité propre à la filtration.** Seules constantes neuves tolérées : le
   *magic* du record et la borne de 24 h.
7. **Accesseur unique, et les TROIS sites l'utilisent** — `web_routes_config.cpp:1361` cesse de
   recalculer. *« Tant que la garde garde sa propre expression, la parité est garantie pour deux
   sites sur trois — et c'est le troisième qui décide du dosage. »*
8. ⛔ **NE PAS RÉORDONNER les refus de `evaluatePhTune`.** Le refus n°1 (`isnan`) doit rester
   **strictement avant** le refus n°2 (filtration) : il est désormais la **SEULE** chose qui
   protège les ~45 premières secondes après un boot. Commentaire **imposé** dans `ph_tune_logic.h`
   signalant que cet ordre est *load-bearing* depuis feature-158.
9. **Journal : les deux nombres, à chaque fois.** Report ⇒ *« Reprise après redémarrage : N s de
   filtration recréditées (coupure M s, dispensée) — garde pH de 10 min NON réarmée »*. Refus ⇒
   *« Reprise à zéro (<motif>) — la garde pH de 10 min s'applique »*. **Pas de ligne muette.**
10. **Logique pure + mutation** : 5 mutants doivent mourir (voir ci-dessous).
11. ⚖️ **Modes « Marche » et Boost : la reprise S'APPLIQUE** — le gate tranche **contre cette
    spec**, avec l'architecte. *« L'enregistrement porte une durée MESURÉE de circulation, pas une
    inférence administrative. La physique ne lit pas le libellé du mode. »* Conserver la
    distinction donnerait **deux verdicts pour une même situation physique** selon une étiquette —
    la classe de défaut déjà corrigée par feature-146.
12. **Modes Powered/External : bug séparé, pas d'urgence** — le sens de l'erreur est **fail-closed**
    (la correction est refusée, jamais accordée à tort). ⚠️ Mais le message **ment** (« dans
    10 min » pour une attente infinie), et une telle installation ne peut **jamais** aligner sa
    mesure. ⛔ Ne rien y toucher ici.
13. **L'ordre d'initialisation ne doit pas devenir une dépendance silencieuse** : drapeau explicite
    « verdict produit », et si `filtration.begin()` consomme avant ⇒ `carry = 0` + ligne
    `critical`. *« Demain quelqu'un réordonnera `setup()` pour une raison sans rapport, et la garde
    doit se refermer toute seule, bruyamment. »*

### ⚠️ Risque résiduel INSTRUIT ET ASSUMÉ (ne pas « corriger » sans repasser au gate)

Une **boucle de redémarrage** fragmenterait la marche réelle tout en accumulant le report, chaque
intervalle restant sous la grâce. Le gate **refuse** d'ajouter un plafond du nombre de reports
consécutifs : il bloquerait le cas **légitime** (plusieurs OTA dans un même cycle de filtration) —
*« le travers exact du quota “une dispense par heure” retiré en feature-139 »*. Chaque seconde
créditée reste réellement mesurée. **La contrepartie est la condition 9** : une succession de
« Reprise après redémarrage » au journal doit se lire comme un signal.

### La frontière, formulée pour être opposable en revue

> `measure_validity` est le **producteur unique** du verdict de continuité de l'eau. `filtration`
> en est un **consommateur**. Elle persiste une **donnée** (des secondes mesurées) ; elle ne
> persiste aucun **verdict** et n'en recalcule aucun. Aucune comparaison de durée de coupure ne
> doit apparaître dans `filtration.*`.

**Test de non-régression de cette frontière** : `grep` de `kWaterStopGraceS`, `outageS` et `graceS`
dans `src/filtration.*` ⇒ **zéro occurrence**, hors commentaire explicatif.

### Vecteurs et mutations imposés

Frontières : `carry = 599 s` ⇒ report accordé **mais garde REFUSE** (prouve que le report n'est pas
un blanc-seing) · `600 s` ⇒ franchie · coupure secteur ⇒ 0 · record corrompu ⇒ 0 · `86 401 s` ⇒ **0,
pas 86 400** · `filtrationRunning = false` ⇒ 0 · mode non managed ⇒ 0 · `initialState = false` puis
démarrage 2 min plus tard ⇒ **120 s, pas 120 + carry** · coupure **180 s pile** ⇒ refusée · wrap de
`millis()`.

Mutants à tuer : **M1** créditer `outageS` en plus · **M2** écrêter au lieu de remettre à zéro ·
**M3** supprimer le test `filtrationRunning` · **M4** retirer le « une seule fois » · **M5**
frontière `>=` → `>`.

### Recette sur cible — deux tests ajoutés par le gate

- **Le plus parlant** : filtration en marche depuis **3 min** → OTA → la correction doit **encore
  être refusée**, reste annoncé ≈ **7 min**. C'est la preuve que le report est un **COMPTEUR**, pas
  une dispense.
- Démarrage à froid : vérifier au journal la ligne « Reprise à zéro », et qu'aucun report n'est
  tracé.
- ⚠️ **Nuance sur « utilisable immédiatement »** : pendant les ~45 premières secondes après le
  boot, le refus n°1 s'applique (message « mesure assez récente », **pas** celui des 10 minutes).
  C'est correct et non trompeur, mais le plan de test doit le prévoir pour ne pas être lu comme un
  échec.

## Notes de clôture (2026-10-12)

### ⚠️ Le défaut bloquant trouvé en revue : le TROISIÈME site, celui que la condition 4 n'avait pas nommé

La condition 4 nommait « les DEUX chemins d'arrêt » et le démarrage de `update()`. Elle ne
nommait pas `begin()`. **C'est donc celui-là qui a divergé** : il annonçait `running = true`
**avant** de poser le couple `(carryMs, startedAtMs)`. Un lecteur dans cette fenêtre aurait vu
« ça tourne » avec une ancre à zéro ⇒ `millis() - 0`, c'est-à-dire **l'uptime** — une
**SUR-ESTIMATION**, la seule direction que le gate interdit.

⚠️ **Ce défaut EXACT avait déjà été relevé en revue sur le site voisin, en feature-155**, et son
commentaire explicatif est toujours dans le fichier. Il a été corrigé là-bas, puis **reproduit
ici**. La fenêtre était inatteignable (aucune tâche ni serveur web n'existe à cet instant de
`setup()` — vérifié), mais s'en remettre à l'ordre de `setup()` est précisément ce que la
condition 13 refuse de laisser silencieux. Les **trois** sites suivent désormais le même ordre,
avec l'historique écrit à côté du code.

**Leçon transposable** : une condition de revue qui ÉNUMÈRE des sites protège les sites
énumérés. Celui qu'on oublie de nommer est celui qui cassera. Formuler la règle (« tout
démarrage pose le couple avant d'annoncer ») plutôt que la liste.

### ⚠️ J'ai corrigé un commentaire trompeur… par un autre commentaire trompeur

En vérifiant par mutation, j'ai constaté qu'élargir la soustraction en `int64_t` **ne casse aucun
vecteur** (mutant équivalent : la troncature vers `uint32_t` au retour ramène le même résultat
modulo 2³²). J'en ai conclu que la sûreté tenait au **type de retour**, et je l'ai écrit.

**La revue m'a repris, à juste titre** : les vecteurs ne prouvent pas cela non plus, parce que
`TEST_ASSERT_EQUAL_UINT32` **tronque ses deux opérandes** avant de comparer — un retour élargi
passerait les quatre vecteurs sans broncher. Ce qui protège réellement est la conversion du site
d'appel (`filtration.h`, 32 bits sur ESP32). Le commentaire dit maintenant que c'est une
protection **par LECTURE, pas par test**, et qu'il ne faut pas se fier à la suite verte pour
autoriser un élargissement.

### Ce qui N'EST PAS prouvé par test — écrit ici plutôt que coché

1. **La sentinelle `startedAtMs == 0 → 1`** (deux sites dans `filtration.cpp`, fichier hors
   `env:native`). Risque analysé : `millis()` ne rend 0 qu'à la première milliseconde du boot ou
   au repliement exact à 49,7 j. Et ce n'est **pas** la garde principale — `filtrationElapsedMs()`
   filtre sur `state.running`, pas sur l'ancre. C'est une ceinture, pas le harnais. Fermable par
   extraction d'un helper pur si on y revient.
2. **La seconde moitié du vecteur M4** (« `initialState = false` puis démarrage 2 min plus tard
   ⇒ 120 s, pas 120 + carry ») : la première moitié est tuée par le test, la seconde vit dans
   `filtration.cpp` et a été vérifiée **par lecture**.
3. **Le mutant `int64_t`** est **équivalent**, pas un trou — voir ci-dessus.

### Angles morts assumés, pour qu'on ne les redécouvre pas comme des bugs

- **Boost actif hors du créneau de base au moment d'un redémarrage** : `decideFiltrationRun()`
  rend `initialState == false` au boot (le Boost n'est pas évalué par `begin()`, comportement
  **préexistant**) ⇒ le record est invalidé et le report perdu. **Fail-closed, sans danger**,
  mais en tension apparente avec la condition 11 (« Marche et Boost : la reprise s'applique »).
- **Troisième constante neuve** : `kFiltrationRtcRefreshMs`, en plus du *magic* et de la borne de
  24 h. Jugée conforme à l'intention du gate (« aucun **seuil de continuité** propre à la
  filtration ») : c'est une **cadence d'écriture**, décalquée de `kOutageRtcRefreshMs`. Tracé ici
  pour que la lecture du gate reste auditable.
- **Boucle de redémarrage** : risque instruit et **refusé** par le gate (un plafond bloquerait le
  cas légitime de plusieurs OTA dans un même cycle). La seule garde est la condition 9 — une
  succession de « Reprise après redémarrage » au journal **doit se lire comme un signal**.

### ⚠️ RECETTE — le premier redémarrage ne bénéficie PAS du correctif, et c'est NORMAL

Le plan de test manuel d'origine était **faux**. Au boot qui livre le correctif, la zone RAM RTC
est neuve ⇒ magic/CRC invalides ⇒ **fail-closed**, durée repartie de zéro, journal :
`[Filtration] Reprise à zéro (mémoire de filtration absente ou corrompue) — la garde pH de 10 min
s'applique`. **C'est la preuve que la sécurité fonctionne, pas un échec.** La vérification se fait
au **DEUXIÈME** redémarrage. Coût : un redémarrage de plus, une seule fois.

**Le test le plus parlant, ajouté par le gate** : filtration en marche depuis **3 min** → OTA →
la correction doit **ENCORE être refusée**, reste annoncé ≈ **7 min**. C'est ce qui prouve que le
report est un **COMPTEUR**, pas une dispense.

⚠️ **Nuance** : pendant les ~45 premières secondes après un boot, c'est le refus n°1 (`isnan`,
fenêtre de médiane vide) qui s'applique — message « mesure assez récente », **pas** celui des
10 minutes. Correct et non trompeur, mais à prévoir pour ne pas le lire comme un échec.

### Validation finale (mesurée, ADR-0073)

`pio run -e esp32dev_core3` SUCCESS, Flash 50,0 %, RAM 28,9 %, zéro warning neuf ·
**69 suites / 2 141 cas**, zéro régression · **5 mutants imposés tués sur 5** · 15 contrôles
d'hygiène verts · `data/` non touché (le client consomme déjà la valeur **servie**, ADR-0100) ·
parité A4 vérifiée **sur les expressions** : trois sites, **une** expression.
