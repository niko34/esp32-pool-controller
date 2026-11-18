# bug-193 — Les handlers `/auth/*` ignorent le découpage du corps HTTP : un corps en deux morceaux tourne deux fois

## Métadonnées
| | |
|---|---|
| Type | bug — **robustesse HTTP** |
| Statut | todo |
| Priorité | basse — ⚠️ **préexistant et non déclenchable** par les corps réels, qui tiennent dans un seul morceau |
| Version cible | à définir |
| Relevé par | `code-reviewer` (mineure 6) **et** `security-reviewer` (cosmétique), en passe P2 de [`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md), 2026-11-17 |
| Specs liées | [`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md) (l'a rendu visible, ⛔ ne l'a pas créé) |

## Domaines touchés
- [x] **Web — routes HTTP** (`src/web_routes_auth.cpp`) · [x] Authentification
- [ ] Capteurs — [ ] Régulation — [ ] Frontend — [ ] MQTT — [ ] OTA

## Agents requis
- [x] spec-reader · [x] architect · [x] embedded-developer · [x] test-engineer · [x] code-reviewer
- [x] **`security-reviewer`** — ⛔ OBLIGATOIRE : le périmètre est `web_routes_auth.cpp`
- [ ] `doc-writer` — ⚠️ à cocher seulement si un comportement observable change
- [ ] `pool-chemistry` · [ ] `ux-ui-designer` — sans objet

## Le fait, vérifié dans le code

`src/web_routes_auth.cpp:146` et `:212` — les deux handlers de corps déclarent bien leurs
paramètres :
```cpp
[](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total)
```
⛔ **mais ils n'utilisent ni `index` ni `total`.** Or AsyncWebServer rappelle ce handler
**à chaque morceau** du corps.

⇒ Un corps reçu en deux morceaux ferait tourner la logique **deux fois** :
- deux réponses HTTP envoyées pour une seule requête ;
- le **compteur d'échecs gonflé** (bug-183) ;
- le quota de requêtes consommé deux fois.

## ⛔ Pourquoi ce n'est PAS urgent, et pourquoi ce n'est pas rien

**Pas urgent** — deux raisons, toutes deux vérifiées :
1. Les corps réels de `/auth/login` et `/auth/change-password` font quelques dizaines
   d'octets : ils **tiennent toujours dans un seul morceau**.
2. Le quota (`checkRateLimit`) est consommé **à chaque appel** du handler : ⛔ un corps
   fragmenté n'ouvre donc **aucun contournement** du plafond de requêtes.

**Pas rien** — c'est un client **mal formé ou malveillant** qui choisit le découpage, pas
l'appareil. ⚠️ Le fait que personne ne l'ait jamais fait ne garantit rien ; ça garantit
seulement que personne n'a essayé.

⭐ **Et le défaut est PRÉEXISTANT** : [`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md) ne l'a pas créé, il l'a rendu visible
en ajoutant un compteur qu'un double appel fausse.

## Comportement attendu

Un corps HTTP reçu en plusieurs morceaux produit **exactement une** exécution de la logique
et **exactement une** réponse.

### Cas limites
- Corps en un seul morceau — ⚠️ le cas de **tous** les clients réels : ⛔ ne pas le
  régresser
- Corps en deux morceaux ou plus
- Corps **incomplet** (connexion coupée en cours) ⇒ ⛔ aucune fuite de mémoire, aucune
  réponse envoyée deux fois
- Corps **plus gros qu'attendu** ⇒ refus borné, ⛔ sans allocation proportionnelle à ce que
  l'attaquant envoie

## Comportement actuel

Le handler s'exécute intégralement à chaque morceau reçu.

## Contraintes techniques

- ⛔ Aucun handler bloquant > 50 ms.
- ⚠️ **Accumuler les morceaux, c'est allouer sur un chemin NON AUTHENTIFIÉ.** Toute mémoire
  réservée pour un corps en cours de réception est pilotée par l'attaquant ⇒ ⛔ plafond
  **dur** obligatoire, dans `constants.h`, et refus au-delà.
- ADR-0082 : pas d'allocation sur le chemin de journalisation.
- ⚠️ Vérifier si **d'autres** routes du projet ont déjà résolu ce problème et ⛔ réutiliser
  ce motif plutôt qu'en inventer un — c'est le genre de code qu'on ne veut pas en quatre
  exemplaires.

## Livrables — check-list

- [ ] A0 — ⭐ **Recenser d'abord** : quelles autres routes à corps du projet ignorent
      `index`/`total` ? ⚠️ Traiter `/auth/*` seul laisserait peut-être le même défaut
      ailleurs, ⛔ sans élargir le périmètre sans arbitrage
- [ ] A1 — Les deux handlers `/auth/*` n'exécutent leur logique qu'une fois le corps complet
- [ ] A2 — Plafond dur sur la taille accumulée, dans `constants.h`, avec refus borné au-delà
- [ ] A3 — ⛔ Aucune réponse envoyée deux fois, aucune fuite sur corps incomplet
- [ ] D1 — Tests : un morceau, deux morceaux, corps incomplet, corps surdimensionné
- [ ] D2 — **Validation par mutation** de chaque test neuf
- [ ] D3 — Essai réel en `curl` avec un corps volontairement fragmenté

## Critères d'acceptation

- [ ] Un corps en deux morceaux produit **une** réponse et **un** incrément du compteur
      d'échecs
- [ ] Un corps en un seul morceau se comporte **exactement** comme aujourd'hui
- [ ] Un corps surdimensionné est refusé sans allocation proportionnelle
- [ ] `mb` (`max_alloc_8bit`) de `/get-system-info?hf=1` stable après une rafale de corps
      fragmentés
- [ ] `pio run` sans nouveau warning ; `check_memory_discipline.sh` vert

## Notes pour agent

- ⚠️ **Commencer par A0.** Si le motif existe déjà ailleurs dans `src/web_routes_*`, le
  réemployer ; sinon, le poser une fois proprement.
- ⛔ **Ne pas accumuler sans plafond.** Un corps non authentifié dont la taille est choisie
  par l'attaquant est exactement le genre d'allocation que [ADR-0082](../../../docs/adr/0082-arene-circulaire-du-journal-en-ram-interne-statique.md) et feature-131
  existent pour empêcher.

## Hors périmètre

- Le ralentissement des essais → [`bug-189`](bug-189-aucun-verrouillage-sur-les-essais-de-mot-de-passe-en-ligne.md).
- `rateLimitMap` (allocation par requête non authentifiée) → `feature-131`.
