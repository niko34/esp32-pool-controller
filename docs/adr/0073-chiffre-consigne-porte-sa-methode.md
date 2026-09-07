# ADR-0073 — Un chiffre consigné porte sa méthode, ou il ne vaut rien

- **Statut** : Accepté
- **Date** : 2026-09-07
- **Décideurs** : architect, embedded-developer, test-engineer, code-reviewer, doc-writer
- **Spec(s) liée(s)** : [bug-098](../../specs/features/done/bug-098-drapeau-dosage-orp-perdu-a-l-ecriture.md), [bug-106](../../specs/features/done/bug-106-journee-archivee-a-zero-sur-compteur-en-recul.md)

## Contexte

**Onze valeurs numériques du dépôt se sont révélées fausses en une seule journée** — le 2026-09-07, pendant le lot bug-098 / bug-106. Ce n'est pas une série de distractions. C'est le symptôme d'un dépôt où les **commentaires chiffrés sont devenus une source de vérité de fait** sans avoir jamais été conçus pour ça : ils ont été écrits une fois, justes ce jour-là, puis recopiés — de commentaire en spec, de spec en brief, de brief en décision.

Les cas, consignés parce qu'ils instruisent :

### 1. Le budget de la trame WebSocket, faux quatre fois de suite

La chaîne de chiffrage partait d'une **mesure vivante** de **2 479 o** — une trame réelle, à un instant donné, avec les valeurs qu'elle portait alors — sur laquelle **huit deltas « pire cas »** ont été empilés. Mélanger une base observée et des majorants ne produit ni l'un ni l'autre. **Deux** de ces quatre chiffrages ont été **relayés par l'agent principal sans vérification**, ce qui les a rendus indiscernables de faits établis.

### 2. La taille du journal : la conclusion en était renforcée

**24 Ko / 32 Ko** annoncés, **6 144 / 8 192** octets réels. Le chiffre faux n'affaiblissait pas la conclusion, il la **masquait** : la fenêtre effaçable ne valait pas 70 s mais **13 s**. Une erreur de facteur 4 du côté rassurant.

### 3. Le carry d'export : le troisième chiffre faux était dans la correction des deux premiers

**163 o** annoncés pour le pire cas d'un point de `/get-history`, **161** réels. La « rectification » de revue a alors porté la partie fixe de 111 à 114 en **gardant** 161 — deux chiffres inconciliables (114 + 52 = 166). Et le bloc de commentaires qui corrigeait ces deux erreurs en contenait une **troisième** : « 159 atteignable », alors que la granularité est bornée à `DAILY = 2` et le masque à 7 — **un** caractère chacun, donc **157**.

### 4. « Le point est sauté sans aucun log » : périmé, et propagé trois fois

L'affirmation était vraie quand elle a été écrite. Elle est **périmée depuis `feature-095`**, qui a ajouté un compteur (`st->skipped`) et un **WARNING throttlé** à la clôture de la réponse. L'agent principal l'a reprise d'un commentaire **sans la vérifier** et propagée dans **trois briefs successifs** — trois décisions prises sur un fait mort.

### 5. `CLAUDE.md` : une fausse base de non-régression, lue par chaque session

Le guide annonçait « **33 suites / 1 291 cas** » pour **38 / 1 418** réels (mesuré le 2026-09-07 sur la v2.46.2). Ce n'est pas un détail d'inventaire : c'est la **base de non-régression** que chaque nouvelle session lit pour savoir ce qu'elle ne doit pas casser.

### 6. Deux specs, deux chiffres à un ordre de grandeur

« Un octet par point, **339 o** » là où c'est **10 o par point et 3 390 o** (le `CHANGELOG` de la v2.42.0 chiffrait le coût de la séparation des deux drapeaux comme s'il s'agissait d'un octet de structure, pas d'une clé JSON). Et « probablement neutre » pour une variante qui est en réalité **meilleure que neutre** : **−4 o par point**.

### Le point commun

Aucune de ces onze valeurs n'était accompagnée de **comment elle avait été obtenue**. Aucune n'était donc **refaisable** — seulement **recopiable**. Et c'est ce qui s'est produit à chaque fois : le chiffre a voyagé, il a gagné en autorité à chaque copie, et personne n'a pu le contredire parce que personne ne savait d'où il venait.

## Décision

**Tout chiffre consigné dans un commentaire, une spec ou une documentation porte trois choses :**

| | Élément | Pourquoi |
|---|---|---|
| **(a)** | **comment il a été obtenu** — mesure vivante, décompte structurel clé par clé, `sizeof`, balayage exhaustif, plafond de `constants.h`… | un « pire cas » et une « mesure » ne se composent pas |
| **(b)** | **la source à relire pour le refaire** — fichier et ligne, nom de test, commande | un chiffre sans source n'est pas vérifiable, donc pas discutable |
| **(c)** | **sa date ou sa version** | un chiffre juste vieillit ; sans date, on ne peut pas savoir s'il a vieilli |

**Un chiffre sans méthode se recopie ; un chiffre avec méthode se refait.**

### Corollaire, qui est la moitié de la décision

**Ne jamais reprendre un chiffre d'un commentaire sans le remesurer.** Écrire la bonne valeur ne suffit pas : le suivant se tromperait pareil, en recopiant à son tour. C'est précisément ce qui s'est passé au cas n° 3, où la correction a introduit une erreur de plus, et au cas n° 4, où un fait mort a traversé trois briefs.

Corollaire du corollaire, pour les agents : **un chiffre relayé sans vérification devient un fait établi**. Le relais est un acte d'écriture, pas de transport.

## Alternatives considérées

- **Corriger les onze valeurs et passer à autre chose** (rejetée) — c'est exactement ce qui avait été fait au cas n° 3, et la correction a produit une **troisième** erreur dans le bloc même qui corrigeait les deux premières. Corriger sans changer la méthode ne fait que déplacer la date de la prochaine erreur.
- **Interdire les chiffres dans les commentaires** (rejetée) — ce sont eux qui rendent les arbitrages de ce projet lisibles : un budget de partition, une marge de trame, un plafond de tampon **doivent** être écrits quelque part. Les retirer priverait le lecteur du raisonnement et le forcerait à tout remesurer pour comprendre une décision déjà prise.
- **Une source unique de vérité générée** (un fichier de métriques produit par les tests, inclus dans la doc) (rejetée pour ce cycle) — séduisante et probablement juste à terme, mais elle demande un outil, un format et un point d'injection dans la chaîne de build. Et elle ne couvre pas les chiffres qui **n'ont pas de test** (taille de partition, choix de dimensionnement). Consignée comme piste, pas retenue ici.
- **Un test par chiffre consigné** (rejetée comme règle générale, retenue comme pratique) — c'est la forme la plus forte, et le lot l'applique là où c'est possible : le pire cas d'un point de `/get-history` est désormais **asserté** (`161` absolu, `157` atteignable, marges `31` et `35`) et le pire cas est **mesuré par balayage**, pas postulé. Mais un `static_assert` ou un test Unity ne peut pas garder un chiffre qui décrit le **monde** (taille d'une partition sur la flash, débit d'une pompe) : la règle doit valoir aussi pour ceux-là, donc elle est documentaire avant d'être outillée.

## Conséquences

### Positives

- **Un chiffre devient refaisable**, donc contestable — et donc corrigible par n'importe qui, pas seulement par celui qui l'a écrit.
- **Les deux méthodes de chiffrage du projet sont désormais nommées et distinctes** : la **mesure vivante** (une trame réelle, un fichier réel, à un instant daté) et le **décompte structurel au pire cas** (clé par clé, saturation de chaque champ). Elles ne se mélangent pas — c'est l'erreur du cas n° 1.
- **Exemples d'application déjà en place**, à citer comme modèles :
  - la section normative de [`docs/subsystems/ws-manager.md`](../subsystems/ws-manager.md#deux-trames-deux-gabarits--un-seul-plafond-dur-à-lire-avant-tout-chiffrage) — « décompte clé par clé au pire cas **structurel**, jamais sur une trame vivante », avec la convention de comptage (guillemets + deux-points + valeur + virgule, valeurs signées sur 8 caractères, `null` = 4 o) ;
  - le bloc de `kHistoryPointJsonMaxBytes` dans [`src/constants.h`](../../src/constants.h) — qui porte désormais son décompte ligne à ligne, sa **source** (`history_export_chunk.h` pour le format, `history.h` pour les bits), la mention explicite « **le total ci-dessus est MESURÉ, pas recopié** », et l'histoire des deux chiffrages faux qui l'ont précédé ;
  - le bloc du throttle de WARN de calibration dans [`src/constants.h`](../../src/constants.h) (v2.46.1), qui porte désormais sa **source** — `kMaxLogFileBytes = 8 192` et `kRotateKeepBytes = 6 144`, [`logger.h:50-51`](../../src/logger.h:50) —, la **rectification datée** des 24 Ko / 32 Ko historiques, et l'avertissement littéral : « toute reprise de ce calcul doit lire `logger.h`, **jamais un commentaire** ».
- **Les chiffres du lot bug-098 sont tous adossés à un test** : `−1 113 o` sur 339 points (balayage des longueurs de clé, `test_native_dose_mask` section 6), `161` / `157` o de pire cas (assertions de format, `test_native_history_export`), `65 536` couples (masque, granularité) sans saut silencieux (balayage exhaustif), `sizeof(DataPoint) == 28` (`static_assert`, validé **par mutation**).

### Négatives / dette assumée

- **Un commentaire chiffré devient plus long.** Trois lignes de méthode pour un chiffre d'un octet, c'est le prix — et c'est moins cher qu'une décision prise sur un chiffre faux.
- **La règle ne se vérifie pas automatiquement.** Rien n'échoue si un chiffre est écrit sans sa méthode. C'est une discipline documentaire, avec le mode de défaillance de toutes les disciplines documentaires — le même que celui qui a produit les onze cas. Elle est **écrite**, ce qui est strictement plus qu'avant, pas suffisant.
- **Les chiffres antérieurs à cet ADR ne sont pas rétro-documentés.** Une valeur croisée dans un commentaire ancien doit être traitée comme **non vérifiée** jusqu'à preuve du contraire.
- **Deux réserves restent ouvertes dans ce lot précis**, consignées et non traitées :
  - `historyPointPersistedLen` ([`test/test_native_history/test_history_logic.cpp`](../../test/test_native_history/test_history_logic.cpp)) modélise encore `"d":true/false` et une partition de **64 Ko** (256 Ko depuis [ADR-0068](0068-partitions-layout-v5-flash-8-mo.md)). Il se trompe **du côté sûr** — il annonce le fichier **plus gros** qu'il n'est, de 1 113 o sur la charge de 339 points — donc rien n'est rouge et le modèle reste valable comme **majorant**. Le corriger propage sur une dizaine de constantes asservies (`23497 / 25513 / 30616 / 2016 / 7119 / 8102 / 4006 / 43008`) et sur **tout le récit de budget** de la partition. **Une spec de suivi est nécessaire.**
  - `CLAUDE.md` sera à recaler à chaque évolution du parc de tests : **39 suites / 1 451 cas** à la v2.47.0 (mesuré le 2026-09-07 par `ls -d test/test_native_* | wc -l` et `grep -rn 'RUN_TEST(' test/ | wc -l`).

### Ce que ça verrouille

- **La méthode de chiffrage du budget de trame WebSocket** : pire cas **structurel**, jamais une mesure vivante ([ADR-0071](0071-parite-data-trame-capteurs.md) l'avait déjà verrouillée pour cet axe ; la règle est ici généralisée à tout chiffre du dépôt).
- **La forme d'un commentaire chiffré** : décompte visible, source citée, date ou version. Un chiffre nu dans un commentaire est désormais un défaut de revue, pas une omission de style.
- **Le statut d'un chiffre relayé par un agent** : il engage celui qui le relaie. « Repris du commentaire X » n'est pas une méthode.

## Références

- Code : [`src/constants.h`](../../src/constants.h) (`kHistoryPointJsonMaxBytes` et le throttle de WARN de calibration — deux blocs qui portent leur méthode), [`src/history_export_chunk.h`](../../src/history_export_chunk.h) (format de référence du décompte), [`src/logger.h`](../../src/logger.h) (source des tailles de journal)
- Tests : [`test/test_native_history_export/test_history_export.cpp`](../../test/test_native_history_export/test_history_export.cpp) (pire cas absolu/atteignable assertés, balayage des 65 536 couples), [`test/test_native_dose_mask/test_dose_mask.cpp`](../../test/test_native_dose_mask/test_dose_mask.cpp) (budget du format persisté), [`test/test_native_history/test_history_logic.cpp`](../../test/test_native_history/test_history_logic.cpp) (modèle périmé, signalé en tête de section)
- Doc : [`docs/subsystems/ws-manager.md`](../subsystems/ws-manager.md), [`docs/subsystems/history.md`](../subsystems/history.md), [`docs/subsystems/logger.md`](../subsystems/logger.md)
- Specs : [`bug-098`](../../specs/features/done/bug-098-drapeau-dosage-orp-perdu-a-l-ecriture.md), [`bug-106`](../../specs/features/done/bug-106-journee-archivee-a-zero-sur-compteur-en-recul.md)
- ADR liés : [ADR-0071](0071-parite-data-trame-capteurs.md) (pire cas structurel de la trame, origine de la règle), [ADR-0033](0033-budget-espace-partition-history.md) (budget de la partition `history`), [ADR-0068](0068-partitions-layout-v5-flash-8-mo.md) (layout v5 — la partition qui a changé d'ordre de grandeur), [ADR-0038](0038-metrique-memoire-allouable-8-bits.md) (précédent : une métrique fausse qui a rendu toutes les gardes aveugles), [ADR-0072](0072-absence-de-valeur-hors-du-domaine.md) (l'autre ADR du même lot)
