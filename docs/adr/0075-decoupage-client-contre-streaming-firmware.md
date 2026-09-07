# ADR-0075 — La restauration d'historique est découpée par le CLIENT, pas streamée par le firmware

- **Statut** : Accepté
- **Date** : 2026-09-07
- **Décideurs** : `architect`, `web-ui-developer`, `code-reviewer`, utilisateur (arbitrage du plafond)
- **Spec(s) liée(s)** : [`bug-101`](../../specs/features/done/bug-101-import-historique-plafonne-a-16-ko.md), [`bug-113`](../../specs/features/done/bug-113-tamponnage-json-non-authentifie.md)

## Contexte

`POST /history/import` utilise `AsyncCallbackJsonWebHandler`, qui **tamponne le corps
entier** (`calloc(total + 1)`) avant d'appeler son callback. Son plafond,
`_maxContentLength` = **16 384 o**, était le défaut de la bibliothèque et n'avait
**jamais** été surchargé.

Le défaut est apparu **en situation réelle**, pas en revue : le 05/09/2026, après le
changement de table de partitions ([ADR-0068](0068-partitions-layout-v5-flash-8-mo.md)),
**245 points** devaient être réimportés et **145 seulement** sont passés. Les trois
défauts constatés étaient distincts : la limite n'était **écrite nulle part** ; l'échec
était **muet** (le `413` est émis par la bibliothèque, **avant** `REQUIRE_AUTH`, sans
corps JSON) ; et la route **REMPLAÇAIT** l'historique au lieu d'y ajouter — donc
découper « pour contourner » perdait tout sauf le dernier lot, sans message.

Deux specs tiraient en sens **opposé**, et il fallait trancher ensemble :

- `bug-101` voulait **plus** de capacité (restaurer un historique complet) ;
- `bug-113` voulait **moins** : ces 16 Ko sont allouables **sans authentification**, sur
  **trois** routes, soit 49 152 o réclamables sans identifiants, pendant la famine
  mémoire documentée par `bug-080`.

**Une prémisse partagée par les deux specs était fausse**, et il faut l'écrire pour
qu'elle ne soit pas redécouverte de travers. « Relever le plafond agrandit une
allocation contiguë dans un tas 8 bits affamé » est **faux depuis le lot B-1 de
`bug-080`** : établi **par désassemblage** de `libheap.a` (section `.iram1.2`, le
2026-09-07), `calloc` → `heap_caps_malloc_default` → si
`size > malloc_alwaysinternal_limit` (**4 095**, posé par `main.cpp`), l'allocateur
charge `MALLOC_CAP_DEFAULT|MALLOC_CAP_SPIRAM`, donc **PSRAM d'abord**, avec repli
interne seulement si la PSRAM rend `NULL`. Un tampon de 16 Ko ne partait **pas** dans le
tas affamé.

## Décision

**Le découpage est fait par le client, et l'API gagne un mode `append` explicite.** Le
firmware ne streame pas le corps entrant : il continue de le tamponner, mais sous un
plafond **abaissé** à **8 192 o** par route, et fusionne chaque lot dans l'historique
existant.

Cette décision **résout la contradiction** entre les deux specs : une fois le découpage
porté chez le client, `/history/import` n'a plus besoin de capacité du tout, donc le
plafond peut **descendre** — et `bug-113` est satisfaite **par le correctif de
`bug-101`**, pas malgré lui.

**Le motif du plafond bas change, pas la décision.** Ce n'est pas « ça va planter » :
sans lui, la sûreté de ce chemin reposerait **entièrement sur une préférence
d'allocateur**, dont le repli interne est le contre-exemple. Un plafond bas est vrai
**indépendamment de l'état de la PSRAM** ; une préférence, non. Le dépôt s'est déjà
trompé **dans l'autre sens** — le parachute anti-OOM a vécu **en PSRAM**, donc
décoratif, depuis `feature-066`.

**Corollaire contre-intuitif, à ne pas « optimiser »** : découper **plus fin**
ramènerait l'allocation **dans** le tas affamé (≤ 4 095 o ⇒ interne). Le budget de lot
client, **7 168 o**, est **au-dessus** de ce seuil à dessein, et un `static_assert`
interdit de le réduire sous 4 095.

## Alternatives considérées

- **Relever `setMaxContentLength()` (rejetée)** — le correctif évident, et le mauvais.
  Techniquement suffisant, il aggrave `bug-113` (plus de mémoire réclamable sans
  authentification) et ne corrige **ni** l'échec muet **ni** le remplacement destructeur.
  ⚠️ Son argument d'origine (« allocation contiguë en RAM interne ») était **faux** ; le
  rejet tient sur les deux autres motifs.

- **Streaming JSON incrémental côté firmware (rejetée — c'est l'alternative sérieuse)**
  — traiter le corps au fil de l'eau plutôt que l'accumuler. Le motif du rejet **n'est
  pas la flash** : depuis l'ADR-0068 il reste **1,9 Mo** libres par slot. C'est la
  **machine à états de parsing JSON incrémental sur `async_tcp`**, alimentée par des
  fragments qui coupent **au milieu d'un nombre**, d'une chaîne échappée ou d'une clé.

  **Le dépôt a déjà payé ce prix exact sur le chemin sortant.** `handleGetHistory` est
  une machine à états chunkée dont un commentaire documente que le pied `],"count":N}`
  n'était **jamais émis** : le cas `kFooter` chargeait le pied dans le tampon de report
  **et** passait en `kDone` dans la même itération, alors que la condition de boucle
  sortait avant de vider ce report. Résultat : **JSON tronqué sur toutes les réponses**,
  graphiques morts, **pendant plusieurs versions** (v2.14.0 → v2.18.1, 5 jours en
  production et 2 revues de code sans le voir). **La sauvegarde réelle du 05/09 en porte
  encore la cicatrice** : sa clé `count` apparaît **deux fois**, `0` puis `245`.

  Un parseur entrant est **strictement plus difficile** que ce formateur sortant — et il
  siège sur le chemin d'écriture de l'historique, où une erreur ne tronque pas un
  affichage mais **corrompt des données**.

- **Format compact dédié à la restauration (rejetée)** — CSV, ou JSON à clés courtes
  (`pf`/`of`, celles du fichier persisté). Diviser le volume par ~1,8 laisserait le pire
  cas à **~28 Ko**, soit **encore** au-dessus du plateau visé — le problème ne serait
  donc **pas résolu**, seulement déplacé. Et cela créerait un **troisième** format
  d'historique, après le persisté (clés courtes) et l'exporté (clés longues), avec la
  matrice de compatibilité correspondante.

- **Découpage client + mode `append` (retenue)** — le seul chemin qui ne touche pas au
  budget mémoire de l'appareil, qui **abaisse** la surface exposée sans authentification,
  et qui ferme les trois défauts de `bug-101` d'un coup. Le découpage se fait par
  **octets mesurés** (`jsonByteLength`), jamais par nombre de points : un point pèse de
  **110** à **149 octets** selon les décimales rendues, et un découpage « par N points »
  repasserait le plafond au premier fichier à décimales longues.

## Conséquences

### Positives

- **Une restauration de taille quelconque devient possible** sans allocation
  supplémentaire côté appareil. Le seul contigu du chemin reste le vecteur existant,
  339 × 28 = **9 492 o**, verrouillé par `static_assert`.
- **La surface exposée sans authentification chute de 80,73 %** : 9 472 o contre
  49 152 (8 192 + 1 024 + 256), **assertée à la compilation** — toute révision casse le
  build au lieu de périmer la documentation.
- **Le refus n'est plus muet** : `413` et `503` sont rendus en **JSON maison** par une
  garde prioritaire ([ADR-0067](0067-garde-503-avant-fopen-handler-prioritaire.md)),
  l'interface affiche la limite **avant** l'envoi, et `dropped` remonte toute éviction.
- **La reprise après coupure est sûre** : la règle de collision « le point entrant
  écrase, toutes granularités RAW comprise, toutes les occurrences du créneau » donne
  l'**idempotence exacte**. Réenvoyer un lot ne crée aucun doublon.
- **Rétrocompatibilité stricte** : `mode` absent ⇒ `replace`, comme avant. Aucun client
  existant ne change de comportement.

### Négatives / dette assumée

- **Une réécriture complète de `history.json` par lot** : `appendData()` appelle
  `saveToFile()` à chaque appel, soit **8 réécritures** pour 339 points, depuis
  `async_tcp` (latence, usure de partition). Un `flush` de fin de séquence supposerait
  une **notion de session** côté appareil — précisément ce qu'on a refusé, puisque
  l'autonomie de chaque lot est ce qui rend la reprise possible. Préexistant
  (`importData()` sauvegardait déjà par appel), atténué par le filtre 503.
- **`saveToFile()` est `void`** : elle peut sauter en silence alors qu'`appendData()`
  rend `true`. Préexistant, consigné.
- **L'état de reprise vit en mémoire de page** : recharger l'onglet le perd. Acceptable,
  puisque « Compléter » est idempotent — relancer l'import complet est sans risque.
- **Un `continue` sur timestamp invalide** écarte de **vrais** entrants qui ne sont
  comptés **nulle part** (`count` = points **retenus**). Préexistant, **ouvert**.
- **Effet de bord d'une interface en cache** : une page antérieure POSTant ~130 points
  (~14 Ko) passait sous 16 384 et reçoit désormais un **413**. D'où le message « rechargez
  la page », côté interface **et** côté documentation.

### Ce que ça verrouille

- **Le contrat public de `/history/import`** : le corps est un objet, le champ `mode`
  est optionnel, son **défaut est `replace`**, toute valeur inconnue vaut `replace`. Ce
  défaut ne peut plus changer sans casser tout client antérieur à la v2.48.0.
- **La sémantique de `dropped`** : points **évincés de l'historique**, jamais entrants
  refusés. Un client la traduit littéralement ou mentira à l'utilisateur.
- **L'ordre d'enregistrement garde-puis-handler** : `registerJsonBodyGuard()` **doit**
  précéder `addHandler()`. Permuter les deux lignes rend la garde inerte **sans rien
  casser visiblement** — d'où une sentinelle structurelle dédiée (mutant M7).
- **Le plancher du budget de lot client** : > 4 095 o, sous peine de ramener le
  tamponnage dans le tas interne. Asserté.
- **Le firmware ne parsera pas de JSON incrémental sur ce chemin.** Rouvrir la question
  demande de rouvrir cet ADR, avec au minimum une machine à états **extraite en logique
  pure** et testée en natif ([ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md))
  — la leçon du pied jamais émis.

## Références

- Code : [`src/history_import_logic.h`](../../src/history_import_logic.h) ·
  [`src/history.cpp`](../../src/history.cpp) (`appendData`, `trimToCaps`) ·
  [`src/web_helpers.cpp`](../../src/web_helpers.cpp) (`registerJsonBodyGuard`) ·
  [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp) ·
  [`src/constants.h`](../../src/constants.h) (plafonds + `static_assert`) ·
  [`data/app.js`](../../data/app.js) (`chunkHistoryPoints`, `sendHistoryChunks`)
- Tests : `test/test_native_history_import/` — 72 cas, 7 mutants tués
- Spec : [`specs/features/done/bug-101-import-historique-plafonne-a-16-ko.md`](../../specs/features/done/bug-101-import-historique-plafonne-a-16-ko.md) ·
  [`specs/features/done/bug-113-tamponnage-json-non-authentifie.md`](../../specs/features/done/bug-113-tamponnage-json-non-authentifie.md)
- Doc : [`docs/API.md`](../API.md#post-historyimport--write) ·
  [`docs/subsystems/history.md`](../subsystems/history.md) ·
  [`docs/subsystems/web-server.md`](../subsystems/web-server.md) ·
  [`docs/features/page-settings.md`](../features/page-settings.md)
- ADR liés : [ADR-0061](0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md) (seuil 4 095) ·
  [ADR-0067](0067-garde-503-avant-fopen-handler-prioritaire.md) (handler prioritaire) ·
  [ADR-0058](0058-gardes-heap-metrique-8-bits.md) (métrique 8 bits) ·
  [ADR-0068](0068-partitions-layout-v5-flash-8-mo.md) (l'événement déclencheur) ·
  [ADR-0073](0073-chiffre-consigne-porte-sa-methode.md) (chiffres et méthode)
