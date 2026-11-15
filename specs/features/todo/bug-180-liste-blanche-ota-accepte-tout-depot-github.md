## Métadonnées
- **ID** : bug-180
- **Slug** : liste-blanche-ota-accepte-tout-depot-github
- **Type** : bug — 🔴 **sécurité applicative**
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : 2.89.0
- **Spec(s) liée(s)** : `feature-151` (D3-c : le refus), `bug-176`, `bug-112`

## Domaines touchés
- [ ] Capteurs · [ ] Régulation · [ ] Filtration / Éclairage
- [x] Web — routes HTTP · [x] OTA · [ ] MQTT · [ ] UART · [ ] Authentification · [ ] Historique · [ ] Frontend

## Agents requis
- [x] spec-reader · [x] architect · [ ] pool-chemistry · [ ] ux-ui-designer
- [x] embedded-developer · [ ] web-ui-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer

⚠️ `security-reviewer` : **OBLIGATOIRE** (chaîne OTA).

## Description
La liste blanche des connexions sortantes OTA ne valide que **l'hôte**. Elle accepte donc
**n'importe quel dépôt `github.com`**, et pas seulement celui du projet.

## Contexte
**Relevé par la revue de sécurité de `bug-178`, le 2026-11-15** : `src/web_routes_ota.cpp:70-74`.

Conjugué à deux autres propriétés **actuelles** :
- `/download-update` accepte une **URL fournie par l'appelant**, avec une **empreinte fournie par
  l'appelant** ;
- la signature est **journalisée sans refuser** (`web_routes_ota.cpp:2090`, « aucun refus appliqué
  (D3-a) »), et le **plancher de version est figé à `0.0.0`** (`ota_manifest_logic.h:87-95`).

⇒ **Un attaquant AUTHENTIFIÉ peut faire installer une image arbitraire** hébergée sur un dépôt
GitHub quelconque. ⚠️ Cela exige un **jeton valide** : ce n'est pas une porte ouverte, c'est une
**surface plus large que nécessaire**.

## Comportement attendu
1. La liste blanche valide **le dépôt**, pas seulement l'hôte : seules les URL de releases du
   dépôt du projet (et le domaine d'assets vers lequel GitHub redirige) sont acceptées.
2. ⚠️ **La redirection reste le point délicat** : `github.com` redirige vers
   `objects.githubusercontent.com`, dont le chemin n'est pas prévisible. ⇒ Dire explicitement ce
   qui est vérifié **après** redirection. ⛔ Aujourd'hui l'allowlist **n'est pas revalidée** après
   redirection (établi pendant `bug-176`).
3. ⛔ Aucun refus neuf sur un usage **légitime** : une release du projet doit s'installer comme avant.
4. Le refus doit être **diagnostique** : motif explicite au journal et à l'appelant.

### Cas limites
- Dépôt renommé ou transféré (⚠️ le projet a déjà changé de nom de dépôt ?) — à vérifier.
- Asset servi depuis un autre domaine CDN que celui attendu.
- URL légitime avec paramètres de requête (jetons de téléchargement temporaires).
- ⚠️ Un durcissement trop strict casserait la mise à jour en ligne **en silence** — le motif exact
  de `bug-169`, six semaines sans que personne ne le voie. ⇒ **Le refus doit être bruyant.**

## Comportement actuel
`isUrlAllowed` (`web_routes_ota.cpp:70-74`, `:102-127`) vérifie **l'hôte** et le protocole.
⚠️ Elle journalise par ailleurs **l'URL brute sans borne** (`:105`) — même classe que `bug-112`
(`bug-176` a posé une variante muette pour `/ota-sig`, mais le chemin `/download-update` reste).

## Contraintes techniques
- ESP32, offline first. Logique **pure** testable en natif (ADR-0017).
- ⛔ Aucune dépendance neuve.
- ⚠️ Conserver la borne de blocage (`static_assert`, `constants.h`) et `esp_task_wdt_reset()`.
- ⛔ L'appareil régule un bassin réel et doit continuer à doser.

## Fichiers potentiellement concernés
- `src/web_routes_ota.cpp` — `isUrlAllowed` et ses appelants
- `src/constants.h` — dépôt et domaines attendus
- `docs/subsystems/ota-manager.md`, `docs/API.md`, `CHANGELOG.md`

## Livrables — check-list
- [ ] A1 — Validation du **dépôt**, pas seulement de l'hôte, en logique pure
- [ ] A2 — Politique écrite et **testée** pour l'URL **après redirection**
- [ ] A3 — Refus **diagnostique** (motif au journal et à l'appelant)
- [ ] A4 — URL **bornée avant journalisation** (rejoint `bug-112`)
- [ ] C1 — `docs/subsystems/ota-manager.md`, `CHANGELOG.md`
- [ ] D1 — Suite native : URL légitimes et illégitimes, redirection, cas limites
- [ ] D2 — Validation par MUTATION
- [ ] D3 — ⭐ **Essai de terrain : une mise à jour en ligne RÉELLE fonctionne toujours**

## ADR attendu ?
- **Probable** : non — durcissement d'une garde existante, sans alternative structurante

## Critères d'acceptation
- [ ] Une release du **dépôt du projet** s'installe exactement comme avant
- [ ] Une URL d'un **autre dépôt** `github.com` est **refusée**, avec motif
- [ ] Le refus est **visible** (⛔ pas une panne silencieuse — cf. `bug-169`)
- [ ] L'URL journalisée est **bornée**
- [ ] Suite native verte, mutations tuées

## Plan de test manuel
1. ⭐ Mise à jour en ligne réelle depuis le dépôt du projet ⇒ **fonctionne**.
2. URL d'un autre dépôt GitHub ⇒ refusée, motif lisible au journal.
3. URL hors `github.com` ⇒ refusée (comportement actuel, non régressé).

## Notes pour agent
- ⚠️ **Défaut PRÉEXISTANT**, relevé en auditant `bug-178`. Exige un **jeton valide** : surface trop
  large, pas porte ouverte.
- ⛔ **Le risque de ce lot est de casser la mise à jour en ligne en silence.** C'est exactement ce
  qui est arrivé six semaines durant (`bug-169`). ⇒ L'essai de terrain D3 n'est pas optionnel.
