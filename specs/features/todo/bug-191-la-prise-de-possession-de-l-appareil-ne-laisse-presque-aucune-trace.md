# bug-191 — La prise de possession de l'appareil ne laisse presque aucune trace

## Métadonnées
| | |
|---|---|
| Type | bug — **sécurité, observabilité** |
| Statut | todo |
| Priorité | normale — ⚠️ l'événement est **le plus grave du système**, mais il se révèle de lui-même |
| Version cible | à définir |
| Relevé par | `security-reviewer` (point F) **et** `code-reviewer` (mineure 1), indépendamment, en passe P2 de [`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md), 2026-11-17 |
| Specs liées | [`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md) (a tracé `/auth/login`, pas celle-ci) · [`bug-185`](../done/bug-185-fenetre-de-premier-demarrage-ouverte-indefiniment.md) (fenêtre de premier démarrage) · [`feature-172`](feature-172-retention-protegee-des-traces-de-securite.md) |

## Domaines touchés
- [x] **Authentification** (`src/auth.cpp`, `src/web_routes_auth.cpp`, `src/web_routes_config.cpp`)
- [ ] Capteurs — [ ] Régulation — [ ] Filtration — [ ] MQTT — [ ] OTA — [ ] Frontend

## Agents requis
- [x] **`security-reviewer`** — ⛔ OBLIGATOIRE (chemin d'authentification)
- [x] spec-reader · [x] architect · [x] embedded-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [ ] `pool-chemistry` · [ ] `ux-ui-designer` — sans objet

## Le fait, vérifié dans le code

[`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md) a tracé `/auth/login` : échecs **et** réussite, avec l'adresse et le nombre
d'essais ratés. ⛔ **Le changement de mot de passe RÉUSSI, lui, n'a rien de tout ça.**

Sa seule trace est, dans `src/auth.cpp:215` :
```cpp
systemLogger.info("Mot de passe administrateur modifié");
```
⛔ **Sans l'adresse**, sans compteur d'échecs, et ⛔ **sans remettre le compteur à zéro**
— alors que cette route rend elle aussi un **jeton valide**.

### ⚠️ Et il y a un TROISIÈME chemin d'écriture

`src/web_routes_config.cpp:1646` émet **exactement la même ligne**, depuis `/save-config`.
⇒ Deux routes différentes produisent une trace **indistinguable**. En lisant le journal, on
ne peut pas dire par où le mot de passe a été changé.

### ⭐ Pourquoi c'est le cas le plus grave du système

Sur un appareil **vierge**, `/auth/change-password` accepte un changement **anonyme** :
c'est le droit de premier démarrage, et c'est voulu ([`bug-185`](../done/bug-185-fenetre-de-premier-demarrage-ouverte-indefiniment.md)). ⇒ Un inconnu qui
atteint l'appareil avant son propriétaire **en prend possession**, et le journal n'en dira
ni d'où, ni combien d'essais, ni par quelle route.

⚠️ **C'est l'événement qui fait basculer la propriété de l'appareil, et c'est le moins bien
tracé des trois.**

## ⛔ Pourquoi ce n'était PAS bloquant pour bug-183

Arbitrage posé et assumé le 2026-11-17 : **cette voie se révèle d'elle-même.** Le
propriétaire est éjecté (son jeton est révoqué, les WebSocket sont fermées) — il le voit
immédiatement. Un attaquant **discret**, lui, passera par `/auth/login`, qui est désormais
tracé.

⇒ Le défaut n'est pas l'absence de détection, c'est l'absence de **preuve exploitable
après coup** : on sait que c'est arrivé, on ne sait ni d'où ni comment.

## Comportement attendu

Le changement de mot de passe réussi doit laisser une trace **au moins aussi informative**
que la connexion réussie :

- l'**adresse** d'où il vient ;
- le nombre d'échecs qui l'ont précédé, puis **remise à zéro** du compteur (c'est une
  authentification réussie, au même titre qu'une connexion) ;
- ⭐ la **route** qui l'a produit (`/auth/change-password` ou `/save-config`) — deux lignes
  distinctes, ou une mention qui les sépare ;
- ⚠️ une mention explicite quand le changement est **ANONYME** (appareil vierge) : c'est
  l'information la plus importante de toute la ligne.

### Cas limites

- ⛔ **Jamais** le mot de passe, ni son ancien, ni sa longueur, ni aucun fragment.
- Changement **légitime** par le propriétaire : la ligne doit rester lisible et non
  alarmante — ⚠️ c'est le cas le plus fréquent, de loin.
- Le compteur `gFailuresSinceSuccess` est **partagé** avec `/auth/login` (bug-183) : dire
  ce que « remise à zéro » veut dire des deux côtés, ⛔ ne pas créer un second compteur.

## Comportement actuel

Une ligne INFO sans adresse, identique pour deux routes, sans compteur et sans remise à
zéro.

## Contraintes techniques

- ADR-0082 : ⛔ **aucune allocation avant la décision d'émettre**. La ligne est atteignable
  sur un chemin **anonyme** (appareil vierge) ⇒ elle doit être **bornée**, ou porter un
  `AUTH-LOG-EXEMPT` dont la raison est **vraie**.
- ⚠️ Le site de bornage de bug-183 (`gAuthFailSite`) existe déjà : ⛔ ne pas en créer une
  copie — c'est précisément ce que [`bug-112`](../done/bug-112-journal-non-borne-sur-route-protegee.md) a corrigé.
- Si un site s'ajoute, `src/constants.h` doit être **recompté** (aujourd'hui 10 sites,
  380 lignes/24 h, ≈87 Ko) — ⛔ recompter, ne pas recopier.
- ⚠️ `docs/subsystems/auth.md` et l'addendum de [ADR-0127](../../../docs/adr/0127-le-bornage-du-journal-d-authentification-est-global-par-emetteur.md) portent ces chiffres : les deux
  devront suivre.

## Livrables — check-list

- [ ] A1 — Adresse dans la ligne de changement réussi, sur **les deux** routes
- [ ] A2 — Les deux routes deviennent **distinguables** dans le journal
- [ ] A3 — Compteur d'échecs affiché puis **remis à zéro**, cohérent avec `/auth/login`
- [ ] A4 — ⭐ Mention explicite du changement **ANONYME** (appareil vierge)
- [ ] A5 — Bornage respecté (site existant) ou exemption à raison vraie
- [ ] D1 — Tests natifs sur la logique de classement de la ligne
- [ ] D2 — **Validation par mutation** de chaque test neuf
- [ ] C1 — `docs/subsystems/auth.md`, `CHANGELOG.md`, et ⚠️ les chiffres de sites si un
      site s'ajoute (y compris l'addendum ADR-0127)

## Critères d'acceptation

- [ ] Un changement de mot de passe réussi nomme son adresse et sa route
- [ ] Un changement **anonyme** sur appareil vierge est identifiable **sans ambiguïté** dans
      le journal
- [ ] ⛔ Aucun fragment du mot de passe, ancien ou nouveau, n'apparaît
- [ ] 200 tentatives ne purgent pas le journal
- [ ] `pio run` sans nouveau warning ; `check_unauth_log_guard.sh` et
      `check_memory_discipline.sh` verts

## Notes pour agent

- ⚠️ **Vérifier d'abord le troisième chemin.** `/save-config` écrit le mot de passe
  (`web_routes_config.cpp:1646`) : traiter `/auth/change-password` seul laisserait la moitié
  du défaut en place.
- ⭐ Le patron est déjà écrit : `logLoginSuccess()` dans `src/web_routes_auth.cpp` fait
  exactement ce qu'on demande ici. ⛔ S'en inspirer, ne pas réinventer.

## Hors périmètre

- La rétention des traces → [`feature-172`](feature-172-retention-protegee-des-traces-de-securite.md). ⚠️ Sans elle, les lignes ajoutées
  ici vivent dans un journal de 6 Ko.
- Le ralentissement des essais → [`bug-189`](bug-189-aucun-verrouillage-sur-les-essais-de-mot-de-passe-en-ligne.md).
