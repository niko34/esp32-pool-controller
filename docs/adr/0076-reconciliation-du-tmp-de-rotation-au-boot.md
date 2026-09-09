# ADR-0076 — Le `.tmp` de rotation est réconcilié au démarrage, avant toute ouverture du journal

- **Statut** : Accepté
- **Date** : 2026-09-08
- **Décideurs** : `architect`, `embedded-developer`, `test-engineer`, `code-reviewer`
- **Spec(s) liée(s)** : [`bug-099`](../../specs/features/done/bug-099-tmp-de-rotation-jamais-nettoye-au-boot.md)

## Contexte

La rotation de `/system.log` ([`logger.cpp`](../../src/logger.cpp), `flushToDisk()`)
recopie les `kRotateKeepBytes` derniers octets dans `/system.log.tmp`, puis
**enchaîne deux opérations LittleFS distinctes** :

```cpp
if (tmpOk) {                                   // taille du .tmp > 0, après close()
  _persistFs->remove(kSystemLogPath);          // (1)
  _persistFs->rename(kSystemLogTmpPath, kSystemLogPath);  // (2)
}
```

Elles ne sont **pas atomiques entre elles**. Une coupure secteur entre (1) et (2)
laisse le journal **absent** et le `.tmp` porteur de l'**unique copie** des
derniers kilooctets de journal. Or aucun code ne le reprenait : ni
`Logger::begin()` ni `setPersistenceFs()` ne regardaient le `.tmp`, qui n'était
effacé qu'à l'effacement **explicite** des journaux (`clearAll()`). Le prochain
démarrage recréait `/system.log` **vide** et l'orphelin restait à vie.

La perte porte sur la trace de diagnostic, c'est-à-dire précisément ce dont on
dépend pour analyser la coupure qui l'a produite (cf.
[`docs/incidents/`](../incidents/README.md)).

**Ce qui n'est PAS le motif, et doit être dit** : `bug-099` avait été ouverte pour
une raison **budgétaire** — deux blocs LittleFS immobilisés sur une partition
`history` de 64 Ko. Ce motif était réel et il est **mesuré** : sous 64 Ko, le
`.tmp` orphelin abaissait le plafond admissible de `/history.json` de **8 176 o**
et faisait refuser **73 tailles sur 124** dans la bande réaliste 20-31 Ko
([`test/test_native_logger_logic/`](../../test/test_native_logger_logic/)). Sous
le **layout v5** ([ADR-0068](0068-partitions-layout-v5-flash-8-mo.md), 256 Ko) :
**zéro refus**. La conséquence budgétaire est **dissoute**. C'est ce que la
requalification du 2026-09-06 avait vu juste et ce que la spec n'avait pas vu :
ce lot ne vaut **que** par la perte de journal.

## Décision

`Logger::reconcileLogTmp()` est appelée **en tête** de `setPersistenceFs()`, et
réconcilie l'état des deux fichiers avant toute autre chose :

| `/system.log` | `.tmp` | Action |
|---|---|---|
| absent | présent | **PROMOUVOIR** — `rename(tmp → log)` + `WARN` |
| présent | présent | **EFFACER** le `.tmp` + `WARN` (la rotation se rejouera) |
| — | absent | rien, **SILENCE** (premier démarrage compris) |
| admission mémoire refusée | — | **ABSTENTION** : ni promotion ni effacement, **muette** |

Trois éléments constitutifs de la décision :

1. **La sonde porte sur le journal, pas sur le `.tmp`.** Une seule ouverture
   (`existsIfHeapAllows(_persistFs, kSystemLogPath, …)`) ; la présence du `.tmp`
   se **déduit du code de retour** du `rename`/`remove`.
2. **Classe B** ([ADR-0070](0070-fopen-interne-helper-unique-et-classes-de-contention.md)),
   seuil `kInternalFopenMinAllocBytes` = **5 120** : une admission unique
   (`fsGuardAdmit`) couvre la sonde **et** l'appel FS.
3. **La logique est pure** ([ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)) :
   [`src/logger_logic.h`](../../src/logger_logic.h) sépare
   `evaluateLogTmpRecovery()` — l'**intention**, avant tout effet de bord — de
   `classifyLogTmpOutcome()` — le **fait**, à la lumière du code de retour FS.

### ⚠️ Contrainte d'ORDRE imposée à tout futur appelant

Le bloc doit rester **avant** l'ouverture du marqueur `--- DÉMARRAGE ---`. Cette
ouverture est en mode `"a"`, qui **crée** `/system.log` s'il est absent : après
elle, la sonde répondrait **toujours** « présent », la branche `Promote`
deviendrait **inatteignable**, et le survivant partirait en `Discard`. Le code
censé sauver le journal le **détruirait**, sans qu'aucune ligne ne le dise.

Il doit aussi rester **avant** le `flushToDisk()` de fin de fonction, qui peut
relancer une rotation — donc recréer un `.tmp` par-dessus lequel on ne
réconcilie pas.

## Alternatives considérées

- **(1) Effacement aveugle de tout `.tmp`** (rejetée) — c'est la formulation
  initiale de la spec : « sa survie prouve qu'il est orphelin ». **Fausse dans la
  fenêtre `remove`/`rename`**, et destructrice : dans l'état « journal absent »,
  le `.tmp` est **complet et validé** — le `remove` n'est atteint que sous
  `tmpOk` (taille > 0 après la boucle de copie et `wf.close()`). L'effacer
  détruirait le journal au lieu de nettoyer un déchet, en violation du critère
  d'acceptation 4 de la spec elle-même.
- **(2) Sonder le `.tmp` plutôt que le journal** (rejetée) — ajouterait un
  **second `fopen`** (`exists()` **ouvre** le fichier, cf.
  [acces-fichiers.md](../subsystems/acces-fichiers.md)) sans rien apprendre de
  plus : l'information est déjà dans le code de retour du `rename`/`remove`.
  Doubler l'exposition à l'`abort()` newlib pour une redondance est le contraire
  de ce que fait l'ADR-0070.
- **(3) Classe A, seuil 8 192** (rejetée) — on s'abstiendrait dans toute la bande
  **5 120–8 192** où le filesystem est parfaitement sain. Et **au boot, « plus
  tard » n'existe pas** : contrairement à un flush, une abstention ici ne se
  rejoue pas dans 10 minutes — l'orphelin resterait à vie, et dans l'état
  « journal absent » c'est la trace de la coupure qu'on perdrait. L'absence de
  contention TCP est acquise **par construction** (`history.begin()` à
  [`main.cpp:271`](../../src/main.cpp:271) précède `mqttManager.begin()` 301 et
  `webServer.begin()` 317).
- **(4) Remplacement atomique à la rotation** — supprimer le `remove` préalable
  et s'appuyer sur un `rename` écrasant : **retenue, mais renvoyée à une spec de
  suite**. Deux raisons, aucune de confort. D'abord elle ne **dispense pas** de
  reprendre les orphelins **déjà présents** sur l'appareil : la fenêtre a pu
  s'ouvrir avant la mise à jour. Ensuite, livrer les deux ensemble rendrait la
  reprise **inéprouvable** — on ne saurait plus produire l'état qu'elle corrige,
  puisque le code capable de le créer aurait disparu dans le même lot.

## Conséquences

### Positives

- Le journal **survit** à une coupure survenue pendant une rotation : l'état
  `log absent + tmp présent` est désormais rattrapé, avec un `WARN` explicite.
- Le `.tmp` orphelin ne s'installe plus à vie ; les deux blocs qu'il immobilisait
  sont rendus (gain **réel mais sans enjeu** sous le layout v5).
- La **réconciliation est idempotente** : un appel FS échoué laisse le `.tmp`
  intact et le démarrage suivant réessaie. Aucun état intermédiaire n'est créé.
- Les deux cas actifs sont journalisés **distinctement** — « rotation à rejouer »
  et « journal récupéré » n'ont pas la même signification pour un post-mortem —
  et le cas nominal reste **strictement silencieux** (zéro octet de journal).

### Négatives / dette assumée

- **Rien n'a tourné sur cible.** Ni `Promote` ni `Discard` n'ont été exécutés
  contre un LittleFS réel, et la **sémantique de `rename` quand la cible existe**
  n'est **jamais** testée. La couverture est en logique pure (30 cas,
  [`test/test_native_logger_logic/`](../../test/test_native_logger_logic/)).
- **L'inférence « échec de l'appel FS ⇒ le `.tmp` était absent » est une
  hypothèse.** Un `rename` échouant pour corruption ou `ENOSPC` produirait le
  **même silence**. Acceptable pour deux raisons : elle ne pilote que la
  **journalisation**, jamais un effet destructeur ; et la réconciliation étant
  idempotente, le démarrage suivant réessaie.
- **Dégradation bornée si la sonde se trompe** (relevé en revue) : si
  `exists()` répondait « absent » à tort — journal présent mais illisible en
  `"r"` — la promotion écraserait la **tête** du journal. Jamais la **queue** :
  le `.tmp` contient les octets les **plus récents**. La perte porte donc sur les
  lignes les plus anciennes, c'est-à-dire celles que la rotation allait de toute
  façon supprimer.
- **L'abstention est muette.** Aucune ligne n'est émise sur refus d'admission :
  `_persistBuffer` (100 entrées, éviction FIFO) est précisément la ressource sous
  pression, et `history.cpp` émet déjà son `WARN` de famine juste après.
  L'observabilité durable de ce refus est le compteur du `fs_guard`, pas le
  journal.
- **La classe B repose sur « appelé au boot »** — pari que le bloc voisin
  (marqueur de démarrage, classe A) **refuse explicitement de faire**. Les deux
  sites sont à trois lignes d'écart et n'ont pas la même classe : c'est
  volontaire, et c'est fragile. Consigné comme invariant dans
  [acces-fichiers.md](../subsystems/acces-fichiers.md).

### Ce que ça verrouille

- **L'ordre d'initialisation de `setPersistenceFs()`.** Tout futur appelant, tout
  refactor qui déplacerait `reconcileLogTmp()` après l'ouverture du marqueur ou
  après `flushToDisk()`, **détruit le survivant en silence**. L'ancrage est
  commenté au point exact dans [`logger.cpp`](../../src/logger.cpp) et testé par
  contrôle textuel du source (le dépouilleur de commentaires du suite native
  désarme le leurre d'un appel mentionné en commentaire).
- **La classe de contention du site.** Déplacer `setPersistenceFs()` hors du boot
  — depuis une route HTTP, une tâche MQTT, un rechargement de configuration —
  invalide la preuve d'absence de contention TCP et rend le seuil 5 120
  insuffisant. Le site devrait alors passer en classe A, **et l'on retomberait
  sur l'alternative (3)** : abstention possible dans la bande 5 120–8 192.
- **L'obligation de garder la rotation non atomique jusqu'à la spec de suite.**
  Supprimer le `remove` préalable **avant** que la reprise ait été éprouvée sur
  cible priverait le projet du seul moyen de produire l'état à corriger.

## Références

- Code : [`src/logger_logic.h`](../../src/logger_logic.h) /
  [`.cpp`](../../src/logger_logic.cpp) (module pur),
  [`src/logger.cpp`](../../src/logger.cpp) (`reconcileLogTmp()`,
  `setPersistenceFs()`), [`src/logger.h`](../../src/logger.h)
  (`kSystemLogPath`, `kSystemLogTmpPath`)
- Tests : [`test/test_native_logger_logic/`](../../test/test_native_logger_logic/) — **30 cas**
- Spec : [`specs/features/doing/bug-099-tmp-de-rotation-jamais-nettoye-au-boot.md`](../../specs/features/done/bug-099-tmp-de-rotation-jamais-nettoye-au-boot.md)
- Doc : [logger.md](../subsystems/logger.md#reprise-dune-rotation-interrompue-au-démarrage-bug-099-v2481) ·
  [acces-fichiers.md](../subsystems/acces-fichiers.md) ·
  [history.md](../subsystems/history.md)
- Décisions liées : [ADR-0070](0070-fopen-interne-helper-unique-et-classes-de-contention.md)
  (classes de contention, point d'admission unique) ·
  [ADR-0068](0068-partitions-layout-v5-flash-8-mo.md) (256 Ko — ce qui dissout la
  conséquence budgétaire) ·
  [ADR-0033](0033-budget-espace-partition-history.md) (`kMaxLogFileBytes`,
  `kRotateKeepBytes`) ·
  [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)
