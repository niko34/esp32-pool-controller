# ADR-0070 — Tout `fopen` du firmware pour son propre compte passe par un point d'admission unique, gardé par CLASSE DE CONTENTION

- **Statut** : Accepté — stratégie arrêtée au **cycle 1** (2026-09-06). **Aucun code n'est livré par ce cycle** ; l'implémentation est le cycle 2, après validation humaine.
- **Date** : 2026-09-06
- **Décideurs** : `architect` (stratégie + preuve binaire, révisée après réfutation), `spec-reader` (recensement), `pool-chemistry` — **GO au 2ᵉ passage**, conditions **C1-C6 levées**
- **Spec(s) liée(s)** : `bug-100-abort-sur-fopen-de-l-historique` (cycle 1), `bug-080-redemarrage-panic-et-fragmentation-heap` (parent, lot B-3)

> 📋 **Amendement du 2026-09-06 (cycle 2 de `bug-100`, v2.43.0 — implémentation)** : cet ADR reste **Accepté** et son corps est **inchangé**. Deux faits s'y ajoutent, que l'implémentation a établis et que le §d ne dit pas.
>
> **1. Le §d est nécessaire mais NON SUFFISANT.** Il pose que « la garde va au point d'ADMISSION, pas d'ouverture », et localise ce point en **tête de `saveToFile()`**. La revue du cycle 2 a établi qu'**une garde placée avant l'ouverture ne suffit pas : il faut la placer avant le PIC D'ALLOCATION du chemin**. Entre l'admission en tête de fonction et le `fopen`, `saveToFile()` construit le `JsonDocument` de l'historique et ses `String` — elle **dépense la marge qu'elle vient de valider**. L'ouverture s'exécutait donc sur une mesure devenue fausse, c'est-à-dire sur une **autorisation périmée** : exactement ce que l'asymétrie du §e existe pour interdire. D'où une **seconde admission**, après `measureJson()` et toujours avant le `remove()`. Le §d n'est pas contredit — il est **complété** : les deux gardes sont nécessaires et non redondantes. La règle générale et son tableau de placement vivent dans [`docs/subsystems/acces-fichiers.md` § Piège de placement nº 2](../subsystems/acces-fichiers.md#-piège-de-placement-nº-2--retiré-feature-131--le-pic-quil-protégeait-a-disparu) (⚠️ **section retitrée depuis feature-131/v2.53.0** — la seconde admission qu'elle documentait n'existe plus pour ce chemin, voir la note ajoutée plus haut dans cet ADR), lieu **vivant** où un futur auteur de garde ira les lire ; cette bannière n'existe que pour empêcher qu'on implémente le §d à la lettre en croyant avoir fini.
>
> **2. Le compte passe de 14 à 13 sites.** Le 14ᵉ — l'`exists()` de `history.cpp`, le site qu'aucun `grep` ne trouvait — a été **supprimé** et non gardé : il était strictement redondant avec l'ouverture en lecture qui le suivait. Contrepartie **assumée** : `loadFromFile()` ne distingue plus « absent » d'« illisible », l'API `fs::` n'offrant aucun `stat()` pour les départager sans réintroduire le site fermé. Le libellé est rendu explicitement non discriminant.
>
> **Statut du code** : livré, compilé (flash 49,0 %, +3 312 o), testé en logique pure (34 cas, 1 325 tests natifs), contrôlé en CI (`tools/check_fopen_guard.sh`). ⚠️ **NON validé sur matériel** — aucun déploiement n'a eu lieu.

> 📋 **Amendement du 2026-09-19 (feature-131, v2.53.0) — le point 1 ci-dessus devient HISTORIQUE pour `saveToFile()` de l'historique.** La seconde admission qu'il imposait protégeait le pic de construction du `JsonDocument` complet entre la mesure et l'écriture. L'écriture en flux de l'historique (« un émetteur, deux puits », [ADR-0083](0083-ecriture-atomique-historique-et-contrat-de-format-persiste.md)) supprime ce pic : la mesure et l'écriture n'allouent plus qu'un point à la fois (128 o) plus un tampon (512 o), tous deux sur pile. `saveToFile()` ne pose donc plus qu'**une seule** admission. Le §d et le point 1 restent la référence **générale** pour tout futur chemin qui construirait un document complet en mémoire avant de l'écrire — ils ne décrivent simplement plus le chemin historique lui-même. Le compte de sites passe en outre de **14 à 13** (le point 2 ci-dessus l'avait déjà fait passer de 14 à 13 puis `bug-099` l'avait remonté à 14 ; feature-131 l'a fait redescendre à 13 par un mécanisme différent — la lecture d'`oldSize`, devenue inutile, a disparu) : voir [`docs/subsystems/acces-fichiers.md`](../subsystems/acces-fichiers.md#le-site-1-a-disparu-feature-131-v2530).

## Contexte

### Le crash, daté au SHA256

```
abort() ← lock_init_generic (newlib locks.c:77) ← __retarget_lock_init_recursive
← __sfp ← _fopen_r ← fopen ← VFSFileImpl ← VFSImpl::open
← fs::FS::open(historyFs, "/history.json", "r", create=false)
```

Tâche **`loopTask`**, core 1, hors contexte d'interruption. Coredump archivé :
[`docs/incidents/2026-09-05-panic-fopen-historique.bin`](../incidents/2026-09-05-panic-fopen-historique.bin),
trace décodée dans le `.txt` de même nom. `esp-coredump` vérifie le SHA256 de
l'image applicative : l'ELF de la **v2.40.0** décode, celui de la v2.38.0 est
rejeté. Le crash est **daté par preuve**, pas par inférence.

C'est le mécanisme du PANIC fondateur du 29/08 ([ADR-0067](0067-garde-503-avant-fopen-handler-prioritaire.md)),
sur un chemin frère : le `malloc` du verrou récursif que newlib attache à chaque
`FILE` échoue, et `lock_init_generic` **aborte**. Il n'y a **aucun code de retour
à tester**. La seule défense possible reste **de ne pas entrer dans `fopen`**.

### Ce que le lot B-3 ferme — et ce qu'il ne ferme pas

[ADR-0067](0067-garde-503-avant-fopen-handler-prioritaire.md) pose un handler
prioritaire **`HTTP_GET`**, enregistré après `wsManager.begin(server)` et avant
`/login.html`. Il couvre **le service de fichiers statiques** : les 7 routes HTML
explicites et `serveStatic`. Il ne couvre **rien** de ce que le firmware ouvre
pour son propre compte :

- les sites internes ne passent par **aucun matcher HTTP** ;
- `/download-logs` est déclaré **avant** le handler → **exempt par construction** ;
- `/history/import` est un **`HTTP_POST`** → **hors matcher**.

Le recensement de `bug-100` chiffre le périmètre : **14 sites** d'ouverture de
fichier pour le compte propre du firmware, dont B-3 en garde **zéro**. Le
verdict « au moins trois chemins » de la spec parent est remplacé par un compte
exact — voir [`docs/subsystems/acces-fichiers.md`](../subsystems/acces-fichiers.md).

### ⚠️ Trois chiffres du projet sont FAUX

Le bloc de dimensionnement de [`constants.h:244-246`](../../src/constants.h:244)
repose sur des ordres de grandeur repris de proche en proche. Le désassemblage de
`libc.a` (newlib, GCC 14.2.0) et la lecture des sources IDF 5.5 les corrigent :

| Poste | Valeur écrite | Valeur **réelle** |
|---|---|---|
| verrou récursif newlib | ~88 o | **92 o** (`sizeof(Queue_t)`, IDF 5.5) |
| expansion du pool `FILE` | ~440 o | **428 o** (`0x1ac`) |
| symbole `__sfmoreglue` | supposé présent | **n'existe pas comme symbole — il est inliné** |

Le troisième point est le plus coûteux en méthode : **une recherche par symbole
aurait conclu à tort à son absence**. La correction de ces trois chiffres est un
**travail du cycle 2** ; elle ne change **aucun seuil** (les marges ci-dessous
sont calculées sur les valeurs réelles).

### ✅ La confirmation au bit près

L'allocation qui tue demande `caps = 0x804`, soit exactement
`MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL` — c'est-à-dire précisément le masque de
`heapCaps8BitMask()`. L'exigence « les gardes décident en métrique 8 bits »
d'[ADR-0058](0058-gardes-heap-metrique-8-bits.md) **n'est pas stylistique ici** :
elle est **juste au bit près** pour cette allocation.

Et **92 o ≤ `kPsramExtmemThresholdBytes` (4 095)** : ce bloc reste **toujours** en
RAM interne. La bascule PSRAM du lot B-1
([ADR-0061](0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md))
ne le déplace pas, et ne pouvait pas le déplacer.

### 🔒 La preuve qui CLÔT l'option du pool `FILE` pré-chauffé

La spec `bug-100` supposait que `__sfp` n'est appelé qu'à **épuisement** du pool
de 4 `FILE`, et en déduisait qu'un pool pré-chauffé au démarrage supprimerait la
classe entière de crashs. **Le désassemblage montre l'inverse.**

1. L'initialisation du verrou est sur le chemin de **réutilisation** d'un slot :
   `callx8 __retarget_lock_init_recursive` à l'**offset 0x37**, dans la boucle de
   scan, pas dans la branche d'expansion.
2. Le chemin d'expansion **ne retourne pas** : il rejoint la boucle de scan
   (`j 1b` en **0xae**) et **retombe donc nécessairement** sur cette même
   initialisation.
3. Le `malloc` de **428 o** (expansion) et l'init du verrou sont donc
   **séquentiels, pas exclusifs**. Pré-chauffer supprimerait le **premier** —
   celui qui échoue **proprement** en `NULL`/`ENOMEM` — et laisserait intact le
   **second**, celui qui **aborte**.
4. Pire : `__retarget_lock_init_recursive` **remet le verrou à zéro** avant de
   l'initialiser. Tout pré-chauffage est **annulé**, et le garde-fou
   `if (*lock != 0) skip` de `lock_init_generic` est rendu **inatteignable**.

**Piste close. Aucune spec d'étude n'est nécessaire.** C'est la deuxième fois de
ce chantier qu'une intuition confortable est réfutée par le binaire ; la première
avait coûté trois cycles au lot B-1.

### ⚠️ Le site que `grep` ne trouve pas

[`history.cpp:581`](../../src/history.cpp:581) — `historyStore->exists(path)`
**ouvre le fichier**. `VFSImpl::exists()` du framework ne fait **pas** de
`stat()` : il construit un `VFSFileImpl(this, fpath, "r")`. **Aucune recherche
sur `open(` ne le trouve.** C'est la leçon la plus transférable du cycle, et la
raison pour laquelle le recensement vit dans une doc durable plutôt que dans une
commande `grep` qu'on rejouerait.

## Décision

### (a) Un point d'admission unique — le défaut devient inexprimable

Tout `fopen` que le firmware exécute **pour son propre compte** passe par un
helper unique (`openIfHeapAllows()` ou équivalent), sur le modèle de
`oomPmSetFlags()` du lot B-2 : **rendre le défaut inexprimable plutôt
qu'untesté**. Une garde par site est explicitement rejetée — c'est le **mode de
défaillance du lot B-3 lui-même**, qui a fermé un chemin en laissant 14 sites
ouverts parce que la discipline ne se propage pas.

Le prédicat de décision est **pur** ([ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)),
donc testable en natif et validable par mutation.

### (b) Deux classes de contention, deux seuils

La contention TCP est ce qui sépare les deux régimes : un site atteignable
pendant qu'un handshake ou une réponse chunked est en vol doit couvrir, en plus
du besoin dur, **les pbuf lwip en vol**.

Besoin dur d'une ouverture (valeurs **réelles**, corrigées ci-dessus) :

| Poste | Taille | Échec = |
|---|---|---|
| verrou récursif newlib (`__sfp`) | **92 o** | **`abort()`** |
| expansion du pool `FILE` (inliné) | **428 o** | `NULL` propre |
| `esp_littlefs_open` + cache lfs | ~190 o | `NULL` propre |
| `new VFSFileImpl` + bloc `shared_ptr` | ~64 o | `bad_alloc` |
| `String` de chemin (×2-3) | ~100 o | — |
| tampon stdio 4096 | **PSRAM** (lot B-1) | hors budget interne |
| **Total besoin dur** | **≈ 874 o** | |
| pbuf lwip TCP, 2 en vol | +2 × 1 622 o | retransmission |
| **Total avec contention TCP** | **≈ 4 118 o** | |

**Classe A — contention TCP réelle → 6ᵉ barreau EXISTANT, `8192`** (marge
**×1,99** sur 4 118 o). Aucun barreau n'est créé : la classe A réutilise
`kStaticFileMinAllocBytes`. Y appartiennent :
- les deux sites de `saveToFile()`, atteignables depuis **`POST /history/import`**
  (→ `importData()` → `saveToFile()`, sur `async_tcp`) ;
- **tout le journal**, parce que `flushToDisk()` s'exécute **sur la tâche qui
  logue** — n'importe laquelle ;
- l'ouverture paresseuse de **`/download-logs`**, sur `async_tcp`.

**Classe B — 4 sites de boot → 7ᵉ barreau NOUVEAU, `5120`** (marge **×5,9** sur
874 o). L'absence de contention n'est pas supposée, elle est **prouvée par
l'ordre de `setup()`** : `history.begin()` est appelé à
[`main.cpp:270`](../../src/main.cpp:270), **avant** `mqttManager.begin()`
([300](../../src/main.cpp:300)) et `webServer.begin()`
([316](../../src/main.cpp:316)). Aucune pile TCP applicative n'est démarrée. La
marge y est donc du besoin dur, par **construction**.

Échelle résultante — **les 6 barreaux existants restent INCHANGÉS** (condition C5
des gates B-1/B-2, reconduite) :

```
 2 048  kOomFallbackRearmMinMaxAllocBytes  réarmement réserve de repli
 4 096  kRestartMaxAllocHeapBytes          reboot d'auto-guérison
 5 120  <NOUVEAU>                          ← 7ᵉ barreau : tripwire de boot fichiers
 6 144  kWsPushMinAllocBytes               pushes WS sautés
 8 192  kStaticFileMinAllocBytes           service de fichiers refusé (503) + classe A
12 288  kLowHeapWarnBytes                  WARN heap + flush disque
18 432  kOomRearmMinMaxAllocBytes          réarmement réserve nominale
```

### (c) Le 7ᵉ barreau n'est pas un seuil de report : c'est un TRIPWIRE DE BOOT

Au démarrage, **« reporter » n'a pas de sens — il n'y a pas de plus tard**. Sous
`5 120`, la classe B ne diffère pas l'ouverture : elle bascule
`historyEnabled = false` **pour ce boot**, état **déjà supporté et déjà
journalisé** ([`history.cpp:326-331`](../../src/history.cpp:326)), au lieu
d'`abort()`.

Conséquence opérationnelle, et c'est le point : entre **4 096** (auto-guérison) et
**5 120**, l'appareil **démarre sans historique persistant mais en dosant**. Ce
n'est pas un confort de diagnostic, c'est un **gain de disponibilité de la
régulation** — le régime alternatif est un bootloop sur `abort()`.

### (d) La garde se place au point d'ADMISSION, pas au point d'OUVERTURE

`saveToFile()` **n'est pas atomique** : `historyStore->remove(path)`
([l. 529](../../src/history.cpp:529)) **précède** `open(path, "w")`
([l. 531](../../src/history.cpp:531)). Une garde posée sur l'ouverture
transformerait l'`abort()` en **destruction du fichier d'historique** — on
échangerait un crash contre une perte de données silencieuse.

Et elle doit précéder la **ligne 500**, pas seulement la 529 : `historySaveAdmit()`
consomme `oldSize`, une taille obtenue par le `fopen` de la ligne 500. Garder la
529 laisserait le premier `fopen` s'exécuter sans garde.

**D'où le placement en tête de `saveToFile()`.** Règle générale à retenir : la
garde va là où la fonction peut encore **ne rien faire**, pas là où elle a déjà
commencé.

### (e) L'asymétrie mesure/décision : on ne met en cache QUE LE REFUS

[ADR-0058:99](0058-gardes-heap-metrique-8-bits.md) verrouille : **« toute nouvelle
garde heap lit `heapLargestFreeBlock8Bit()` »**, et
[ADR-0058:79](0058-gardes-heap-metrique-8-bits.md) rejette **nommément** la
mutualisation avec l'échantillon 2 s — « des effondrements de ~45 Ko ont été
observés entre deux échantillons ». Le coût d'un parcours TLSF, lui, **croît avec
la fragmentation** : il est maximal exactement dans le régime pour lequel la garde
existe.

> ⚠️ **Ce qu'il ne faut PAS invoquer ici.** `heap_metrics.h:51-58` **prescrit** le
> miroir aux chemins d'échec ; il ne le leur **réserve** pas. Le gate l'avait lu
> comme une restriction, et l'a **reconnu**. Un ADR qui s'appuie sur une source
> mal lue est **exactement ce qui a coûté trois cycles au lot B-1** : l'autorité
> citée ici est **ADR-0058:79 et ADR-0058:99**, pas `heap_metrics.h`.

La réconciliation tient dans une asymétrie :

> **Autoriser** exige une mesure fraîche. **Refuser** n'en exige aucune.
> Une autorisation périmée coûte un **`abort()`**. Un refus périmé coûte une
> **sauvegarde différée de 5 min**.

L'épisode ne met donc en cache **que le refus** : tant qu'un épisode est ouvert,
les demandes suivantes sont refusées **sans re-mesurer** ; toute autorisation, y
compris la sortie d'épisode, exige une **mesure au moment de la décision**. La
fraîcheur d'ADR-0058 est intégralement respectée là où elle décide quelque chose,
et le coût **s'effondre exactement dans le régime qui produit les rafales**.

### (f) La contradiction `critical()` est résolue par le correctif lui-même

[ADR-0067](0067-garde-503-avant-fopen-handler-prioritaire.md) pose un interdit
général : `ERROR`/`CRITICAL` sont interdits dans tout chemin écrit pour éviter
`fopen`, parce que [`logger.cpp:71-73`](../../src/logger.cpp:71) flushe en
**synchrone** → [`logger.cpp:276-277`](../../src/logger.cpp:276) → `fopen` →
`abort()`.

La chaîne comporte un maillon **« `fopen` non gardé »**. Ce cycle **garde ce
`fopen`** (classe A, tout le journal) : **la prémisse devient fausse par l'effet
du correctif**. L'interdit n'est pas levé pour autant — il est **conservé dans le
helper comme défense en profondeur**, parce qu'un chemin anti-`fopen` n'a aucune
raison de parier sur l'ordre de livraison des lots.

L'escalade retenue **désynchronise** audibilité et durabilité :

| Quand | Canal | Pourquoi c'est sûr |
|---|---|---|
| **Pendant** l'épisode | `Serial` | aucune allocation, aucun FS |
| **Pendant** l'épisode | alerte MQTT (`publishAlert`) | passe par `enqueueOutbound` ; se termine par un **`warning()`**, jamais `error`/`critical` ([`mqtt_manager.cpp:755`](../../src/mqtt_manager.cpp:755)) |
| **Pendant** l'épisode | compteurs dans le payload **retenu** `<base>/diagnostic` | publié par `mqttTask`, `retain=true` ([`mqtt_manager.cpp:1166`](../../src/mqtt_manager.cpp:1166)) — lisible **après coup** |
| **Front descendant** | `critical()` **différé** | le tas est revenu ; le flush synchrone peut atteindre le disque |

> **On ne renonce ni à l'audibilité ni à la durabilité : on les désynchronise.**

## Alternatives considérées

Toutes datées du **2026-09-06**.

- **Option A — une garde par site** (rejetée) : c'est **le mode de défaillance du
  lot B-3**. B-3 a appliqué une garde à un chemin et laissé 14 sites ouverts ; le
  crash du 05/09 est la démonstration empirique que la discipline par site ne se
  propage pas. Et le site de `history.cpp:581` **ne serait jamais trouvé** par la
  méthode qui produit ces gardes (recherche textuelle sur `open(`).
- **Option B — pool `FILE` pré-chauffé au démarrage** (rejetée **par preuve
  binaire**) : l'init du verrou est sur le chemin de **réutilisation** (offset
  0x37), le chemin d'expansion **retombe dessus** (`j 1b` en 0xae), et
  `__retarget_lock_init_recursive` **remet le verrou à zéro**. Pré-chauffer
  supprime l'allocation qui échoue proprement et laisse celle qui aborte. Cf.
  Contexte. **Piste close, pas de spec d'étude.**
- **Option C — mutualiser la mesure avec le miroir atomique 2 s** (rejetée) :
  [ADR-0058:79](0058-gardes-heap-metrique-8-bits.md), effondrements de ~45 Ko
  entre deux échantillons. Un faux négatif vaut ici un `abort()`.
- **Option D — seuil unique `5120` partout** (rejetée) : marge **×1,24** seulement
  sur la classe à contention TCP (4 118 o de besoin). Trop mince pour un chemin
  sans code de retour.
- **Option E — seuil unique `8192` partout** (rejetée) : laisserait les **4 sites
  de boot sans dégradation propre**. Au boot, un refus à 8 192 signifie
  simplement « pas d'historique » plus souvent qu'il n'est nécessaire, alors que
  le besoin réel y est de 874 o sans contention.
- **Option F — retenue : point d'admission unique + deux classes (8 192 / 5 120) +
  cache du seul refus** — seule combinaison qui ferme les 14 sites, respecte la
  fraîcheur d'ADR-0058 là où elle décide, et borne le coût dans le régime
  fragmenté.

## Conséquences

### Positives

- Les **14 sites** internes deviennent inatteignables en famine, par **un seul
  point de code** — pas par 14 disciplines.
- Un `fopen` interne **ajouté dans six mois** hérite de la garde s'il passe par le
  helper, et le recensement dit **où** est le helper et **pourquoi** il existe.
- L'appareil **démarre et dose** dans la bande `4 096 – 5 120` au lieu de
  bootlooper : gain de disponibilité de la régulation, pas de confort.
- L'`abort()` d'un flush de journal `ERROR` — chemin le plus perfide, puisqu'il se
  déclenche **quand ça va déjà mal** — est fermé.
- Le coût du parcours de tas **s'effondre dans le régime de rafale**, grâce au
  cache du seul refus, sans perdre une once de fraîcheur sur les autorisations.

### Négatives / dette assumée

- ⚠️ **Un historique peut être perdu en silence relatif.** Un refus répété
  toutes les 5 min ne perd que la fraîcheur, mais un boot en classe B refusé perd
  l'historique **du boot entier**. L'observabilité (compteurs `<base>/diagnostic`
  retenus, WARN throttlé, `critical()` différé) est donc **une condition de la
  décision**, pas un agrément — c'est le mode de défaillance déjà identifié dans
  `bug-099`.
- **Une 7ᵉ marche est ajoutée à l'échelle ADR-0058.** Elle était jusqu'ici un tout
  ordonné à six barreaux ; elle en compte sept. Chaque barreau supplémentaire
  rend la lecture d'un diagnostic un peu plus coûteuse.
- **Le cache du refus introduit un état d'épisode** — donc un cycle de vie, donc
  une condition de fermeture à ne pas rater. B-3 a déjà montré (WARN orphelin)
  que ces états sont subtils.
- **Trois chiffres de [`constants.h:244-246`](../../src/constants.h:244) restent
  faux jusqu'au cycle 2.** Ils sont documentaires, aucun seuil n'en dépend, mais
  ils sont dans le fichier que l'on lit pour comprendre les seuils.
- **Le critère « 7 jours sans redémarrage inopiné » du `bug-080` reste ouvert** :
  ce cycle ne livre aucun code.

### Ce que ça verrouille

- **Tout `fopen` que le firmware exécute pour son propre compte passe par le
  helper.** Un appel direct à `fs::FS::open()` hors helper est un défaut, y
  compris s'il « ne peut pas » manquer de mémoire.
- **`exists()` compte comme une ouverture.** Toute API du framework qui construit
  un `VFSFileImpl` est soumise à la même règle, quel que soit son nom.
- **Un site nouveau doit être classé A ou B**, et sa classe **prouvée** — par
  l'ordre de `setup()` pour B, pas par conviction.
- **La garde va au point d'admission** d'une opération non atomique, jamais au
  point d'ouverture.
- **On ne met en cache qu'un refus.** Toute autorisation, y compris une sortie
  d'épisode, mesure au moment où elle décide ([ADR-0058:99](0058-gardes-heap-metrique-8-bits.md)).
- **L'échelle ADR-0058 devient un tout ordonné à SEPT barreaux** ; aucun ne se
  modifie isolément.
- **`heap_metrics.h:51-58` n'est pas une autorité pour restreindre le miroir aux
  chemins d'échec** — il prescrit, il ne réserve pas.

## Références

- Incident : [`docs/incidents/2026-09-05-panic-fopen-historique.bin`](../incidents/2026-09-05-panic-fopen-historique.bin) / `.txt`
- Recensement : [`docs/subsystems/acces-fichiers.md`](../subsystems/acces-fichiers.md) — **les 14 sites**, leur classe, le périmètre exact de B-3, les accès hors classe
- Code (état **avant** correctif, cycle 2 à venir) : [`src/history.cpp`](../../src/history.cpp) (l. 290, 294, 500, 531, 581, 586), [`src/logger.cpp`](../../src/logger.cpp) (l. 230, 276-277, 299, 307, 317, 329), [`src/web_routes_data.cpp:618`](../../src/web_routes_data.cpp:618), [`src/constants.h`](../../src/constants.h) (échelle ADR-0058)
- Spec : `specs/features/doing/bug-100-abort-sur-fopen-de-l-historique.md` (cycle 1 : blocs A et B)
- ADR liés : [ADR-0067](0067-garde-503-avant-fopen-handler-prioritaire.md) (**complément côté HTTP — cet ADR en est le pendant côté firmware, et en borne le périmètre**), [ADR-0058](0058-gardes-heap-metrique-8-bits.md) (échelle 8 bits — **7ᵉ barreau ajouté ici, les 6 existants inchangés** ; autorité de fraîcheur, l. 79 et 99), [ADR-0061](0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md) (bascule PSRAM — **sans effet sur le verrou de 92 o**), [ADR-0035](0035-parachute-oom-new-handler.md) (parachute — **structurellement absent de ce chemin**, `malloc` et non `operator new`), [ADR-0033](0033-budget-espace-partition-history.md) (budget partition history), [ADR-0031](0031-disjoncteur-bootloop-history.md) (disjoncteur de boot-loop), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (prédicat pur testable)
