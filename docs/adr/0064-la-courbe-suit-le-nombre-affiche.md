# ADR-0064 — La courbe suit le nombre affiché : la mesure lissée est la référence des courbes 5 min, le brut reste en second plan, les agrégats restent bruts

- **Statut** : Accepté
- **Date** : 2026-09-05
- **Décideurs** : architect, ux-ui-designer, embedded-developer, web-ui-developer, code-reviewer
- **Spec(s) liée(s)** : `specs/features/done/feature-095-historiser-la-mesure-filtree.md` (v2.38.0)

## Contexte

Le contrôleur mesure le pH et l'ORP **deux fois** : il y a la mesure **brute** de la sonde (plus l'offset logiciel pour le pH) et la mesure **lissée** (médiane glissante + EMA). **La régulation ne décide que sur la lissée.** C'est aussi la lissée que l'interface affiche **en grand** sur les cartes du tableau de bord et sur les pages pH/ORP, via `/data`.

Mais l'historique, lui, ne contenait **que le brut**. D'où deux problèmes cumulés :

1. **Le grand nombre et la courbe qui l'accompagne ne parlaient pas du même signal.** Sur la carte pH, la valeur affichée était la lissée, tracée au-dessus d'une courbe brute — deux mauvais diagnostics en sont déjà sortis (mémoire projet : « historique = brut + offset », *bruit de mesure pH*). Une incohérence d'affichage a produit des conclusions fausses sur le comportement de la régulation.
2. **Aucune analyse de la régulation n'était possible sans instrument externe.** Toutes les analyses du plan 2026-09 ont exigé un script interrogeant `/data` toutes les 10-60 s depuis un Mac qui devait rester éveillé — une mise en veille a déjà **troué** un relevé (04/09). Les questions « la porte de session robuste au bruit ([ADR-0062](0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md)) marche-t-elle ? », « quelle est la dérive nocturne ? », « le KPI d'atteinte de cible ([ADR-0063](0063-definition-normative-du-temps-d-atteinte-de-cible.md)) est-il recalculable a posteriori ? » n'avaient aucune donnée persistée pour être tranchées.

La contrainte qui rend l'arbitrage non trivial : la partition `history` fait **64 Ko**, et son dépassement a un mode de défaillance **silencieux** — `historySaveAdmit()` refuse l'écriture, un WARNING throttlé 1/h est journalisé, l'UI continue d'afficher la RAM, et la perte n'apparaît qu'au reboot ([ADR-0033](0033-budget-espace-partition-history.md), [history.md](../subsystems/history.md#le-mode-de-défaillance-du-budget-est-le-silence-pas-la-corruption-)). On ne peut donc pas « voir ce que ça donne » : il faut chiffrer avant.

## Décision

**La courbe suit le nombre affiché.** Quatre règles, indissociables :

1. **Chaque point `RAW` (5 min) porte la mesure lissée** pH/ORP telle que vue par la régulation à l'instant de l'enregistrement — mêmes accesseurs que `/data`, donc superposable au nombre affiché. **On ajoute, on ne remplace pas** : le brut reste indispensable au diagnostic sonde.
2. **La lissée n'est historisée QUE sur les points `RAW`** (~8 h de profondeur). Sur les moyennes **horaires** et **journalières**, elle est **absente du fichier**, **jamais agrégée**, et vaut **`null`** sur `/get-history`.
3. **Sur les courbes 5 min du tableau de bord, la lissée est la série PRINCIPALE** (trait épais, aire, coloration, point de survol) et le brut la série **secondaire**, masquable par un interrupteur mémorisé, **visible par défaut**. **Les graphiques d'historique des écrans détail restent bruts** (ils agrègent 1 point/jour).
4. **Asymétrie assumée entre le fichier et le fil** : `/history.json` utilise des clés **courtes** (`pf`, `of`) et **omet** la clé quand la valeur n'est pas finie ; `/get-history` et `/history/import` utilisent des clés **longues** (`phFiltered`, `orpFiltered`) **toujours présentes**, valeur **`null`** en cas d'absence — le contrat existant de `ph` / `orp`.

**Le motif de la règle 2 est mesuré, pas supposé** : historiser la lissée à **toutes** les granularités coûtait **+7 119 o** (339 points × 21 o) et portait l'occupation à **16 blocs sur 16** ⇒ `historySaveAdmit()` aurait refusé la sauvegarde, et l'historique aurait cessé **silencieusement** d'être persisté. En `RAW` seul : **+2 016 o**, **12/16 blocs**, résiduel **8 102 o**, profondeur horaire (7 j) intacte.

**Second motif, indépendant du budget** : la lissée est un signal **passe-bas** — sa moyenne horaire **coïncide** avec celle du brut. On aurait payé ~5,1 Ko pour une série **redondante**.

## Alternatives considérées

- **Historiser la lissée à toutes les granularités** (rejetée) — **+7 119 o**, `/history.json` ~30,6 Ko, **16/16 blocs**. Ce n'est pas une marge « mince » : c'est un franchissement, avec le mode de panne le plus discret du système. Et la série obtenue aurait été indiscernable du brut aux échelles horaire et journalière.
- **Réduire la profondeur horaire pour financer la lissée partout** (`kMaxHourlyDataPoints` 168 → 120, soit 7 j → 5 j) (rejetée) — on aurait sacrifié une **rétention réelle et utilisée** pour une série **redondante** (cf. ci-dessus). Mauvais échange : on paie une information par la perte d'une autre.
- **Clés courtes (`pf` / `of`) sur le fil HTTP** (rejetée) — économie sans objet : `/get-history` est **chunked**, sans plafond dur, et son coût est du réseau, pas de la flash. En revanche cela aurait cassé la lisibilité de l'API et introduit **deux conventions de nommage sur le même endpoint**. L'asymétrie retenue est motivée : dans le fichier chaque octet compte, sur le fil c'est la stabilité du contrat qui compte.
- **Omettre la clé sur le fil quand la lissée est absente** (rejetée) — aurait forcé chaque client à distinguer « clé absente » de « valeur nulle », alors que `ph` / `orp` / `temperature` disent `null` depuis feature-061. Un contrat, une convention.
- **Historiser aussi le signal lent de décision `pd`** (moyenne glissante `kPhDecisionWindowMs` de la lissée, [ADR-0062](0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md)) (rejetée) — quatre raisons cumulées : il est **dérivable** de la lissée historisée (aucune information nouvelle) ; il coûterait un 3ᵉ champ sur un budget déjà arbitré ; il **importerait `kPhDecisionWindowMs` dans une série longue durée**, que toute modification future de la constante rendrait non comparable **sans l'estampille de série** dont dispose l'anneau KPI ([ADR-0063](0063-definition-normative-du-temps-d-atteinte-de-cible.md)) ; enfin il créerait un **nouveau consommateur** de `PumpController::getPhDecisionValue()`, accesseur sous contrat (loopTask uniquement, sens unique vérifié par grep) — donc un **gate `pool-chemistry`** obligatoire, pour zéro information nouvelle.
- **Remplacer le brut par la lissée** (rejetée) — le brut est le seul signal qui permette de diagnostiquer une sonde, de lire un transitoire de reprise ou de constater un EMI. La lissée le masque **par construction**.
- **Promouvoir la lissée aussi sur les graphiques des écrans détail** (rejetée pour ce cycle) — 1 point/jour : la lissée y vaut toujours `null` et y serait indiscernable du brut. Garder `createLineChart` **mono-série** était la réduction de risque du lot.

## Conséquences

### Positives

- **Le nombre affiché et la courbe qui l'accompagne parlent enfin du même signal** — la cause directe de deux mauvais diagnostics est supprimée.
- **L'analyse de la régulation ne dépend plus d'un enregistreur externe** : plus de Mac à garder éveillé, plus de relevé troué par une mise en veille.
- **La profondeur d'historique est intacte** : aucune constante de rétention n'a été réduite.
- **Ajout purement additif** : un client ancien ignore les clés inconnues, un firmware ancien n'en sert aucune, les points antérieurs à la 2.38 restent lisibles. Aucune migration.
- **Aller-retour complet** : `/get-history` → `/history/import` → `/get-history` restitue la lissée à l'identique — plus de perte silencieuse à la restauration d'une sauvegarde.
- **Bénéfice collatéral** : les points omis de `/get-history` (JSON trop long) ne sont plus sautés **sans un mot**. Ils sont comptés et un WARNING throttlé 1/h l'annonce — un trou de courbe n'est plus indiscernable d'une panne de sonde.

### Négatives / dette assumée

- **La lissée ne couvre que ~8 h d'une fenêtre de 24 h.** L'analyse au-delà d'une journée se fait sur le brut. Traité côté UI, pas côté données : le brut couvre 100 % de la fenêtre, la lissée est une **aire** (une zone qui commence évoque une période, une ligne qui s'arrête évoque une panne) et l'opacité du brut bascule à la **même** frontière — l'œil lit un seul événement.
- **`sizeof(DataPoint)` passe de 20 à 28 o**, verrouillé par `static_assert`. Conséquence directe : l'allocation **contiguë** de `/get-history` passe à **9 492 o d'un seul bloc**, demandés en contexte `async_tcp` sur un appareil dont le plus gros bloc libre tourne autour de 28-30 Ko (bug-080, famine 8 bits). C'est confortable aujourd'hui ; ça ne le restera pas indéfiniment.
- **Le budget de la partition est consommé à 12/16 blocs.** Il reste 8 102 o de résiduel : de quoi absorber un champ, pas trois.
- **`kHistoryPointJsonMaxBytes` = 192 o est une borne documentée, pas une garantie** : un pire cas **théorique** de 213 o existe (toutes les conversions saturant leurs tampons de 15 caractères), inatteignable avec des mesures plausibles. Assumé **parce que** le chemin de saut n'est plus muet.
- **`getPhFiltered()` n'a pas de garde d'âge** : une mesure périmée serait historisée comme fraîche. Volontairement **non corrigé** — c'est exactement ce que `/data` sert au même instant, et une divergence entre l'historique et l'interface serait pire que la nuance. Documenté dans [history.md](../subsystems/history.md#fraîcheur-de-la-mesure-lissée--nuance-connue-aucun-changement).

### Ce que ça verrouille

- **Le contrat public de `/get-history`** : `phFiltered` / `orpFiltered`, clés **toujours présentes**, `null` quand absentes, **`null` sur tout agrégat**. Un client qui trace la lissée doit gérer une série plus courte que celle du brut. Retirer ces clés serait un changement **incompatible**.
- **Toute nouvelle clé du point persisté doit refaire le décompte de la partition**, chiffres à l'appui, avant d'être écrite — la garde `historySaveAdmit()` ne prévient qu'après coup, et en silence.
- **Toute nouvelle clé du point exporté doit refaire le décompte de `kHistoryPointJsonMaxBytes`** (le commentaire de la constante porte le décompte ligne à ligne).
- **La règle « la lissée n'est jamais moyennée »** : le `validCount` partagé de la consolidation reste celui du brut et de la température. Les moyennes horaires et journalières produisent **exactement** les mêmes valeurs qu'avant la v2.38.0 (invariant de *characterization*).
- **Le préfixe `poolctl.ui.` de `localStorage`** et le passage obligatoire par `uiPrefGet` / `uiPrefSet` (accès protégés en lecture **et** en écriture) pour toute préférence d'interface persistée — première utilisation du stockage navigateur dans le projet.
- **Le vocabulaire** : côté utilisateur on dit **brut** et **lissé**. « Filtrée » reste un terme d'API (`phFiltered`), jamais un mot d'interface.

## Références

- Code : [`src/history.h`](../../src/history.h) (`DataPoint`, `static_assert`), [`src/history.cpp`](../../src/history.cpp) (`recordDataPoint`, `saveToFile`, `loadFromFile`, `consolidateData`, `dedupAggregatedPoints`), [`src/history_export_chunk.h`](../../src/history_export_chunk.h) (`formatHistoryPointJson`), [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp) (`/get-history`, `/history/import`), [`src/constants.h`](../../src/constants.h) (`kHistoryPointJsonMaxBytes`, `kHistorySkipWarnThrottleMs`), [`data/app.js`](../../data/app.js) (`createMiniLineChart`, `uiPrefGet`/`uiPrefSet`, `bindMiniRawToggles`)
- Spec : `specs/features/done/feature-095-historiser-la-mesure-filtree.md`
- Doc : [`docs/subsystems/history.md`](../subsystems/history.md), [`docs/API.md`](../API.md#get-get-history--write), [`docs/features/page-dashboard.md`](../features/page-dashboard.md)
- ADR liés : [ADR-0033](0033-budget-espace-partition-history.md) (budget de la partition), [ADR-0062](0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md) (signal lent de décision), [ADR-0063](0063-definition-normative-du-temps-d-atteinte-de-cible.md) (estampille de série), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique pure)
