# Tests automatisés — ce qui se vérifie **sans** le matériel

> **Document frère** : [`test-on-target.md`](test-on-target.md) couvre ce que cette page ⛔ **ne
> peut pas** atteindre, et [`test-materiel-manuel.md`](test-materiel-manuel.md) en donne la
> procédure encadrée.
> ⭐ **La frontière entre les deux est la raison d'être de ces trois documents** : un test logiciel
> simulé ⛔ **n'est JAMAIS** une validation du matériel. Voir [§6](#6-ce-que-ces-tests-ne-prouvent-pas).

## 1. Ce qui existe déjà — ⚠️ mesurer, ⛔ ne pas recopier

⛔ **Aucun compteur n'est écrit en dur dans ce document** (ADR-0073 : les compteurs recopiés
pourrissent, ce projet en a déjà laissé trois mentir pendant des semaines — ⚠️ et il en reste un à
`test-on-target.md:3`, qui annonce « 118 tests » alors que la suite en compte des milliers).

| Couche | Ce qu'elle teste | Comment la **compter** |
|---|---|---|
| **Suites natives** (ADR-0017) | la **logique pure** extraite de `src/`, hors Arduino | `ls -d test/test_native_* \| wc -l` ; le total de cas est la **dernière ligne** de `pio test -e native` |
| **Harnais de vecteurs JS** (ADR-0099) | le **JS vivant** de `data/app.js`, évalué sous Node | `ls test/*_vectors.js \| wc -l` |
| **Sentinelles** | des **invariants statiques** du dépôt | `ls tools/check_*.sh \| wc -l` |
| **Couverture** | lignes/branches de la logique pure | `./tools/coverage.sh` (gcovr) |

**Intégration continue** : `.github/workflows/` — `tests-natifs.yml`, `build-firmware.yml`
(la compilation est un **gate**), `hygiene-specs.yml`.

---

## 2. 🔴 La contrainte du monolithe — et pourquoi elle est **déjà résolue**

`data/app.js` est **un seul fichier**, volontairement : il réduit les échanges navigateur ↔ ESP32 et
tient dans les ressources de l'appareil. ⛔ **Ce document ne propose à aucun moment de découper les
ressources servies.**

⭐ **Le projet a déjà sa réponse, et elle est antérieure à cette page : [ADR-0099](adr/0099-tester-par-extraction-de-texte-jamais-par-duplication.md) — « tester par EXTRACTION de texte, jamais par duplication ».**

**Le mécanisme**, factorisé dans [`test/js_extract.js`](../test/js_extract.js) :
1. le harnais **lit `data/app.js` tel qu'il est livré** ;
2. il en **isole** la fonction visée, par **ancre + comptage de profondeur d'accolades** ;
3. il **évalue ce texte** sous Node, dans un DOM factice ;
4. il **assertte** le comportement.

⇒ **Le format de livraison ne bouge pas d'un octet.** ⛔ Aucun module ES, ⛔ aucun bundler,
⛔ aucune duplication du code dans le test.

⚠️ **Et c'est ce qui donne sa valeur au test** : *un test qui recopie le code ne teste que la
copie.* Si quelqu'un renomme la fonction ou déplace l'ancre, l'extraction **échoue** — ⛔ elle ne
passe pas au vert en ne gardant plus rien.

> ⭐ **Règle à retenir pour tout harnais neuf** : il **échoue fermé**. Ancre absente, fichier
> introuvable, région vide ⇒ **rouge**. Une sentinelle qui ne trouve plus ce qu'elle garde et rend
> `0` est **pire** que pas de sentinelle : c'est une garantie fausse.

---

## 3. Ce qui **peut** être vérifié automatiquement

### 3.1 Interface — par extraction du JS vivant
Précédents **réels** dans le dépôt (⛔ pas des exemples inventés) : voir `test/*_vectors.js`.

| Ce qui se teste | Exemple existant |
|---|---|
| Un **texte rendu, mot pour mot**, selon l'état servi | `ph_stale_banner_vectors.js` — les quatre rendus du bandeau selon les dates connues ou non |
| La **visibilité** d'un élément (`hidden`) selon la config | `ph_card_permanent_vectors.js` |
| Le **classement** d'une valeur en état visuel (pastille, ton) | `ph_health_vectors.js`, `dash_pill_vectors.js` |
| L'**ordre des nœuds** et le **focus** | `ph_card_permanent_vectors.js` |
| Les **mots bannis** dans un texte produit | vecteurs « sonde », « refait » |
| L'**accessibilité par comportement** : n'écrire que si le texte a changé, ordre écriture/affichage | `ph_card_permanent_vectors.js` |
| Un **repli** quand une clé servie manque | `ph_tune_vectors.js` |

### 3.2 Logique — valeurs, capteurs et états **simulés**
Les suites natives testent la **logique pure** (ADR-0017), c'est-à-dire du code **extrait de `src/`
et sans Arduino** : on lui passe des valeurs, elle rend un verdict.

**Ce qui se simule honnêtement :**
- une **mesure** (pH, ORP, température), y compris `NaN` et les valeurs hors échelle ;
- un **état** (filtration commandée, présence d'eau selon le mode d'installation, mode de
  régulation, étalonnage insuffisant) ;
- le **temps** (passé en paramètre explicite, ⛔ jamais lu par un `millis()` masqué) ;
- une **réponse d'API** ou une **config servie**, dans les harnais JS ;
- une **corruption** (clé absente, type faux, chaîne au lieu d'un nombre).

⚠️ **Condition de fond** : une logique n'est testable ainsi **que si elle est pure**. ⭐ Quand elle
ne l'est pas, le réflexe du projet est d'**extraire ce qui VARIE** — ⛔ pas la condition d'appel.
*Une fonction pure qui enveloppe un unique test trivial est décorative.*

### 3.3 Invariants statiques — les sentinelles
Elles ne simulent rien : elles **relisent le dépôt** et refusent un motif. Exemples réels :
aucune écriture HTML non littérale (barrière anti-XSS, la CSP restant permissive) · aucune
allocation sur le chemin de journalisation · aucune ouverture de fichier hors du helper unique ·
**parité d'un texte entre un `.cpp` et un `.js`**, octet pour octet.

### 3.4 Rendu visuel — ⚠️ **photographié, ⛔ PAS asserté**
`./tools/ui_shot.sh` affiche `data/` dans un navigateur sans interface, **avec les données réelles
recopiées de l'appareil**, et enregistre un PNG. Options utiles : `-w` largeur (⚠️ **390 px
d'abord**), `-r` route, `-f` forcer un champ de `/data`, `-c` un champ de `/get-config`,
`-k` cliquer avant la capture, `-n` faux WebSocket.

🔴 **C'est un outil d'OBSERVATION, pas un test** : il ⛔ **n'assertte rien**. L'image est lue par un
humain (ou par l'agent d'ergonomie).
⭐ **Et ce n'est pas un défaut mineur** : deux défauts réels de ce projet n'ont été vus **que sur
capture** — un lien de retour coincé entre deux cartes, et une même valeur affichée deux fois à
2 cm d'écart. ⛔ Ni le code ni les vecteurs ne les montraient.

---

## 4. Scénarios prioritaires, et le résultat attendu

Classés par **ce que coûte l'erreur**, ⛔ pas par facilité.

| # | Scénario | Résultat attendu | Couche |
|---|---|---|---|
| **P1** | Une **garde de dosage** refuse quand elle doit refuser (mesure absente, filtration non commandée, étalonnage insuffisant) | refus **et** cause **nommée** | native |
| **P2** | Les **filets bruts** de pH ne bougent **jamais** avec la correction de mesure | seuils identiques pour toute correction | native |
| **P3** | Un **verdict servi** n'est jamais recalculé côté navigateur (ADR-0100) | le client **lit la clé**, ⛔ ne dérive rien | JS + sentinelle |
| **P4** | **Parité** `/data` ↔ WebSocket : même clé, **même expression** | appel au **même accesseur** des deux côtés | native + sentinelle |
| **P5** | Un **texte imposé** par un gate est rendu **mot pour mot** | égalité stricte | JS |
| **P6** | Un **repli** quand une clé servie manque | ⛔ jamais un défaut codé en dur, ⛔ jamais une valeur inventée | JS |
| **P7** | Un **geste destructif** demande un second geste au-delà du seuil **servi** | friction armée, défaut sûr en premier | JS |
| **P8** | **Accessibilité** : rendu identique ⇒ **zéro écriture** | pas de relecture par lecteur d'écran | JS |

⭐ **Et la règle qui vaut pour tous** : **un test neuf se valide par MUTATION.** On casse
volontairement le code qu'il protège, et on vérifie qu'il **meurt**. ⛔ *Une suite qui ne tue aucun
mutant ne prouve rien* — plusieurs défauts réels de ce projet n'ont été trouvés que comme ça.

---

## 5. Outils et commandes

```bash
pio run                     # compilation — gate obligatoire
pio test -e native          # suites natives (le total de cas est la DERNIÈRE ligne)
node test/<nom>_vectors.js  # un harnais JS seul
./tools/check_<nom>.sh      # une sentinelle
./tools/coverage.sh         # couverture (gcovr)
./build_fs.sh               # système de fichiers (après toute modif de data/)
./tools/ui_shot.sh -n -w 390 -r '#/ph'   # capture — OBSERVATION, pas assertion
```

⚠️ **Dépendances déjà présentes** : PlatformIO (env `native` pour les suites, `esp32dev_core3`
pour la cible), **Node** (harnais JS), `gcovr` (couverture), Chromium sans interface (captures).
⛔ **Ce document n'en propose aucune nouvelle.**

🔴 **La compilation est une ressource EXCLUSIVE et GLOBALE** : ⛔ jamais deux `pio run` /
`pio test` / `build_fs.sh` / `ui_shot.sh` en parallèle. Un incident réel a bloqué la machine.
⇒ Avant de lancer : `uptime` et `pgrep -af "cc1plus|platformio"`.

**Variable utile** : plusieurs harnais acceptent un chemin de substitution
(ex. `PH_STALE_APP_JS=/tmp/copie.js node test/ph_stale_banner_vectors.js`) ⇒ ⭐ c'est ainsi qu'on
**mute sur une copie** sans toucher au code livré.

---

## 6. Ce que ces tests ⛔ NE prouvent PAS

🔴 **Aucun test de cette page ne valide le matériel.** Ni les sondes, ni les relais, ni les pompes.

| ⛔ Hors d'atteinte | Pourquoi |
|---|---|
| Sondes pH / ORP (EZO, I²C) | aucune simulation d'un EZO ; une sonde **figée** rend une valeur plausible |
| Sondes de température (OneWire) | présence, adressage, câblage |
| **Relais** filtration / éclairage | un bit de sortie ⛔ n'est pas un contact qui colle |
| **Pompes doseuses** (PWM) | un rapport cyclique ⛔ n'est pas un volume injecté |
| **Circulation d'eau réelle** | ⚠️ l'appareil **déduit** la présence d'eau de l'état **commandé** ; ⛔ il ne mesure aucun débit |
| RTC, I²C, LittleFS, WiFi, MQTT, OTA | intégration, pas logique |
| Rendu visuel réel, lecteur d'écran | les captures ⛔ n'assertent rien |

⚠️ **Le piège de vocabulaire à ne jamais franchir** : un vecteur vert sur une garde de dosage prouve
que **la décision** est juste pour les entrées fournies. ⛔ Il ne prouve **pas** que la pompe s'est
arrêtée. ⭐ *Un vecteur natif ne prouve JAMAIS un site d'appel* — ça, c'est le travail d'une
sentinelle ; et qu'un actionneur ait obéi, c'est le travail d'un essai **sur matériel**.

---

## 7. ⚠️ Manques connus — signalés, ⛔ pas comblés ici

1. **Aucune assertion sur le rendu.** `ui_shot.sh` photographie, un humain juge. ⇒ Une régression
   purement visuelle **passe la CI**.
2. **Aucune simulation d'EZO / de capteur au niveau pilote.** Les suites natives s'arrêtent à la
   logique pure ; la couche I²C n'est pas testée hors matériel.
3. **Aucun test de contrat HTTP contre un firmware en marche.** La parité `/data` ↔ WS est vérifiée
   **par lecture de code**, ⛔ pas par deux requêtes comparées sur l'appareil.
4. ⚠️ **`test-on-target.md:3` annonce « 118 tests »** — chiffre **périmé**, exactement le travers
   qu'ADR-0073 interdit. ⇒ À corriger dans le document concerné, ⛔ pas ici.

⛔ **Ces manques ne sont pas des propositions de chantier** : ce document les **nomme** pour qu'on
ne croie pas la couverture plus large qu'elle n'est.
