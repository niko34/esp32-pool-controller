# bug-100 — `abort()` sur le `fopen` de l'historique : le lot B-3 ne couvre pas ce chemin

## Métadonnées

| Champ | Valeur |
|---|---|
| Type | **bug critique** — crash firmware en production, reproduit le jour même |
| Statut | todo |
| Priorité | **Haute** — même mécanisme que le PANIC fondateur du bug-080, chemin non couvert |
| Découvert | 05/09/2026 ~15:35, en relevant le coredump après le déploiement de la v2.41.0 |
| Version au crash | **2.40.0** (concordance prouvée par SHA256, voir ci-dessous) |
| Parent | `bug-080` — lots A, B-1, B-2, B-3 livrés ; celui-ci est un **chemin frère non couvert** |

## Domaines touchés

- `src/history.cpp` — `historyStore->open(historyFilePath, "r")` aux lignes **500** et **586**
- `src/logger.cpp` — `flushToDisk()` (l. 276-277), **même classe de défaut**, déjà identifié hors périmètre en B-3
- `docs/subsystems/history.md`, `docs/subsystems/logger.md`

## Sécurité chimique

**Indirecte mais réelle**, comme pour le bug-080 parent : un redémarrage inopiné interrompt une session de dosage et relance une purge de stabilisation complète (plancher 10 min sans régulation). Les compteurs journaliers survivent (NVS) et l'intégrale pH est désormais persistée (feature-093), mais le crash reste une perte de service.

Aucun code de dosage n'est modifié par le correctif attendu → gate `pool-chemistry` **non requis a priori**, à reconfirmer si le correctif touchait la cadence de sauvegarde.

## Le constat — preuve, pas inférence

Coredump relevé sur l'appareil le 05/09 à 15:35, archivé dans
`builds/2.40.0/esp32dev_core3/coredump_panic_20260905_history.bin`, trace décodée
dans `coredump_panic_20260905_decoded.txt`.

```
abort() ← lock_init_generic (newlib locks.c:77) ← __retarget_lock_init_recursive
← __sfp ← _fopen_r ← fopen ← VFSFileImpl ← VFSImpl::open
← fs::FS::open(historyFs, "/history.json", "r", create=false)
```

Tâche : **`loopTask`**, core 1, hors contexte d'interruption.

**Datation prouvée** : `esp-coredump` vérifie le SHA256 de l'image applicative. L'ELF
de la **v2.40.0** décode ; celui de la v2.38.0 est **rejeté**
(`coredump SHA256(2895ab0e4) != app SHA256()`). Le crash a donc eu lieu **sous la
v2.40.0**, c'est-à-dire entre **14:05 et 14:59 le 05/09** — pendant la session de
développement, sans que personne ne le remarque sur le moment.

Corroboration indépendante : l'enregistreur de plateau mémoire (échantillon toutes
les 2 min) montre **deux discontinuités d'`uptime` non expliquées**, vers **14:16**
et **14:20**, alors qu'aucun déploiement n'a eu lieu entre 14:05 et 14:47.

## Pourquoi c'est important

**C'est exactement le mécanisme du PANIC fondateur du 29/08** — `lock_init_generic`
qui appelle `abort()` parce que le `malloc` d'un verrou newlib d'environ 88 octets
échoue —, mais sur un **chemin différent** :

| | 29/08 | 05/09 (celui-ci) |
|---|---|---|
| Appelant | `AsyncWebServerRequest::send(fs, "text/html")` | `fs::FS::open(historyFs, "/history.json", "r")` |
| Tâche | `async_tcp` | **`loopTask`** |
| Déclencheur | chargement d'interface | **sauvegarde périodique de l'historique** |
| Couvert par B-3 ? | **oui** | **NON** |

Le lot **B-3 garde les requêtes HTTP GET** servant des fichiers. Il ne garde **rien**
de ce que le firmware ouvre pour son propre compte. Or l'historique est sauvegardé
**toutes les 5 minutes depuis la boucle principale**, et la ligne 500
(`File old = historyStore->open(historyFilePath, "r")`) est sur ce chemin — le
candidat le plus probable, la pile étant tronquée au-delà de `fs::FS::open`.

⚠️ **Le lot B-3 a donc fermé un chemin sur au moins trois.** Le `code-reviewer`
l'avait anticipé pour `Logger::flushToDisk()` ; celui de l'historique n'avait pas
été vu, et c'est celui qui a crashé.

## Ce qui est demandé

1. **Recenser TOUS les `fopen` du firmware pour son propre compte** — historique
   (lecture et écriture), journal (`flushToDisk`), configuration, calibrations,
   coredump. Produire la liste exhaustive : c'est elle le livrable principal, pas le
   correctif d'un site isolé.
2. **Décider d'une stratégie unique** plutôt que d'un rustine par site. Pistes à
   arbitrer :
   - une garde mémoire **avant** chaque `fopen` interne, qui **reporte** l'opération
     au lieu de la tenter (l'historique peut sauter un cycle de 5 min sans dommage —
     contrairement à une requête utilisateur, il n'y a personne à qui répondre) ;
   - un **helper unique** `openIfHeapAllows()` par lequel tout `fopen` interne doit
     passer, sur le modèle de `oomPmSetFlags()` du lot B-2 : rendre le défaut
     **inexprimable** plutôt qu'untesté ;
   - pré-allouer le verrou newlib au démarrage (à étudier : `__sfp` n'est appelé que
     lorsque le pool de 4 `FILE` est épuisé — un pool pré-chauffé supprimerait
     peut-être la classe entière).
3. **Ne pas oublier le report** : une sauvegarde d'historique sautée doit être
   **retentée**, et le saut doit être **observable** (compteur, WARN throttlé). Un
   report silencieux répété ferait perdre l'historique sans que rien ne le dise —
   c'est le mode de défaillance déjà identifié dans `bug-099`.

## Critères d'acceptation

- [ ] Liste exhaustive des `fopen` internes, avec pour chacun : peut-il être reporté,
      ou doit-il aboutir ?
- [ ] Aucun `fopen` interne ne peut plus être tenté sous le seuil retenu
- [ ] Le saut est observable (jamais silencieux) et l'opération est retentée
- [ ] Test natif du prédicat de report + **validation par mutation**
- [ ] Budget flash chiffré ⚠️ **marge actuelle : 9 291 o, flash à 99,5 %** — ce lot
      devra probablement arriver avec son propre poste de récupération

## Notes

Le crash est survenu **pendant** la session qui livrait B-2 et B-3, sur un appareil
qu'on surveillait. Il n'a été vu qu'après coup, en relevant le coredump — les deux
redémarrages étaient passés inaperçus dans les relevés d'`uptime`.

Leçon à retenir : **`uptime` seul est un mauvais détecteur de redémarrage** quand on
ne lit que les minutes modulo 60. Vérifier `/coredump/info` fait désormais partie de
tout relevé de diagnostic.
