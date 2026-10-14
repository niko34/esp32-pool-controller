# bug-108 — La garde TLS de l'OTA décide sur une métrique aveugle à la famine qu'elle vise

## Métadonnées
- **ID** : bug-108
- **Slug** : garde-tls-ota-sur-metrique-ira-inclusive
- **Type** : bug
- **Statut** : todo
- **Priorité** : **haute** — une garde de sécurité mémoire est **inopérante dans le cas exact pour lequel elle existe**
- **Version cible** : à déterminer (base 2.42.1)
- **Dernière mise à jour** : 2026-09-06
- **Spec(s) liée(s)** : `bug-100` (cycle 1 — c'est `architect` qui a requalifié le défaut), `bug-080` (lot B), [ADR-0058](../../../docs/adr/0058-gardes-heap-metrique-8-bits.md) (« hors périmètre consigné »), [ADR-0038](../../../docs/adr/0038-metrique-memoire-allouable-8-bits.md) (cause racine de la métrique), [ADR-0036](../../../docs/adr/0036-psram-requise-mise-a-jour-en-ligne.md) (garde TLS d'origine)

> ⚠️ **Numéro** : l'`architect` avait écrit « bug-101 » dans son rapport. **Ce numéro
> est déjà pris** par `bug-101-import-historique-plafonne-a-16-ko.md`. La spec est
> ouverte sous **108**.

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [x] OTA
- [ ] Authentification
- [ ] Historique / persistance
- [ ] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry — aucun code de dosage touché
- [ ] ux-ui-designer
- [x] embedded-developer
- [ ] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

La garde qui refuse un handshake TLS sortant quand la mémoire est trop dégradée
lit `ESP.getFreeHeap()` et `ESP.getMaxAllocHeap()`. Ces deux fonctions renvoient
une métrique **IRAM-inclusive**. Pendant une famine **8 bits** réelle — celle qui
crashe l'appareil — elles peuvent annoncer un `maxAlloc` **largement au-dessus du
seuil** et **autoriser** le handshake. La garde n'est pas imprécise : elle est
**inopérante dans le cas qu'elle vise**.

## Contexte — le constat

[ADR-0058](../../../docs/adr/0058-gardes-heap-metrique-8-bits.md) a basculé les
quatre gardes heap décisionnelles sur `heapLargestFreeBlock8Bit()` et a consigné,
dans « Négatives / dette assumée » :

> « **Hors périmètre consigné** : `web_routes_debug.cpp:55` […] et
> `web_routes_ota.cpp` (garde TLS `kCheckUpdateMin*`, `/get-system-info`) restent
> en métrique **brute** — suivi lot B ou spec dédiée. »

**Cette formulation sous-estime le défaut.** Le cycle 1 de `bug-100` a fait relire
les sites concernés par l'`architect`, qui les requalifie :

> Ce n'est **pas du rapport**, c'est une **garde aveugle**.

Les deux sites :

| Site | Route | Ce que les valeurs alimentent |
|---|---|---|
| [`web_routes_ota.cpp:654-655`](../../../src/web_routes_ota.cpp) | `GET /check-update` | `oomTlsGateReason(psramFree, freeHeap, maxAlloc, …)` |
| [`web_routes_ota.cpp:893-894`](../../../src/web_routes_ota.cpp) | `POST /download-update` | `refuseIfTlsGateClosed(...)` → même prédicat |

`oomTlsGateReason()` ([`src/oom_guard_logic.h:657`](../../../src/oom_guard_logic.h))
est un **prédicat de refus** : son retour non nul provoque une réponse **503**.
Ce ne sont donc pas des valeurs affichées, ce sont les **entrées d'une décision**.

### Pourquoi c'est le pire cas possible

1. `ESP.getMaxAllocHeap()` est **IRAM-inclusive**. Le pool `MALLOC_CAP_8BIT |
   MALLOC_CAP_INTERNAL` — le seul dont mbedTLS a besoin pour ses BIGNUM — peut être
   effondré pendant que la valeur lue reste haute. C'est exactement le mécanisme
   qui a rendu les quatre autres gardes **mortes deux fois** (29/08 et 02/09), et
   qui a motivé ADR-0058.
2. Le sens de l'erreur est le **mauvais** : la garde **autorise** au lieu de
   refuser. Un faux négatif d'une garde qui a été écrite parce que « sur heap
   dégradé l'échec était lent et aléatoire (BIGNUM alloc en cours de handshake) »
   ramène **exactement** le mode de défaillance d'origine, avec sa fenêtre de crash
   d'environ une seconde.
3. C'est un **chemin OTA** : le moment où on l'emprunte est celui où on essaie de
   déployer un correctif. Une garde qui laisse passer là est une garde qui casse
   la voie de sortie.

## Pourquoi le correctif n'était PAS absorbable dans `bug-100`

Trois raisons cumulatives, toutes constatées au cycle 1 :

1. **Ce n'est pas le même chemin.** `bug-100`/[ADR-0070](../../../docs/adr/0070-fopen-interne-helper-unique-et-classes-de-contention.md)
   traite les `fopen` que le firmware exécute pour son propre compte. La garde TLS
   ne touche aucun fichier : elle protège un **handshake réseau**. Aucun code, aucun
   helper, aucun seuil n'est partagé.
2. **Les deux seuils sont exprimés dans l'ANCIENNE métrique.**
   `kCheckUpdateMinFreeHeapBytes = 65536` et `kCheckUpdateMinMaxAllocBytes = 20480`
   ([`constants.h:607-608`](../../../src/constants.h)) ont été calibrés sur des
   relevés `ESP.getFreeHeap()` / `ESP.getMaxAllocHeap()`.
   **Les basculer sans les redimensionner rendrait la garde soit permanente, soit
   muette** :
   - `20 480` en métrique 8 bits est **au-dessus du nominal terrain** relevé par
     ADR-0058 (21 492 o de plateau, et 13 300 o post-B-2) → la garde refuserait
     **tout OTA, en permanence**, y compris sur un appareil sain ;
   - un seuil abaissé « au jugé » pour éviter cela deviendrait **muet** au moment
     de la famine.
   Le redimensionnement demande donc des **relevés propres**, pas une substitution
   d'appel.
3. **`bug-100` cycle 1 ne livre aucun code.** Ajouter un correctif OTA à un cycle
   dont le livrable est un recensement et un ADR aurait mélangé deux périmètres et
   deux niveaux de preuve.

## Comportement attendu

- Les deux sites décident sur la **métrique 8 bits** (`heapLargestFreeBlock8Bit()`
  et l'équivalent « libre » 8 bits), conformément au verrou d'ADR-0058 :
  « **toute nouvelle garde heap lit `heapLargestFreeBlock8Bit()`** ».
- Les seuils `kCheckUpdateMinFreeHeapBytes` / `kCheckUpdateMinMaxAllocBytes` sont
  **redimensionnés sur des relevés**, pas transposés, et leur dérivation est écrite
  dans `constants.h` comme celle des autres barreaux.
- La position des nouveaux seuils **par rapport à l'échelle ADR-0058** est explicite
  (et, si elle doit s'y intercaler, verrouillée par `static_assert` comme le 6ᵉ
  barreau).
- La distinction **rapport** / **décision** est tranchée site par site :
  `/get-system-info` peut légitimement **rapporter** la métrique brute ; une garde
  ne peut pas **décider** dessus. Les deux usages ne doivent plus être confondus.

### Cas limites
- Appareil **sans PSRAM libre** : le motif de refus PSRAM d'ADR-0036 prime et doit
  rester inchangé.
- Refus **permanent** : si le seuil retenu refuse l'OTA en régime nominal, c'est le
  seuil qui est faux, pas le terrain — critère d'échec explicite de la spec.
- **Coût du parcours** : `heapLargestFreeBlock8Bit()` est en O(blocs libres) et son
  coût **croît avec la fragmentation** (caveat consigné par
  [ADR-0067](../../../docs/adr/0067-garde-503-avant-fopen-handler-prioritaire.md)).
  La fréquence est ici bornée par des requêtes OTA — négligeable, mais à constater
  plutôt qu'à supposer.

## Comportement actuel

Les deux sites lisent `ESP.getFreeHeap()` / `ESP.getMaxAllocHeap()` et passent ces
valeurs à `oomTlsGateReason()`. Sous famine 8 bits, le prédicat peut retourner
« autorisé » et le handshake TLS démarre.

## Contraintes techniques
- ESP32, offline first
- Aucun handler AsyncWebServer bloquant > 50 ms
- Chemin OTA : **aucune régression de la voie de secours** (`deploy.sh ota-firmware`,
  `POST /update`)
- Prédicat de décision **pur** ([ADR-0017](../../../docs/adr/0017-logique-metier-pure-humble-object-testabilite.md))
  — `oomTlsGateReason()` l'est déjà, le correctif porte sur ses **entrées** et ses
  **seuils**

## Fichiers potentiellement concernés
- `src/web_routes_ota.cpp` — l. 654-655 et 893-894 : source des trois métriques
- `src/constants.h` — `kCheckUpdateMinFreeHeapBytes`, `kCheckUpdateMinMaxAllocBytes` : redimensionnement + dérivation écrite
- `src/oom_guard_logic.h` — `oomTlsGateReason()` : signature inchangée a priori, **sémantique des paramètres** à re-documenter
- `src/heap_metrics.h` — accesseurs 8 bits (lecture seule)
- `docs/subsystems/ota-manager.md`, `docs/subsystems/oom-guard.md`, `docs/API.md` (codes d'erreur `/check-update`, `/download-update`)

## Livrables — check-list
- [ ] A1 — Bascule des deux sites sur la métrique 8 bits
- [ ] A2 — Redimensionnement **justifié par relevés** des deux seuils, dérivation écrite dans `constants.h`
- [ ] A3 — Position vis-à-vis de l'échelle ADR-0058 explicitée (et `static_assert` si intercalation)
- [ ] A4 — Séparation nette **rapport** (`/get-system-info`, brute admise) / **décision** (8 bits obligatoire)
- [ ] C1 — `docs/subsystems/ota-manager.md` + `docs/subsystems/oom-guard.md` mis à jour
- [ ] C2 — ADR **ou** amendement documenté si la dérivation des seuils contraint durablement (à trancher par `architect`)
- [ ] D1 — Tests natifs du prédicat aux nouveaux seuils + **validation par mutation**

## Critères d'acceptation

- [ ] Aucune décision de la garde TLS ne repose plus sur `ESP.getFreeHeap()` /
      `ESP.getMaxAllocHeap()` — vérifié par lecture du code **et** par grep
- [ ] Les deux seuils sont **redimensionnés**, et leur dérivation est écrite à côté
      d'eux (pas seulement leur valeur)
- [ ] **La garde ne refuse pas en régime nominal** : un OTA sur appareil sain passe
      (relevé terrain à l'appui — un refus permanent est un échec de la spec)
- [ ] **La garde refuse en famine 8 bits simulée** : test natif où la métrique 8
      bits est sous le seuil alors que la métrique brute est au-dessus → refus.
      **C'est le test qui prouve la correction du défaut**, il ne peut pas manquer.
- [ ] Validation par **mutation** : rétablir `ESP.getMaxAllocHeap()` dans le
      prédicat doit **tuer** au moins un test
- [ ] `docs/subsystems/ota-manager.md` et `oom-guard.md` disent quelle métrique
      décide, et pourquoi
- [ ] La dette « hors périmètre consigné » d'ADR-0058 est **explicitement soldée**
      (référencée depuis la doc du correctif)

## ADR attendu ?
- **Probable** : **à trancher par `architect`**
- **Critère(s) candidat(s)** : 1 (si les seuils redimensionnés s'intercalent dans
  l'échelle ADR-0058, ils contraignent les autres barreaux) — sinon **aucun** : la
  bascule de métrique **applique** une décision déjà prise par ADR-0058, elle n'en
  prend pas une nouvelle.
- **Sujet pressenti** : dérivation des seuils TLS en métrique 8 bits, et frontière
  normative **rapport ≠ décision**

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `GET /check-update` — conditions de refus **503** modifiées
- `POST /download-update` — idem

### Champs WebSocket
- aucun

### Topics MQTT
- aucun

### Documentation à produire / mettre à jour
#### Docs vivantes
- `docs/subsystems/ota-manager.md` — métrique et seuils de la garde TLS
- `docs/subsystems/oom-guard.md` — `oomTlsGateReason()` : sémantique des entrées
- `docs/features/` — aucune page UI touchée (le refus 503 est déjà rendu par l'UI existante)

#### Docs transverses
- `docs/API.md` — conditions de `503` sur `/check-update` et `/download-update`
- `CHANGELOG.md` — entrée sous la version cible

#### Docs immuables
- ADR seulement si `architect` retient le critère 1 (cf. ci-dessus)

## Notes

Ce défaut illustre une règle de méthode qui a déjà coûté cher au projet : **une
valeur lue n'est pas un rapport tant qu'on n'a pas vérifié qui la consomme.** Elle
avait été classée « hors périmètre, métrique brute » — donc rangée mentalement avec
les affichages — alors qu'elle **décide** d'un refus. La requalification a demandé
une relecture du chemin d'appel, pas une relecture du site.

---

## ⚠️ MISE À JOUR DU 2026-10-14 — ce qui a changé, et ce qui bloque

### 1. ⛔ BLOQUÉ PAR [`bug-165`](bug-165-recherche-de-mise-a-jour-en-ligne-echoue.md)
`GET /check-update` **échoue systématiquement** sur l'appareil réel (`code: -1`, échec de
connexion). ⚠️ **Corriger la garde d'un chemin qui ne fonctionne pas n'a pas de sens** — et
surtout, le **relevé décisif** (mémoire pendant une session TLS) est **impossible à prendre** : le
chiffrement n'alloue jamais rien. ⇒ **bug-165 passe d'abord.**

### 2. 🚨 L'ARGUMENT CENTRAL DE CETTE SPEC EST PÉRIMÉ — mais le défaut reste

La spec dit que la bascule est bloquée parce que `kCheckUpdateMinMaxAllocBytes = 20 480` serait
*« au-dessus du nominal terrain »* (21 492 o, ADR-0058, septembre).
⛔ **Faux aujourd'hui.** Relevé appareil réel, **2026-10-14, v2.80.0** :

| Grandeur | Valeur |
|---|---|
| `max_alloc_8bit` | **45 044** |
| `free_heap_8bit` | **54 224 – 55 068** |
| `mb` — plancher du plus gros bloc depuis le démarrage (~1 h) | **30 708** |
| `mf` — **plancher de la mémoire libre** depuis le démarrage | **37 024** |
| `psram_free` | 4 174 860 / 4 194 304 |

⇒ Le nominal a **plus que doublé** depuis septembre. ⚠️ **Mais ce n'était pas le bon seuil.**

### 3. 🚨 LE VRAI BLOQUANT EST L'AUTRE SEUIL — que cette spec n'avait pas chiffré

`oomTlsGateReason()` (`src/oom_guard_logic.h:661`) refuse si **l'UN OU L'AUTRE** critère cède :
```cpp
if (freeHeapBytes < minFreeBytes || maxAllocBytes < minMaxAllocBytes) return kOomTlsGateLowMemory;
```

| Critère | Seuil | Valeur 8 bits | Verdict d'une bascule à seuil constant |
|---|---|---|---|
| `kCheckUpdateMinMaxAllocBytes` | 20 480 | 45 044 (pire : 30 708) | ✅ **passe**, 50 % de marge |
| **`kCheckUpdateMinFreeHeapBytes`** | **65 536** | **54 224** (pire : **37 024**) | ❌ **REFUS PERMANENT, en régime SAIN** |

⚠️ **Le projet l'avait DÉJÀ écrit**, mot pour mot, dans
[ADR-0040](../../../docs/adr/0040-pas-de-calibrage-sur-fragmentation-pathologique.md) :
> « `kCheckUpdateMinFreeHeapBytes` 65 536 > `libre8` 46 480 → **mise à jour en ligne définitivement
> refusée**. La bascule sans recalibrage n'est pas une demi-mesure prudente, **c'est la panne**. »

`libre8` est passé de 46 480 à ~54 000 : **l'écart s'est réduit, le signe n'a pas changé.**

⇒ **Le lot reste un redimensionnement, mais d'UN SEUL seuil** (`free`), pas des deux. Périmètre
**nettement plus petit** que ce que cette spec anticipait — à acter par l'`architect`.

### 4. L'amélioration est IDENTIFIÉE, donc fiable — mais pas acquise
Trois lots ont sorti des allocations de la RAM interne : **ADR-0061** (tampon stdio de 4 096 o par
fichier ouvert basculé en PSRAM), **seuil de bascule PSRAM 4095 → 1024**, **feature-129** (le
chemin de journalisation n'alloue plus).
⚠️ **Deux réserves** : (a) le gain dépend d'un `sdkconfig` de **bibliothèques précompilées** —
une montée de plateforme peut le **défaire en silence** ; (b) la PSRAM est un **pool de travail** :
si elle sature, les allocations **retombent en interne**. ⇒ Le seuil doit garder une **marge**,
⛔ pas coller au relevé.

### 5. Corrections de la spec elle-même
- ⛔ **Numéros de ligne FAUX** : les deux sites sont `src/web_routes_ota.cpp:887-888` et
  `:1125-1127`, ⛔ pas 654-655 / 893-894.
- ⛔ **`security-reviewer` N'EST PAS COCHÉ** dans « Agents requis » — **omission** : `CLAUDE.md`
  l'impose sur toute modification de `web_routes_ota.cpp`. Il passe **deux fois** (design, puis diff).
- `src/web_routes_debug.cpp` est **déjà soldé** (bug-080) ⇒ la dette d'ADR-0058 se réduit à
  `web_routes_ota.cpp` seul.
- ⚠️ **Piège de coût** : `heapFree8Bit()` + `heapLargestFreeBlock8Bit()` = **DEUX** parcours TLSF
  interruptions coupées. `heapMetricsSnapshot()` les livre en **UN**. ⇒ C'est celui-là qu'il faut
  appeler.
- Le test sentinelle `test_oom_guard.cpp:311-313` fige `65536u` / `20480u` : il **cassera**, et
  c'est **voulu** — ⛔ à mettre à jour délibérément, **jamais** à neutraliser.

### 6. Les relevés qui manquent encore
1. `mf` / `wf` du masque 8 bits après **≥ 24 h** d'uptime — ⚠️ les chiffres ci-dessus ne couvrent
   qu'**une heure**, après un redémarrage de déploiement.
2. Les mêmes **sous charge web** (interface complète, plusieurs onglets).
3. ⛔ **Les mêmes PENDANT une session TLS réelle** — **IMPOSSIBLE aujourd'hui** (bug-165). *C'est le
   seul régime où la garde sert, et aucun chiffre du dépôt ne le couvre.*
4. Les contextes `cf` / `cb`, qui **nomment ce qui a creusé** le tas.

### 7. Hors périmètre, mais désormais mesurable
`kOtaTlsMinPsramBytes = 1 048 576` **n'a jamais été mesuré sur WROVER réel** (ADR-0036). ⇒ Relevé
du 14/10 : `psram_free` = **4 174 860**, soit **4 fois** le seuil. Spec de suivi possible.
