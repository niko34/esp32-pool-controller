# ADR-0115 — Une garde sur un état partagé vit au point d'usage final, pas sur chaque chemin d'écriture

- **Statut** : Accepté
- **Date** : 2026-10-17
- **Décideurs** : `pool-chemistry` (gate), utilisateur
- **Spec(s) liée(s)** : [feature-163](../../specs/features/done/feature-163-refus-de-dosage-pendant-un-etalonnage.md) · [bug-160](../../specs/features/done/bug-160-l-indicateur-de-stabilite-ment-et-le-dosage-ne-se-suspend-pas.md) · [bug-170](../../specs/features/todo/bug-170-routes-duty-demarrent-une-pompe-sans-aucune-garde.md) (confirme la thèse, hors périmètre du correctif) · [ADR-0107](0107-etat-visible-et-persistant-plutot-que-suspension-invisible.md)

## Contexte

bug-160 (volet 2) a protégé l'étalonnage pH/ORP en **basculant le mode de régulation** en
`manual` à l'entrée de l'écran de calibration (`POST /calibration/enter`), puis en verrouillant
**chaque chemin nommé** capable de réécrire ce mode pendant qu'une session est ouverte :

1. HTTP `POST /save-config` (bug-161, gate C2) ;
2. la commande MQTT `ph|orp_regulation_mode/set` — une automation Home Assistant, ou une simple
   restauration d'état au redémarrage **sans aucun humain**, pouvait remettre la régulation en
   `automatic` pendant qu'une sonde trempait dans un tampon (bug-161, lot C) ;
3. l'écriture directe du champ dérivé `orp_enabled`, chemin non fermé depuis v2.25.0 (bug-161,
   lot C).

Chaque découverte fermait le trou trouvé, sans changer la méthode : protéger **un chemin de plus**.
En instruisant feature-163, le gate `pool-chemistry` a trouvé un **quatrième** chemin — le mode
**Programmé** ne passe par aucune des fonctions gardées par cette liste (`canDose()` /
`evaluateDose()`) — et la spec a elle-même recensé un **cinquième** hors de son périmètre de
correction (`bug-170` : les routes `/pumpN/duty/:duty` démarrent une pompe sans passer par
`injectionAllowedOrReject()`, et ne vérifient donc ni la présence d'eau, ni l'étalonnage).

Trois découvertes successives de trous, puis une quatrième, puis une cinquième : **la liste des
chemins d'écriture n'a jamais été prouvée exhaustive, et rien dans la méthode ne pouvait le
garantir.** Chaque correctif rendait la protection un peu plus large, jamais totale par
construction.

## Décision

Quand un comportement de sécurité doit être suspendu tant qu'un état partagé est actif (ici :
« aucun dosage tant qu'un étalonnage est en cours »), la garde se pose **au point d'usage final**
de l'action protégée — les fonctions qui décident réellement du dosage
(`evaluateDose()`, `evaluateManualInject()`, `evaluateScheduledDose()`,
`injectionAllowedOrReject()`) — et **pas** sur chacun des chemins qui pourraient modifier l'état
intermédiaire (ici : le mode de régulation) dont dépendait l'ancienne protection.

Concrètement (feature-163) : un drapeau **dédié et persisté**, `mqttCfg.calSessionOpen`, distinct
du drapeau de bascule de mode `calSwitchActive`, est lu directement par les quatre points de
décision ci-dessus. Peu importe **qui** a changé le mode de régulation et **comment** — un nouveau
chemin HTTP, une commande MQTT non encore vue, un bug de restauration au boot — la pompe ne part
pas tant que ce drapeau est vrai.

## Alternatives considérées

- **Continuer à fermer les chemins au fur et à mesure qu'ils sont découverts** (méthode de
  bug-160/bug-161) — **rejetée** : trois trous déjà fermés, un quatrième trouvé pendant
  l'instruction même de cette spec, un cinquième ouvert à sa suite. La méthode ne converge pas ;
  elle a converti un problème de sécurité chimique en problème d'exhaustivité d'audit de code.
- **Réutiliser `calSwitchActive` comme prédicat de la nouvelle garde** — **rejetée** :
  `calSwitchActive` reste `false` quand pH et ORP sont déjà en `manual` à l'entrée en calibration
  (`/calibration/enter` n'ouvre alors aucune session de bascule, rien à mémoriser). C'est
  exactement le cas où l'utilisateur est le **plus prudent** — une garde fondée dessus serait morte
  précisément là où elle devrait être la plus fiable.
- **Un drapeau dédié, persisté, lu au point d'usage final** — **retenue** : indépendante du
  nombre de chemins d'écriture du mode (connus ou non), indépendante de l'état du mode lui-même,
  et triviale à tenir correcte au boot (simple relecture NVS, aucun mécanisme de réconciliation).

## Conséquences

### Positives
- La protection ne dépend plus de l'exhaustivité d'une liste de chemins d'écriture : un chemin
  futur, ou un chemin déjà existant mais non encore recensé, est couvert par construction tant
  qu'il finit par appeler une des quatre fonctions de décision.
- Le cas « déjà prudent » (pH et ORP déjà en `manual` à l'entrée) devient protégé pour la première
  fois — il ne l'était pas avant feature-163, aucun mécanisme existant ne le couvrait.
- La garde est testable nativement au même titre que les autres (module pur, ADR-0017) : 17 tests
  dédiés valident le rang d'évaluation et le non-masquage des causes absolues.

### Négatives / dette assumée
- Deux drapeaux désormais nécessaires pour une même session de calibration (`calSwitchActive` pour
  le bandeau UI et l'alerte d'oubli, `calSessionOpen` pour la garde de dosage) — une duplication
  d'état assumée plutôt que de surcharger le sens d'un seul champ.
- Cette décision ne corrige pas rétroactivement `bug-170` (les routes `/pumpN/duty/:duty`) : elle a
  seulement rendu ce trou **visible** en changeant la méthode de recensement. Le correctif reste à
  faire, séparément.
- Un cinquième chemin encore inconnu resterait un risque réel si une future fonction de dosage
  était ajoutée **sans** relire `calSessionOpen` — la garantie tient tant que toute décision de
  dosage passe par l'un des quatre points nommés, pas par construction du langage.

### Ce que ça verrouille
- Toute future protection d'un comportement de sécurité dépendant d'un état modifiable par
  plusieurs chemins (mode, configuration, drapeau applicatif) se pose par défaut **au point
  d'usage final** de l'action protégée, pas en tentant de fermer chaque chemin d'écriture un par
  un. Un futur lot qui reviendrait à la méthode « chemin par chemin » doit justifier pourquoi le
  point d'usage final n'est pas atteignable dans son cas.
- Toute nouvelle fonction de décision de dosage ajoutée au firmware doit, par construction, évaluer
  `calSessionOpen` (ou son équivalent pour un futur état partagé similaire) — l'omission serait un
  sixième chemin de la même famille.

## Références

- Code : [`src/config.h`](../../src/config.h) (champ `calSessionOpen`), [`src/config.cpp`](../../src/config.cpp)
  (persistance NVS `cal_sess_open`), [`src/dosing_logic.h`](../../src/dosing_logic.h) /
  [`.cpp`](../../src/dosing_logic.cpp) (lecture dans les trois fonctions pures),
  [`src/web_routes_calibration.cpp`](../../src/web_routes_calibration.cpp) (ouverture/fermeture),
  [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp) (`injectionAllowedOrReject`),
  [`src/pump_controller.cpp`](../../src/pump_controller.cpp) (fermeture d'un cycle Programmé actif,
  textes de refus).
- Spec : [`specs/features/doing/feature-163-refus-de-dosage-pendant-un-etalonnage.md`](../../specs/features/done/feature-163-refus-de-dosage-pendant-un-etalonnage.md),
  [`specs/features/todo/bug-170-routes-duty-demarrent-une-pompe-sans-aucune-garde.md`](../../specs/features/todo/bug-170-routes-duty-demarrent-une-pompe-sans-aucune-garde.md).
- Doc : [`docs/subsystems/pump-controller.md`](../subsystems/pump-controller.md#garde-rang-4--session-détalonnage-ouverte-tous-modes-feature-163-adr-0115),
  [`docs/API.md`](../API.md#codes-de-refus-409-v260).
