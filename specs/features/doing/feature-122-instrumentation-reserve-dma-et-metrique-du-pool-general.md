## Métadonnées
- **ID** : feature-122
- **Slug** : instrumentation-reserve-dma-et-metrique-du-pool-general
- **Type** : feature — instrumentation **PASSIVE** (lot C-1a), **aucun correctif et aucune expérience active dans ce lot**
- **Statut** : **doing** — instrumentation livrée (code + tests + revue + doc), **relevés sur cible restants** ; corrigé en cohérence avec l'emplacement du fichier (`specs/features/doing/`)
- **Priorité** : **haute** — elle débloque le lot C de `bug-080`, dont l'utilisateur subit les 503
- **Version cible** : 2.49.0
- **Dernière mise à jour** : 2026-09-08 (doc-writer, lot C-1a livré)
- **Spec(s) liée(s)** : `bug-080` lot C (la cause racine qu'elle sert à confirmer), `feature-123` (lot C-1b — l'expérience de réserve, **détachée le 08/09**), ADR-0058 (métrique 8 bits), `bug-121` (réduction de demande, indépendante)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP (exposition des mesures)
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — aucun chemin de dosage
- [ ] ux-ui-designer — aucune surface UI dans ce lot
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Mesurer les deux chiffres qui manquent pour corriger la famine de RAM interne
capable de DMA : la **demande concurrente réelle en pointe**, et la **visibilité
d'une réserve DMA dans les métriques de tas**. Ce lot n'installe **aucune**
réserve et ne corrige **aucune** garde — il produit les nombres qui permettront
de le faire sans se tromper.

## ⚠️ DÉCOUPAGE DU 2026-09-08 — ce lot est la MOITIÉ PASSIVE

Décision de l'utilisateur, prise **avant** le début du cycle : le périmètre
d'origine (34 items) mélangeait une mesure sans risque et une expérience qui
touche le démarrage de façon **irréversible**. Il est scindé.

**Reste ici (C-1a) — risque nul, aucune écriture, aucune allocation retenue** :
les planchers glissants par masque (A1), l'exposition en lecture (A4), la
corrélation aux événements (A5), le plancher PSRAM (A6), et le **témoin
4096/4095** (A7) — un simple `malloc()` immédiatement libéré, qui répond à lui
seul à « le lot B-1 est-il décoratif ? ».

**Part en `feature-123` (C-1b)** : l'expérience de réserve DMA (A2) et le verdict
avant/après qui en dépend (A3).

**Motif du découpage — trois faits établis pendant la préparation du plan** :

1. `esp_psram_extram_reserve_dma_pool` prend le **plus gros bloc contigu
   d'abord** — exactement la grandeur dont dépendent toutes les gardes du
   projet, dont les six gardes de sécurité chimique.
2. **Aucune API ne retire une région** enregistrée : un mauvais dimensionnement
   est **irréversible jusqu'au redémarrage**.
3. **L'OTA est lui-même gardé par des seuils mémoire.** Une réserve mal calibrée
   peut donc fermer la porte par laquelle on viendrait la corriger — et il n'y a
   **aucun repli série** depuis le conteneur.

**Et ce que le découpage ne coûte pas** : la question que seule C-1b pouvait
trancher — la visibilité de la réserve dans les quatre masques — a été
**confirmée depuis la source IDF** pendant la préparation du plan. Le tableau est
`caps[] = {0, MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL, MALLOC_CAP_8BIT|MALLOC_CAP_32BIT}`,
donc `all_caps = 0x080E`, donc `0x0804` **voit** la réserve et `0x1804` ne la voit
pas. La table dérivée ci-dessous est donc corroborée par deux voies
indépendantes. La mesure sur cible reste souhaitable — les bibliothèques sont
précompilées et l'ADR-0058 est né d'une dérivation crue sur parole — mais elle
n'est plus le verrou du chantier.

## Contexte

Cause racine candidate établie le 2026-09-08 (voir `bug-080`, section « LOT C ») :

- Les allocations qui échouent portent `caps=0x080C` = `8BIT|DMA|INTERNAL`. Le
  DMA ne sait pas adresser la PSRAM : ces 1 622 o **ne peuvent structurellement
  jamais** y aller.
- Le `sdkconfig.h` réellement compilé porte
  `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL 0` alors que le défaut de l'IDF est
  **32768**, et que ce réglage est justement lié à `SPIRAM_USE_MALLOC=y`, la
  configuration de ce projet. **Rien n'est réservé** : les allocations ordinaires
  peuvent consommer jusqu'au dernier octet contigu de la mémoire dont le WiFi a
  un besoin exclusif.
- Le levier existe à l'exécution : `esp_psram_extram_reserve_dma_pool(size_t)`,
  exportée, mécanisme confirmé au désassemblage.

### Ce que la dérivation a déjà tranché — à CONFIRMER, non à explorer

En appliquant la règle `(all_caps(heap) & caps) == caps`, que
[`src/oom_guard_logic.h`](../../src/oom_guard_logic.h) documente déjà :

| Masque | DRAM interne | PSRAM | Réserve DMA |
|---|---|---|---|
| **0x0804** `INTERNAL\|8BIT` — métrique **actuelle** des gardes | voit | ignore | **voit** ⚠️ |
| **0x080C** `INTERNAL\|DMA\|8BIT` — ce que le WiFi demande | voit | ignore | voit ✅ |
| **0x1000** `DEFAULT` seul | voit | **voit** ⚠️ | ignore |
| **0x1804** `INTERNAL\|8BIT\|DEFAULT` — **proposée** | voit | ignore | ignore ✅ |

⇒ **Le piège est prévu** : poser une réserve sans toucher à la métrique ferait
afficher aux gardes un gros bloc libre — celui de la réserve — pendant que le
pool général resterait affamé. La garde 503 cesserait de se déclencher et
l'appareil planterait, exactement là où elle protège aujourd'hui. Le projet a
**déjà** vécu ce mode de défaillance deux fois : le parachute anti-OOM logé en
PSRAM, et les quatre gardes rendues mortes par une métrique IRAM-inclusive
(ADR-0058).

⇒ **La métrique correcte après correctif est 0x1804** : elle ignore la PSRAM
(absence d'`INTERNAL`) **et** la réserve (absence de `DEFAULT`), ne gardant que
le pool où `malloc()`/`new` puisent réellement.

**L'HYPOTHÈSE À CONFIRMER, et elle est unique** : les capacités exactes de la
région ré-enregistrée par `heap_caps_add_region_with_caps`. Le tableau supposé
est `{DMA|INTERNAL, 0, 8BIT|32BIT}` → `all_caps = 0x80E`. Le désassemblage
n'a pas permis de le lire de façon fiable : le littéral pointe `.rodata + 0` et
les trois mots donnent `[0x1400, 0, 0x100006]`, soit `DEFAULT|SPIRAM` en tête —
**incohérent** avec un pool DMA interne, donc soit des littéraux fusionnés avec
`esp_psram_extram_add_to_heap_allocator`, soit une erreur de lecture des
déplacements. **Cette question se tranche par la mesure sur cible, pas au
désassemblage.**

## Comportement attendu

Après ce lot, il doit être possible de répondre par des chiffres relevés, sans
redéployer :

1. **Quelle est la demande en pointe** en mémoire `0x080C` lors des événements
   connus pour déclencher la famine — chargement de l'interface, session de
   dosage, rafale de découverte MQTT ?
2. **Une réserve DMA est-elle visible** dans `0x0804`, `0x080C`, `0x1000` et
   `0x1804` ? Autrement dit : la table dérivée ci-dessus est-elle exacte ?
3. **Quelle taille de réserve** est soutenable, sachant que le tas 8 bits ne
   dispose que d'environ **57 Ko au démarrage** et retombe à **26 Ko** en
   exploitation — réserver les 32 Ko du défaut IDF en prendrait plus de la
   moitié.

### Cas limites
- **Réserve refusée** (`ESP_ERR_NO_MEM`) : l'appareil doit démarrer normalement,
  sans réserve, et le dire. Jamais d'`abort()`.
- **Module sans PSRAM** : `psramFound()` faux → l'expérience est sautée en
  silence (binaire unique WROOM/WROVER-E, précédent `main.cpp:129`).
- **Appel trop tardif** : la fonction a besoin de gros blocs contigus. Si elle
  est appelée après `WiFi.begin()` ou après le serveur web, elle peut réussir en
  ne réservant que des miettes — il faut **mesurer et journaliser ce qu'elle a
  réellement obtenu**, pas ce qu'on lui a demandé.
- **Pointe non capturée** : un échantillonnage à cadence fixe peut manquer une
  pointe de quelques millisecondes. Le minimum doit être retenu par un
  **plancher glissant**, jamais par un instantané.

## Comportement actuel

`heapProbeForCaps(uint32_t caps)` ([`src/heap_metrics.h`](../../src/heap_metrics.h))
donne déjà `free` et `largestBlock` en **un seul parcours**, et conserve
explicitement le parcours O(blocs) pour `0x0804` et `0x080C`. **La brique de
mesure existe** : ce lot l'exploite, il ne la réécrit pas.

En revanche, une seule métrique est **rapportée** : `main.cpp:1070` journalise
`libre8`/`maxBloc8` (0x0804). Ni `0x080C`, ni `0x1804`, ni la moindre trace de
réserve n'apparaissent où que ce soit — et `nospc_cnt` non plus (cf.
`feature-120`).

## Contraintes techniques
- ESP32, offline first
- **Chemin de démarrage** : toute expérience de réserve s'exécute avant
  `systemLogger.begin()` → **aucun log possible sur place**, il faut mémoriser
  puis journaliser plus tard (idiome déjà en place à `main.cpp:127-131`)
- `heapProbeForCaps` est **sans allocation et sans verrou** : le rester
- **Interdit d'entête `oom_guard`** : ne jamais journaliser depuis un chemin
  d'échec d'allocation (récursion)
- ADR-0033 : budget de journalisation. Le journal persistant a ~25-35 min de
  rétention — une ligne par échantillon l'effacerait. **Agréger, puis rapporter.**
- ADR-0058 : la métrique de référence est le pool 8 bits, pas
  `ESP.getMaxAllocHeap()` (IRAM-inclusive)
- Ne pas élargir la surface non authentifiée (`bug-113` vient de la réduire de
  80,73 %) : l'exposition passe par une route **authentifiée**

## Fichiers potentiellement concernés
- `src/heap_metrics.h` / `.cpp` — planchers glissants par masque
- `src/main.cpp` — l'expérience de réserve (après `initOomGuard`, à côté de
  `heap_caps_malloc_extmem_enable`), et la ligne de journal agrégée
- `src/web_routes_config.cpp` — exposition dans `/get-system-info`
- `src/constants.h` — taille de réserve à l'essai, cadence d'échantillonnage
- `docs/subsystems/system-health.md`, `docs/API.md`, ADR probable

## Livrables — check-list
- [x] A1 — Planchers glissants (`min` depuis le boot **et** sur fenêtre) pour les
      quatre masques `0x0804`, `0x080C`, `0x1000`, `0x1804` — réutiliser
      `heapProbeForCaps`, ne pas la dupliquer
      → **livré en CINQ emplacements** (les quatre + `MALLOC_CAP_SPIRAM` d'A6),
      et sur **les deux grandeurs** (libre ET plus gros bloc), avec l'octet de
      contexte au creux de chacune.
- ~~A2 — Expérience de réserve au démarrage~~ → **déplacé en `feature-123`** (C-1b)
- ~~A3 — Les quatre masques avant/après l'appel~~ → **déplacé en `feature-123`**,
      il n'a de sens qu'avec A2
- [x] A4 — Exposition des quatre masques et du résultat de réserve dans
      `/get-system-info` (route authentifiée)
- [x] A5 — Corrélation : marquer les échantillons pris pendant un chargement
      d'interface, une session de dosage, une rafale de découverte — sans quoi
      les planchers ne diront pas **quel** événement les a produits
- [x] C1 — `docs/subsystems/system-health.md` : la table des quatre masques et ce
      que chacun voit ; `docs/API.md` : les champs neufs
- [x] C2 — ADR si le choix de la métrique de garde est arrêté dans ce lot
- [x] C3 — `CHANGELOG.md`
- [x] T1 — Tests purs : la table de sélection `(all_caps & mask) == mask` pour les
      quatre masques × trois tas (interne, PSRAM, réserve) — **12 cas**, c'est la
      dérivation elle-même mise sous test
- [x] T2 — Tests : plancher glissant (un minimum n'est jamais oublié, une fenêtre
      se réarme). Les cas « taille obtenue ≠ demandée », « réserve refusée →
      démarrage normal » et « essai non confirmé → saut » partent en `feature-123`
- [x] T3 — **Mutation** : masque `0x1804` remplacé par `0x0804` (le défaut même
      qu'on cherche à éviter) ; plancher remplacé par un instantané ; `== caps`
      remplacé par `!= 0`

## Nouvelles signatures / API
- Endpoints HTTP nouveaux : aucun — enrichissement de `/get-system-info`
- Champs WebSocket nouveaux : **aucun** (des planchers de diagnostic ne sont pas
  du temps réel ; ne pas alourdir la trame — la marge de budget WS est de 43 o)
- Topics MQTT nouveaux : aucun
- Fonctions publiques : planchers par masque dans `src/heap_metrics.h`
- Constantes : taille de réserve à l'essai, cadence d'échantillonnage

## Sécurité chimique
- Dosage impliqué : **non**
- Filtration requise : non
- Calibration touchée : non
- Risque identifié : **aucun chemin de dosage**. Mais attention : l'expérience
  s'exécute au **démarrage**, et un échec bruyant y coûterait le boot. Refus →
  démarrage normal sans réserve, jamais d'`abort()`.

## ADR attendu ?
- **Probable** : oui
- **Critère(s) candidat(s)** : 1, 2, 3
- **Sujet pressenti** : « La métrique d'une garde doit exclure les pools qu'un
  `malloc()` ne peut pas atteindre » — troisième occurrence du même piège dans ce
  dépôt (parachute en PSRAM, métrique IRAM-inclusive, et maintenant la réserve
  DMA). C'est le motif qui mérite d'être figé, plus que le masque `0x1804`
  lui-même.

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `GET /get-system-info` — champs de diagnostic ajoutés

### Champs WebSocket
- Aucun

### Topics MQTT
- Aucun

### Documentation à produire / mettre à jour
- `docs/subsystems/system-health.md`, `docs/API.md`, `CHANGELOG.md`, ADR probable.
  **Pas** de documentation utilisateur : rien de visible à l'écran.

## Notes

Ce lot est délibérément **une mesure et rien d'autre**. La tentation sera forte
d'y glisser la réserve définitive puisque le levier est identifié — il faut y
résister : la table de sélection est **dérivée**, pas observée, et elle repose
sur une hypothèse unique mais non vérifiée (les capacités du tableau passé à
`heap_caps_add_region_with_caps`). Poser la réserve sur une dérivation fausse
aveuglerait la garde 503 et transformerait une coupure d'interface de 50 s en
plantage.

Deux chantiers **ne dépendent d'aucun résultat de ce lot** et peuvent avancer en
parallèle, car ils réduisent la demande au lieu d'augmenter l'offre :
`bug-121` (60 entités republiées à chaque reconnexion MQTT) et la double
ouverture `exists()` + `open()` des fichiers statiques, qui coûte 2 × 4 096 o par
fichier servi et jusqu'à ~49 Ko en rafale au chargement de l'interface.

---

## ⚠️ AJOUT DU 2026-09-08 — le correctif B-1 est-il décoratif ? Troisième livrable

Question posée par l'utilisateur : « pourquoi ne force-t-on pas ces allocations
en PSRAM, quelle que soit la taille ? » Elle a exposé un fait qui n'était pas
mesuré.

### Le constat

`heap_caps_malloc_extmem_enable(kPsramExtmemThresholdBytes)` a été posé en
**lot B-1 (v2.35.0)** précisément pour que les tampons stdio de **4 096 o**
(`vfs_api.cpp:250`) préfèrent la PSRAM — « il s'en fallait d'UN OCTET », le test
de l'allocateur étant `taille <= seuil ⇒ interne`.

**Or `psram_free` ne varie JAMAIS** :

- **152 relevés** `Heap 8bits:` dans les journaux capturés (07 et 08/09, deux
  versions de firmware, trois redémarrages) : `psram=4180340` dans **tous**, sans
  une seule exception.
- Mesure live du 08/09, pendant un transfert **volontairement ralenti** de
  `/app.js` (`--limit-rate 6000`, fichier tenu ouvert ~12 s) : `psram_free`
  immobile à 4180340 sur cinq échantillons à 1 s d'intervalle, tandis que
  `heap8` chutait de **49 820 à 32 232** puis remontait.

⇒ Rien ne s'alloue en PSRAM pendant le service de fichiers. Tout se passe en
RAM interne.

### Pourquoi c'est grave, et pourquoi il ne faut pas conclure trop vite

Ce serait la **troisième** occurrence du même mode de défaillance dans ce dépôt :
le parachute anti-OOM logé en PSRAM donc décoratif (feature-066 → lot B-1), les
quatre gardes rendues mortes par une métrique IRAM-inclusive (ADR-0058), et
maintenant, peut-être, une politique d'allocation sans effet.

**Mais `psram_free` peut être un mauvais témoin** — c'est exactement le genre de
raccourci qui a produit les deux erreurs précédentes. Trois hypothèses restent
ouvertes :

1. Les tampons **ne partent pas** en PSRAM (le correctif B-1 est inopérant).
2. Ils y partent, mais `ESP.getFreePsram()` ne le montre pas.
3. Ils y partent et en reviennent entre deux échantillons — improbable, le
   fichier étant tenu ouvert pendant la mesure, mais non exclu.

### Ce que cela ajoute aux livrables

- [x] A6 — Relever `psram_free` **et** `heapProbeForCaps(MALLOC_CAP_SPIRAM)`
      avec plancher glissant, **corrélés** aux mêmes événements que les masques
      internes (A5). Sans plancher, une allocation transitoire est invisible.
- [x] A7 — **Instrumenter** l'efficacité du lot B-1 : allocation témoin de
      4 096 o au démarrage + **témoin de contrôle de 4 095 o**, classement par
      l'adresse retournée (deux, voire trois avis croisés), verdict publié.
      ⚠️ **L'instrument est livré, le VERDICT ne l'est pas** : il se lira au
      premier relevé après déploiement.
- [x] T4 — Mutation : seuil `extmem` remis à 4096 (le défaut d'origine) → le
      témoin doit changer de camp. Si rien ne change, **le correctif B-1 ne fait
      rien** et le mutant le prouve.

### Ce que cela ne change PAS — la limite est structurelle

Même si le lot B-1 était pleinement opérant, **il ne corrigerait pas la cause
racine**. Les allocations qui échouent portent `caps=0x080C` =
`INTERNAL | DMA | 8BIT` : le moteur DMA de l'ESP32 **ne sait pas adresser la
PSRAM**. Aucun seuil, aucune politique, aucune configuration ne peut les
déplacer. Pousser davantage d'allocations en PSRAM réduit la **concurrence**
pour la RAM interne — c'est utile, et c'est la phase 2 du plan — mais ne peut
jamais déplacer celles qui échouent.

Et forcer **toutes** les tailles en PSRAM se heurte à quatre obstacles, à
consigner pour que la question ne se repose pas :

1. C'est une politique **globale** : rien ne permet de cibler un site d'appel
   depuis l'extérieur du framework.
2. Les petites allocations **chaudes** deviennent nettement plus lentes derrière
   le bus SPI et son cache.
3. Certaines **exigent** la RAM interne pour être correctes — tout ce qu'une
   interruption touche, les descripteurs DMA, les piles de tâches — et la
   préférence n'a aucun moyen de le savoir.
4. C'est une **préférence, pas une contrainte** : sous pression PSRAM,
   l'allocateur retombe silencieusement en interne, donc la garantie n'existe
   pas.

### Conséquence sur la priorité de la double ouverture

Le chiffre de **~49 Ko en rafale** pour la double ouverture
`exists()` + `open()` (`constants.h`) décrit l'état **AVANT** le lot B-1. Il a
été cité trois fois dans cette session comme si le lot B-1 ne l'avait pas
adressé — ce qui était une **erreur de raisonnement**, corrigée ici. Sa validité
actuelle dépend entièrement de A7 :

- si les tampons sont bien en PSRAM → la double ouverture coûte peu de RAM
  interne, et sa correction devient un gain de latence, pas de mémoire ;
- si les tampons sont restés internes → le chiffre tient, et supprimer la double
  ouverture est bien le meilleur rapport bénéfice/risque du dossier.

**Ne pas ouvrir de spec sur la double ouverture avant A7.** La mesure décide de
sa priorité, et elle coûte moins cher que le correctif.

---

## Notes d'implémentation (2026-09-08, v2.49.0 — lot C-1a livré)

### Ce qui a été livré

| Fichier | Nature |
|---|---|
| `src/heap_caps_logic.h` | **neuf** — module PUR (ADR-0017) : masques, `heapMaskSelectsHeap`, `heapFloorUpdate`, `heapAtomicFetchMin`, `heapPtrIsExternal`, `heapMaskRoundRobinNext`, seaux de durée + `heapWalkMedianUpperUs`, `heapCtxDiscoveryWindowActive`, bits `kHeapCtx*` |
| `src/heap_metrics.h` / `.cpp` | planchers multi-masques (5 emplacements), tour de rôle, chronométrage du parcours de référence, témoin A7, `static_assert` d'épinglage des masques sur les constantes IDF et de la table (`0x02ED`) |
| `src/heap_ring.cpp` | composition du contexte + fusion des **deux** appels `0x0804` d'avant en **un** `heap_caps_get_info` |
| `src/main.cpp` | témoin 4096/4095 au `setup()` (avant `systemLogger.begin()`, résultat mémorisé) + **deux** lignes de journal agrégées + réarmement de fenêtre |
| `src/web_routes_config.cpp` | objet `hf` sous `?hf=1`, helpers `putHeapFreeOrNull` / `putHeapBlockOrNull` / `putHeapFloorEntry` / `putHeapWitnessVerdict` |
| `src/web_server.cpp`, `src/fs_guard.cpp` | `heapNoteHttpGetSample` / `heapNoteMaxAlloc8Sample` sur des valeurs **déjà mesurées** (aucun parcours neuf) |
| `src/mqtt_manager.h` / `.cpp` | `lastDiscoveryMarkMs()` + deux `store` relâchés dans `publishDiscovery()` |
| `src/constants.h` | `kHeapFloorMaskCount`, `kHeapFloorWalksPerSample` (+ `static_assert` de plafond), `kMqttDiscoveryCtxWindowMs` |
| `test/test_native_heap_caps_table/`, `test/test_native_heap_floors/` | **neuves** |
| `docs/adr/0077-*.md`, `docs/subsystems/system-health.md`, `docs/API.md`, `docs/subsystems/mqtt-manager.md`, `CHANGELOG.md`, `docs/README.md`, `docs/adr/README.md` | documentation |

### Compteurs MESURÉS (et non recopiés — ADR-0073)

- `ls -d test/test_native_* | wc -l` → **44 suites**
- `pio test -e native`, dernière ligne → **1 677 cas**, **zéro régression**
- `pio run` → **SUCCESS**, avec **exactement les 6 warnings préexistants**
  (aucun neuf)

### Table des mutants tués

| # | Mutation | Tué par | Origine |
|---|---|---|---|
| 1 | `0x1804` → `0x0804` (le défaut même qu'on cherche à éviter) | `test_the_candidate_0x1804_sees_neither_psram_nor_reserve`, `test_expected_table_encodes_to_749` — **et** `static_assert(heapFloorsPredMask() == 0x02ED)` côté firmware | `test-engineer` |
| 2 | `(all_caps & caps) == caps` → `!= 0` | `test_non_empty_intersection_is_not_selection` (intersection non vide `0x0404` construite exprès), `test_selection_table_twelve_cases` | `test-engineer` |
| 3 | plancher glissant → **instantané** (écriture inconditionnelle) | `test_floor_keeps_the_deepest_trough_not_the_last_sample`, `test_floor_equal_sample_does_not_rewrite_the_context` | `test-engineer` |
| 4 | `fetch_min` → `load`-`compare`-`store` | `test_fetch_min_does_not_lose_a_deeper_concurrent_trough`, `test_fetch_min_two_writers_interleaved_over_a_long_series` (**entrelacement scripté déterministe**) | `test-engineer` |
| 5 | seuil du témoin `4095` → `4096` (T4) | `test_the_4096_4095_witness_is_wired_to_the_extmem_threshold` — **tué à la COMPILATION**, cf. « verdict T4 » ci-dessous | `test-engineer` |
| 6 | octets de contexte `uint8_t` → `uint32_t` (la ligne de journal passerait à 167 o → troncature silencieuse du témoin) | `test_journal_context_fields_are_bytes` | **doc/revue** |
| 7 | le même champ déplacé **en commentaire** (leurre) | idem, **après** bascule de la sentinelle sur `readProjectCode` (source dépouillée) ; le dépouilleur est lui-même vérifié par `test_le_depouilleur_desarme_le_leurre` | **doc/revue** |

Les mutants **6 et 7** sont ceux de la revue : la version brute de la sentinelle
(`readProjectFile`) était satisfaite par un simple **commentaire** — **le défaut
exact de `bug-114`, reproduit dans le test censé s'en prémunir**.

### Verdict T4, en clair

La mutation `4095 → 4096` est tuée **à la compilation**, par
`static_assert(seuil + 1 == kVfsStdioBufferBytes)`. **Le témoin n'a donc PAS pu
s'exécuter sous mutation** : on ne peut pas montrer qu'il change de camp.

Vérification de **substitution**, par désassemblage de la `libheap.a` réellement
liée : le test de l'allocateur est `bgeu limit, size` ⇒ « **taille ≤ seuil ⇒
interne** ». La constante est donc bien **porteuse** — le seuil de 4 095 laisse
un `malloc(4096)` **strictement au-dessus**, ce qui est la condition nécessaire à
l'effet visé.

**Conclusion, sans l'embellir : le lot B-1 n'est NI prouvé opérant, NI prouvé
décoratif.** La question se tranchera à la **lecture du témoin après
déploiement** (`hf.w96` / `hf.w95`, ou le champ `tem=` de la première ligne de
journal agrégée).

### Ce qui reste NON prouvé — à reprendre tel quel

- **Le verdict A7 lui-même.** L'instrument est en place, la réponse n'est pas
  encore lue.
- **Toute mesure sur cible** : planchers réels des cinq masques, durées de
  parcours (`walkMaxUs` / `walkMedianUs`), coût réel du 2ᵉ parcours O(blocs).
  Aucun chiffre de ce lot n'a été relevé sur l'appareil.
- **`heapFloorsSample` / `heapFloorsRead` / `heapFloorsRearmWindow` sont
  impures.** Seules leurs **parties pures** (`heapFloorUpdate`,
  `heapAtomicFetchMin`, `heapMaskRoundRobinNext`, `heapWalkMedianUpperUs`,
  `heapCtxDiscoveryWindowActive`) et des **sentinelles de câblage** (aucune garde
  ne lit les nouveaux planchers, la route ne lit que des atomiques rangées, les
  lignes de journal tiennent dans leurs tampons de pile) sont couvertes.
- **`pred` est *constant-folded*** (`movi a8, 0x2ed` dans l'objet) : **aucune
  information d'exécution**. Ce qui la protège est le `static_assert`, c'est-à-dire
  le **build**.
- **Aucun test à vrais fils d'exécution.** La course du `fetch_min` est prouvée
  par **entrelacement scripté déterministe** (atomique instrumentée passée en
  paramètre de patron), pas par des tâches concurrentes.

### Deux corrections de revue déjà appliquées

1. **`putHeapBytesOrNull` scindé en `putHeapFreeOrNull` / `putHeapBlockOrNull`.**
   `kOomProbeBlockUnknown` (4 194 240) est la sentinelle du **plus gros bloc**
   des masques à chemin O(régions) — mais c'est aussi une **valeur de libre
   parfaitement atteignable en PSRAM** (4 Mo − 64 o ; relevé réel du 08/09 :
   4 180 340). L'appliquer aux champs `f` / `mf` / `wf` transformait une **mesure
   juste en `null`** : une collision [ADR-0072](../../../docs/adr/0072-absence-de-valeur-hors-du-domaine.md)
   **à l'envers** — non plus une absence déguisée en valeur, mais une valeur
   déguisée en absence. `heapFloorNoteSlot` faisait déjà la distinction côté
   firmware ; la route la fait désormais aussi.
2. **Sentinelle de types basculée de `readProjectFile` (brut) vers
   `readProjectCode` (dépouillé)**, avec ajout du dépouilleur de commentaires
   **et de son auto-test anti-leurre**. Elle était satisfiable par un
   commentaire — cf. mutants 6 et 7.

### Suite immédiate

1. Déployer, laisser tourner **≥ 30 min**, puis suivre le
   [protocole de relevé](../../../docs/subsystems/system-health.md#protocole-de-relevé-sur-cible).
2. Lire **d'abord** le témoin A7 (il conditionne la priorité de la double
   ouverture `exists()` + `open()`, cf. la section « Conséquence sur la priorité
   de la double ouverture » ci-dessus).
3. Reporter les chiffres dans `bug-080` § LOT C, puis ouvrir `feature-123` avec
   le plancher `0x080C` **et** sa réserve de résolution (4 s, tour de rôle).
