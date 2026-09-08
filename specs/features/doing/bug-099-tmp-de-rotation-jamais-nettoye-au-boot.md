# bug-099 — Le fichier temporaire de rotation n'est jamais nettoyé au démarrage

## Métadonnées

| Champ | Valeur |
|---|---|
| Type | bug — occupation résiduelle d'une partition contrainte |
| Statut | doing — **code, tests et docs livrés en v2.48.1** ; reste la validation sur cible |
| Priorité | Basse tant que la marge tient ; **monte si un lot consomme encore la partition `history`** |
| Découvert | 05/09/2026, par le `test-engineer` pendant la vérification de budget de la feature-095 |
| Version en cours | 2.39.0 (déployée) — correctif livré en **2.48.1** (non déployé) |

## ⚠️ Requalification du 2026-09-06 — le défaut tient, sa criticité a disparu

Le `CHANGELOG` de la **v2.42.0** annonce que « **`bug-099` est dissous** ». Pris
isolément, c'est **trompeur** : ce qui est dissous, c'est la **conséquence**, pas la
**cause**.

- **La cause est intacte, vérifiée dans le code le 2026-09-06** : le `.tmp` de
  rotation n'est effacé qu'à l'effacement **explicite** des journaux
  (`logger.cpp:222`). `Logger::begin()` et `setPersistenceFs()` ne contiennent
  **aucun nettoyage au démarrage**. Un `.tmp` laissé par une rotation interrompue
  survit donc indéfiniment, exactement comme décrit ci-dessous.
- **La conséquence, elle, a disparu** : le layout v5 (ADR-0068) a porté la partition
  `history` de 64 Ko à **256 Ko**. Six kilo-octets orphelins ne menacent plus le
  budget — c'était le motif d'origine de ce bug.

**Ce qui reste** n'est donc plus un risque de saturation mais un **fichier fantôme** :
il occupe un bloc LittleFS, et un lecteur futur peut le prendre pour une rotation en
cours. Priorité **basse**, sans échéance.

⚠️ **Ne pas clore cette spec au motif qu'elle serait « dissoute »** : le code n'a pas
changé. Et ne pas l'ouvrir en croyant traiter une urgence mémoire : ce n'en est plus
une.

## Domaines touchés

- `src/logger.cpp` — rotation de `/system.log`
- `src/history.cpp` — `historySaveAdmit()` (consommateur de la marge)
- `docs/subsystems/logger.md`, `docs/subsystems/history.md`

## Sécurité chimique

**Non.** Aucun chemin de dosage. Gate `pool-chemistry` non requis.

## Le problème

La rotation du journal système écrit un fichier `.tmp` transitoire d'environ
6 Ko. Ce fichier n'est effacé qu'à l'**effacement explicite des journaux** — il
n'existe **aucun nettoyage au démarrage**. Un `.tmp` laissé par une rotation
interrompue (coupure secteur, redémarrage pendant l'écriture) reste donc
indéfiniment sur la partition.

Or les deux fichiers partagent la même partition de **64 Ko**, et la feature-095
vient d'y consommer 2 016 octets : le régime établi est à **12 blocs sur 16**,
résiduel **8 102 octets**.

**Le cas mesuré** : un `.tmp` orphelin **plus** un tampon d'historique plein
réduisent la marge de croissance à **90 octets** — soit un seul point
d'historique. Avant la feature-095, ce même cas laissait encore 4 006 octets.

Le mode de défaillance est le **silence** : `historySaveAdmit()` refuse la
sauvegarde et journalise un avertissement une fois par heure. L'historique cesse
d'être persisté sans qu'aucun signal fort ne le dise, et le compteur `nospc_cnt`
n'est exposé par **aucun endpoint** (vérifié par grep).

## Ce qui est demandé

1. **Réconcilier** l'état du journal au démarrage, **avant** le premier calcul
   d'admission — et **avant** l'écriture du marqueur `--- DÉMARRAGE ---`
   (`logger.cpp:246`, mode `"a"`, qui **recrée** `/system.log` s'il est absent et
   détruirait donc l'état à observer). Selon l'état des deux fichiers, le `.tmp`
   est **effacé** ou **promu** (cf. tableau des états ci-dessous).

   ⚠️ **Correction du 2026-09-08 — la formulation initiale « effacer tout `.tmp`,
   sa survie signifie qu'il est orphelin » est FAUSSE dans une fenêtre**, et
   l'appliquer littéralement violerait le critère 4 de cette même spec (« ne
   touche jamais le journal courant »). `logger.cpp:386-391` exécute
   `remove("/system.log")` **puis** `rename("/system.log.tmp", "/system.log")` :
   deux opérations LittleFS distinctes, **non atomiques entre elles**. Une coupure
   entre les deux laisse `/system.log` **absent** et le `.tmp` porteur de
   **l'unique copie** des 6 derniers Ko de journal (`kRotateKeepBytes = 6144`,
   `logger.h:51`). Ce `.tmp` est alors **complet et validé** : le `remove` n'est
   atteint que sous `tmpOk` (`logger.cpp:383` — taille > 0, après la boucle de
   copie et `wf.close()`). L'effacer inconditionnellement **détruirait** le
   journal au lieu de nettoyer un déchet.

### Tableau des états au démarrage

| `/system.log` | `/system.log.tmp` | Interprétation | Action |
|---|---|---|---|
| absent | présent | coupure entre `remove` et `rename` — le `.tmp` est le **survivant** | **PROMOUVOIR** `rename(tmp → log)` + WARN |
| présent | présent | coupure pendant la copie, ou avant le `remove` — l'original tient | **EFFACER** le `.tmp` + WARN (la rotation se rejouera) |
| présent | absent | nominal | rien, **silence** |
| absent | absent | premier boot / après effacement explicite | rien, **silence** |
| — | — | admission mémoire refusée | **ABSTENTION** (ni promotion ni effacement) |

2. Journaliser **distinctement** les deux cas actifs — effacement et promotion
   n'ont pas la même signification : le premier signale une rotation à rejouer,
   le second une récupération de journal. Le cas nominal reste **silencieux**.
3. Envisager d'exposer `nospc_cnt` (ou l'occupation de la partition) dans
   `/get-system-info` : aujourd'hui, rien ne permet de constater à distance que
   l'historique a cessé d'être écrit. **À arbitrer** — cela peut aussi relever
   d'une spec d'observabilité séparée.

## Critères d'acceptation

- [x] Un `.tmp` présent au démarrage est **effacé OU promu** selon l'état de
      `/system.log`, et **journalisé distinctement** dans les deux cas
      → `Logger::reconcileLogTmp()`, deux `WARNING` distincts
- [x] La réconciliation a lieu **avant** l'écriture du marqueur de démarrage
      (sinon l'état `absent + présent` est effacé par le marqueur lui-même)
      → appel en **tête** de `setPersistenceFs()`, ancrage commenté et **vérifié
      par contrôle textuel du source** en test natif
- [x] Admission mémoire refusée → **abstention** (aucun `remove`, aucun `rename`)
      → `evaluateLogTmpRecovery(admitted=false, …)` rend `Abstain` **sans repli**,
      et la sonde elle-même est court-circuitée par `admitted &&`
- [x] La marge d'admission est recalculée après nettoyage, pas avant
      → les **deux** appels de `history.cpp` (l. 332 tripwire, l. 377 nominal)
      précèdent `historySaveAdmit()` (l. 630)
- [x] Test : `.tmp` orphelin → partition libérée, sauvegarde admise
      → prouvé **par l'arithmétique**, sur le module pur `historySaveAdmit` :
      le `.tmp` coûte **8 176 o** (2 blocs). Sous 64 Ko, plafond 32 704 → 24 528 o
      avec `.tmp`, et **73 tailles sur 124** de la bande réaliste 20–31 Ko sont
      refusées (premier refus à 24 539 o) — le motif d'origine de la spec est
      **confirmé, il était réel**. Sous layout v5 (256 Ko, ADR-0068) : plafond
      220 752 o, **zéro refus**.
- [ ] **Vérification sur cible du cas NOMINAL** (en attente de déploiement)
      → démarrage sans `.tmp` : **aucune ligne nouvelle** dans le journal, boot
      inchangé. C'est le seul chemin exerçable sur l'appareil, et il vérifie
      l'invariant de **silence nominal** — celui qui, s'il était cassé, ferait
      émettre un faux avertissement à **chaque** démarrage.

### ⚠️ Ce qui ne sera JAMAIS prouvé sur cible — et pourquoi ce n'est pas un manquement

Ni `Promote` ni `Discard` ne peuvent tourner contre un LittleFS réel dans cet
environnement : la fenêtre `remove`/`rename` dure quelques millisecondes, le
conteneur n'a **pas de port série**, `deploy.sh fs` flashe `spiffs` et non
`history`, et **aucune route n'écrit de fichier arbitraire**. Fabriquer l'état
`journal absent + .tmp présent` exigerait un firmware jetable ajoutant une route
d'écriture arbitraire sur la partition `history` — soit exactement la surface
d'attaque que `bug-113` vient de réduire de 80,7 %.

Trois conséquences à assumer explicitement, plutôt qu'à laisser ce critère ouvert
indéfiniment :

1. La **sémantique de `rename` quand la cible existe** n'est jamais éprouvée. Elle
   n'affecte pas ce lot — dans `Promote`, la cible est sondée **absente** — mais
   elle devra être prouvée avant d'écrire la spec de suite sur le remplacement
   atomique à la rotation, qui en dépend entièrement.
2. L'inférence « échec de l'appel FS ⇒ le `.tmp` était absent » reste une
   **hypothèse** : un `rename` échouant pour corruption ou ENOSPC produirait le
   même silence qu'une absence. Acceptable, pour deux raisons établies en revue —
   elle ne pilote **que la journalisation**, jamais un effet destructeur, et la
   réconciliation est **idempotente** (un `rename` échoué laisse le `.tmp` intact,
   le démarrage suivant réessaie). Coût maximal d'une erreur : une ligne de journal
   manquante.
3. La preuve repose donc sur **trois axes de substitution** — table de vérité du
   module pur, sentinelles textuelles sur code dépouillé de ses commentaires, et
   arithmétique de budget — validés par **14 mutants tués sur 14** (6 par le
   `test-engineer`, 8 de plus par le `code-reviewer`, 2 par moi sur le cardinal de
   l'enum). C'est le maximum atteignable sans matériel d'instrumentation, et ce
   plafond doit être **écrit**, pas contourné par un critère coché à la légère.
- [x] Le nettoyage ne touche **jamais** le journal courant ni `/history.json`
      → `Discard` n'efface que le `.tmp` ; `Promote` n'écrit que sur un chemin
      sondé **absent**. ⚠️ Dégradation **bornée** relevée en revue : si la sonde
      répondait « absent » à tort (journal corrompu, illisible en `"r"`), la
      promotion écraserait la **tête** du journal — jamais la queue.

## Notes

### Note du 2026-09-08 — la criticité d'origine reste dissoute, l'enjeu a changé de nature

**À lire avant de juger cette spec sur sa priorité « basse ».** Le motif d'origine
— la saturation d'une partition `history` de 64 Ko — est bien **dissous** : le
layout v5 (ADR-0068) l'a portée à **256 Ko**, six kilo-octets orphelins ne
menacent plus aucun budget. Ce que ce lot corrige n'est donc **pas** un risque de
saturation.

Mais ce n'est pas non plus le simple « fichier fantôme » décrit par la
requalification du 2026-09-06. C'est une **perte de journal après coupure** : dans
l'état `log absent + tmp présent`, le `.tmp` détient l'unique copie des 6 derniers
Ko de journal, et le code actuel ne le promeut jamais — le prochain
`setPersistenceFs()` recrée `/system.log` vide et le `.tmp` reste orphelin pour
toujours. L'enjeu est **différent, et plus sérieux** que celui inscrit dans les
métadonnées : il touche la trace de diagnostic, c'est-à-dire précisément ce dont
on dépend pour analyser la coupure qui l'a produite (cf. `docs/incidents/`).

Trouvé en construisant le cas de charge du pire scénario, pas en observant une
panne. C'est exactement le genre de défaut qui ne se manifeste que le jour où
une autre feature a mangé la marge — et qui se diagnostique alors comme un
problème de cette autre feature.

### Note du 2026-09-08 (2) — ce qui a été livré en v2.48.1, et ce qui reste

**Livré** : `Logger::reconcileLogTmp()` appelée en tête de `setPersistenceFs()`,
module pur `src/logger_logic.{h,cpp}` (`evaluateLogTmpRecovery` = l'intention,
`classifyLogTmpOutcome` = le fait d'après le code de retour FS), constantes
`kSystemLogPath` / `kSystemLogTmpPath` dans `logger.h`, sonde de classe **B**
(`kInternalFopenMinAllocBytes` = 5 120) sous **une seule** admission couvrant la
sonde et l'appel FS. Décision consignée en
[ADR-0076](../../../docs/adr/0076-reconciliation-du-tmp-de-rotation-au-boot.md).
Tests : `test/test_native_logger_logic/`, **30 cas**.

**Le motif budgétaire de cette spec est dissous, et c'est maintenant mesuré.**
Sous une partition de 64 Ko, le `.tmp` orphelin abaissait le plafond admissible
de `/history.json` de **8 176 o** et faisait refuser **73 tailles sur 124** dans
la bande réaliste 20-31 Ko : le motif d'origine était **réel**. Sous le layout v5
(256 Ko, ADR-0068) : **zéro refus**. Ce lot ne vaut donc **que** par la perte de
journal — ce que la requalification du 2026-09-06 avait vu juste, et ce que la
rédaction initiale de cette spec n'avait pas vu.

**Reste ouvert** :

1. **Vérification sur cible du cas NOMINAL**, en attente de déploiement — seul
   critère encore ouvert, et le seul exerçable (cf. la section sur ce qui ne sera
   jamais prouvé sur cible). L'appareil tourne en **2.45.1** ; ce lot est en
   **2.48.1**.
2. **Point 3 de « Ce qui est demandé » — DÉTACHÉ, pas abandonné** : exposer
   `nospc_cnt` ou l'occupation de la partition dans `/get-system-info`. Il était
   marqué « à arbitrer » ; l'arbitrage rendu le 2026-09-08 est de le renvoyer à
   [`feature-120-observabilite-des-refus-de-persistance`](../todo/feature-120-observabilite-des-refus-de-persistance.md),
   **créée**. Trois motifs : la conséquence budgétaire qui le motivait est
   dissoute (mesuré : zéro refus sous v5) ; exposer un champ engage
   `web_routes_config.cpp`, la page Système et `docs/API.md`, donc un cycle UI
   complet avec `ux-ui-designer` ; et surtout, faire entrer cette surface ici
   aurait **dilué la revue** sur la partie du lot qui, elle, **détruit des
   données** si elle est fausse. La spec détachée porte aussi l'arbitrage de fond
   « lire NVS dans un handler async, ou mettre en cache au boot ? », qui vaudra
   pour tout compteur persistant exposé ensuite.
3. **Remplacement atomique à la rotation** (supprimer le `remove` préalable) —
   retenu sur le fond, **renvoyé à une spec de suite** : il ne dispense pas de
   reprendre les orphelins déjà présents sur l'appareil, et livrer les deux
   ensemble rendrait la reprise **inéprouvable** (on ne saurait plus produire
   l'état qu'elle corrige). Cf. ADR-0076, alternative (4).
