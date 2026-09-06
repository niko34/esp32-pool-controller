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

- **Statut du correctif** : stratégie arrêtée au **cycle 1 de `bug-100`**
  (2026-09-06). **Aucune garde n'est encore posée sur ces sites** —
  l'implémentation est le cycle 2. Ce document décrit donc l'**état des lieux**,
  pas un état protégé.
- **Décision de référence** : [ADR-0070](../adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md)
- **Décision voisine (côté HTTP)** : [ADR-0067](../adr/0067-garde-503-avant-fopen-handler-prioritaire.md)

---

## ⚠️ Encadré — le site qu'aucun `grep` ne trouve

```cpp
// src/history.cpp:581
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
ouverture**, quel que soit son nom.

---

## Les 14 sites

**Classe A** = contention TCP réelle possible → seuil `8192`
(`kStaticFileMinAllocBytes`, 6ᵉ barreau [ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md)).
**Classe B** = sites de boot, absence de contention **prouvée par l'ordre de
`setup()`** → seuil `5120` (7ᵉ barreau, tripwire).

### Classe A — 10 sites

| # | Site | Tâche(s) appelante(s) **réelles** | Cadence | Qui attend le résultat | Verdict |
|---|---|---|---|---|---|
| 1 | [`history.cpp:500`](../../src/history.cpp:500) — `open(path,"r")`, taille de l'ancien fichier | `loopTask` (`history.update()` → `consolidateData()`) **et** `async_tcp` (`POST /history/import` → `importData()`) | 5 min (`SAVE_INTERVAL`) ; + à chaque import ; + `migrateLegacyHistory()` (une fois) | personne en périodique ; le client HTTP en import | **REPORTABLE** (5 min) |
| 2 | [`history.cpp:531`](../../src/history.cpp:531) — `open(path,"w")`, écriture | idem #1 | idem #1 | idem #1 | **REPORTABLE** (5 min) |
| 3 | [`logger.cpp:230`](../../src/logger.cpp:230) — `open("/system.log","a")`, marqueur `--- DÉMARRAGE ---` | tâche appelant `setPersistenceFs()` — aujourd'hui `loopTask` au boot, mais **rien ne le garantit** | 1 fois par boot | personne | **REPORTABLE** (perte du marqueur) |
| 4 | [`logger.cpp:276`](../../src/logger.cpp:276) — `open("/system.log","a")` | **la tâche qui logue** — `flushToDisk()` s'exécute en synchrone sur `ERROR`/`CRITICAL` ([`logger.cpp:71-73`](../../src/logger.cpp:71)) : `loopTask`, `mqttTask`, `async_tcp`… | 10 min (`kFlushIntervalMs`) **+ immédiat sur chaque `ERROR`/`CRITICAL`** | personne | **REPORTABLE** (buffer réinséré) |
| 5 | [`logger.cpp:277`](../../src/logger.cpp:277) — repli `open(…,"w")` | idem #4 | uniquement si #4 échoue | personne | **REPORTABLE** |
| 6 | [`logger.cpp:299`](../../src/logger.cpp:299) — `open(…,"r")`, relecture de taille | idem #4 | à chaque flush réussi | personne | **REPORTABLE** |
| 7 | [`logger.cpp:307`](../../src/logger.cpp:307) — `open(…,"r")`, source de rotation | idem #4 | quand `taille > kMaxLogFileBytes` (8 Ko) | personne | **REPORTABLE** |
| 8 | [`logger.cpp:317`](../../src/logger.cpp:317) — `open("/system.log.tmp","w")` | idem #4 | idem #7 | personne | **REPORTABLE** |
| 9 | [`logger.cpp:329`](../../src/logger.cpp:329) — `open("/system.log.tmp","r")`, contrôle non vide | idem #4 | idem #7 | personne | **REPORTABLE** |
| 10 | [`web_routes_data.cpp:618`](../../src/web_routes_data.cpp:618) — `open("/system.log","r")`, phase `kFileOpen` de `GET /download-logs` | **`async_tcp`** | 1 fois par requête | **le client HTTP** | **DOIT ABOUTIR — ou dégrader** : le code sait déjà servir le journal RAM seul si l'ouverture échoue (`→ kRamHeader`) |

> ⚠️ **Le journal entier est classe A, y compris #3.** Ce n'est pas une
> précaution : `flushToDisk()` s'exécute **sur la tâche qui a émis le log**. Une
> ligne `ERROR` émise depuis un handler `async_tcp` ouvre le fichier **pendant**
> qu'une réponse est en vol. Il n'existe pas de « tâche du logger ».

### Classe B — 4 sites, tous dans `history.begin()`

**Preuve d'absence de contention** : `history.begin()` est appelé à
[`main.cpp:270`](../../src/main.cpp:270). `mqttManager.begin()` est à
[`main.cpp:300`](../../src/main.cpp:300), `webServer.begin()` à
[`main.cpp:316`](../../src/main.cpp:316). **Aucune pile applicative réseau n'est
démarrée** quand ces quatre ouvertures ont lieu — c'est acquis **par
construction**, pas par observation.

| # | Site | Rôle | Cadence | Qui attend | Verdict |
|---|---|---|---|---|---|
| 11 | [`history.cpp:290`](../../src/history.cpp:290) — `open("/.fscheck","w")` | test d'intégrité write/read post-montage | 1 fois par boot | `setup()` | **DOIT ABOUTIR** → sous 5 120 : `historyEnabled = false` |
| 12 | [`history.cpp:294`](../../src/history.cpp:294) — `open("/.fscheck","r")` | relecture du test | 1 fois par boot | `setup()` | **DOIT ABOUTIR** → idem |
| 13 | [`history.cpp:581`](../../src/history.cpp:581) — **`exists()` ⇒ ouverture** (cf. encadré) | présence de `/history.json` | 1 fois par boot | `setup()` | **DOIT ABOUTIR** → idem |
| 14 | [`history.cpp:586`](../../src/history.cpp:586) — `open(path,"r")`, chargement | désérialisation de l'historique | 1 fois par boot | `setup()` | **DOIT ABOUTIR** → idem |

> **« Reporter » n'a pas de sens au boot** : il n'y a pas de plus tard. Sous
> 5 120 o, la classe B bascule sur `historyEnabled = false` pour ce boot — état
> déjà supporté et déjà journalisé ([`history.cpp:326-331`](../../src/history.cpp:326))
> — au lieu d'`abort()`. Entre 4 096 (auto-guérison) et 5 120, **l'appareil
> démarre sans historique persistant mais en dosant**.

### ⚠️ Un piège de placement : `saveToFile()` n'est pas atomique

[`history.cpp:529`](../../src/history.cpp:529) — `remove(path)` — **précède**
l'ouverture en écriture (#2). Une garde posée **sur l'ouverture** convertirait
l'`abort()` en **destruction du fichier d'historique**.

Et elle doit précéder la **ligne 500** (#1), pas seulement la 529 :
`historySaveAdmit()` consomme `oldSize`, une taille obtenue **par** le `fopen` de
la ligne 500.

**La garde va en tête de `saveToFile()`** — là où la fonction peut encore ne rien
faire.

---

## Périmètre exact du lot B-3 (garde 503, [ADR-0067](../adr/0067-garde-503-avant-fopen-handler-prioritaire.md))

### Ce qu'il garde

Un handler prioritaire **`HTTP_GET`** enregistré dans `setupRoutes()` **après
`wsManager.begin(server)`, avant `/login.html`**. Il couvre le **service de
fichiers statiques** : les 7 routes HTML explicites (`/`, `/index.html`,
`/login.html`, `/wifi.html`, `/wizard.html`, `/config`, `/config.html`) et
`serveStatic`. C'est le chemin du PANIC du 29/08.

### Ce qu'il ne garde pas — et pourquoi

**Aucun des 14 sites ci-dessus.** Trois raisons distinctes, aucune accidentelle :

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
> statiques — et **zéro** des 14 sites internes. La formule « au moins trois
> chemins » de la spec `bug-080` est remplacée par ce compte exact.

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

## Voir aussi

- [ADR-0070](../adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md) — point d'admission unique, deux classes de contention, preuve qui clôt le pool `FILE` pré-chauffé
- [ADR-0067](../adr/0067-garde-503-avant-fopen-handler-prioritaire.md) — garde 503 côté HTTP (le pendant de ce document)
- [ADR-0058](../adr/0058-gardes-heap-metrique-8-bits.md) — échelle des seuils en métrique 8 bits
- [history.md](history.md) · [logger.md](logger.md) · [web-server.md](web-server.md) · [oom-guard.md](oom-guard.md)
