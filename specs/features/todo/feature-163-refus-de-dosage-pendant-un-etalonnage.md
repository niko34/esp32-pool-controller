## Métadonnées
- **ID** : feature-163
- **Slug** : refus-de-dosage-pendant-un-etalonnage
- **Type** : feature
- **Statut** : todo
- **Priorité** : **haute**
- **Version cible** : 2.78.0
- **Dernière mise à jour** : 2026-10-14
- **Spec(s) liée(s)** : [`feature-161`](../done/feature-161-choisir-le-mode-depuis-les-cartes.md) (lot C — recommandation **non bloquante** n°1 du gate `pool-chemistry`), [`bug-160`](../done/bug-160-l-indicateur-de-stabilite-ment-et-le-dosage-ne-se-suspend-pas.md), [ADR-0107](../../../docs/adr/0107-etat-visible-et-persistant-plutot-que-suspension-invisible.md)

## Domaines touchés
- [x] Capteurs (pH / ORP / température)
- [x] **Régulation (pump_controller, PID)** → ⚠️ **passage obligatoire par `pool-chemistry`**

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry** — ⚠️ OBLIGATOIRE **et prioritaire** : cette spec ajoute un refus dans le
      chemin de dosage lui-même. C'est lui qui a demandé cette garde ; c'est lui qui doit en fixer
      la forme et la place exacte.
- [ ] ux-ui-designer — **non** : aucune surface nouvelle (sauf si le refus doit être affiché, à
      trancher avec `pool-chemistry`).
- [x] embedded-developer
- [ ] web-ui-developer — seulement si un motif de refus doit être exposé
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description
Refuser **tout** dosage tant qu'une session d'étalonnage est ouverte (`calSwitchActive`), au
niveau de `canDose()` — donc **indépendamment du mode de régulation**.

## Contexte
⚠️ **C'est la leçon de feature-161, et elle vaut plus que le lot lui-même.**

bug-160 a protégé l'étalonnage en **basculant le mode** en Arrêt. feature-161 a dû ensuite poser
le verrou sur **trois chemins d'écriture du mode**, successivement découverts :
1. HTTP `/save-config` ;
2. MQTT `*_regulation_mode/set` — une automation Home Assistant, ou une **simple restauration
   d'état au redémarrage, sans aucun humain**, remettait la régulation en Auto pendant que la
   sonde trempait dans un tampon ;
3. l'écriture directe de `orp_enabled`, hors garde depuis v2.25.0.

Et un quatrième effet, pire que les trois : la session d'étalonnage était **purgée au redémarrage
suivant** — bandeau disparu, alerte d'oubli jamais émise, **sonde toujours dans le tampon**.

⇒ Verdict du gate, mot pour mot : *« C'est la défense en profondeur qui rendrait ces trois trous
inoffensifs. »*

⚠️ **Le point de principe** : les gardes livrées protègent **trois chemins NOMMÉS**. Celle-ci
protégerait **le chemin FINAL** — donc aussi les chemins **pas encore trouvés**. Toute l'histoire
de ce sujet est une suite de chemins qu'on croyait exhaustive et qui ne l'était pas.

## Comportement attendu
- Tant que `calSwitchActive` est vrai, **aucune injection** n'est autorisée — ni automatique, ni
  manuelle, ni de secours, **quel que soit le mode**.
- Le refus porte un **motif propre** (rang à faire fixer par `pool-chemistry`), distinct de ceux
  qui existent, pour que le journal dise **pourquoi**.
- ⚠️ Le refus est **journalisé**, jamais muet — le motif récurrent de ce projet est un écran ou un
  journal qui ne dit pas ce que l'appareil fait.

### Cas limites
- **Le rang du refus dans l'ordre des gardes est porteur** (ADR-0095 : une garde cite le seuil qui
  mord réellement). ⛔ Ne pas le placer sans que `pool-chemistry` l'ait fixé.
- Session ouverte **et** chloration de secours active : que se passe-t-il ? ⚠️ À trancher
  **explicitement** — c'est exactement le type de croisement qui a produit les défauts précédents.
- Session restée ouverte après un redémarrage : la garde doit tenir **au boot**, avant toute
  reprise de régulation.

## Comportement actuel
Le dosage est empêché **indirectement**, par la bascule du mode en Arrêt. La protection est donc
aussi solide que l'exhaustivité de la liste des chemins qui écrivent le mode — et cette liste
s'est révélée fausse **trois fois**.

## Contraintes techniques
- Logique en **module pur** (ADR-0017) pour être testable nativement.
- ⚠️ **Validation par MUTATION obligatoire** : casser le refus doit **tuer** des tests. Une suite
  qui ne tue aucun mutant ne prouve rien.
- ⛔ Aucune allocation sur le chemin périodique (ADR-0082).

## Fichiers potentiellement concernés
`src/pump_controller.*`, `src/dosing_logic.*`, `src/cal_manual_switch_logic.*`, `src/config.*`.

## Sécurité chimique
⚠️ Cette spec **ajoute un refus**, elle n'en retire aucun — le sens est protecteur. Mais :
- ⛔ un refus trop large qui empêcherait une **chloration de secours légitime** serait un défaut,
  au sens du principe « les plafonds protègent, le reste informe » ;
- ⚠️ une session d'étalonnage **jamais refermée** gèlerait le dosage indéfiniment. L'alerte d'oubli
  existe (C4 de bug-160) — vérifier qu'elle suffit, ou dire ce qui manque.

## ADR attendu ?
**Probablement oui.** La règle candidate : *une protection qui dépend d'un état modifiable par
plusieurs chemins doit être posée au point d'usage final, pas sur chaque chemin d'écriture.*
C'est généralisable bien au-delà de l'étalonnage.

## Critères d'acceptation
- [ ] Aucune injection possible pendant une session d'étalonnage, **quel que soit le mode**.
- [ ] Le refus a un motif propre, journalisé.
- [ ] La garde tient **après un redémarrage** avec une session ouverte.
- [ ] Mutations : casser le refus tue au moins deux cas.
- [ ] Le croisement avec la chloration de secours est tranché **par écrit**.

## Plan de test manuel
1. Ouvrir un étalonnage, forcer le mode en Auto par **MQTT** → aucune injection, refus au journal.
2. Même situation, demander une **injection manuelle** → refusée, motif propre.
3. Ouvrir un étalonnage, **redémarrer** → la garde tient dès le boot.

## Notes pour agent
⚠️ `pool-chemistry` passe **AVANT** toute écriture de code, et son verdict est **contraignant**.
