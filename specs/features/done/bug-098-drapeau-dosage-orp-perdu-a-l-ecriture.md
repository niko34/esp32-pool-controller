# bug-098 — Le drapeau de dosage ORP est perdu à l'ÉCRITURE de l'historique

## Métadonnées

| Champ | Valeur |
|---|---|
| Type | **bug de format persisté** — perte d'information irréversible |
| Statut | **livré — tests verts, revue OK** (2026-09-07) |
| Priorité | Modérée — aucune conséquence sur la régulation, mais l'historique ment |
| Découvert | 05/09/2026, par l'`embedded-developer` pendant le cycle feature-095 |
| Version en cours | 2.39.0 (déployée) |
| **Version de livraison** | **2.47.0** |
| ADR produits | [ADR-0072](../../../docs/adr/0072-absence-de-valeur-hors-du-domaine.md), [ADR-0073](../../../docs/adr/0073-chiffre-consigne-porte-sa-methode.md) |

## ✅ Obstacle levé le 2026-09-06 — il ne reste qu'à écrire le code

Le coût de stockage qui retenait ce correctif **n'est plus un obstacle**. Le layout v5
(ADR-0068) a porté la partition `history` de 64 Ko à **256 Ko** ; le `CHANGELOG` de la
v2.42.0 chiffre le surcoût de la séparation des deux drapeaux à **un octet par point,
339 o au total**, désormais absorbables sans discussion.

> ⚠️ **Ces deux chiffres sont FAUX** (constaté le 2026-09-07) — et la conclusion
> « ce n'est plus un obstacle » reste juste, pour une meilleure raison : le format
> retenu **rétrécit** le fichier. Voir la [rectification de
> clôture](#️-rectification-de-clôture-2026-09-07--trois-chiffres-de-cette-section-étaient-faux).

**Le défaut, lui, est intact** — vérifié dans le code le 2026-09-06 :

```c
history.cpp:573   obj["d"] = point.phDosing || point.orpDosing;   // fusion à l'écriture
history.cpp:750   dp.phDosing = point["d"];                        // relecture partielle
```

`orpDosing` est toujours perdu. La mention « devient trivial à finaliser » du
`CHANGELOG` signifiait **l'obstacle est levé**, jamais **c'est fait**.

## Domaines touchés

- `src/history.cpp` — `saveToFile()` (~474-486), `loadFromFile()` (~610-616)
- `src/web_routes_data.cpp` — `/import-history` (~962-968)
- `src/history.h` — `DataPoint::phDosing` / `orpDosing`
- `docs/subsystems/history.md`, `docs/API.md`

## Sécurité chimique

**Non.** Aucun chemin de dosage, aucune décision de régulation ne lit ces
drapeaux — ce sont des champs d'observation. Le gate `pool-chemistry` n'est pas
requis. À rediscuter **uniquement** si le correctif touchait la structure
`DataPoint` d'une manière qui déplacerait l'allocation de `/get-history`
(verrouillée par `static_assert(sizeof(DataPoint) == 28)`).

## Le problème

`DataPoint` porte **deux** drapeaux distincts, `phDosing` et `orpDosing`. Mais à
la sérialisation, ils sont **fusionnés en une seule clé `d`**. La lecture ne peut
donc pas les séparer : `loadFromFile()` et `/import-history` posent tous deux
`orpDosing = false` faute d'information.

Ce n'est **pas** un bug d'import, comme on l'a d'abord cru en le découvrant sur
`/import-history` : c'est le **format persisté** qui perd l'information. Aucune
relecture, aussi soignée soit-elle, ne peut la retrouver.

Conséquence : sur tout point rechargé depuis `/history.json` — donc après chaque
redémarrage — l'historique affirme qu'aucun dosage ORP n'a eu lieu. Les points
encore en mémoire vive, eux, disent vrai. L'historique est donc **incohérent avec
lui-même** selon l'âge du point.

## Ce qui est demandé

1. Sérialiser les deux drapeaux séparément (deux clés, ou un champ de bits — le
   second économise un octet par point, ce qui compte : voir le budget ci-dessous).
2. Relire les deux clés, en **tolérant leur absence** (les fichiers écrits par
   ≤ 2.39.0 n'ont que `d`) : un point ancien garde `phDosing` depuis `d`, et
   `orpDosing` reste indéterminé — décider si cela se dit `false` ou s'il faut un
   troisième état.
3. Adapter `/import-history` de la même façon.
4. Un test natif de round-trip couvrant les trois cas : fichier ancien (clé `d`
   seule), fichier neuf, et fichier mixte.

## Contrainte de budget — la vraie difficulté

⚠️ **CE PARAGRAPHE SE CONTREDISAIT AVEC L'EN-TÊTE DE SA PROPRE SPEC** — corrigé le
2026-09-07. Il raisonnait encore sur une partition `history` de **64 Ko** et un
résiduel de 8 102 octets, alors que l'ADR-0068 (layout v5) l'a portée à **256 Ko**.
Le budget n'est donc **plus la vraie difficulté** de cette spec : les 339 octets sont
absorbables sans discussion, comme l'annonce l'en-tête. Chiffrer reste utile — ne pas
chiffrer du tout est ce qui a produit quatre budgets faux ailleurs dans ce projet —
mais ce n'est plus un obstacle à l'arbitrage.
Un octet de plus par point coûte **339 octets** ; deux clés séparées en coûtent
davantage. Le mode de défaillance n'est **pas** la corruption mais le **silence** :
`historySaveAdmit()` refuse la sauvegarde et journalise un avertissement une fois
par heure — l'historique cesse simplement d'être persisté.

**Chiffrer avant d'écrire.** Un champ de bits remplaçant `d` est probablement
neutre en taille.

> ### ⚠️ RECTIFICATION DE CLÔTURE (2026-09-07) — trois chiffres de cette section étaient faux
>
> Cette section, et l'en-tête de la spec, annonçaient « **un octet par point, 339 o
> au total** » et « probablement neutre ». Les trois affirmations sont fausses, et
> chacune l'était dans un sens différent :
>
> 1. **« Un octet par point »** confondait un octet de **structure C** avec une clé
>    **JSON**. Séparer les deux drapeaux en deux clés booléennes aurait coûté
>    **~10 o par point**, soit **~3 390 o** sur les 339 points — un ordre de
>    grandeur au-dessus.
> 2. **« 339 o au total »** est le produit de la première erreur, recopié du
>    `CHANGELOG` de la v2.42.0 sans être refait.
> 3. **« Probablement neutre »** sous-estime la variante retenue : le masque entier
>    est **meilleur que neutre**. `,"d":false` coûte 10 o et `,"d":true` 9 o, contre
>    **6 o** pour `,"d":0` … `,"d":7` — donc **−4 o par point** dans le régime
>    dominant. Sur la charge de référence, les clés `d` passent de **3 147 à
>    2 034 o** : **le fichier RÉTRÉCIT de 1 113 o**.
>
> **Méthode** : décompte structurel des longueurs de clé, régime réaliste (96 RAW
> sans dosage, 168 HOURLY et 75 DAILY avec au moins une voie), **asserté** par
> `test_budget_charge_realiste_de_339_points_retrecit`
> (`test/test_native_dose_mask/test_dose_mask.cpp`, section 6). Les trois chiffres
> ci-dessus se **refont** ; les trois précédents ne se **recopiaient**. C'est
> exactement le motif de l'[ADR-0073](../../../docs/adr/0073-chiffre-consigne-porte-sa-methode.md).

## Critères d'acceptation

- [x] **Les deux drapeaux survivent à un redémarrage, prouvé par round-trip** —
      `DataPoint::doseMask` (`kHistDosePh 0x01` / `kHistDoseOrp 0x02` /
      `kHistDoseUnknownChannel 0x04`) écrit en entier par `saveToFile()` et relu par
      `loadFromFile()`. Round-trip exécuté sur un **codec alimenté par le texte des
      sources de production** (`test_native_dose_mask`, `DoseCodec`) : changer
      `& 0x07` en `& 0x03` dans `history.cpp` change le **résultat** des tests, pas
      seulement une assertion textuelle.
- [x] **Un `/history.json` écrit par ≤ 2.39.0 se relit sans perte ni erreur** —
      discriminant par **type JSON**, sans clé de version : `is<bool>()` ⇒ ancien
      format ⇒ `kHistDoseUnknownChannel` ; `is<int>()` ⇒ masque réduit à `0x07` ;
      clé absente ⇒ `0`. Un booléen `true` ne se relit **jamais** « pH ». Trois
      formats couverts (ancien, neuf, mixte) + **deux cycles consécutifs** — c'est
      ce dernier test qui a tué le mutant `& 0x03`, lequel retransformait « voie
      indéterminée » en « aucun dosage » **cinq minutes après l'OTA**.
- [x] **Budget de partition chiffré avant/après, verdict d'admission explicite** —
      ⚠️ **le critère était mal orienté** : le masque **RÉTRÉCIT** le fichier de
      **1 113 o** sur les 339 points de la charge de référence (`,"d":false` 10 o et
      `,"d":true` 9 o → `,"d":0` … `,"d":7` 6 o ; clés `d` : **3 147 → 2 034 o**).
      Aucun verdict d'admission à rendre, `historySaveAdmit()` n'est pas plus
      sollicité qu'avant. Sur le fil, le pire cas d'un point passe de 163 (annoncés)
      à **161 o mesurés** — la marge sur `kHistoryPointJsonMaxBytes = 192` **gagne
      2 o**. Méthode inscrite dans le test et dans `constants.h` ([ADR-0073](../../../docs/adr/0073-chiffre-consigne-porte-sa-methode.md)).
- [x] **Mutation : retirer la séparation → un test meurt** — **13 mutants tués sur
      13**, dont `& 0x07` → `& 0x03` (le mutant décisif), la fusion `||` restaurée à
      l'écriture, la relecture en `kHistDosePh` d'un booléen, et l'union bit à bit
      remplacée par une affectation. Couverture **lignes et fonctions 100 %** sur
      les modules purs touchés.
- [x] **Balayage exhaustif du mode de défaillance silencieux** (critère ajouté en
      cours de cycle) — les **65 536** couples (masque, granularité) sur les valeurs
      les plus longues des cinq conversions flottantes : aucun ne fait sauter un
      point de l'export. Un point sauté est **silencieux** par construction (trou de
      courbe indiscernable d'une panne de sonde) ; le pire cas retenu, **161 o**, est
      le **maximum du balayage** — mesuré, pas postulé.
- [x] **`sizeof(DataPoint) == 28` préservé**, prouvé **par mutation** : l'allocation
      contiguë de `/get-history` (339 × 28 o) en dépend.

## Notes

Découvert en cherchant autre chose, ce qui est le cas habituel. Le développeur
l'a signalé sans le corriger — changer un format persisté est un lot à part
entière, pas un ajout opportuniste en fin de cycle.

## Notes techniques de clôture (2026-09-07, v2.47.0)

### Ce que le défaut faisait vraiment

Il ne **perdait** pas une donnée, il en **fabriquait une fausse et FAVORABLE**.
`d = true` sur une injection **ORP seule** se relisait `phDosing = true` : le
firmware **inventait un dosage pH**, sous-déclarant la consommation de chlore et
surdéclarant celle d'acide — puis `consolidateData()` figeait ce faux positif dans
les agrégats **DAILY** pour 75 à 90 jours, **dans le jeu de données même de la
campagne de mesure du biais de dosage** en cours. Un trou se voit ; un chiffre
flatteur, non. C'est ce qui a motivé l'[ADR-0072](../../../docs/adr/0072-absence-de-valeur-hors-du-domaine.md).

### Décisions d'implémentation

- **Champ de bits, pas deux clés** — et un **troisième état** obligatoire :
  `kHistDoseUnknownChannel` (`0x04`). Sans lui, il fallait choisir entre inventer
  une voie et effacer le fait qu'un dosage a eu lieu.
- **Relecture sur `0x07`, pas `0x03`** : avec `0x03`, le premier cycle
  sauvegarde/relecture suivant l'OTA (**5 min**) écrivait `"d":4` puis relisait
  `0` — le mensonge favorable réintroduit **par le correctif**.
- **Discriminant par type JSON**, sans clé de version : les deux écritures
  possibles sont déjà exactement séparables. ⚠️ `item["dosing"] | false` rend
  `false` sur un **entier** (l'opérateur `|` d'ArduinoJson exige la concordance de
  type) — il aurait effacé tout masque relu d'un export neuf. Lecture **typée** aux
  deux sites d'import.
- **Union BIT À BIT** à l'agrégation (deux boucles de `consolidateData()`) et à la
  déduplication (`dedupAggregatedPoints()`). Un agrégat horaire peut donc porter
  `0x05` ou `0x07` : c'est la lecture littérale du créneau.
- **`anyTrue()` supprimée** de `history_logic.*` **avec ses trois tests** : ce diff
  a tué son dernier appelant, et des assertions vertes sur du code mort font croire
  à un invariant vivant tout en maintenant une couverture de 100 % trompeuse.
- **Colonne CSV `dosing_mask` ajoutée**, `dosing` conservée. Réutiliser le nom
  aurait rendu un `1` **indécidable** — les exports ≤ 2.46 écrivaient déjà `1`.
- **Rétrogradation sûre** : un firmware ≤ 2.46 relisant `"d":3` obtient `true`,
  exactement son comportement actuel.

### Limite assumée du discriminant

Un `"d"` **flottant** (`3.0`) ou entier **64 bits hors plage `int`** ne satisfait
ni `is<bool>()` ni `is<int>()` : il retombe sur `0`, donc sur le mensonge
favorable. **Inatteignable** sans édition manuelle du fichier ou export étranger —
ni le firmware ni l'export de l'interface ne produisent ces formes. Non traité pour
ne pas élargir le discriminant à des types qui rendraient la séparation des deux
formats moins nette.

### Réserve OUVERTE — spec de suivi nécessaire

`historyPointPersistedLen` (`test/test_native_history/test_history_logic.cpp`)
modélise encore `"d":true/false` et une partition de **64 Ko** (256 Ko depuis
l'[ADR-0068](../../../docs/adr/0068-partitions-layout-v5-flash-8-mo.md)). Il se
trompe **du côté sûr** — il annonce le fichier plus **gros** qu'il n'est, de ces
mêmes 1 113 o — donc **rien n'est rouge** et le modèle reste valable comme
**majorant**. Le corriger propage sur une dizaine de constantes asservies
(`23497 / 25513 / 30616 / 2016 / 7119 / 8102 / 4006 / 43008`) et sur **tout le
récit de budget** de la partition. **Une spec de suivi est nécessaire** ; elle n'a
pas été écrite dans ce cycle.

### Fichiers modifiés

`src/history.h`, `src/history.cpp`, `src/history_logic.h`, `src/history_logic.cpp`,
`src/history_export_chunk.h`, `src/web_routes_data.cpp`, `src/constants.h`,
`src/version.h`, `data/app.js`, `test/test_native_dose_mask/test_dose_mask.cpp`
(nouveau), `test/test_native_history/test_history_logic.cpp`,
`test/test_native_history_export/test_history_export.cpp`.

### Documentation

`docs/adr/0072-absence-de-valeur-hors-du-domaine.md` (créé),
`docs/adr/0073-chiffre-consigne-porte-sa-methode.md` (créé), `docs/adr/README.md`,
`docs/README.md`, `docs/API.md`, `docs/subsystems/history.md`,
`docs/features/page-dashboard.md`, `docs/user/src/guide-utilisation.html`,
`CHANGELOG.md`.

⚠️ **Capture à refaire** : `screenshots/03_Tableau_de_bord.jpg` — les marqueurs de
dosage apparaissent sur les mini-graphiques pH et chlore. Seule étape humaine ;
aucun PDF n'a été régénéré dans ce cycle.
