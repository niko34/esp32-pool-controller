# bug-101 — L'import d'historique est plafonné à 16 Ko, et l'échec est muet

## Métadonnées

| Champ | Valeur |
|---|---|
| Type | bug — limite non documentée + échec non diagnosticable |
| Statut | **done (2026-09-07)** — livré en **v2.48.0**, tests verts, revue OK. ⚠️ Recette **on-target non jouée** (voir fin de fiche) |
| Priorité | Modérée — sans conséquence en régime normal, **bloquante le jour d'une restauration** |
| Découvert | 05/09/2026 ~19:20, en restaurant l'historique après le passage au layout v5 (8 Mo) |
| Version | découvert en 2.42.0 — **corrigé en 2.48.0** |

## Domaines touchés

- `src/web_routes_data.cpp` — enregistrement du handler `/history/import` (~l. 1024)
- `data/app.js` — l'écran d'import côté interface
- `docs/API.md`

## Sécurité chimique

**Non.** Aucun chemin de dosage. Gate `pool-chemistry` non requis.

## Le problème, constaté en vraie grandeur

Après le changement de table de partitions du 05/09, l'historique sauvegardé
(**245 points**) devait être réimporté. Seuls **145 points ont pu l'être** : au-delà,
la route répond **HTTP 413**. Les 100 points les plus anciens (23 → 31 août) sont
restés dans le fichier de sauvegarde.

Le plafond est **16 384 octets exactement** : c'est
`AsyncCallbackJsonWebHandler::_maxContentLength`, initialisé en dur par la
bibliothèque et **jamais surchargé** dans ce projet.

Trois défauts distincts, qu'il ne faut pas confondre :

1. **La limite n'est écrite nulle part.** Ni dans `docs/API.md` (corrigé depuis), ni
   dans l'interface. On la découvre le jour où l'on restaure, c'est-à-dire le pire
   jour possible.
2. **L'échec est muet.** Le `413` est émis par la bibliothèque **avant**
   `REQUIRE_AUTH` et ne passe pas par `sendErrorResponse()` : pas de JSON d'erreur
   maison, donc côté client rien n'explique ce qui s'est passé.
3. **La route REMPLACE l'historique, elle n'ajoute pas.** Un envoi par lots
   successifs écraserait donc le lot précédent — l'utilisateur qui « découpe pour
   contourner » perdrait tout sauf le dernier morceau, en silence.

## ⚠️ Le correctif évident est le mauvais

`setMaxContentLength()` au moment de l'enregistrement suffirait techniquement. Mais
`AsyncCallbackJsonWebHandler` fait un **`calloc(total + 1)`** pour accumuler le corps :
relever le plafond, c'est agrandir d'autant une allocation contiguë, **sur un
appareil dont tout le chantier `bug-080` porte précisément sur la famine 8 bits et
la fragmentation**. Un import de 250 points demanderait ~28 Ko contigus — c'est-à-dire
plus que le plus grand bloc disponible en régime établi (13 300 o mesurés le 05/09).

**Ce serait donc offrir un `abort()` à l'utilisateur exactement quand il restaure ses
données.**

## Pistes à arbitrer

1. **Découpage côté interface** (piste privilégiée) : `data/app.js` envoie des lots
   sûrs et l'API gagne un mode **« ajouter »** distinct du mode « remplacer ».
   C'est le seul chemin qui ne touche pas au budget mémoire.
2. **Streaming côté firmware** : traiter le corps au fil de l'eau plutôt que de
   l'accumuler. Plus propre, plus coûteux à écrire, à chiffrer en flash — mais la
   marge est désormais confortable (1,9 Mo depuis le layout v5).
3. **Format compact dédié à la restauration** (CSV, ou JSON à clés courtes) : le
   fichier persisté utilise déjà `pf`/`of` là où l'API expose `phFiltered`/`orpFiltered`.
   Un import au format persisté diviserait le volume.
4. **Au minimum, et quoi qu'il arrive** : un message d'erreur intelligible côté
   client, et la limite affichée **avant** l'envoi.

## Critères d'acceptation

- [x] Un historique complet (≥ 250 points) se restaure sans perte silencieuse
      — découpage client par **octets mesurés** (`chunkHistoryPoints`, budget
      **7 168 o**) + mode `append` ; fixture `history-339.csv` couverte au banc.
      ⚠️ **Reste à confirmer sur l'appareil** (recette ci-dessous).
- [x] Un dépassement produit un message compréhensible, pas un échec muet
      — `413`/`503` **en JSON maison** via `registerJsonBodyGuard()`, limite
      affichée **avant** l'envoi, `dropped` remonté et affiché.
- [x] Si un mode « ajouter » est introduit, la distinction avec « remplacer » est
      explicite dans l'API **et** dans l'interface — champ `mode` documenté
      ([`docs/API.md`](../../../docs/API.md)), **quatre** boutons distincts
      ([`docs/features/page-settings.md`](../../../docs/features/page-settings.md)).
- [x] Aucune allocation contiguë supplémentaire notable — **chiffré, pas supposé** :
      fusion **en place**, zéro tampon ; le seul contigu reste 339 × 28 =
      **9 492 o**, verrouillé par `static_assert`. ⚠️ Le relevé
      `psram_free` / `max_alloc_8bit` on-target reste à faire.
- [x] Test natif du découpage / de la limite + validation par mutation
      — `test/test_native_history_import/` (**72 cas**), **7 mutants / 7 morts**.

## Notes

Trouvé en situation réelle, pas en revue : c'est la restauration après le
changement de partitions qui l'a révélé. ⚠️ **CETTE PROMESSE EST CADUQUE EN L'ÉTAT — constaté le 2026-09-07.**
`builds/sauvegarde-avant-flash-8mo/history.json` **n'existe pas dans le conteneur de
développement**, et aucun `history.json` n'existe sur la machine (`find / -name
history.json` : vide). L'explication est structurelle : `builds/` est **ignoré par git**
(`.gitignore:23`) et délibérément **local**. Or le développement a migré du Mac vers un
conteneur LXC le 05/09, et la sauvegarde a été faite le même jour **avant** le flash :
elle est restée **sur le Mac**.

**Conséquence** : les 100 points du 23 au 31 août ne sont récupérables que tant que
l'ancien dépôt local du Mac n'est pas nettoyé. Le geste à faire est de sortir ce fichier
de `builds/`, qui n'est ni versionné ni synchronisé. Ce correctif reste utile sans lui
— pour la prochaine restauration — mais **le critère d'acceptation « un historique
complet se restaure » devra être éprouvé sur un jeu SYNTHÉTIQUE**, pas sur la
sauvegarde réelle.

## Notes techniques — passage `test-engineer` (2026-09-07)

**Suite neuve** : `test/test_native_history_import/` (72 cas). Natif :
40 suites / 1 535 cas → **41 suites / 1 607 cas**, 100 % vert.
**Fixtures** : `test/fixtures/*.csv` (6 fichiers) régénérables par
`python3 tools/gen_history_fixture.py` — **synthétiques et déterministes**
(PRNG à graine fixe, instant de référence figé : deux exécutions donnent un diff
git vide). La sauvegarde réelle du 05/09 est restée **hors dépôt**
(`/root/pool-backups/2026-09-05-avant-flash-8mo/`, elle porte l'IP du broker MQTT
et le SSID) : elle a servi à **calibrer** les formes — distribution
77 RAW / 168 HOURLY, cellules manquantes, séparateur d'espace de l'export
hérité, colonne `dosing` booléenne sans `dosing_mask` — jamais à les remplir.
`test_fixtures_ne_contiennent_aucun_secret` verrouille cette règle.

### ⚠️ Correction d'un chiffre : 148 o/point, pas 211-213

Le brief de test annonçait **211-213 o/point** pour le pire cas d'octets. Mesure
refaite au node du poste, en exécutant les **vraies** fonctions extraites de
`data/app.js` (`parseHistoryCsv` → `chunkHistoryPoints`) sur les fixtures :

| forme de la charge | o/point |
|---|---|
| point typique (`%.1f`, `null`) | **104-110** |
| 17 chiffres significatifs, forme **réellement postée** (7 clés) | **148** |
| la même **+ `phFiltered`/`orpFiltered`** | **212** |

Les deux chiffres sont justes, mais **212 n'est pas atteignable aujourd'hui** :
`parseHistoryCsv` ne produit pas les mesures filtrées, donc la charge ne les
porte pas. Le pire cas **réel** est 148 o.
**Ce n'est pas un défaut** — le découpage étant fait par **octets mesurés**, il
s'adapte au poids constaté quel qu'il soit ; c'est un découpage « par N points »
qui aurait cassé. `history-worst-bytes.csv` porte tout de même les colonnes
filtrées, et `test_parite_un_point_du_pire_cas_tient_toujours_dans_un_lot` borne
contre **212**, pas 148 : le jour où les filtrées rejoindront la charge,
l'invariant tiendra déjà.

### Campagne de mutation — 7 mutants, 7 morts, 0 survivant

| Mutant | Fichier | Tué par |
|---|---|---|
| M1 `historyImportModeIsReplace` rend `false` par défaut | `history_import_logic.cpp` | 6 cas de la partie 1 (`mode_absent`, `mode_vide`, `mode_replace`, `mode_inconnu`, mutation d'un caractère) |
| M2 `if (gran != RAW)` ajouté dans `historyImportSameSlot` | `history_import_logic.cpp` | 7 cas — `sameslot_couvre_explicitement_raw`, `idempotence_exacte`, `dix_reenvois`, `lot_raw_reenvoye_ne_double_pas`, `lot_portant_deux_fois_le_meme_creneau`, matrice exhaustive |
| M3 `trimToCaps` purge les points **récents** (`rbegin/rend`) | `history.cpp` | `sentinelle_trimToCaps_delegue_le_plan_et_purge_les_anciens` |
| M4 budget client porté à 9 000 o | `data/app.js` | 3 cas de parité (plafond, égalité firmware, marge 1 024) |
| M5 `>` → `>=` sur le plafond du garde | `web_helpers.cpp` | `sentinelle_garde_compare_strictement_le_plafond` |
| M6 (bonus) `break` : écrase la **première** occurrence | `history.cpp` | `sentinelle_appendData_ecrase_toutes_les_occurrences` |
| M7 (bonus) garde enregistré **après** son handler | `web_routes_data.cpp` | `sentinelle_route_import_gardee_avant_son_handler` |

⚠️ **M3, M5, M6 et M7 ne meurent que par sentinelle structurelle** —
`history.cpp`, `web_helpers.cpp` et `web_routes_data.cpp` ne compilent pas en
natif. Ces sentinelles lisent donc du code **dépouillé de ses commentaires**
(`readProjectCode`) et visent chaque point **dans le corps de sa fonction**,
extrait par signature. Ce n'est pas un raffinement : la sentinelle de
`test_native_temp_compensation` était, le même jour, satisfaite par une simple
**mention en commentaire** de `clampTempCalOffset()`, et supprimer l'appel réel
laissait la suite verte. Trois méta-tests (`test_outillage_*`) vérifient l'outil
avant que les sentinelles s'en servent.

**Non-régression structurelle** : `recordDataPoint()` garde son bornage
`MAX_RAW_POINTS`, le pas 4 de `consolidateData()` ses `MAX_HOURLY_POINTS` /
`MAX_DAILY_POINTS` et son `dedupAggregatedPoints()` ; aucun des deux n'appelle
`trimToCaps()`. `sizeof(DataPoint) == 28` et l'ordre de l'enum `Granularity`
(RAW=0 < HOURLY=1 < DAILY=2, valeur écrite dans la colonne `granularity` des
fixtures) sont verrouillés eux aussi.

### Recette on-target (reste à jouer sur l'appareil)

Relever `psram_free`, `free_heap_8bit` et `max_alloc_8bit` sur
`GET /get-system-info` **avant** et **après** chaque étape.

1. `history-339.csv` en **« remplacer »** ⇒ `GET /get-history?range=all` rend
   **339** points, dernier lot `dropped=0`, aucun doublon `(timestamp, granularité)`.
2. **Réimport du même fichier** en **« compléter »** ⇒ **encore 339**, `dropped=0`
   (idempotence prouvée sur l'appareil, pas seulement au banc).
3. `history-overflow.csv` (200 HOURLY) ⇒ `dropped=32` **remonté et affiché**,
   et les 32 partis sont les **plus anciens**.
4. `history-legacy.csv` ⇒ les points dosés portent **0x04**, jamais 0x01.
5. `history-hostile.csv` ⇒ **7 points sur 9 lignes** (les 2 sans timestamp
   exploitable sont sautées), aucun pH 0 inventé, aucun plantage.
6. Coupure Wi-Fi au **3ᵉ lot** ⇒ historique **partiel et cohérent** (aucun
   doublon), bouton « reprendre » visible, reprise **sans doublon** et total final
   identique à l'import sans coupure.

**Invariant mémoire à confirmer** : l'interne (`max_alloc_8bit`) ne bouge pas de
plus de **9 492 o** (le vecteur, 339 × 28) et la **PSRAM absorbe** les tampons de
corps — c'est la mesure qui confirmera sur l'appareil le désassemblage de
l'allocateur (`heap_caps_malloc_default` oriente vers la PSRAM dès que
`size > 4 095`).

## Clôture (2026-09-07, v2.48.0)

**Fichiers livrés** : `src/history_import_logic.{h,cpp}` (neuf) ·
`src/history.{h,cpp}` (`appendData`, `trimToCaps`) · `src/web_helpers.{h,cpp}`
(`registerJsonBodyGuard`) · `src/web_routes_data.cpp` · `src/web_routes_config.cpp` ·
`src/web_routes_sensor_id.cpp` · `src/constants.h` (plafonds + 4 `static_assert`) ·
`data/app.js` · `data/index.html` · `test/test_native_history_import/` ·
`test/fixtures/*.csv` · `tools/gen_history_fixture.py`.

**Verdicts** : `pio test -e native` **41 suites / 1 607 cas**, 100 % vert ·
`pio run` après `-t clean` **SUCCESS**, **6 warnings tous préexistants**, Flash
**49,6 %** · mutation **7/7** · `code-reviewer` : **aucun défaut de sûreté**.

**Décision consignée** :
[ADR-0075](../../../docs/adr/0075-decoupage-client-contre-streaming-firmware.md) —
découpage client contre streaming firmware. `bug-113` est **close par ce lot**.

**Documentation** : `docs/API.md`, `docs/UPDATE_GUIDE.md`, `docs/PASSATION.md`,
`docs/subsystems/history.md`, `docs/subsystems/web-server.md`,
`docs/features/page-settings.md`, `docs/user/src/guide-utilisation.html`,
`CHANGELOG.md` `[2.48.0]`.
📸 **Capture à refaire** : `screenshots/10_Paramètres_8_Avancé_1.jpg`.

### ✅ Divergence 148/149 tranchée par exécution (ADR-0073)

L'écart de 1 octet entre les notes de test (**148**) et `src/constants.h` (**149**) a
été tranché en réexécutant `parseHistoryCsv` + `chunkHistoryPoints` extraites de
`data/app.js`. **148 o est le bon chiffre** pour la fixture
`test/fixtures/history-worst-bytes.csv` ; le 149 venait d'un cas particulier — un ORP
**négatif**, dont le signe coûte l'octet manquant.

Mais la vérification a trouvé plus grave que l'écart cherché. En balayant le domaine
au lieu de la seule fixture :

| forme | o/point | points/lot |
|---|---|---|
| point typique | 105-109 | ~64 |
| fixture pire cas (tout positif) | **148** | 48 |
| ORP négatif | 149 | 47 |
| ORP **et** température négatifs | 150 | 47 |
| valeur sous `1e-6` → notation exponentielle | 156 | 45 |
| **maximum théorique** | **172** | 41 |

Le maximum théorique vaut 97 o de squelette de clés plus 3 × 25 o, **25** étant la
longueur JSON maximale d'un `double` (balayage de tous les exposants : le pire est
`-0.0000012345678901234567`). Il est atteint dès qu'une valeur passe sous `1e-6` —
donc par un **ORP qui traverse zéro** ou une **température proche de 0 °C**, deux
situations physiques, pas des cas d'école.

⇒ **Les 157 o que `constants.h` présentait comme une « borne conservatrice » n'en
étaient pas une** : 172 la dépasse. Le chiffre avait été **emprunté au pire cas de
l'export firmware** (`formatHistoryPointJson`, 9 clés en `%.1f`, `constants.h:467`) —
une quantité voisine mais différente, calculée sur un autre format. C'est la
quatrième valeur fausse de cette lignée, après 211-213 et 149.

**Rien n'a cassé, et pour une raison de conception, pas de chance** : le découpage
mesure les octets réels (`jsonByteLength`) au lieu de diviser un budget par un poids
supposé. Vérifié par exécution — 200 points au pire cas donnent 5 lots, charge
maximale 7 085 o, aucun 413. C'est l'argument à opposer à toute proposition future de
remplacer cette mesure par une division : quatre chiffres se sont succédé, tous faux,
et l'invariant n'a jamais bougé. Corrigé dans `src/constants.h`, `data/app.js`,
`docs/API.md` et `docs/features/page-settings.md`, avec l'historique des chiffres faux
conservé sur place pour qu'ils ne reviennent pas.

### 📋 Défauts ouverts, consignés sans traitement

1. **`appendData()` appelle `saveToFile()` par lot** — 8 réécritures complètes de
   `history.json` pour 339 points, depuis `async_tcp` (latence, usure). Un `flush` de
   fin de séquence supposerait une **notion de session** côté appareil, refusée : c'est
   l'autonomie de chaque lot qui rend la reprise possible. **Préexistant**, atténué par
   le filtre 503.
2. **`saveToFile()` est `void`** et peut **sauter en silence** alors qu'`appendData()`
   rend `true` : la route répond `200` avec un `total` juste **en RAM**. **Préexistant.**
3. **`continue` sur timestamp invalide** ([`src/web_routes_data.cpp:1181`](../../../src/web_routes_data.cpp))
   — écarte de **vrais** entrants, comptés **nulle part** (`count` = points **retenus**).
   ⚠️ **Ne jamais écrire « un entrant n'est jamais refusé ».** **Préexistant.**
4. **Le test anti-secrets dans les fixtures ne mord que sur une liste de mots**
   (`ssid`, `mqtt`, `192.168.`) : un broker en `10.x` passerait. **Garde-fou faible,
   pas faux.**

### Recette on-target — NON JOUÉE, étape humaine indispensable

Le critère décisif ne peut pas être joué au banc : **coupure Wi-Fi au 3ᵉ lot sur 6**,
puis vérifier un historique **partiel et cohérent** (aucun doublon), et une reprise
**sans doublon** dont le total final égale l'import sans coupure. Relever en même temps
`psram_free`, `free_heap_8bit` et `max_alloc_8bit` sur `GET /get-system-info`
**avant/après** : l'invariant est que l'**interne ne bouge pas de plus de 9 492 o** et
que la **PSRAM absorbe** les tampons de corps — c'est la mesure qui **confirmera sur
l'appareil** le désassemblage de l'allocateur. Étapes détaillées dans la section
« Recette on-target » ci-dessus (points 1 à 6).
