# Accès fichiers du firmware — recensement des `fopen`

> **Pourquoi cette fiche existe.** Sur ESP32, `fopen()` alloue un verrou récursif
> newlib. Si ce `malloc` échoue, `lock_init_generic` appelle **`abort()`** : il
> n'y a **aucun code de retour à tester**. Deux crashs de production
> ([29/08](../incidents/2026-08-29-panic-fopen-page-html.txt),
> [05/09](../incidents/2026-09-05-panic-fopen-historique.txt)) viennent de là.
> La seule défense est de **ne pas entrer dans `fopen`** — donc de savoir
> **exactement où le firmware l'appelle**.
>
> **Vous ajoutez une ouverture de fichier ?** Lisez
> [ADR-0070](../adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md),
> classez votre site (A ou B), et **ajoutez-le au tableau ci-dessous dans le même
> commit**.

- **Statut du correctif** : **gardes posées** au **cycle 2 de `bug-100`**
  (2026-09-06, **v2.43.0**). Les **13 sites** recensés (depuis feature-131,
  v2.53.0 — était 14, voir [la note dédiée](#le-site-1-a-disparu-feature-131-v2530))
  passent tous par le point d'admission unique
  [`src/fs_guard.cpp`](../../src/fs_guard.cpp), et le contrôle
  [`tools/check_fopen_guard.sh`](../../tools/check_fopen_guard.sh) échoue en CI si
  un autre fichier de `src/` ouvre un fichier lui-même.
  ⚠️ **Correctif NON validé sur matériel** : la défense est argumentée et testée
  en logique pure, aucun déploiement n'a eu lieu.
- **Décision de conception à lire avant d'ajouter une garde ailleurs** :
  [Piège de placement nº 2](#-piège-de-placement-nº-2--retiré-feature-131--le-pic-quil-protégeait-a-disparu)
  — une garde va avant le **pic d'allocation** du chemin, pas seulement avant
  l'appel qu'elle protège (règle générale toujours valide ; son application
  historique à `saveToFile()` de l'historique est retirée depuis feature-131).
- **Décision de référence** : [ADR-0070](../adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md)
- **Décision voisine (côté HTTP)** : [ADR-0067](../adr/0067-garde-503-avant-fopen-handler-prioritaire.md)

---

## ⚠️ Encadré — le site qu'aucun `grep` ne trouve

```cpp
// src/history.cpp — SUPPRIMÉ au cycle 2 (v2.43.0), conservé ici comme leçon de méthode
if (!historyStore->exists(historyFilePath)) { … }
```

**`exists()` OUVRE le fichier.** `VFSImpl::exists()` du framework Arduino-ESP32
ne fait **pas** de `stat()` : il construit un `VFSFileImpl(this, fpath, "r")`,
c'est-à-dire un `fopen` complet, verrou newlib compris.

**Conséquence de méthode** : une recherche par motif textuel (`grep "open("`,
`grep "fopen"`) **ne trouve pas ce site**. Le recensement ci-dessous ne peut donc
pas être régénéré par une commande — c'est la raison pour laquelle il est
maintenu à la main, ici, et pas dans une spec archivée.

**Toute API du framework qui construit un `VFSFileImpl` compte comme une
ouverture**, quel que soit son nom. `openNextFile()` en est une autre — le
contrôle automatique surveille les **trois** (`open`, `exists`, `openNextFile`),
mais un `grep` reste mono-ligne : cf. [ce qu'il ne peut pas fermer](#le-contrôle-automatique--et-ce-quil-ne-peut-pas-fermer).

**Ce site précis n'existe plus** : le cycle 2 l'a **supprimé** au lieu de le
garder — il était strictement redondant avec l'ouverture en lecture qui le
suivait (cf. [Le 14ᵉ site a disparu](#le-14ᵉ-site-a-disparu-il-na-pas-été-gardé) —
il s'agit du 14ᵉ du **recensement de cycle 1**, à ne pas confondre avec le site
**#14 actuel**, ajouté en v2.48.1).

---

## Les 13 sites — état **gardé** (cycle 2 v2.43.0, +1 site en v2.48.1, −1 site en feature-131/v2.53.0)

> 📋 **Compte réconcilié le 2026-09-19** (feature-131). Ce document annonçait
> « 14 sites » depuis `bug-099` (10 A + 4 B). L'écriture atomique de l'historique
> (`.tmp`+`rename`) a fait **disparaître** l'ouverture en lecture qui mesurait la
> taille de l'ancien `/history.json` (`oldSize`) — ce fopen n'a plus lieu d'être,
> puisque l'ancien fichier n'est plus crédité par `historySaveAdmit()`. Le compte
> est désormais **9 (A) + 4 (B) = 13**. Toute mention de « 14 » ailleurs dans le
> dépôt qui n'est pas explicitement datée renvoie à un état antérieur à
> feature-131.

Petite histoire du compte, parce qu'il a bougé quatre fois :

| Étape | Total | Détail |
|---|---|---|
| Recensement du cycle 1 de `bug-100` | **14** | dont l'`exists()` de `history.cpp`, le site qu'aucun `grep` ne trouvait |
| Cycle 2 (v2.43.0) | **13** | le 14ᵉ a été **supprimé**, pas gardé (cf. plus bas) |
| `bug-099` (v2.48.1) | **14** | la sonde de `Logger::reconcileLogTmp()` s'ajoute en **classe B** |
| `feature-131` (v2.53.0) | **13** | le site #1 (lecture de `oldSize`) a **disparu** — écriture atomique `.tmp`+`rename`, plus de crédit de l'ancien fichier (cf. [la note dédiée](#le-site-1-a-disparu-feature-131-v2530)) |

**Classe A** = contention TCP réelle possible → seuil `8192`
(`kStaticFileMinAllocBytes`, 6ᵉ barreau [ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md)).
**Classe B** = sites de boot, absence de contention **prouvée par l'ordre de
`setup()`** → seuil `5120` (`kInternalFopenMinAllocBytes`, 7ᵉ barreau, tripwire).
⚠️ Cette classe **parie** sur « appelé au boot » — lire l'[invariant](#-invariant-de-la-classe-b--le-pari--appelé-au-boot--que-le-site-voisin-refuse-de-faire)
avant d'y ajouter un site ou de déplacer un appelant.

Toute ouverture passe par [`src/fs_guard.cpp`](../../src/fs_guard.cpp) — seul
fichier de `src/` autorisé à appeler `fs::FS::open()`, contrôlé en CI par
[`tools/check_fopen_guard.sh`](../../tools/check_fopen_guard.sh).

Deux façons d'être gardé, et la distinction compte :

| Forme | Ce qu'elle fait |
|---|---|
| `openIfHeapAllows(fs, path, mode, <seuil>)` | **mesure** `heapLargestFreeBlock8Bit()` puis décide |
| `fsGuardAdmit(<seuil>)` … `openIfHeapAllows(…, kFsGuardAlreadyAdmitted)` | **une** mesure au point d'admission, `N` ouvertures ensuite **sans re-mesurer** |

La seconde forme n'est pas une optimisation : c'est la seule qui soit correcte
quand une opération enchaîne plusieurs ouvertures indissociables (rotation du
journal), ou quand l'opération elle-même n'est pas instantanée et doit être
décidée **avant** de commencer à agir (cf. le [piège de placement nº 2](#-piège-de-placement-nº-2--retiré-feature-131--le-pic-quil-protégeait-a-disparu)
ci-dessous). ⚠️ **`saveToFile()` de l'historique EST atomique depuis
feature-131** (`.tmp`+`rename`, v2.53.0) — voir la note qui remplace l'ancien
« piège de placement nº 1 », plus bas.

### Classe A — 9 ouvertures, seuil 8 192

> ⚠️ **Le site #1 a disparu** (feature-131, v2.53.0) : voir
> [la note dédiée](#le-site-1-a-disparu-feature-131-v2530) plus bas — le
> numéro **n'est pas réattribué**, par cohérence avec le traitement du 14ᵉ site
> disparu au cycle 2.

| # | Site | Tâche(s) appelante(s) **réelles** | Garde | Verdict |
|---|---|---|---|---|
| ~~1~~ | ~~`open(path,"r")`, taille de l'ancien fichier — ligne 620 en v2.52.2, **numéro non significatif aujourd'hui** (code supprimé, les décalages ultérieurs du fichier ne s'appliquent à rien)~~ | — | — | **SUPPRIMÉ** (feature-131 : plus de crédit de l'ancien fichier, donc plus besoin de mesurer sa taille) |
| 2 | [`history.cpp:738`](../../src/history.cpp:738) — `open("/history.json.tmp","w")`, écriture atomique | `loopTask` (`consolidateData()`) **et** `async_tcp` (`POST /history/import`/`appendData()`) | admise en 689 | **REPORTÉE** (retentative en 5 min) |
| 3 | [`logger.cpp:321`](../../src/logger.cpp:321) — `open("/system.log","a")`, marqueur `--- DÉMARRAGE ---` | tâche appelant `setPersistenceFs()` — `loopTask` aujourd'hui, **rien ne le garantit** | mesure propre (8 192) | **REPORTÉE** (marqueur perdu, jamais le journal) |
| 4 | [`logger.cpp:404`](../../src/logger.cpp:404) — `open("/system.log","a")` | **la tâche qui logue** — `flushToDisk()` est synchrone sur `ERROR`/`CRITICAL` | admise en 379 | **REPORTÉE** (buffer réinséré) |
| 5 | [`logger.cpp:405`](../../src/logger.cpp:405) — repli `open(…,"w")` | idem #4 | admise en 379 | **REPORTÉE** |
| 6 | [`logger.cpp:427`](../../src/logger.cpp:427) — `open(…,"r")`, relecture de taille | idem #4 | admise en 379 | **REPORTÉE** |
| 7 | [`logger.cpp:441`](../../src/logger.cpp:441) — `open(…,"r")`, source de rotation | idem #4 | admise en 379 | **REPORTÉE** |
| 8 | [`logger.cpp:451`](../../src/logger.cpp:451) — `open("/system.log.tmp","w")` | idem #4 | admise en 379 | **REPORTÉE** |
| 9 | [`logger.cpp:463`](../../src/logger.cpp:463) — `open("/system.log.tmp","r")`, contrôle non vide | idem #4 | admise en 379 | **REPORTÉE** |
| 10 | [`web_routes_data.cpp:804`](../../src/web_routes_data.cpp:804) — `open("/system.log","r")`, phase `kFileOpen` de `GET /download-logs` | **`async_tcp`** | mesure propre (8 192) | **DÉGRADE** : repli `kRamHeader` (journal RAM seul) + **troncature annoncée dans le fichier téléchargé** |

> ⚠️ **Le journal entier est classe A, y compris #3.** Ce n'est pas une
> précaution : `flushToDisk()` s'exécute **sur la tâche qui a émis le log**. Une
> ligne `ERROR` émise depuis un handler `async_tcp` ouvre le fichier **pendant**
> qu'une réponse est en vol. Il n'existe pas de « tâche du logger ».
>
> **Une mesure par flush, six ouvertures ensuite.** L'admission de
> [`logger.cpp:379`](../../src/logger.cpp:379) couvre les sites #4 à #9 : la
> rotation complète du journal ne paie **qu'un** parcours du tas, pas six —
> et le parcours TLSF coûte d'autant plus cher que le tas est fragmenté,
> c'est-à-dire exactement dans le régime où la garde sert.

### Classe B — 4 ouvertures, seuil 5 120 (tripwire de boot)

**Preuve d'absence de contention** : `history.begin()` est appelé à
[`main.cpp:271`](../../src/main.cpp:271), `mqttManager.begin()` à
[`main.cpp:301`](../../src/main.cpp:301), `webServer.begin()` à
[`main.cpp:317`](../../src/main.cpp:317). **Aucune pile applicative réseau n'est
démarrée** quand ces ouvertures ont lieu — acquis **par construction**, pas par
observation.

| # | Site | Rôle | Garde | Verdict |
|---|---|---|---|---|
| 11 | [`history.cpp:424`](../../src/history.cpp:424) — `open("/.fscheck","w")` | test d'intégrité write/read post-montage | admise en 398 | **TRIPWIRE** → `historyEnabled = false` pour ce boot |
| 12 | [`history.cpp:428`](../../src/history.cpp:428) — `open("/.fscheck","r")` | relecture du test | admise en 398 | idem |
| 13 | [`history.cpp:854`](../../src/history.cpp:854) — `open(path,"r")`, chargement | désérialisation de l'historique (lecture en flux, feature-131) | admise en 841 | **TRIPWIRE** → `historyEnabled = false` pour ce boot |
| 14 | [`logger.cpp:253`](../../src/logger.cpp:253) — `exists("/system.log")` (**sonde**, `existsIfHeapAllows`) de `Logger::reconcileLogTmp()` | savoir si le journal courant a survécu à la fenêtre `remove`/`rename` de la rotation (`bug-099`, v2.48.1) | admise en 248 — **une** admission pour la sonde **et** le `rename`/`remove` qui suit | **ABSTENTION** : ni promotion, ni effacement, **muette** ([ADR-0076](../adr/0076-reconciliation-du-tmp-de-rotation-au-boot.md)) |

> **Pourquoi le site #14 est en classe B et pas A** — alors que les sites #3 à #9
> du **même fichier** sont en classe A. Le seuil 8 192 ferait s'abstenir dans
> toute la bande **5 120–8 192** où le filesystem est sain, et **au boot « plus
> tard » n'existe pas** : une abstention ne se rejoue pas dans 10 minutes comme
> un flush — l'orphelin resterait à vie, et dans l'état « journal absent » c'est
> la trace de la coupure qu'on perdrait. Ce site **ne sonde pas le `.tmp`** : sa
> présence se déduit du code de retour du `rename`/`remove`, ce qui évite un
> **second** `fopen` sans rien apprendre de moins.

> **« Reporter » n'a pas de sens au boot** : il n'y a pas de plus tard. Sous
> 5 120 o, la classe B bascule sur `historyEnabled = false` pour ce boot — état
> déjà supporté et déjà journalisé — au lieu d'`abort()`. Entre 4 096
> (auto-guérison) et 5 120, **l'appareil démarre sans historique persistant mais
> en dosant**.

#### Le tripwire est observable **durablement**, et son WARN ne l'est que **bornément**

Le tripwire de [`history.cpp:398`](../../src/history.cpp:398) **incrémente son
propre refus** (`hdef`) : c'est la dégradation la plus sévère du lot, elle doit
être visible dans le canal **retenu** `<base>/diagnostic`, qui survit à un
reboot. Le WARN qui l'accompagne, lui, n'est durable que de façon **bornée** :
`_persistBuffer` plafonne à **100 entrées en éviction FIFO**, donc sous famine
prolongée la ligne de boot sort par le haut. **Le compteur est l'observabilité,
la ligne de journal est un confort.**

Corollaire de conception : `setPersistenceFs()` **arme inconditionnellement**
(`_persistEnabled = true` avant l'ouverture gardée du marqueur). Sous le
tripwire, seul le **marqueur de démarrage** se perd — pas le journal du boot.
L'observabilité ne doit pas dépendre du sous-système qu'elle garde.

### ⚠️ Invariant de la classe B — le pari « appelé au boot » que le site voisin refuse de faire

**À lire avant de déplacer un appelant.** La classe B ne repose pas sur une
mesure : elle repose sur une **affirmation d'ordonnancement** — « ce code
s'exécute avant que la pile réseau applicative ne démarre ». Tant qu'elle est
vraie, le seuil 5 120 offre une marge ×5,9 ; dès qu'elle est fausse, le site est
sous-gardé et l'`abort()` newlib redevient atteignable.

Or **le site #3 refuse explicitement ce pari** — l'ouverture du marqueur de
démarrage, à **trois lignes** du site #14, est classée **A** au motif que
`setPersistenceFs()` est appelé depuis `loopTask` « aujourd'hui, mais **rien ne
le garantit** ». Les deux sites voisins n'ont donc pas la même classe, et c'est
**volontaire** : #3 peut se contenter de perdre un marqueur, #14 ne peut pas se
permettre de s'abstenir (au boot, l'abstention est définitive). Cette asymétrie
est un compromis assumé, pas une incohérence — mais elle est **fragile**.

> **Invariant à préserver** : tout site de classe B est atteint **uniquement**
> depuis `setup()`, avant `mqttManager.begin()` et `webServer.begin()`.
>
> **Ce qu'un futur appelant casse s'il l'enfreint** : appeler
> `setPersistenceFs()` depuis une route HTTP, une tâche MQTT ou un rechargement
> de configuration invalide la preuve d'absence de contention, rend le seuil
> 5 120 **insuffisant** pour le site #14, et — indépendamment de la mémoire —
> **détruit le journal** si l'ordre interne de `setPersistenceFs()` a aussi
> bougé ([ADR-0076](../adr/0076-reconciliation-du-tmp-de-rotation-au-boot.md),
> contrainte d'ordre). Le site devrait alors repasser en classe A, en acceptant
> l'abstention dans la bande 5 120–8 192.

### Le 14ᵉ site a **disparu**, il n'a pas été gardé

`history.cpp` appelait `historyStore->exists(historyFilePath)` **juste avant**
d'ouvrir le même fichier en lecture. Cet `exists()` étant un `fopen` complet
(cf. l'encadré en tête), il était **strictement redondant** avec l'ouverture qui
le suivait, tout en **doublant** l'exposition à l'`abort()`. Le cycle 2 l'a
**supprimé** plutôt que gardé : une garde de plus aurait protégé un appel qui
n'avait aucune raison d'exister.

⚠️ **Perte de distinction assumée** : sans cet `exists()`, `loadFromFile()` ne
peut plus distinguer « fichier absent » (cas nominal au premier boot) de
« fichier illisible ». L'API `fs::` n'offre **aucun `stat()`** pour les
départager sans payer une seconde ouverture — c'est-à-dire sans réintroduire le
site qu'on vient de fermer. Le libellé est donc rendu **explicitement non
discriminant** :

```
Aucun historique exploitable (fichier absent ou illisible)
```

Niveau `INFO`, jamais `ERROR` : l'absence au premier boot est le cas nominal, et
un `ERROR` déclencherait le flush synchrone donc un `fopen`.

### Le site #1 a disparu (feature-131, v2.53.0)

`history.cpp` ouvrait `/history.json` en lecture **juste avant** de le
supprimer et de le réécrire, pour connaître `oldSize` — l'octet que
`historySaveAdmit()` créditait à l'ancien fichier ([ADR-0033](../adr/0033-budget-espace-partition-history.md)).
Depuis l'écriture atomique `.tmp`+`rename` (feature-131), l'ancien fichier
n'est **plus jamais crédité** (il coexiste avec le `.tmp` jusqu'au `rename`
final, donc son espace n'est jamais récupérable pendant l'écriture) —
`historySaveAdmit()` a perdu son paramètre `oldFileBytes`, et cette lecture
n'a plus de raison d'exister. Elle a été **supprimée**, comme le 14ᵉ site l'a
été au cycle 2 : le numéro **n'est pas réattribué**, il reste barré dans le
tableau ci-dessus pour que le compte reste traçable dans le temps. Détail de
la décision : [ADR-0083](../adr/0083-ecriture-atomique-historique-et-contrat-de-format-persiste.md),
[history.md](history.md#atomicité-tmp--rename-décision-utilisateur-2026-09-19).

### ⚠️ Piège de placement nº 1 : RETIRÉ (feature-131) — `saveToFile()` est désormais atomique

> **Historique — ne décrit plus le comportement actuel depuis la v2.53.0.**
> Conservé pour la traçabilité : c'est le piège que la garde en tête de
> `saveToFile()` a été posée pour éviter, et le raisonnement reste correct
> pour tout code **futur** qui enchaînerait un `remove()` puis une réécriture.

Jusqu'à la v2.52.x, `remove(path)` **précédait** l'ouverture en écriture :
une garde posée **sur l'ouverture** aurait converti l'`abort()` en
**destruction du fichier d'historique**, et elle devait précéder la lecture
d'`oldSize`, que `historySaveAdmit()` consommait alors. D'où la garde en
**tête** de `saveToFile()` — là où la fonction pouvait encore ne rien faire.

Depuis feature-131 (v2.53.0), `saveToFile()` écrit `/history.json.tmp` puis
**seul un `rename()`** le fait remplacer l'ancien : il n'y a plus de fenêtre
`remove()`→écriture à protéger de cette façon. La garde reste en tête de
fonction (elle protège désormais la fenêtre de **coexistence** `.tmp`/ancien,
pas instantanée non plus), mais pour une raison différente — voir
[history.md](history.md#atomicité-tmp--rename-décision-utilisateur-2026-09-19).

### ⚠️ Piège de placement nº 2 : RETIRÉ (feature-131) — le pic qu'il protégeait a disparu

> **Historique — la seconde admission qu'il documentait n'existe plus depuis
> la v2.53.0.** La **règle générale** ci-dessous reste vraie et transférable à
> toute garde mémoire future de ce dépôt ; c'est son **application à
> `saveToFile()`** qui est retirée.

Jusqu'à la v2.52.x, la première version du correctif posait l'admission en
tête de `saveToFile()`, conformément au piège nº 1 — et c'était
**insuffisant** : entre cette admission et le `fopen`, la fonction construisait
le `JsonDocument` complet de l'historique et ses `String` de sérialisation,
**dépensant la marge qu'elle venait de valider**. L'ouverture s'exécutait donc
sur une mesure **devenue fausse** — exactement le mode de défaillance que
l'asymétrie mesure/décision
([ADR-0070](../adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md) §e)
existe pour interdire. D'où une **seconde admission**, posée après
`measureJson()`, dont l'amendement du 2026-09-06 à ADR-0070 §d gardait la
trace.

**Depuis feature-131, ce pic n'existe plus** : l'écriture en flux
(`emitPersistedJson()`, un puits compteur puis un puits fichier) n'alloue
qu'un point à la fois (128 o) plus un tampon de 512 o, tous deux sur **pile** —
aucun tas engagé entre les deux passes. La seconde admission a donc été
**retirée**, `saveToFile()` n'en pose plus qu'une. Détail : [history.md § Une
admission unique](history.md#une-admission-unique-dans-savetofile-feature-131),
[ADR-0083](../adr/0083-ecriture-atomique-historique-et-contrat-de-format-persiste.md).

**Règle à retenir pour toute garde future** (celle-ci ne change pas) :

> Une garde mémoire doit être placée **avant le pic d'allocation du chemin
> gardé**, pas seulement avant l'appel qu'elle protège. Si du code alloue entre
> la mesure et l'appel, la mesure ne décrit plus l'état dans lequel l'appel
> s'exécutera — et une autorisation périmée ne se paie pas en performance, elle
> se paie en `abort()`.

### La mesure sous mutex tenu est **acceptée**, et c'est un constat mesuré

`saveToFile()` est appelée avec `_mutex` tenu depuis `consolidateData()` (chemin
des 5 min) **et** depuis `importData()` : ce n'est pas un cas de bord évitable.
Trois arguments rendent l'appel sûr :

1. **Ordres de grandeur** — un parcours TLSF coûte ~10² µs ; l'écriture LittleFS
   déjà faite **sous ce même mutex** coûte 10¹-10² ms. Deux à trois ordres de
   grandeur d'écart : la mesure ne change pas le profil de contention du verrou.
2. **Nature du coût** — le coût réel est « interruptions coupées », et il est
   **identique** avec ou sans mutex tenu. Relâcher le mutex n'économiserait rien.
3. **Pas d'inversion possible** — le verrou du tas est **terminal** : rien n'est
   pris en le tenant, aucun cycle d'attente ne peut se former.

Ce qui reste interdit : appeler depuis une **ISR**, ou depuis le **chemin d'échec
d'allocation** (`heap_metrics.h` y réserve l'entonnoir borné d'`oom_guard.cpp`).

---

### Réexamen du seuil `kStaticFileMinAllocBytes`, feature-132 (2026-09-20) — il ne change pas

feature-132 a rouvert le seuil **après feature-131**, pour vérifier si la
disparition du pic d'allocation de l'historique le rendait surdimensionné.
**Décision : `kStaticFileMinAllocBytes` reste à 8 192.** Deux raisons, dans
l'ordre de force :

1. **La prémisse était fausse.** Le bloc de dimensionnement
   (`constants.h:445-464`) ne contient **aucun terme d'historique** : verrou
   récursif newlib (92 o), expansion du pool `FILE` (428 o),
   `esp_littlefs_open` (~190 o), `AsyncFileResponse` (156 o), chemins (~100 o),
   pbufs lwip (1 622 o pièce) — besoin dur **874 o**, **4 118 o** avec deux
   pbufs en vol. feature-131 a supprimé un **concurrent** sur le même tas, pas
   un **terme** de cette somme.
2. **La garde ne refuse rien.** Campagne du **2026-09-20**, lecture seule sur
   l'appareil en production (v2.53.0, 192.168.1.48), qui dose pendant la
   mesure — méthode : rafale parallèle des 9 assets d'`index.html`, encadrée de
   lectures `GET /get-system-info?hf=1`, plancher lu sur `hf.i8.wb`
   (`fetch_min` glissant, fenêtre 30 min) : **19 rafales, 171 requêtes d'assets
   + 38 lectures de sonde = 209 requêtes HTTP, toutes en 200, zéro 503**.
   Plancher glissant le plus bas atteint : **19 444 o** (×2,37 du seuil, ×4,72
   du besoin avec contention). Détail complet, y compris la table par fenêtre :
   [feature-132](../../specs/features/done/feature-132-rouvrir-le-seuil-de-la-garde-fichiers-avec-des-mesures.md).

**Le seuil est partagé** — **11 sites sur 6 fichiers** le consomment directement
(`web_server.cpp` ×3 · `history.cpp` ×2 · `logger.cpp` ×2 · `fs_guard.cpp` ×2 ·
`web_helpers.cpp` ×1 · `web_routes_data.cpp` ×1), et `fs_guard.cpp` en dérive un
**second palier implicite à 16 384 o**
(`probeFloorArmed = maxAlloc8 < 2 × kStaticFileMinAllocBytes`).
⚠️ Les **noms de fichiers** sont donnés sans numéro de ligne à dessein : un
renvoi par ligne rancit, et c'est très exactement le défaut que feature-132 a
réparé dans `constants.h`. Un `grep kStaticFileMinAllocBytes src/*.cpp` les
retrouve tous, et ne périme jamais. *(Comptage du 2026-09-20 : `heap_metrics.cpp`
et `web_routes_config.cpp` citent la constante en **commentaire** et ne sont donc
pas des sites.)* Le seul palier
intermédiaire crédible en dessous (7 168) aurait fait glisser ce second palier
à 14 336 **en silence**, sans qu'aucune mesure ne le justifie — c'est ce qui a
achevé de fermer la porte à un ajustement pour un gain non mesuré.

⚠️ **Ce que la mesure du 2026-09-20 NE dit PAS** (à ne pas combler par
supposition) : elle lit ce qui est **disponible** à l'admission, jamais ce que
le service **consomme** — le besoin reste le budget de `constants.h`, établi
par désassemblage ; un `delta = 0` sur `hf.i8.wb` ne veut pas dire « gratuit »,
seulement « n'a pas déplacé le creux déjà enregistré dans la fenêtre » ; elle
ne voit pas le pic intra-requête (la sonde de `web_server.cpp` — celle qui
appelle `heapNoteHttpGetSample` dans le filtre de la garde 503 — échantillonne
à l'**admission** du GET, avant le service) ; ce n'est pas un pire cas annuel
(19 rafales sur deux jours, un appareil, un firmware, un réseau) ; et l'ambiant
n'est pas une constante — pendant cette campagne l'**instantané** vaut
uniformément **34 804 o**, tandis que les **planchers de fenêtre glissante**
vont de 19 444 à 34 804 o (deux grandeurs distinctes, à ne pas confondre), et le
55 284 o relevé au suivi de feature-131 vient encore d'une autre fenêtre. Tout chiffre
cité ci-dessus porte sa date (2026-09-20) et sa méthode ; ne pas le recopier
sans revérifier (ADR-0073 — trois compteurs divergents ont circulé le jour même
pour cette campagne avant le recomptage définitif à 19/171).

---

## Observabilité des refus

**Aucun log dans `fs_guard.cpp`, à aucun niveau.** Un `ERROR`/`CRITICAL` y
déclencherait le flush synchrone de `logger.cpp` → `open("/system.log","a")` →
l'`abort()` même que l'on prévient. Trois canaux, dans cet ordre de fiabilité :

| Canal | Où | Durabilité |
|---|---|---|
| **Compteurs** `hdef` / `ldef` / `xdef` | topic **retenu** `<base>/diagnostic` | **survit au reboot** (retain broker) |
| **WARN throttlé 1/h** | `/system.log` | bornée (`_persistBuffer` = 100 entrées, FIFO) |
| **Serial** | console | volatile |

- `hdef` — sauvegardes/chargements d'historique refusés (**≠** `nospc_cnt`,
  « partition pleine » : deux pannes différentes, deux compteurs, deux libellés) ;
- `ldef` — flushs de journal refusés (lignes **conservées**, flush reporté) ;
- `xdef` — exports `/download-logs` servis **tronqués** faute d'ouverture.

Aucun topic neuf, aucun endpoint neuf : trois champs compacts dans un payload
existant.

### Refus servi par le cache : le libellé le dit

`fsGuardLastMaxAlloc8()` ne vaut rien sans `fsGuardLastRefusalMeasured()`. Un
refus servi par le **cache du refus** (asymétrie ADR-0070 §e) n'a mesuré
**aucun** `maxBloc8` ; afficher la dernière valeur connue laisserait croire
qu'elle date de l'instant. Le message dit donc :

```
maxBloc8 non mesure (refus en cache) < 8192 o
```

au lieu d'un chiffre périmé. La distinction est portée par l'API, pas par la
discipline de l'appelant.

### Bornage du journal — machine à états d'épisode

`fsGuardDrain()`, appelé **depuis la loop uniquement**
([`main.cpp:351`](../../src/main.cpp:351)), émet **quelques lignes par épisode,
jamais une par refus** : un journal persistant de 8 Ko a 25-35 min de rétention,
journaliser par refus effacerait la fenêtre forensique de l'épisode même qu'on
cherche à documenter.

| Transition | Niveau | Condition |
|---|---|---|
| `EpisodeStart` | `WARN` | 1ᵉʳ refus drainé (un seul WARN, même pour 200 refus) |
| `Escalate` | alerte MQTT + compteurs retenus | `kInternalFopenEscalateAfterRefusals` = **5** refus, **une fois par épisode** |
| `EpisodeEnd` | `INFO` + `critical()` **différé** | `kInternalFopenEpisodeQuietMs` = **30 s** sans refus **et** mesure **fraîche** ≥ seuil |

La machine est **asymétrique à dessein** : l'ouverture d'épisode se décide sur un
compteur (aucune mesure requise), la **fermeture** exige une mesure fraîche —
sinon on prononcerait une autorisation sans l'avoir mesurée. `kInternalFopenProbeMinIntervalMs`
= **100 ms** est le plancher **dur** entre deux parcours du tas, et il n'est armé
que si le dernier relevé était **sous 2 × seuil** : en régime confortable chaque
décision re-mesure, et le cache ne peut produire **aucun** refus périmé.

### ⚠️ Correctif v2.51.0 (lot B1) — `EpisodeStart` était structurellement inatteignable

**Confirmé et corrigé** — cf. [`docs/incidents/2026-09-10-recidive-fragmentation-v2500.md`](../incidents/2026-09-10-recidive-fragmentation-v2500.md).
`fsGuardAdmit()` posait `sFopenEpisode.active = true` **au point de décision**,
depuis n'importe quelle tâche appelante. `fopenGuardDrain()` (appelé **depuis la
loop uniquement**) n'initialisait `episodeStartMs`/`deferralsAtStart` que sous
`if (!st.active)`. Or `active` était **toujours déjà vrai** quand le drain
s'exécutait — la branche `EpisodeStart` était donc **inatteignable**, et ce
sur 28,9 h de terrain réel :

- **zéro** ligne « Ouvertures de fichiers refusées » n'a jamais été émise ;
- les trois durées d'épisode journalisées (« 1 refus en **92 279** s », « 2 refus
  en **99 488** s », « 3 refus en **102 792** s ») étaient en réalité l'**uptime**
  (`episodeStartMs` restait à 0) ;
- le compteur de refus rapporté était le **cumul depuis le boot**, pas celui de
  l'épisode (`deferralsAtStart` restait à 0 également).

**Correction — deux drapeaux, pas un.** `FopenGuardEpisode` porte désormais
`active` (cache DU REFUS, inchangé dans son rôle : écrit au point de décision,
depuis n'importe quelle tâche) **et** `reported` (neuf, propriété **exclusive du
drain** : « ai-je déjà émis l'ouverture de cet épisode ? »). `fopenGuardDrain()`
teste `!st.reported`, jamais `!st.active`, pour décider `EpisodeStart` ; il remet
`reported` à `false` à `EpisodeEnd`, sans quoi l'épisode suivant ne serait jamais
détecté.

Le front montant lui-même (« est-ce le premier refus de l'épisode ? ») est
extrait dans `fopenGuardNoteRefusal(FopenGuardEpisode&, uint32_t nowMs)`, appelée
par `fsGuardAdmit()` sur les **deux** chemins de refus (mesuré et servi par le
cache) — c'est elle, et elle seule, qui pose `active`/`episodeStartMs`. La durée
de l'épisode est lue via `fopenGuardEpisodeDurationMs(const FopenGuardEpisode&,
uint32_t nowMs)`, qui remplace le calcul en ligne dans `fs_guard.cpp`. Les deux
fonctions vivent dans le module **pur** [`src/fopen_guard_logic.h`](../../src/fopen_guard_logic.h) / `.cpp`.

> **Pourquoi l'extraction dans le module pur compte, et pas seulement le
> correctif logique.** Une première version corrigeait la règle directement dans
> `src/fs_guard.cpp` — la **coquille impure**, hors de `[env:native]`
> ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md)).
> Deux mutants sur la règle devenaient alors **intestables** (le fichier n'est
> pas compilé côté natif) et deux autres **survivaient par masquage**. Le
> placement du code décidait de ce que la validation par mutation pouvait
> atteindre. `fopenGuardNoteRefusal()` rapatrie la règle là où ADR-0017 la
> protège : dans le module que `pio test -e native` exerce.

> ⚠️ **Un mutant sur `fopenGuardEpisodeDurationMs()` est ÉQUIVALENT, pas tué —
> et ça n'est pas la même chose.** La soustraction `nowMs - st.episodeStartMs`
> est non signée à dessein (survie au repliement de `millis()`, 24,8 j). Le
> mutant « recaster les deux opérandes en signé avant de soustraire » produit,
> une fois le résultat repassé en `uint32_t`, **le même motif de bits modulo
> 2³²** qu'en non signé : aucun test, quelle que soit sa valeur, ne peut le
> distinguer de l'original. Ce n'est **pas un trou de couverture** — c'est un
> mutant qu'aucune suite ne pourra jamais tuer, par construction du calcul.
> Il avait été annoncé « tué » à tort : compter un mutant équivalent comme tué
> fait pourrir le ratio de mutation exactement comme un compteur de tests
> recopié (cf. ADR-0073). Nuance relevée en revue : le recast signé serait
> formellement un comportement indéfini du C++ (dépassement de capacité d'un
> type signé) — l'équivalence constatée ici est **pratique** (même sortie
> observable pour toute paire d'entrées licites), **pas normative**. Les deux
> mutations voisines — opérandes inversés (`episodeStartMs - nowMs`) et
> addition au lieu de soustraction — sont, elles, **réellement tuées** par
> `test_episode_duration_ms_survit_au_repliement_de_millis`.

---

## Le contrôle automatique — et ce qu'il ne peut pas fermer

[`tools/check_fopen_guard.sh`](../../tools/check_fopen_guard.sh), job
`fopen-guard` du workflow **Hygiène du dépôt**
([`.github/workflows/hygiene-specs.yml`](../../.github/workflows/hygiene-specs.yml)) :
échoue si un `.cpp`/`.h` de `src/` **autre que `fs_guard.cpp`** appelle
`open(`, `exists(` ou `openNextFile(`. Les trois sont contrôlés — `exists()` et
`openNextFile()` construisent un `VFSFileImpl`, donc un `fopen` complet.

⛔ **Deux trous que ce script ne peut pas fermer**, à savoir plutôt qu'à croire
couverts :

1. **`grep` travaille ligne à ligne.** Une coupure de ligne après le `->` ou le
   `.` (formatage automatique d'un appel long) **échappe au motif**. Aucun motif
   régulier mono-ligne n'y remédie ; il faudrait un analyseur syntaxique.
2. **Toute API du framework qui construit un `VFSFileImpl` sous un AUTRE nom
   passe.** Le script ferme le chemin qu'un `grep` **peut** voir, pas la
   discipline entière.

C'est pour ces deux raisons que **le recensement ci-dessus reste tenu à la
main** : il n'est pas régénérable par une commande. « L'ordre se voit, la liste
s'oublie » — le script tient l'ordre, ce document tient la liste.

---

## Périmètre exact du lot B-3 (garde 503, [ADR-0067](../adr/0067-garde-503-avant-fopen-handler-prioritaire.md))

### Ce qu'il garde

Un handler prioritaire **`HTTP_GET`** enregistré dans `setupRoutes()` **après
`wsManager.begin(server)`, avant `/login.html`**. Il couvre le **service de
fichiers statiques** : les 7 routes HTML explicites (`/`, `/index.html`,
`/login.html`, `/wifi.html`, `/wizard.html`, `/config`, `/config.html`) et
`serveStatic`. C'est le chemin du PANIC du 29/08.

### Ce qu'il ne garde pas — et pourquoi

**Aucun des sites ci-dessus** — le lot B-3 n'en garde pas un. Trois raisons
distinctes, aucune accidentelle *(compte au 2026-09-19, feature-131 : **13
sites**, dont **12 non-HTTP** ; le recensement de cycle 1 en comptait 14, le
cycle 2 en a gardé 13 et supprimé le 14ᵉ, `bug-099` en a ajouté un — retour à
14 — et feature-131 en a retiré un — retour à 13)* :

1. **Les 12 sites non-HTTP ne passent par aucun matcher.** Le firmware les ouvre
   pour son propre compte, hors de toute requête. Une garde posée sur le routage
   HTTP ne peut structurellement pas les voir.
2. **`/download-logs` (#10) est EXEMPT par l'ordre d'enregistrement.**
   `_attachHandler` parcourt `_handlers` **dans l'ordre d'enregistrement** et
   s'arrête au **premier** `filter() && canHandle()`. `/download-logs` fait partie
   des **19 routes `HTTP_GET` déclarées avant** le handler de garde — c'est un
   choix délibéré d'ADR-0067 (garder l'appareil **observable** pendant la famine),
   pas un oubli. **L'ordre d'enregistrement EST la politique d'exemption.**
3. **`POST /history/import` est hors matcher** : la garde est `HTTP_GET`-only.
   Le site **#2** (`.tmp` de l'écriture atomique de l'historique, l'ancien #1
   ayant disparu — cf. [plus haut](#le-site-1-a-disparu-feature-131-v2530)) est
   donc atteignable depuis `async_tcp` **sans aucune garde HTTP**.

> **Chiffre à retenir** : le lot B-3 ferme **un** chemin — le service de fichiers
> statiques — et **zéro** des sites internes. La formule « au moins trois
> chemins » de la spec `bug-080` est remplacée par ce compte exact.
>
> ✅ **Depuis la v2.43.0** (cycle 2 de `bug-100`), les sites internes — **13** à
> ce jour (feature-131, était 14) — sont gardés par `fs_guard`, et
> `/download-logs` — exempt du 503 par l'ordre d'enregistrement — est gardé
> **à l'intérieur** de son handler. L'exemption d'ADR-0067 subsiste (l'appareil
> reste observable en famine) ; ce qui disparaît, c'est l'`abort()` qu'elle
> laissait passer.

---

## Accès fichiers **hors classe** — ils n'allouent pas de `FILE` newlib

Ces API touchent au stockage mais **ne passent pas par `fopen`**, donc **pas par
`lock_init_generic`**. Elles sont **hors du périmètre** d'ADR-0070.

| Famille | Exemples | Pourquoi hors classe |
|---|---|---|
| **`Preferences` / NVS** | [`config.cpp`](../../src/config.cpp) (~20 sites), [`history.cpp:63,71,109,121,176,189,355`](../../src/history.cpp:63) | NVS est un magasin clé-valeur de l'IDF ; `nvs_open` retourne un **handle**, pas un `FILE`. Aucun verrou newlib. |
| **`esp_partition_*`** | [`web_routes_coredump.cpp:10,74,92`](../../src/web_routes_coredump.cpp:10), [`history.cpp:154,318,352,444`](../../src/history.cpp:154) | Accès **bloc** direct à la flash, sous le niveau du VFS. Ni FS monté, ni `FILE`. |
| **`Update`** (OTA) | [`web_routes_ota.cpp`](../../src/web_routes_ota.cpp) | Écrit dans la partition applicative via `esp_ota_*`. Aucun `FILE`. ⚠️ **Mais sa garde heap a son propre défaut** — voir `bug-108`. |
| **`remove()` / `rename()`** | [`history.cpp:610,783,802,1643,1648`](../../src/history.cpp:802), [`logger.cpp`](../../src/logger.cpp) (rotation) | Opèrent sur les métadonnées LittleFS **sans construire de `FILE`**. ⚠️ **Depuis feature-131 (v2.53.0), le couple `.tmp`+`rename()` de `/history.json` (ligne 802) EST le mécanisme d'atomicité, pas un point de non-atomicité** — voir [le site #1 a disparu](#le-site-1-a-disparu-feature-131-v2530) et [history.md](history.md#atomicité-tmp--rename-décision-utilisateur-2026-09-19). Le `remove()` de rotation du journal (`logger.cpp`), lui, reste un point de non-atomicité documenté par [ADR-0076](../adr/0076-reconciliation-du-tmp-de-rotation-au-boot.md). |

---

## Les deux modules ([ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md))

| Fichier | Nature | Contenu |
|---|---|---|
| [`src/fopen_guard_logic.h`](../../src/fopen_guard_logic.h) / `.cpp` | **pur** (compile en natif) | `fopenGuardShouldDefer()` (frontière **stricte** : `< seuil` refuse), `fopenGuardNeedsFreshMeasure()` (l'asymétrie mesure/décision), `fopenGuardDrain()` (machine d'épisode **asymétrique**), compteur d'escalade |
| [`src/fs_guard.h`](../../src/fs_guard.h) / `.cpp` | **coquille impure** | `openIfHeapAllows()`, `existsIfHeapAllows()`, `fsGuardAdmit()`, `fsGuardDrain()`, compteurs atomiques, mesure `heapLargestFreeBlock8Bit()`, émission des lignes |

Aucun `Arduino.h`, aucun FreeRTOS, aucun `String` dans le module pur — les
soustractions non signées y sont justes **au repliement de `millis()` (24,8 j)**.

Tests : [`test/test_native_fopen_guard/`](../../test/test_native_fopen_guard/),
**34 cas** (1 291 → **1 325 tests** natifs).

### ⚠️ Dette de test assumée — trois mutants non tués

Trois comportements de `history.cpp` ne sont **pas** couverts par mutation, parce
qu'ils vivent dans la coquille, hors du `build_src_filter` de l'environnement
natif :

1. le **tripwire de boot** de `begin()` ;
2. le **tripwire de `loadFromFile()`** ;
3. la **seconde admission** de `saveToFile()` (celle du piège de placement nº 2).

Ce n'est pas un oubli, c'est **la frontière de l'ADR-0017** : la logique pure est
testable, la coquille ne l'est pas. Les prédicats qu'ils invoquent, eux, sont
couverts et tuent leurs mutants.

**Ce qu'il faudrait pour lever la dette** : un double de `fs::FS` en natif (ou
l'extraction des trois séquences en fonctions pures prenant `(admis, taille,
état)` et retournant une action). Aucun des deux n'est gratuit ; aucun n'est
justifié par le risque courant, la décision étant *un booléen déjà testé* dans
les trois cas. À réévaluer si un quatrième site de coquille apparaît.

---

## Voir aussi

- [ADR-0070](../adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md) — point d'admission unique, deux classes de contention, preuve qui clôt le pool `FILE` pré-chauffé
- [ADR-0067](../adr/0067-garde-503-avant-fopen-handler-prioritaire.md) — garde 503 côté HTTP (le pendant de ce document)
- [ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md) — échelle des seuils en métrique 8 bits (**7ᵉ barreau** `5 120` ajouté par ADR-0070)
- [ADR-0076](../adr/0076-reconciliation-du-tmp-de-rotation-au-boot.md) — le site **#14** : réconciliation du `.tmp` de rotation au boot, classe B, contrainte d'ordre
- [ADR-0083](../adr/0083-ecriture-atomique-historique-et-contrat-de-format-persiste.md) — le site **#1 a disparu** : écriture atomique `.tmp`+`rename` de l'historique, plus de crédit de l'ancien fichier
- [feature-132](../../specs/features/done/feature-132-rouvrir-le-seuil-de-la-garde-fichiers-avec-des-mesures.md) — réexamen du seuil `kStaticFileMinAllocBytes` post-feature-131 : mesuré, **inchangé**
- [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md) — pourquoi le prédicat vit dans `fopen_guard_logic` et la coquille dans `fs_guard`
- [history.md](history.md) · [logger.md](logger.md) · [web-server.md](web-server.md) · [oom-guard.md](oom-guard.md)
