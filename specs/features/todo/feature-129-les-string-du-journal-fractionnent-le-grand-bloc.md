## Métadonnées
- **ID** : feature-129
- **Slug** : les-string-du-journal-fractionnent-le-grand-bloc
- **Type** : bug de conception — la correction touche une structure centrale, elle ne se limite pas à un correctif local
- **Statut** : todo
- **Priorité** : **haute** — le phénomène est actif, mesuré, et coûte 138 suspensions d'ouverture de fichiers et 14 sauvegardes d'historique sur une seule journée
- **Version cible** : à déterminer
- **Dernière mise à jour** : 2026-09-15
- **Spec(s) liée(s)** : `feature-127` et `feature-128` (les instruments qui ont produit la preuve), `feature-125` (réserves du lot C), `bug-080` (lot C), incident [`2026-09-10-recidive-fragmentation-v2500.md`](../../../docs/incidents/2026-09-10-recidive-fragmentation-v2500.md)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [ ] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] Historique / persistance
- [ ] Frontend (`data/`)

⚠️ Le `logger` n'est pas un sous-système périphérique : il porte les **gardes de
dosage**. Un refus d'injection, une bascule pH auto→manuel, une famine mémoire —
tout passe par lui. Trois des quatre options ci-dessous touchent le chemin qui
journalise ces refus.

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry — OBLIGATOIRE** : le `logger` sert les gardes de dosage
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Les entrées du journal (`std::vector<LogEntry>` avec `String message`,
`src/logger.h:22,35`) s'allouent sur le tas interne 8 bits, **au milieu du plus
gros bloc libre**, et le fractionnent. Le plus gros bloc contigu ne remonte alors
jamais, et la garde `fs_guard` finit par refuser toute ouverture de fichier.

## Contexte — la preuve, et comment elle a été obtenue

Trois fenêtres d'observation successives ont été nécessaires. Les deux premières
ne voyaient que des **agrégats** et ne pouvaient pas désigner de coupable ; la
troisième, instrumentée par `feature-127` et `feature-128`, a produit ceci :

| uptime | trou avant | trou après | bloc responsable | taille demandée |
|---|---:|---:|---|---:|
| 18,0 h | 16 752 o | 9 024 o | `0x3FFF964C` | **144 o** |
| 19,4 h | 9 024 o | 7 260 o | `0x3FFF7894` | **144 o** |

Les deux blocs portent **le même message** : l'avertissement de famine mémoire de
`src/history.cpp:542`. Adresses différentes, deux marches distinctes.

**L'amplification est le fait central** : à 18,0 h, **160 octets alloués ont
détruit 7 680 octets de contiguïté** — un trou de 16 752 coupé en 9 024 + 7 568.
Le coût pour le système n'est pas la *taille* de l'allocation, c'est la
*position* où elle tombe.

### La boucle de rétroaction (fortement probable, non démontrée)

    famine → avertissement journalisé → String de ~144 o dans le grand trou
           → trou coupé → famine aggravée → …

L'avertissement est throttlé à **une émission par heure**
(`kHistorySaveWarnThrottleMs`, `src/constants.h:281`), ce qui **colle aux 1,4 h**
qui séparent les deux découpes observées. Le tampon circulaire retient 200
entrées (`kMaxLogEntries`), donc une `String` y survit longtemps avant éviction.

⚠️ **Non démontré** : que la `String` suivante se réinsère dans l'emplacement que
la précédente vient de libérer. C'est l'explication la plus simple du cliquet,
mais les sondages sont espacés de 20 min, pas continus.

## Comportement attendu

Les entrées de journal cessent de fractionner le plus gros bloc contigu du tas
interne 8 bits. **Le journal continue de fonctionner** : c'est lui qui a permis
ce diagnostic, et il porte les gardes de dosage.

## Les quatre options, et leurs contre-indications connues

Aucune n'est retenue par cette spec. `architect` conçoit, **`pool-chemistry`
tranche** ce qui touche aux gardes.

### A — Déporter les `String` de journal en PSRAM
Le seuil `kPsramExtmemThresholdBytes` vaut déjà **1 024 o** ; une `String` de
144 o reste donc en interne.
⛔ **Contre-indication mesurée** : le `static_assert` de `constants.h:864` interdit
de descendre sous **512 o**, parce que les caches LittleFS (512 o) et le pool FILE
de la libc (428 o) basculeraient alors en PSRAM — or ce sont **eux** qui côtoient
les écritures flash, pendant lesquelles le cache flash/PSRAM est coupé.
« L'exposition passerait de QUANTIFIÉE à BINAIRE. »
Une bascule **ciblée** sur le seul `logger` (allocateur dédié) échapperait à cette
objection : à instruire.

### B — Pool statique réservé au démarrage
Réserver les 200 entrées en `.bss`, quand le tas est vierge, et ne plus jamais
allouer.
⛔ Coût en RAM permanente, à chiffrer : 200 entrées × taille moyenne.
✅ Seule option qui **garantit** la contiguïté plutôt que de la rendre probable.

### C — Borner la taille des `String` de journal
Les faire tomber dans une classe de taille qui ne fractionne pas les grands blocs.
⛔ Tronquer un message de garde chimique est inacceptable : `pool-chemistry` a
déjà refusé une troncature en 2.25.0 (les messages d'escalade sortaient coupés,
donc en JSON invalide, et Home Assistant rejetait l'alerte entière).

### D — Ne pas journaliser la famine pendant la famine
Le remède le plus simple au cycle décrit.
⛔ **Il supprime le signal qui a permis ce diagnostic.** Et `constants.h:336-341`
énonce l'inverse comme doctrine : « un refus hors PANIC prouve que le lot C est
dû — C'EST LE SIGNAL VOULU, pas un défaut à corriger. »

## Cas limites

- **L'effondrement initial de −29 696 octets à 1,0 h d'uptime** n'est **pas**
  expliqué par ce mécanisme : autre échelle, probablement autre cause. Corriger le
  `logger` sans le comprendre laisserait une part majeure du problème en place.
  ⚠️ Aucune des trois fenêtres n'a de point avant 6,7 h sur les deux premières,
  ni de recensement bloc par bloc sur la première heure de la troisième.
- Le palier s'installe vers **7-8 Ko** et y reste : atteint à 19,4 h ici contre
  28,2 h sur v2.51.0. Pourquoi cette valeur, et pourquoi elle tient, reste ouvert.
- Trois blocs apparus pendant la marche de 19,4 h ont été libérés avant lecture :
  leur nature restera inconnue.

## Contraintes techniques

- Aucun changement de `sdkconfig`, aucune recompilation des bibliothèques Arduino
  précompilées (pioarduino `55.03.37` épinglé, ADR-0042/0069).
- Le `logger` est appelé depuis **plusieurs tâches** (`loop`, `async_tcp`, MQTT) :
  toute structure neuve doit être sûre en concurrence.
- ⚠️ **Récursion connue** : `systemLogger → broadcastLog → _heapAllowsPush →
  systemLogger` (documentée en tête de `src/heap_metrics.h`). Une allocation ou un
  log dans le chemin d'allocation du journal la rouvrirait.
- Interdiction de journaliser en `ERROR`/`CRITICAL` sur un chemin qui peut
  déclencher un `flushToDisk()` synchrone → `fopen` → l'`abort()` même qu'on
  prévient (`src/history.cpp:536-538`).

## Livrables — check-list

- [ ] A1 — l'option retenue est implémentée, et **le journal reste fonctionnel**
- [ ] A2 — mesure **avant/après** du plus gros bloc contigu sur une fenêtre ≥ 30 h
- [ ] A3 — le coût RAM de l'option retenue est **mesuré**, pas estimé
- [ ] D1 — tests natifs de la logique pure extraite
- [ ] D2 — **validation par mutation** de chaque garde
- [ ] C1 — ADR (la décision verrouille une orientation), `docs/subsystems/logger.md`, `CHANGELOG.md`

## Sécurité chimique

Le `logger` porte les gardes de dosage : refus d'injection, bascule pH
auto→manuel, plancher de sécurité pH, alertes MQTT. **Aucune option ne doit
réduire la fiabilité, la latence ou l'intégrité de ces messages.**
`pool-chemistry` valide le design (P1) **et** le diff (P2).

## ADR attendu ?

**Oui.** Le choix entre les quatre options verrouille une orientation durable et
écarte trois alternatives crédibles. L'ADR doit consigner **pourquoi les trois
autres ont été écartées**, avec leurs contre-indications mesurées.

## Critères d'acceptation

- [ ] Sur une fenêtre ≥ 30 h, le plus gros bloc contigu ne descend plus sous le
      seuil `kStaticFileMinAllocBytes` (8 192 o) du fait des `String` de journal
- [ ] Aucune sauvegarde d'historique sautée pour cause de famine sur cette fenêtre
- [ ] Le journal reste complet : aucun message de garde chimique tronqué ou perdu
- [ ] La récursion `systemLogger → broadcastLog → _heapAllowsPush` n'est pas rouverte
- [ ] Le coût RAM est mesuré et documenté

## Plan de test manuel

1. Fenêtre d'observation ≥ 30 h avec les instruments de `feature-128`, même
   protocole (recensement toutes les 20 min sur la bande 18-30 h).
2. `tools/compare_census.py` sur deux recensements encadrant chaque marche
   éventuelle : **aucun bloc responsable ne doit être une `String` de journal**.
3. Provoquer une famine et vérifier que l'avertissement est toujours émis.
4. Vérifier qu'un message de garde chimique long arrive **entier** en MQTT.

## Notes pour agent

⚠️ **Ne pas choisir l'option avant `pool-chemistry`.** Trois des quatre touchent
le chemin qui journalise les refus d'injection.

⚠️ **Ce mécanisme n'explique pas tout.** L'effondrement de −29 696 octets à 1,0 h
d'uptime est d'une autre échelle et reste non instruit. Une spec de suivi devra
l'attaquer — et elle exigera un recensement **dès la première heure**, ce qu'aucune
fenêtre n'a encore fait.

⚠️ **Le journal est l'instrument de diagnostic de ce projet.** Toute option qui le
dégrade pour gagner de la contiguïté doit être pesée contre la perte de capacité à
diagnostiquer la prochaine panne.
