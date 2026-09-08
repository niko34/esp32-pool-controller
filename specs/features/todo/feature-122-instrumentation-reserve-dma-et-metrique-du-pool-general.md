## Métadonnées
- **ID** : feature-122
- **Slug** : instrumentation-reserve-dma-et-metrique-du-pool-general
- **Type** : feature — instrumentation préalable à un correctif, **aucun correctif dans ce lot**
- **Statut** : todo
- **Priorité** : **haute** — elle débloque le lot C de `bug-080`, dont l'utilisateur subit les 503
- **Version cible** : 2.49.0
- **Dernière mise à jour** : 2026-09-08
- **Spec(s) liée(s)** : `bug-080` lot C (la cause racine qu'elle sert à confirmer), ADR-0058 (métrique 8 bits), `bug-121` (réduction de demande, indépendante)

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
- [ ] A1 — Planchers glissants (`min` depuis le boot **et** sur fenêtre) pour les
      quatre masques `0x0804`, `0x080C`, `0x1000`, `0x1804` — réutiliser
      `heapProbeForCaps`, ne pas la dupliquer
- [ ] A2 — Expérience de réserve au démarrage, **taille par constante**, gardée
      par `psramFound()`, retour vérifié, **taille réellement obtenue** mémorisée
      (pas la taille demandée)
- [ ] A3 — Les quatre masques relevés **avant et après** l'appel, et l'écart
      journalisé une fois, après `systemLogger.begin()`
- [ ] A4 — Exposition des quatre masques et du résultat de réserve dans
      `/get-system-info` (route authentifiée)
- [ ] A5 — Corrélation : marquer les échantillons pris pendant un chargement
      d'interface, une session de dosage, une rafale de découverte — sans quoi
      les planchers ne diront pas **quel** événement les a produits
- [ ] C1 — `docs/subsystems/system-health.md` : la table des quatre masques et ce
      que chacun voit ; `docs/API.md` : les champs neufs
- [ ] C2 — ADR si le choix de la métrique de garde est arrêté dans ce lot
- [ ] C3 — `CHANGELOG.md`
- [ ] T1 — Tests purs : la table de sélection `(all_caps & mask) == mask` pour les
      quatre masques × trois tas (interne, PSRAM, réserve) — **12 cas**, c'est la
      dérivation elle-même mise sous test
- [ ] T2 — Tests : plancher glissant (un minimum n'est jamais oublié, une fenêtre
      se réarme), taille obtenue ≠ demandée, réserve refusée → démarrage normal
- [ ] T3 — **Mutation** : masque `0x1804` remplacé par `0x0804` (le défaut même
      qu'on cherche à éviter) ; plancher remplacé par un instantané ; taille
      demandée journalisée au lieu de la taille obtenue

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

- [ ] A6 — Relever `psram_free` **et** `heapProbeForCaps(MALLOC_CAP_SPIRAM)`
      avec plancher glissant, **corrélés** aux mêmes événements que les masques
      internes (A5). Sans plancher, une allocation transitoire est invisible.
- [ ] A7 — **Trancher l'efficacité du lot B-1** : instrumenter une allocation
      témoin de 4 096 o au démarrage et vérifier, par `heap_caps_check_ptr`
      ou par l'adresse retournée, **de quel côté elle tombe**. C'est la seule
      preuve directe ; `psram_free` n'est qu'un indice.
- [ ] T4 — Mutation : seuil `extmem` remis à 4096 (le défaut d'origine) → le
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
