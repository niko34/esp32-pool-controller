# bug-100 — `abort()` sur le `fopen` de l'historique : le lot B-3 ne couvre pas ce chemin

## Métadonnées

| Champ | Valeur |
|---|---|
| Type | **bug critique** — crash firmware en production, reproduit le jour même |
| Statut | **done** — **cycle 1** (recensement, arbitrage, ADR-0070) **et cycle 2** (implémentation, tests, CI) **livrés le 2026-09-06**, sous **v2.43.0**. ⚠️ **Correctif NON validé sur matériel** — cf. la mention de clôture en fin de document |
| Priorité | **Haute** — même mécanisme que le PANIC fondateur du bug-080, chemin non couvert |
| Découvert | 05/09/2026 ~15:35, en relevant le coredump après le déploiement de la v2.41.0 |
| Version au crash | **2.40.0** (concordance prouvée par SHA256, voir ci-dessous) |
| Parent | `bug-080` — lots A, B-1, B-2, B-3 livrés ; celui-ci est un **chemin frère non couvert** |

## Domaines touchés

- `src/history.cpp` — `historyStore->open(historyFilePath, "r")` aux lignes **500** et **586**
- `src/logger.cpp` — `flushToDisk()` (l. 276-277), **même classe de défaut**, déjà identifié hors périmètre en B-3
- `src/web_routes_data.cpp` — `/download-logs` (l. 618), **10ᵉ site de classe A** (découvert au cycle 1)
- `docs/subsystems/history.md`, `docs/subsystems/logger.md`, `docs/subsystems/web-server.md`,
  **`docs/subsystems/acces-fichiers.md`** (nouveau — le recensement)

## Sécurité chimique

**Indirecte mais réelle**, comme pour le bug-080 parent : un redémarrage inopiné interrompt une session de dosage et relance une purge de stabilisation complète (plancher 10 min sans régulation). Les compteurs journaliers survivent (NVS) et l'intégrale pH est désormais persistée (feature-093), mais le crash reste une perte de service.

Aucun code de dosage n'est modifié par le correctif attendu → gate `pool-chemistry` **non requis a priori**, à reconfirmer si le correctif touchait la cadence de sauvegarde.

## Le constat — preuve, pas inférence

Coredump relevé sur l'appareil le 05/09 à 15:35, archivé dans
`docs/incidents/2026-09-05-panic-fopen-historique.bin`, trace décodée
dans `docs/incidents/2026-09-05-panic-fopen-historique.txt`.

```
abort() ← lock_init_generic (newlib locks.c:77) ← __retarget_lock_init_recursive
← __sfp ← _fopen_r ← fopen ← VFSFileImpl ← VFSImpl::open
← fs::FS::open(historyFs, "/history.json", "r", create=false)
```

Tâche : **`loopTask`**, core 1, hors contexte d'interruption.

**Datation prouvée** : `esp-coredump` vérifie le SHA256 de l'image applicative. L'ELF
de la **v2.40.0** décode ; celui de la v2.38.0 est **rejeté**
(`coredump SHA256(2895ab0e4) != app SHA256()`). Le crash a donc eu lieu **sous la
v2.40.0**, c'est-à-dire entre **14:05 et 14:59 le 05/09** — pendant la session de
développement, sans que personne ne le remarque sur le moment.

Corroboration indépendante : l'enregistreur de plateau mémoire (échantillon toutes
les 2 min) montre **deux discontinuités d'`uptime` non expliquées**, vers **14:16**
et **14:20**, alors qu'aucun déploiement n'a eu lieu entre 14:05 et 14:47.

## Pourquoi c'est important

**C'est exactement le mécanisme du PANIC fondateur du 29/08** — `lock_init_generic`
qui appelle `abort()` parce que le `malloc` d'un verrou newlib d'environ 88 octets
échoue —, mais sur un **chemin différent** :

| | 29/08 | 05/09 (celui-ci) |
|---|---|---|
| Appelant | `AsyncWebServerRequest::send(fs, "text/html")` | `fs::FS::open(historyFs, "/history.json", "r")` |
| Tâche | `async_tcp` | **`loopTask`** |
| Déclencheur | chargement d'interface | **sauvegarde périodique de l'historique** |
| Couvert par B-3 ? | **oui** | **NON** |

Le lot **B-3 garde les requêtes HTTP GET** servant des fichiers. Il ne garde **rien**
de ce que le firmware ouvre pour son propre compte. Or l'historique est sauvegardé
**toutes les 5 minutes depuis la boucle principale**, et la ligne 500
(`File old = historyStore->open(historyFilePath, "r")`) est sur ce chemin — le
candidat le plus probable, la pile étant tronquée au-delà de `fs::FS::open`.

⚠️ **Le lot B-3 a donc fermé un chemin sur au moins trois.** Le `code-reviewer`
l'avait anticipé pour `Logger::flushToDisk()` ; celui de l'historique n'avait pas
été vu, et c'est celui qui a crashé.

## Ce qui est demandé

1. **Recenser TOUS les `fopen` du firmware pour son propre compte** — historique
   (lecture et écriture), journal (`flushToDisk`), configuration, calibrations,
   coredump. Produire la liste exhaustive : c'est elle le livrable principal, pas le
   correctif d'un site isolé.
2. **Décider d'une stratégie unique** plutôt que d'un rustine par site. Pistes à
   arbitrer :
   - une garde mémoire **avant** chaque `fopen` interne, qui **reporte** l'opération
     au lieu de la tenter (l'historique peut sauter un cycle de 5 min sans dommage —
     contrairement à une requête utilisateur, il n'y a personne à qui répondre) ;
   - un **helper unique** `openIfHeapAllows()` par lequel tout `fopen` interne doit
     passer, sur le modèle de `oomPmSetFlags()` du lot B-2 : rendre le défaut
     **inexprimable** plutôt qu'untesté ;
   - pré-allouer le verrou newlib au démarrage (à étudier : `__sfp` n'est appelé que
     lorsque le pool de 4 `FILE` est épuisé — un pool pré-chauffé supprimerait
     peut-être la classe entière).
3. **Ne pas oublier le report** : une sauvegarde d'historique sautée doit être
   **retentée**, et le saut doit être **observable** (compteur, WARN throttlé). Un
   report silencieux répété ferait perdre l'historique sans que rien ne le dise —
   c'est le mode de défaillance déjà identifié dans `bug-099`.

## Critères d'acceptation

- [x] Liste exhaustive des `fopen` internes, avec pour chacun : peut-il être reporté,
      ou doit-il aboutir ? → **14 sites**, dans un lieu durable :
      [`docs/subsystems/acces-fichiers.md`](../../../docs/subsystems/acces-fichiers.md)
      (⚠️ dont `history.cpp:581`, invisible à tout `grep` : `exists()` ouvre le fichier)
- [x] Aucun `fopen` interne ne peut plus être tenté sous le seuil retenu
      *(classe A **8 192**, classe B **5 120** ;
      [ADR-0070](../../../docs/adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md))* —
      **13 sites** routés par `src/fs_guard.cpp`, **point d'admission unique**,
      contrôlé en CI par `tools/check_fopen_guard.sh` (job `fopen-guard`)
- [x] Le saut est observable (jamais silencieux) et l'opération est retentée —
      compteurs **`hdef` / `ldef` / `xdef`** dans le payload **RETENU**
      `<base>/diagnostic`, WARN throttlé 1/h, retentative naturelle au prochain
      `SAVE_INTERVAL` (5 min) / `kFlushIntervalMs` (10 min)
- [x] Test natif du prédicat de report + **validation par mutation** —
      `test/test_native_fopen_guard/`, **34 cas**, **1 291 → 1 325 tests**.
      ⚠️ **3 mutants non tués**, consignés comme **dette assumée** (voir plus bas)
- [x] Budget flash chiffré — **1 825 701 → 1 829 013 o (+3 312), 49,0 %,
      marge 1 906 539** ; RAM **+64 o**
      *(mis à jour le 05/09 : le layout v5 a doublé la partition applicative. La
      version initiale de cette spec annonçait 9 291 o et exigeait « son propre
      poste de récupération » — cette contrainte n'existe plus.)*

## Notes

Le crash est survenu **pendant** la session qui livrait B-2 et B-3, sur un appareil
qu'on surveillait. Il n'a été vu qu'après coup, en relevant le coredump — les deux
redémarrages étaient passés inaperçus dans les relevés d'`uptime`.

Leçon à retenir : **`uptime` seul est un mauvais détecteur de redémarrage** quand on
ne lit que les minutes modulo 60. Vérifier `/coredump/info` fait désormais partie de
tout relevé de diagnostic.

---

## Décisions du cycle 1 (2026-09-06)

**Statut** : blocs **A** (recensement) et **B** (arbitrage, preuve, ADR) **terminés**.
**Aucune ligne de code n'a été écrite.** La spec **reste dans `doing/`** :
l'implémentation est le **cycle 2**, après validation humaine.

**Gate** : `pool-chemistry` **GO au 2ᵉ passage**, conditions **C1-C6 levées**.
`architect` a produit la stratégie, puis l'a **révisée après réfutation** (cf. la
preuve binaire ci-dessous).

### Livrables du cycle

| Livrable | Où |
|---|---|
| Décision structurante | [`docs/adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md`](../../../docs/adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md) |
| Recensement des 14 sites (**lieu durable**) | [`docs/subsystems/acces-fichiers.md`](../../../docs/subsystems/acces-fichiers.md) |
| Périmètre borné du lot B-3 | mis à jour dans `bug-080` + `docs/subsystems/web-server.md` |
| Spec de suivi ouverte | `specs/features/todo/bug-108-garde-tls-ota-sur-metrique-ira-inclusive.md` |

**Deux docs volontairement NON touchées, et c'est une conclusion, pas un oubli :**

- **`CHANGELOG.md` — aucune entrée.** Rien n'est livré, aucun comportement
  n'est modifié, aucun binaire ne change. Le changelog datera le **cycle 2**.
- **`docs/user/src/*` — aucune modification, aucun PDF régénéré.** Le guide de
  régulation et le guide d'utilisation décrivent le comportement de l'appareil
  tel qu'il tourne : ce cycle ne le change pas d'un iota. La dégradation
  utilisateur *future* (« démarrage sans historique persistant mais en dosant »)
  deviendra visible au cycle 2 — c'est **là** que le guide d'utilisation devra
  décrire le message correspondant, et c'est là seulement que la question d'une
  capture d'écran se posera.

### La stratégie retenue, en six points

1. **Point d'admission unique** (helper `openIfHeapAllows()` ou équivalent) — le
   défaut devient **inexprimable**, sur le modèle de `oomPmSetFlags()` du lot B-2.
   La garde par site est rejetée : **c'est le mode de défaillance du lot B-3**.
2. **Deux classes de contention.** **A** (contention TCP réelle : les deux sites de
   `saveToFile()` atteignables depuis `POST /history/import`, **tout le journal**
   puisque `flushToDisk()` s'exécute sur la tâche qui logue, et `/download-logs`)
   → **6ᵉ barreau EXISTANT, 8 192**, marge **×1,99**. **B** (les 4 sites de boot)
   → **7ᵉ barreau NOUVEAU, 5 120**, marge **×5,9**, absence de contention
   **prouvée par l'ordre de `setup()`** (`history.begin()` à `main.cpp:270`, avant
   `mqttManager.begin()` 300 et `webServer.begin()` 316).
3. **Le 7ᵉ barreau est un tripwire de boot, pas un seuil de report** : au
   démarrage, « reporter » n'a pas de sens. Sous 5 120 → `historyEnabled = false`
   pour ce boot (état déjà supporté, `history.cpp:326-331`) au lieu d'`abort()`.
   Entre 4 096 et 5 120, **l'appareil démarre sans historique persistant mais en
   dosant** — gain de disponibilité de la régulation.
4. **La garde va au point d'ADMISSION, pas d'ouverture** : `saveToFile()` n'est pas
   atomique (`remove()` l. 529 précède `open("w")` l. 531) — une garde sur
   l'ouverture transformerait l'`abort()` en **destruction du fichier
   d'historique**. Et elle doit précéder la **ligne 500**, dont le `fopen` fournit
   la taille consommée par `historySaveAdmit()`. → **tête de `saveToFile()`**.
5. **On ne met en cache QUE LE REFUS** (asymétrie mesure/décision) : *autoriser*
   exige une mesure fraîche, *refuser* n'en exige aucune. Autorisation périmée =
   `abort()` ; refus périmé = sauvegarde différée de 5 min. La fraîcheur
   d'**ADR-0058:99** est respectée là où elle décide, et le coût s'effondre
   **exactement dans le régime qui produit les rafales**.
6. **La contradiction `critical()` est résolue par l'effet du correctif** : la
   chaîne `critical` → flush synchrone → `fopen` **non gardé** → `abort()` perd sa
   prémisse dès lors que ce `fopen` est gardé. Escalade vive = Serial + alerte
   MQTT + compteurs dans le payload **retenu** `<base>/diagnostic` ; `critical()`
   **différé au front descendant**. L'interdit reste dans le helper en défense en
   profondeur. *On ne renonce ni à l'audibilité ni à la durabilité, on les
   désynchronise.*

### Réponses aux trois pistes du §« Ce qui est demandé »

- **Garde mémoire avant chaque `fopen` interne avec report** → **retenue**, sous la
  forme (a) + (d) ci-dessus.
- **Helper unique `openIfHeapAllows()`** → **retenu**, c'est le point 1.
- **Pré-allouer le verrou newlib / pool `FILE` pré-chauffé** → **PISTE CLOSE PAR
  PREUVE BINAIRE, aucune spec d'étude nécessaire.** La spec supposait que `__sfp`
  n'est appelé qu'à épuisement du pool de 4 `FILE` ; le désassemblage de `libc.a`
  (newlib, GCC 14.2.0) montre l'inverse : l'init du verrou est sur le chemin de
  **réutilisation** d'un slot (`callx8 __retarget_lock_init_recursive`, offset
  **0x37**), et le chemin d'expansion **ne retourne pas** — il rejoint la boucle de
  scan (`j 1b` en **0xae**) et retombe donc nécessairement dessus. Le `malloc` de
  428 o et l'init du verrou sont **séquentiels, pas exclusifs** : pré-chauffer
  supprimerait celui qui échoue **proprement** en `NULL`/`ENOMEM` et laisserait
  intact celui qui **aborte**. De plus `__retarget_lock_init_recursive` **remet le
  verrou à zéro** avant de l'initialiser, ce qui annule tout pré-chauffage et rend
  **inatteignable** le garde-fou `if (*lock != 0) skip` de `lock_init_generic`.

### ⚠️ Trois chiffres du projet sont faux — à corriger au CYCLE 2

Dans [`src/constants.h:244-246`](../../../src/constants.h) (bloc de dimensionnement
de `kStaticFileMinAllocBytes`). **Non corrigés par ce cycle**, qui ne touche aucun
fichier de `src/` :

| Écrit | Réel |
|---|---|
| verrou récursif « ~88 o » | **92 o** (`sizeof(Queue_t)`, IDF 5.5) |
| expansion du pool « ~440 o » | **428 o** (`0x1ac`) |
| `__sfmoreglue` supposé présent | **n'existe pas comme symbole — inliné** (une recherche par symbole aurait conclu à tort à son absence) |

Aucun seuil n'en dépend : les marges ×1,99 et ×5,9 sont calculées sur les valeurs
réelles (besoin dur **≈ 874 o**, besoin avec 2 pbuf en vol **≈ 4 118 o**).

### ✅ Confirmation au bit près

L'allocation qui tue demande `caps = 0x804` = `MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL`,
soit **exactement** `heapCaps8BitMask()`. L'exigence « métrique 8 bits »
d'ADR-0058 n'est pas stylistique ici, elle est **juste au bit près**. Et
**92 o ≤ `kPsramExtmemThresholdBytes` (4 095)** : ce bloc reste **toujours** en RAM
interne — la bascule PSRAM du lot B-1 ne le déplace pas.

### ⚠️ Le site caché — la leçon la plus transférable du cycle

[`history.cpp:581`](../../../src/history.cpp) — `historyStore->exists()` **ouvre le
fichier**. `VFSImpl::exists()` du framework ne fait pas de `stat()` : il construit
un `VFSFileImpl(this, fpath, "r")`. **Aucun `grep` sur `open(` ne le trouve.**
C'est pourquoi le recensement vit dans `docs/subsystems/acces-fichiers.md`, tenu à
la main, et non dans une commande qu'on rejouerait.

### Ce qui reste à faire (cycle 2)

- [x] Implémenter le helper d'admission + le prédicat **pur** (ADR-0017) —
      `src/fopen_guard_logic.{h,cpp}` (pur) + `src/fs_guard.{h,cpp}` (coquille)
- [x] Ajouter le **7ᵉ barreau `5120`** à l'échelle ADR-0058 + `static_assert`
      **d'encadrement** (2 assertions) ; **6 barreaux existants intacts**
- [x] Router les sites par le helper, garde en **tête de `saveToFile()`** —
      **13 sites** (le 14ᵉ, l'`exists()`, a été **supprimé** et non gardé)
      **+ une SECONDE admission après la sérialisation** (cf. bloquant nº 1)
- [x] Observabilité : compteurs `<base>/diagnostic` (retain), WARN throttlé,
      `critical()` différé au front descendant
- [x] Corriger les **trois chiffres** de `constants.h`
- [x] Tests natifs du prédicat + **validation par mutation** (3 mutants de
      coquille non tués — dette assumée, voir plus bas)
- [x] Budget flash chiffré

---

## Cycle 2 (2026-09-06) — implémentation, revue, clôture

**Statut** : **livré — tests verts, revue OK**. Version : **2.43.0**.

### Fichiers modifiés

| Fichier | Nature |
|---|---|
| `src/fopen_guard_logic.{h,cpp}` | **[nouveaux]** module **pur** ([ADR-0017](../../../docs/adr/0017-logique-metier-pure-humble-object-testabilite.md)) : prédicat de report, machine d'épisode **asymétrique**, compteur d'escalade |
| `src/fs_guard.{h,cpp}` | **[nouveaux]** coquille impure : `openIfHeapAllows()`, `existsIfHeapAllows()`, `fsGuardAdmit()`, `fsGuardDrain()`, compteurs atomiques |
| `src/constants.h` | `kInternalFopenMinAllocBytes = 5120` (**7ᵉ barreau**) + 3 cadences + 2 `static_assert` d'encadrement ; échelle ADR-0058 mise à jour, **6 barreaux existants intacts** ; **3 chiffres faux corrigés** |
| `src/history.cpp` | admission en tête de `saveToFile()` **+ seconde admission après sérialisation** ; tripwire de boot (`begin()`, `loadFromFile()`) ; `exists()` **supprimé** |
| `src/logger.cpp` | une seule mesure par `flushToDisk()`, **7 ouvertures gardées** |
| `src/web_routes_data.cpp` | `/download-logs` gardé, **troncature explicite** dans le fichier téléchargé |
| `src/mqtt_manager.cpp` | 3 champs `hdef` / `ldef` / `xdef` dans le payload **retenu** `<base>/diagnostic` |
| `src/main.cpp` | `fsGuardDrain()` dans la loop |
| `tools/check_fopen_guard.sh` | **[nouveau]** + job `fopen-guard` dans `.github/workflows/hygiene-specs.yml` (workflow renommé **« Hygiène du dépôt »**) |
| `test/test_native_fopen_guard/` | **[nouveau]** 34 cas — **1 291 → 1 325 tests** |

**Coût** : flash **1 825 701 → 1 829 013 o** (+3 312, **49,0 %**, marge
**1 906 539**) ; RAM **+64 o**.

### Les trois bloquants trouvés en revue, et leurs correctifs

#### 1. ⚠️ Une garde placée avant l'ouverture ne suffit pas — il faut avant le PIC D'ALLOCATION

**C'est la leçon la plus transférable du cycle**, et elle généralise le §d de
l'ADR-0070 sans le contredire.

La première version posait l'admission en tête de `saveToFile()`, conformément à
l'ADR. Entre cette admission et le `fopen` réellement gardé, la fonction
construit le `JsonDocument` de l'historique et ses `String` : **elle dépense la
marge qu'elle vient de valider**. L'ouverture s'exécutait donc sur une mesure
**devenue fausse** — c'est-à-dire sur une **autorisation périmée**, exactement ce
que l'asymétrie mesure/décision du §e existe pour interdire (*autoriser* sur une
mesure périmée coûte un `abort()` ; *refuser* coûte 5 min).

**Correctif** : une **seconde admission**, après `measureJson()` (donc après le
pic) et toujours **avant** le `remove()` (donc là où l'opération peut encore ne
rien faire). Les deux gardes sont nécessaires et non redondantes ; coût = une
mesure de plus toutes les 5 min.

**Lieu durable choisi** :
[`docs/subsystems/acces-fichiers.md` § Piège de placement nº 2](../../../docs/subsystems/acces-fichiers.md) —
un document **vivant**, celui qu'un futur auteur de garde consulte, et non un ADR
figé. L'ADR-0070 en porte une **bannière d'amendement** (corps **inchangé**) dont
la seule fonction est d'empêcher qu'on implémente son §d à la lettre en croyant
avoir fini. Rappel repris aussi dans `docs/subsystems/history.md`.

#### 2. Le tripwire de boot ne comptait pas son propre refus

La dégradation **la plus sévère** du lot (démarrage sans historique persistant)
était la **moins visible** : elle n'incrémentait pas `hdef`. Or le WARN qui
l'accompagne n'est durable que de façon **bornée** — `_persistBuffer` plafonne à
**100 entrées en éviction FIFO**, donc sous famine prolongée la ligne de boot
sort par le haut. **Correctif** : le tripwire incrémente `hdef`, qui part dans le
payload **retenu** et **survit au reboot**.

Corollaire posé au même endroit : `setPersistenceFs()` **arme
inconditionnellement** (`_persistEnabled = true` **avant** l'ouverture gardée du
marqueur). Sous le tripwire, seul le **marqueur de démarrage** se perd, **pas le
journal du boot**. *L'observabilité ne doit pas dépendre du sous-système qu'elle
garde.*

#### 3. Un refus servi par le cache affichait un `maxBloc8` périmé

L'asymétrie du §e met en cache **le refus**, donc un refus peut être prononcé
**sans aucune mesure**. Le libellé citait néanmoins la dernière valeur connue,
comme si elle datait de l'instant. **Correctif** : `fsGuardLastRefusalMeasured()`
distingue une mesure fraîche d'une valeur absente, et le message dit
`maxBloc8 non mesure (refus en cache)` au lieu d'un chiffre faux. La distinction
est portée par **l'API**, pas par la discipline de l'appelant.

### Décisions consignées

- **Perte de distinction assumée dans `loadFromFile()`** : l'`exists()` a été
  **supprimé** (strictement redondant avec l'ouverture qui le suivait, et
  doublant l'exposition à l'`abort()`), donc « absent » et « illisible » ne sont
  plus discernables — l'API `fs::` n'offre **aucun `stat()`** pour les départager
  sans réintroduire le site fermé. Libellé rendu **explicitement non
  discriminant**, en `INFO`.
- **Mesure sous mutex tenu acceptée**, sur l'argument du réviseur :
  `consolidateData()` et `importData()` tiennent `_mutex` ; parcours TLSF ~10² µs
  contre une E/S LittleFS de 10¹-10² ms **déjà payée sous ce verrou** ; coût
  « interruptions coupées » **identique** avec ou sans mutex ; verrou du tas
  **terminal**, donc **aucune inversion** possible.
- **Limites du script de garde, écrites plutôt que tues** : `grep` est
  **mono-ligne** (une coupure après `->` échappe au motif) et toute API
  construisant un `VFSFileImpl` **sous un autre nom** passe. D'où le recensement
  tenu **à la main** dans `docs/subsystems/acces-fichiers.md`. *Le script tient
  l'ordre ; le document tient la liste.*

### ⚠️ Dette assumée — trois mutants non tués

Tous dans `history.cpp`, **hors du `build_src_filter` natif** :

1. le **tripwire de boot** de `begin()` ;
2. le **tripwire de `loadFromFile()`** ;
3. la **seconde admission** de `saveToFile()` (bloquant nº 1 ci-dessus).

**Ce n'est pas un oubli : c'est la frontière de l'[ADR-0017](../../../docs/adr/0017-logique-metier-pure-humble-object-testabilite.md).**
La logique pure est testable, la coquille ne l'est pas. Les **prédicats** que ces
trois chemins invoquent sont couverts et **tuent** leurs mutants ; ce qui n'est
pas couvert, c'est le **câblage** entre le prédicat et l'effet.

**Ce qu'il faudrait pour lever la dette** : un **double de `fs::FS`** compilable
en natif, ou l'**extraction des trois séquences en fonctions pures** prenant
`(admis, taille, état)` et retournant une action. Aucune des deux n'est gratuite,
et aucune n'est justifiée par le risque courant — la décision est *un booléen
déjà testé* dans les trois cas. **À réévaluer si un quatrième site de coquille
apparaît.**

### 🔴 Action à faire avant tout déploiement — bump de version

**`src/version.h` porte encore `2.42.1`.** Il doit passer à **`2.43.0`**.

Le motif n'est pas cosmétique : l'entrée `[2.42.1]` du `CHANGELOG.md` affirme
« **aucun changement de comportement : binaire fonctionnellement identique** ».
Ce cycle **change** le comportement (report des écritures, tripwire de boot,
troncature annoncée, trois champs MQTT). Faire atterrir `bug-100` sous `2.42.1`
rendrait cette entrée **fausse**.

L'entrée `CHANGELOG.md` est **déjà rédigée sous `[2.43.0]`** ; `src/version.h`
était **gelé** pendant ce cycle de documentation et n'a pas été touché.

### Ce que ce cycle ne ferme PAS

- **Le critère « 7 jours sans redémarrage inopiné » de `bug-080`** reste
  **ouvert** — aucun déploiement, donc aucune mesure possible. Consigné dans
  `specs/features/doing/bug-080-*.md` (section ajoutée le 2026-09-06).
- **Le lot C de `bug-080`** (cause de la fragmentation) n'est **pas** ouvert :
  ce cycle traite la **conséquence**, pas la cause.
- **`bug-108`** (garde TLS/OTA sur métrique IRAM-inclusive) reste dans `todo/`.

---

## ⚠️ Mention de clôture

> **Correctif NON validé sur matériel.** L'appareil est joignable
> (192.168.1.48) mais **aucun déploiement n'a été fait** — c'est une décision
> humaine. Ce cycle livre une **défense argumentée et testée en logique pure** ;
> il **ne prouve pas** la disparition du crash en production.
