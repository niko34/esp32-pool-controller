# bug-183 — `/auth/login` : les échecs ne laissent aucune trace, et le mot de passe se compare en temps variable

## Métadonnées
| | |
|---|---|
| Type | bug (sécurité) |
| Statut | todo |
| Priorité | **Haute** — deux défauts indépendants sur la seule porte d'entrée par mot de passe |
| Version cible | à définir |
| Relevé par | `security-reviewer`, 2026-11-16, passe P2 de [`bug-112`](../done/bug-112-journal-non-borne-sur-route-protegee.md) |
| Specs liées | [`bug-112`](../done/bug-112-journal-non-borne-sur-route-protegee.md) (bornage du journal des refus), [`feature-172`](feature-172-retention-protegee-des-traces-de-securite.md) (rétention protégée) |

## Domaines touchés
- [x] Web — routes HTTP / Authentification (`src/web_routes_auth.cpp`, `src/auth.*`)
- [x] Observabilité (`src/logger.cpp`)
- [ ] Régulation — [ ] Capteurs — [ ] MQTT — [ ] Frontend — [ ] OTA

## Agents requis
- [x] spec-reader · [x] architect · [x] embedded-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [x] **`security-reviewer`** — obligatoire : la spec touche `auth.*` et le chemin d'authentification. Passe **P1 sur le design** (le choix du bornage du journal n'est pas visible dans un diff) **et P2 sur le diff**.
- [ ] `pool-chemistry` — sans objet : aucun chemin de dosage, aucun seuil chimique.
- [ ] `ux-ui-designer` — sans objet : aucun fichier `data/`.

## Description

Deux défauts **indépendants** sur `POST /auth/login`, la seule porte d'entrée par mot de passe de l'appareil :

1. **Un échec de connexion ne laisse aucune trace.** Rien n'est journalisé, donc une campagne d'essais de mots de passe est **invisible** — y compris après coup.
2. **Le mot de passe est comparé en temps variable.** `src/web_routes_auth.cpp:58` fait `password != authManager.getPassword()` : la comparaison de `String` s'arrête au premier octet différent, donc le temps de réponse dépend du nombre d'octets corrects.

## Contexte

**Le second défaut est une régression de principe, pas un oubli isolé.** Le projet a **déjà** traité ce sujet : `authManager.secureTokenEquals()` existe depuis la **v2.11.2** (`feature-028`) et impose une comparaison à temps constant sur le **jeton**, y compris côté WebSocket (`src/ws_manager.cpp`, cf. [`docs/subsystems/ws-manager.md`](../../docs/subsystems/ws-manager.md)). La règle est posée, l'outil existe — **le mot de passe y échappe**.

⚠️ **Portée réelle de l'attaque temporelle, à établir avant de dimensionner le correctif.** Sur un réseau local, la gigue réseau et l'ordonnancement FreeRTOS peuvent noyer l'écart, qui se compte en microsecondes. ⛔ Mais « probablement noyé » n'est pas un argument : le correctif coûte une ligne, la primitive existe, et l'écart se mesure. Le dimensionnement de l'attaque est un **objet d'étude de la spec**, pas une raison de ne rien faire.

**Le premier défaut, lui, est sans ambiguïté.** `bug-112` vient de borner le journal des refus `401` pour qu'une trace survive. ⇒ Mais `/auth/login` ne produit **aucune** ligne à borner : un attaquant qui essaie des mots de passe ne laisse rien derrière lui, et le quota de `checkRateLimit` le ralentit sans jamais le **nommer**.

## Comportement actuel

- `POST /auth/login` avec de mauvais identifiants : réponse `401`, **aucune ligne de journal**, à aucun niveau.
- La comparaison est `username != "admin" || password != authManager.getPassword()` (`web_routes_auth.cpp:58`) : deux comparaisons de `String`, l'une et l'autre à court-circuit.
- ⚠️ Un `403` distinct est rendu au premier démarrage (mot de passe d'usine) **après** la vérification du mot de passe : la distinction `401` / `403` dit donc à un appelant anonyme si le mot de passe essayé était le bon.

## Comportement attendu

### Les échecs laissent une trace, bornée

- Un échec de connexion produit une ligne **WARNING**, nommant l'IP source.
- ⛔ **Jamais le mot de passe essayé, ni aucun de ses fragments**, ni sa longueur.
- ⭐ **Bornée par le mécanisme de `bug-112`** : un `AuthLogThrottleSite` dédié, recul exponentiel, chemin muet **sans allocation**, drain depuis la `loop()`. ⛔ Sans ce bornage, on rouvre exactement le défaut que `bug-112` vient de fermer — une route non authentifiée qui écrit à volonté.
- La ligne doit distinguer **mauvais mot de passe** de **mauvais nom d'utilisateur** dans le journal (information utile à l'exploitant), ⛔ **sans** que la **réponse HTTP** les distingue.

### La comparaison est à temps constant

- `authManager.secureTokenEquals()` ou l'équivalent sur le mot de passe, sur le modèle de `feature-028`.
- ⚠️ Le nom d'utilisateur est comparé **aussi**, ou alors l'ordre des opérations ne doit rien révéler : aujourd'hui `username != "admin" || …` court-circuite avant même de regarder le mot de passe.
- ⚠️ **Mesurer l'écart avant et après** : une primitive qu'on croit à temps constant mais dont le compilateur réordonne les accès ne prouve rien. Le projet préfère une **mesure** à une intention.

### Le `403` du premier démarrage

À réexaminer : rendu **après** la vérification du mot de passe, il distingue « bon mot de passe, premier démarrage » de « mauvais mot de passe ». ⇒ Arbitrer entre la valeur pédagogique du message et ce qu'il révèle, et **écrire la décision**.

### Cas limites

- Première tentative depuis le démarrage → **journalisée** (cas « jamais journalisé » du module de `bug-112`).
- Tentative isolée une heure après une campagne → journalisée, avec le compte des occultées.
- Corps JSON absent, malformé, ou champs manquants → aucun changement de comportement, mais **vérifier** qu'aucun de ces chemins ne journalise sans borne.
- Authentification désactivée ou premier démarrage → comportement actuel conservé ; ⛔ ne pas ajouter de ligne non bornée sur ces chemins.

## Contraintes techniques

- ⛔ **Le chemin muet n'alloue AUCUNE mémoire** (ADR-0082) : la sortie muette précède toute construction de `String`, y compris celle de l'IP. C'est la condition qui a été vérifiée à la main quatre fois pendant `bug-112`.
- ⛔ La réponse `401` part **toujours**, bornage ou non. Confondre « ne pas journaliser » et « ne pas répondre » transformerait un correctif d'observabilité en faille d'authentification.
- `tools/check_unauth_log_guard.sh` (job CI `unauth-log-guard`) s'applique : tout `systemLogger` dans ce handler devra passer par la coquille, ou porter un `AUTH-LOG-EXEMPT(raison)` **vraie**.
- AsyncWebServer : aucun handler bloquant > 50 ms. ⚠️ Une comparaison à temps constant **ne doit pas** être obtenue par une temporisation.
- Français dans les journaux.

## Fichiers potentiellement concernés

- `src/web_routes_auth.cpp` — le handler `/auth/login` (comparaison l.58, absence de journal)
- `src/auth.{h,cpp}` — la primitive de comparaison à temps constant, si elle doit être étendue au mot de passe
- `src/auth_log_throttle_site.h` — réemploi, ⛔ pas de copie
- `src/constants.h` — constante éventuelle
- `test/test_native_auth_log_throttle/` ou suite neuve
- `docs/subsystems/auth.md`, `docs/API.md`, `CHANGELOG.md`

## Livrables — check-list

- [ ] A1 — Comparaison du mot de passe à **temps constant**, sur la primitive existante (`feature-028`)
- [ ] A2 — Le nom d'utilisateur ne court-circuite plus la comparaison du mot de passe
- [ ] A3 — Échec de connexion **journalisé** en WARNING, avec l'IP, ⛔ jamais le mot de passe ni sa longueur
- [ ] A4 — Ligne **bornée** par un `AuthLogThrottleSite` dédié + drain dans la `loop()`
- [ ] A5 — Décision écrite sur le `403` du premier démarrage (conservé tel quel, ou aligné sur le `401`)
- [ ] D1 — Tests natifs : bornage, premier échec journalisé, chemin muet sans allocation
- [ ] D2 — **Validation par mutation** de chaque test neuf
- [ ] D3 — ⭐ **Mesure** de l'écart de temps de réponse **avant / après**, sur l'appareil, entre un mot de passe faux au 1ᵉʳ octet et faux au dernier. Sans ce chiffre, A1 n'est pas démontré.
- [ ] C1 — `docs/subsystems/auth.md`, `docs/API.md`, `CHANGELOG.md`

## Critères d'acceptation

- [ ] Un échec de connexion produit une ligne de journal nommant l'IP, **sans** le mot de passe
- [ ] 500 échecs consécutifs produisent un nombre **borné** de lignes, et ne purgent pas le journal
- [ ] Un échec isolé, une heure après, reste journalisé
- [ ] L'écart de temps de réponse mesuré entre « faux au 1ᵉʳ octet » et « faux au dernier » est **indiscernable du bruit**, chiffres à l'appui
- [ ] La réponse HTTP ne distingue pas mauvais nom d'utilisateur et mauvais mot de passe
- [ ] `pio run` sans nouveau warning ; `check_unauth_log_guard.sh` et `check_memory_discipline.sh` verts

## Plan de test manuel

1. 200 tentatives avec un mauvais mot de passe → toutes rendent `401` ; le journal n'est pas purgé ; un nombre borné de lignes apparaît, avec le compte des occultées.
2. Attendre le drain, envoyer une tentative isolée → journalisée.
3. Mesurer le temps de réponse sur mille tentatives « faux au 1ᵉʳ octet » et mille « faux au dernier octet » ; comparer les distributions, pas les moyennes.
4. Mauvais **nom d'utilisateur** avec le bon mot de passe → même réponse, même délai apparent qu'un mauvais mot de passe.
5. `mb` (`max_alloc_8bit`) de `/get-system-info?hf=1` stable avant / après la rafale.

## Notes pour agent

- ⚠️ **Ne pas livrer A3 sans A4.** Ajouter un journal sur une route non authentifiée **sans** le borner, c'est rouvrir `bug-112` au lieu de le prolonger. Les deux vont ensemble ou aucun des deux.
- ⚠️ Le projet a déjà la primitive à temps constant **et** la règle (`feature-028`, v2.11.2) : ce lot **applique** une règle existante, il n'en invente pas. Si un autre chemin compare encore un secret avec `==` ou `!=`, le dire — ⛔ sans élargir le périmètre sans arbitrage.
- La coquille de bornage est **factorisée** depuis `bug-112` (`src/auth_log_throttle_site.h`) : ⛔ ne pas en écrire une quatrième copie, c'est précisément ce qui a été corrigé.

## Hors périmètre

- La rétention séparée des traces de sécurité → [`feature-172`](feature-172-retention-protegee-des-traces-de-securite.md).
- `rateLimitMap` : allocation par requête non authentifiée et absence de plafond dur → `feature-131`.
- Les autres routes sans authentification → [`bug-181`](bug-181-get-config-expose-la-configuration-sans-authentification.md), [`feature-070`](feature-070-durcissement-routes-debug-et-cooldown-reset.md).
