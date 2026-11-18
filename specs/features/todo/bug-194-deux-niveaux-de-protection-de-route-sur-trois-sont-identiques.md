# bug-194 — Deux des trois niveaux de protection de route sont le même niveau

## Métadonnées
| | |
|---|---|
| Type | bug — ⚠️ **pas une faille : un classement qui fait raisonner faux** |
| Statut | todo |
| Priorité | normale |
| Version cible | à définir |
| Relevé par | agent principal, pendant l'étude de [`bug-187`](../doing/bug-187-mot-de-passe-du-point-d-acces-a-32-bits.md), 2026-11-18 — ⭐ **vérifié dans le code le jour même** |
| Specs liées | [`bug-184`](../done/bug-184-authentification-basic-accepte-un-mot-de-passe-qui-n-est-pas-le-bon.md) (source unique d'admission) · [`bug-185`](../done/bug-185-fenetre-de-premier-demarrage-ouverte-indefiniment.md) |

## Domaines touchés
- [x] **Authentification** (`src/auth.h`, `src/auth.cpp`) · [x] Web — routes HTTP
- [ ] Capteurs — [ ] Régulation — [ ] Filtration — [ ] MQTT — [ ] OTA — [ ] Frontend

## Agents requis
- [x] **`security-reviewer`** — ⛔ OBLIGATOIRE (c'est l'objet) · [x] architect · [x] embedded-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [ ] `ux-ui-designer` — sans objet · [ ] `pool-chemistry` — sans objet

## Le fait, mesuré dans le code

`src/auth.h:11-15` annonce **trois** niveaux :

```cpp
enum class RouteProtection {
  NONE,      // Route publique (lecture seule)
  WRITE,     // Route d'écriture (nécessite auth)
  CRITICAL   // Route critique (admin uniquement)
};
```

⛔ Mais `AuthManager::checkAuth` (`src/auth.cpp:327-342`) ne consulte le niveau **qu'une fois** :

```cpp
in.publicRoute = (level == RouteProtection::NONE);
```

⇒ `WRITE` et `CRITICAL` suivent **exactement le même chemin** et exigent **exactement la même
preuve** : le jeton `X-Auth-Token`. Il n'existe qu'une seule identité dans l'appareil. Le
commentaire « admin uniquement » décrit une distinction qui **n'est implémentée nulle part**.

## Pourquoi ça compte, alors que ça n'ouvre aucune porte

⛔ **Ce n'est pas une faille** : rien n'est accessible sans jeton qui devrait l'être. Le défaut
est de **raisonnement**, et il a déjà failli produire une conclusion fausse.

⭐ **L'incident qui l'a révélé** (étude de bug-187, 2026-11-18) : la question posée était « le
mot de passe du point d'accès journalisé en clair est-il moins protégé que la route
`/auth/ap-password` qui le sert ? ». La route est marquée `CRITICAL`, le journal se lit avec
`WRITE` — la réponse *paraissait* être oui, et aurait justifié un correctif inutile. La
vérification du code a montré que **les deux demandent la même chose**.

⇒ Un classement dont deux valeurs sont indiscernables **invite** à ce genre d'inférence. Il
produira d'autres raisonnements faux tant qu'il existera.

⚠️ Et le risque symétrique est pire : quelqu'un peut marquer une route neuve `CRITICAL` en
croyant l'avoir durcie, alors qu'il ne l'a **pas** durcie.

## Comportement attendu — ⛔ à arbitrer, deux directions opposées

| Direction | Ce que ça veut dire | Coût |
|---|---|---|
| **A — Retirer `CRITICAL`** | Reconnaître qu'il n'y a qu'un niveau protégé. Deux valeurs : publique / protégée. | Toucher chaque site d'appel marqué `CRITICAL` ; aucune régression fonctionnelle possible, puisque le comportement est déjà identique |
| **B — Le rendre RÉEL** | Lui donner une seconde preuve (mot de passe redemandé, confirmation, fenêtre temporelle courte après connexion…) | ⚠️ Décision **produit**, pas technique : qui doit être gêné, et pour quelles routes ? ⛔ Ne pas la prendre sans l'utilisateur |

⚠️ **Ne pas trancher dans la fiche** : c'est précisément le genre d'arbitrage qui doit être posé
à l'utilisateur, chiffres en main — combien de routes portent `CRITICAL` aujourd'hui, et
lesquelles mériteraient vraiment une seconde preuve.

## Travail préalable, à faire AVANT de proposer quoi que ce soit

- [ ] Compter les routes marquées `CRITICAL` et celles marquées `WRITE` — **chiffres réels**,
      pas une estimation ([ADR-0073](../../../docs/adr/0073-chiffre-consigne-porte-sa-methode.md))
- [ ] Lister ce que `CRITICAL` garde aujourd'hui, et dire si cette liste a un sens commun
- [ ] Vérifier qu'aucune documentation n'affirme une différence de niveau qui n'existe pas
      (⚠️ `docs/API.md` et `docs/subsystems/auth.md` décrivent les protections route par route)

## Critères d'acceptation
- [ ] Le classement des routes dit **la vérité** : deux valeurs indiscernables ne coexistent plus,
      ou bien elles deviennent discernables
- [ ] Aucune route ne change de protection **effective** sans que ce soit écrit et voulu
- [ ] La documentation des routes est alignée sur ce que le code fait réellement
- [ ] Une garde ou un test empêche la réapparition d'un niveau décoratif

## Hors périmètre
- L'identité multiple (plusieurs comptes, plusieurs rôles) : l'appareil n'en a qu'une, et rien
  dans ce lot ne doit en introduire une seconde sans décision produit explicite.
