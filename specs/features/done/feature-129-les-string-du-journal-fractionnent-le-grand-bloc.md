## Métadonnées
- **ID** : feature-129
- **Slug** : les-string-du-journal-fractionnent-le-grand-bloc
- **Type** : bug de conception — la correction touche une structure centrale, elle ne se limite pas à un correctif local
- **Statut** : doing — implémentation, tests (1 874 cas natifs) et revue **terminés** ; P2 `pool-chemistry` levée par la documentation (dernière condition bloquante) ; **reste ouvert** : A2, la fenêtre de mesure sur cible ≥ 30 h, qui seule clôt le lot. Non commité.
- **Priorité** : **haute** — le phénomène est actif, mesuré, et coûte 138 suspensions d'ouverture de fichiers et 14 sauvegardes d'historique sur une seule journée
- **Version cible** : 2.52.1 (`src/version.h`)
- **Dernière mise à jour** : 2026-09-16
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

## ✅ DÉCISION — option retenue et paramétrée (16/09/2026)

Arbitrage rendu par l'utilisateur après dossier d'`architect` et verdict P1 de
`pool-chemistry` (**GO SOUS CONDITIONS**). Les options ci-dessous sont conservées
pour mémoire du raisonnement ; **c'est cette section qui fait foi.**

### Option retenue : B′ — arène circulaire d'octets en RAM interne statique

Deux objets en `.bss`, **pas de PSRAM** :

| Objet | Taille | Rôle |
|---|---|---|
| `sLogArena[kLogArenaBytes]` | 16 384 o | texte des messages, longueurs variables, file circulaire |
| `sLogSlots[kMaxLogEntries]` | 200 × 16 o | descripteurs POD (`millis`, `wallSec`, `offset`, `len`, `level`) |

Conséquences : `log()` n'exécute **plus aucune allocation dynamique** (un `memcpy`
sous le mutex existant) ; `_persistBuffer` est **supprimé** ; la ligne de
`/system.log` est **dérivée** de l'anneau au moment d'écrire, au lieu d'être
stockée en double.

### Constantes arbitrées

| Constante | Valeur | Motif |
|---|---|---|
| `kLogArenaBytes` | **16 384** | 82 o/entrée pour 200 entrées, contre 52 o mesurés. On corrige une pénurie de **contiguïté**, pas une pénurie d'octets : gratter 4 Ko de plus au prix de la marge serait se tromper de problème. |
| `kLogMessageMaxBytes` | **768** | ⚠️ **et non 256** — voir ci-dessous. Ne coûte **aucun octet de `.bss`** : le plafond ne réserve rien, il borne le pire cas de profondeur. |

### ⚠️ Pourquoi 768 et non 256 — une mesure qui a failli tronquer les gardes

Le recomptage d'`architect` (718 sites, littéral agrégé, max 257 o) est
**méthodologiquement faux pour les messages de sécurité** : il agrège les
littéraux *d'un même appel*, or les messages de garde les plus longs sont
construits **avant** l'appel — dans un `char[]` ou par concaténation — et l'appel
ne porte plus qu'un préfixe court (`"[Dosage pH] "`). Les trois plus gros messages
de sécurité du firmware étaient **invisibles au comptage**.

Maximum réel relevé par `pool-chemistry` : **≈ 630 o** (`mqtt_manager.cpp:1614`,
refus de mode pH + `reason`). Huit des dix sites concernés sont des messages de
sécurité chimique.

Deux pièces vérifiées directement dans le code :

- `src/pump_controller.cpp:3615` — `char msg[480]`, précédé d'un commentaire qui
  documente **ce défaut déjà commis puis corrigé** : « Un `msg[248]` coupait à
  "…bande mor" — la bande morte, c'est-à-dire l'ENTRÉE du défaut v2.26.0 que ce
  filet doit rendre traçable, était perdue. » Plafonner à 256 réinfligerait ce
  défaut à tous les sites à la fois.
- `src/config.cpp:379-380` — `critical()` concaténant `phAutoManualReason`, qui
  peut faire 427 o à elle seule. C'est la **seule trace persistante** du fait que
  la régulation pH est fermée au redémarrage, et pourquoi.

Autre tampon relevé : `char msg[448]` en `src/pump_controller.cpp:1615`.

### P-3 : retiré par construction, pas corrigé

`logger.cpp:25` fait `_persistBuffer.reserve(100)` mais `logger.cpp:358` fait
`toWrite.swap(_persistBuffer)` — `std::vector::swap` échange la **capacité**. Le
`reserve` est donc consommé au premier flush puis perdu, et `_persistBuffer`
regrandit géométriquement à chaque cycle. Le commentaire du constructeur affirme
le contraire de ce que fait le code.

**Décision : ne pas le corriger séparément.** `_persistBuffer` disparaît avec
cette option ; corriger deux lignes d'une structure qu'on retire ajouterait un
commit sans rien protéger. À consigner dans l'ADR comme **défaut retiré par
construction**, jamais comptabilisé comme défaut corrigé.

### Pourquoi la PSRAM est écartée

Elle est **supérieure sur un seul axe** : elle rendrait ~30 Ko de RAM interne au
lieu de ~10. Mais la contrainte ici est la **contiguïté**, et sur cet axe les deux
options sont **équivalentes** — toutes deux retirent 100 % des blocs fragmentants
du tas 8 bits. La PSRAM paierait 20 Ko dont on n'a pas besoin, contre trois
risques :

1. **Le danger de cache coupé.** `logger.cpp:10` est explicite : « `flushToDisk()`
   s'exécute SUR LA TÂCHE QUI LOGUE ». Lire les octets d'un message depuis la
   PSRAM pendant une écriture flash transformerait une exposition quantifiée en
   exposition **binaire**. C'est le motif retenu par `pool-chemistry`.
2. **La latence** ×2-4 sur un chemin parcouru à chaque ligne, depuis trois tâches.
3. **Le piège P-5.** `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` n'est pas
   activé dans le sdkconfig épinglé : une réserve PSRAM statique est **impossible**.
   Il faudrait la faire à l'exécution dans `begin()` — et `Logger systemLogger;`
   (`logger.cpp:18`) est un global dont le constructeur tourne **avant**
   `main.cpp:152` (`heap_caps_malloc_extmem_enable`). C'est exactement la cicatrice
   de feature-066 : « le parachute anti-OOM était en PSRAM, donc décoratif ».

### Le coût net, et comment il sera vérifié

`.bss` et tas viennent de la **même DRAM** : figer 19,6 Ko de `.bss` retire
autant au tas *total*, mais rend les ~30 Ko que le journal y occupait. Net attendu :
**≈ −10 Ko**, et surtout **0 bloc mobile** là où il y en avait ~300.

Ce n'est pas une estimation à croire, c'est une **mesure à faire** (livrable A3) :

- `xtensa-esp32-elf-size` avant/après → Δ`.bss` attendu **≈ +19,6 Ko** ;
- `free8` à t = 60 s, même build de diagnostic → Δ attendu **≈ +10 Ko**.

> **Si Δ`.bss` est conforme mais Δ`free8` négatif, l'hypothèse « le pool reloge
> au lieu d'ajouter » est FAUSSE et l'arbitrage doit être rouvert.**

### Conditions bloquantes de `pool-chemistry` (P1)

`embedded-developer` ne code pas tant que C1-C4 ne sont pas intégrées au plan.

| # | Condition | Nature |
|---|---|---|
| **C1** | `kLogMessageMaxBytes = 768` + suite native de non-régression reconstruisant les dix messages de garde pire cas et affirmant `len <= kLogMessageMaxBytes`. Mutation : ramener la constante à 256 doit faire échouer **six des dix** (mesuré en phase B le 16/09, et non huit comme estimé au départ — `fs_guard.cpp:212`, `oom_guard.cpp:931` et `main.cpp:942` sont bornés **structurellement** par de petits tampons de pile fixes ou des littéraux courts, et `sensors.cpp:716` reste sous 256 à son pire cas honnête ; c'est une propriété du firmware, pas un trou du test). | bloquante |
| **C2** | **Marqueur de troncature obligatoire**, en ASCII, compté dans le budget **avant** le clamp UTF-8 (modèle `kAlertTail`, `mqtt_manager.cpp:746`, réservation l. 761-762). Motif de sécurité : un refus d'injection tronqué **sans marqueur se lit comme complet**. | bloquante |
| **C3** | Concurrence du callback : `logger.cpp:82` appelle `cb(entry)` **hors mutex** (anti-deadlock délibéré). Aucune vue non propriétaire ne doit pouvoir être réécrite pendant sa sérialisation. Concerne aussi `getLogEntryByIndex()` (`logger.cpp:176-193`), consommé par `/download-logs` **depuis `async_tcp`**. | bloquante |
| **C4** | Politique **« pas d'enjambement »** de fin d'arène : si la place en queue est < `len`, marquer la queue perdue et repartir à l'offset 0. Gaspillage borné par `kLogMessageMaxBytes`. | bloquante |
| C5 | **Pas de rétention prioritaire par niveau** — casserait l'ordre chronologique dont dépendent `forEachRecentLog` et `logsPageAdmit`, et la durabilité des gardes vient du **flush immédiat** sur `ERROR`/`CRITICAL` (`logger.cpp:85-87`), pas de la profondeur RAM. Exposer `log_entries_current` et `log_arena_bytes_used`. | ADR |
| C6 | Compteur **`log_persist_overrun`** distinct de `log_arena_evictions`. `_persistBuffer` était une FIFO **indépendante** de l'anneau ; la dériver de l'anneau supprime cette indépendance et **inverse une propriété actuelle** — à écrire noir sur blanc. | ADR |
| C7 | **Rattrapage des lignes de démarrage** (aujourd'hui jamais persistées), filtré à `WARNING`/`ERROR`/`CRITICAL` — sinon un boot produit > 8 Ko d'`INFO` et rotationne immédiatement, emportant les lignes de sécurité qu'on vient d'écrire. Après `reconcileLogTmp()`, sans jamais émettre d'`ERROR`/`CRITICAL` sur ce chemin. Bénéficiaire identifié : le `CRITICAL` de `config.cpp:379`. | ADR |

**Contrainte de pile** : aucun tampon de `kLogMessageMaxBytes` sur la pile dans
`log()` ni dans le flush — `async_tcp` a une pile courte. Le `memcpy` va de
`message.c_str()` directement dans l'arène ; le flush écrit en deux temps
(en-tête horodaté dans ~32 o, puis les octets lus directement dans l'arène),
sans jamais reconstituer une ligne de 800 o.

**Clamp UTF-8 : un seul module pur.** Deux implémentations existent déjà —
`clampReasonUtf8()` (`pump_controller.cpp:536-551`) et la boucle de recul de
`publishAlert` (`mqtt_manager.cpp:766`). Ne pas en créer une troisième.

### Ce que le correctif ne change pas

**P-1, confirmé par lecture** (`grep LogEntry src/` ne rend que `logger.*`,
`ws_manager.*`, `web_routes_data.cpp` — **aucune occurrence dans
`mqtt_manager.cpp`**) : `publishAlert()` reçoit la `String` de l'appelant et la
tronque avec sa propre boucle bornée à `kMaxPayloadLen = 128`, indépendante du
journal. **Aucune alerte MQTT n'est tronquée par ce correctif**, et le refus
opposé par `pool-chemistry` en v2.25.0 est **formellement levé**.

### Le risque propre à ce design, à ne pas noyer

Les pointeurs dans l'arène ne sont valides que sous mutex. C'est **le** défaut que
cette option introduit, il est **silencieux**, et il n'est **pas détectable en
test natif** — notamment `o["message"] = ptr` au lieu de `JsonString(ptr, len)`,
ArduinoJson 7 stockant un `const char*` **par référence**, sérialisé après
libération du mutex.

À traiter comme **mutant non couvrable** : documenté, porté par la checklist de
`code-reviewer` et une épreuve on-target sous charge — **jamais compté comme tué**
(ADR-0073).


## Les options, et ce qui est connu de chacune

> ⚠️ **Section historique.** L'arbitrage est rendu — voir la section
> « ✅ DÉCISION » ci-dessus, qui seule fait foi. Ces options sont conservées pour
> garder trace du raisonnement et des motifs de rejet, notamment parce que la
> PSRAM y était présentée comme la piste la plus complète : elle a été écartée
> pour un motif qui n'apparaissait dans aucune de ces descriptions (le cache
> coupé pendant `flushToDisk()`, et le `.bss` PSRAM fermé dans le sdkconfig).

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

### 🆕 E — Supprimer la SECONDE `String` : formater à l'écriture, pas à la journalisation

**Constat de code, non anticipé par la première rédaction** : chaque ligne de
journal alloue **DEUX** `String`, pas une.

```cpp
struct LogEntry { String message; };   // logger.h:22 — stockée dans `logs`,
                                       //   vit jusqu'à éviction (200 entrées)
std::vector<String> _persistBuffer;    // logger.h:46 — ligne FORMATÉE (horodatage
                                       //   + niveau), en attente de flush
```

C'est exactement ce qu'A1 a trouvé au bord du trou : `0x3FFFA3D0` (64 o demandés)
porte le message brut, `0x3FFFA420` (80 o demandés) porte la ligne formatée.
**Les deux, côte à côte, au point de découpe.**

`flushToDisk()` échange `_persistBuffer` sous mutex puis écrit. La ligne formatée
pourrait donc être construite **au moment d'écrire**, dans un tampon `char`
statique réutilisé, au lieu d'être allouée au moment de journaliser et conservée
jusqu'au flush suivant.

✅ **Supprime la moitié des allocations de journal**, sans toucher au stockage.
✅ Aucune PSRAM, aucun pool, aucune structure neuve — le risque est faible.
✅ Ne dépend d'aucune des autres options : **cumulable avec n'importe laquelle**.

⚠️ **Ne suffit pas seule** : la `String` stockée dans `LogEntry` reste allouée
dynamiquement et peut toujours scinder un trou. E réduit la fréquence, elle ne
ferme pas le mécanisme.

⚠️ À vérifier : `_persistBuffer` accumule **entre deux flushs**. Un tampon statique
unique suppose d'écrire ligne par ligne au lieu d'échanger un lot — ce qui
rallonge la section où le fichier est ouvert. `flushToDisk()` s'exécutant **sur la
tâche qui logue** (`logger.cpp:10`), y compris depuis `async_tcp`, l'effet sur la
latence doit être mesuré.

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

- [x] A1 — l'option retenue est implémentée, et **le journal reste fonctionnel** (arène circulaire `.bss`, `log()` sans allocation dynamique, 1 874 cas natifs verts, revue de code faite)
- [ ] A2 — mesure **avant/après** du plus gros bloc contigu sur une fenêtre ≥ 30 h — **reste ouvert**, seul item qui clôt le lot
- [x] A3 — le coût RAM de l'option retenue est **mesuré**, pas estimé (+21 240 o de `.bss`, contre 21 216 prédits)
- [x] D1 — tests natifs de la logique pure extraite (`test/test_native_log_arena/`, `test/test_native_log_arena_integration/`, `test/test_native_log_guard_messages/`, `test/test_native_logger_logic/`)
- [x] D2 — **validation par mutation** de chaque garde (C1 : six mutants sur dix tués, propriété du firmware ; défaut de recouvrement trouvé et corrigé par comparaison à un modèle de référence indépendant)
- [x] C1 — ADR ([ADR-0082](../../../docs/adr/0082-arene-circulaire-du-journal-en-ram-interne-statique.md)), [`docs/subsystems/logger.md`](../../../docs/subsystems/logger.md), [`CHANGELOG.md`](../../../CHANGELOG.md) — **ce lot doc-writer, 16/09**

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
      discriminant, il donne un verdict en 10 minutes au lieu de 30 heures.
      **Reste à exécuter sur cible.**
- [ ] Sur une fenêtre ≥ 30 h, le plus gros bloc contigu ne descend plus sous le
      seuil `kStaticFileMinAllocBytes` (8 192 o) du fait des `String` de journal.
      **Reste à exécuter sur cible — clôt le lot.**
- [ ] Aucune sauvegarde d'historique sautée pour cause de famine sur cette fenêtre.
      **Reste à exécuter sur cible.**
- [ ] Le journal reste complet : aucun message de garde chimique tronqué ou perdu.
      Couvert par construction (politique « pas d'enjambement » C4, marqueur de
      troncature C2, `kLogMessageMaxBytes >= 640` verrouillé par `static_assert`)
      et par les tests natifs ; **confirmation en conditions réelles reportée à
      la fenêtre de mesure sur cible** — laissé non coché par prudence.
- [x] La récursion `systemLogger → broadcastLog → _heapAllowsPush` n'est pas
      rouverte — vérifié par lecture : les six compteurs de l'arène
      (`logArenaTruncations`, `logArenaEvictions`, `logPersistOverruns`,
      `logEntriesCurrent`, `logArenaBytesUsed`, `logArenaOverlapGuardTrips`)
      sont mis à jour par des atomiques `relaxed`, sans aucun appel à
      `systemLogger` dans leur propre chemin — invariant documenté en tête de
      `src/heap_metrics.h` et porté par la revue de code.
- [x] Le coût RAM est mesuré et documenté — +21 240 o de `.bss` mesurés
      (21 216 prédits) ; voir [ADR-0082](../../../docs/adr/0082-arene-circulaire-du-journal-en-ram-interne-statique.md)
      et [`docs/subsystems/logger.md`](../../../docs/subsystems/logger.md#arène-circulaire-du-journal-feature-129).

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

---

## 🏁 CLÔTURE — résultats de la fenêtre de mesure (17/09/2026)

Déployé le 16/09 en **v2.52.1**. Fenêtre de **30,1 h** (uptime 54 → 108 388 s),
136 relevés dans `/root/pool-captures/f129/fenetre.tsv`.

### Objectif atteint : le bloc contigu se stabilise au lieu de s'effondrer

| | v2.51.0 (référence) | v2.52.1 |
|---|---|---|
| Comportement sur 30 h | effondrement en 4 marches entre 20,7 h et 28,2 h | déclin **confiné aux 7,4 premières heures** |
| Remontées | **0 sur 28 relevés** | **1** (+6 144 o à 6,8 h) |
| État final | descente continue | **34 804 o, PLAT de 7,4 h à 30,1 h** (~100 relevés identiques) |
| Marge au plancher `kStaticFileMinAllocBytes` (8 192) | en route vers le refus | **4,2×** |

**Critère 3 (jamais sous 8 192 o sur 30 h) : SATISFAIT.** Le problème
d'exploitation — fichiers statiques refusés, sauvegardes d'historique reportées
— est traité.

### L'arène se comporte comme prévu

191 évictions sur 30 h (cycle normal d'un anneau plein, ~16 310 / 16 384 o
occupés pour ~188 entrées), **0 troncature**, **0 `log_persist_overrun`**, et
surtout **0 `log_arena_overlap_guard_trips`** : la comptabilité a tenu 30 h en
production, sans que le filet ait jamais à se déclencher.

`oom_reserve_internal = true` : le parachute anti-OOM reste en RAM interne
(feature-066 non régressée).

### ⚠️ PRÉDICTION INVALIDÉE — le gain de mémoire libre n'existe pas

Le critère de falsification A3 écrit dans cette spec a été appliqué :

| `free_heap_8bit` à ~5 h d'uptime | v2.52.0 | v2.52.1 |
|---|---|---|
| | 53 568 | **52 664** |

**Le gain annoncé de ~+9 Ko ne s'est pas produit.** La mémoire libre est stable,
très légèrement en dessous.

Cause de l'erreur : le chiffre de ~30 Ko attribué au journal incluait des pics
**transitoires** de `_persistBuffer`, pas un régime permanent. La mesure réelle
donne ~20 Ko (16 310 o de texte + 4 000 o de descripteurs) pour 21,2 Ko figés en
`.bss` — bilan net **zéro**.

**Ce qui tient** : l'argument principal, « le pool RELOGE au lieu d'ajouter ».
Si l'hypothèse avait été fausse dans l'autre sens, `free8` aurait chuté de
21 Ko ; il n'a pas bougé, et `heap_size` a baissé de 21 272 o — exactement le
`.bss` mesuré au build (21 248 o). Le relogement est prouvé ; le bonus annoncé
en plus, non. **Ne pas reconduire ce chiffre de +9 Ko** (ADR-0073 : un chiffre
porte sa méthode, et celui-ci était une estimation présentée comme un attendu).

### Ce qui reste — un SECOND mécanisme, hors périmètre de cette spec

Le bloc a perdu 22 528 o pendant les 7,4 premières heures, en marches de
4 096 / 6 144 / 6 144 / **+6 144** / 12 288, puis plus rien.

**Ce n'est plus le journal** : `log()` n'alloue plus rien. La mesure de départ ne
pouvait pas distinguer ce mécanisme du premier, qui le masquait.

Hypothèse testée et **RÉFUTÉE** : la rotation du journal n'alloue pas de tampon
de 6 144 o — elle copie par blocs de 512 (`logger.cpp:709-716`).

Piste pour une spec de suivi : les marches tombent dans les 7 premières heures,
donc un recensement du tas (ADR-0081) toutes les 20 min sur une nuit suffit à
les attribuer. ⚠️ Cadence **jamais** sous 60 s — à 17 s l'instrument redevient la
cause (effet d'observateur, condition X4).

**Priorité réelle : basse.** Un plateau à 34 Ko avec 4,2× de marge est une
curiosité, pas un problème d'exploitation.

### Deux lacunes du dispositif de mesure, à ne pas prendre pour des résultats

- **Critères 4 et 5 NON MESURÉS** (nombre de trous ≤ +40, `allocatedBlocks`
  < +40). Ils exigent le recensement du tas ; le suivi n'a interrogé que
  `/get-system-info`. Erreur de montage de la fenêtre, pas un résultat manquant
  de l'appareil.
- **Critère 1 non formellement évalué** : l'instant de la première sauvegarde
  d'historique n'a jamais pu être confirmé dans le journal. La chute unique de
  4 096 o à t=84 s est compatible avec le critère (≤ 4 096), mais rien ne prouve
  qu'elle corresponde à cet événement.

### Enseignement de méthode, au-delà de ce lot

Un défaut de **recouvrement silencieux** a été livré en phase C puis intercepté
avant production (détail et post-mortem dans ADR-0082 et dans l'en-tête de
`test/test_native_log_arena_integration/`). Trois leçons :

1. Les cinq compteurs de l'arène restaient **cohérents** pendant la corruption —
   l'instrumentation d'un lot ne surveille pas le lot lui-même.
2. Un test unitaire de la phase B attendait une valeur **dérivée de la formule
   fautive** : il confirmait le défaut au lieu de le révéler, et restait vert.
   **Ne jamais dériver une attente de ce que le code calcule.**
3. Seule une comparaison à un **modèle de référence indépendant** pouvait
   trancher : le défaut n'apparaît qu'après un état accumulé.

**Statut : CLOS.** Objectif atteint, une prédiction secondaire invalidée et
consignée, un second mécanisme identifié comme existant mais non attribué.
