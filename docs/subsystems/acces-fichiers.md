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
  (2026-09-06, **v2.43.0**). Les **14 sites** recensés passent tous par le point
  d'admission unique [`src/fs_guard.cpp`](../../src/fs_guard.cpp), et le contrôle
  [`tools/check_fopen_guard.sh`](../../tools/check_fopen_guard.sh) échoue en CI si
  un autre fichier de `src/` ouvre un fichier lui-même.
  ⚠️ **Correctif NON validé sur matériel** : la défense est argumentée et testée
  en logique pure, aucun déploiement n'a eu lieu.
- **Décision de conception à lire avant d'ajouter une garde ailleurs** :
  [Piège de placement nº 2](#-piège-de-placement-nº-2--avant-louverture-ne-suffit-pas--il-faut-avant-le-pic-dallocation)
  — une garde va avant le **pic d'allocation** du chemin, pas seulement avant
  l'appel qu'elle protège.
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

## Les 14 sites — état **gardé** (cycle 2 v2.43.0, +1 site en v2.48.1)

> 📋 **Compte réconcilié le 2026-09-08** (lot `bug-099`). Ce document annonçait
> « 13 sites » en quatre endroits et « 14 sites » en trois autres, alors que le
> décompte par classe donnait **10 (A) + 3 (B) = 13**. Le compte est désormais
> **recompté depuis les deux tableaux ci-dessous**, et il vaut **14** :
> **10 (A) + 4 (B)**. Toute mention de « 13 » ou « 14 » ailleurs dans le dépôt
> qui n'est pas explicitement datée renvoie à un état antérieur.

Petite histoire du compte, parce qu'il a bougé trois fois :

| Étape | Total | Détail |
|---|---|---|
| Recensement du cycle 1 de `bug-100` | **14** | dont l'`exists()` de `history.cpp`, le site qu'aucun `grep` ne trouvait |
| Cycle 2 (v2.43.0) | **13** | le 14ᵉ a été **supprimé**, pas gardé (cf. plus bas) |
| `bug-099` (v2.48.1) | **14** | la sonde de `Logger::reconcileLogTmp()` s'ajoute en **classe B** |

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
quand l'opération **n'est pas atomique** (cf. les deux pièges de placement plus
bas) ou quand elle enchaîne plusieurs ouvertures indissociables.

### Classe A — 10 ouvertures, seuil 8 192

| # | Site | Tâche(s) appelante(s) **réelles** | Garde | Verdict |
|---|---|---|---|---|
| 1 | [`history.cpp:620`](../../src/history.cpp:620) — `open(path,"r")`, taille de l'ancien fichier | `loopTask` (`consolidateData()`) **et** `async_tcp` (`POST /history/import`) | admise en 534 **et** 596 | **REPORTÉE** (retentative en 5 min) |
| 2 | [`history.cpp:651`](../../src/history.cpp:651) — `open(path,"w")`, écriture | idem #1 | idem | **REPORTÉE** (5 min) |
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
| 11 | [`history.cpp:343`](../../src/history.cpp:343) — `open("/.fscheck","w")` | test d'intégrité write/read post-montage | admise en 317 | **TRIPWIRE** → `historyEnabled = false` pour ce boot |
| 12 | [`history.cpp:347`](../../src/history.cpp:347) — `open("/.fscheck","r")` | relecture du test | admise en 317 | idem |
| 13 | [`history.cpp:716`](../../src/history.cpp:716) — `open(path,"r")`, chargement | désérialisation de l'historique | admise en 703 | **TRIPWIRE** → `historyEnabled = false` pour ce boot |
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

Le tripwire de [`history.cpp:316`](../../src/history.cpp:316) **incrémente son
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

### ⚠️ Piège de placement nº 1 : `saveToFile()` n'est pas atomique

Le `remove(path)` **précède** l'ouverture en écriture (#2). Une garde posée **sur
l'ouverture** convertirait l'`abort()` en **destruction du fichier
d'historique**. Et elle doit précéder la lecture d'`oldSize` (#1), que
`historySaveAdmit()` consomme.

**La garde va en tête de `saveToFile()`** — là où la fonction peut encore ne rien
faire.

### ⚠️ Piège de placement nº 2 : avant l'ouverture ne suffit pas — il faut avant le **pic d'allocation**

> **La leçon la plus transférable du cycle 2.** Elle a coûté un bloquant de revue,
> et elle se généralise à **toute** garde mémoire de ce dépôt.

La première version du correctif posait l'admission en tête de `saveToFile()`,
conformément au piège nº 1 — et c'était **insuffisant**. Entre cette admission et
le `fopen` réellement gardé, la fonction construit le `JsonDocument` de
l'historique et ses `String` : **elle dépense la marge qu'elle vient de valider.**
L'ouverture s'exécutait donc sur une mesure **devenue fausse** — ce qui est
exactement le mode de défaillance que l'asymétrie mesure/décision
([ADR-0070](../adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md) §e)
existe pour interdire : *autoriser* sur une mesure périmée coûte un `abort()`.

D'où une **seconde admission**, posée **après** `measureJson()` — donc après le
pic — et toujours **avant** le `remove()`, là où l'opération peut encore ne rien
faire. Les deux gardes sont nécessaires et non redondantes :

| Admission | Emplacement | Ce qu'elle protège |
|---|---|---|
| 1ʳᵉ | tête de `saveToFile()` ([`history.cpp:532`](../../src/history.cpp:532)) | évite de **payer** la sérialisation en famine ; couvre la non-atomicité |
| 2ᵈᵉ | après `measureJson()` ([`history.cpp:591`](../../src/history.cpp:591)) | garantit que la mesure qui **autorise le `fopen`** est postérieure à la dépense |

**Règle à retenir pour toute garde future** :

> Une garde mémoire doit être placée **avant le pic d'allocation du chemin
> gardé**, pas seulement avant l'appel qu'elle protège. Si du code alloue entre
> la mesure et l'appel, la mesure ne décrit plus l'état dans lequel l'appel
> s'exécutera — et une autorisation périmée ne se paie pas en performance, elle
> se paie en `abort()`.

Le coût de la seconde garde est **une mesure de plus toutes les 5 minutes**.
L'ADR-0070 exige une garde avant la lecture d'`oldSize` ; il n'en interdit pas
une seconde.

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
distinctes, aucune accidentelle *(compte au 2026-09-08 : **14 sites**, dont
**13 non-HTTP** ; le recensement de cycle 1 en comptait 14, le cycle 2 en a gardé
13 et supprimé le 14ᵉ, et `bug-099` en a ajouté un)* :

1. **Les 13 sites non-HTTP ne passent par aucun matcher.** Le firmware les ouvre
   pour son propre compte, hors de toute requête. Une garde posée sur le routage
   HTTP ne peut structurellement pas les voir.
2. **`/download-logs` (#10) est EXEMPT par l'ordre d'enregistrement.**
   `_attachHandler` parcourt `_handlers` **dans l'ordre d'enregistrement** et
   s'arrête au **premier** `filter() && canHandle()`. `/download-logs` fait partie
   des **19 routes `HTTP_GET` déclarées avant** le handler de garde — c'est un
   choix délibéré d'ADR-0067 (garder l'appareil **observable** pendant la famine),
   pas un oubli. **L'ordre d'enregistrement EST la politique d'exemption.**
3. **`POST /history/import` est hors matcher** : la garde est `HTTP_GET`-only.
   Les sites #1 et #2 sont donc atteignables depuis `async_tcp` **sans aucune
   garde**.

> **Chiffre à retenir** : le lot B-3 ferme **un** chemin — le service de fichiers
> statiques — et **zéro** des sites internes. La formule « au moins trois
> chemins » de la spec `bug-080` est remplacée par ce compte exact.
>
> ✅ **Depuis la v2.43.0** (cycle 2 de `bug-100`), les sites internes — **14** à
> ce jour — sont gardés par `fs_guard`, et `/download-logs` — exempt du 503 par l'ordre
> d'enregistrement — est gardé **à l'intérieur** de son handler. L'exemption
> d'ADR-0067 subsiste (l'appareil reste observable en famine) ; ce qui disparaît,
> c'est l'`abort()` qu'elle laissait passer.

---

## Accès fichiers **hors classe** — ils n'allouent pas de `FILE` newlib

Ces API touchent au stockage mais **ne passent pas par `fopen`**, donc **pas par
`lock_init_generic`**. Elles sont **hors du périmètre** d'ADR-0070.

| Famille | Exemples | Pourquoi hors classe |
|---|---|---|
| **`Preferences` / NVS** | [`config.cpp`](../../src/config.cpp) (~20 sites), [`history.cpp:35,43,81,93,148,161,251`](../../src/history.cpp:35) | NVS est un magasin clé-valeur de l'IDF ; `nvs_open` retourne un **handle**, pas un `FILE`. Aucun verrou newlib. |
| **`esp_partition_*`** | [`web_routes_coredump.cpp:10,74,92`](../../src/web_routes_coredump.cpp:10), [`history.cpp:126,214,248,310`](../../src/history.cpp:126) | Accès **bloc** direct à la flash, sous le niveau du VFS. Ni FS monté, ni `FILE`. |
| **`Update`** (OTA) | [`web_routes_ota.cpp`](../../src/web_routes_ota.cpp) | Écrit dans la partition applicative via `esp_ota_*`. Aucun `FILE`. ⚠️ **Mais sa garde heap a son propre défaut** — voir `bug-108`. |
| **`remove()` / `rename()`** | [`history.cpp:300,529,551,1130`](../../src/history.cpp:529), [`logger.cpp`](../../src/logger.cpp) (rotation) | Opèrent sur les métadonnées LittleFS **sans construire de `FILE`**. ⚠️ Ils restent des **points de non-atomicité** (cf. le piège de placement ci-dessus), simplement pas des points d'`abort()`. |

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
- [ADR-0017](../adr/0017-logique-metier-pure-humble-object-testabilite.md) — pourquoi le prédicat vit dans `fopen_guard_logic` et la coquille dans `fs_guard`
- [history.md](history.md) · [logger.md](logger.md) · [web-server.md](web-server.md) · [oom-guard.md](oom-guard.md)
