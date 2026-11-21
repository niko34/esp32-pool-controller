## Métadonnées
- **ID** : bug-207
- **Slug** : la-sortie-ota-gele-la-cause-de-blocage-comme-le-faisait-l-arret-de-securite
- **Type** : bug
- **Statut** : todo
- **Priorité** : normale
- **Version cible** : à définir
- **Dernière mise à jour** : 2026-11-21
- **Spec(s) liée(s)** : [bug-200](../done/bug-200-le-mode-maintenance-suspend-tout-le-dosage-sans-que-l-ecran-le-dise.md) (**même défaut**, corrigé pour l'arrêt de sécurité) · [ADR-0137](../../../docs/adr/0137-un-etat-qui-court-circuite-le-dosage-nomme-sa-cause-dans-le-canal-de-refus.md) (la règle générale)

## Domaines touchés
- [x] Régulation — **consommée**, pas modifiée → `pool-chemistry` pour avis
- [x] Frontend (`data/`)
- [ ] Web — routes HTTP / WebSocket *(rien à ajouter : le canal existe)*

## Agents requis
- [x] spec-reader · [x] architect · [x] pool-chemistry · [ ] ux-ui-designer
- [x] embedded-developer · [x] web-ui-developer · [x] test-engineer · [x] code-reviewer · [x] doc-writer

## Description
`updateDosingCore()` a **deux** sorties anticipées qui coupent le dosage **sans poser de code de
refus**. bug-200 en a corrigé **une** (l'arrêt de sécurité). ⛔ **L'autre est toujours là** :
l'envoi OTA (`src/pump_controller.cpp:3748`).

## Contexte
Relevé pendant bug-200, par `pool-chemistry` puis confirmé par lecture du code le 2026-11-21.

### La règle générale, posée par ADR-0137
> ⭐ **Toute sortie anticipée d'`updateDosingCore()` qui ne pose pas de code de refus est invisible
> au tableau de bord ET laisse la cause précédente GELÉE, avec sa durée qui continue de courir.**

⚠️ Ce n'est pas une cécité, c'est un **faux témoignage** : l'écran affiche une cause périmée
**assortie d'une durée fausse**, ce qui fournit au propriétaire une explication rassurante et close.

### Fait vérifié dans le code courant
| Fait | Emplacement |
|---|---|
| La branche OTA coupe les deux pompes et sort | `src/pump_controller.cpp:3748-3758` |
| ⛔ Elle ne pose **aucune** cause | même bloc — ⇒ `_lastRefusalCode` gèle |
| La branche voisine, corrigée par bug-200, pose la sienne | `src/pump_controller.cpp:3784-3785` |

## Comportement attendu
Même remède que bug-200 : un membre `DoseRefusal` **ajouté en fin d'énum** (prochaine valeur libre :
**27** — ⛔ 4, 5, 19-23 et 26 ne se recyclent jamais), posé **après** `applyPumpDuty(0,0)` et
`settlePumpAccounting()`, **avant** le `return`.
⇒ La pastille, le bandeau et la durée tombent tout seuls par le canal unique, et la durée se
ré-ancre via `setRefusalCode()`.

## ⚠️ Pourquoi c'est MOINS grave que bug-200 — et pourquoi ça mérite quand même une fiche
| | Arrêt de sécurité (bug-200) | Envoi OTA (celui-ci) |
|---|---|---|
| Durée | ⛔ **non bornée**, ne retombe qu'au redémarrage | quelques **minutes** |
| Déclencheur | rafale de redémarrages, **corruption du tas** | ⭐ **le propriétaire vient de le lancer lui-même** |
| Surprise | totale | **nulle** |
⇒ Priorité **normale**, pas critique. ⛔ Mais la cause gelée reste un faux témoignage, et
l'appareil redémarre juste après : la trace est perdue.

## Cas limites
- OTA **refusée ou interrompue** : la cause doit-elle retomber, et sur quoi ?
- OTA **pendant** un blocage chimique légitime : ⚠️ le piège de **rang 0** de bug-200 se repose
  (`postSuspendCause()` sort sur `_refusalCauseFromGuard`, `src/pump_controller.cpp:862`).
  ⛔ **La justification de bug-200 ne s'applique PAS telle quelle ici** : l'OTA, elle, **se termine**
  et l'appareil reprend — donc la cause chimique **serait** perdue au retour. ⇒ C'est exactement le
  motif que le rang 0 protège. **À trancher par `pool-chemistry`, et la réponse peut être
  l'inverse de celle de bug-200.**

## Livrables — check-list
- [ ] A1 — `DoseRefusal::OtaInProgress = 27` en fin d'énum + `kDoseRefusalMaxValue` + `static_assert`
- [ ] A2 — Cause posée dans la branche OTA, après la coupe des pompes
- [ ] A3 — ⭐ **Rang 0 tranché par `pool-chemistry`** : la cause OTA prime-t-elle sur une cause de garde ? ⛔ Ne pas recopier la réponse de bug-200
- [ ] B1 — Entrée 27 dans `DOSE_BLOCK_CODES` · ton à décider (⚠️ l'OTA est une marche **voulue** : l'ambre est probablement **inadapté**)
- [ ] C1 — `docs/subsystems/pump-controller.md` (retirer la mention « non corrigé »), `docs/API.md`, `CHANGELOG.md`
- [ ] D1 — Vecteurs : code 27 ⇒ pastille et bandeau attendus · ⛔ cumul avec une cause de garde
- [ ] D2 — **Mutation** : retirer la pose de cause doit tuer un test
- [ ] D3 — Essai réel : un envoi OTA est **non destructif** et se provoque à volonté ⇒ ⭐ contrairement à bug-200, **ce lot est prouvable sur l'appareil de production**

## Sécurité chimique
- Dosage impliqué : **oui** — l'OTA le suspend entièrement, mais pour quelques minutes
- Limites concernées : aucune n'est modifiée
- Algorithme PID touché : **non**
- Risque identifié : ⛔ **aucune garde ajoutée ni retirée.** Le défaut est d'information, le remède
  doit l'être aussi — même doctrine que bug-200.
