# Page pH — `/ph`

- **Fichier UI** : [`data/index.html`](../../data/index.html) (section `#view-ph`)
- **URL** : `http://poolcontroller.local/#/ph`

## Rôle

Point d'entrée unique pour **tout** ce qui concerne le pH : mesure, mode de régulation, injection manuelle, historique, calibration.

## Structure

⚠️ **Depuis la v2.72.0 (feature-156), la page a DEUX états** — voir [Mode réglage](#mode-réglage-calibration--correction-feature-156-v2720) pour le détail complet. Ce qui suit décrit l'**état courant** (le seul visible par défaut).

En état courant, cinq zones :

1. **Bloc Statistiques** (bandeau compact, sans titre) — valeur pH **officielle** courante en grand (1 décimale, cohérent avec le tableau de bord) + **ligne brute discrète** « brut · maj » + **rangée de chips** : **chip unique sonde + calibration** cliquable (cf. [Chip d'état sonde](#chip-détat-sonde-feature-024)) + dosage du jour (barre de progression). Depuis feature-034, le chip de calibration distinct est **supprimé** : son information est fusionnée dans le chip sonde. ⚠️ **Depuis la refonte de la chaîne de mesure (feature-152, v2.68.0)**, il n'existe plus de chip ni de modal d'état du **filtre** distincts : voir [Ce qui a été retiré, fusionné, conservé](#ce-qui-a-été-retiré-fusionné-conservé-feature-152-v2680). Depuis la **v2.37.0** (feature-096), ce bloc se termine par « **Atteinte de la cible** » (`#ph-ttt-block`) : ligne de valeur + bande de 7 pastilles — voir [Atteinte de la cible](#atteinte-de-la-cible-feature-096-v2370).
2. **Carte Débit d'injection pH** (`#ph-card-dosing`, feature-092, v2.46.0) — insérée **entre** le bloc Statistiques et la carte Régulation, visible **en mode automatique uniquement** : quatre lignes (dose de base apprise, écart courant, rattrapage accumulé, **Demande** en total) et une pastille d'état à 5 priorités. Voir [Carte « Débit d'injection pH »](#carte--débit-dinjection-ph--ph-card-dosing-feature-092-v2460).
3. **Carte Régulation pH** — sélecteur de mode : `Auto`, `Programme`, `Arrêt` (libellés révisés en **feature-161, v2.77.0** — étaient `Automatique`, `Programmée`, `Manuelle` ; voir [Libellés révisés](#libellés-révisés-auto--programme--arrêt-feature-161-v2770)). Sous-blocs conditionnels :
   - **Auto** : pH cible — **contrôle segmenté 4 positions** `#ph_target_group` depuis la v2.25.0 (7,20 / 7,30 / 7,40 / 7,50), avec hint de plage `#ph_target_hint`. Voir [Consigne pH : contrôle segmenté](#consigne-ph--contrôle-segmenté-v2250) et [Règles firmware appliquées](#règles-firmware-appliquées).
   - **Programme** : volume quotidien en mL (borné par `max_ph_ml_per_day`) + bouton Sauvegarder. Depuis la v2.8.0 (feature-011) : ligne « **Débit calculé : X,X mL/min** » (`#ph_scheduled_flow_line` / `#ph_scheduled_flow_value`, alimentée par le champ WS `ph_scheduled_flow_ml_per_min` ; affiche « — » avec `title` « Hors plage de filtration ou données indisponibles » quand la valeur est `null`) + hint statique « Le volume non injecté avant minuit est perdu (pas de report au lendemain). ». Volontairement **pas** de « sur Y h restantes » côté client : l'horloge du navigateur peut différer de celle de l'ESP32, seul le débit (source firmware) est affiché.
   - **Les trois modes** : bloc Injection manuelle (volume + bouton ▶ Injecter / ⏹ Stopper) — ⚠️ **déconditionné du mode depuis feature-161** (était visible en Manuelle seulement) : voir [Injection manuelle déconditionnée du mode](#injection-manuelle-déconditionnée-du-mode-feature-161-v2770).
   - Le bloc « Rattrapage accumulé » qui vivait en pied de cette carte (`#ph-integral-section`, v2.26.0) est **retiré** depuis la v2.46.0 : son contenu est passé dans la carte « Débit d'injection pH ».
4. **Carte Historique** — graphique uPlot (feature-043, ex-Chart.js), plages `Tout` / `30j` / `7j` / `24h`. Depuis la **v2.28.1** (feature-083) : bande fixe **7,2-7,6** (plage idéale de l'eau) + **ligne de consigne pointillée** à `ph_target` — voir [Repères du graphique d'historique](#repères-du-graphique-dhistorique-feature-083-v2281). Ce graphique trace la mesure **brute** et le reste en v2.38.0 — voir [Le graphique d'historique de cette page reste brut](#le-graphique-dhistorique-de-cette-page-reste-brut-feature-095-v2380).
5. **Point d'entrée du mode réglage** (`#ph_cal_trigger_btn`, « **Calibration et correction de la mesure** ») — sous la rangée de chips. **Ni la carte Calibration ni la carte Correction manuelle de la mesure ne sont visibles dans cet état** (changement v2.72.0 — avant cette version, la carte de correction était **toujours affichée** entre Régulation et Historique).

Le bloc Statistiques reste visible **en permanence**, y compris en mode réglage.

> **Calibration accessible dans tous les modes (feature-034)** : le point d'entrée du mode réglage est désormais affiché et fonctionnel quel que soit le mode de régulation — **automatique, programmée et manuelle**. Depuis l'itération 3 de feature-034, ce bouton est placé **sous la rangée de chips** du bloc Statistiques (et non plus en bas de la carte de régulation dans `#ph-calibration-info`, supprimé). Le mode réglage ne modifie pas `ph_regulation_mode` : à la sortie, on **revient au mode précédemment sélectionné** (aucune bascule forcée en automatique).

## Mode réglage (calibration + correction, feature-156, v2.72.0)

Les **deux seuls gestes qui touchent à la justesse de la mesure** — étalonnage de la sonde et correction manuelle — ont quitté l'état courant pour un **mode réglage** dédié, atteint depuis un point d'entrée unique. Voir [ADR-0103](../adr/0103-gestes-d-entretien-quittent-la-vue-courante.md) pour l'arbitrage (mécanisme générique, partagé avec les pages [ORP](page-orp.md#mode-réglage-calibration-seule-feature-156-v2720) et [Température](page-temperature.md#mode-réglage-calibration-seule-feature-156-v2720)).

### Pourquoi le point d'entrée s'appelle « Calibration et correction de la mesure »

Avant ce lot, le bouton s'appelait « Calibrer la sonde » et n'ouvrait que la carte de calibration — la correction manuelle restait **toujours visible** sur la page. En la faisant entrer dans le même mode réglage, un bouton resté « Calibrer la sonde » aurait rendu la correction **introuvable** : personne ne clique sur « Calibrer » pour corriger une mesure sur une bandelette. Le titre doit **à la fois** contenir « calibration »/« calibrer » (contrainte utilisateur) **et** laisser deviner que la correction est derrière — d'où le nom à deux destinations, arbitré par `ux-ui-designer`. Sur l'ORP et la Température, le bouton reste « Calibrer la sonde » : il n'y a **qu'une seule** destination (pas de carte de correction pour ces deux capteurs), le nom suit le contenu.

### Les deux états de la page

**État courant** (par défaut) : les cinq zones listées ci-dessus. **Ni** la carte Calibration **ni** la carte Correction ne sont visibles.

**État réglage** (après clic sur le point d'entrée) : le reste de la page (débit d'injection, régulation, historique) est masqué, et on voit, dans cet ordre :

1. un **lien de retour** (« ← Retour à la page pH ») ;
2. la carte **Calibration de la sonde pH** (inchangée dans son contenu, cf. [Workflow calibration](#workflow-calibration)) ;
3. la carte **Correction manuelle de la mesure** (inchangée dans son contenu, cf. [Carte « Correction manuelle de la mesure »](#carte--correction-manuelle-de-la-mesure--ph-card-tune-feature-155-v2710)) ;
4. un second **lien de retour**, identique au premier.

L'ordre **calibration puis correction** n'est pas arbitraire : dans le cas « sonde non étalonnée » (refus n°3 de la carte Correction), le geste à faire d'abord est **juste au-dessus**, sans qu'aucun texte supplémentaire n'ait à l'expliquer.

Le sous-titre d'en-tête de la page (`<p class="settingsHead__sub">`, normalement « Calibration et régulation du pH ») est **réécrit** en mode réglage : « Calibration de la sonde et correction de la mesure » — et **restauré** à la sortie. L'en-tête lui-même (titre, bloc Statistiques) reste visible dans les deux états : la page reste conceptuellement la page pH, en vue resserrée.

### Mécanisme — `enterMaintenance()` / `exitMaintenance()`

Générique, partagé avec l'ORP et la Température ([`data/app.js`](../../data/app.js), table `MAINTENANCE_PAGES`) : une classe CSS unique (`is-flow-maintenance`) posée sur la vue (`#view-ph`), qui masque tout enfant direct **non** marqué `.flow-maintenance` (les cartes Calibration et Correction, les liens de retour et la région d'annonce `sr-only` portent cette classe). **Aucun style inline** (`hidden` natif uniquement) : un `el.style.display` écraserait la règle CSS du mode sans effet visible — piège déjà rencontré par l'ancien `hidePhCalibrationCard()`.

### Liens de retour et accessibilité

- Deux boutons (`#ph-maintenance-back-top` / `#ph-maintenance-back-bottom`), en haut et en bas de l'état réglage. ⚠️ **Depuis le 13/10/2026 (bug-160, correctif d'interface) leur libellé n'est plus fixe** : il est **dynamique et binaire**, miroir du [bouton de fermeture](#bouton-de-fermeture-dynamique) (`_calSwitchBackLabel()`) :

  | Cas | Libellé des deux liens |
  |---|---|
  | Une bascule a eu lieu (pH et/ou ORP) | « ← Retour à la page pH **et rétablir la régulation** » (idem ORP) |
  | Aucune bascule (exception de sécurité — pH déjà verrouillé par une escalade `phAutoManual*`) | « ← Retour à la page pH » (**inchangé**) |

  ⚠️ **La destination reste toujours annoncée** : ce sont des liens de *retour*, pas des boutons de fermeture — seul le texte qui suit la flèche change (`_calSwitchSetBackLinksLabel()`, qui cible le nœud texte par son type, jamais le `<span aria-hidden>` de la flèche). L'action de rétablissement est **ajoutée** au libellé, jamais **substituée** à la destination.
  ⚠️ **Binaire, pas un troisième état** : le cas « aucune bascule » est réel, pas théorique — si une escalade de sécurité verrouille déjà le pH en Manuel, l'entrée en calibration ne touche à rien côté régulation, et annoncer un rétablissement serait **faux**.

- **Trois chemins rétablissent la régulation basculée, un seul geste cliqué par chemin** : le bouton de fermeture et les deux liens de retour (ci-dessus) portent chacun leur libellé dynamique propre. Le **menu** ne porte, lui, aucun libellé — aucun bouton n'y est cliqué. Sortir par le menu alors qu'une bascule est active affiche à la place un **toast bref** sur la page d'arrivée : « Régulation rétablie » (`_exitAllMaintenanceModes()`, qui capture `_calSwitchOccurred` **avant** l'appel à `exitMaintenance()` — celui-ci le remet à `false` via `onExit`/`_calSwitchExit()`).
  ⛔ **Pas de fenêtre de confirmation sur cette sortie-là** : le code l'interdit explicitement — `showView()` appelle `_exitAllMaintenanceModes()`, jamais `requestExitMaintenance()` (celle-ci ouvrirait une modale) — « une modale qui s'ouvre pendant un changement de route piégerait l'utilisateur dans une page qu'il est déjà en train de quitter » (`app.js:670-673`).
- Une région `role="status" aria-live="polite"` (`#ph-maintenance-announce`, `sr-only`) annonce le changement d'état à l'entrée comme à la sortie — aucun contenu visuel, vocal uniquement.
- Le retour restaure la **position de défilement** mémorisée à l'entrée (`requestAnimationFrame` après le repaint, le temps que les cartes redeviennent `hidden` et que la page retrouve sa hauteur normale) et redonne le focus au point d'entrée.
- **Quitter par le menu** (sans cliquer un lien de retour) ramène automatiquement toute page restée en mode réglage à son état normal (`_exitAllMaintenanceModes()`, appelée depuis `showView()`) — correctif d'un défaut trouvé en test (voir [ADR-0103](../adr/0103-gestes-d-entretien-quittent-la-vue-courante.md#leçon--généraliser-un-mécanisme-en-oubliant-ce-quune-version-spécifique-faisait-en-plus)).
- **Calibration en cours** : le lien de retour reste actif, mais demande confirmation (« Une calibration est en cours — la quitter maintenant ? ») si une calibration pH/ORP est en vol côté client — la commande déjà envoyée à l'EZO n'est pas annulable, seule l'attente du client (minuteur, polling) l'est.

#### Tension assumée : le bandeau pointe UN bouton, trois chemins existent

Le texte du [bandeau de bascule](#bandeau-de-bascule-en-manuel-ph-cal-switch-banner--orp-cal-switch-banner-bug-160) dit « Utilisez le bouton **ci-dessous** pour la rétablir », alors que trois chemins équivalents la rétablissent (bouton, deux liens, menu). Ce texte est **imposé mot pour mot par le gate `pool-chemistry`** et ⛔ **n'a pas été réécrit** par ce correctif. Jugement de l'ergonome (`ux-ui-designer`) : **tolérable** — le bandeau pointe vers **une** action suffisante parmi plusieurs équivalentes, il n'exclut aucune des autres.

#### ⚠️ Point ouvert : quitter EN COURS d'étalonnage rétablit immédiatement la régulation

Si l'écran est quitté (par n'importe lequel des trois chemins) **au milieu** d'un étalonnage — sonde encore plongée dans un bécher, point pas encore validé — la régulation repart **immédiatement** sur la lecture de la sonde, qui peut encore lire la solution tampon plusieurs minutes après la sortie. **Ce défaut n'est pas corrigé par ce lot.**

Le mécanisme **abandonné** lors de la refonte du bail (bug-160, volet 2 — voir [ADR-0107](../adr/0107-etat-visible-et-persistant-plutot-que-suspension-invisible.md)) couvrait précisément ce cas : il maintenait la suspension ~9 min **après** la sortie, le temps que la sonde revienne dans l'eau et que le filtre de mesure (médiane + lissage) absorbe un oubli du bécher. La version actuelle (état visible + geste humain, ADR-0107) a **perdu cette garde** en simplifiant le mécanisme.

Trois options sont sur la table, **non tranchées** :
1. ne rien changer (le bandeau et l'alerte d'oubli 2 h/6 h restent le seul filet) ;
2. demander confirmation si l'on quitte un étalonnage **entamé mais non terminé** (point en cours de lecture, pas encore calibré) ;
3. retarder le rétablissement de quelques minutes après la sortie (réintroduire une forme bornée du mécanisme abandonné).

### Responsive

Conçu **mobile d'abord** (390 px) : `.maintenance-back-link` est `width: 100%`, jamais partagée avec un autre élément sur la même ligne — donc aucun `flex-wrap` « en repli » possible (règle responsive permanente, `CLAUDE.md`). Marge dédiée sous chaque lien pour ne jamais le coller au dernier élément de la carte qui précède. Sur un écran court, le lien du bas n'est atteint qu'après avoir fait défiler les deux cartes — c'est **voulu** (c'est sa raison d'être). ⚠️ Vérifié par lecture de `data/app.css` (largeur, absence de partage de ligne) — pas par capture d'écran sur appareil.

## Badge d'état mesure (`#ph-page-badge`, feature-061, v2.13.0)

Sous le bouton « Calibrer la sonde » du bloc Statistiques, un badge (`role="status"`, masqué par défaut) affiche l'**état de validité de la mesure** quand l'eau ne circule pas ou vient de reprendre. Il est alimenté par les **seuls segments d'état mesure** (`measureStateSegments('ph', …)`, app.js — pas les segments dosage/stock, déjà portés ailleurs sur la page) :

| Condition | Libellé exact | Variant |
|---|---|---|
| `water_present === false` (arrêt franc) | `Filtration arrêtée` | `info` (bleu — état normal, attendre) |
| `water_present === false` + `filtration_state_stale` | `Signal filtration périmé` | `warning` (ambré — anomalie du signal externe) |
| `water_present === true` + `measures_valid === false` | **v2.62.0** : `Reprise automatique dans N min` (`measures_dose_available_in_s`) — `Reprise automatique imminente` sous ~1 min, repli `Mesure en pause, reprise automatique à venir` si le champ manque (firmware < 2.62.0). Avant : `Stabilisation depuis N min` / `Re-stabilisation depuis N min` (v2.34.0), `Stabilisation : ~N min` (avant) | `info` (bleu) |

> **v2.34.0 à v2.61.x (feature-091, historique)** : la purge se terminait sur la **stabilité des mesures**, sans fin datée à l'avance. Le **plafond atteint sans stabilité** n'apparaissait **pas** ici (il ouvrait la porte) : il était signalé par un bandeau dédié, **retiré en v2.62.0** (section ci-dessous). **v2.62.0 (feature-139)** : le badge affiche désormais le délai **restant** avant reprise (`measures_dose_available_in_s`), déterministe.

Pendant ces états, la **valeur pH « actuelle » affiche `--`** (le badge explique pourquoi) et les points ne sont **plus historisés** (trous dans le graphique). Les lectures live de la **calibration** et de la ligne brute `#ph-filter-sub` ne sont **pas** gatées (diagnostic / sonde hors de l'eau). Détails et rétrocompat : [page-dashboard.md](page-dashboard.md#mesures-phorp-non-représentatives--filtration-arrêtée--stabilisation-feature-061-v2130) ; côté firmware : [measure-validity.md](../subsystems/measure-validity.md).

## Affichage mesure filtrée / brute (feature-025)

La valeur affichée **en grand** dans le bloc Statistiques est la **valeur officielle** (`ph` = médiane 9 + lissage exponentiel, champ `phFiltered` équivalent). C'est aussi la valeur utilisée par la régulation. La fonction `_renderFilterSub('ph', json)` ([`data/app.js`](../../data/app.js)) rend une **ligne discrète** `#ph-filter-sub` :

```
brut 7.24 · maj à l'instant
```

- **brut** = `phRaw` (dernière mesure Atlas non filtrée, **sans offset** — diagnostic EMI) ;
- **maj** = âge de la dernière mesure brute valide (`à l'instant` / `il y a N s/min`).

⚠️ **Depuis la refonte de la chaîne de mesure (feature-152, v2.68.0)** : la **médiane a disparu de cette ligne** — « étape interne du lissage, pas un fait à lire en permanence sur l'écran principal » (commentaire du code). La médiane reste consultable en diagnostic dans le modal sonde (cf. [Chip d'état sonde](#chip-détat-sonde-feature-024)) et via [`GET /debug/sensor_filter_state`](../API.md#get-debugsensor_filter_state--pas-dauth-cohérent-avec-autres-debug).

> **feature-067 (v2.20.0)** : la valeur affichée en grand (`ph`), la médiane et la valeur filtrée intègrent l'**offset logiciel** `ph_offset` ; seul `phRaw` reste brut. **feature-155 (v2.71.0)** ne change pas cette composition — seul le **geste** qui définit `ph_offset` change. Voir [Carte « Correction manuelle de la mesure »](#carte--correction-manuelle-de-la-mesure--ph-card-tune-feature-155-v2710).

`--` est affiché si la valeur est `null` (montée en régime, sonde figée ou mesure périmée — **plus de repli silencieux** sur une autre valeur, feature-152).

## Repères du graphique d'historique (feature-083, v2.28.1)

Le graphique de détail (carte Historique) porte deux repères **distincts** :

- **Bande fixe 7,2 – 7,6** (lignes rouges + zones ombrées hors plage) : la **plage idéale de l'eau** selon la littérature (poolplanet — optimum 7,4 ; sous 7,2 : irritation, corrosion ; au-dessus de 7,6 : calcaire, chlore moins efficace). C'est la **seule** borne de graphique codée en dur côté client (`PH_IDEAL_MIN` / `PH_IDEAL_MAX`, commentées avec leur référence dans [`data/app.js`](../../data/app.js)) — **Option B** de la spec 083 ; l'option « bornes servies par le firmware » est écartée pour ce cycle, reportable dans une spec future. L'ancienne bande **7,0-7,4** ne correspondait à aucune référence.
- **Ligne de consigne pointillée** (bleue) à `ph_target` : la valeur **configurée**, dynamique.

> ⚠️ **Trois plages différentes, et elles le restent** (arbitrage du 30/08/2026, consigné dans la spec — ne pas les unifier) :
>
> | | Valeur | Sens |
> |---|---|---|
> | Bande du graphique | **7,2 – 7,6** | plage idéale de l'eau (littérature) |
> | Plage de consigne réglable | **7,20 – 7,50** | ce que l'appareil autorise à viser |
> | Seuils de garde | **6,70 / 7,60** | où le dosage est refusé |
>
> La consigne est **délibérément plus resserrée** que la plage idéale : elle laisse une marge avant l'inhibition du chlore à 7,60. Caler la bande sur la plage de consigne (première version de la demande, écartée) ferait apparaître un pH 7,10 « hors plage » alors qu'il est acceptable.

**Mise à jour sans rechargement** : la ligne de consigne est posée/mise à jour par `loadConfig()` (`phChart?.setTarget(cfg.ph_target)`) — point de passage **unique** couvrant l'UI, l'API et MQTT : chaque sauvegarde recharge `/get-config`, et les trames WS `config` passent aussi par `loadConfig`. La valeur est **normalisée à 2 décimales** avant comparaison (`/get-config` sert un arrondi, le WS le float brut — sans normalisation, chaque trame provoquerait un re-render cosmétique).

**Repli** : consigne jamais servie (firmware antérieur, clé absente) → **aucune ligne tracée**, jamais de valeur inventée (piège maison : clé absente ≠ 0). Une valeur **absente ou non finie** dans une trame est ignorée : une trame partielle ne fait **jamais disparaître** une ligne existante. Les bornes d'axe Y (`calculateAxisLimits`) incluent la consigne en plus des données : la ligne reste toujours visible dans le cadre.

## Le graphique d'historique de cette page reste **brut** (feature-095, v2.38.0)

La v2.38.0 historise la mesure **lissée** (celle sur laquelle la régulation décide) à côté de la mesure **brute**, et en fait la **courbe principale des mini-graphiques du tableau de bord**. Le graphique de la **carte Historique de cette page n'est pas concerné** :

- il agrège **1 point par jour** sur ses plages `Tout` / `30j` / `7j` / `24h` — granularité à laquelle la lissée vaut toujours `null` (elle n'est historisée que sur les points 5 min, ~8 h) ;
- à cette échelle, elle serait de toute façon **indiscernable du brut** : la lissée est un signal passe-bas, sa moyenne journalière coïncide avec celle du brut ;
- laisser `createLineChart` **mono-série** était la réduction de risque du lot.

Les **repères** (bande fixe 7,2-7,6, ligne de consigne) sont donc inchangés et se lisent toujours par rapport à la courbe tracée. La comparaison « ce que la régulation voit » se fait sur le mini-graphique du tableau de bord ou directement sur `/get-history` (`phFiltered`).

Détails : [page-dashboard.md](page-dashboard.md#mini-graphiques-ph--orp--la-courbe-suit-le-nombre-affiché-feature-095-v2380), [ADR-0064](../adr/0064-la-courbe-suit-le-nombre-affiche.md), [history.md](../subsystems/history.md#mesure-lissée-historisée-feature-095-v2380).

## Carte « Débit d'injection pH » (`#ph-card-dosing`, feature-092, v2.46.0)

Une **carte unique** remplace quatre morceaux empilés au fil des versions : le bloc « Rattrapage accumulé » (`#ph-integral-section`), la ligne « Dose de base apprise » (`#ph_ff_line`), la ligne « Débit soutenable » du bloc capacités et le bandeau « Dosage au maximum autorisé » (`#ph_budget_banner`). Elle dit, à tout instant, la phrase que l'utilisateur cherchait sans jamais la trouver : *la régulation demande tant, composé de ceci + cela, borné par ça — et voici pourquoi elle l'applique ou non.*

Point de décision **unique** de sa visibilité : `updatePhDosingCard()`, qui remplace `updatePhIntegralDisplay()` **et** `updatePhActuatorLimitDisplay()` (les deux sont supprimées, pas laissées mortes).

### Placement (arbitrage `ux-ui-designer`, rejoué avec le bloc TTT)

La carte s'insère **après `.ph-stats`** — donc après le bloc « [Atteinte de la cible](#atteinte-de-la-cible-feature-096-v2370) », qui vit *à l'intérieur* de la 2ᵉ colonne des stats — et **avant `#ph-card-regulation`**. Lecture obtenue : **mesure → performance du jour → ce que fait la régulation maintenant → réglages**. Visible **en mode automatique uniquement**.

L'ordre de la spec est **retenu**, mais **sans intitulé de section ni séparateur** entre les deux blocs : deux temporalités très différentes se succèdent (« la cible a été atteinte en 3 h 44 » puis « la régulation demande 4,33 mL/min à l'instant »), et la transition est portée par les éléments existants — la légende « **7 derniers jours — le plus récent à droite** » **ferme** le bloc TTT, le **titre** et la **pastille** **ouvrent** la carte. Ajouter un titre de section ou un `<hr>` aurait créé une hiérarchie qui n'existe pas dans le modèle mental de la page.

`#ph_undersized_banner` est placé **juste sous la carte**, adjacent à ce qu'il explique.

### Les 4 lignes (`.kv.kv--dose`, tableau — **pas** de barre empilée)

Une barre empilée mentirait dès que le terme P est négatif et changerait d'échelle dès que la demande dépasse le plafond ; sur 360 px, trois segments de 20-40 px sont illisibles sans les nombres qu'ils dupliquent. Le liseré du total repose sur `.kv--dose .kv__row:last-child` — **ne rien ajouter après la ligne Demande dans cette `.kv`** (les pieds vivent hors du bloc clé/valeur).

| Ligne | Valeur (`#ph-dose-*-value`) | Note (`#ph-dose-*-note`) |
|---|---|---|
| **Dose de base apprise** | `ph_ff_ml_per_min` — la contribution **appliquée**, celle qui rend le total additionnable. « — » au statut `3` ou si `null` | Dérivée de `ph_ff_status`, **la dose apprise** (`ph_ff_learned_ml_per_min`) descendant ici : statut `0` → « **en attente** — dose apprise X,XX, appliquée à la prochaine session » · `1` → *aucune note* (la valeur parle) · `2` → « suspendue (pH sous consigne, mesure invalide ou réglages modifiés) — dose apprise X,XX conservée » · `3` → « en apprentissage (historique < 3 j) » · `4` → « suspendue (60 % du quota atteint) — dose apprise X,XX conservée ». Aucune note si la dose apprise n'est pas un nombre fini |
| **Écart courant** | `ph_p_term_ml_per_min`, **signé** (« −0,35 mL/min » quand le pH est sous la consigne) | *(vide côté pH — le bornage du couple s'annonce sur la ligne Rattrapage)* |
| **Rattrapage accumulé** | `ph_i_term_ml_per_min` — le rattrapage **servi**, `0` dans la bande morte | Bornage du couple **en tête** s'il y a lieu (voir ci-dessous), puis « réserve N % » (`ph_integral_pct`, servi par le firmware). **Le « N pH·s » a disparu** : jargon sans usage pour la cible |
| **Demande** (total, `border-top`) | `ph_demand_ml_per_min` en gras | « demande instantanée : la régulation attend une mesure stabilisée avant d'ouvrir une session » (6ᵉ cas de pastille seulement, demande non nulle), puis le plafond : bridé → « apport moyen possible X,XX mL/min **en moyenne sur l'heure** » ; sinon → « sous le maximum de X,XX mL/min ». Plafond `null` → **aucune note** |

> ⚠️ **Le total est la DEMANDE, jamais un « appliqué ».** Il n'existe **aucun** champ `ph_applied_ml_per_min`, et il n'en existera pas. Le firmware borne la demande à l'**amplitude de la pompe** (26,0 mL/min sur cet appareil), **jamais** au débit soutenable ; la limite horaire agit **en aval**, par **refus**. Avec la configuration vivante (10 min/h), une demande de 5,20 mL/min est réellement servie **à 5,20 de moyenne jusqu'à épuisement du budget**, puis 0. Afficher `min(demande, soutenable)` comme un débit servi **sous-estimait de 17 %** ce que le bassin reçoit dans les dix minutes suivantes (verdict `pool-chemistry`). D'où : la Demande en total, le plafond en note, sous son vrai nom.

**Note de bornage du couple** — l'égalité de référence n'est **pas** `demande = ff + p + i`, mais `demande = coupleServi + ffServi`, où `coupleServi = demande − ff` se **reconstitue par soustraction** (aucune régulation n'est recalculée côté client, et le couple borné n'est **jamais** réparti entre P et I selon une règle inventée). Quand le couple brut diffère du couple servi de plus de **0,01 mL/min** (seuil impératif : les champs sont servis arrondis à 2 décimales, un `!==` faux-positiverait d'un centime), la ligne le **nomme** : « **plancher appliqué · couple retenu 0,00 mL/min** » si le couple brut est négatif, « **couple non servi · maintien de session** » sinon. Un trait de total sous des nombres qui ne s'additionnent pas, **sans note**, serait le pire des deux mondes.

### Pastille d'état (`#ph-dosing-pill`) — 5 priorités, 3 couleurs

`.pill` dans `.card__head--wrap`, `role="status"` `aria-live="polite"` : **seule zone vocalisée de la carte** (invariant `ux-ui-designer` — deux régions live à 20 px l'une de l'autre se parasitent sur mobile). Dérivée **côté client** de champs servis, dans cet ordre :

| Priorité | Condition | Classe | Libellé |
|---:|---|---|---|
| 1 | cause de refus de ton `fault` (panne sonde, filtre) | `.pill.bad` | « Dosage suspendu · {libellé court} » |
| 2 | `ph_pulse_session === true` | `.pill.ok` | « Injection en cours » |
| 3 | `phMixingDelayActive === true` | `.pill` neutre | « Pause mélange · N min » (`ceil(ph_mix_remaining_s / 60)`) ; « Pause mélange » seul si la valeur manque |
| 4 | cause de refus de ton `normal` (filtration arrêtée, limite horaire, anti-rafale, stabilisation…) | `.pill` neutre | « En attente · {libellé court} » |
| 5 | aucune cause, pas de session, `ph_p_term_ml_per_min === 0` | `.pill` neutre | « En attente · pH à la consigne » |

**6ᵉ cas — « En attente » seul.** Aucune cause servie, pas de session, mais terme P **non nul**. C'est le cas trompeur, et il est atteignable : la porte de session statue sur le **signal lent** (moyenne 10 min), la composition sur l'erreur **instantanée** — avec la dispersion pH de cet appareil, la demande peut être non nulle porte fermée. La ligne Demande porte alors la note « demande instantanée … » qui l'explique : *un chiffre sans motif de non-application est exactement le vide que cette carte existe pour combler*. Un code non répertorié (firmware plus récent) donne aussi « En attente », mais **sans** cette note — la cause existe, la page ne sait pas la nommer.

**Sémantique de couleur** : **vert = pompe commandée, gris = état normal, rouge = panne uniquement. Pas d'ambre** — une pause n'est pas un avertissement. **Aucun glyphe « ● / ○ »** : indiscernables à 12 px, et ils créeraient une seconde sémantique de vert à 20 px des pastilles du calendrier 7 jours. Compte à rebours **uniquement** sur la pause mélange (champ servi et borné, il répond exactement à la question posée) ; **pas** de durée écoulée de session (aucun champ, et un compteur qui tourne n'est que du bruit).

`DOSE_BLOCK_CODES` reste la **source unique** des libellés de refus : la pastille lit un libellé **court** (`pill` / `chip`) et retombe sur `label` à défaut ; le modal filtre garde le texte complet. Cas particulier de la sentinelle `250` (cause de garde, message porteur de valeurs runtime, trop long pour une pastille) : « Sécurité pH/ORP active ».

### Pieds de carte

| Pied | Id | Règle |
|---|---|---|
| « Débits moyens sur la session : la pompe injecte par impulsions. » | *(statique)* | **Toujours** affiché — la session est la maille des débits |
| « La limite horaire (X min/h) autorise Y en moyenne sur l'heure : la pompe sert la demande, puis s'arrête jusqu'à l'heure suivante. » | `#ph-dose-budget-foot` | Affiché quand `ph_limited_by_hourly_budget === true` **et** que le soutenable **et** `ph_limit_minutes` (`/get-config`) sont connus — sans quoi la phrase serait creuse. Phrase validée `pool-chemistry` : elle dit que la pompe **sert la demande puis s'arrête**, et non qu'elle débite en continu à un débit réduit |
| « Débit de la pompe non caractérisé : ces valeurs reposent sur le débit d'usine, pas sur une pesée. » | `#ph-dose-uncharacterized-foot` | `ph_pump_flow_uncharacterized === true`. Qualification de la **fiabilité** des nombres — elle vivait avant dans la ligne « Débit soutenable » retirée. Clé absente ⇒ pied masqué, jamais un `false` inventé |
| Callout « Rattrapage au maximum » | `#ph_integral_warning` | **Déplacé** au pied de la carte, **texte et règle inchangés** : verdict **servi** par le firmware (`ph_integral_saturated`), jamais recalculé ici — le seuil firmware porte sur `demande / soutenable` et lui seul connaît la **durée** (2 h / 6 h). ⚠️ `role="status"` et `aria-live` ont été **retirés** en entrant dans la carte : **ne pas les restaurer** |

### Règles de dégradation — une sentinelle, des tirets, jamais de zéro

| Condition | Comportement |
|---|---|
| Mode de régulation ≠ `automatic` | **Carte entière masquée** : le feedforward et le rattrapage n'évoluent que là ; ailleurs on afficherait un reliquat figé |
| `ph_demand_ml_per_min` **absent** (`undefined`, firmware < 2.46.0) | **Carte entière masquée.** Aucun repli sur l'ancienne présentation, qui est retirée ; aucune erreur console |
| Une valeur `null` | « — » sur sa ligne, la raison en note ; la **pastille** porte la cause |
| **Mesure non représentative** (filtration arrêtée, purge) | Écart et Demande « — » ; **Dose de base et Rattrapage restent affichés** (grandeurs lentes, toujours vraies) ; pastille « En attente · Filtration arrêtée » **en gris, jamais en rouge** |
| Statut FF `3` | Ligne Dose de base « — » + note « en apprentissage (historique < 3 j) », **même si** le firmware sert un `0` de contribution |
| **Archivage de minuit** portant l'anneau à 3 jours | La ligne quitte « en apprentissage » **au rafraîchissement `/data` suivant** (≤ 30 s), sans attendre la filtration du matin — correctif racine du défaut du 03/09 |
| Charge médiane **nulle** avec 3 journées exploitables | « 0,00 mL/min » : observation légitime, **pas** un tiret |
| Plafond `null` (pompe non caractérisée) | Demande sans note de plafond + pied « débit non caractérisé » |
| WebSocket déconnecté | `/data` porte **tous** les champs (parité `PARITE-DATA`) → **aucun clignotement** |

Un rendu qui lance est capté (`try/catch` autour de `updatePhDosingCard()`), pour ne jamais masquer les rubriques suivantes en silence.

### Ce qui a été retiré, déplacé, conservé

**Retiré du DOM** (aucun id mort dans `app.js`) : `#ph-integral-section` (bloc + `<hr>` + paragraphe pédagogique « correction lente… », désormais couvert par le [guide de régulation](../user/src/guide-regulation.html)), `#ph_ff_line`, `#ph_budget_banner` (+ `_text`), la ligne « Débit soutenable » (`#ph_sustainable_value`) du bloc capacités, et le détail `#ph_integral_detail` (« N pH·s »).

**Déplacé** : `#ph_integral_warning` → pied de la carte · `#ph_undersized_banner` → juste sous la carte, **conservé tel quel** (nature différente : diagnostic 3 jours) · `#ph_integral_pct_value` → note de la ligne Rattrapage (« réserve N % »).

**Conservé** : `#daily-dose-ph*`, le bloc capacités `#ph_capacity_block` **réduit à « Maximum journalier »** (`#ph_daily_max_value`) — grandeur **journalière**, pas un débit, qui reste sous la barre du jour et n'est **pas** fusionnée dans l'en-tête `daily-dose` (deux « maximum » côte à côte prêteraient à confusion) —, les chips sonde et filtre, et le modal filtre (la pastille ne remplace pas le détail du code de refus).

Le `hidden` du bloc capacités et celui de `#ph_undersized_banner` sont pilotés par `updatePhDosingCard()` (ils vivent hors de la carte, mais dépendaient de la fonction supprimée) et restent, comme avant, **indépendants du mode de régulation** : un maximum journalier reste une capacité lisible en mode manuel. Sentinelle de présence inchangée : `ph_limited_by_hourly_budget` non booléen ⇒ les deux éléments restent masqués.

### Données consommées

`ph_ff_learned_ml_per_min`, `ph_ff_ml_per_min`, `ph_ff_status`, `ph_p_term_ml_per_min`, `ph_i_term_ml_per_min`, `ph_demand_ml_per_min`, `ph_sustainable_ml_per_min`, `ph_limited_by_hourly_budget`, `ph_integral_pct`, `ph_integral_saturated`, `ph_undersized`, `ph_daily_max_ml`, `ph_pulse_session`, `ph_pump_flow_uncharacterized`, `phMixingDelayActive`, `ph_mix_remaining_s`, `phDoseBlockedCode` — **tous servis à l'identique** par `GET /data` et la trame WS ([ADR-0071](../adr/0071-parite-data-trame-capteurs.md)) ; `ph_limit_minutes` vient de `/get-config`.

Contrats : [API.md](../API.md#champs-de-composition-du-débit-dinjection-feature-092-v2460) · [ws-manager.md](../subsystems/ws-manager.md#champs-sensor_data-ajoutés-en-feature-092--composition-du-débit-dinjection-v2460) · firmware : [pump-controller.md](../subsystems/pump-controller.md#composition-du-débit-dinjection--accesseurs-de-lecture-seule-feature-092-v2460).

## Sens de « en dosage » (feature-094, v2.36.0)

Aucun élément d'interface n'est ajouté, déplacé ni renommé par ce lot — mais **le champ `ph_dosing` que la page consomme ne veut plus dire la même chose** ([ADR-0062](../adr/0062-porte-de-session-ph-dimensionnee-par-le-bruit-de-mesure.md)) :

| | Avant (≤ 2.35.0) | Depuis 2.36.0 |
|---|---|---|
| `ph_dosing === true` | un débit est commandé (≈ la pompe tourne) | une **session de dosage est ouverte** — **la pompe peut être à l'arrêt** |

Le verrou de session pH suit désormais la **porte** de session, décidée sur une moyenne glissante de 10 min de la mesure filtrée, et non plus le débit instantané ([Porte de session pH](../subsystems/pump-controller.md#porte-de-session-ph-robuste-au-bruit-feature-094-v2360)). Pendant une **prolongation** (durée minimale de 30 min murales, débit plafonné à la dose de base puis nul), `ph_dosing` **reste `true`** — jusqu'à 3 h au pire cas.

Conséquences pour cette page :

- **Verrou de calibration** (`_updateInjectButtons`, condition `(ph_inject_remaining_s > 0) || (ph_dosing === true)`, cf. [État de calibration](#état-de-calibration--bouton--calibrer-la-sonde--feature-034)) : le bouton « Calibrer la sonde » reste **désactivé plus longtemps**, y compris pompe à l'arrêt. C'est **cohérent avec l'intention d'origine** du verrou — il a toujours couvert **toute la session** d'impulsions, repos compris — et il va dans le sens **restrictif**, donc aucun changement n'est apporté.
- **Statut de dosage des cartes** : la distinction « **commandé** vs **effectif** » posée en v2.21.1 ([ADR-0045](../adr/0045-etat-impulsion-expose-en-calendrier.md)) reste la règle et devient indispensable — « la pompe est alimentée maintenant » se lit **uniquement** sur `ph_pulse_phase_ms < ph_pulse_ms`, **jamais** sur `ph_dosing`. Voir [page-dashboard.md](page-dashboard.md#statut-dosage-des-cartes-phorp--commandé-vs-effectif-v2211).

## Capacités de dosage et limite d'actionneur (feature-086, v2.29.0 ; remanié en v2.46.0)

Deux éléments exposent encore la [limite d'actionneur pH](../subsystems/pump-controller.md#limite-dactionneur-ph--exposition-et-alerte-de-sous-dimensionnement-feature-086-v2290) ([ADR-0056](../adr/0056-sous-dimensionnement-actionneur-3-jours-80-pct.md)) : la ligne « Maximum journalier » du bloc capacités, et l'alerte de diagnostic. Point de décision **unique** : `updatePhDosingCard()` depuis la v2.46.0 (`updatePhActuatorLimitDisplay()` est **supprimée**), appelée sous `try/catch`. **Sentinelle de présence** inchangée : `ph_limited_by_hourly_budget` non booléen (`undefined`, firmware < 2.29.0) → les deux éléments restent masqués, rien n'est inventé.

> ⚠️ **Deux éléments de cette feature ont été retirés en v2.46.0** ([feature-092](#carte--débit-dinjection-ph--ph-card-dosing-feature-092-v2460)) :
>
> - la ligne « **Débit soutenable** » (`#ph_sustainable_value`) du bloc capacités — le soutenable est désormais la **note du total** de la carte « Débit d'injection », là où il qualifie ce qu'il borne. Sa mention « Indisponible — pompe non caractérisée » est devenue le **pied de carte** « débit non caractérisé » ;
> - le bandeau informatif « **Dosage au maximum autorisé** » (`#ph_budget_banner`) — remplacé par la note « apport moyen possible … en moyenne sur l'heure » sur la ligne Demande **et** le pied de carte « limite horaire ». Il n'existe plus dans le DOM.
>
> Ce qui a changé au passage, et qui compte : l'ancien bandeau annonçait un débit **plafonné**, ce que la pompe ne fait pas. Elle **sert la demande, puis s'arrête** — voir l'avertissement « le total est la Demande » de la carte.

### Bloc capacités (`#ph_capacity_block`) — réduit à une ligne

Sous la barre de dose journalière, présentation volontairement **discrète** (`muted small`) : c'est une **capacité dérivée des réglages** (soutenable × minutes de filtration programmées), pas une mesure.

| Élément | Id | Source | Format |
|---|---|---|---|
| Maximum journalier | `#ph_daily_max_value` | `ph_daily_max_ml` | `N mL/j` (entier, fr-FR) ; `null` → « Indisponible » |

Cette ligne **reste ici** et n'est **pas** fusionnée dans l'en-tête `daily-dose` : c'est une grandeur **journalière**, pas un débit, et deux « maximum » côte à côte prêteraient à confusion. Elle reste affichée **quel que soit le mode de régulation** — un maximum journalier est une capacité lisible aussi en mode manuel.

### Alerte « Capacité de dosage insuffisante » (`#ph_undersized_banner`)

`callout--warn`, `role="status"` — diagnostic d'installation, plus grave qu'une note de plafond, mais **pas une alarme critique**. Affichée quand `ph_undersized` est `true`. Texte **statique** (seul `hidden` est piloté), libellé exact validé par `pool-chemistry` :

> **Capacité de dosage insuffisante**
> Depuis 3 jours, la pompe pH délivre chaque jour tout ce que la limite horaire autorise, sans ramener le pH à la consigne. Le réglage actuel ne couvre pas le besoin du bassin : allonger la durée de filtration ou, après vérification de l'installation, relever la limite horaire.

**Règles de rédaction tenues** :
- **Plus rien à empiler depuis la v2.46.0** : le bandeau informatif qu'elle devait remplacer n'existe plus. L'alerte est le **seul** callout de ce domaine, et elle est placée **juste sous la carte « Débit d'injection »**, adjacente à ce qu'elle explique.
- **Aucune mention de bidon** (leçon v2.26.0 : le client annonçait « vérifiez le bidon » sur une saturation qui désignait la limite horaire).
- Le conseil cite **la filtration en premier** ; le relèvement de la limite horaire n'est proposé qu'**« après vérification de l'installation »** (le relèvement augmente proportionnellement le volume injecté pendant le retard de mélange — point 3 de la spec, différé).
- « 3 jours » est la **constante firmware** du diagnostic (`kPhActuatorLimitConsecutiveDays`), pas un champ servi.

## Atteinte de la cible (feature-096, v2.37.0)

Bloc `#ph-ttt-block`, placé **en pied du bloc Statistiques**, sous les capacités de dosage — donc **au-dessus** de la carte « [Débit d'injection pH](#carte--débit-dinjection-ph--ph-card-dosing-feature-092-v2460) », dont il est séparé sans intitulé ni filet (sa légende « 7 derniers jours » ferme le bloc, le titre et la pastille de la carte l'ouvrent). Patron maison `field__label` + `<strong>` + `<span class="muted small">`, identique aux autres rubriques de cette colonne (pas de `.kv`). Il expose l'[indicateur « temps d'atteinte de cible »](../subsystems/time-to-target.md) ([ADR-0063](../adr/0063-definition-normative-du-temps-d-atteinte-de-cible.md)) : combien de temps la régulation pH met, chaque jour, à ramener le bassin sur sa consigne et à l'y tenir.

> ⚠️ **« Minutes de circulation », jamais « de filtration »** — dans tous les libellés de ce bloc. La base de temps est le compteur de **présence d'eau** (`filtTotalMin`) : sans circulation, ni mélange ni mesure représentative, donc la régulation ne progresse pas.

**Point de décision unique** : `updateTimeToTargetDisplay()` ([`data/app.js`](../../data/app.js)), appelée depuis `updateDetailSections()` sous `try/catch` — donc après une charge `/data` **et** après une trame WS `sensor_data`. Une seule fonction `renderTttChannel(prefix, unit, data)` sert **les deux pages**, paramétrée par l'unité.

**Sentinelle de présence** : `ph_ttt_state` non numérique (`undefined`, firmware < 2.37.0) → **bloc entier masqué**, jamais un « — » qui ferait croire à un indicateur en panne.

### Ligne de valeur (`#ph-ttt-value` + `#ph-ttt-detail`)

| `ph_ttt_state` | Valeur (gras) | Détail (discret) |
|---|---|---|
| `2`, `min > 0` | `en 3 h 53` | `de circulation — écart initial +0,18` |
| `2`, `min === 0` | `déjà à la cible au départ` | `écart initial +0,02` |
| `1` | `en cours — 1 h 10` | `de circulation depuis le départ du jour — écart initial +0,18` |
| `3` | `non atteinte` | `après 10 h 12 de circulation — écart initial +0,24` |
| `0`, `time_synced === false` | `indicateur suspendu` | `heure système non synchronisée — aucune mesure enregistrée` |
| `0`, heure valide | `en attente du départ` | `le chronomètre démarre au premier relevé valide, filtration en marche` |

**Formats** : durée `45 min` sous l'heure, `3 h 53` (minutes sur 2 chiffres), `5 h` si le reste est nul — **espaces insécables** (une durée ne doit pas se couper en fin de ligne). Écart initial pH à **2 décimales**, virgule française, signe **toujours explicite**, moins typographique **U+2212** (jamais le trait d'union, qui se confond avec une puce à 11 px). `ph_ttt_e0` à `null` → « écart initial indisponible », jamais « 0,00 ».

⚠️ Le contrat firmware **ne porte pas d'écart résiduel** : seul `e0` existe. Le détail dit « écart **initial** » — ne pas inventer un « à l'arrêt » que le firmware n'expose pas.

**Drapeau sur le jour courant** : si `ph_ttt_flag` porte au moins un bit et que l'état n'est pas `0`, le détail est suffixé `— jour non comparable (consigne modifiée · boost chlore)`, causes jointes par ` · `.

### Bande de 7 pastilles (`#ph-ttt-days`, `role="list"`)

Sept emplacements, **toujours rendus**, du plus ancien (gauche) au plus récent (droite). Un historique court n'efface aucune case : **le vide EST une information** (« pas de jugement rendu »), pas un trou à masquer.

| Cas | Contenu | Style |
|---|---|---|
| `s === 2`, `m ≤ 120 min` | `3h53` (chiffres tabulaires) | **vert** (`.ttt__day--good`) |
| `s === 2`, `m > 120 min` | `5h12` | **ambré** (`.ttt__day--long`) — seuil **2 h** (`TTT_LONG_MIN`) |
| `s === 3` (non atteinte) | `—` | gris neutre (style de base) |
| `s === 1` (en cours) | `1h10` | gris, contour **tireté** (`.ttt__day--pending`) |
| journée absente | `·` | pointillé estompé (`.ttt__day--none`) |
| **`f & 0x20`** (compteur de filtration en recul, v2.47.0) | `—` — **aucune durée** | pointillé estompé (`.ttt__day--none`), **testé AVANT l'état** |
| au moins un bit de drapeau | suffixe **`*`** | **hachures à 45°** (`.ttt__day--flag`) |

- **`kTttFlagCounterReset` (`0x20`) est testé AVANT l'état, et c'est le cœur du correctif bug-106 (v2.47.0).** Une journée dont le compteur de filtration a reculé (effacement NVS `pool-daily`, restauration d'usine ou de configuration) s'archive en `s = 2` **avec `m = 0`**. Sans ce test, `fmtShort(0)` rendrait **« 0h00 » en `.ttt__day--good`**, parce que `0 <= TTT_LONG_MIN` : **l'ABSENCE de mesure s'afficherait comme la MEILLEURE journée possible**, et tirerait vers le bas toute moyenne de comparaison de versions. Ce n'est pas un échec non plus (pas de `.ttt__day--long`) : on réemploie le **vocabulaire des journées sans entrée** — pointillé estompé, aucune durée. La phrase complète dit « **durée non mesurable** », jamais « déjà à la cible au départ ». Le bit reste par ailleurs un drapeau ordinaire : il ajoute l'astérisque, les hachures et la cause « compteur de filtration remis à zéro » dans la note. Voir [time-to-target.md](../subsystems/time-to-target.md#bit-5--ktttflagcounterreset--cette-valeur-nest-pas-une-mesure-bug-106-v2470) et [ADR-0072](../adr/0072-absence-de-valeur-hors-du-domaine.md).
- **La table des libellés de drapeaux est EXTENSIBLE** (`TTT_FLAG_LABELS`) : un bit inconnu servi par un firmware plus récent est **ignoré** — il ne casse ni la phrase ni la note. Un client **teste des bits, jamais l'égalité**.
- **La couleur ne porte jamais seule l'information** : l'astérisque double les hachures, et chaque pastille porte une **phrase complète** en `title` **et** en `aria-label` (« Vendredi 4 septembre : cible atteinte en 3 h 53 de circulation, écart initial +0,18. Jour non comparable : boost chlore. »).
- **Note visible sous la bande** (`#ph-ttt-note`, `role="status"`) affichée seulement s'il existe au moins un drapeau sur les 7 jours : `* Jours non comparables : consigne modifiée, boost chlore.`
- **Alignement par DATE, jamais par ordre du tableau** : les sept emplacements sont les sept dates attendues (J-6 … aujourd'hui) et chaque entrée de `/get-kpi` est placée par égalité de date. Il **y aura** des trous (appareil éteint une journée, ou journée sans heure synchronisée — le firmware refuse alors d'archiver une entrée datée faux). Un simple `reverse()` aligné à droite décalerait tout ce qui suit un trou, et chaque pastille **nomme son jour** : elle annoncerait la mauvaise date. Un indicateur qui se trompe de jour est pire qu'un indicateur absent.
- **Le dernier emplacement (aujourd'hui) porte l'état VIVANT** (`/data` + WS) dès qu'un chronomètre existe, et non l'entrée « en cours » archivée en NVS à t0 — celle-ci est figée à 0 minute jusqu'à la clôture, la pastille afficherait « 0h00 » (qui se lit « déjà à la cible ») sous une ligne disant « en cours — 1 h 10 ». **Choix délibéré**, pas un bug.

### Source des données

- **Ligne de valeur** : les 4 champs `ph_ttt_*` de `latestSensorData` (`/data` **et** WS, parité stricte).
- **Bande** : `GET /get-kpi`, relu **à l'entrée dans la vue**, puis uniquement au **changement d'état ou de jour civil** — **jamais en polling** (la mémoire n'est écrite que deux fois par jour et par régulation). Après un échec, le drapeau n'est pas réarmé par un rendu : la relecture n'a lieu qu'à la ré-entrée dans la vue.
- **Échec ou `503`** (mémoire indisponible, hors ligne) : la bande est **vidée** et la légende remplacée par « Historique indisponible » — la ligne de valeur, elle, est **conservée** (elle ne vient pas de `/get-kpi`). Jamais de bande vide qui se lirait « aucune journée enregistrée ».

**Volontairement absent du tableau de bord** — cohérent avec l'arbitrage qui avait déjà écarté le bloc « Rattrapage accumulé » : les cartes du tableau de bord sont denses, et c'est une grandeur d'analyse, pas une mesure d'eau.

## ~~Bandeau « Stabilisation des mesures non atteinte »~~ — RETIRÉ (feature-139, v2.62.0)

Présent de la v2.34.0 à la v2.61.x (`#measures_purge_timeout_banner`), ce bandeau `callout--warn` signalait qu'après 90 min de circulation le pH ou l'ORP brut variait encore. **Retiré** : le gate `pool-chemistry` du 2026-10-08 a établi que le mécanisme qu'il signalait (purge « adaptative » sur stabilité des bruts, plancher 10 min / plafond 90 min) était un détecteur d'étendue structurellement aveugle à une mesure durablement fausse — il a été démonté, pas assoupli ([ADR-0092](../adr/0092-retrait-analyse-stabilite-porte-mesures.md), supersède l'[ADR-0059](../adr/0059-purge-adaptative-fenetre-de-medianes.md)). Le motif qu'il affichait (`measures_purge_last_end === 2`) n'est **plus jamais émis** par le firmware (valeur réservée, non réattribuée) : la condition d'affichage ne se déclenche donc plus. Le `<div>` a été retiré du DOM ([`data/index.html`](../../data/index.html)) ; la lecture défensive du code `2` reste dans `updateMeasuresPurgeDisplay()` (ligne « Dernière purge » des [Réglages](page-settings.md)) en cas de firmware ancien ou de rollback.

**Ce qui le remplace** : l'attente après un arrêt d'eau reste visible, mais par un texte **déterministe** qui annonce quand elle se termine (« Reprise automatique dans N min », badge capteur — voir [page-dashboard.md](page-dashboard.md#mesures-phorp-non-représentatives--filtration-arrêtée--stabilisation-feature-061-v2130)), et non plus par un bandeau d'anomalie séparé.

L'ancien doublon ORP (`#measures_purge_timeout_banner_orp`) est retiré pour la même raison — voir [page-orp.md](page-orp.md). L'alerte MQTT `measures_purge_timeout` qui accompagnait ce bandeau est également retirée ([MQTT.md](../MQTT.md#alertes)).

## Carte « Correction manuelle de la mesure » (`#ph-card-tune`, feature-155, v2.71.0)

Carte `#ph-card-tune` (**ex-`#ph-card-offset`**, feature-067). ⚠️ **Depuis la v2.72.0 (feature-156), elle a quitté l'état courant de la page** : elle ne vit plus en permanence entre Régulation et Historique, mais dans le [mode réglage](#mode-réglage-calibration--correction-feature-156-v2720), **juste sous** la carte Calibration. Le geste n'est pas moins accessible pour autant — il est désormais atteint via le point d'entrée « Calibration et correction de la mesure » plutôt que d'être toujours visible. Elle reste **distincte de la calibration EZO** (la carte juste au-dessus, dans le même mode) : deux gestes différents, deux effets différents, rappelés par `#ph_tune_calib_note`.

⚠️ **Depuis la v2.73.0 (feature-157)** : toutes les valeurs de pH de cette carte s'affichent avec **une seule décimale**, et un bouton permet de **supprimer** la correction en vigueur sans attendre un étalonnage. Voir [Une décimale, et un geste de suppression](#une-décimale-et-un-geste-de-suppression-feature-157-v2730) plus bas.

> ⚠️ **Le geste a changé de nature.** Jusqu'en v2.70.0, l'utilisateur réglait un **écart abstrait** (« offset ») entre −0,50 et +0,50 pH. Depuis la v2.71.0, il **amène un nombre** jusqu'à la valeur qu'il a lue sur son propre test (bandelette, PoolLab, pH-mètre) : c'est l'appareil qui en déduit la correction à appliquer. **Le mot « offset » a disparu de l'interface** ; le vocabulaire retenu est **ajuster**, **amener à**, **indiquer**, **corriger** — jamais « saisir » (il n'y a pas de champ, pas de clavier : rien n'est saisi).

### Ce qui a disparu (et ce qui ne l'a pas)

Quatre éléments de l'ancienne carte sont **retirés**, pas déplacés :

- le libellé **« Offset pH »** ;
- le bouton **« Remettre à zéro »** (`#ph_offset_reset`, masqué par défaut) ;
- le **texte d'aide permanent** (`#ph_offset_help`, « Correction logicielle de la mesure pH… pH corrigé = pH brut + offset ») ;
- l'**avertissement à deux paliers** `#ph_offset_warning` (0,15 / 0,30 pH, callout ambré qui recalculait un pH réel probable côté client).

Le **pas-à-pas n'est pas supprimé : il est réaffecté.** Même composant visuel (`−` / valeur / `+`), mais il n'affiche plus un écart signé — il affiche **la valeur que vous amenez**, celle que vous avez lue sur votre test.

### Le pas-à-pas : `#ph-tune-stepper`

| Élément | Détail |
|---|---|
| `#ph_tune_value` (`role="spinbutton"`) | Affiche la **valeur amenée**, au dixième. Position de départ : la mesure **officielle courante** (`ph`, échelle corrigée) arrondie au dixième — zéro appui = aucun changement proposé. Dès le premier appui `+`/`−`, la valeur n'est plus resynchronisée automatiquement sur la lecture vivante. |
| `#ph_tune_dec` / `#ph_tune_inc` | Pas de **0,10** (`ph_offset_step`, servi par `GET /get-config` — même source que l'ancien incrémenteur). **Désactivés aux bornes de l'excursion affichée**, pas d'auto-répétition sur appui long : chaque cran reste un acte délibéré. Flèches clavier (haut/droite/bas/gauche), `Home`/`End` pour les extrêmes. `aria-label` statique aligné sur « 0,1 » (v2.73.0, correctif de revue) — il disait « 0,10 » alors que le rendu dynamique écrit déjà « 0,1 » (règle une décimale) : un lecteur d'écran atteignant la carte avant ce rendu annonçait un pas périmé. |
| Excursion affichée | **±1,00** autour de la **lecture brute courante de l'appareil** (`phValueRaw`), écrêtée à l'échelle physique `[0 ; 14]`. C'est une **plage d'ergonomie**, pas un seuil de sécurité : seule sa **moitié centrale** (±0,50) est effectivement acceptée par le serveur — le reste de l'excursion existe pour que l'utilisateur **voie** qu'il s'éloigne trop, au lieu de buter sans explication sur un bouton désactivé. |

⚠️ **Les bornes de sécurité ±0,50 (`ph_offset_min`/`_max`, servies par `GET /get-config`) sont INCHANGÉES** : `kPhFloorRawHard` et `kPhCeilingRawHard` en dérivent toujours ([ADR-0048](../adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md)). Seule l'**interaction** change, pas ce qui est accepté.

### Les trois nombres, avant validation

`#ph_tune_result` affiche : « **Mesure actuelle : 7,2 → après validation : 7,4** ». Le premier nombre est ce que l'appareil **annonce aujourd'hui** (échelle corrigée, offset courant compris — le même nombre que la valeur en grand du bloc Statistiques) ; le second est la **valeur réellement atteinte après arrondi au dixième** — **jamais** la valeur demandée. Amener le nombre pile à 7,45 quand l'appareil lit en réalité 7,13 donne un écart de 0,32, arrondi à 0,30 : l'appareil atteindra en réalité 7,43 (valeur **interne**, inchangée) — et c'est ce nombre qui **s'affiche arrondi à 7,4** (`_fmtPh`), pour ne jamais promettre une valeur que l'arrondi au dixième ne permettra pas d'atteindre.

⚠️ **Depuis la v2.73.0 (feature-157), ce sont les DEUX seuls nombres affichés à une décimale ici** — la valeur `valeurAtteinte` reste calculée en interne sans arrondi supplémentaire (voir [frontière affichage/calcul](#une-décimale-et-un-geste-de-suppression-feature-157-v2730)) ; seule sa représentation textuelle change.

### Avant le geste : le geste, puis l'enjeu — un seul bloc, deux paragraphes

`#ph_tune_stakes` (callout d'information, toujours visible, **avant** le pas-à-pas) porte **les deux textes**, en deux paragraphes, le second en **gras**.

Le premier dit le **geste** et le **pourquoi** de l'écart, sans jamais mettre la sonde en cause :

> Si votre test donne une valeur différente de celle affichée, indiquez-la ici : l'appareil corrigera sa mesure. L'écart vient de l'installation, pas de la sonde.

Le second dit l'**enjeu** — texte **imposé mot pour mot ET ponctuation pour ponctuation** par le gate `pool-chemistry` (condition B4) :

> **Ce réglage ne vaut que ce que vaut votre test. La valeur que vous indiquez ne change pas que l'affichage : c'est elle qui décidera des injections d'acide.**

⚠️ **POURQUOI UN SEUL BLOC, ET POURQUOI EN GRAS.** Les deux textes vivaient dans deux blocs séparés (`#ph_tune_explain` et `#ph_tune_stakes`). La carte comptait alors **6 blocs visibles contre 3 avant la refonte**, alors que la consigne utilisateur était de **simplifier** — chaque ajout justifié, le total contredisant la demande, c'est le travers que décrit la règle d'accumulation. Décision utilisateur : **fusionner** (6 → 5 blocs).
⚠️ Le gate a alors relevé que la fusion **affaiblissait** l'avertissement : noyé dans quatre lignes de prose continue, il perdait sa force. Et qu'une tentative de liaison par virgule **rétrogradait** la clause finale — le deux-points de « *l'affichage : c'est elle qui décidera* » est un deux-points **de conséquence**, qui met en position focale l'affirmation la plus importante. ⇒ **Deux paragraphes dans le même bloc, le second en `<strong>`** : on garde les 5 blocs **et** la ponctuation d'origine. ⚠️ Ne pas re-fusionner en un seul paragraphe, ne pas retirer le gras.

⚠️ **Cette phrase n'est pas un avertissement de confort : la valeur indiquée sert au dosage, pas seulement à l'affichage.** Elle entre dans `getPhFiltered()` ([`sensors.cpp`](../../src/sensors.cpp)), la valeur que lisent **à la fois** la régulation et les gardes de sécurité pH. Une valeur mal amenée **déplace le pH réel du bassin tel que l'appareil le perçoit** — indiquer 7,4 au lieu de 6,4 ferait croire l'eau plus basique qu'elle n'est, et ferait injecter de l'acide dans une eau déjà acide.

### Les cinq états — prêt, ou l'un des quatre refus

Un seul callout `#ph_tune_blocked` (ambré, `role="status"`, `aria-live="polite"`), évalué dans le **même ordre** que le serveur (`src/ph_tune_logic.h`, `evaluatePhTune()`) : un même jeu de conditions produit toujours le même verdict côté aperçu et côté serveur.

| Ordre | Condition | Texte affiché |
|---:|---|---|
| 1 | Mesure de référence indisponible (`phValueRaw` NaN) | « L'appareil n'a pas de mesure assez récente. Attendez qu'une valeur s'affiche, puis recommencez. » |
| 2 | Filtration arrêtée, ou démarrée depuis moins de **10 minutes** — ⚠️ depuis la v2.74.0 (feature-158), **le décompte peut partir d'un report** : si l'appareil vient de redémarrer brièvement sans que l'eau cesse de circuler, le temps déjà écoulé avant la coupure est recrédité, voir ci-dessous | « La filtration doit tourner depuis 10 minutes pour que l'eau testée soit celle qui passe devant la sonde. Possible dans N min. » — **avec décompte vivant**, jamais un refus sec |
| 3 | Sonde non étalonnée (`phCalPoints < 2`) | « La sonde n'a pas encore ses deux points de référence. Faites d'abord l'étalonnage (tampon 7 puis tampon 4), puis revenez ici. » — ⚠️ **le bouton « Aller à l'étalonnage » (`#ph_tune_goto_cal_btn`) a disparu en v2.72.0** (B10) : il faisait défiler vers une carte Calibration lointaine, désormais **juste au-dessus** dans le même mode réglage, le geste à faire est déjà sous les yeux |
| 4 | Écart demandé hors **±0,50** | « Trop loin de ce que l'appareil mesure. Refaites le test (bandelettes non périmées, lecture à la bonne échelle) et vérifiez l'étalonnage de la sonde avant de recommencer. » |
| — | Aucun refus | Callout masqué, bouton « Corriger » actif |

⚠️ **Aucun de ces textes ne met la sonde en cause.** Un écart hors fenêtre dit « hors fenêtre de diagnostic », jamais « sonde morte » (même nuance que le [chip de santé sonde](#chip-détat-sonde-feature-024)) : un écart de plus de 0,5 pH signale le plus souvent un test périmé ou une lecture à la mauvaise échelle, pas une pièce défaillante.

Les **10 minutes** de filtration (`kPhTuneMinFiltrationMs`, servi en lecture seule par `ph_tune_min_filtration_ms`) couvrent le temps nécessaire pour que l'eau testée soit celle qui passe réellement devant la sonde — le plancher instrumental calculé par `pool-chemistry` est de ~180 s (médiane 9×5 s + 3τ), mais l'utilisateur a explicitement écarté cette option plus courte le 2026-10-11 : les 10 minutes sont confirmées, avec décompte visible.

⚠️ **Depuis la v2.74.0 (feature-158), cette garde ne se réarme plus systématiquement après un redémarrage de l'appareil** (déploiement OTA, auto-guérison mémoire) : si la pompe de filtration n'a jamais cessé de tourner pendant la coupure (redémarrage logiciel court, eau présente avant et après), le temps déjà écoulé est recrédité et le décompte reprend là où il en était. **Ce qui réarme encore pleinement la garde** : une coupure secteur (la mémoire qui porte le report ne survit pas à une coupure d'alimentation), un arrêt de la circulation d'eau de 180 s ou plus (`kWaterStopGraceS`, même grâce que la purge des mesures pH/ORP), et toute corruption de la mémoire de reprise (fail-closed, repli automatique sur le comportement d'avant ce lot). Détail firmware : [docs/subsystems/filtration.md](../subsystems/filtration.md#reprise-de-la-durée-de-filtration-à-travers-un-redémarrage-feature-158-v2740) et [ADR-0104](../adr/0104-la-continuite-de-l-eau-a-un-producteur-unique-et-des-consommateurs.md). Le texte affiché par cette carte **ne change pas** — seul ce qui alimente le décompte (`filtration_elapsed_s`) change de sémantique.

### Distinction avec l'étalonnage de la sonde

`#ph_tune_calib_note` (texte discret, toujours visible) :

> Cela n'étalonne pas la sonde : pour cela, utilisez la carte « Calibration de la sonde pH » ci-dessus.

⚠️ Le texte disait « plus bas » avant la v2.72.0 (la calibration vivait après l'historique) ; depuis ce lot, elle est **juste au-dessus** dans le mode réglage — texte corrigé en conséquence, sinon la phrase devient fausse.

Les deux gestes ne doivent jamais être confondus : cette carte **corrige une mesure** (une constante ajoutée en sortie du filtre), l'étalonnage **recalcule la pente et le zéro de la sonde** elle-même. Accorder l'appareil sur un test avant d'avoir étalonné la sonde (< 2 points) est un contresens — refus 3 ci-dessus — car un offset est une **constante** alors qu'une erreur d'étalonnage est **proportionnelle** : sans 2ᵉ point, une pente dégradée serait figée dans la correction, au seul point d'accord, et fausse partout ailleurs.

### Enregistrement : `POST /tune-ph-offset`

Le bouton **« Corriger »** (`#ph_tune_submit_btn`) envoie `{"ph_reference": <valeur amenée>}` à `POST /tune-ph-offset` — **jamais** un écart déjà calculé (voir [API.md](../API.md#post-tune-ph-offset--critical)). C'est le **firmware** qui calcule l'écart, avec **sa** mesure brute (`getPhValueRaw()`) à l'instant où il reçoit la requête : l'aperçu de cette carte (les trois nombres, l'état bloqué) n'est qu'une **prévision**, jamais la décision — un clic sur « Corriger » encore trop tôt aux yeux du firmware affiche son propre message, avec son propre décompte, qui fait toujours foi.

Bouton désactivé tant qu'aucun appui `+`/`−` n'a eu lieu (zéro appui = aucune correction envoyée, jamais un cran appliqué par accident) ou qu'un refus est affiché. Succès : bouton « Corrigé » 2 s puis retour à la lecture vivante (`loadConfig()` resynchronise la config). Échec : message serveur affiché **tel quel** (jamais réécrit), bouton « Erreur » 2 s.

La correction s'applique **immédiatement** (ADR-0039 inchangé : appliquée en sortie des accesseurs, sans attendre le remplissage d'une fenêtre). Voir [ADR-0101](../adr/0101-la-correction-nait-d-une-mesure-jamais-d-un-ecart.md) pour l'arbitrage qui sous-tend ce geste.

### Correction devenue caduque après un étalonnage (`#ph_tune_stale_banner`, feature-156, v2.72.0)

Un étalonnage modifie le zéro **et** la pente de la sonde : une correction manuelle posée **avant** le dernier étalonnage réussi compensait un état de la sonde qui **n'existe plus** — elle est **caduque**. Le firmware le détecte (champ `ph_offset_stale`, voir [API.md](../API.md#get-get-config--write)) et le signale par une bannière **persistante**, en tout premier dans `#ph-card-tune`, avant même `#ph_tune_stakes` : elle engage le dosage, elle pèse plus lourd que le conseil transitoire de la carte Calibration.

Condition d'affichage : `ph_offset_stale === true` **et** la correction en vigueur est non nulle (`renderPhOffsetStaleBanner()`, [`data/app.js`](../../data/app.js)). Ce verdict est calculé **par le firmware** et **jamais recalculé côté client** — voir [ADR-0102](../adr/0102-ordre-des-gestes-de-justesse-par-compteur.md) — et il **survit** à un rechargement de page et à un redémarrage de l'appareil, contrairement au conseil transitoire de la carte Calibration (`#ph_cal_final_advice`, voir [Workflow calibration](#workflow-calibration)).

⚠️ **Texte imposé mot pour mot par le gate `pool-chemistry`, après un premier brouillon refusé** (« Correction désactivée par l'étalonnage » — **faux** : `applyPhOffset` continue d'ajouter la correction à chaque lecture tant que l'utilisateur n'a pas tranché, un utilisateur rassuré par « désactivée » aurait pu laisser la bannière des semaines pendant que la correction périmée orientait les injections d'acide) :

> **Correction à revoir après l'étalonnage**
> Une correction de **{valeur} pH** **reste appliquée à la mesure** : elle continue d'orienter le dosage tant que vous n'avez pas choisi. Elle a été posée **avant** l'étalonnage qui vient d'avoir lieu et compensait un réglage de sonde qui a changé — elle n'est peut-être plus justifiée.

⛔ « **reste appliquée** » et « **continue d'orienter le dosage** » sont **non négociables** — ne pas les reformuler.

Deux issues explicites (`#ph_tune_stale_cancel_btn` / `#ph_tune_stale_keep_btn`), **pas** de remise à zéro automatique (« rien dans le dos de l'utilisateur sur une valeur qui engage le dosage ») :

- **« Annuler la correction »** — remet `ph_offset` à 0 via `POST /save-config`, **sans** les gardes de `/tune-ph-offset` (10 min de filtration, sonde étalonnée…) : exiger une attente pour **retirer** une correction serait une garde qui bloque plus qu'elle ne protège (verdict `pool-chemistry`).
- **« Garder la correction »** — ré-envoie la valeur courante par la **même route** `POST /save-config` : ce ré-envoi **re-estampille** la correction (`setPhOffsetStamped()`) et **éteint durablement** le verdict de caducité — c'est la manière dont l'utilisateur confirme qu'il assume la valeur après l'avoir vue signalée comme périmée. Message de confirmation : « Correction conservée — **vérifiez-la par un test fiable** » (condition mineure du gate).

⚠️ **Échec du POST sur ces deux boutons** (correctif de revue, v2.73.0) : même traitement que le bouton « Supprimer la correction » ci-dessous — `sendConfigDetailed` plutôt que `sendConfig`, message du contrôleur affiché tel quel au lieu de « réessayez ». Même nuance : les deux n'écrivent que `0` ou la valeur **déjà en vigueur**, le refus de plage est hors d'atteinte ; seul un contrôleur injoignable produit un échec réel.

**Rétrocompatibilité assumée** : sur un appareil mis à jour depuis une version antérieure (compteurs d'horodatage à `0` des deux côtés), une correction héritée n'est **jamais** signalée caduque tant qu'aucun étalonnage n'a eu lieu depuis la mise à jour — pas de fausse alerte sur tout le parc au premier démarrage. Elle le sera au **prochain** étalonnage.

### Une décimale, et un geste de suppression (feature-157, v2.73.0)

#### Une seule décimale — et seulement à l'affichage

Toutes les valeurs de pH de cette carte (mesure actuelle, valeur amenée, valeur atteinte, le pas) s'affichent désormais avec **une seule décimale**, conformément à la règle produit du 2026-10-10. ⚠️ **La frontière est l'AFFICHAGE, pas le CALCUL** (précision de l'utilisateur, 2026-10-12) : la règle porte sur ce qui **se voit** et sur ce que l'utilisateur **choisit** (le pas-à-pas est déjà au dixième, aucun changement là) — **pas** sur ce qui se calcule. La valeur envoyée à `POST /tune-ph-offset`, l'écart calculé par `evaluatePhTunePreview()` et la valeur `phValueRaw` restent **inchangés, précis, non arrondis** : seule la fonction de formatage `_fmtPh()` a changé (`toFixed(2)` → `toFixed(1)`), jamais une valeur réinjectée dans un calcul. Un arrondi qui remonterait dans la logique serait une **erreur réelle**, pas un confort de lecture.

`_fmtPh()` est une fonction **partagée** ; tous ses appelants ont été vérifiés avant le changement. Elle est exclue d'un seul endroit : **la fenêtre d'état du filtre**, qui utilise `_fmtFilterVal()` (fonction distincte, non touchée) et reste à 2 décimales pour un diagnostic fin (ligne `brut 7.24 · maj à l'instant`, voir [Affichage mesure filtrée / brute](#affichage-mesure-filtrée--brute-feature-025)). **L'écran « Consigne pH » change aussi de format** (contrôle segmenté, hint de plage, avertissement de borne haute — voir [Consigne pH : contrôle segmenté](#consigne-ph--contrôle-segmenté-v2250) ci-dessous) : il affichait « 7,20 » là où « 7,2 » suffit — **mise en conformité** avec la règle produit, pas un élargissement du périmètre du lot. **L'écran de calibration EZO garde deux décimales** (`#cal-ph-mid-readout` / `#cal-ph-low-readout`, lecture brute d'étalonnage où la précision compte) : ⚠️ **point soumis à l'utilisateur, non tranché** — ce n'est pas une décision acquise, à rouvrir si demandé.

Un onzième site de formatage pH existait **hors** de `_fmtPh()` : la bannière « correction à revoir » (`#ph_tune_stale_value`) formatait l'offset en dur. Elle passe désormais par la même fonction commune.

#### Bouton « Supprimer la correction »

Jusqu'à ce lot, la correction en vigueur n'était **affichée nulle part** sur la carte en dehors du pas-à-pas lui-même, et le seul moyen de la retirer était le bouton « Annuler la correction » de la bannière « correction caduque » (visible **uniquement** après un étalonnage). Une ligne **« Correction actuelle : {valeur} pH »** (`#ph_tune_remove_label` / `#ph_tune_remove_value`) et un bouton **« Supprimer la correction »** (`#ph_tune_remove_btn`, `btn--danger`) couvrent désormais ce cas en permanence :

- **Visible** si et seulement si la correction en vigueur (`ph_offset`) est **non nulle** — pas de bouton inerte sur une carte qu'on vient d'alléger.
- **Masquage mutuel avec la bannière « correction à revoir »** (`#ph_tune_stale_banner`) : les deux **ne coexistent jamais**. Quand la bannière est affichée, elle seule agit — ses deux boutons (« Annuler » / « Garder ») couvrent déjà le cas de la suppression ; afficher les deux aurait créé un doublon visuel disant la même chose deux fois.
- Écrit `ph_offset: 0` par `POST /save-config` — **pas** `/tune-ph-offset` : le chemin était déjà validé au gate 2 de feature-156 (les gardes de `/tune-ph-offset` — 10 min de filtration, sonde étalonnée — protègent l'**ajout** d'une interprétation, pas son **retrait**).
- **Aucune confirmation** avant suppression (verdict `pool-chemistry`) : le geste est **réversible** (on peut re-corriger) et va vers le **défaut sûr** (plus de correction = mesure nue de la sonde). *« Les gardes de `/tune-ph-offset` protègent l'AJOUT d'une interprétation, pas son RETRAIT. »*
- **Échec du POST** (correctif de revue, v2.73.0) : la correction reste en vigueur, **aucun** toast de succès n'est émis, et le message montré est désormais celui du **contrôleur** (`res.message || res.error`, via `sendConfigDetailed` plutôt que `sendConfig`) au lieu du générique « réessayez » — qui invitait à répéter un geste voué à échouer à l'identique. ⚠️ **Le refus de plage est hors d'atteinte sur ce bouton** : il n'écrit que `0`, toujours valide ; le cas réel est le **contrôleur injoignable**, d'où l'intérêt de voir le vrai motif plutôt qu'une invitation à réessayer.
- **Accessibilité** : `#ph_tune_remove_btn` porte `aria-describedby="ph_tune_remove_label ph_tune_remove_hint"` — la **valeur** retirée et sa **conséquence** (« Sans correction, l'appareil régule sur la mesure de la sonde telle quelle. »), dans l'ordre de lecture, pas la valeur seule.
- **Placement** : classe locale `.action-destructive` qui écarte ce bouton de l'action principale de la carte — `margin-left: auto` en vue large (rejeté à l'opposé), `margin-top: 12px` sous 768 px (les boutons s'empilent en pleine largeur et le `gap` de 8 px ne suffisait pas pour une cible tactile). Classe **locale**, pas une règle sur `.detail-actions` (ce conteneur sert sur tout le produit).

Textes imposés par le gate `pool-chemistry`, à ne pas reformuler :

- Sous le bouton (`#ph_tune_remove_hint`) : « **Sans correction, l'appareil régule sur la mesure de la sonde telle quelle.** »
- Message de succès après suppression (toast) : « **Correction supprimée — la mesure affichée est celle de la sonde.** » ⛔ **Sans** invitation à en reposer une — ce serait le piège symétrique de celui déjà évité sur le bouton « Garder » de la bannière caduque : encourager un geste risqué quand l'instrument de référence de l'utilisateur est une bandelette (±0,5 pH).

⚠️ **Effet de bord à connaître** : retirer un offset **positif** abaisse le pH corrigé et peut le faire **repasser sous le seuil d'inhibition ORP** (`kPhOrpInhibit`, 7,60) — donc **lever l'inhibition** du dosage chlore, qui peut reprendre. Comportement **correct** (la mesure nue de la sonde fait foi), mais à savoir avant de cliquer : **cas réel du 2026-10-12**, correction de **+0,50** en vigueur, pH affiché **7,66** (chlore inhibé) — supprimer la correction ramène l'affichage à **~7,15**, sous le seuil de 7,60, ce qui lève l'inhibition ORP.

#### Un défaut réparé

Le bouton « Remettre à zéro » (`#ph_offset_reset`) avait été retiré en feature-156 (v2.72.0) au motif que revenir à zéro n'était « qu'une position du pas-à-pas » — **ce motif était faux** : supprimer une correction est un **geste distinct**, pas une valeur parmi d'autres. Conséquence constatée en production : une correction devenue inutile restait en vigueur, sans aucun moyen direct de la retirer en dehors d'un étalonnage. Ce lot répare ce défaut, sous une forme différente (bouton dédié « Supprimer la correction », masqué par défaut et par la bannière caduque) plutôt qu'en ressuscitant l'ancien bouton.

### Consigne pH : contrôle segmenté (v2.25.0)

Le champ `<input type="number">` est remplacé par un **contrôle segmenté** `#ph_target_group` construit dynamiquement par le JS : une position par valeur autorisée (4 positions à 7,20 / 7,30 / 7,40 / 7,50). Un `<input type="hidden" id="ph_target">` porte la valeur courante pour le reste du code.

- **La saisie hors plage devient impossible depuis l'interface** : ni champ libre, ni curseur `input[type=range]`. Un clic sur une position **enregistre immédiatement** (autosave, pas de bouton Sauvegarder).
- **Aucune borne n'est codée en dur côté client.** Les positions sont calculées depuis `ph_target_min` / `ph_target_max` / `ph_target_step`, servis par `GET /get-config`. Un repli de secours existe pour un firmware antérieur qui ne servirait pas ces champs, mais il n'a **jamais autorité** : un lot de bornes absent ou incohérent (borne inversée, pas nul) est ignoré **en bloc** et les bornes déjà connues sont conservées — indispensable, car la trame `config` du WebSocket ne transporte pas ces bornes.
- Le nombre de positions est **borné à 20** (garde-fou : un pas aberrant servi par un firmware corrompu ne doit pas fabriquer des milliers de boutons).
- `#ph_target_hint` n'est plus un avertissement de validation (il n'y a plus rien à valider côté client) : il rappelle la plage servie par le firmware et le **référentiel** des seuils — « Plage autorisée : 7,20 à 7,50, par pas de 0,10. Les seuils de sécurité du dosage portent sur le pH affiché (offset compris). » Le **rouge est réservé aux échecs de sauvegarde** (`showPhTargetSaveError`).

> ⚠️ **Pourquoi pas un `input[type=range]`** : sur mobile, un curseur n'affiche pas sa valeur, n'a pas de cran tactile et se déclenche au premier contact — y compris pendant un scroll. Voir [ADR-0048](../adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md).

### ⚠️ feature-164 (v2.80.0) — même contrôle, une SECONDE instance sur le tableau de bord

La carte pH du [tableau de bord](page-dashboard.md#régler-la-consigne-depuis-la-carte-feature-164-v2800)
porte désormais le **même geste** (`#dashboard-ph-target-group` / `#dashboard-ph-target`),
généré par la **même** fonction (`buildPhTargetSegments()`, généralisée pour accepter un second
jeu d'identifiants) — mêmes bornes serveur, ⛔ **aucune valeur en dur**, aucun second composant.

⚠️ **Sur cette page, l'enregistrement reste au clic** (autosave, inchangé) — **sur la carte**, un
bouton **Sauvegarder explicite** est requis (ADR-0110 : régler une consigne est une commande
**différée**, jamais en enregistrement implicite). Cette asymétrie est assumée : la page de
réglage est une surface **dédiée** où le clic sur une position *est* le geste de confirmation ;
la carte d'accueil, elle, exige un geste **séparé**.

⚠️ **Verrou d'étalonnage étendu (C10)** : `calActive` **seul** désactive désormais aussi ce
contrôle (sur les deux surfaces) — ⛔ **pas** l'escalade de sécurité pH (qui force déjà le mode en
Arrêt et ne gagnerait rien à verrouiller la consigne en plus). Détail :
[pump-controller.md](../subsystems/pump-controller.md#verrou-étendu-aux-consignes-feature-164-gate-c5c6).
⚠️ **Un étalonnage, quelle que soit la sonde (pH OU ORP), verrouille les DEUX consignes** —
`calSwitchActive` est un drapeau global, sans distinction de canal.

⚠️ **Changer la consigne en marche purge l'intégrale pH côté firmware** (sans toucher aux
compteurs de dosage) — transparent pour l'utilisateur, détail dans
[pump-controller.md](../subsystems/pump-controller.md#purge-de-lintégrale-sur-changement-runtime-de-consigne-feature-164-gate-c5c6).

### ~~Offset pH : incrémenteur~~ et ~~deux paliers d'avertissement~~ — RETIRÉS (feature-155, v2.71.0)

Présents de la v2.25.0 à la v2.70.x : un incrémenteur `#ph-offset-stepper` réglant un **écart signé** `[−0,50 ; +0,50]`, un texte d'aide permanent (`#ph_offset_help`), et un callout ambré à **deux paliers** (`#ph_offset_warning`, 0,15 / 0,30 pH) recalculant côté client le pH réel probable en cas d'offset erroné. Les trois ont **disparu** avec le changement de geste décrit ci-dessus — voir [Ce qui a disparu (et ce qui ne l'a pas)](#ce-qui-a-disparu-et-ce-qui-ne-la-pas). L'enregistrement ne passe plus par `POST /save-config {"ph_offset": …}` depuis cette carte (route toujours valide pour un appel API direct, voir [API.md](../API.md#post-save-config--critical)), mais par [`POST /tune-ph-offset`](#enregistrement--post-tune-ph-offset) — voir la section ci-dessus.

### Diagnostic dans le modal sonde pH

⚠️ **Réduit par la refonte de la chaîne de mesure (feature-152, v2.68.0)** — jusqu'en v2.67.x ce diagnostic vivait dans un `<dialog id="ph-filter-modal">` séparé et comptait cinq lignes (brute, offset, corrigée, médiane, filtrée) plus les pics rejetés / l'âge / le « filtre prêt » / la raison de blocage. Ce modal a **disparu** (voir [Ce qui a été retiré, fusionné, conservé](#ce-qui-a-été-retiré-fusionné-conservé-feature-152-v2680)) ; ce qui reste utile en diagnostic est **absorbé** dans `<dialog id="ph-probe-modal">` (« État sonde pH », cf. [Chip d'état sonde](#chip-détat-sonde-feature-024) → [Modal détails](#modal-détails)) :

| Ligne | Élément | Contenu |
|---|---|---|
| Valeur brute | `#ph-fm-raw` | `phRaw` — brut Atlas EZO, **sans offset** |
| Valeur retenue | `#ph-fm-filtered` | **valeur officielle** (médiane 9 + lissage, **corrigée**) — identique au champ `ph` |
| Dernière mesure | `#ph-fm-age` | âge de la dernière brute reçue côté UI |

L'offset, la valeur « corrigée » intermédiaire et la médiane en tant que ligne séparée **ne sont plus affichés** : il n'y a plus qu'une seule valeur officielle à lire (fin du repli silencieux, feature-152), et la correction se consulte sur la [carte Correction manuelle de la mesure](#carte--correction-manuelle-de-la-mesure--ph-card-tune-feature-155-v2710). Les **pics rejetés**, l'état **« filtre prêt »** et la **raison de blocage dosage** ne sont plus affichés dans ce modal non plus : la raison de blocage se lit sur la [pastille d'état dosage](#pastille-détat-ph-dosing-pill--5-priorités-3-couleurs), et il n'y a plus d'état « prêt » distinct de la valeur (`isnan(ph)` suffit).

### L'écran de calibration reste sur le BRUT

⚠️ **Invariant** : `updateCalibrationReadouts()` alimente `#cal-ph-mid-readout` / `#cal-ph-low-readout` **exclusivement** depuis `json.phRaw`. Le **repli sur `json.ph`** qui existait auparavant a été **supprimé** : ce champ porte désormais la valeur corrigée, et afficher un pH corrigé face à une solution étalon reviendrait à calibrer la sonde sur une valeur déjà décalée. Conséquence assumée : si `phRaw` est indisponible (EZO muet, lecture stale), le readout affiche `--` au lieu de replier sur une autre valeur. Le lissage léger dédié calibration (médiane des ~5 dernières brutes) et l'indicateur de stabilité Δ60 s travaillent également sur le brut, comme avant.

## ~~Chip d'état filtre~~ (feature-025) — Ce qui a été retiré, fusionné, conservé (feature-152, v2.68.0)

Présent de la feature-025 à la v2.67.x, le chip cliquable `#ph-filter-chip` + le modal `#ph-filter-modal` + le bouton « Réarmer le filtre pH » (`#ph-filter-reset`) **ont disparu** avec la refonte de la chaîne de mesure ([ADR-0097 « le filtre lisse, le dosage juge »](../adr/0097-le-filtre-lisse-le-dosage-juge.md)). Zéro occurrence des trois identifiants dans `data/` aujourd'hui.

**Pourquoi** : ce chip existait pour diagnostiquer des **états bloquants** (filtre non prêt, capteur instable, alarme « filtre non prêt durablement ») qui n'existent plus — la chaîne de mesure est passée de 14 étages et 13 états bloquants à 5 étages et 4, sans aucun verrou collant. Il n'y a donc plus d'état « capteur instable »/« sonde figée bloquante »/« régulation à l'arrêt » distinct à faire clignoter sur un chip séparé : la montée en régime (ou son absence) se lit déjà sur `measures_valid`/le [badge d'état mesure](#badge-détat-mesure-ph-page-badge-feature-061-v2130) et sur la [pastille d'état dosage](#pastille-détat-ph-dosing-pill--5-priorités-3-couleurs) (code de refus `15`, « Les mesures se mettent en place »).

**Ce qui est fusionné** : les trois champs de diagnostic qui restent utiles (valeur brute, valeur retenue, âge de la dernière mesure) sont **absorbés dans le modal sonde** `#ph-probe-modal` (qui se retitre « État sonde pH », cf. [Chip d'état sonde](#chip-détat-sonde-feature-024) → [Modal détails](#modal-détails)) — plus de modal séparé.

**Ce qui n'est PAS retrouvable dans l'UI, mais vit toujours côté API** : `POST /reset-sensor-filter?sensor=ph` (vidage manuel de la fenêtre + détection de sonde figée). ⚠️ **Route doublement orpheline, conservée en l'état, délibérément** : (1) plus aucun bouton ne l'appelle, (2) sa raison d'être (filtre verrouillable) a disparu. Elle reste un usage de **diagnostic** légitime (`curl`), voir [API.md](../API.md#post-reset-sensor-filter--critical) et le commentaire au-dessus du handler dans [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp).

### Bannière « Bascule automatique en mode manuel » (v2.20.3)

`<div id="ph-auto-manual-banner" class="regul-alert regul-alert--inline" role="alert" hidden>` dans la carte Régulation pH, rendu par `renderPhAutoManualBanner(cfg)` à partir de `GET /get-config` (`ph_auto_manual_epoch` / `ph_auto_manual_reason`).

- Affichée dès que l'un des deux champs est renseigné ; masquée sinon.
- Texte (**v2.25.0**) : `Le <date locale FR>, l'appareil a basculé la régulation pH en mode manuel par sécurité. Cause : <raison>. Tant que cette alerte n'est pas acquittée, la régulation ne peut pas être réactivée : traitez d'abord la cause, puis utilisez le bouton ci-dessous. L'acquittement n'enclenche aucun dosage — il vous rend seulement le choix du mode.`
- `epoch = 0` (horloge non synchronisée au moment de la bascule) ⇒ « date inconnue (horloge non synchronisée au moment de la bascule) », la cause restant affichée.
- La trace est **persistée en NVS** côté firmware : elle survit au redémarrage et à la rotation du ring buffer du journal. Voir [pump-controller.md](../subsystems/pump-controller.md#persistance-de-la-bascule-automatique-en-manuel-v2203).

#### Bouton « Acquitter l'alerte » (v2.25.0)

`<button id="ph-auto-manual-ack" class="btn btn--primary">Acquitter l'alerte</button>`, placé **dans** la bannière (`bindPhAutoManualAck()`).

- Envoie **uniquement** `{"ph_auto_manual_ack": true}` sur `POST /save-config` — **jamais** de mode : effacer la trace ne doit pas réarmer la régulation. L'appareil **reste en manuel**, l'utilisateur choisit ensuite.
- Pendant l'appel : bouton désactivé, libellé « Acquittement… ». Il est **toujours** réhabilité en sortie (succès comme hors ligne) — jamais figé.
- Succès ⇒ toast « Alerte acquittée — vous pouvez choisir un mode de régulation. » puis `loadConfig()` (la bannière disparaît, le sélecteur se resynchronise).
- Échec ⇒ toast d'erreur, la bannière reste.

#### Parcours en cas de refus 409 sur le sélecteur de mode

⚠️ **Mécanisme partagé depuis feature-161 (v2.77.0)** — ce qui suit décrivait jusque-là une logique
propre au sélecteur `#ph_regulation_mode` de cette page (`.mode-select` depuis le 14/10, était
`.segmented` — voir [page-dashboard.md](page-dashboard.md#composant-visuel--mode-select-pas-segmented-correctif-du-1410)
pour le détail du correctif visuel) ; elle vit désormais dans une fonction **unique**
`applyRegulationMode(kind, mode)` (`data/app.js`), appelée **identiquement** par ce sélecteur **et**
par celui de la carte pH du [tableau de bord](page-dashboard.md#sélecteur-de-mode-depuis-les-cartes-ph--orp--filtration-feature-161-v2770)
— aucune seconde copie, aucun second chemin d'écriture.

Tant que la trace d'escalade existe, le firmware **refuse (409)** toute sortie du mode `manual`
(voir [API.md](../API.md#post-save-config--critical)). Un second verrou, **nouveau en feature-161
(gate `pool-chemistry` C2)**, refuse également tout changement de mode **pendant une session
d'étalonnage** (`calSwitchActive`) — voir [pump-controller.md](../subsystems/pump-controller.md#verrou-étalonnage-sur-le-changement-de-mode-bug-161-gate-c2).
Le clic sur le sélecteur (ou la carte) est donc asynchrone et gère ces refus :

1. Mémorisation du mode précédent (`previousMode`, lu depuis `window._config` — **jamais** un repli deviné si la config n'est pas encore chargée), affichage optimiste du mode cliqué.
2. `POST /save-config {"ph_regulation_mode": …}` via `sendConfigDetailed()` (qui remonte `status` et le champ `message` du corps d'erreur, en plus de `error`).
3. Sur `409` (trace d'escalade **ou** étalonnage en cours) : **revert** de `window._config` **et** du sélecteur sur le mode réel (décision pure `resolveSelectorMode(requested, lastConfirmed, outcome)`, testée par vecteurs — voir [page-dashboard.md](page-dashboard.md#un-sélecteur-qui-ment-est-pire-que-pas-de-sélecteur--le-défaut-qui-a-mordu-en-revue)), toast portant le message français du serveur. **Seul le refus d'escalade** déclenche en plus un **défilement automatique vers la bannière** (`scrollIntoView`) — l'utilisateur est renvoyé vers la cause et le bouton d'acquittement ; le refus d'étalonnage ne pointe vers aucun bouton d'acquittement (il n'y en a pas : le mode est rétabli de lui-même à la sortie de la session, voir [bug-160](#mode-réglage-calibration--correction-feature-156-v2720)).
4. Sur toute autre erreur (panne réseau) : même revert + toast générique — **sendConfigDetailed() traite refus serveur et panne réseau de façon identique**.
5. Dans tous les cas de refus, `loadConfig()` resynchronise l'UI sur l'état **réel** du contrôleur.

> Le toast de refus **ne propose pas** d'acquitter en un clic : l'acquittement doit rester un acte **lu et réfléchi** (bouton de la bannière, qui affiche la cause), pas un réflexe pour débloquer un clic refusé.

> ⚠️ L'acquittement n'est possible **que depuis cette interface web**. La commande MQTT/Home Assistant `ph_regulation_mode/set` est refusée sous trace et n'efface jamais rien (recommandation du gate `pool-chemistry` — voir [MQTT.md](../MQTT.md#refus-du-changement-de-mode-ph-sous-trace-de-sécurité-v2250)). ✅ **Le verrou d'étalonnage (C2) couvre désormais aussi le chemin MQTT** (lot C, correctif post-livraison le jour même) — la commande MQTT équivalente était d'abord restée hors de portée de ce verrou (angle mort signalé puis fermé), voir [MQTT.md](../MQTT.md#refus-du-changement-de-mode-phorp-pendant-un-étalonnage-désormais-aussi-côté-mqtt-bug-161-lot-c) et [pump-controller.md](../subsystems/pump-controller.md#lot-c--le-verrou-couvre-désormais-mqtt-et-deux-trous-de-plus-fermés-bug-161-correctif-post-livraison).

> **v2.25.0 — `collectConfig()` n'envoie plus `ph_regulation_mode` ni `ph_enabled`.** Le mode a son **propre** chemin de sauvegarde (clic sur le sélecteur) ; le réémettre dans le payload global avec un repli **en dur** sur `"automatic"` réarmait la régulation depuis un onglet périmé (incident du 2026-08-25 : bascule de sécurité annulée et trace effacée sur une simple consultation de l'app). Même patron que feature-074 pour `pumpN_max_duty_pct`. `ph_enabled` n'est en outre plus accepté en écriture par le firmware.

### Libellés révisés : Auto / Programme / Arrêt (feature-161, v2.77.0)

Décision utilisateur du 2026-10-13 : le sélecteur affiche **Auto / Programme / Arrêt**, à la place
de **Automatique / Programmée / Manuelle**. « Manuelle » laissait croire qu'on dose à la main ;
« Arrêt » dit que **la régulation automatique ne fonctionne plus**, ce qui est plus vrai — le mode
ne répond qu'à une seule question, *la régulation automatique tourne-t-elle ?*. ⛔ **La valeur
interne `manual` est INCHANGÉE** — NVS, MQTT, miroir `ph_enabled` ([`config.h:146`](../../src/config.h:146)) : seul le libellé
bouge. Même renommage, même invariant, côté [ORP](page-orp.md) et côté
[Home Assistant](../MQTT.md#libellés-français-des-selects-mode-régulation-phorp-bug-ha-regulation-mode-labels-v2173)
(republication de la découverte MQTT requise, [ADR-0085](../adr/0085-republication-decouverte-ha-sur-empreinte.md)).

### Injection manuelle déconditionnée du mode (feature-161, v2.77.0)

Jusqu'à la v2.76.x, le bloc Injection manuelle (`#ph-inject-section`) n'était **affiché qu'en mode
Manuelle** (`inject.style.display = mode === "manual" ? "" : "none"`, purement cosmétique — aucune
des neuf gardes d'`evaluateManualInject()` ne regardait le mode, et aucune route d'injection ne le
vérifiait non plus). Décision utilisateur du 2026-10-13 : « Manuelle ne devrait pas être un mode …
on pourrait laisser la possibilité d'injecter manuellement dans la page des réglages. » ⇒ **le bloc
est désormais visible dans les trois modes**, avec un texte imposé au-dessus du bouton Injecter :
« Injection manuelle — disponible dans tous les modes. Les limites de sécurité s'appliquent. »
(condition C7 du gate `pool-chemistry`).

⚠️ **Déconditionner ce bloc a rendu atteignable une garde manquante.** Rien n'empêchait jusqu'ici
d'injecter à la main **par-dessus un dosage automatique en cours** — le trou existait déjà dans
`evaluateManualInject()`, mais était **caché** par le masquage du bouton hors mode `manual` (le
mode automatique ne pouvait jamais être actif en même temps que le bouton visible). Une **nouvelle
garde serveur** (rang 4, `AutomaticInjectInProgress`) a donc été ajoutée **avant** de démasquer —
voir [pump-controller.md](../subsystems/pump-controller.md#garde-rang-4--injection-automatique-en-cours-bug-161-gate-c1).
C'est le défaut le plus intéressant du lot : une simplification d'interface a failli ouvrir un
chemin de dosage qui n'avait jamais été accessible en pratique.

Conséquence pour l'utilisateur : **injecter à la main pendant que l'automatique régule est
désormais possible et visible** (les compteurs journaliers/horaires/anti-rafale sont **partagés**
entre auto et manuel, donc les limites de sécurité tiennent dans tous les cas) — refusé uniquement
si une injection **automatique** est déjà en train de doser sur la même pompe, message : « Une
injection automatique est en cours — réessayez dans un instant. »

## Chip d'état sonde (feature-024)

Sous la valeur pH dans le bloc Statistiques, un **chip cliquable** affiche en permanence l'état diagnostique de la sonde pH (pente Atlas EZO + décalage zéro). L'évaluation est faite **côté UI** — le firmware expose les valeurs brutes (cf. [docs/subsystems/sensors.md](../subsystems/sensors.md#pente-sonde-ph--feature-024)).

### Placement

Élément `<button id="ph-probe-chip" class="chip chip--probe ...">` à l'intérieur du bloc Statistiques pH (`#ph-card-stats`), sous l'affichage pH (1 décimale). Le chip est focusable (rôle bouton, `Enter`/`Espace` ouvrent le modal).

### Classification (UI)

Source : champs WS `phSlopeAcid`, `phSlopeBase`, `phSlopeZero`, `phSlopeAgeMs`, `phCalPoints`. **Depuis la v2.70.0 (feature-154)**, les **seuils de santé** (plage des pentes, plafond du zéro) ne sont **plus** codés en dur dans `data/app.js` : ils viennent de `GET /get-config` (`ph_slope_healthy_min_pct`, `ph_slope_healthy_max_pct`, `ph_zero_healthy_max_mv` — voir [API.md](../API.md)), les **mêmes constantes** (`constants.h`) que celles consommées par `isPhProbeHealthy()` côté firmware (`src/ph_guard_logic.h`), qui décide la bascule de sécurité pH. Un seul jugement de santé existe désormais dans l'application.

> **v2.21.2 (parité de transport)** : les 4 champs `phSlope*` sont désormais **aussi dans `GET /data`** (mêmes règles null/arrondi que le WS) — un client sans WebSocket voit la pente via le poll `/data`, voir [API.md](../API.md#get-data--write).

> **feature-034 — chip unique sonde + calibration** : ce chip est désormais l'**unique** indicateur « sonde + calibration » du bloc Statistiques pH (le chip de calibration séparé `#ph-cal-chip` a été supprimé). Pour `phCalPoints < 2`, il affiche l'**état de calibration** ; pour `≥ 2`, il affiche le **diagnostic de pente** (santé sonde), résumé des **trois critères** détaillés dans le [modal](#modal-détails).

`classifyPhCriterion(value, min, max)` rend l'un de trois états — `sain` / `hors` / `indisponible`, jamais un booléen — pour chacun des trois critères (pente acide, pente base, décalage zéro). Le chip agrège les trois :

| `phCalPoints` | Agrégat des 3 critères | Variante CSS | Libellé | Couleur |
|---|---|---|---|---|
| `null` ou `< 0` | — | `chip--probe-unknown` | « Sonde pH : lecture en cours… » | gris |
| `0` | — | `chip--probe-bad` | « Calibration requise » | rouge |
| `1` | — | `chip--probe-warn` | « Calibration 1/2 » | ambré |
| `≥ 2` | pentes NaN/`null` **et** `phSlopeAgeMs` `null` (jamais lu) | `chip--probe-unknown` | « Pente en cours de lecture… » | gris |
| `≥ 2` | pentes NaN/`null` **et** `phSlopeAgeMs` non-`null` (pente invalidée) | `chip--probe-warn` | « Pente non disponible » | ambré |
| `≥ 2` | au moins un des 3 critères **hors** de la fenêtre servie | `chip--probe-bad` | « Calibré · critère hors norme » | rouge (`var(--danger)`, point pulsant) |
| `≥ 2` | aucun critère hors norme, mais au moins un **indisponible** (ex. zéro non rapporté par un module EZO ancien, ou seuils pas encore reçus) | `chip--probe-unknown` | « Calibré · critère indisponible » | gris |
| `≥ 2` | **les trois** critères mesurés et dans la fenêtre (condition C2 du gate `pool-chemistry` : un seul « indisponible » interdit le vert) | `chip--probe-good` | « Calibré · sain » | vert (`var(--success)`) |

> **v2.21.2 (bug-slope-boot-perdue) — 3 états au lieu du fourre-tout « EZO indisponible » / « Pente non disponible »** :
>
> 1. **« Sonde pH : lecture en cours… »** (`calPoints` `null`/`< 0`, classe neutre) — transitoire de boot : le cache calibration vaut `-1` les toutes premières secondes. Aucun champ de `/data` ou du WS ne distingue ce transitoire d'un vrai mutisme EZO (les deux donnent `calPoints -1` et `ph null`) → choix architect : **un seul libellé**, adouci (l'ancien « EZO indisponible » était trompeur). Un vrai mutisme finit de toute façon en log `EZO pH` au journal + valeurs pH figées à « -- ».
> 2. **« Pente en cours de lecture… »** (`phSlopeAgeMs` `null`, classe neutre, **pas** warning) — la pente n'a **jamais** été lue depuis le boot. État transitoire attendu : le firmware **retente automatiquement** la `Slope,?` de boot ([sensors.md](../subsystems/sensors.md#retentative-bornée-de-la-slope-de-boot--v2212)) → se résout en **< 2 min**.
> 3. **« Pente non disponible »** (`phSlope*` `null` **mais** `phSlopeAgeMs` non-`null`, classe `warn`) — la pente a déjà été lue **puis invalidée** après échecs répétés (`kEzoBusFailMaxConsecutive`) : **erreur réelle**, seul état d'échec des trois.
>
> Les états calibration requise / 1-2 sont inchangés. Les quatre derniers états (« Calibré · sain » et suivants) sont **nouveaux** en v2.70.0, voir ci-dessous.

> ⚠️ **v2.70.0 (feature-154) — « Sonde à remplacer » a disparu** (vocabulaire retenu par le gate `pool-chemistry`, condition C3, **bloquante**). Ce libellé a accusé une sonde **saine** le 2026-08-25 (pentes 101/100 %, zéro −1,03 mV — cause réelle : un décalage de mesure **externe**, pas une pièce défaillante) et **récidivait** le 2026-10-11 sur des pentes à 100,2/100,0 % (zéro 128,16 mV — jonction de référence, membrane intacte). Les nouveaux libellés, **choisis par l'utilisateur**, « Calibré · sain » et « Calibré · critère hors norme », n'accusent **aucune pièce** et renvoient au [modal de détail](#modal-détails) pour savoir **lequel** des trois critères est en cause.
>
> **La borne HAUTE des pentes est désormais testée.** Avant cette feature, une pente de 115 % passait « Sonde excellente » (le chip ne regardait qu'un plancher `≥ 95 %`) alors qu'elle est **hors norme pour le firmware** (`isPhProbeHealthy()` refuse `> 110 %`) : l'utilisateur aurait vu du vert sur une sonde que l'appareil juge hors norme. Un zéro `null`/NaN — ce que rapporte un module EZO qui ne sert que deux valeurs (`atlas_ezo.cpp:271`) — n'est **plus jamais compté sain par défaut** : c'était un faux vert potentiellement **permanent** sur ce type de module (condition C1 du gate).
>
> ⚠️ **Aucune de ces pastilles ni le chip qui les résume ne conditionnent un dosage** (condition C6 du gate `pool-chemistry`, bloquante) — c'est un **affichage de diagnostic pur**. La seule décision de sécurité qui consomme la même fenêtre de seuils est `isPhProbeHealthy()` côté firmware, **inchangée** par cette feature : elle ne fait que nuancer le message de la bascule automatique (3 refus de garde pH / 24 h).
>
> **Nuance à ne pas perdre** : un zéro hors fenêtre (ex. 20 mV) reste **exploitable après recalibration** (trempage KCl, vérification de l'équipotentialité) — la pastille rouge dit « **hors fenêtre de diagnostic** », pas « sonde morte ».

### État stale (âge > 36 h)

Si `phSlopeAgeMs > 36 h` (`PH_PROBE_STALE_MS`), la classe `chip--probe-stale` est ajoutée (encadré jaune) en plus de la variante de couleur. Indique que la dernière mesure de pente date — la valeur affichée est toujours valide mais l'utilisateur peut forcer un refresh.

### Modal détails

Au clic (ou `Enter`/`Espace`), un `<dialog id="ph-probe-modal">` s'ouvre. **Depuis la v2.70.0 (feature-154)**, il est réorganisé en **deux groupes nommés** au lieu de sept lignes à plat (règle d'accumulation du 3ᵉ ajout, `ux-ui-designer` mandat complet sur l'écran entier) :

**Groupe « Étalonnage »** — les trois critères qui décident `isPhProbeHealthy()` côté firmware, chacun suivi d'une **pastille** (voir [Les 4 états de pastille](#les-4-états-de-pastille-feature-154) ci-dessous) :

- **Pente acide** — `<id="ph-probe-acid">` au format `99.7 %` (1 décimale). `--` si NaN ou `phCalPoints < 2`. Pastille `#ph-probe-acid-pastille`.
- **Pente base** — `<id="ph-probe-base">` au format `100.3 %`. Pastille `#ph-probe-base-pastille`.
- **Décalage zéro** — `<id="ph-probe-zero">` au format `-0.89 mV` (2 décimales). `--` si firmware EZO ancien ne le rapporte pas. Pastille `#ph-probe-zero-pastille`.

Suivi de la légende **« Vérifié »** (`<p class="probe-modal__legend">`, `<id="ph-probe-age">`) : libellé lisible « à l'instant », « il y a 14 h », « il y a 2 j 3 h », « jamais ». ⚠️ **Ex-ligne pleine largeur, devenue légende** sous le groupe en v2.70.0 : elle faisait **doublon** avec « Dernière mesure » du second groupe, qui est un **autre âge** (celui de la mesure brute, pas de la pente).

**Groupe « Mesure en direct »** — trois lignes de diagnostic de la chaîne de mesure, absorbées ici depuis la v2.68.0 (feature-152 — plus de modal « filtre » séparé, voir [Ce qui a été retiré, fusionné, conservé](#ce-qui-a-été-retiré-fusionné-conservé-feature-152-v2680)), **sans pastille** (ce ne sont pas des critères de santé, juste des valeurs de diagnostic) :

- **Valeur brute** — `<id="ph-fm-raw">` (`phRaw`, 2 décimales, sans offset).
- **Valeur retenue** — `<id="ph-fm-filtered">` (valeur officielle, médiane 9 + lissage, corrigée — identique au champ `ph`).
- **Dernière mesure** — `<id="ph-fm-age">` (âge de la dernière brute, même formatage que « Vérifié »).

**Bouton « Rafraîchir »** (`#ph-probe-refresh`) → `POST /debug/ph_slope_refresh`. Texte bascule en « Rafraîchissement… » jusqu'à ce que `phSlopeAgeMs` redescende sous 60 s (succès → toast « Pente sonde pH rafraîchie ») ou timeout 8 s (`PH_PROBE_REFRESH_TIMEOUT_MS` → toast « Sonde non joignable »).
**Bouton « Fermer »** (`#ph-probe-close`) ou clic backdrop ou `ESC`.

#### Les 4 états de pastille (feature-154)

⚠️ **Aucune pastille ne conditionne un dosage** — c'est un affichage de diagnostic pur (condition C6 du gate `pool-chemistry`, **bloquante**). Chaque état se distingue par **forme ET couleur**, jamais la couleur seule (daltonisme rouge/vert — exactement la paire employée ici) :

| État | Rendu | Condition | Lu par un lecteur d'écran |
|---|---|---|---|
| **Sain** | cercle **plein** vert, glyphe `✓` | valeur dans `[min, max]` **inclus** — seuils servis par `GET /get-config` | « sain » |
| **Hors norme** | cercle **plein** rouge, glyphe `✕` | valeur hors de cette plage | « hors norme » |
| **Indisponible** | cercle **CREUX**, contour pointillé gris, `–` | valeur non mesurée (`null`/NaN) **ou** seuils de santé pas encore reçus du firmware — **jamais compté sain** | « indisponible » |
| **Non calibré** | **aucune pastille** | `phCalPoints < 2` — les pentes n'ont pas de sens tant que la sonde n'est pas calibrée, la valeur affiche déjà `--` | — |

Le glyphe porte `aria-hidden="true"` ; le sens est porté par une classe `.sr-only` dédiée (`app.css:3424`), jamais par la couleur seule. La pastille est **non interactive** (pas de rôle bouton, pas de contrainte de cible tactile 44 px).

**Responsive, vérifié à 390 px** : le modal est plafonné à 320 px de large, contenu utile ≈ 284 px ; le pire cas (« Décalage zéro » + « -128,16 mV » + pastille) occupe ≈ 207 px — tient avec ~77 px de marge. Le `flex-wrap` reste posé par prudence et ne doit jamais se déclencher en usage réel.

### Fallback `<dialog>` non supporté

Si `dialog.showModal` est absent (vieux navigateur), le chip est rendu non cliquable (`aria-disabled="true"`, `cursor: default`) avec un `console.warn` — la valeur reste affichée mais le détail n'est pas accessible. Aucun crash, aucun fallback DOM custom.

### Cas particuliers

- **WebSocket déconnecté** : le chip ne change pas immédiatement (dernière classification mémorisée par `latestSensorData`). À la reconnexion, l'UI re-classifie sur le payload reçu.
- **EZO pH débranché en runtime** : après 2 échecs consécutifs (`kEzoBusFailMaxConsecutive`), les champs `phSlope*` passent à `null` mais `phSlopeAgeMs` reste connu (`_phSlopeQueriedMs` non remis à zéro) → chip ambré « Pente non disponible » (erreur réelle, v2.21.2) — pas de crash, pas de toast.
- **Refresh en cours et payload WS qui apporte une nouvelle valeur** : si `phSlopeAgeMs < 60 s`, l'état refresh est résolu en succès (toast).

## État de calibration & bouton « Calibration et correction de la mesure » (feature-034 ; renommé feature-156, v2.72.0)

Depuis l'itération 2 de feature-034, l'état de calibration EZO n'est plus présenté par trois callouts noyés dans la carte Régulation (supprimés). L'itération suivante a **fusionné** le chip de calibration séparé (`#ph-cal-chip`, supprimé) dans le **chip unique sonde + calibration** `#ph-probe-chip` (cf. [Chip d'état sonde](#chip-détat-sonde-feature-024)) : pour `phCalPoints < 2`, ce chip affiche directement l'état de calibration. Il ne reste donc plus qu'**un seul chip** côté sonde/calibration pH.

Le **bouton point d'entrée** (`#ph_cal_trigger_btn`) est rendu **sous la rangée de chips** avec un **libellé fixe « Calibration et correction de la mesure »** (était « Calibrer la sonde » avant la v2.72.0 — voir [Pourquoi le point d'entrée s'appelle ainsi](#pourquoi-le-point-dentrée-sappelle--calibration-et-correction-de-la-mesure-)), accompagné d'un **hint** texte (`#ph_cal_hint`). Le clic ouvre désormais le [mode réglage](#mode-réglage-calibration--correction-feature-156-v2720) (`enterMaintenance("ph")`), au lieu d'afficher directement la carte Calibration à sa place d'avant. Le bouton/hint sont pilotés par `renderCalibrationStatus()` ([`data/app.js`](../../data/app.js)) à partir de `phCalPoints` (WebSocket / `GET /data`, voir [API.md](../API.md)) :

| `phCalPoints` | Bouton | Hint | Régulation auto |
|---------------|---------------------|------|-----------------|
| `null` (avant données) | actif | — | — |
| `-1` | **désactivé** | « EZO pH injoignable — vérifiez le câblage I²C et l'alimentation. » | Inhibée (cond #5 pool-chemistry) |
| `0` | actif | « Régulation auto inhibée tant que non calibré. » | Inhibée |
| `1` | actif | « Régulation auto inhibée (1/2 point). » | Inhibée |
| `≥ 2` | actif | — | Active |

- Le **libellé du bouton est fixe** (« Calibration et correction de la mesure ») quel que soit l'état (itération 3, renommé v2.72.0) ; le bouton est **sous la rangée de chips** (`#ph_cal_trigger_btn`), **toujours accessible** dans tous les modes — y compris quand la sonde est calibrée (recalibration possible).
- On ne désactive le bouton que sur **preuve d'EZO injoignable** (`phCalPoints < 0`). Le garde « injection en cours » reste **prioritaire** et peut le désactiver indépendamment.
- L'état de calibration ORP, lui, conserve son **chip dédié** `#orp-cal-chip` (l'ORP n'a pas de chip sonde) piloté par `setCalChip('orp', …)`.
- ⚠️ Quand `phCalPoints == 1`, le chip ne distingue pas mid seul de low seul (l'EZO ne renvoie que le compteur via `Cal,?`). Risque résiduel mineur — l'utilisateur sait normalement quel point il vient d'effectuer.
- **Garde injection prioritaire** : le bouton « Calibrer la sonde » est forcé **désactivé** pendant une injection pH en cours, indépendamment de l'état de calibration (cf. [Cas limites](#cas-limites)). Depuis la **v2.21.1**, la désactivation proactive (`_updateInjectButtons`, à chaque push WS) inclut aussi la **séance de dosage automatique** (`ph_dosing === true`), comme l'ORP le faisait déjà — harmonisation dans le sens **restrictif**. C'est un **verrou fonctionnel voulu** (pas de calibration pendant une injection), distinct de la conflation d'affichage corrigée par l'itération 3 : le verrou couvre **toute la session** d'impulsions, y compris les repos entre deux pouls.

## Workflow calibration

### Architecture UI — écran guidé (feature-034)

La carte `#ph-card-calibration` présente un **stepper** (`<ol class="calibration-steps" id="ph-cal-steps">`, réutilisant le pattern `.step` / `.calibration-steps` déjà employé pour la calibration température). Les étapes pour le pH (2 points) :

| Idx | Étape | Action UI |
|-----|-------|-----------|
| 0 | Rincer la sonde, plonger en solution **pH 7.00** | bouton « C'est fait → » (`.cal-step-advance`) |
| 1 | Attendre la stabilisation + **calibrer le point milieu** | minuterie + bouton « Calibrer le point 7.0 » (`#btn-cal-ph-mid`) → `Cal,mid,7.00` |
| 2 | Rincer, plonger en solution **pH 4.00** | bouton « C'est fait → » |
| 3 | Attendre la stabilisation + **calibrer le point bas** | minuterie + bouton « Calibrer le point 4.0 » (`#btn-cal-ph-low`) → `Cal,low,4.00` |
| 4 | Terminé | — |

**États visuels du stepper** (`setCalStepState()` dans [`data/app.js`](../../data/app.js)) : chaque `.step` reçoit une classe selon sa position relative à l'étape courante —

| État | Classe CSS | `aria-current` |
|------|-----------|----------------|
| Faite (✓) | `is-completed` | — |
| En cours | `is-active` | `step` |
| Restante | `is-upcoming` | — |

Le focus est déplacé sur le contrôle de l'étape active (`focusActiveStep()`) pour l'accessibilité clavier.

**Minuterie de stabilisation** (aide **non bloquante**) : chaque étape d'attente propose un bouton « Démarrer la minuterie de stabilisation (60 s) » (`.cal-timer-start`). Le compte à rebours est géré **côté client** par timestamp (`startCalTimer()`, constante `CAL_STAB_DURATION_S = 60`), affiche `mm:ss` + une barre de progression dans `#ph-cal-timer` (`role="timer" aria-live="polite"`), et bascule en « Stabilisation atteinte, vous pouvez calibrer » à 0. L'utilisateur **peut calibrer avant la fin** — la minuterie n'inhibe pas le bouton « Calibrer ». Annulation par « Annuler » (`.cal-timer-cancel`).

### Les six états de l'indicateur de stabilité (`renderStability()`, bug-160)

Avant la v2.75.0, l'état d'arrivée sur une étape était **muet** : `Δ60 s : --`, un libellé purement numérique, sans un mot, pendant les 10 à 45 premières secondes. La v2.75.0 (feature-159) a d'abord rendu l'attente **lisible** (cinq états, icône + texte), sans toucher au **critère** lui-même — qui restait une **étendue** (`max − min` sur 20 s, seuil 0,9 pH), chiffrée par le gate `pool-chemistry` comme annonçant « stable » à **0,8 à 1,1 pH** de l'équilibre réel de l'électrode.

`bug-160` remplace ce critère et ajoute un **sixième** état. `renderStability()` affiche désormais toujours l'un de ces six, distincts par **icône et texte** (jamais la seule couleur, daltonisme) :

| État | Icône | Texte |
|------|-------|-------|
| Lecture en cours (< 6 échantillons, ou `G < G_MIN`) | ⏳ | « Lecture de la sonde en cours — ne calibrez pas encore » |
| Mesure instable | ○ | « La mesure bouge encore — attendez (écart *X* sur *N* s) » |
| Stable | ● | « Stable — vous pouvez calibrer » |
| Calibration en cours | ↻ | « Calibration en cours — ne retirez pas la sonde du tampon » |
| Échec **persistant** | ✕ | voir [Échec persistant](#échec-persistant-cal-failure-feature-159-v2750) |
| **Diagnostic** (toujours instable après 5 min) | ⚠ | voir [Diagnostic après 5 minutes](#diagnostic-après-5-minutes) |

Priorité d'affichage, un seul message à la fois : calibration en vol (↻) **>** échec persistant (✕) **>** diagnostic (⚠) **>** lecture/stabilité (⏳ ○ ●).

⚠️ **Le bouton « Calibrer » n'est jamais désactivé pour cause d'instabilité**, dans aucun des six états, diagnostic compris — une sonde dégradée qui ne redescend jamais sous le seuil ne doit jamais devenir une impasse (voir [ADR-0105](../adr/0105-un-indicateur-de-transitoire-d-etalonnage-n-est-pas-une-porte-de-mesure.md), point 1). **Le nombre affiché est bien celui qui décide** (même valeur `D` que la comparaison au seuil, durée `S` comprise), en virgule française (`0,42`).

### Le critère — un estimateur de vitesse de dérive, pas une étendue (bug-160)

⚠️ **Le calcul a changé, pas seulement l'affichage.** L'ancien critère (`max − min` sur une fenêtre de temps fixe) est une **étendue** : elle croît avec le nombre d'échantillons et mélange bruit et dérive. Le nouveau critère coupe le tampon en deux moitiés (récente / ancienne, par comptage) et compare leurs **médianes** :

- `D` = écart entre les deux médianes de valeur — c'est lui que l'affichage compare au seuil.
- `S` / `a` = séparation et âge **réellement observés** des deux moitiés (pas la fenêtre nominale).
- `G = e^(a/τmax) · (e^(S/τmax) − 1)` — garde-fou : le verdict n'est rendu que si `n ≥ 6` **et**
  `G ≥ G_MIN`, sinon **muet** (état ⏳, jamais un faux « stable » par manque de données).

Détail complet de l'arithmétique, des sources de constantes (ADR-0098) et des résiduels garantis
(pH 0,0911 à τ = 120 s · ORP 4,65 mV à τ = 180 s) : [`docs/subsystems/sensors.md`](../subsystems/sensors.md#indicateur-de-stabilité-de-calibration-bug-160).
Distinction avec le mécanisme retiré par [ADR-0092](../adr/0092-retrait-analyse-stabilite-porte-mesures.md) : [ADR-0105](../adr/0105-un-indicateur-de-transitoire-d-etalonnage-n-est-pas-une-porte-de-mesure.md).

La réinitialisation à chaque changement de solution (et à chaque geste « Recommencer ») est **inchangée**.

### Bandeau de bascule en Manuel (`#ph-cal-switch-banner` / `#orp-cal-switch-banner`, bug-160)

⚠️ **REFONTE du 2026-10-13 (décision utilisateur)** : un premier mécanisme (bandeau à deux états,
bail/plafond/réfractaire) a été conçu puis **abandonné avant tout déploiement**. Voir
[ADR-0107](../adr/0107-etat-visible-et-persistant-plutot-que-suspension-invisible.md).

À l'entrée dans l'écran de calibration (pH ou ORP), le firmware bascule la régulation **pH ET
ORP en `manual`** — mais **seulement si elle ne l'est pas déjà** (correctif du 2026-10-13,
v2.76.2 ; détail firmware :
[pump-controller.md](../subsystems/pump-controller.md#bascule-de-régulation-en-manuel-pendant-une-calibration-bug-160-volet-2)).
⚠️ **Avant ce correctif**, la décision ne regardait que le verrou de sécurité pH — jamais le mode
courant — et côté ORP aucune condition n'existait du tout : la bascule était **toujours**
déclarée appliquée, même pour un utilisateur **déjà** en Manuel par choix. Trois affirmations
fausses en découlaient : bandeau « a été mise en Manuel » alors qu'elle l'était déjà, bouton/liens
« rétablir la régulation » alors que rien n'avait changé, et une **alerte de rappel** (2 h puis
6 h) réclamant de rétablir un mode jamais quitté. Ce cas était pourtant **spécifié** par le gate 5
(« entrée alors que la régulation est déjà en `manual` → aucune bascule ») — non vérifié avant que
l'utilisateur, dans sa propre configuration (ses deux boucles en Manuel), ne pose la question.

**Règle, par voie (pH et ORP évalués indépendamment)** : aucune bascule si **soit** la boucle est
déjà en `manual`, **soit** (pH seulement) un verrou de sécurité `phAutoManual*` l'y maintient
déjà. Si **aucune des deux voies** ne bascule, aucune session de bascule ne s'ouvre : pas de
bandeau, bouton « Fermer » simple, aucune alerte de rappel. Un bandeau unique, placé juste après
le lien de retour haut (hors des cartes), rend compte de la session **quand elle existe** — texte
**imposé mot pour mot** (gate 5), aucune icône, aucune reformulation :

> La régulation pH et chlore a été mise en Manuel : aucun dosage pendant l'étalonnage. Elle ne
> repartira pas toute seule. Utilisez le bouton ci-dessous pour la rétablir.

Couleur **ambre** (`callout--warn`), pas le bleu neutre habituel des `callout--info` : ce n'est
plus « bleu actif / ambre expiré » comme dans le mécanisme abandonné — c'est un état **unique**
qui réclame une action en fin de message (rétablir la régulation), et l'ambre est déjà la teinte
du jeu existant réservée à « il reste quelque chose à faire ».

⚠️ **Texte identique pour pH et ORP** (hérité du gate 2 sur le mécanisme précédent, toujours
valable) — annoncer « La régulation ORP a été mise en Manuel » lors d'un étalonnage ORP
mentirait par omission, puisque la régulation **pH** est elle aussi basculée. Les deux pages
affichent donc le **même** bandeau, quel que soit le produit en cours d'étalonnage.

**Condition d'affichage** : le bandeau n'est **pas** piloté par `cal_switch_active`/`/data` —
il reflète l'état **optimiste** côté client (`_calSwitchOccurred`, mis à jour par la réponse de
`POST /calibration/enter`), affiché tant que l'écran reste en mode réglage
(`is-flow-maintenance`). ⚠️ **Deux conditions, pas une** (relevé en revue) : le bandeau affirme
« la régulation a été mise en Manuel » — le gater sur le seul mode réglage le ferait **mentir**
si `/calibration/enter` avait échoué (503, mutex occupé) : il annoncerait une bascule qui n'a pas
eu lieu, alors que le bouton de fermeture afficherait encore « Fermer » (pas « Fermer et rétablir
la régulation »). Les deux champs `/data`/WS (`cal_switch_active`, `cal_switch_epoch`) existent
pour un consommateur externe (MQTT, diagnostic) — ce n'est pas la source du bandeau lui-même.

### Bouton de fermeture dynamique

Le bouton qui ferme l'écran de calibration change de libellé selon ce que l'entrée a réellement
basculé :

| Cas | Libellé |
|---|---|
| Une bascule a eu lieu (pH et/ou ORP) | **« Fermer et rétablir la régulation »** |
| Aucune bascule — pH déjà verrouillé par une escalade de plancher acide, feature-067 | **« Fermer »** |
| Aucune bascule — la voie (ou les deux) était **déjà** en `manual` par choix de l'utilisateur, hors tout verrou de sécurité (correctif 2026-10-13, v2.76.2) | **« Fermer »** |

La **même action** (`POST /calibration/exit`) est appelée par le bouton et par les deux liens de
retour (haut et bas) — ⛔ pas deux chemins différents. Idempotente : appelée deux fois, ou sans
session jamais ouverte, ne mute rien de plus. ⚠️ **Depuis le 13/10/2026**, les deux liens de retour
portent le **même libellé dynamique** que ce bouton (miroir, pas de nouveau mécanisme) — voir
[Liens de retour et accessibilité](#liens-de-retour-et-accessibilité) pour le détail des deux
états et le cas de la sortie par le menu.

### Diagnostic après 5 minutes

Si l'indicateur reste différent de « Stable » **5 minutes** après le premier échantillon de la
session de lecture courante, il bascule sur un texte de **diagnostic** (icône ⚠, `.is-diag`) :

> Toujours instable après 5 min (écart *D* sur *S* s).
> Vous pouvez calibrer quand même. Une dérive qui dure est aussi un signe d'usure de la sonde :
> vérifiez la pente dans « État sonde pH » après l'étalonnage.

(Côté ORP, la seconde ligne dit « … est aussi un signe d'usure de la sonde redox », **sans**
renvoi vers une pente : un étalonnage ORP est à un seul point, il n'y a pas de pente à vérifier —
les deux textes ne sont **pas** symétriques.)

⚠️ **Ce n'est ni un succès ni un échec, c'est un constat.** Le mécanisme a statistiquement
**raison** sur une sonde dégradée qui ne redescendra jamais sous le seuil — ce qui change, c'est la
**nature du message** (un diagnostic, pas une consigne d'attente), **jamais le critère** : le
diagnostic cède la priorité dès que « Stable » redevient vrai. **Le bouton « Calibrer » reste
cliquable** pendant le diagnostic, comme dans tous les autres états.

### Échec persistant (`_calFailure`, feature-159, v2.75.0)

Avant la v2.75.0, un échec (EZO injoignable, délai de 20 s dépassé, erreur HTTP/réseau) n'était signalé que par un **toast volatile** : une fois disparu, l'étape redevenait visuellement identique à « pas encore tentée », sans dire s'il fallait recommencer.

Depuis, l'échec est **mémorisé par étape** (`_calFailure`) et reste affiché (état ✕) tant qu'un nouvel essai ou un succès ne l'a pas remplacé. Causes distinguées dans le texte : EZO injoignable, délai dépassé, erreur HTTP, erreur réseau — toujours accompagnées de « Vérifiez le câblage, puis réessayez. ».

⚠️ **Aucun de ces textes n'affirme que la sonde est hors service, ni que la calibration précédente est intacte.** Sur un délai dépassé en particulier, l'appareil **ne sait pas** si l'EZO a exécuté la commande ou non — ce n'est pas le silence qui déciderait à sa place.

Après une calibration EZO réussie, `completeCalStep()` marque l'étape « calibrer » comme faite (✓) et avance le stepper. Le bouton (`#ph_cal_close_btn`, libellé **dynamique** depuis bug-160 — voir [Bouton de fermeture dynamique](#bouton-de-fermeture-dynamique)) appelle `requestExitMaintenance("ph")` : envoie `POST /calibration/exit` (restaure le mode antérieur de la régulation), annule toute minuterie en cours et **quitte le mode réglage** (voir [Mode réglage](#mode-réglage-calibration--correction-feature-156-v2720)) — avant la v2.72.0, il restaurait seulement la carte Régulation masquée à sa place.

### Conseil post-étalonnage (`#ph_cal_final_advice`, feature-156, v2.72.0)

À l'étape finale du stepper (idx 4, « Calibration terminée »), un encart **transitoire** (`callout--info`, `role="note"`) invite à vérifier l'étalonnage avant de corriger — il n'est **jamais persisté** et disparaît dès qu'on quitte cette étape (nouvelle calibration, sortie du mode réglage). Texte **imposé mot pour mot** par le gate `pool-chemistry`, en remplacement du mot « Recommandé » initialement proposé pour le titre de la carte Correction (refusé — voir [Carte « Correction manuelle de la mesure »](#carte--correction-manuelle-de-la-mesure--ph-card-tune-feature-155-v2710)) :

> Étalonnage terminé. Vérifiez avec un test **fiable** (gouttes ou testeur électronique) : si un écart **≥ 0,3 pH** se confirme sur **deux mesures** à des moments différents, corrigez ci-dessous. Une **bandelette (±0,5)** ne suffit pas à justifier une correction.

⚠️ **Pourquoi ce texte et pas un simple badge « Recommandé »** : un étalonnage EZO sur tampons est l'opération la plus juste de la chaîne (±0,01-0,02 pH) ; le vérifier à la bandelette (±0,5, l'amplitude **complète** de `kPhOffsetMax`) revient à arbitrer une référence avec un instrument 25× plus grossier. Une correction de bruit de +0,3 fait viser au régulateur un pH réel de 7,5-7,8 au lieu de 7,20-7,50 : à pKa 7,5, HOCl passe de 52 % à 41 %, la boucle ORP compense par surdosage puis `kPhOrpInhibit` coupe le chlore — un mot d'interface déplace la chimie du bassin. La condition **double** (moment ET résultat confirmé deux fois) est délibérée : ne pas en retirer une.

**Côté ORP et Température** : pas d'équivalent — il n'y a pas de carte de correction derrière leur calibration.

> **Readout de calibration = valeur brute.** `updateCalibrationReadouts()` alimente `#cal-ph-mid-readout` et `#cal-ph-low-readout` avec la mesure **brute** (`json.phRaw` **uniquement** — le repli sur `json.ph` a été **supprimé** en v2.20.0, ce champ portant désormais la valeur corrigée par l'offset ; cf. [L'écran de calibration reste sur le BRUT](#lécran-de-calibration-reste-sur-le-brut)), **pas** la valeur officielle affichée ailleurs dans l'UI. Raison : en changeant de solution étalon (ex. pH 7 → pH 4, saut ≈ 3.0), la valeur officielle — médiane (**9** échantillons ≈ **45 s**, était 13 ≈ 65 s avant la refonte feature-152, v2.68.0) puis lissage exponentiel (τ = 45 s) — met plusieurs dizaines de secondes à converger vers la nouvelle valeur ; la médiane absorbe seule le saut initial, sans seuil de rejet dédié (le rejet de spike isolé a disparu avec la refonte : voir [ADR-0097](../adr/0097-le-filtre-lisse-le-dosage-juge.md)). Le brut suit immédiatement le potentiel réel de l'électrode, indispensable pour juger la stabilité avant de calibrer. Le reste de l'UI (dashboard, valeur principale pH, MQTT) continue d'afficher la valeur officielle.

⚠️ **L'ordre d'exécution est IMPOSÉ par le capteur, pas libre** (corrigé en v2.75.0 — feature-159 ; l'affirmation inverse, « parallèles : ordre libre », figurait à tort dans `data/index.html` et [`docs/UPDATE_GUIDE.md`](../UPDATE_GUIDE.md), **deux endroits corrigés**). D'abord le point **milieu (7.00)**, puis le point **bas (4.00)** : `Cal,mid` sur l'EZO Atlas Scientific **efface tous les autres points enregistrés**, y compris le 4.00 déjà fait (datasheet Atlas, cf. [sensors.md](../subsystems/sensors.md#portée-chimique--deux-régimes-opposés-tous-deux-graves)). Recalibrer le milieu oblige donc à refaire les deux points — voir [Le geste « Recommencer »](#le-geste--recommencer--feature-159-v2750) ci-dessous.

### Figeage des points déjà calibrés (`_calFrozen`, feature-159, v2.75.0)

**Défaut corrigé, reproduit avant correction** : `resetCalStepper()`, appelée à chaque (ré)ouverture de l'écran de calibration, réactivait **inconditionnellement** tous les boutons « Calibrer » et remettait le stepper à l'étape 0 — **sans jamais consulter** `phCalPoints`/`orpCalPoints`, la seule vérité firmware sur ce qui est réellement acquis. Un point déjà calibré redevenait donc cliquable à la moindre fermeture/réouverture de l'écran.

Le figeage repose désormais sur une variable dédiée, `_calFrozen` (`{ ph: { mid, low }, orp: { orp } }`), peuplée **à chaque ouverture** depuis le compteur firmware :

- `phCalPoints >= 1` ⇒ `_calFrozen.ph.mid = true` ; `>= 2` ⇒ `_calFrozen.ph.low = true` également (idem `orpCalPoints >= 1` côté ORP).
- `phCalPoints === -1` (EZO **injoignable**) ⇒ **aucun bouton n'est armé** (ni « Calibrer » ni « Recommencer ») : un compteur inconnu n'autorise rien, le bouton reste désactivé jusqu'à la prochaine ouverture.
- `_calFrozen` est **distinct** de `_calActiveIdx` (la position du stepper **dans la session courante**, qui reste remise à 0 à chaque réouverture) : le premier **survit** à la fermeture de l'écran, le second non. C'est précisément ce qui manquait.
- `_calRefreshFrozenButtons()` ne fait **que désactiver** un bouton (jamais le réactiver) ; c'est `resetCalStepper()` et les gestes « Recommencer » qui portent explicitement la réactivation.
- La réouverture de l'écran **reste systématiquement à l'étape visuelle 0** (rinçage), **même si les 2 points sont déjà acquis** : elle ne saute jamais directement à l'étape « terminé », qui réveillerait sans raison le [conseil post-étalonnage transitoire](#conseil-post-étalonnage-ph_cal_final_advice-feature-156-v2720) alors qu'aucun étalonnage n'a eu lieu dans la session.

### Le geste « Recommencer » (feature-159, v2.75.0)

Avant ce lot, recommencer un point déjà calibré n'avait **aucun geste dédié** — le seul moyen était la réouverture de l'écran, c'est-à-dire **le défaut** décrit ci-dessus. Il est remplacé par un lien « **Recommencer** », visible uniquement quand `_calFrozen` indique un point **réellement** acquis (pas une simple étape dépassée dans la session) :

| Point | Bouton | Comportement |
|-------|--------|--------------|
| 7.0 (milieu) | `#btn-recal-ph-mid` (« Recommencer l'étalonnage (2 points) ») | **Confirmation obligatoire** (`confirmDialog`, `danger: true`) : « Recalibrer ce point efface aussi le point 4.00 sur la sonde : les deux seront à refaire. Il n'existe pas de retour en arrière. » → bouton « Refaire les 2 points ». Remet **les deux** points à l'état non figé et ramène le stepper à l'étape 0. |
| 4.0 (bas) | `#btn-recal-ph-low` (« Recommencer ce point ») | **Lien simple**, sans confirmation : refaire ce point seul n'efface pas le point milieu (datasheet Atlas). Ramène uniquement l'étape 4.0 (idx 2) à refaire. |
| ORP | `#btn-recal-orp` (« Recommencer ce point ») | **Lien simple**, identique au point 4.0 : un seul point sur l'EZO ORP, rien d'autre à effacer. |

⚠️ **L'asymétrie entre le 7.0 et les deux autres est un FAIT MATÉRIEL, pas un choix d'interface** : elle découle directement de l'effacement en cascade de `Cal,mid` documenté ci-dessus. Les liens 4.0/ORP portent par ailleurs la mention statique « Le nouvel étalonnage remplace l'ancien. Il n'existe pas de retour en arrière. ».

Techniquement, les trois boutons appellent la même fonction générique `_calRestartPoint(product, step, targetIdx)` (dégèle le point, vide la fenêtre de stabilité, réarme le bouton « Calibrer », replace le stepper) — seul le point milieu l'enrobe d'une confirmation et d'un dégel supplémentaire du point bas avant de l'appeler.

### Workflow temporel

0. **À l'entrée dans l'écran** (`onEnter`), le client envoie `POST /calibration/enter` (sans
   corps) — bascule la régulation pH **et** ORP en Manuel côté firmware (bug-160, voir
   [Bandeau de bascule en Manuel](#bandeau-de-bascule-en-manuel-ph-cal-switch-banner--orp-cal-switch-banner-bug-160)
   ci-dessus), et met à jour le libellé du bouton de fermeture en fonction de la réponse. À la
   **sortie** (`onExit`, succès ou non), le client envoie `POST /calibration/exit` — restaure le
   mode antérieur de chaque boucle. Aucune route de relâche : la session reste ouverte tant que
   `/calibration/exit` n'est pas appelé, y compris si l'onglet est fermé entre-temps.
1. L'utilisateur prépare la sonde (rinçage eau distillée, plongée dans le tampon, attente 1 min).
2. Clic sur **« Calibrer le point 7.0 »** → POST `/calibrate_ph {step:"mid"}`.
3. Réponse 200 immédiate `{success:true, queued:true, step:"mid"}` → toast info **« Calibration en cours… »**.
4. Le firmware exécute la commande EZO via la queue `_ezoQueue` (~900 ms transaction I²C bloquante dans `loopTask`).
5. Après calibration, `_phCalCachedPoints` est rafraîchi automatiquement → publié sur le WS → l'UI observe la transition.
6. **Polling 15 s** côté UI sur `latestSensorData.phCalPoints` :
   - Si `phCalPoints` augmente → toast succès **« Calibration mid réussie (N points) »** + mise à jour des badges.
   - Si timeout 15 s sans transition → toast warning **« Calibration : pas de retour, vérifier la connexion EZO »**.
7. Idem pour le point bas (clic sur l'autre sous-bloc).

### Cas d'erreur

| Réponse HTTP | Toast affiché |
|--------------|---------------|
| 400 `step must be 'mid' or 'low'` | « Erreur : payload invalide » (bug client) |
| 503 `calibration queue saturée — réessayer dans 1s` | « Calibration en cours, réessayer dans 1 seconde » |
| Erreur réseau | « Erreur de communication, vérifier le réseau » |

⚠️ **Depuis la v2.75.0 (feature-159), chacun de ces cas pose en plus l'[état d'échec persistant](#échec-persistant-_calfailure-feature-159-v2750)** sur l'étape concernée (icône ✕, message qui reste affiché) — le toast seul ne suffisait plus à dire, une fois évanoui, qu'il fallait recommencer.

## Données consommées (WebSocket `/ws`)

**Mesure** : `ph` (= valeur **filtrée corrigée**, 3 décimales) ; `phMedian`, `phFiltered` (**corrigés**) ; `phRaw` (**brut EZO**, feature-025 / feature-067)
**État filtre** : `phRejectedCount` (diagnostic seul), `phMixingDelayActive`, `phDoseBlockedReason` (feature-025), **`phDoseBlockedCode`** (v2.25.1 — code machine de la cause de refus, source de la catégorie) — voir [Modal détails](#modal-détails). ⚠️ `phFilterReady` / `phFilterUnstable` / `phFilterAlarm` sont **retirés en v2.68.0** (feature-152) — voir [Ce qui a été retiré, fusionné, conservé](#ce-qui-a-été-retiré-fusionné-conservé-feature-152-v2680) et [API.md](../API.md#get-data--write)
**Calibration** : `phCalPoints` (`-1..3`) — voir [Chip d'état de calibration](#chip-détat-de-calibration-feature-034)
**Bascule de calibration (bug-160)** : `cal_switch_active`, `cal_switch_epoch` — champs **communs** aux pages pH et ORP (même bascule, les deux boucles) ; ne pilotent **pas** le bandeau (état optimiste côté client, voir [Bandeau de bascule en Manuel](#bandeau-de-bascule-en-manuel-ph-cal-switch-banner--orp-cal-switch-banner-bug-160)), mais servent un consommateur externe (MQTT, diagnostic)
**Pente sonde** : `phSlopeAcid`, `phSlopeBase`, `phSlopeZero`, `phSlopeAgeMs` (feature-024) — voir [Chip d'état sonde](#chip-détat-sonde-feature-024)
**Dosage** : `ph_dosing`, `ph_used_ms`, `ph_daily_ml`, `ph_limit_reached` — ⚠️ **`ph_dosing` change de SENS en v2.36.0**, voir [Sens de « en dosage »](#sens-de--en-dosage--feature-094-v2360)
**Mode** : `ph_regulation_mode` (`automatic` / `scheduled` / `manual`), `ph_daily_target_ml`, `ph_enabled` (miroir, voir [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md))
**Répartition scheduled (feature-011, v2.8.0)** : `ph_scheduled_flow_ml_per_min` (float 1 décimale, mL/min — débit moyen planifié restant ; `null` hors mode `scheduled`, hors plage de filtration ou heure ESP32 invalide → l'UI affiche « — »)
**Terme intégral (v2.26.0)** : `ph_integral_pct` (float, % — remplissage de la réserve), `ph_integral` (float, pH·s), `ph_integral_term` (float, mL/min). Servis **à l'identique** par `GET /data`. Clé **absente** = firmware antérieur → bloc masqué ; `null` = « non calculable » → « — » ; `0` = **réserve vide, affirmé**. Voir [Rattrapage accumulé](#rattrapage-accumulé-par-la-régulation-ph-v2260)
**Dose de base apprise (feature-085, v2.28.0)** : `ph_ff_ml_per_min` (float 2 décimales, mL/min — **contribution appliquée**, `0` hors session, `0,00` aussi en régime de maintien à zéro depuis la v2.46.0), `ph_ff_status` (int 0-4, contrat figé ; le `3` est traduit en `0` dès 3 journées exploitables).

**Composition du débit (feature-092, v2.46.0)** : `ph_ff_learned_ml_per_min`, `ph_p_term_ml_per_min`, `ph_i_term_ml_per_min`, `ph_demand_ml_per_min` (float 2 décimales | `null`), plus `phMixingDelayActive` et `ph_mix_remaining_s` (pause mélange, désormais servis **aussi** par `/data`) et `ph_pump_flow_uncharacterized` (désormais servi **aussi** par le WebSocket). Tous en **parité stricte** ([ADR-0071](../adr/0071-parite-data-trame-capteurs.md)). `ph_demand_ml_per_min` absent = firmware antérieur → **carte masquée**. Voir [Carte « Débit d'injection pH »](#carte--débit-dinjection-ph--ph-card-dosing-feature-092-v2460)
**Atteinte de la cible (feature-096, v2.37.0)** : `ph_ttt_state` (uint8 `0`-`3`, **4-255 réservés**), `ph_ttt_min` (uint16, **minutes de circulation** — ⚠️ **sens variable selon l'état**), `ph_ttt_e0` (float 2 décimales \| `null`), `ph_ttt_flag` (**champ de bits**). Servis **à l'identique** par `GET /data` (même passage des deux côtés). `ph_ttt_state` non numérique = firmware antérieur → bloc masqué. La mémoire 7 jours vient de [`GET /get-kpi`](../API.md#get-get-kpi--write), **jamais du WS**. Voir [Atteinte de la cible](#atteinte-de-la-cible-feature-096-v2370)
**Limite d'actionneur (feature-086, v2.29.0)** : `ph_sustainable_ml_per_min` (float 2 décimales \| `null`), `ph_daily_max_ml` (int mL/j \| `null`), `ph_limited_by_hourly_budget` (bool), `ph_undersized` (bool). Servis **à l'identique** par `GET /data` (même passage des deux côtés — aucun clignotement possible). `ph_limited_by_hourly_budget` non booléen = firmware antérieur → capacités **et** bandeaux masqués. Voir [Capacités et bandeaux](#capacités-de-dosage-et-bandeaux-de-limite-dactionneur-feature-086-v2290)
**Config** : `ph_target`, `ph_correction_type` (`ph_minus` / `ph_plus`), `max_ph_ml_per_day`, `ph_offset` (feature-067, message `config` — pas `sensor_data`)
**Horodatage de justesse (feature-156, v2.72.0)** : `ph_offset_epoch`, `ph_cal_epoch` (uint32, secondes Unix | `0` = date inconnue — **affichage seul**, jamais utilisés pour décider un ordre), `ph_offset_stale` (bool, **verdict** calculé par le firmware — correction non nulle et antérieure au dernier étalonnage réussi). Servis **à l'identique** par `GET /get-config` et la trame WS `config` ; jamais recalculés côté client (ADR-0100, [ADR-0102](../adr/0102-ordre-des-gestes-de-justesse-par-compteur.md)). Pilote [la bannière « Correction à revoir »](#correction-devenue-caduque-après-un-étalonnage-ph_tune_stale_banner-feature-156-v2720). Voir [API.md](../API.md).
**Seuils de santé pH (feature-154, v2.70.0)** : `ph_slope_healthy_min_pct`, `ph_slope_healthy_max_pct`, `ph_zero_healthy_max_mv` — `GET /get-config` **uniquement**, jamais la trame WS (constantes de compilation, budget de trame) ; le client conserve la dernière valeur reçue entre deux appels. Voir [Classification (UI)](#classification-ui) et [API.md](../API.md).
**Injection manuelle** : `ph_inject_remaining_s`
**Stabilisation** : `stabilization_remaining_s`
**Validité mesure (feature-061, v2.13.0 ; retrait de l'analyse de stabilité feature-139, v2.62.0)** : `water_present`, `measures_valid`, `measures_stab_elapsed_s`, `measures_purge_last_duration_s`, `measures_purge_last_end`, `measures_dose_available_in_s`, `filtration_state_stale` — badge `#ph-page-badge` + valeur `--` quand la mesure n'est pas représentative (voir [Badge d'état mesure](#badge-détat-mesure-ph-page-badge-feature-061-v2130)). Servis **à l'identique** par `GET /data` (parité obligatoire). ⚠️ `measures_stab_remaining_s` **n'existe plus** depuis la v2.34.0 ; `measures_purge_count` **n'existe plus** depuis la v2.62.0 ; le bandeau `#measures_purge_timeout_banner` est **retiré** depuis la v2.62.0 (voir section dédiée plus haut).

> Champs **supprimés** depuis feature-021 : `ph_raw`, `ph_voltage_mv`, `ph_cal_valid` (remplacé par `phCalPoints`), `ph_calibration_date`, `ph_calibration_temp` (calibration mémorisée dans le module EZO, pas en NVS ESP32).

## Actions

| Action | Endpoint | Payload | Auth |
|--------|----------|---------|------|
| Sauvegarder config (mode, cible, volume) | `POST /save-config` | JSON config complète | CRITICAL |
| Changer le mode de régulation pH | `POST /save-config` | `{"ph_regulation_mode":"automatic"}` (payload **dédié**, envoi au clic) | CRITICAL |
| **Acquitter la bascule de sécurité (v2.25.0)** | `POST /save-config` | `{"ph_auto_manual_ack":true}` | CRITICAL |
| Enregistrer l'offset pH (feature-067) | `POST /save-config` | `{"ph_offset":0.15}` | CRITICAL |
| Injection manuelle start | `POST /ph/inject/start?volume=N` | querystring | WRITE |
| Injection manuelle stop | `POST /ph/inject/stop` | — | WRITE |
| Calibration EZO (mid ou low) | `POST /calibrate_ph` | `{"step":"mid"}` ou `{"step":"low"}` | WRITE |
| Effacer calibration EZO | `POST /calibrate_clear` | `{"sensor":"ph"}` | WRITE |
| Refresh pente sonde (feature-024) | `POST /debug/ph_slope_refresh` | — | aucune |

> Routes legacy supprimées (404) : `/calibrate_ph_neutral`, `/calibrate_ph_acid`, `/clear_ph_calibration`. Voir [ADR-0014](../adr/0014-migration-atlas-ezo.md).
>
> ⚠️ **`POST /reset-sensor-filter?sensor=ph` n'est plus déclenchable depuis cette page** depuis la v2.68.0 (feature-152) — le bouton qui l'appelait a disparu avec le modal « État filtre pH » (voir [Ce qui a été retiré, fusionné, conservé](#ce-qui-a-été-retiré-fusionné-conservé-feature-152-v2680)). La route reste vivante, en usage de diagnostic `curl` : [API.md](../API.md#post-reset-sensor-filter--critical).

Validation côté firmware :
- `ph_regulation_mode` rejeté si hors de l'enum → valeur ignorée. **v2.25.0** : sortie du mode `manual` refusée en **HTTP 409** tant que la bascule de sécurité n'est pas acquittée.
- `ph_auto_manual_ack` : **booléen strict** (une chaîne `"true"` / `"false"` est rejetée). Envoyé seul, il efface la trace **sans** réarmer.
- `ph_enabled` : **plus accepté en écriture** (dérivé pur du mode) ; une clé envoyée est ignorée.
- `ph_daily_target_ml` > `max_ph_ml_per_day` → HTTP 400.
- `ph_offset` hors de `[−0,50 ; +0,50]` → **HTTP 400** (refus, pas d'écrêtage) ; champ absent ou non numérique → inchangé.

## Règles firmware appliquées

Voir [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md) pour le contrat complet, notamment la section [Garde-fous `canDose`](../subsystems/pump-controller.md#garde-fous-candose). Résumé pH :

- **Mode `automatic`** : PID vers `ph_target`, actif uniquement pendant filtration (mode `pilote`), bloqué pendant stabilisation. **Inhibé tant que `phCalPoints < 2`** (calibration EZO incomplète).
- **Mode `scheduled`** : injecte jusqu'à `ph_daily_target_ml` pendant les plages de filtration, **intentionnellement aveugle à la valeur mesurée du pH** — un capteur déréglé ou en cours de remplacement n'arrête pas l'injection. Voir [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md).
  - ✅ **Répartition 24 h (v2.8.0, feature-011,** [ADR-0021](../adr/0021-repartition-scheduled.md)**)** : le volume quotidien n'est plus injecté d'un bloc mais **réparti par fenêtres de 15 min** sur la plage de filtration restante, **bornée à minuit**. Le volume de fenêtre (`restant / fenêtres restantes`, recalculé à chaque fenêtre — donc auto-corrigé après changement de cible, injection manuelle ou reboot) est borné par le **budget horaire partagé** (`ph_limit_minutes`, [ADR-0020](../adr/0020-budget-horaire-dosage-unique.md)) et **reporté** à la fenêtre suivante si l'injection correspondante durerait moins de 30 s (anti short-cycling). **Pas de rattrapage J+1** : le reliquat non injecté à minuit est perdu (log `info`). Heure ESP32 invalide → dosage suspendu. **Nuance pompe rapide** : avec un débit élevé (ex. 60 mL/min) et une petite cible, les premières fenêtres sont reportées (volume < équivalent 30 s) — les injections démarrent quand l'horizon se resserre ; c'est le fonctionnement voulu du report.
- **Mode `manual`** : aucune régulation automatique, seule l'injection manuelle pilote la pompe.
- **Gardes de sécurité pH — seuils FIXES sur le pH CORRIGÉ (feature-067, v2.20.0 ; refondues v2.25.0, [ADR-0048](../adr/0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md))** : le plancher asservi à l'offset est **supprimé**. Les seuils sont désormais **fixes** et portent sur le pH **affiché** (offset compris) ; seules deux butées de dernier recours portent sur le pH **brut**. Régulation automatique **et** programmée.

  | Produit | Refus sur le pH CORRIGÉ | Butée sur le pH BRUT |
  |---|---|---|
  | `ph_minus` (acide) | sous **6,70** | sous **6,20** |
  | `ph_plus` (base) | au-dessus de **7,60** | au-dessus de **8,10** |

  **Hystérésis de relâche 0,05 pH** sur toutes les gardes : plus de rafale de refus sur une mesure qui oscille. `critical` + alerte MQTT **`ph_guard`** (ex-`ph_raw_floor`) à la transition. **3 refus en 24 h en mode `automatic`** → bascule de `ph_regulation_mode` en `manual` (persistée) ; le mode **programmé** refuse tout autant mais **n'escalade pas** (alerte `ph_guard_scheduled` réémise au plus une fois par heure). Le message d'escalade est **conditionné au diagnostic EZO** et dit explicitement « la sonde n'est pas en cause » quand les pentes et le zéro sont nominaux. **Le pH+ est gardé pour la première fois** : aucun seuil ne l'encadrait avant la v2.25.0.
- **Avertissement d'eau acide NON BLOQUANT à 7,00 (v2.25.0)** : `warning` + alerte MQTT `ph_low_warning` quand le pH corrigé passe sous 7,00. Ne bloque rien, n'incrémente aucun compteur — c'est le premier avertisseur, conservé malgré l'abaissement du seuil de blocage à 6,70.
- **Les alertes de garde partent hors de toute conjonction d'éligibilité (v2.25.0)** : quota journalier épuisé et **mode `manual`** compris — c'est-à-dire dans l'état exact où l'escalade laisse l'appareil. Avant, la détection était enfermée dans un court-circuit `&&` et devenait muette précisément là.
- **Consigne pH bornée à [7,20 ; 7,50], pas 0,10 (v2.25.0)** : ⚠️ la borne haute passe de 7,60 à **7,50** — à 7,60 pile, la consigne atteindrait le seuil d'inhibition ORP et le chlore serait suspendu **en permanence**. Une installation à 7,60 est **clampée au premier démarrage**, avec avertissement journalisé. La saisie hors plage est devenue **impossible depuis l'interface** (contrôle segmenté, voir ci-dessous) ; la validation serveur (`400`) est conservée en défense en profondeur pour l'API et MQTT.
- **`checkPhTargetReachable()` et l'alerte `ph_target_unreachable` sont SUPPRIMÉES (v2.25.0)** : la marge entre la consigne et le seuil est devenue **structurelle**, garantie par `static_assert` à la compilation — il n'y a plus rien à détecter à l'exécution.
- **Limites** : `ph_limit_minutes` (défaut 5 min/h glissante) et `max_ph_ml_per_day` (défaut 300 mL/j).
- **Anti-rafale court terme** : ≤ 6 cycles/min ET ≤ 20 cycles/15 min (correctif Pass 3.5).
- **Stabilisation post-cal pH** : 5 min (`kStabilizationDurationPhMs`) après chaque calibration EZO réussie. Le dosage est refusé pendant cette fenêtre (cond #3 pool-chemistry).
- **Pause mélange pH** : 15 min (`kPhMixingDelayMs`) **après chaque injection** — le dosage suivant est différé le temps que le correcteur se mélange et que la sonde reflète l'effet (anti-surdosage). Depuis **v2.19.1**, affichée sur le widget du tableau de bord (« Pause mélange : … »). Chronologie complète : [pump-controller.md § Cycle de régulation](../subsystems/pump-controller.md#cycle-de-régulation--chronologie-stabilisation--pause-mélange).
- **Cumul journalier** : persisté en NVS, reset à minuit local — voir [ADR-0008](../adr/0008-persistance-cumuls-journaliers-nvs.md).
- ✅ **Injection manuelle gardée (v2.6.0, feature-006)** : `POST /ph/inject/start` passe par les gardes de `evaluateManualInject()` — dans l'ordre : watchdog, filtration (sauf mode `continu`), stabilisation post-cal **de la pompe pH**, double démarrage, **limite journalière prédictive** (frontière `==` acceptée ; depuis la **v2.9.2**, une demande supérieure au reliquat est **écrêtée automatiquement au reliquat** au lieu d'être refusée — le refus `daily_limit` ne subsiste que si le reliquat est nul ou < 1 s de pompe, avec `remaining_ml` arrondi vers le bas), **limite horaire partagée avec l'auto** (`ph_limit_minutes`, [ADR-0020](../adr/0020-budget-horaire-dosage-unique.md)), cycles/jour (20) et anti-rafale (6/min, 20/15 min — ring partagé avec l'auto). Refus = **409 JSON** `{"error","message","seconds_remaining"?,"remaining_ml"?}` — voir [Comportement UI injection manuelle](#comportement-ui-injection-manuelle-v260) et [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md#gardes-des-injections-manuelles-feature-006). Le mode de régulation et la mesure capteur ne sont volontairement **pas** vérifiés (le manuel est disponible dans tous les modes et aveugle à la mesure). Arrêt cyclique automatique conservé si la filtration s'arrête pendant l'injection. `POST /ph/inject/stop` n'est **jamais** gardé.
- **Bornage durée** : `duration` plafonné à 600 s (10 min, `kManualInjectMaxDurationS`) au lieu de 3600 s avant v2.1.2. Les gardes évaluent le volume **post-plafonnement** (ce qui sera réellement injecté).
- **Durée estimée sous le champ volume (« ≈ X min »)** : `calcInjectDurationS()` ([`data/app.js`](../../data/app.js)) est un miroir *best-effort* de `planManualInject()` — amplitude == duty calibré → débit mesuré de la pompe ; sinon débit **majorant à 255**. ⚠️ **v2.25.2** ([ADR-0050](../adr/0050-debit-pleine-puissance-constante-unique-de-flotte.md)) : ce débit majorant n'est plus une **constante recopiée dans le navigateur** (`FLOW_255_ML_PER_MIN = {1: 61, 2: 67}`, devenue fausse de ~21 %) mais la valeur **servie** par `flow_assist.flow255` de [`GET /data`](../API.md#get-data--write) — **et de la trame WS `sensor_data` depuis la v2.47.1** (`bug-082` : servi par `/data` seul, le champ était effacé par chaque push WS, donc l'estimation retombait **en permanence** sur le repli 61/67 alors que le firmware dosait à 74). Le **repli 61 / 67** est conservé **délibérément** pour les firmwares antérieurs, qui ne publient pas ce champ et dosent bien à ces débits-là — l'aligner sur 74 mentirait sur un appareil ancien. Le firmware reste l'autorité : l'estimation client n'est qu'un indicatif.
  > **Dette connue** : `MAX_INJECT_DURATION_S = 600` reste un **miroir en dur** de `kManualInjectMaxDurationS` dans `data/app.js` (4ᵉ occurrence du motif). Candidat à être servi par `/get-config`.

### Comportement UI injection manuelle (v2.6.0)

**Blocage proactif** : à chaque push WS, `_updateInjectButtons()` → `getInjectBlockReason()` ([`data/app.js`](../../data/app.js)) désactive le bouton « ▶ Injecter » (`#ph_inject_btn`) et affiche la raison dans le hint `#ph_inject_block_hint` (+ `title`) quand un blocage est **déjà connu côté client** :

| Blocage connu (ordre d'évaluation) | Hint affiché |
|---|---|
| `ph_limit_reached === true` | « Limite journalière atteinte — injection impossible jusqu'à minuit » |
| `stabilization_remaining_s > 0` | « Stabilisation post-calibration en cours — réessayer dans N s/min » |
| Filtration arrêtée en mode `pilote` | « Filtration arrêtée — injection impossible (sécurité chimique) » |

Le firmware reste l'**autorité** : miroir best-effort seulement (WS déconnecté → bouton cliquable, le 409 tranche). Le bouton « ⏹ Arrêter » n'est **jamais** désactivé (coupe-circuit opérateur).

#### Avertissement « sécurité pH brut active » avant injection manuelle (v2.25.0)

Encart `#ph_inject_raw_guard_warning` (titre `#ph_inject_raw_guard_title`, texte `#ph_inject_raw_guard_text`), rendu par `renderInjectRawGuardWarning()` **au-dessus** du bouton Injecter.

**Le chemin d'injection manuelle ignore délibérément les butées de sécurité sur le pH brut** — c'est l'échappatoire de l'opérateur, décision produit assumée : le firmware obéit. L'interface n'a donc **rien à bloquer** ; elle doit dire, **avant le clic**, que la sécurité est active, pourquoi, et que l'injection passera outre.

- **Condition d'affichage** : `phRawGuardBlocked === true` **strictement**. `false` **et** information absente (firmware antérieur à la parité `/data` ↔ WS) → **aucun encart**. Un encart affiché « par précaution » quand l'info manque serait ignoré et détruirait la valeur de signal de l'encart réel.
- **Aucun seuil n'est reconstruit côté client** (ni 6,20 ni 8,10) : l'UI consomme le booléen et la **chaîne de cause** rapportée par l'appareil, avec pour seule normalisation la virgule décimale française.
- Le texte explique que la butée porte sur le **pH brut** — la mesure du capteur, sans l'offset : c'est le seul repère fiable si l'offset est mal réglé, car le pH affiché peut alors sembler normal. Il invite à contrôler au réactif et à vérifier l'offset.
- L'encart est rendu **avant et indépendamment** de l'état du bouton : il reste visible pendant une injection en cours (la sécurité mord toujours) et n'entre jamais en concurrence avec `#ph_inject_block_hint`, qui porte les blocages **réels** (limite journalière, filtration).
- **Confirmation obligatoire** : un clic sur Injecter alors que la butée mord ouvre une `confirmDialog` annonçant explicitement le contournement. Elle n'apparaît **jamais** quand l'information est absente.

> ⚠️ Cet indicateur **gèle** sur sa dernière valeur quand la boucle de dosage sort par anticipation (filtration arrêtée, OTA, mode maintenance) : la surveillance ne tourne pas en eau stagnante. Après un arrêt de filtration, l'encart peut refléter un état antérieur. Voir [pump-controller.md](../subsystems/pump-controller.md#surveillance-inconditionnelle-séparée-de-la-décision).


**Toasts sur refus 409** (mapping `INJECT_REFUSAL_MESSAGES` par code `error`) :

| Situation | Réaction UI |
|-----------|-------------|
| 409 JSON (8 codes : `watchdog_inactive`, `filtration_off`, `stabilization_in_progress`, `already_injecting`, `daily_limit`, `hourly_limit`, `max_cycles`, `burst_limit`) | Bouton restauré + toast rouge français par code ; `daily_limit` ajoute « Reste disponible aujourd'hui : N mL » (`remaining_ml`), `stabilization_in_progress` ajoute « Réessayer dans N s/min » (`seconds_remaining`) |
| 409 non JSON (firmware ancien / proxy) | Fallback : toast filtration (seule cause 409 de l'ancien format) |
| Filtration **s'arrête en cours** d'injection | Toast rouge capté via WS log critical (`[Injection] pH INTERROMPUE`) : « Injection pH interrompue : la filtration s'est arrêtée. Relancez l'injection après reprise de la filtration. » |
| Erreur HTTP autre (4xx/5xx) | Lecture du body texte affiché si court (< 200 chars), sinon message générique. |

**Comportement attendu utilisateur** : démarrer la filtration **avant** d'injecter, ou relancer manuellement après reprise. Pas de reprise automatique — l'injection en cours est perdue.

## Cas limites

- **Filtration arrêtée / purge post-reprise (feature-061)** : valeur pH « actuelle » à `--`, badge `#ph-page-badge` bleu « Filtration arrêtée » ou « Stabilisation depuis N min », trous dans le graphique d'historique. Ligne brute et calibration non gatées.
- **EZO pH injoignable** : hint `#ph_cal_hint` « EZO pH injoignable — vérifiez le câblage I²C et l'alimentation. » sous le point d'entrée du mode réglage, et header de la carte Calibration « EZO pH injoignable ». Régulation auto inhibée (cond #5).
- **Lecture pH stale > 20 s** : valeur du bloc Statistiques en `is-stale` (opacité 0.5). Régulation auto inhibée (cond #1) + alerte MQTT `pool/alerts/sensor_stale`.
- **Calibration en cours et utilisateur clique 2× sur le même bouton** : queue à 4 slots accepte sans erreur les 4 premières mises en file. Au-delà : 503 + toast « Calibration en cours, réessayer dans 1 seconde ».
- **WebSocket déconnecté** : éléments dépendants des données capteur taggés `.is-stale` (transition 300 ms). Reset automatique à la reconnexion.
- **Reboot inattendu** : à la première réception WS après un reboot dont `reset_reason` ∈ {`WATCHDOG`, `BROWNOUT`, `PANIC`, `EXTERNAL`, `UNKNOWN`}, un toast d'alerte est affiché une seule fois par session.
- **Limite journalière atteinte** : message « Limite atteinte — dosage suspendu jusqu'à minuit », barre en rouge.
- **Pendant la calibration** : le bouton « Calibrer le point » de l'étape concernée affiche un spinner. Les autres actions UI restent disponibles.
- **Injection pH en cours (feature-034)** : le clic sur le point d'entrée (`#ph_cal_trigger_btn`) est **bloqué** si une injection pH est active — condition `(ph_inject_remaining_s > 0) || (ph_dosing === true)`, toast « Calibration impossible pendant une injection pH ». Ce garde existait déjà côté ORP ; il est désormais ajouté côté pH (symétrie de sécurité). Depuis la **v2.21.1**, la **désactivation proactive** du bouton (`_updateInjectButtons`) applique la même condition (elle n'incluait pas `ph_dosing` avant — le clic était bloqué mais le bouton restait cliquable pendant une séance auto).
- **Quitter le mode réglage par le menu, sans cliquer un lien de retour (feature-156, v2.72.0)** : défaut trouvé en test — la page revenait **amputée** (débit d'injection, régulation, historique restaient masqués). Corrigé par `_exitAllMaintenanceModes()`, appelée depuis `showView()` à chaque changement de vue. Voir [ADR-0103](../adr/0103-gestes-d-entretien-quittent-la-vue-courante.md#leçon--généraliser-un-mécanisme-en-oubliant-ce-quune-version-spécifique-faisait-en-plus).
- **Quitter par le menu pendant une calibration en vol (feature-156, v2.72.0)** : même défaut, une couche plus bas — le minuteur de calibration (`_calPollTimer`) continuait de tourner après la sortie. `_exitAllMaintenanceModes()` annule désormais aussi l'attente client (`_calEndAwait()`) dans ce cas.
- **Correction manuelle devenue caduque après un étalonnage (feature-156, v2.72.0)** : bannière persistante sur la carte Correction (voir [section dédiée](#correction-devenue-caduque-après-un-étalonnage-ph_tune_stale_banner-feature-156-v2720)), avec deux choix explicites. Survit au rechargement et au redémarrage.

## Interaction MQTT

| Entité HA | Topic | Commande |
|-----------|-------|----------|
| `sensor.*_ph` | `{base}/ph` (3 décimales depuis 2.0.0) | — |
| `sensor.*_ph_cal_points` | `{base}/ph_cal_points` (`-1..3`) | — |
| `number.*_ph_target` | `{base}/ph_target` | `{base}/ph_target/set` |
| `select.*_ph_regulation_mode` | `{base}/ph_regulation_mode` | `{base}/ph_regulation_mode/set` (feature-009, v2.7.0) |
| `binary_sensor.*_ph_dosing` | `{base}/ph_dosing` | — |
| `binary_sensor.*_ph_limit` | `{base}/ph_limit` | — |
| `sensor.*_ph_slope_acid` | `{base}/ph_slope_acid` (% — feature-024) | — |
| `sensor.*_ph_slope_base` | `{base}/ph_slope_base` (% — feature-024) | — |
| `sensor.*_ph_slope_zero` | `{base}/ph_slope_zero` (mV — feature-024) | — |
| `sensor.*_ph_integral_pct` | `{base}/ph_integral_pct` (% — v2.26.0) | — |
| `sensor.*_ph_integral_term` | `{base}/ph_integral_term` (mL/min — v2.26.0, diagnostic) | — |
| `sensor.*_ph_integral` | `{base}/ph_integral` (pH·s — v2.26.0, diagnostic) | — |
| (alerte) | `{base}/alerts/calibration_required` | — |
| (alerte) | `{base}/alerts` type `ph_integral_saturated` (v2.26.0) | — |

> **Mode de régulation commandable depuis HA (feature-009, v2.7.0)** : l'entité `select.*_ph_regulation_mode` (options `automatic` / `scheduled` / `manual`) permet de changer le mode depuis Home Assistant, par la **même voie** que le sélecteur de la carte Régulation (miroir `ph_enabled`, [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md), persistance NVS). La **carte Régulation de l'UI web reste la source principale** ; un changement initié par HA se reflète dans l'UI via les canaux existants (broadcast WS `ph_regulation_mode` + `/get-config`) — aucun code UI ajouté. Valeur hors enum → ignorée + log `warning`. Un changement pendant une injection en cours ne l'interrompt pas : le nouveau mode prend effet au cycle de régulation suivant. Voir [docs/MQTT.md](../MQTT.md#entités-ajoutées-en-feature-009-modes-de-régulation-commandables-v270).

## Fichiers

- [`data/index.html`](../../data/index.html) — structure HTML (carte Régulation + mode réglage : carte Calibration 2 sous-blocs + carte Correction, feature-156)
- [`data/app.js`](../../data/app.js) — logique mode / sauvegarde / injection manuelle / calibration EZO (POST + polling) ; `MAINTENANCE_PAGES`, `enterMaintenance()`/`exitMaintenance()` (feature-156)
- [`data/app.css`](../../data/app.css) — classe `.cal-point-block` introduite pour les sous-blocs parallèles ; `.is-flow-maintenance`/`.flow-maintenance` (feature-156)
- [`src/pump_controller.h`](../../src/pump_controller.h), [`src/pump_controller.cpp`](../../src/pump_controller.cpp) — `canDose(0)` 10 garde-fous + stabilisation par pompe
- [`src/sensors.h`](../../src/sensors.h), [`src/sensors.cpp`](../../src/sensors.cpp) — pilotage Atlas EZO pH + queue commandes ; `markPhCalibrationStamped()` appelée aux trois points d'estampillage (feature-156)
- [`src/atlas_ezo.h`](../../src/atlas_ezo.h), [`src/atlas_ezo.cpp`](../../src/atlas_ezo.cpp) — mini-classe pilote EZO
- [`src/ph_offset.h`](../../src/ph_offset.h) — logique pure `isPhOffsetStale()` / `nextJustnessSeq()` (feature-156)
- [`src/config.h`](../../src/config.h), [`src/config.cpp`](../../src/config.cpp) — champs `phOffset*`/`phCal*`, écrivains uniques `setPhOffsetStamped()` / `markPhCalibrationStamped()` (feature-156)
- [`src/web_routes_calibration.cpp`](../../src/web_routes_calibration.cpp) — endpoints `/calibrate_ph`, `/calibrate_clear`
- [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp) — endpoints `/ph/inject/*` (coquille des gardes : collecte + 409 JSON, feature-006)
- [`src/dosing_logic.h`](../../src/dosing_logic.h), [`src/dosing_logic.cpp`](../../src/dosing_logic.cpp) — `evaluateManualInject()` : gardes d'injection manuelle pures (feature-006)
- [`src/ws_manager.cpp`](../../src/ws_manager.cpp) — diffusion `ph` (= valeur officielle) + `phRaw`/`phMedian`/`phFiltered`/`phRejectedCount` (feature-025 ; `phFilterReady`/`phFilterUnstable`/`phFilterAlarm` retirés en v2.68.0, feature-152) + `ph_offset_epoch`/`ph_cal_epoch`/`ph_offset_stale` (feature-156)
- [`src/sensor_filter.h`](../../src/sensor_filter.h), [`src/sensor_filter.cpp`](../../src/sensor_filter.cpp) — chaîne de mesure à 5 étages, médiane 9 + lissage exponentiel (feature-152, v2.68.0 ; ex-feature-025)

## Documentation liée

- [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md) — règles de régulation, garde-fous `canDose`, anti-rafale, pause mélange.
- [docs/subsystems/sensors.md](../subsystems/sensors.md) — lecture EZO pH, queue, cache cal_points, chaîne de filtrage, horodatage de justesse (feature-156).
- [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md) — mode `scheduled` aveugle au capteur.
- [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md) — enum à 3 valeurs et booléen miroir `ph_enabled`.
- [ADR-0008](../adr/0008-persistance-cumuls-journaliers-nvs.md) — persistance NVS du cumul journalier.
- [ADR-0014](../adr/0014-migration-atlas-ezo.md) — migration Atlas EZO (supersedes ADR-0001).
- [ADR-0016](../adr/0016-regulation-p-temporisee-vs-pid.md) — régulation P temporisée sur mesure filtrée (feature-025).
- [ADR-0021](../adr/0021-repartition-scheduled.md) — répartition scheduled par fenêtres de 15 min (feature-011, v2.8.0).
- [ADR-0039](../adr/0039-offset-ph-applique-en-sortie-des-accesseurs.md) — offset logiciel pH appliqué en sortie des accesseurs (feature-067, v2.20.0).
- [ADR-0101](../adr/0101-la-correction-nait-d-une-mesure-jamais-d-un-ecart.md) — la correction naît d'une mesure, jamais d'un écart (feature-155, v2.71.0).
- [ADR-0102](../adr/0102-ordre-des-gestes-de-justesse-par-compteur.md) — l'ordre des gestes de justesse se décide par compteur, jamais par l'horloge (feature-156, v2.72.0).
- [ADR-0103](../adr/0103-gestes-d-entretien-quittent-la-vue-courante.md) — les gestes d'entretien quittent la vue courante (feature-156, v2.72.0).
