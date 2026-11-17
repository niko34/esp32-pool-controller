# bug-185 — La fenêtre de premier démarrage reste ouverte indéfiniment, et le jeton qu'on y prend ne s'invalide jamais

## Métadonnées
| | |
|---|---|
| Type | bug — 🔴 **sécurité, authentification** |
| Statut | todo |
| Priorité | **CRITIQUE** — ⚠️ qualifiée **« bloquante pour la vente »** par `security-reviewer` (P2 de [`bug-184`](../done/bug-184-authentification-basic-accepte-un-mot-de-passe-qui-n-est-pas-le-bon.md)) |
| Version cible | à définir |
| Relevé par | `security-reviewer`, 2026-11-17, passe P2 de `bug-184` — **vérifié dans le code** le même jour |
| Specs liées | [`bug-184`](../done/bug-184-authentification-basic-accepte-un-mot-de-passe-qui-n-est-pas-le-bon.md) (⚠️ ce lot ne le corrige PAS) · [`bug-183`](bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md) |

## Domaines touchés
- [x] **Web — routes HTTP** · [x] **Authentification** (`src/auth.{h,cpp}`, `src/web_routes_auth.cpp`)
- [ ] Capteurs — [ ] Régulation — [ ] Filtration — [ ] MQTT — [ ] OTA — [ ] Frontend
- ⚠️ **Conséquence** : `/save-config` écrit les **consignes de dosage**.

## Agents requis
- [x] spec-reader · [x] architect · [x] embedded-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [x] **`security-reviewer`** — ⛔ OBLIGATOIRE, P1 sur le design **et** P2 sur le diff.
- [ ] `ux-ui-designer` — ⚠️ **à cocher** si le correctif impose un geste à l'utilisateur (preuve de présence physique).
- [ ] `pool-chemistry` — pas sur le mécanisme ; ⚠️ le périmètre d'impact est chimique.

## Description

Tant que `isFirstBoot` est vrai, **n'importe qui sur le réseau local** peut prendre le contrôle complet de l'appareil. Et cet état **ne se termine pas tout seul**.

## Les quatre faits, vérifiés dans le code

1. **`POST /auth/change-password` est anonyme et rend le jeton.** `src/web_routes_auth.cpp:99-104` : si `isFirstBoot`, ⛔ **aucune authentification n'est demandée**. La réponse contient `response["token"] = authManager.getApiToken()` (`:158`).
2. **`POST /save-config` est anonyme.** `saveConfigBodyAuthorized` (`src/web_routes_config.cpp:545-549`) rend `true` dès `isFirstBootDetected()`, ⛔ **sans contrôle de quota**. ⇒ Les consignes de dosage sont écrites sans identité.
3. 🔴 **Le jeton n'est JAMAIS régénéré.** Ni au changement de mot de passe (`web_routes_auth.cpp:144` appelle `setPassword()`, ⛔ **pas** `regenerateApiToken()` — qui **existe pourtant**, `src/auth.h:56`), ni à la clôture de l'assistant (`AuthManager::clearFirstBootFlag()`, `src/auth.cpp:316-322`).
   ⇒ ⛔ **Un jeton pris pendant la fenêtre reste valide APRÈS que le propriétaire a choisi un mot de passe fort.** Changer le mot de passe ne ferme rien.
4. **Seul `/auth/complete-wizard` referme la fenêtre.** ⇒ Un appareil **livré, alimenté, mais dont l'assistant n'a pas été terminé** reste ouvert **indéfiniment**.

## 🔴 Pourquoi c'est bloquant pour la vente

⭐ **C'est l'état dans lequel les clients reçoivent l'appareil.** Et il y revient après toute **remise à zéro par le bouton physique** (`src/auth.cpp:325-327`).

⚠️ **`bug-184` ne le corrige pas.** Son critère A2 (« aucune route protégée ne s'ouvre avec le mot de passe d'usine ») est tenu **à la lettre** — il n'y a plus de mot de passe d'usine exploitable — ⛔ **mais pas dans l'esprit** : la fenêtre de premier démarrage ouvre tout sans mot de passe du tout.

## Comportement attendu — ⚠️ à CONCEVOIR, rien n'est tranché

Trois axes, ⛔ à arbitrer, **pas** à présumer :
1. ⭐ **Régénérer le jeton** à la clôture de l'assistant **et** à tout changement de mot de passe. La méthode existe (`regenerateApiToken()`), elle n'est pas appelée. ⇒ C'est le correctif le moins cher et le plus évident ; ⚠️ il invalide les sessions ouvertes — **dire ce que voit l'utilisateur**.
2. **Borner la fenêtre** : dans le temps ? au premier changement de mot de passe réussi ? ⚠️ Attention à ne pas créer d'appareil inadministrable — c'est le piège déjà rencontré sur `bug-184`.
3. **Exiger une preuve de présence physique** pour l'amorçage (appui sur le bouton, code affiché, point d'accès dédié). ⭐ C'est la seule réponse qui ferme vraiment « quiconque sur le réseau local », ⛔ mais c'est une **décision produit** : elle change l'expérience d'installation du client.

### Cas limites
- Assistant jamais terminé (appareil alimenté en stock, chez le revendeur, chez le client avant installation).
- Remise à zéro par le bouton physique.
- ⛔ **Ne pas créer d'impasse** : un appareil doit rester administrable à la sortie du carton.
- ⚠️ Appareils **déjà livrés** : un correctif ne les atteint que par mise à jour — et leur jeton actuel pourrait déjà être compromis.

## Contraintes techniques
- ⛔ Aucune solution exigeant un équipement chez le client (décision du 2026-11-17).
- Serveur en **HTTP clair** (même décision) ⇒ ce lot protège de **l'émission**, pas de l'écoute.
- ⛔ Aucune allocation sur le chemin de journalisation (ADR-0082) ; handler AsyncWebServer < 50 ms.
- ⛔ `/auth/status` : contrat de sondabilité (bug-177), ne pas toucher.

## Livrables — check-list
- [ ] A1 — Décision écrite sur les trois axes (régénération / bornage / preuve de présence)
- [ ] A2 — Le jeton est **régénéré** au moins à la clôture de l'assistant et au changement de mot de passe
- [ ] A3 — Un jeton obtenu pendant la fenêtre **cesse de fonctionner** une fois l'assistant terminé
- [ ] A4 — `/save-config` anonyme : borné (quota au minimum) ou fermé, **décision écrite**
- [ ] A5 — ⛔ L'appareil reste administrable à la sortie du carton — **démontré**
- [ ] D1 — Tests natifs, **validés par mutation**
- [ ] D2 — ⭐ Essai réel : prendre un jeton pendant la fenêtre, terminer l'assistant, **vérifier que le jeton ne marche plus**
- [ ] C1 — `docs/subsystems/auth.md`, `docs/API.md`, `CHANGELOG.md`, ADR (décision produit structurante)

## Critères d'acceptation
- [ ] Un jeton pris pendant le premier démarrage est **invalide** après la clôture de l'assistant
- [ ] Changer le mot de passe **invalide** les jetons existants
- [ ] Un appareil neuf reste configurable **sans impasse**
- [ ] `pio run` sans nouveau warning ; gardes CI vertes

## Notes pour agent
- ⚠️ **Les quatre faits ont été VÉRIFIÉS dans le code** le 2026-11-17. ⛔ Ne pas les re-chercher ; vérifier qu'ils sont toujours vrais, oui (l'application évolue).
- ⚠️ `regenerateApiToken()` **existe déjà** (`src/auth.h:56`) : ce lot **applique** une capacité présente, il n'en invente pas.
- ⛔ **Ne pas absorber `bug-183`** (comparaisons en temps variable, journalisation des échecs) : fiche distincte, en attente.

## Hors périmètre
- Le retrait de HTTP Basic → [`bug-184`](../done/bug-184-authentification-basic-accepte-un-mot-de-passe-qui-n-est-pas-le-bon.md).
- Les comparaisons de mot de passe et le journal des échecs → [`bug-183`](bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md).
- Le chiffrement des échanges → ⛔ tranché le 2026-11-17 : on reste en clair.
