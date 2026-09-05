# bug-099 — Le fichier temporaire de rotation n'est jamais nettoyé au démarrage

## Métadonnées

| Champ | Valeur |
|---|---|
| Type | bug — occupation résiduelle d'une partition contrainte |
| Statut | todo |
| Priorité | Basse tant que la marge tient ; **monte si un lot consomme encore la partition `history`** |
| Découvert | 05/09/2026, par le `test-engineer` pendant la vérification de budget de la feature-095 |
| Version en cours | 2.39.0 (déployée) |

## Domaines touchés

- `src/logger.cpp` — rotation de `/system.log`
- `src/history.cpp` — `historySaveAdmit()` (consommateur de la marge)
- `docs/subsystems/logger.md`, `docs/subsystems/history.md`

## Sécurité chimique

**Non.** Aucun chemin de dosage. Gate `pool-chemistry` non requis.

## Le problème

La rotation du journal système écrit un fichier `.tmp` transitoire d'environ
6 Ko. Ce fichier n'est effacé qu'à l'**effacement explicite des journaux** — il
n'existe **aucun nettoyage au démarrage**. Un `.tmp` laissé par une rotation
interrompue (coupure secteur, redémarrage pendant l'écriture) reste donc
indéfiniment sur la partition.

Or les deux fichiers partagent la même partition de **64 Ko**, et la feature-095
vient d'y consommer 2 016 octets : le régime établi est à **12 blocs sur 16**,
résiduel **8 102 octets**.

**Le cas mesuré** : un `.tmp` orphelin **plus** un tampon d'historique plein
réduisent la marge de croissance à **90 octets** — soit un seul point
d'historique. Avant la feature-095, ce même cas laissait encore 4 006 octets.

Le mode de défaillance est le **silence** : `historySaveAdmit()` refuse la
sauvegarde et journalise un avertissement une fois par heure. L'historique cesse
d'être persisté sans qu'aucun signal fort ne le dise, et le compteur `nospc_cnt`
n'est exposé par **aucun endpoint** (vérifié par grep).

## Ce qui est demandé

1. Effacer tout `.tmp` de rotation au démarrage, **avant** le premier calcul
   d'admission. C'est un fichier transitoire par nature : sa survie à un
   redémarrage signifie qu'il est orphelin.
2. Journaliser l'effacement quand il a lieu — un `.tmp` orphelin est le signe
   d'une rotation interrompue, information utile en soi.
3. Envisager d'exposer `nospc_cnt` (ou l'occupation de la partition) dans
   `/get-system-info` : aujourd'hui, rien ne permet de constater à distance que
   l'historique a cessé d'être écrit. **À arbitrer** — cela peut aussi relever
   d'une spec d'observabilité séparée.

## Critères d'acceptation

- [ ] Un `.tmp` présent au démarrage est effacé et journalisé
- [ ] La marge d'admission est recalculée après nettoyage, pas avant
- [ ] Test : démarrage avec `.tmp` orphelin → partition libérée, sauvegarde admise
- [ ] Le nettoyage ne touche **jamais** le journal courant ni `/history.json`

## Notes

Trouvé en construisant le cas de charge du pire scénario, pas en observant une
panne. C'est exactement le genre de défaut qui ne se manifeste que le jour où
une autre feature a mangé la marge — et qui se diagnostique alors comme un
problème de cette autre feature.
