# Page ORP — `/orp`

- **Fichier UI** : [`data/index.html`](../../data/index.html) (section `#view-orp`)
- **URL** : `http://poolcontroller.local/#/orp`

## Rôle

Point d'entrée unique pour **tout** ce qui concerne l'ORP (potentiel redox, piloté par injection de chlore liquide) : mesure, mode de régulation, injection manuelle, historique, calibration. Structure **symétrique** de la page [pH](page-ph.md).

## Structure

⚠️ **Depuis la v2.72.0 (feature-156), la page a DEUX états** — voir [Mode réglage](#mode-réglage-calibration-seule-feature-156-v2720), même mécanisme que la page [pH](page-ph.md#mode-réglage-calibration--correction-feature-156-v2720). Ce qui suit décrit l'**état courant**.

En état courant, quatre zones :

1. **Bloc Statistiques** (bandeau compact, sans titre) — valeur ORP **officielle** courante en grand (mV) + **ligne brute discrète** « brut · maj » + **rangée de chips** : **chip d'état de calibration** (cf. [Chip d'état de calibration](#chip-détat-de-calibration-feature-034)) + dosage du jour (barre de progression, borne `max_orp_ml_per_day`). ⚠️ **Depuis la refonte de la chaîne de mesure (feature-152, v2.68.0)**, il n'existe plus de chip ni de modal d'état du **filtre** distincts — voir [Ce qui a été retiré, fusionné, conservé](#ce-qui-a-été-retiré-fusionné-conservé-feature-152-v2680). Depuis la **v2.37.0** (feature-096), ce bloc se termine par « **Atteinte de la cible** » (`#orp-ttt-block`) — voir [Atteinte de la cible](#atteinte-de-la-cible-feature-096-v2370).
2. **Carte Débit d'injection ORP** (`#orp-card-dosing`, feature-092, v2.46.0) — insérée **entre** le bloc Statistiques et la carte Régulation, visible **en mode automatique uniquement** : **deux** lignes (écart courant, **Demande** en total) et une pastille d'état. Voir [Carte « Débit d'injection ORP »](#carte--débit-dinjection-orp--orp-card-dosing-feature-092-v2460).
3. **Carte Régulation ORP** — sélecteur de mode : `Automatique`, `Programmée`, `Manuelle`. Sous-blocs :
   - **Automatique** : ORP cible (mV) + bouton Sauvegarder.
   - **Programmée** : volume quotidien de chlore en mL + bouton Sauvegarder. Depuis la v2.8.0 (feature-011) : ligne « **Débit calculé : X,X mL/min** » (`#orp_scheduled_flow_line` / `#orp_scheduled_flow_value`, alimentée par le champ WS `orp_scheduled_flow_ml_per_min` ; affiche « — » avec `title` « Hors plage de filtration ou données indisponibles » quand la valeur est `null`) + hint statique « Le volume non injecté avant minuit est perdu (pas de report au lendemain). ». Volontairement **pas** de « sur Y h restantes » côté client : l'horloge du navigateur peut différer de celle de l'ESP32, seul le débit (source firmware) est affiché.
   - **Manuelle** : bloc Injection manuelle.
4. **Carte Historique** — graphique uPlot (feature-043, ex-Chart.js), plages `Tout` / `30j` / `7j` / `24h`. Depuis la **v2.28.1** (feature-083) : **plus de zone ombrée** — **ligne de consigne pointillée** à `orp_target`, voir [Repère du graphique d'historique](#repère-du-graphique-dhistorique-feature-083-v2281). Ce graphique trace la mesure **brute** et le reste en v2.38.0 — voir [Le graphique d'historique de cette page reste brut](#le-graphique-dhistorique-de-cette-page-reste-brut-feature-095-v2380).

Le point d'entrée du mode réglage (« **Calibrer la sonde** », `#orp_cal_trigger_btn`) est rendu **sous la rangée de chips** du bloc Statistiques — voir [Mode réglage](#mode-réglage-calibration-seule-feature-156-v2720). ⚠️ **La carte Calibration n'est plus visible dans l'état courant** depuis la v2.72.0 (avant cette version, elle s'affichait au clic **au milieu** des autres cartes de la page, sans les masquer).

Le bloc Statistiques reste visible en permanence.

> **Calibration accessible dans tous les modes (feature-034)** : le bouton « Calibrer la sonde » (`#orp_cal_trigger_btn`) est désormais affiché et fonctionnel en **automatique, programmée et manuelle**. Depuis l'itération 3, ce bouton est placé **sous la rangée de chips** du bloc Statistiques (et non plus en bas de la carte de régulation dans `#orp-calibration-info`, supprimé). Le mode réglage ne modifie pas `orp_regulation_mode` : à la sortie, on **revient au mode précédemment sélectionné**.

## Mode réglage (calibration seule, feature-156, v2.72.0)

**Même mécanisme** que la page [pH](page-ph.md#mode-réglage-calibration--correction-feature-156-v2720) ([ADR-0103](../adr/0103-gestes-d-entretien-quittent-la-vue-courante.md)) : au clic sur « Calibrer la sonde », le reste de la page (débit d'injection, régulation, historique) est masqué, et on voit, dans cet ordre : un lien de retour (« ← Retour à la page ORP »), la carte **Calibration de la sonde ORP** (inchangée dans son contenu, cf. [Workflow calibration](#workflow-calibration)), un second lien de retour.

⚠️ **Pourquoi le point d'entrée reste « Calibrer la sonde », à la différence de la page pH** : l'ORP n'a **qu'une seule destination** derrière ce bouton — pas de carte de correction manuelle, le module Atlas EZO ORP n'expose ni pente ni zéro, il se calibre en 1 point. Le nom suit le **contenu** : deux destinations sur le pH (« Calibration et correction de la mesure »), une seule ici. Ce n'est pas une incohérence, c'est l'asymétrie assumée entre les deux capteurs (cf. [Hors périmètre de feature-156](../../specs/features/done/feature-156-mode-reglage-de-la-mesure-ph.md)).

Mécanisme technique, liens de retour, accessibilité, responsive, comportement à la navigation par le menu et pendant une calibration en vol : **identiques** à la page pH — voir [le détail complet](page-ph.md#mécanisme--entermaintenance--exitmaintenance) plutôt que de le dupliquer ici.

## Badge d'état mesure (`#orp-page-badge`, feature-061, v2.13.0)

Sous le bouton « Calibrer la sonde » du bloc Statistiques, un badge (`role="status"`, masqué par défaut) affiche l'**état de validité de la mesure** — symétrique du badge `#ph-page-badge` de la page pH, alimenté par les **seuls segments d'état mesure** (`measureStateSegments('orp', …)`, app.js) :

| Condition | Libellé exact | Variant |
|---|---|---|
| `water_present === false` (arrêt franc) | `Filtration arrêtée` | `info` (bleu — état normal, attendre) |
| `water_present === false` + `filtration_state_stale` | `Signal filtration périmé` | `warning` (ambré — anomalie du signal externe) |
| `water_present === true` + `measures_valid === false` | **v2.62.0** : `Reprise automatique dans N min` (`measures_dose_available_in_s`) — `Reprise automatique imminente` sous ~1 min, repli `Mesure en pause, reprise automatique à venir` si le champ manque (firmware < 2.62.0). Avant : `Stabilisation depuis N min` / `Re-stabilisation depuis N min` (v2.34.0), `Stabilisation : ~N min` (avant) | `info` (bleu) |

> **v2.34.0 à v2.61.x (feature-091, historique)** : la purge des mesures se terminait sur leur **stabilité** et non plus au bout d'un délai réglé. Le **plafond de 90 min atteint sans stabilité** n'apparaissait **pas** dans ce badge (il ouvrait la porte) : il était signalé par un bandeau dédié, **retiré en v2.62.0** (section ci-dessous). **v2.62.0 (feature-139)** : le badge affiche désormais le délai **restant** avant reprise (`measures_dose_available_in_s`), déterministe — il n'y a plus d'« écoulé » à afficher puisqu'il n'y a plus de stabilité à attendre.

Pendant ces états, la **valeur ORP « actuelle » affiche `--`** (le badge explique pourquoi) et les points ne sont **plus historisés** (trous dans le graphique). Les lectures live de la **calibration** (`#cal-orp-readout`) et de la ligne brute `#orp-filter-sub` ne sont **pas** gatées. Détails et rétrocompat : [page-dashboard.md](page-dashboard.md#mesures-phorp-non-représentatives--filtration-arrêtée--stabilisation-feature-061-v2130) ; côté firmware : [measure-validity.md](../subsystems/measure-validity.md).

## ~~Bandeau « Stabilisation des mesures non atteinte »~~ — RETIRÉ (feature-139, v2.62.0)

Présent de la v2.34.0 à la v2.61.x (`#measures_purge_timeout_banner_orp`), miroir strict du bandeau de la page pH. **Retiré pour la même raison** : le mécanisme qu'il signalait (purge « adaptative » sur stabilité des bruts) est démonté, pas assoupli ([ADR-0092](../adr/0092-retrait-analyse-stabilite-porte-mesures.md), supersède l'[ADR-0059](../adr/0059-purge-adaptative-fenetre-de-medianes.md)). Le motif qu'il affichait (`measures_purge_last_end === 2`) n'est plus jamais émis. Détail : [page-ph.md](page-ph.md#bandeau--stabilisation-des-mesures-non-atteinte--retiré-feature-139-v2620), [measure-validity.md](../subsystems/measure-validity.md). L'alerte MQTT `measures_purge_timeout` est retirée en conséquence ([MQTT.md](../MQTT.md#alertes)).

## Atteinte de la cible (feature-096, v2.37.0)

Bloc `#orp-ttt-block`, **miroir strict** de celui de la page pH : même patron HTML, même fonction de rendu (`renderTttChannel('orp', 'orp', …)`, une seule implémentation paramétrée par l'unité), même code couleur, même seuil de 2 h. Il expose l'[indicateur « temps d'atteinte de cible »](../subsystems/time-to-target.md) ([ADR-0063](../adr/0063-definition-normative-du-temps-d-atteinte-de-cible.md)) pour la régulation ORP.

Structure, formats de durée, règles de la bande de 7 pastilles, alignement par date, écrasement du dernier emplacement par l'état vivant, comportement en cas d'échec de `/get-kpi` : **détail complet dans [page-ph.md](page-ph.md#atteinte-de-la-cible-feature-096-v2370)**. Ne sont listées ici que les **différences**.

| Point | Page pH | Page ORP |
|---|---|---|
| Champs consommés | `ph_ttt_*` | `orp_ttt_*` |
| Grandeur chronométrée | **signal lent** de décision (moyenne 10 min, feature-094) | mesure **filtrée** — **qui EST la grandeur de décision** de la régulation ORP |
| Bande d'atteinte | `phStartThreshold` (**0,05 pH**) | `orpStartThreshold` (**15 mV**) |
| Format de l'écart initial | 2 décimales, sans unité (`+0,18`) | **entier + `mV`** (`−45 mV`), signe explicite, moins typographique **U+2212** |
| Garde de vraisemblance | refus de démarrer au-delà de **1,0 pH** | refus au-delà de **300 mV** |
| Drapeau **boost chlore** (bit `0x02`) | jamais posé (aucun boost pH) | posé les jours de boost — le chronomètre **juge contre la consigne effective** (celle du boost), pas contre la consigne configurée |

**Commun aux deux pages, ajouté en v2.47.0** : une journée dont le **compteur de filtration a reculé** porte `f & 0x20` (`kTttFlagCounterReset`) et se rend en **pointillé estompé sans durée**, jamais en « bonne journée à 0h00 » — même règle, même fonction de rendu. Détail : [page-ph.md](page-ph.md#bande-de-7-pastilles-ph-ttt-days-rolelist), [time-to-target.md](../subsystems/time-to-target.md#bit-5--ktttflagcounterreset--cette-valeur-nest-pas-une-mesure-bug-106-v2470).

> ⚠️ **« Minutes de circulation », jamais « de filtration »** dans tous les libellés — la base de temps est le compteur de **présence d'eau**.

**Sentinelle de présence** : `orp_ttt_state` non numérique (firmware < 2.37.0) → bloc masqué, jamais un « — ».

**Volontairement absent du tableau de bord**, comme son homologue pH.

## Carte « Débit d'injection ORP » (`#orp-card-dosing`, feature-092, v2.46.0)

Miroir **réduit** de la [carte pH](page-ph.md#carte--débit-dinjection-ph--ph-card-dosing-feature-092-v2460) : **deux lignes seulement**, parce que le chlore est régulé en **P pur** (`Ki = 0`, aucun feedforward). Ce n'est **pas un repli** — la carte est complète pour ce qu'elle régule, et elle montre au passage *pourquoi* la régulation du chlore est plus simple.

Placement : **après** le bloc Statistiques (donc après « [Atteinte de la cible](#atteinte-de-la-cible-feature-096-v2370) ») et **avant** `#orp-card-regulation`, sans intitulé de section ni séparateur. Visible **en mode automatique uniquement**. Point de décision **unique** de sa visibilité : `updateOrpDosingCard()`.

### Les 2 lignes (`.kv.kv--dose`)

| Ligne | Valeur | Note |
|---|---|---|
| **Écart courant** | `orp_p_term_ml_per_min`, **signé** (négatif = mesure au-dessus de la cible) ; « — » si `null` | Bornage du couple s'il y a lieu — côté ORP il n'y a **pas** de ligne Rattrapage, la note s'annonce donc ici |
| **Demande** (total, `border-top`) | `orp_demand_ml_per_min` en gras | Bridé → « apport moyen possible X,XX mL/min **en moyenne sur l'heure** » ; sinon → « sous le maximum de X,XX mL/min » (`orp_sustainable_ml_per_min`). Plafond `null` → **aucune note** |

> **Pas** de note « demande instantanée » ici, à la différence de la carte pH — et ce n'est pas un oubli. Le signal lent et la **porte de session** de feature-094 (`phSessionGate`, moyenne glissante de 10 min) n'existent **que** pour le pH : il n'y a **pas** d'`orpSessionGate`. Côté chlore, la décision lit la mesure **filtrée instantanée**, la même que la composition — une demande non nulle sans session relève de la **bande morte d'hystérésis**, pas d'une attente de stabilisation. Reprendre la phrase du pH y serait **faux**.

### Pastille d'état (`#orp-dosing-pill`)

Composant **identique** à celui de la page pH : mêmes 5 priorités, mêmes trois couleurs (vert = pompe commandée, gris = normal, rouge = panne), même compte à rebours sur la seule pause mélange. Champs consommés : `orp_pulse_session`, `orpMixingDelayActive`, `orp_mix_remaining_s`, `orpDoseBlockedCode` — `_doseBlockInfo(json, prefix)` est préfixé. Voir la [table des priorités](page-ph.md#pastille-détat-ph-dosing-pill--5-priorités-3-couleurs).

Une variante ORP existe pour la sentinelle `250` : côté chlore, cette garde est l'**inhibition sur pH élevé**, un état d'**attente normal** — pastille « pH trop haut, chlore peu actif » en **gris**, jamais en ambre ni en rouge (verdict `pool-chemistry`).

### Pieds de carte

| Pied | Id | Règle |
|---|---|---|
| « La régulation chlore corrige l'écart courant sans rattrapage cumulé : un écart résiduel permanent est normal et ne demande ni consigne plus haute ni gain plus fort. » | *(statique)* | **Permanent.** Texte validé `pool-chemistry`, **ne pas reformuler** : sans lui, un écart résiduel permanent se lit comme un défaut de réglage et pousse à relever la consigne ou le gain — les deux mauvaises réponses |
| « La limite horaire (X min/h) autorise Y en moyenne sur l'heure : la pompe sert la demande, puis s'arrête jusqu'à l'heure suivante. » | `#orp-dose-budget-foot` | `orp_limited_by_hourly_budget === true` + soutenable et `orp_limit_minutes` connus. **Ce cas n'est pas théorique** : la boucle chlore sature son quota horaire **tous les jours** sur cet appareil |
| « Débit de la pompe non caractérisé… » | `#orp-dose-uncharacterized-foot` | `orp_pump_flow_uncharacterized === true` |

> **`orp_limited_by_hourly_budget` est un DIAGNOSTIC, pas une alarme.** Verdict `pool-chemistry` : à CYA **35 mg/L**, une boucle chlore qui sature son quota quotidien est **cohérente avec le coût de la protection UV** (chlore libre visé 2,5-3 mg/L). Ce n'est **pas** une anomalie à corriger en montant la consigne ou le gain — or c'est exactement la tentation que produit la lecture de cette carte. Le pied permanent le dit côté interface ; le **pourquoi chimique** est ici, et dans [API.md](../API.md#champs-de-composition-du-débit-dinjection-feature-092-v2460).

### Règles de dégradation

Identiques à la carte pH, aux deux lignes près : mode ≠ `automatic` **ou** `orp_demand_ml_per_min` `undefined` ⇒ **carte entière masquée** ; `null` ⇒ « — » sur sa ligne, la pastille portant la cause ; mesure non représentative ⇒ Écart et Demande « — » ; `/data` porte **tous** les champs (parité `PARITE-DATA`) ⇒ **aucun clignotement**. Le rendu est appelé sous `try/catch`.

## Affichage mesure filtrée / brute (feature-025)

La valeur affichée **en grand** est la **valeur officielle** ORP (médiane 9 + lissage exponentiel, champ `orpFiltered` équivalent), également utilisée par la régulation. La ligne discrète `#orp-filter-sub` (`_renderFilterSub('orp', json)`) affiche :

```
brut 720 mV · maj à l'instant
```

- **brut** = `orpRaw` ; **maj** = âge de la dernière mesure brute valide. `--` si `null` (montée en régime, sonde figée ou mesure périmée — plus de repli silencieux sur une autre valeur, feature-152).

⚠️ **Depuis la refonte de la chaîne de mesure (feature-152, v2.68.0)** : la **médiane a disparu de cette ligne** (étape interne du lissage) ; elle reste consultable en diagnostic dans le modal sonde ORP (« Valeur retenue », cf. [Ce qui a été retiré, fusionné, conservé](#ce-qui-a-été-retiré-fusionné-conservé-feature-152-v2680)).

## Repère du graphique d'historique (feature-083, v2.28.1)

La **bande ombrée 600-800 mV est supprimée** (constantes `ORP_MIN`/`ORP_MAX` retirées de `data/app.js`). Motif : **l'ORP n'a pas de plage universelle** — le potentiel utile dépend du pH, du stabilisant (CYA), de la température et de l'état de l'électrode. Une bande fixe affirmait une norme qui n'existe pas, et affichait « hors plage basse » en permanence une installation dont la consigne (550 mV) avait été choisie délibérément après analyse.

Elle est remplacée par une **ligne de consigne pointillée** (bleue) à `orp_target` — même mécanisme que la page pH ([détail complet](page-ph.md#repères-du-graphique-dhistorique-feature-083-v2281)) :

- mise à jour **sans rechargement** via `loadConfig()` (`orpChart?.setTarget(cfg.orp_target)`), quel que soit le canal du changement (UI, API, MQTT — les trames WS `config` passent aussi par `loadConfig`) ; valeur **normalisée à 2 décimales** avant comparaison ;
- **repli** : consigne jamais servie (firmware antérieur, clé absente) → **aucune ligne tracée**, jamais de valeur inventée ; une valeur absente ou non finie dans une trame est ignorée — une trame partielle ne fait **jamais disparaître** la ligne ;
- les bornes d'axe Y (`calculateAxisLimits`) incluent la consigne : la ligne reste toujours visible dans le cadre.

## Le graphique d'historique de cette page reste **brut** (feature-095, v2.38.0)

La v2.38.0 historise la mesure **lissée** (celle sur laquelle la régulation décide) à côté de la mesure **brute**, et en fait la **courbe principale du mini-graphique ORP du tableau de bord** — accompagnée d'un interrupteur « Brut » mémorisé. Le graphique de la **carte Historique de cette page n'est pas concerné** : il agrège **1 point par jour**, granularité à laquelle la lissée vaut toujours `null` (elle n'est historisée que sur les points 5 min, ~8 h de profondeur) et où elle serait **indiscernable du brut**. Le repère de consigne est donc inchangé.

Détails : [page-dashboard.md](page-dashboard.md#mini-graphiques-ph--orp--la-courbe-suit-le-nombre-affiché-feature-095-v2380), [ADR-0064](../adr/0064-la-courbe-suit-le-nombre-affiche.md), [history.md](../subsystems/history.md#mesure-lissée-historisée-feature-095-v2380).

## ~~Chip d'état filtre~~ (feature-025) — Ce qui a été retiré, fusionné, conservé (feature-152, v2.68.0)

Présent de la feature-025 à la v2.67.x, le chip cliquable `#orp-filter-chip` + le bouton « Réarmer le filtre ORP » (`#orp-filter-reset`) **ont disparu** avec la refonte de la chaîne de mesure ([ADR-0097 « le filtre lisse, le dosage juge »](../adr/0097-le-filtre-lisse-le-dosage-juge.md)) — même retrait que côté pH, voir [page-ph.md](page-ph.md#ce-qui-a-été-retiré-fusionné-conservé-feature-152-v2680) pour le détail du raisonnement (14 étages → 5, 13 états bloquants → 4, zéro verrou collant).

**Ce qui est fusionné** : le modal `<dialog id="orp-filter-modal">` **survit, retitré** « État sonde ORP » (identifiant HTML inchangé, contenu réduit) et affiche désormais trois lignes de diagnostic seulement : **Valeur brute** (`#orp-fm-raw`, `orpRaw`), **Valeur retenue** (`#orp-fm-filtered`, valeur officielle médiane 9 + lissage, corrigée — identique au champ `orp`), **Dernière mesure** (`#orp-fm-age`). Plus de médiane en ligne séparée, plus de « Pics rejetés », plus de « Filtre prêt », plus de raison de blocage dosage (qui se lit sur la [pastille d'état dosage](#pastille-détat-orp-dosing-pill)) : une seule valeur officielle à lire, fin du repli silencieux (feature-152). Ouvert par le chip de calibration `#orp-cal-chip` (cf. [Chip d'état de calibration](#chip-détat-de-calibration-feature-034)), qui porte désormais seul la responsabilité « sonde + calibration » pour cette page — plus de chip séparé.

**Ce qui n'est PAS retrouvable dans l'UI, mais vit toujours côté API** : `POST /reset-sensor-filter?sensor=orp` (vidage manuel de la fenêtre + détection de sonde figée). ⚠️ **Route doublement orpheline, conservée en l'état, délibérément** : (1) plus aucun bouton ne l'appelle, (2) sa raison d'être (filtre verrouillable) a disparu — la chaîne de mesure ne compte plus aucun verrou collant. Elle reste un usage de **diagnostic** légitime (`curl`), voir [API.md](../API.md#post-reset-sensor-filter--critical) et le commentaire au-dessus du handler dans [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp). Le cooldown reste **par capteur** : réarmer le pH ne consomme pas le crédit de l'ORP.

## Chip d'état de calibration (feature-034)

Depuis l'itération 2 de feature-034, l'état de calibration EZO n'est plus présenté par trois callouts noyés dans la carte Régulation (supprimés). Il est condensé dans un **chip d'état de calibration** placé dans la **rangée de chips** du bloc Statistiques (à côté du chip filtre) + un **hint** texte. Depuis l'itération 3, le **bouton de calibration** (`#orp_cal_trigger_btn`) est rendu **sous la rangée de chips** avec un **libellé fixe « Calibrer la sonde »** (le libellé adaptatif de l'itération 2 est abandonné) ; l'état de calibration reste entièrement porté par le chip + le hint. L'EZO ORP n'accepte qu'**un seul point de calibration** côté Atlas (pas d'état « 1/2 » comme le pH).

**Élément** : `<span id="orp-cal-chip" role="status" aria-live="polite">` — **non cliquable** (annonce d'état, pas d'action). Classification côté UI par `renderCalibrationStatus()` / `setCalChip('orp', …)` ([`data/app.js`](../../data/app.js)) à partir de `orpCalPoints` (WebSocket / `GET /data`).

| `orpCalPoints` | Libellé chip | Variante CSS | Bouton « Calibrer la sonde » | Hint | Régulation auto |
|----------------|--------------|--------------|---------------------|------|-----------------|
| `null` (avant données) | « Calibration — » | `chip--probe-unknown` (gris) | actif | — | — |
| `-1` | « EZO indisponible » | `chip--probe-unknown` (gris) | **désactivé** | « EZO ORP injoignable — vérifiez le câblage I²C et l'alimentation. » | Inhibée (cond #5) |
| `0` | « Calibration requise » | `chip--probe-bad` (rouge) | actif | « Régulation auto inhibée tant que non calibré. » | Inhibée |
| `≥ 1` | « Calibré » | `chip--probe-good` (vert) | actif | — | Active |

- Le **libellé du bouton est fixe** (« Calibrer la sonde ») quel que soit l'état (itération 3) ; le bouton est **sous la rangée de chips** (`#orp_cal_trigger_btn`), **toujours accessible** dans tous les modes — y compris une fois la sonde calibrée (recalibration possible).
- Tant que les données ne sont pas reçues (`orpCalPoints == null`) et hors EZO injoignable, le chip affiche « Calibration — » et le bouton reste **actif**.
- **Garde injection prioritaire** : le bouton « Calibrer la sonde » est forcé **désactivé** pendant une injection ORP en cours, indépendamment de l'état de calibration (cf. [Cas limites](#cas-limites)).
- ⚠️ **Depuis la v2.72.0 (feature-156)**, le clic ouvre le [mode réglage](#mode-réglage-calibration-seule-feature-156-v2720) (`enterMaintenance("orp")`) au lieu d'afficher la carte Calibration à sa place d'avant, au milieu des autres cartes.

## Workflow calibration

### Architecture UI — écran guidé (feature-034)

La carte `#orp-card-calibration` présente un **stepper** (`<ol class="calibration-steps" id="orp-cal-steps">`, même pattern `.step` / `.calibration-steps` que le pH). Étapes ORP (1 point) :

| Idx | Étape | Action UI |
|-----|-------|-----------|
| 0 | Saisir la référence + rincer, plonger en **solution étalon** | champ `#orp-cal-reference` + bouton « C'est fait → » (`.cal-step-advance`) |
| 1 | Attendre la stabilisation + **calibrer** | minuterie + bouton « Calibrer » (`#btn-cal-orp`) → `Cal,<reference>` |
| 2 | Terminé | — |

- **Champ d'entrée** `orp-cal-reference` : valeur de référence en mV (range `0..1000`, défaut `470`). Standards usuels : 225 mV (kit Hanna), 470 mV (kit Atlas), 650 mV (rare).
- **États visuels du stepper** : `is-completed` (✓) / `is-active` (`aria-current="step"`) / `is-upcoming`, identiques à la page pH (voir [page-ph.md](page-ph.md#architecture-ui--écran-guidé-feature-034)).
- **Minuterie de stabilisation** (aide non bloquante, `CAL_STAB_DURATION_S = 60`) : bouton « Démarrer la minuterie (60 s) » → compte à rebours `mm:ss` + barre dans `#orp-cal-timer` (`role="timer"`), message « Stabilisation atteinte » à 0. On peut calibrer avant la fin.
- **Indicateur de stabilité Δ60 s** (indicatif) : amplitude `max − min` de la mesure brute sur ~60 s, seuil cosmétique `5` mV.
- **Readout live** : `<div class="readout">` (`#cal-orp-readout`) affichant la valeur ORP **brute** lue toutes les 5 s. `updateCalibrationReadouts()` utilise `json.orpRaw` (repli sur `json.orp` si absent), **pas** la valeur officielle. Raison : en changeant de solution étalon (saut de plusieurs centaines de mV), la valeur officielle — médiane (**9** échantillons ≈ **45 s**, était 13 ≈ 65 s avant la refonte feature-152, v2.68.0) puis lissage exponentiel (τ = 45 s) — met plusieurs dizaines de secondes à converger ; la médiane absorbe seule le saut initial, sans seuil de rejet dédié (le rejet de spike isolé a disparu avec la refonte : voir [ADR-0097](../adr/0097-le-filtre-lisse-le-dosage-juge.md)). Le brut suit immédiatement le potentiel réel de l'électrode, indispensable pour juger la stabilité avant de calibrer. Le reste de l'UI (valeur principale ORP, MQTT) continue d'afficher la valeur officielle.

### Workflow temporel

1. L'utilisateur saisit la valeur de référence de sa solution standard dans le champ.
2. Il prépare la sonde (rinçage eau distillée, plongée dans le tampon, attente 1 min).
3. Clic sur **« Calibrer »** → POST `/calibrate_orp {reference: <mV>}`.
4. Réponse 200 immédiate `{success:true, queued:true, reference:470}` → toast info **« Calibration en cours… »**.
5. Le firmware exécute `Cal,<reference>` via la queue (~900 ms transaction I²C).
6. **Polling 15 s** sur `latestSensorData.orpCalPoints` :
   - Si `orpCalPoints` augmente (passe de `0` à `1`) → toast succès **« Calibration ORP réussie »**.
   - Si `orpCalPoints` était déjà à `1` (recalibration) → fallback succès silencieux après 5 s sans transition de compteur (l'EZO ne change pas le compteur lors d'une recalibration sur un point existant).
   - Si timeout 15 s → toast warning **« Calibration : pas de retour »**.

### Cas d'erreur

| Réponse HTTP | Toast affiché |
|--------------|---------------|
| 400 `reference must be 0..1000 mV` | « Valeur de référence hors plage (0–1000 mV) » |
| 503 `calibration queue saturée — réessayer dans 1s` | « Calibration en cours, réessayer dans 1 seconde » |

## Données consommées (WebSocket `/ws`)

**Mesure** : `orp` (= valeur **filtrée**, entier mV) ; `orpRaw`, `orpMedian`, `orpFiltered` (feature-025)
**État filtre** : `orpRejectedCount` (diagnostic seul), `orpMixingDelayActive`, `orpDoseBlockedReason` (feature-025), **`orpDoseBlockedCode`** (v2.25.1 — code machine de la cause de refus, source de la catégorie) — voir le modal sonde ORP, [Ce qui a été retiré, fusionné, conservé](#ce-qui-a-été-retiré-fusionné-conservé-feature-152-v2680). ⚠️ `orpFilterReady` / `orpFilterUnstable` / `orpFilterAlarm` sont **retirés en v2.68.0** (feature-152) — voir [API.md](../API.md#get-data--write)
**Calibration** : `orpCalPoints` (`-1..1`) — voir [Chip d'état de calibration](#chip-détat-de-calibration-feature-034)
**Dosage** : `orp_dosing`, `orp_used_ms`, `orp_daily_ml`, `orp_limit_reached`
**Composition du débit (feature-092, v2.46.0)** : `orp_p_term_ml_per_min`, `orp_demand_ml_per_min`, `orp_sustainable_ml_per_min` (float 2 décimales \| `null`), `orp_limited_by_hourly_budget` (bool, **verdict** firmware — jamais `null`), plus `orpMixingDelayActive` / `orp_mix_remaining_s` (pause mélange, désormais servis **aussi** par `/data`) et `orp_pump_flow_uncharacterized` (désormais servi **aussi** par le WebSocket). Tous en **parité stricte** ([ADR-0071](../adr/0071-parite-data-trame-capteurs.md)). `orp_demand_ml_per_min` absent = firmware antérieur → **carte masquée**. Voir [Carte « Débit d'injection ORP »](#carte--débit-dinjection-orp--orp-card-dosing-feature-092-v2460)
**Mode** : `orp_regulation_mode`, `orp_daily_target_ml`, `orp_enabled` (miroir — voir [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md))
**Répartition scheduled (feature-011, v2.8.0)** : `orp_scheduled_flow_ml_per_min` (float 1 décimale, mL/min — débit moyen planifié restant ; `null` hors mode `scheduled`, hors plage de filtration ou heure ESP32 invalide → l'UI affiche « — »)
**Atteinte de la cible (feature-096, v2.37.0)** : `orp_ttt_state` (uint8 `0`-`3`, **4-255 réservés**), `orp_ttt_min` (uint16, **minutes de circulation** — ⚠️ **sens variable selon l'état**), `orp_ttt_e0` (float 1 décimale, mV \| `null`), `orp_ttt_flag` (**champ de bits**). Servis **à l'identique** par `GET /data`. `orp_ttt_state` non numérique = firmware antérieur → bloc masqué. La mémoire 7 jours vient de [`GET /get-kpi`](../API.md#get-get-kpi--write), **jamais du WS**. Voir [Atteinte de la cible](#atteinte-de-la-cible-feature-096-v2370)
**Config** : `orp_target`, `max_orp_ml_per_day` (= `max_chlorine_ml_per_day`)
**Injection manuelle** : `orp_inject_remaining_s`
**Stabilisation** : `stabilization_remaining_s`
**Validité mesure (feature-061, v2.13.0 ; retrait de l'analyse de stabilité feature-139, v2.62.0)** : `water_present`, `measures_valid`, `measures_stab_elapsed_s`, `measures_purge_last_duration_s`, `measures_purge_last_end`, `measures_dose_available_in_s`, `filtration_state_stale` — badge `#orp-page-badge` + valeur `--` quand la mesure n'est pas représentative (voir [Badge d'état mesure](#badge-détat-mesure-orp-page-badge-feature-061-v2130)). Servis **à l'identique** par `GET /data`. ⚠️ `measures_stab_remaining_s` **n'existe plus** depuis la v2.34.0 ; `measures_purge_count` **n'existe plus** depuis la v2.62.0 ; le bandeau `#measures_purge_timeout_banner_orp` est **retiré** depuis la v2.62.0.

> Champs **supprimés** depuis feature-021 : `orp_raw`, `orp_voltage_mv`, `orp_calibration_offset`, `orp_calibration_slope`, `orp_calibration_reference`, `orp_calibration_temp`, `orp_calibration_date` (calibration mémorisée dans le module EZO, pas en NVS ESP32). Le champ `orp_cal_valid` est conservé en miroir pour rétrocompat HA (`true` ssi `orpCalPoints >= 1`).

## Actions

| Action | Endpoint | Payload | Auth |
|--------|----------|---------|------|
| Sauvegarder config (mode, cible, volume) | `POST /save-config` | JSON config complète | CRITICAL |
| Injection manuelle start | `POST /orp/inject/start?volume=N` | querystring | WRITE |
| Injection manuelle stop | `POST /orp/inject/stop` | — | WRITE |
| Calibration EZO ORP | `POST /calibrate_orp` | `{"reference": <mV float>}` | WRITE |
| Effacer calibration EZO | `POST /calibrate_clear` | `{"sensor":"orp"}` | WRITE |

> Avant feature-021 : la calibration ORP se faisait entièrement côté UI client (calcul offset/slope, persistance via `POST /save-config` avec `orp_calibration_*`) — voir [ADR-0003](../adr/0003-calibration-orp-cote-client.md). Cette voie est **supersédée** par la calibration interne au module Atlas EZO ORP. ADR-0003 reste applicable historiquement (PCB v1).
>
> ⚠️ **`POST /reset-sensor-filter?sensor=orp` n'est plus déclenchable depuis cette page** depuis la v2.68.0 (feature-152) — le bouton qui l'appelait a disparu (voir [Ce qui a été retiré, fusionné, conservé](#ce-qui-a-été-retiré-fusionné-conservé-feature-152-v2680)). La route reste vivante, en usage de diagnostic `curl` : [API.md](../API.md#post-reset-sensor-filter--critical).

Validation côté firmware :
- `orp_regulation_mode` rejeté si hors de l'enum.
- `orp_daily_target_ml` > `max_orp_ml_per_day` → HTTP 400.
- `reference` hors plage `0..1000` mV → HTTP 400.

## Règles firmware appliquées

Voir [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md), notamment [Garde-fous `canDose`](../subsystems/pump-controller.md#garde-fous-candose). Résumé ORP :

- **Mode `automatic`** : PID vers `orp_target`, actif uniquement pendant filtration (mode `pilote`), bloqué pendant stabilisation. **Inhibé tant que `orpCalPoints < 1`** (calibration EZO incomplète).
- **Mode `scheduled`** : injecte jusqu'à `orp_daily_target_ml` pendant filtration, **intentionnellement aveugle à la valeur ORP mesurée**. Voir [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md).
  - ✅ **Répartition 24 h (v2.8.0, feature-011,** [ADR-0021](../adr/0021-repartition-scheduled.md)**)** : le volume quotidien de chlore n'est plus injecté d'un bloc mais **réparti par fenêtres de 15 min** sur la plage de filtration restante, **bornée à minuit**. Le volume de fenêtre (`restant / fenêtres restantes`, recalculé à chaque fenêtre — donc auto-corrigé après changement de cible, injection manuelle ou reboot) est borné par le **budget horaire partagé** (`orp_limit_minutes`, [ADR-0020](../adr/0020-budget-horaire-dosage-unique.md)) et **reporté** à la fenêtre suivante si l'injection correspondante durerait moins de 30 s (anti short-cycling). **Pas de rattrapage J+1** : le reliquat non injecté à minuit est perdu (log `info`). Heure ESP32 invalide → dosage suspendu. **Nuance pompe rapide** : avec un débit élevé (ex. 60 mL/min) et une petite cible, les premières fenêtres sont reportées (volume < équivalent 30 s) — les injections démarrent quand l'horizon se resserre ; c'est le fonctionnement voulu du report.
- **Mode `manual`** : seule l'injection manuelle est autorisée.
- **Limites** : `orp_limit_minutes` (défaut 10 min/h glissante) et `max_chlorine_ml_per_day` (défaut 500 mL/j).
- **Anti-rafale court terme** : ≤ 6 cycles/min ET ≤ 20 cycles/15 min (correctif Pass 3.5).
- **Stabilisation post-cal ORP** : 3 min (`kStabilizationDurationOrpMs`) après chaque calibration EZO réussie.
- **Pause mélange ORP** : 20 min (`kOrpMixingDelayMs`) **après chaque injection** — le dosage suivant est différé le temps que le chlore s'homogénéise et que la sonde reflète l'effet (anti-surdosage) ; la régulation ORP est donc plafonnée à ~1 injection / 20 min. Depuis **v2.19.1**, ce délai est affiché sur le widget du tableau de bord (« Pause mélange : … »), plus seulement dans le modal « État filtre ». Chronologie complète : [pump-controller.md § Cycle de régulation](../subsystems/pump-controller.md#cycle-de-régulation--chronologie-stabilisation--pause-mélange).
- ✅ **Injection manuelle gardée (v2.6.0, feature-006)** : `POST /orp/inject/start` passe par les gardes de `evaluateManualInject()` — dans l'ordre : watchdog, filtration (sauf mode `continu`), stabilisation post-cal **de la pompe ORP**, double démarrage, **limite journalière prédictive** (frontière `==` acceptée ; depuis la **v2.9.2**, une demande supérieure au reliquat est **écrêtée automatiquement au reliquat** au lieu d'être refusée — le refus `daily_limit` ne subsiste que si le reliquat est nul ou < 1 s de pompe, avec `remaining_ml` arrondi vers le bas), **limite horaire partagée avec l'auto** (`orp_limit_minutes`, [ADR-0020](../adr/0020-budget-horaire-dosage-unique.md)), cycles/jour (20) et anti-rafale (6/min, 20/15 min — ring partagé avec l'auto). Refus = **409 JSON** `{"error","message","seconds_remaining"?,"remaining_ml"?}` — voir [Comportement UI injection manuelle](#comportement-ui-injection-manuelle-v260) et [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md#gardes-des-injections-manuelles-feature-006). Le mode de régulation et la mesure capteur ne sont volontairement **pas** vérifiés. Arrêt cyclique automatique conservé si la filtration tombe pendant l'injection (alerte MQTT `orp_injection_aborted` sur `{base}/alerts`). `POST /orp/inject/stop` n'est **jamais** gardé.
- **Bornage durée** : `duration` plafonné à 600 s (`kManualInjectMaxDurationS`) au lieu de 3600 s avant v2.1.2. Les gardes évaluent le volume **post-plafonnement**.
- **Durée estimée sous le champ volume (« ≈ X min »)** : même mécanisme que la page pH — depuis la **v2.25.2**, le débit majorant à 255 est **servi** par `flow_assist.flow255` (`GET /data`, **et la trame WS `sensor_data` depuis la v2.47.1** — `bug-082`) au lieu d'être recopié en dur dans le navigateur, repli 61 / 67 conservé pour les firmwares antérieurs. Voir [page-ph.md](page-ph.md#règles-firmware-appliquées) et [ADR-0050](../adr/0050-debit-pleine-puissance-constante-unique-de-flotte.md).

### Comportement UI injection manuelle (v2.6.0)

**Blocage proactif** : identique à la page pH (`_updateInjectButtons()` → `getInjectBlockReason()`) — bouton « ▶ Injecter » (`#orp_inject_btn`) désactivé + raison dans le hint `#orp_inject_block_hint` quand le blocage est connu côté client : `orp_limit_reached`, `stabilization_remaining_s > 0`, ou filtration arrêtée en mode `pilote`. Le firmware reste l'autorité (WS déconnecté → bouton cliquable, le 409 tranche). Le bouton « ⏹ Arrêter » n'est **jamais** désactivé.

#### Avertissement « sécurité pH brut active » avant injection manuelle (v2.25.0)

Encart `#orp_inject_raw_guard_warning` (titre `#orp_inject_raw_guard_title`, texte `#orp_inject_raw_guard_text`), rendu par `renderInjectRawGuardWarning()` **au-dessus** du bouton Injecter.

**Le chemin d'injection manuelle ignore délibérément les butées de sécurité sur le pH brut** — c'est l'échappatoire de l'opérateur, décision produit assumée : le firmware obéit. L'interface n'a donc **rien à bloquer** ; elle doit dire, **avant le clic**, que la sécurité est active, pourquoi, et que l'injection passera outre.

- **Condition d'affichage** : `orpPhRawGuardBlocked === true` **strictement**. `false` **et** information absente (firmware antérieur à la parité `/data` ↔ WS) → **aucun encart**. Un encart affiché « par précaution » quand l'info manque serait ignoré et détruirait la valeur de signal de l'encart réel.
- **Aucun seuil n'est reconstruit côté client** (ni 6,20 ni 8,10) : l'UI consomme le booléen et la **chaîne de cause** rapportée par l'appareil, avec pour seule normalisation la virgule décimale française.
- Le texte explique que la butée porte sur le **pH brut** — la mesure du capteur, sans l'offset : c'est le seul repère fiable si l'offset est mal réglé, car le pH affiché peut alors sembler normal. Il invite à contrôler au réactif et à vérifier l'offset.
- L'encart est rendu **avant et indépendamment** de l'état du bouton : il reste visible pendant une injection en cours (la sécurité mord toujours) et n'entre jamais en concurrence avec `#orp_inject_block_hint`, qui porte les blocages **réels** (limite journalière, filtration).
- **Confirmation obligatoire** : un clic sur Injecter alors que la butée mord ouvre une `confirmDialog` annonçant explicitement le contournement. Elle n'apparaît **jamais** quand l'information est absente.

> ⚠️ Cet indicateur **gèle** sur sa dernière valeur quand la boucle de dosage sort par anticipation (filtration arrêtée, OTA, mode maintenance) : la surveillance ne tourne pas en eau stagnante. Après un arrêt de filtration, l'encart peut refléter un état antérieur. Voir [pump-controller.md](../subsystems/pump-controller.md#surveillance-inconditionnelle-séparée-de-la-décision).


**Toasts sur refus 409** :

| Situation | Réaction UI |
|-----------|-------------|
| 409 JSON (8 codes : `watchdog_inactive`, `filtration_off`, `stabilization_in_progress`, `already_injecting`, `daily_limit`, `hourly_limit`, `max_cycles`, `burst_limit`) | Bouton restauré + toast rouge français par code ; `daily_limit` ajoute le reliquat (`remaining_ml`), `stabilization_in_progress` le compte à rebours (`seconds_remaining`) |
| 409 non JSON (firmware ancien / proxy) | Fallback : toast filtration (seule cause 409 de l'ancien format) |
| Filtration **s'arrête en cours** d'injection | Toast rouge capté via WS log critical (`[Injection] ORP INTERROMPUE`) : « Injection ORP/chlore interrompue : la filtration s'est arrêtée. Relancez l'injection après reprise de la filtration. » |
| Erreur HTTP autre (4xx/5xx) | Lecture du body texte affiché si court (< 200 chars), sinon message générique. |

**Comportement attendu utilisateur** : démarrer la filtration **avant** d'injecter, ou relancer manuellement après reprise. Pas de reprise automatique — l'injection en cours est perdue.

## Cas limites

- **Filtration arrêtée / purge post-reprise (feature-061)** : valeur ORP « actuelle » à `--`, badge `#orp-page-badge` bleu « Filtration arrêtée » ou « Stabilisation depuis N min », trous dans le graphique d'historique. Ligne brute et calibration non gatées.
- **EZO ORP injoignable** : chip `#orp-cal-chip` gris « EZO indisponible » + hint « EZO ORP injoignable — vérifiez le câblage I²C. », header de la carte Calibration « EZO ORP injoignable » (voir [Chip d'état de calibration](#chip-détat-de-calibration-feature-034)).
- **Lecture ORP stale > 20 s** : valeur en `is-stale` + alerte MQTT `pool/alerts/sensor_stale`. Régulation auto inhibée.
- **Recalibration sur un capteur déjà à `orpCalPoints == 1`** : fallback succès UI après 5 s (le compteur EZO ne change pas, le polling de transition échoue → on bascule sur un succès optimiste).
- **WebSocket déconnecté** : éléments capteur taggés `.is-stale`. Reset à la reconnexion.
- **Reboot inattendu** : toast à la première réception WS si `reset_reason` ∈ {`WATCHDOG`, `BROWNOUT`, `PANIC`, `EXTERNAL`, `UNKNOWN`}.
- **Mode Programmée sans sonde** : fonctionne — c'est justement un cas d'usage (utilisateur sans sonde ORP). Aucune dépendance sur `orpCalPoints` dans cette branche.
- **Injection ORP en cours (feature-034)** : le clic sur « Calibrer » (`#orp_cal_trigger_btn`) est **bloqué** si une injection ORP est active — condition `(orp_inject_remaining_s > 0) || (orp_dosing === true)`, toast « Calibration impossible pendant une injection ORP ».
- **Quitter le mode réglage par le menu, ou pendant une calibration en vol (feature-156, v2.72.0)** : identique à la page pH, mécanisme partagé — voir [le détail](page-ph.md#cas-limites) et [ADR-0103](../adr/0103-gestes-d-entretien-quittent-la-vue-courante.md).

## Interaction MQTT

| Entité HA | Topic | Commande |
|-----------|-------|----------|
| `sensor.*_orp` | `{base}/orp` (= valeur officielle) | — |
| `sensor.*_orp_raw` | `{base}/orp_raw` (feature-025) | — |
| `sensor.*_orp_filtered` | `{base}/orp_filtered` (feature-025, diagnostic seul) | — |
| ~~`binary_sensor.*_orp_filter_ready`~~ | ~~`{base}/orp_filter_ready`~~ | **RETIRÉ en v2.68.0** (feature-152) — `{base}/orp` vaut directement `null` pendant la montée en régime. Voir [MQTT.md](../MQTT.md#auto-discovery-ha-feature-025). |
| `sensor.*_orp_cal_points` | `{base}/orp_cal_points` (`-1..1`) | — |
| `number.*_orp_target` | `{base}/orp_target` | `{base}/orp_target/set` |
| `select.*_orp_regulation_mode` | `{base}/orp_regulation_mode` | `{base}/orp_regulation_mode/set` (feature-009, v2.7.0) |
| `binary_sensor.*_orp_dosing` | `{base}/orp_dosing` | — |
| `binary_sensor.*_orp_limit` | `{base}/orp_limit` | — |
| (alerte) | `{base}/alerts/calibration_required` | — |

> **Mode de régulation commandable depuis HA (feature-009, v2.7.0)** : l'entité `select.*_orp_regulation_mode` (options `automatic` / `scheduled` / `manual`) permet de changer le mode depuis Home Assistant, par la **même voie** que le sélecteur de la carte Régulation (miroir `orp_enabled`, [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md), persistance NVS). La **carte Régulation de l'UI web reste la source principale** ; un changement initié par HA se reflète dans l'UI via les canaux existants (broadcast WS `orp_regulation_mode` + `/get-config`) — aucun code UI ajouté. Valeur hors enum → ignorée + log `warning`. Un changement pendant une injection en cours ne l'interrompt pas : le nouveau mode prend effet au cycle de régulation suivant. Voir [docs/MQTT.md](../MQTT.md#entités-ajoutées-en-feature-009-modes-de-régulation-commandables-v270).

## Fichiers

- [`data/index.html`](../../data/index.html) — structure HTML (carte Régulation + mode réglage : carte Calibration 1 bloc, feature-156)
- [`data/app.js`](../../data/app.js) — logique mode / calibration EZO (POST + polling) / injection manuelle ; `MAINTENANCE_PAGES["orp"]` (feature-156, mécanisme partagé avec la page pH)
- [`data/app.css`](../../data/app.css) — classe `.cal-point-block` réutilisée
- [`src/pump_controller.h`](../../src/pump_controller.h), [`src/pump_controller.cpp`](../../src/pump_controller.cpp) — `canDose(1)` 10 garde-fous
- [`src/sensors.h`](../../src/sensors.h), [`src/sensors.cpp`](../../src/sensors.cpp) — pilotage Atlas EZO ORP
- [`src/atlas_ezo.h`](../../src/atlas_ezo.h), [`src/atlas_ezo.cpp`](../../src/atlas_ezo.cpp)
- [`src/web_routes_calibration.cpp`](../../src/web_routes_calibration.cpp) — endpoints `/calibrate_orp`, `/calibrate_clear`
- [`src/web_routes_control.cpp`](../../src/web_routes_control.cpp) — endpoints `/orp/inject/*` (coquille des gardes : collecte + 409 JSON, feature-006)
- [`src/dosing_logic.h`](../../src/dosing_logic.h), [`src/dosing_logic.cpp`](../../src/dosing_logic.cpp) — `evaluateManualInject()` : gardes d'injection manuelle pures (feature-006)
- [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) — validation `orp_regulation_mode` et `orp_daily_target_ml`
- [`src/ws_manager.cpp`](../../src/ws_manager.cpp) — diffusion `orp` (= valeur officielle) + `orpRaw`/`orpMedian`/`orpFiltered`/`orpRejectedCount` (feature-025 ; `orpFilterReady`/`orpFilterUnstable`/`orpFilterAlarm` retirés en v2.68.0, feature-152)
- [`src/sensor_filter.h`](../../src/sensor_filter.h), [`src/sensor_filter.cpp`](../../src/sensor_filter.cpp) — chaîne de mesure à 5 étages, médiane 9 + lissage exponentiel (feature-152, v2.68.0 ; ex-feature-025)
- [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp) — publication `orp_regulation_mode`, `orp_daily_target_ml`, `orp_cal_points`, `orp_cal_valid`, topics diagnostic (feature-025 ; dépublication des topics « filtre » retirés, feature-152)

## Documentation liée

- [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md) — règles de régulation, garde-fous, anti-rafale, pause mélange.
- [docs/subsystems/sensors.md](../subsystems/sensors.md) — lecture EZO ORP, queue, cache cal_points, chaîne de filtrage.
- [ADR-0016](../adr/0016-regulation-p-temporisee-vs-pid.md) — régulation P temporisée sur mesure filtrée (feature-025).
- [ADR-0002](../adr/0002-mode-programmee-volume-quotidien.md) — mode `scheduled` aveugle au capteur.
- [ADR-0003](../adr/0003-calibration-orp-cote-client.md) — calibration ORP côté client (historique PCB v1, supersédée).
- [ADR-0004](../adr/0004-mode-regulation-enum-3-valeurs.md) — enum à 3 valeurs et booléen miroir `orp_enabled`.
- [ADR-0008](../adr/0008-persistance-cumuls-journaliers-nvs.md) — persistance NVS du cumul journalier.
- [ADR-0014](../adr/0014-migration-atlas-ezo.md) — migration Atlas EZO (supersedes ADR-0001).
- [ADR-0021](../adr/0021-repartition-scheduled.md) — répartition scheduled par fenêtres de 15 min (feature-011, v2.8.0).
- [ADR-0103](../adr/0103-gestes-d-entretien-quittent-la-vue-courante.md) — les gestes d'entretien quittent la vue courante, mécanisme partagé avec le pH et la Température (feature-156, v2.72.0).
