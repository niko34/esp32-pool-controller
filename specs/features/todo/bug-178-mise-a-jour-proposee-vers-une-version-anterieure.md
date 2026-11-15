## Métadonnées
- **ID** : bug-178
- **Slug** : mise-a-jour-proposee-vers-une-version-anterieure
- **Type** : bug
- **Statut** : todo
- **Priorité** : haute
- **Version cible** : 2.88.1
- **Spec(s) liée(s)** : `bug-176` (découvert pendant son essai de terrain), `bug-169`

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [ ] Régulation (pump_controller, PID)
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [x] OTA
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] Authentification
- [ ] Historique / persistance
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [ ] pool-chemistry
- [x] ux-ui-designer
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

⚠️ `security-reviewer` : **obligatoire** (`web_routes_ota.cpp`, chaîne OTA).

## Description
L'appareil annonce « mise à jour disponible » dès que la version de la release **diffère** de la
sienne — y compris quand elle est **plus ANCIENNE**. Il propose alors un **retour en arrière**
présenté comme une mise à jour.

## Contexte

**Constaté le 2026-11-15**, pendant l'essai de terrain de `bug-176`. L'appareil venait de passer
en **2.88.0** par déploiement local ; la dernière release GitHub était la **2.87.1**.

```
GET /check-update  →  current_version 2.88.0 · latest_version 2.87.1 · update_available TRUE
```

### La cause, en une ligne
```cpp
// src/web_routes_ota.cpp:1866
bool updateAvailable = (latestVersion != currentVersion);
```
⇒ **Inégalité de CHAÎNES**, pas comparaison de versions. `"2.87.1" != "2.88.0"` est vrai, donc
l'appareil propose d'installer la 2.87.1 **par-dessus** la 2.88.0.

### Pourquoi ce n'est pas anodin
⚠️ L'utilisateur qui accepte **réinstalle une version antérieure** : il perd les correctifs de la
version installée, **y compris ceux qui corrigeaient la mise à jour elle-même**. Dans le cas
observé, accepter aurait réintroduit le défaut que `bug-176` venait de corriger, **et** remplacé la
page par celle qui ne sait pas appeler `/ota-sig`.

⚠️ **Le défaut est PRÉEXISTANT** (il n'est pas introduit par `bug-176`) mais il était **invisible** :
l'appareil n'avait jamais une version plus récente que la dernière release. Un déploiement local,
une pré-version, ou une release retirée de GitHub suffisent à le révéler.

⚠️ **Cas aggravant à considérer** : une release **supprimée** ou **remplacée** côté GitHub ferait
basculer tout le parc vers la version précédente, sans que personne n'ait rien demandé.

## Comportement attendu
1. ⛔ **Aucune mise à jour proposée vers une version strictement antérieure.** La comparaison doit
   être **sémantique** (majeur, mineur, correctif), pas textuelle.
2. **Versions égales** ⇒ « à jour », comportement actuel conservé.
3. **Version de l'appareil plus récente que la release** ⇒ état explicite et **non alarmant** :
   l'appareil est en avance, ce qui est normal pendant un développement. ⛔ Ne pas afficher
   « à jour » (ce serait faux), ⛔ ni « mise à jour disponible » (dangereux).
4. ⚠️ **Le retour arrière délibéré doit rester possible** — mais **explicitement**, jamais sous
   l'étiquette « mise à jour ». ⇒ Forme à arbitrer : la **mise à jour manuelle** y pourvoit déjà,
   ce qui peut suffire à ne rien ajouter à l'interface.

### Cas limites
- Format de version inattendu (suffixe, pré-version, chaîne vide, non numérique) ⇒ ⛔ **fail-safe :
  ne rien proposer**, et le dire, plutôt que de deviner.
- `tag_name` avec ou sans « v » initial (⚠️ le code retire déjà le préfixe).
- Nombres à plusieurs chiffres : ⚠️ `"2.9.0"` vs `"2.10.0"` — ⛔ une comparaison lexicographique
  les inverse. C'est le piège classique, il doit être couvert par un test.
- Composants manquants (`"2.88"` contre `"2.88.0"`).

## Comportement actuel
`web_routes_ota.cpp:1866` compare deux `String` par inégalité. Toute différence, dans un sens
comme dans l'autre, vaut « mise à jour disponible ».

## Contraintes techniques
- ESP32, offline first. Logique **pure** et testable en natif (ADR-0017).
- ⛔ Aucune dépendance neuve pour parser une version : quelques lignes suffisent.
- Champs de réponse **ADDITIFS** si un état neuf est exposé.
- ⚠️ **RESPONSIVE** si `data/` est touché : 390 px d'abord, libellés comptés.

## Fichiers potentiellement concernés
- `src/web_routes_ota.cpp` — `:1866` et la construction de la réponse `/check-update`
- `src/` — module de comparaison de versions **à créer**, en logique pure (ADR-0017)
- `data/app.js` — libellé et état de la carte « Mise à jour en ligne »
- `docs/API.md`, `docs/subsystems/ota-manager.md`, `docs/UPDATE_GUIDE.md`, `CHANGELOG.md`

## Livrables — check-list
- [ ] A1 — Comparaison **sémantique** de versions, en module pur testable en natif
- [ ] A2 — `/check-update` ne propose **jamais** une version antérieure
- [ ] A3 — État « appareil en avance » distinct de « à jour » et de « mise à jour disponible »
- [ ] A4 — Format inattendu ⇒ fail-safe, rien de proposé, état explicite
- [ ] B1 — La page rend le nouvel état, libellés français comptés à 390 px
- [ ] C1 — `docs/API.md`, `ota-manager.md`, `UPDATE_GUIDE.md`, `CHANGELOG.md`
- [ ] D1 — Suite native : ⚠️ **`2.9.0` vs `2.10.0`**, égalité, antérieure, postérieure, formats
      invalides, préfixe « v », composants manquants
- [ ] D2 — Validation par MUTATION (⚠️ dont le retour à une comparaison lexicographique)
- [ ] D3 — Essai de terrain : appareil **en avance** sur la release ⇒ rien n'est proposé

## ADR attendu ?
- **Probable** : non
- **Critère(s) candidat(s)** : aucun
- **Sujet pressenti** : aucun — correction d'un défaut, sans alternative structurante

## Éléments visibles (pour doc-writer)
### Endpoints HTTP concernés
- `GET /check-update` — sémantique de `update_available`, et éventuel état additif
### Champs WebSocket
- aucun
### Topics MQTT
- aucun
### Documentation à produire / mettre à jour
- `docs/API.md`, `docs/subsystems/ota-manager.md`, `docs/UPDATE_GUIDE.md`, `CHANGELOG.md`
- `docs/features/page-settings.md` si un libellé change

## Critères d'acceptation
- [ ] Appareil **plus récent** que la release ⇒ ⛔ **rien n'est proposé**, et l'état le dit
- [ ] Appareil **plus ancien** ⇒ mise à jour proposée, comportement inchangé
- [ ] Versions **égales** ⇒ « à jour »
- [ ] ⚠️ **`2.10.0` est reconnue postérieure à `2.9.0`**
- [ ] Version illisible ⇒ rien n'est proposé, aucun plantage
- [ ] Libellés en français · `pio run` sans nouveau warning · `build_fs.sh` passe
- [ ] Suite native verte, mutations tuées

## Plan de test manuel
1. Appareil **en avance** sur la dernière release (⚠️ **situation réelle au 2026-11-15**) ⇒ rien proposé.
2. Appareil **en retard** ⇒ mise à jour proposée et installable.
3. Versions égales ⇒ « à jour ».
4. Release au `tag_name` malformé ⇒ rien proposé, message explicite.

## Notes pour agent
- ⚠️ **Découvert par un essai de terrain, pas par relecture.** Le défaut vivait depuis longtemps et
  restait invisible faute d'appareil en avance sur la release.
- ⚠️ **Ne pas « corriger » en bloquant le retour arrière manuel** : il doit rester possible, mais
  jamais présenté comme une mise à jour.
- ⚠️ L'appareil régule un bassin réel et **doit continuer à doser**.

## Hors périmètre
- Le relais des signatures : `bug-176`.
- Le rechargement aveugle après redémarrage : `bug-177`.
- Le refus sur signature invalide : D3-c de `feature-151`.
