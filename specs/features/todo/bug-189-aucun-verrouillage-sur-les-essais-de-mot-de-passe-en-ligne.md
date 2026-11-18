# bug-189 — Rien ne ralentit un essai de mot de passe : 172 800 tentatives par jour et par adresse

## Métadonnées
| | |
|---|---|
| Type | bug — 🔴 **sécurité, chemin d'authentification** |
| Statut | todo |
| Priorité | **HAUTE** — ⭐ c'est le risque CHIFFRÉ réel de la porte d'entrée, celui que [`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md) a rendu visible sans le fermer |
| Version cible | à définir |
| Relevé par | `security-reviewer` **et** `code-reviewer`, indépendamment, en passe P2 de [`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md), 2026-11-17 — ⭐ chiffres vérifiés dans `src/constants.h:238` |
| Specs liées | [`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md) (trace des échecs) · [`bug-184`](../done/bug-184-authentification-basic-accepte-un-mot-de-passe-qui-n-est-pas-le-bon.md) · [`feature-172`](feature-172-retention-protegee-des-traces-de-securite.md) |

## Domaines touchés
- [x] **Authentification** (`src/auth.cpp`, `src/web_routes_auth.cpp`)
- [ ] Capteurs — [ ] Régulation — [ ] Filtration — [ ] MQTT — [ ] OTA
- [ ] Frontend — ⚠️ **à trancher pendant le cycle** : un message « réessayez dans N secondes » se voit, et [`ux-ui-designer`](../../../CLAUDE.md) devient alors obligatoire

## Agents requis
- [x] **`security-reviewer`** — ⛔ OBLIGATOIRE, **passe P1 sur le design exigée** : le choix du mécanisme de ralentissement n'est pas lisible dans un diff
- [x] spec-reader · [x] architect · [x] embedded-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [ ] `ux-ui-designer` — ⚠️ à cocher dès qu'un message de ralentissement s'affiche
- [ ] `pool-chemistry` — sans objet

## Le fait, mesuré dans le code

`src/constants.h:238-239` :
```cpp
constexpr uint16_t kMaxRequestsPerMinute = 120;      // Limite globale par IP
constexpr unsigned long kRateLimitWindowMs = 60000;
```

⇒ **120 × 60 × 24 = 172 800 essais par jour et par adresse IP**, et ⛔ **aucun verrouillage
de compte** : un échec ne coûte rien de plus que le suivant. Le quota est un plafond de
DÉBIT, pas une sanction.

⚠️ **Et il est par IP** : la borne se **multiplie** par le nombre d'adresses que l'attaquant
prend sur le réseau. Sur un Wi-Fi domestique, en prendre plusieurs est trivial.

### Ce que ça donne, chiffré

| Mot de passe | Temps depuis UNE seule adresse |
|---|---|
| Dictionnaire d'un million de mots | **~6 jours** |
| 8 minuscules aléatoires (~37 bits) | des siècles |

⇒ **Le risque porte sur les mots de passe FAIBLES**, pas sur les bons. C'est ce qui rend
ce lot utile sans être urgent au point de tout arrêter : il protège l'utilisateur qui a
choisi un mot de passe court, c'est-à-dire la majorité.

### ⛔ La comparaison qui remet les choses en place

[`bug-183`](../done/bug-183-auth-login-echecs-muets-et-comparaison-non-constante.md) a fermé un canal temporel qui aurait demandé **~10⁶ à 10⁷ requêtes** pour livrer
quoi que ce soit. **Celui-ci demande six jours contre un mot de passe faible.** Les deux
passes de revue l'ont écrit dans les mêmes termes : ⛔ ne pas inverser les priorités.

## Comportement attendu

Un essai de mot de passe raté doit **coûter de plus en plus cher**, sans jamais enfermer
le propriétaire dehors.

### ⛔ CONTRAINTE RÉDHIBITOIRE — ne jamais verrouiller le propriétaire

**L'OTA est le SEUL canal de déploiement : il n'y a pas de port série.** Un appareil dont
le propriétaire serait durablement exclu serait **perdu**, sans aucun moyen de reprise.

⇒ Tout mécanisme proposé doit satisfaire les trois règles suivantes, ⛔ sans exception :
1. **Un jeton déjà émis continue de passer.** Le ralentissement frappe les ÉCHECS, jamais
   une session valide en cours.
2. **Le ralentissement s'efface tout seul**, par simple écoulement du temps. ⛔ Aucun état
   qui exige une action humaine pour être levé.
3. **Il ne survit pas à un redémarrage** de façon qui pourrait enfermer dehors — ou, s'il
   est persistant, il porte une borne de durée **dure**, écrite dans `constants.h`.

⚠️ Un « verrouillage de compte après N échecs » au sens classique est donc **à écarter**
tel quel : c'est précisément la forme qui enferme dehors. Viser un **ralentissement
progressif** (délai croissant avant la réponse, ou quota d'échecs dégressif), ⛔ pas un
refus définitif.

### Piste à instruire, pas à imposer

Un recul exponentiel sur les **échecs**, calqué sur celui que le projet a déjà pour le
journal (`kUnauthWarnMaxShift` / `kUnauthWarnDecayMs`, [ADR-0127](../../../docs/adr/0127-le-bornage-du-journal-d-authentification-est-global-par-emetteur.md)) : la mécanique est
écrite, testée en natif et comprise. ⚠️ **À instruire par `architect` puis P1**, pas à
retenir d'avance.

⛔ **Attention au handler asynchrone** : AsyncWebServer n'autorise **aucun handler bloquant
> 50 ms**. Un « délai avant réponse » ne peut PAS être un `delay()`. C'est le point dur de
la conception, et c'est pour lui que la passe P1 est exigée.

### Robustesse du mot de passe choisi

À examiner dans le même lot, puisque c'est le même risque : exiger (ou au moins **mesurer
et dire**) la robustesse du mot de passe au moment où l'utilisateur le choisit. ⚠️ Un
ralentissement qui protège un mot de passe « 1234 » ne fait que repousser l'échéance.
⇒ Arbitrage produit à poser : **refuser** un mot de passe faible, ou **l'afficher comme
faible** et laisser choisir. ⛔ Ne pas trancher ici.

## Comportement actuel

- `checkRateLimit()` plafonne à 120 req/min/IP, **toutes routes confondues**.
- Un échec d'authentification est désormais **journalisé** et borné (bug-183) — on le
  **voit**, mais rien ne le **freine**.
- Le compteur `gFailuresSinceSuccess` existe (bug-183) mais ne sert **qu'au texte** de la
  ligne de connexion réussie : ⛔ il ne décide de rien.

## Contraintes techniques

- ⛔ **Le serveur reste en HTTP clair.** Ni TLS, ni proxy, ni VPN (décision produit du
  2026-11-17).
- ⛔ **Aucune solution ne doit exiger d'équipement ou de configuration chez le client.**
- ⛔ Aucun handler bloquant > 50 ms (AsyncWebServer).
- ADR-0082 : pas d'allocation sur le chemin de journalisation.
- ADR-0017 : la décision de ralentissement est de la **logique pure** ⇒ module testable en
  natif, et **validé par mutation**.

## Livrables — check-list

- [ ] A1 — Mécanisme de ralentissement progressif des ÉCHECS sur `/auth/login` **et**
      `/auth/change-password`, en **logique pure** (module testable en natif)
- [ ] A2 — ⛔ Garantie, prouvée par test, qu'un **jeton valide passe toujours**, quel que
      soit l'état du ralentissement
- [ ] A3 — Effacement automatique par écoulement du temps, borne de durée dure dans
      `constants.h`
- [ ] A4 — Le ralentissement apparaît dans le journal (borné, site existant de bug-183)
- [ ] B1 — ⚠️ Si l'utilisateur voit un message : passage `ux-ui-designer` **sur l'écran
      entier** de connexion, pas sur l'ajout
- [ ] D1 — Tests natifs : montée, décroissance, jeton valide épargné, bornes
- [ ] D2 — **Validation par mutation** de chaque test neuf
- [ ] D3 — Essai réel : N tentatives mesurées, montrer que le débit d'essais **s'effondre**
      et que le propriétaire reprend la main après la décroissance
- [ ] C1 — `docs/subsystems/auth.md`, `docs/API.md`, `CHANGELOG.md`, ADR si P1 l'exige

## Critères d'acceptation

- [ ] Après N échecs consécutifs, le débit d'essais possibles est **divisé par au moins un
      ordre de grandeur**, chiffres à l'appui
- [ ] ⛔ Un jeton valide émis **avant** le ralentissement continue de fonctionner pendant
      celui-ci — testé
- [ ] Le propriétaire qui connaît son mot de passe **reprend la main sans intervention**,
      après un délai borné et annoncé
- [ ] Aucun handler ne bloque > 50 ms
- [ ] `pio run` sans nouveau warning ; tests natifs verts

## Notes pour agent

- ⚠️ **Ne pas confondre avec le quota existant.** `kMaxRequestsPerMinute` protège l'appareil
  de la **charge**. Ce lot protège le **mot de passe**. Les deux coexistent.
- ⛔ **Le pire résultat possible de ce lot serait un appareil inaccessible à son
  propriétaire.** Relire la contrainte rédhibitoire ci-dessus avant chaque décision.
- ⚠️ L'attaquant peut changer d'IP. Un mécanisme **uniquement** par IP sera contourné :
  dire explicitement ce que la conception retenue couvre et ce qu'elle ne couvre pas,
  plutôt que de laisser croire à une fermeture complète.

## Hors périmètre

- Le mot de passe en clair sur le Wi-Fi — décision produit assumée, ⛔ pas de spec.
- Le chiffrement de la NVS → [`feature-192`](feature-192-chiffrement-de-la-nvs-pour-l-appareil-vole.md).
- La rétention des traces → [`feature-172`](feature-172-retention-protegee-des-traces-de-securite.md).
