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

## ⚠️ PÉRIMÈTRE — cette spec couvre 100 % du problème (mis à jour le 16/09)

Rédigée le 15/09 au matin, elle supposait ne traiter qu'un tiers de la perte de
contiguïté. **A1 a établi qu'il n'y a qu'un seul mécanisme**, et que cette spec le
couvre entièrement.

`feature-130`, ouverte le 15/09 au soir sur l'hypothèse que l'effondrement initial
venait de la sérialisation ArduinoJson de l'historique, est **fusionnée ici et
supprimée** : les blocs qui bordent le trou après l'effondrement sont les deux
`String` de la **ligne de journal** que la sauvegarde émet, pas les morceaux du
`JsonDocument`.

| | | |
|---|---:|---|
| Effondrement initial | **65 %** | ligne de journal de la sauvegarde d'historique |
| Grignotage progressif | 35 % | ligne de journal de l'avertissement de famine |

Les deux sont des `String` de `std::vector<LogEntry>`. Voir la note d'incident,
section « A1 : IL N'Y A QU'UN SEUL MÉCANISME ».

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

## Les options, et ce qui est connu de chacune

Aucune n'est retenue par cette spec. `architect` conçoit, **`pool-chemistry`
tranche** ce qui touche aux gardes de dosage.

### ⭐ A+B — Pool statique réservé au démarrage, EN PSRAM  *(la plus complète)*

Réserver les 200 entrées une fois pour toutes au démarrage, dans la PSRAM.

✅ **Aucune `String` de journal ne se pose jamais dans le tas interne** — le
problème disparaît à la racine, pas seulement pour le `logger` mais pour tout ce
qui aurait été fragmenté par lui.
✅ **Aucune allocation dynamique du tout** : rien qui puisse fragmenter, ni
maintenant ni au prochain ajout de message.
✅ **Zéro octet de RAM interne consommé** — contrairement à B seul, qui figerait
20 à 30 Ko sur la ressource exacte dont on diagnostique la pénurie.

⚠️ **LE DANGER, et il vise le `logger` précisément.** `logger.cpp:10` le dit :
« `flushToDisk()` s'exécute SUR LA TÂCHE QUI LOGUE », et il écrit `/system.log`
**en flash**. Or pendant une écriture flash le cache est coupé, et les accès PSRAM
passent par ce cache. Lire les `String` depuis la PSRAM *pendant* cette fenêtre
fauterait. C'est la même famille de danger que celle qui a fixé le plancher de
512 o (`constants.h:864`) : les caches LittleFS et le pool FILE côtoient les
écritures flash, et « l'exposition passerait de QUANTIFIÉE à BINAIRE ».
**Parade à concevoir, pas à supposer** : recopier dans un tampon DRAM avant
d'appeler l'écriture.

⚠️ **Deux réserves secondaires** : la PSRAM est 2 à 4 fois plus lente et le
`logger` est sur des chemins chauds ; et ce projet a une cicatrice — « le
parachute anti-OOM était en PSRAM depuis feature-066, donc décoratif ».

⚠️ **Contrainte de conception** : un pool de taille fixe impose de borner la
longueur des messages, ce que `pool-chemistry` a déjà refusé une fois (voir C).
Donc entrées de taille fixe généreuse, ou pool à allocation interne propre.

ℹ️ **L'objection C2 de `pool-chemistry` ne s'applique PAS ici.** Elle interdisait
`EXT_RAM_BSS_ATTR` pour les tampons du **recensement**, dont le callback s'exécute
interruptions masquées. Le `logger` ne tourne jamais dans cette fenêtre-là.

### A seul — Déporter les `String` en PSRAM sans pool
Le seuil `kPsramExtmemThresholdBytes` vaut déjà **1 024 o** ; une `String` de
144 o reste donc en interne. Une bascule **ciblée** sur le seul `logger`
(allocateur dédié) échapperait au plancher de 512 o.
⚠️ Même danger de flush que A+B, sans la garantie « aucune allocation dynamique ».

### B seul — Pool statique en RAM interne
✅ Garantit la contiguïté.
⚠️ Fige 20 à 30 Ko de `.bss` **interne**, prélevés sur la ressource rare. Dominé
par A+B sur tous les plans sauf le danger de flush.

### C — Borner la taille des `String` de journal
⛔ `pool-chemistry` a déjà refusé une troncature en 2.25.0 : les messages
d'escalade sortaient coupés, donc en JSON invalide, et Home Assistant rejetait
l'alerte entière. Utilisable seulement comme **contrainte d'un pool**, jamais
comme correctif isolé.

### D — Ne pas journaliser la famine pendant la famine
⛔ Supprime le signal qui a permis ce diagnostic, et `constants.h:336-341` énonce
l'inverse comme doctrine : « un refus hors PANIC prouve que le lot C est dû —
C'EST LE SIGNAL VOULU, pas un défaut à corriger ».
⛔ **Et ça ne suffirait pas** : A1 montre que l'effondrement de 65 % vient de la
ligne de journal de la **sauvegarde d'historique**, pas de l'avertissement de
famine. Supprimer le second laisserait le premier intact.

## Cas limites

- **L'effondrement initial est EXPLIQUÉ depuis le 16/09** (A1) : il survient à la
  première sauvegarde d'historique, et les blocs qui bordent le trou sont les deux
  `String` de la ligne de journal qu'elle émet — 168 octets qui détruisent 23 552
  octets de contiguïté. Il n'est plus « d'une autre échelle » : c'est le même
  mécanisme, appliqué à un trou plus grand.
  ⚠️ **Il tombe à un instant VARIABLE** — 108 s à 528 s selon les démarrages —
  parce que la sauvegarde est périodique (5 min) et que le démarrage tombe à une
  position quelconque du cycle.
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

- [ ] **Sur 4 redémarrages, le plus gros bloc ne chute plus de moitié** à la
      première sauvegarde d'historique — c'est le test le plus rapide et le plus
      discriminant, il donne un verdict en 10 minutes au lieu de 30 heures
- [ ] Sur une fenêtre ≥ 30 h, le plus gros bloc contigu ne descend plus sous le
      seuil `kStaticFileMinAllocBytes` (8 192 o) du fait des `String` de journal
- [ ] Aucune sauvegarde d'historique sautée pour cause de famine sur cette fenêtre
- [ ] Le journal reste complet : aucun message de garde chimique tronqué ou perdu
- [ ] La récursion `systemLogger → broadcastLog → _heapAllowsPush` n'est pas rouverte
- [ ] Le coût RAM est mesuré et documenté

## Méthode de mesure — reproduire A1

Le mécanisme est **reproductible à volonté** : il suffit de redémarrer. Quatre
démarrages successifs ont donné le même résultat (29 684 / 27 636 / 28 660 /
31 732), ce qui en fait un banc d'essai fiable pour valider un correctif.

Protocole établi le 16/09, après deux tentatives ratées :

1. **Sonder `/get-system-info?hf=1` toutes les 8 s** — lecture d'atomiques, aucun
   parcours de tas. C'est ce qui détecte la chute.
2. **Recenser toutes les 60 s au plus**, jamais plus vite : à 17 s de cadence
   l'instrument devient la cause (effondrement à 38 s au lieu de 300 s).
3. **Dès la chute détectée**, recenser immédiatement avec une fenêtre **étroite
   ET POSITIONNÉE** sur le trou — les 24 blocs rendus sont les plus bas en
   adresse, viser l'enveloppe entière ne montre jamais la zone d'intérêt.
4. **Lire le contenu** des blocs qui bordent le nouveau trou : ils se lisent en
   clair.

⚠️ Exige un **build de diagnostic** (`POOL_HEAP_PEEK`), voir ADR-0081.

## Plan de test manuel

1. Fenêtre d'observation ≥ 30 h avec les instruments de `feature-128`, même
   protocole (recensement toutes les 20 min sur la bande 18-30 h).
2. `tools/compare_census.py` sur deux recensements encadrant chaque marche
   éventuelle : **aucun bloc responsable ne doit être une `String` de journal**.
3. Provoquer une famine et vérifier que l'avertissement est toujours émis.
4. Vérifier qu'un message de garde chimique long arrive **entier** en MQTT.

## Pistes issues de l'exploitation réelle de l'instrument (15/09)

Quatre limites rencontrées **en se servant** de `feature-128`, à traiter avec
cette spec ou par une spec d'outillage dédiée :

1. **Recensement incrémental.** Le parcours est passé de 524 µs sur tas frais à
   **2 545 µs** en fin de fenêtre, et la garde `kHeapCensusMaxDurationUs` (1 500)
   l'a **auto-désactivé définitivement** à 13:01 — c'est-à-dire que l'instrument
   devient inutilisable **précisément quand le phénomène s'aggrave**. La sortie
   est de parcourir par tranches d'adresses, plusieurs passages courts, pour que
   le coût par passage reste borné quelle que soit la fragmentation.
2. **Réarmement explicite**, sur requête authentifiée, plutôt qu'un redémarrage —
   qui détruit l'état qu'on veut observer. Publier aussi le nombre de
   désactivations.
3. **Dimensionnement des tampons par constante.** Les 1 456 o de `.bss` sont
   prélevés sur la ressource dont on diagnostique la pénurie. 24 trous et 96
   blocs occupés est un choix, pas une fatalité : la fenêtre ciblée n'en utilise
   qu'une dizaine. ⚠️ `pool-chemistry` (C2) interdit de les déporter en PSRAM —
   écrire en PSRAM sous interruptions masquées gonflerait la durée du parcours.
4. **`region_count` n'est pas sérialisé.** Le champ existe dans la structure
   depuis le correctif B3 mais la route ne le publie pas : l'opérant ne sait pas
   s'il regarde une union de régions trouée. Correctif d'une ligne.

⚠️ **Et une limite du firmware livré, découverte à l'usage** : la route expose
`offset`/`count`, l'instantané retient 96 blocs, mais **le premier GET consomme
l'instantané** — `offset=24` rend `not_ready`. Les blocs 24 à 95 sont
**inatteignables**. Contourné en resserrant la fenêtre ; à corriger en rendant la
lecture idempotente, ou en ne libérant l'instantané qu'à la dernière page.

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
