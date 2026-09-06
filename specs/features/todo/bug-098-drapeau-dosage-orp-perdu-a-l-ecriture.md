# bug-098 — Le drapeau de dosage ORP est perdu à l'ÉCRITURE de l'historique

## Métadonnées

| Champ | Valeur |
|---|---|
| Type | **bug de format persisté** — perte d'information irréversible |
| Statut | todo |
| Priorité | Modérée — aucune conséquence sur la régulation, mais l'historique ment |
| Découvert | 05/09/2026, par l'`embedded-developer` pendant le cycle feature-095 |
| Version en cours | 2.39.0 (déployée) |

## ✅ Obstacle levé le 2026-09-06 — il ne reste qu'à écrire le code

Le coût de stockage qui retenait ce correctif **n'est plus un obstacle**. Le layout v5
(ADR-0068) a porté la partition `history` de 64 Ko à **256 Ko** ; le `CHANGELOG` de la
v2.42.0 chiffre le surcoût de la séparation des deux drapeaux à **un octet par point,
339 o au total**, désormais absorbables sans discussion.

**Le défaut, lui, est intact** — vérifié dans le code le 2026-09-06 :

```c
history.cpp:573   obj["d"] = point.phDosing || point.orpDosing;   // fusion à l'écriture
history.cpp:750   dp.phDosing = point["d"];                        // relecture partielle
```

`orpDosing` est toujours perdu. La mention « devient trivial à finaliser » du
`CHANGELOG` signifiait **l'obstacle est levé**, jamais **c'est fait**.

## Domaines touchés

- `src/history.cpp` — `saveToFile()` (~474-486), `loadFromFile()` (~610-616)
- `src/web_routes_data.cpp` — `/import-history` (~962-968)
- `src/history.h` — `DataPoint::phDosing` / `orpDosing`
- `docs/subsystems/history.md`, `docs/API.md`

## Sécurité chimique

**Non.** Aucun chemin de dosage, aucune décision de régulation ne lit ces
drapeaux — ce sont des champs d'observation. Le gate `pool-chemistry` n'est pas
requis. À rediscuter **uniquement** si le correctif touchait la structure
`DataPoint` d'une manière qui déplacerait l'allocation de `/get-history`
(verrouillée par `static_assert(sizeof(DataPoint) == 28)`).

## Le problème

`DataPoint` porte **deux** drapeaux distincts, `phDosing` et `orpDosing`. Mais à
la sérialisation, ils sont **fusionnés en une seule clé `d`**. La lecture ne peut
donc pas les séparer : `loadFromFile()` et `/import-history` posent tous deux
`orpDosing = false` faute d'information.

Ce n'est **pas** un bug d'import, comme on l'a d'abord cru en le découvrant sur
`/import-history` : c'est le **format persisté** qui perd l'information. Aucune
relecture, aussi soignée soit-elle, ne peut la retrouver.

Conséquence : sur tout point rechargé depuis `/history.json` — donc après chaque
redémarrage — l'historique affirme qu'aucun dosage ORP n'a eu lieu. Les points
encore en mémoire vive, eux, disent vrai. L'historique est donc **incohérent avec
lui-même** selon l'âge du point.

## Ce qui est demandé

1. Sérialiser les deux drapeaux séparément (deux clés, ou un champ de bits — le
   second économise un octet par point, ce qui compte : voir le budget ci-dessous).
2. Relire les deux clés, en **tolérant leur absence** (les fichiers écrits par
   ≤ 2.39.0 n'ont que `d`) : un point ancien garde `phDosing` depuis `d`, et
   `orpDosing` reste indéterminé — décider si cela se dit `false` ou s'il faut un
   troisième état.
3. Adapter `/import-history` de la même façon.
4. Un test natif de round-trip couvrant les trois cas : fichier ancien (clé `d`
   seule), fichier neuf, et fichier mixte.

## Contrainte de budget — la vraie difficulté

La partition `history` fait **64 Ko**, et la feature-095 y a consommé 2 016
octets : le régime établi est à **12 blocs sur 16**, résiduel **8 102 octets**.
Un octet de plus par point coûte **339 octets** ; deux clés séparées en coûtent
davantage. Le mode de défaillance n'est **pas** la corruption mais le **silence** :
`historySaveAdmit()` refuse la sauvegarde et journalise un avertissement une fois
par heure — l'historique cesse simplement d'être persisté.

**Chiffrer avant d'écrire.** Un champ de bits remplaçant `d` est probablement
neutre en taille.

## Critères d'acceptation

- [ ] Les deux drapeaux survivent à un redémarrage, prouvé par round-trip
- [ ] Un `/history.json` écrit par ≤ 2.39.0 se relit sans perte ni erreur
- [ ] Budget de partition chiffré avant/après, verdict d'admission explicite
- [ ] Mutation : retirer la séparation → un test meurt

## Notes

Découvert en cherchant autre chose, ce qui est le cas habituel. Le développeur
l'a signalé sans le corriger — changer un format persisté est un lot à part
entière, pas un ajout opportuniste en fin de cycle.
