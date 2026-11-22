# ADR-0142 — Un compteur de bidon saturé ALERTE, il n'arrête jamais le dosage — et l'écran nomme le compteur, jamais le bidon

- **Statut** : Accepté
- **Date** : 2026-11-22
- **Décideurs** : Nicolas (arbitrage final) + Claude (`pool-chemistry`, `ux-ui-designer`, `architect`)
- **Spec(s) liée(s)** : [bug-211](../../specs/features/done/bug-211-un-bidon-vide-est-annonce-comme-un-stock-faible.md) ; prolonge [ADR-0141](0141-l-appareil-est-temoin-du-froid-jamais-agent.md) (un mot ne promet pas une connaissance absente) et [ADR-0140](0140-quand-un-gate-renonce-a-bloquer-la-contrepartie-est-la-visibilite-permanente.md) (ce qu'on doit en contrepartie quand on renonce à bloquer)

## Contexte

Quand le volume déclaré d'un bidon a été entièrement injecté, le tableau de bord affichait
**« Stock faible (0,0 L) »**. Trois faits, lus dans le code le 2026-11-22, rendent cette phrase
fausse sur deux plans :

1. **L'appareil ne mesure aucun niveau.** `remaining_ml = max(0, contenance déclarée − volume
   injecté compté)` ([`web_routes_data.cpp:774`](../../src/web_routes_data.cpp:774),
   [`ws_manager.cpp:1152`](../../src/ws_manager.cpp:1152),
   [`mqtt_manager.cpp:829`](../../src/mqtt_manager.cpp:829)) — deux grandeurs **déclarées ou
   comptées**, aucune sonde de niveau.
2. **La valeur SATURE.** Le `max(0, …)` borne à zéro : « 0,0 L » s'affiche dès que le compteur
   **atteint ou dépasse** la contenance déclarée, et y reste, potentiellement des jours. Le fait
   réel n'est pas « le bidon est vide » mais « **le compteur est arrivé au bout de ce qui a été
   déclaré** ».
3. **Le compteur dérive.** Deux campagnes sur la pompe ORP : **+5,11 %** puis **+6,25 %**. Ni le
   modèle en pourcentage ni le modèle par démarrage n'est constant — le compteur atteint donc
   zéro **avant** que le bidon ne soit réellement vide.

Il fallait trancher deux choses en même temps : **que dit l'écran**, et **que fait la machine**.
La question de la machine n'était pas rhétorique — l'utilisateur avait d'abord demandé que le
compteur à zéro devienne *« une raison de s'arrêter »*, et `pool-chemistry` avait rendu un **GO
sous conditions** avec arrêt asymétrique (jamais sur l'ORP, sur le pH au-delà d'une marge ×1,10
avec plancher +500 ml).

## Décision

1. **Un compteur de bidon saturé ALERTE, il n'arrête jamais le dosage** — ni sur l'axe pH, ni sur
   l'axe ORP. Le suivi produit reste **purement informatif** ; les gardes qui protègent le bassin
   sont les limites par injection, horaire et journalière, et elles ne changent pas.
2. **L'écran nomme le COMPTEUR, jamais le bidon.** Le texte est « **Compteur du bidon à zéro** » ;
   ⛔ le mot **« vide »** est interdit. Il choisirait une cause parmi plusieurs — tuyau
   d'aspiration pincé, crépine hors du liquide, produit éventé donnent le **même** constat — et
   promettrait une connaissance que l'appareil n'a pas (même défaut que « Hors gel »,
   [ADR-0141](0141-l-appareil-est-temoin-du-froid-jamais-agent.md)).
3. **Aucun nombre dans l'état saturé.** « 0,0 L » est un chiffre produit par une borne, pas une
   mesure : l'afficher lui prête une autorité qu'il n'a pas. Les trois autres états du compteur
   (à renseigner, faible, normal) peuvent porter un nombre, celui-ci non.
4. **La contrepartie du non-blocage est la visibilité** (application d'[ADR-0140](0140-quand-un-gate-renonce-a-bloquer-la-contrepartie-est-la-visibilite-permanente.md)) :
   l'état saturé est rendu en **`danger`**, permanent, avec une phrase d'explication, et ne
   s'éteint que par le geste existant de mise à jour du niveau.
5. **Une contenance absente ou nulle n'est jamais un compteur à zéro.** `0` est une valeur de
   configuration **valide** ([`web_routes_config.cpp:1714`](../../src/web_routes_config.cpp:1714)),
   et `max(0, 0 − injecté)` vaut `0` : sans cette exclusion, tout appareil non configuré crierait
   dès son premier démarrage. Cet état reçoit sa propre sortie — une **invitation** à renseigner
   la contenance, pas une alerte.

## Alternatives considérées

- **Arrêt franc du dosage au compteur à zéro** (rejetée, arbitrage utilisateur du 2026-11-22) —
  il ne protège rien de ce qu'on lui prête : la pompe péristaltique est auto-amorçante, tolère la
  marche à sec, et l'usure de son tube est la même pleine ou vide ; le sur-dosage est **déjà**
  borné par les limites existantes. Il n'achète que l'attention de l'utilisateur — ce qui est le
  rôle d'une alerte, pas d'un arrêt.
- **Arrêt asymétrique recommandé par `pool-chemistry`** (jamais sur l'ORP, sur le pH au-delà de
  ×1,10 et +500 ml — rejetée) — le rapport écrit lui-même, en son point 1, qu'un arrêt
  *« n'ajoute aucune protection sanitaire »*, puis le recommande : les deux ne tiennent pas
  ensemble. Et son coût est réel : **les deux axes sont couplés**. Arrêter l'acide fait monter le
  pH ; au-delà de `kPhOrpInhibit`, l'inhibition coupe **aussi** la boucle ORP. On fabriquerait une
  **coupure de désinfection par ricochet** pour signaler un bidon dont, compte tenu de la dérive,
  on n'est même pas sûr qu'il soit vide. *Une garde qui bloque plus qu'elle ne protège est un
  défaut.*
- **Garder « Stock faible (0,0 L) »** (rejetée) — annonce un reste chiffré là où il n'y a qu'une
  borne, et dit « pensez à en racheter » quand l'état appelle une vérification.
- **Écrire « Bidon vide »** (rejetée) — voir décision 2 ; c'est un diagnostic, pas un constat.
- **Cesser de COMPTER au-delà du zéro** (non retenue dans ce lot, à verser à la réflexion sur le
  biais) — c'est le meilleur argument adverse : sur un bidon réellement vide, l'appareil commande
  des injections qui ne délivrent rien **et incrémente quand même son compteur**, abîmant la donnée
  même qui sert à mesurer la dérive. Réel, mais ça justifie de **ne plus compter**, pas de
  **s'arrêter** : deux choses différentes, et la seconde ne découle pas de la première.
- **Déclencher l'alerte sur le fait mesuré plutôt que sur le compteur**
  (`*_ttt_state == kTttStateMissed`, journée close sans que la cible ait été tenue — partiellement
  retenue) — c'est un verdict **à la journée**, pas un signal immédiat : il ne peut pas porter
  l'alerte seul. Il est donc utilisé pour **choisir la rédaction** de l'explication, pas pour
  déclencher l'état. *Le compteur dit quand s'inquiéter ; la mesure dit si c'est vrai.*

## Conséquences

### Positives
- L'écran cesse d'annoncer un reste chiffré qui n'existe pas, et nomme une grandeur que l'appareil
  possède réellement.
- Aucune coupure de désinfection ne peut naître d'un compteur, donc d'une **déclaration humaine**.
- L'appareil non configuré reçoit une invitation à agir au lieu d'une fausse alerte.
- Aucune ligne de firmware : les trois clés nécessaires (`*_remaining_ml`, `*_container_ml`,
  `*_ttt_state`) sont déjà servies par `/data` et par la trame WebSocket.

### Négatives / dette assumée
- **Un bidon réellement vide ne produit aucun effet sur la conduite de la machine.** Elle continue
  de commander des injections qui ne délivrent rien, et de les compter. Assumé : la seule
  conséquence est une donnée de dérive polluée, pas un risque sanitaire.
- **La page Produits (`/dosages`) affiche toujours « 0,0 L »** dans cet état — elle n'est pas
  traitée par ce lot. Deux surfaces disent donc la même chose de deux façons tant que ce n'est pas
  repris. Écrit noir sur blanc dans [`docs/features/page-dosages.md`](../features/page-dosages.md).
- **L'invitation « Contenance du bidon à renseigner » est masquée quand le capteur est déjà en
  alerte** : `composeSensorBadge()` ne garde un segment `info` que si aucun `danger`/`warning`
  n'existe pour ce capteur. Règle de composition délibérée, aucune exception faite ici — mais
  l'appareil non configuré **et** hors plage ne voit pas l'invitation.
- L'explication du compteur à zéro est figée à deux rédactions ; un troisième cas (dérive connue
  et chiffrée par appareil) demanderait d'y revenir.

### Ce que ça verrouille
- **Aucun signal dérivé du suivi produit ne pourra jamais inhiber le dosage.** Un futur capteur de
  niveau **physique** ne tomberait pas sous cette règle — c'est une mesure, pas un compteur.
- **Un texte ne nomme que la grandeur que l'appareil possède.** « Bidon vide », « réservoir plein »
  et toute formule du même genre restent interdites tant qu'aucune mesure de niveau n'existe.
- **Un état borné par construction n'affiche pas sa borne comme une valeur.**
- Toute alerte assise sur le suivi produit doit exclure explicitement la contenance absente ou
  nulle.

## Références

- Code : [`data/app.js`](../../data/app.js) (`bidonCounterState()`, `buildSensorSegments()`,
  `setBadge()`), [`data/app.css`](../../data/app.css) (`.sensor-badge__cta`,
  `.sensor-badge__explain`)
- Firmware (lecture seule, non modifié) : [`src/web_routes_data.cpp:774`](../../src/web_routes_data.cpp:774),
  [`src/ws_manager.cpp:1152`](../../src/ws_manager.cpp:1152),
  [`src/time_to_target_logic.h`](../../src/time_to_target_logic.h) (`kTttStateMissed`)
- Tests : [`test/decimal_format_vectors.js`](../../test/decimal_format_vectors.js)
- Spec : [bug-211](../../specs/features/done/bug-211-un-bidon-vide-est-annonce-comme-un-stock-faible.md)
- Doc : [`docs/features/page-dashboard.md`](../features/page-dashboard.md),
  [`docs/features/page-dosages.md`](../features/page-dosages.md),
  [`docs/DESIGN_SYSTEM.md`](../DESIGN_SYSTEM.md)
