# Page Tableau de bord — `/dashboard`

- **Fichier UI** : [`data/index.html:162`](../../data/index.html:162) (section `#view-dashboard`)
- **URL** : `http://poolcontroller.local/#/dashboard` (page d'accueil par défaut)

## Rôle

Vue d'ensemble de la piscine en temps réel. C'est la page que l'utilisateur voit en arrivant sur l'interface. Elle doit donner **en un coup d'œil** l'état de l'installation et permettre les actions fréquentes (allumer/éteindre filtration et éclairage) sans navigation.

## Structure

La page est une **grille de cinq cartes** (classe `grid--status-cards`) — **était six** jusqu'à la v2.63.x : la carte « Modes temporaires » séparée a été **supprimée** en feature-147/v2.64.0, son unique contenu (le Boost) devenant un sous-bloc de la carte Filtration (voir [Carte Filtration — quatre boutons, mode vs état, Boost intégré](#carte-filtration--quatre-boutons-mode-vs-état-boost-intégré-feature-147-v2640)) :

1. **Carte Filtration** (`#dashboard-filtration-card`) — en-tête : badge d'**état réel** de la pompe (`#detail-filtration-status`, « En marche » / « Arrêtée », ou « Hors gel » en ambre) et texte de **mode demandé** (`#detail-filtration-mode`, « Mode Auto » etc.) ; corps : **quatre boutons de mode** (Auto · Programme · Marche · Arrêt, feature-147, remplacent l'ancienne ligne « Mode » + les boutons Démarrer/Arrêter), horaires, et un sous-bloc **Boost** (feature-053) — voir section dédiée ci-dessous. Un lien engrenage amène vers `/filtration`.
2. **Carte Éclairage** (`#dashboard-lighting-card`) — état, mode (programmation on/off), horaires, boutons Allumer / Éteindre. Lien vers `/lighting`.
3. **Carte pH** (`#dashboard-ph-card`) — valeur pH courante, cible, badge dosage actif, mini-graphique + légende « Lissé (régulation) » / interrupteur « Brut » (v2.38.0), **jauge de position** (feature-147, v2.64.0 — voir [section dédiée](#jauges-de-position-ph--orp-feature-147-v2640)). Bandeau « Calibration requise » si la sonde n'a jamais été calibrée. Lien vers `/ph`.
4. **Carte ORP** (`#dashboard-orp-card`) — valeur ORP courante (mV), cible, badge dosage actif, mini-graphique + même légende (v2.38.0), **jauge de position** (feature-147, v2.64.0 — voir section dédiée). Bandeau calibration. Lien vers `/orp`.
5. **Carte Température** (`#dashboard-temperature-card`) — valeur courante en °C, badge `#temperature-sensor-badge` (« Capteur indisponible » v2.10.5 + segments d'état mesure feature-061 itération 2), mini-graphique. Lien vers `/temperature`.

Chaque mini-graphique est rendu par **uPlot** (usine `createMiniLineChart`, feature-043 — remplace Chart.js) dans un conteneur dédié, alimenté par l'historique de la **journée en cours** (depuis minuit local — v2.18.9, était une fenêtre glissante de 24 h en v2.18.4 et 3 jours avant ; points bruts + moyennes horaires selon l'ancienneté — voir [Plage « journée en cours »](#plage-des-mini-graphiques--journée-en-cours-v2189)). La coloration de la courbe est **conditionnelle par segment** (vert dans la tolérance, interpolation continue vers le rouge au-delà, gris si donnée absente — `getMiniChartRGB`, app.js).

**Responsive / mobile (v2.18.3)** : la hauteur des graphiques uPlot (mini-charts ~88 px, écrans détail 220 px) est posée en **styles inline** par uPlot et resynchronisée par le `ResizeObserver` partagé (`observeChart`, app.js) — debounce 100 ms, `setSize` uniquement si les dimensions sont > 0 et ont réellement changé, re-synchronisation initiale au premier passage (couvre un conteneur aux dimensions provisoires au moment de la création), rotation portrait/paysage incluse. ⚠️ **Ne jamais dimensionner un `canvas` uPlot en CSS** : une règle morte héritée de Chart.js (`canvas { height: 240px !important }` sous 980 px, `data/app.css`) étirait puis écrêtait les graphiques sur iPhone — supprimée en v2.18.3 (un commentaire de garde marque l'emplacement dans `app.css`).

Un **chip `Calibration nécessaire`** (`#calib-chip`) s'affiche en haut de page si pH **ou** ORP est flagué non calibré.

## Carte Filtration — quatre boutons, mode vs état, Boost intégré (feature-147, v2.64.0)

Refonte de surface (`ux-ui-designer`, zéro logique de régulation touchée) reprenant quatre idées d'une carte Home Assistant jugée plus lisible par l'utilisateur. Remplace la ligne « Mode » + libellé et les boutons Démarrer / Arrêter — redondants depuis feature-146, ces derniers ne faisaient déjà que changer de mode.

- **Quatre boutons de mode** (`#filtration-mode-btn-auto` / `-manual` / `-force` / `-off`, grille 2×2 `.filtration-modes`, reste 2×2 même sous 640 px — écran consulté au bord du bassin), libellés **Auto · Programme · Marche · Arrêt** (feature-146). Le bouton actif reçoit `btn--primary` + `aria-pressed="true"` (`updateFiltrationModeButtons()`, app.js) ; les autres restent `btn--ghost`. **Aucun bouton n'est jamais désactivé** : en Arrêt, cliquer « Auto » est précisément la sortie du mode. Les quatre passent par `setFiltrationRunMode()` existant (`POST /save-config {filtration_mode}`) — **aucun second chemin créé** ; `bindDetailActions()` attache les quatre écouteurs là où il n'y en avait que deux (Démarrer/Arrêter, désormais supprimés du tableau de bord).
- **En-tête : le mode ET l'état, et pourquoi les deux coexistent.** Le texte de mode (`#detail-filtration-mode`, « Mode Auto » / « Mode Programme » / « Mode Marche » / « Mode Arrêt », exception visuelle « Hors gel » en ambre — classe `.card__status-label--warn` — pendant la pause anti-gel) dit ce qui est **demandé** ; le badge d'état (`#detail-filtration-status`, **inchangé depuis feature-001**, « En marche » / « Arrêtée ») dit ce que la pompe **fait réellement**. ⚠️ **En mode Auto hors créneau, le mode affiche « Mode Auto » et l'état affiche « Arrêtée » — ce n'est PAS une anomalie** : c'est l'information qui évite de croire à une panne. Les deux sont mis à jour ensemble par `updateFiltrationBadges()`, à partir du **même** calcul `getFiltrationState()` que le badge `.pill` de la page `/filtration` (`#filtration-current-status`) — les deux badges restent synchronisés.
- **Boost en sous-bloc, plus de carte séparée.** La carte `#dashboard-temp-modes-card` (feature-140) est **supprimée** ; son unique contenu, le sous-bloc `#temp-mode-boost`, devient une section de la carte Filtration (séparateur `<hr class="temp-mode__sep">`, classe déjà existante). **Markup, ids et comportement du Boost strictement inchangés** — la ligne d'effet honnête (`#detail-boost-effect`, feature-055) et l'indisponibilité du bouton Activer en mode Arrêt (`startBtn.disabled = filtrationOff`, feature-146) sont **conservées à l'identique**, seul l'emplacement a bougé. Détail complet : [Carte Modes temporaires (feature-140)](#carte-modes-temporaires-feature-140) ci-dessous (section historique conservée pour le reste de son contenu — Chloration de secours, bandeau dédié — inchangé par ce lot).

**Fichiers** : `data/index.html` (grille des 4 boutons + en-tête) · `data/app.js` : `updateFiltrationModeButtons()`, `updateFiltrationBadges()` (étendu pour piloter aussi les boutons et le texte de mode), `bindDetailActions()`, `setFiltrationRunMode()` (prédiction `filtrationRunningOverride` corrigée au passage : `predictFiltrationRunning()` remplace un test `mode === "force"` qui ignorait auto/manual) · `data/app.css` : `.filtration-modes`, `.card__status-label--warn`.

## Jauges de position pH / ORP (feature-147, v2.64.0)

Les cartes pH et ORP gagnent une **jauge de position** horizontale (`.gauge`, CSS pur — aucun SVG, canvas ni bibliothèque), qui **s'ajoute** au mini-graphique temporel existant, elle ne le remplace pas (arbitrage `ux-ui-designer` : une jauge dit « où j'en suis », le graphique dit « d'où je viens »).

### pH (`#ph-gauge`)

- Trace exclusivement le pH **corrigé** (`data.ph`, la même valeur déjà affichée en gros dans la carte) — **jamais** une valeur brute, ni un mélange des deux (`phGaugePosition()`, app.js, condition C1 du gate `pool-chemistry`).
- Échelle de **dessin** fixe `6,20 – 8,10` (constantes `PH_GAUGE_DRAW_MIN`/`MAX`, app.js), miroir des bornes matérielles `kPhFloorRawHard` / `kPhCeilingRawHard` ([`constants.h:1648`](../../src/constants.h:1648) / [`:1669`](../../src/constants.h:1669)) — purement un cadrage visuel, **non servie par `/get-config`**, hors budget de trame.
- **Trois zones colorées**, bornes **lues depuis la configuration réelle** (`_phBounds.targetMin` / `targetMax`, déjà servies par `/get-config` — repli 7,20/7,50 = `kPhTargetMin`/`kPhTargetMax`) : elles **suivent la fenêtre cible** si l'utilisateur la change dans les réglages, aucune valeur dupliquée côté client.

  | Zone | Condition | Texte d'état |
  |---|---|---|
  | Trop bas | pH < cible min (7,20 par défaut) | « Trop bas » |
  | Idéal | cible min ≤ pH ≤ cible max (7,20–7,50 par défaut) | « Idéal » |
  | Trop haut | pH > cible max (7,50 par défaut) | « Trop haut » |

- **Deux repères non colorés et fixes** (`.gauge__tick`, jamais une limite — toujours doublés d'un texte en légende, condition du gate) : **7,00 « eau agressive »** et **7,60 « chlore inefficace »**.

  ⚠️ **Décision du gate `pool-chemistry`, à ne pas rouvrir sans repasser par lui.** La maquette initiale (reprise telle quelle de la carte Home Assistant qui a inspiré ce lot) proposait trois zones nommées « Acide / Neutre / Basique ». **Réfutée** : la neutralité de l'eau est **7,00 pile**, pas une plage ; et 7,60 n'est **pas** une frontière d'acido-basicité mais le seuil où **le chlore cesse d'être efficace** (pKa de l'acide hypochloreux ≈ 7,5, voir [pump-controller.md](../subsystems/pump-controller.md)). Nommer une zone « Neutre » jusqu'à 7,60 aurait rassuré l'utilisateur à 7,55 — juste sous le seuil d'inhibition du dosage ORP sur pH haut (`kPhOrpInhibit`). Les mots retenus disent **quoi penser** (agressive / inefficace), jamais une catégorie.

### ORP (`#orp-gauge`)

- Échelle fixe **0 – 1000 mV** (`ORP_GAUGE_MIN`/`MAX`, app.js) — choix du designer, **aucune constante firmware mirée** ici.
- **Aucune zone nommée ni colorée.** Deux marqueurs seulement : la **mesure** (curseur rond, `#orp-gauge-cursor`) et la **consigne** (trait vertical, `#orp-gauge-target`, position dérivée de `config.orp_target`).
- **Texte d'état**, calcul **réutilisé à l'identique** de celui du badge capteur ORP déjà en production (consigne ± 150 mV — **aucun nouveau seuil introduit**, `orpGaugeStateText()`) :

  | Condition | Texte |
  |---|---|
  | mesure < consigne − 150 mV | « Sous consigne » |
  | consigne − 150 ≤ mesure ≤ consigne + 150 | « Dans la plage » |
  | mesure > consigne + 150 mV | « Au-dessus » |

- ⚠️ **Pourquoi aucune zone n'est nommée, contrairement au pH** : un ORP « correct » n'existe **pas dans l'absolu** — il dépend du stabilisant (acide cyanurique), du pH et de la température de l'eau. Des bandes fixes inventeraient une vérité universelle fausse. Un relevé réel de **108 mV pour une consigne de 530 mV** (2026-10-09) reste lisible sur cette échelle sans qu'aucune bande ne prétende le qualifier — cas limite explicitement couvert, pas un accident de conception.

### Dégradation propre

- **Mesure absente / `NaN`** : `phGaugePosition()` / `orpGaugePosition()` retournent `null` → curseur masqué (`hidden`), texte d'état « — », jamais de position fausse ni de « barre fantôme ».
- **Hors échelle** (ex. pH corrigé < 6,20 ou > 8,10, cas rare) : le curseur se **borne visuellement** aux extrémités (`Math.max(0, Math.min(100, pct))`) — **la valeur numérique affichée au-dessus de la jauge reste exacte**, seule la position visuelle est clampée.
- **Filtration arrêtée / mesures non représentatives** (`measuresOk`, même garde `measuresAreValid()` que les badges existants) : la jauge entière passe en `.gauge--stale` (grisée, `grayscale(1)`), le texte d'état devient « Mesures non représentatives » — jamais une position qui suggérerait une mesure fiable.

**Fichiers** : `data/index.html` (markup `.gauge` dans `#dashboard-ph-card` / `#dashboard-orp-card`) · `data/app.js` : `phGaugePosition()`, `phGaugeStateText()`, `orpGaugePosition()`, `orpGaugeStateText()`, `updatePhGauge()`, `updateOrpGauge()` (appelées depuis le flux `/data` + WS existant — **aucun nouveau champ**, mesure et consigne déjà publiés) · `data/app.css` : classes `.gauge*` (nouvelles, réutilisent uniquement les variables existantes `--danger`/`--success`/`--warn`/`--accent`/`--text`/`--text-secondary`/`--panel2`).

## Bannière « Régulation à l'arrêt » (v2.20.3)

`<div class="regul-alert" id="regul-alert" role="alert" hidden>` en tête de page, au-dessus de la grille de cartes. Rendue par `updateRegulationAlert(json)` à partir des drapeaux firmware `phFilterAlarm` / `orpFilterAlarm` (`GET /data` **et** WebSocket).

Motivation : lors de l'incident du 2026-08-16, le bassin est resté **sans régulation pH ni chlore pendant ≥ 3 h** et rien ne le signalait — l'utilisateur ne l'a détecté que parce qu'une valeur affichée lui semblait incohérente.

| Drapeaux | Titre | Cible du bouton « Diagnostiquer » |
|---|---|---|
| `phFilterAlarm` seul | « Régulation pH à l'arrêt » | `#/ph` |
| `orpFilterAlarm` seul | « Régulation chlore à l'arrêt » | `#/orp` |
| les deux | « Régulation pH et chlore à l'arrêt » | `#/ph` |
| aucun | masquée (`hidden`) | — |

Texte : `Le filtre de mesure est bloqué depuis plus de 5 minutes alors que la filtration tourne : aucun dosage <pH|chlore|pH et chlore> n'est effectué. Ouvrez l'état du filtre pour diagnostiquer et le réarmer si besoin.`

Le firmware n'arme ces drapeaux que si la filtration tourne, que les mesures sont représentatives **et** que le filtre reste non prêt > 5 min : **aucun affichage** pendant une purge, un warmup ou une filtration à l'arrêt. La reprise se fait depuis le modal d'état filtre de la page concernée ([page-ph.md](page-ph.md#bouton--réarmer-le-filtre-ph--v2203), [page-orp.md](page-orp.md#bouton--réarmer-le-filtre-orp--v2203)).

Une variante `regul-alert--inline` du même style est réutilisée dans la carte Régulation pH pour la bannière de bascule automatique en manuel.

## Bandeau « Chloration de secours active » (`#rescue-active-banner`, feature-140, v2.55.2)

`<div class="rescue-active-banner" id="rescue-active-banner" role="status" aria-live="polite" hidden>`, juste **après** `#regul-alert` et **avant** `#dose-block-banner` — le secours **répond** au blocage, il ne s'y ajoute pas.

**Retour d'usage réel (v2.55.2)** : jusqu'alors, l'état et le bouton Arrêter du secours vivaient dans un sous-bloc du widget « Modes temporaires » (`#temp-mode-rescue`, masqué/affiché selon l'activité, v2.55.1). Motif utilisateur : un dosage à l'aveugle en cours est un état notable qui mérite le **haut de page**, pas un bloc qu'il faut aller regarder dans un widget — même rapprochement que le bandeau « Mesures non représentatives ».

**`#temp-mode-rescue` est retiré du DOM dans tous les états**, actif compris — pas masqué, supprimé. Le widget « Modes temporaires » ne contient plus que le **Boost**, en permanence — depuis la **v2.55.3**, cela inclut aussi la ligne d'effet combiné `#temp-modes-effect`, **retirée du DOM avec ses fonctions JS** (voir [Ligne d'effet combiné, retirée en v2.55.3](#ligne-deffet-combiné-b2-feature-140--retirée-en-v2553) pour le détail et la raison de ce retrait).

**Affiché uniquement quand `rescue_until > 0`** (`updateRescueCard()`, app.js).

⚠️ **Conditions du gate `pool-chemistry` — à traiter comme des règles de sécurité chimique, pas des préférences d'ergonomie** :

- Le bouton **Arrêter** (`#rescue-active-banner-stop` → `POST /rescue/stop`) est **dans** le bandeau, pas seulement un texte qui y renvoie.
- Le bandeau est **ni masquable ni repliable** : aucun bouton de fermeture, aucun `details`/`summary`. Un dosage aveugle de 48 h ne doit pas pouvoir être mis hors de vue d'un clic.
- **Deux tons seulement**, jamais `critical`/`fault` : `info` (`.rescue-active-banner--info`) quand le secours dose réellement, `warning` (`.rescue-active-banner--warning`) quand il est armé mais en attente. Le mode est **voulu par l'utilisateur**, ce n'est pas une panne — un rouge permanent pendant 48 h userait le canal d'alerte exactement comme les deux bandeaux redondants corrigés en v2.55.1.

**Paire de textes opposables** (au même titre que L1-L9 de la spec feature-140), rendus par `updateRescueCard()` — **ne jamais reformuler, abréger ou tronquer** sans repasser le gate `pool-chemistry` :

- **Variante A — actif ET dosant** (`rescue_dosing === true`), ton `info` : *« Chloration de secours active jusqu'au %s : le chlore est injecté à volume fixe, sans contrôle de l'ORP. »* (`%s` = échéance au format `JJ/MM à HH:MM`, dérivée de `rescue_until`.)
- **Variante B — actif mais en attente** (`rescue_dosing === false`), ton `warning` : *« Chloration de secours active jusqu'au %s, mais le chlore ne coule pas actuellement. %s »* (1er `%s` = échéance, 2nd `%s` = cause reprise **mot à mot** parmi L3/L4/L5a/L5b, `_rescueReasonText()`, priorité **L3 (filtration à l'arrêt) > L4 (pH > 7,6) > L5 (plafond journalier)** — inchangée depuis v2.55.0.)
- **Phrase de suggestion Boost, ajoutée aux DEUX variantes (v2.55.3)**, jamais un bloc séparé : *« Suggestion : activez aussi le Boost pour prolonger la filtration — sur une eau chargée en matière organique, c'est le levier le plus utile, davantage que le chlore lui-même. »* **Conditionnelle** — condition du gate `pool-chemistry`, non négociable : affichée uniquement quand le secours est armé **et** que la filtration n'est pas déjà prolongée (`boost_active` faux, ou `boost_filtration_extended === false`) ; affichée en permanence elle deviendrait du décor. C'est l'**unique** endroit de l'application qui dit que la filtration prime sur le chlore — enseignement direct du terrain (2 L/jour pendant trois jours, ORP bloqué à 261 mV pour une consigne de 530 mV) et contre-pied du réflexe « ça ne monte pas, j'en remets ». Vient du widget « Modes temporaires » (ex-`#temp-modes-suggest-boost`), retiré en même temps que la ligne d'effet combiné — voir [Ligne d'effet combiné, retirée en v2.55.3](#ligne-deffet-combiné-b2-feature-140--retirée-en-v2553).

**L1 disparaît de l'écran** avec le widget. C'est **admissible** : le libellé **L2**, déjà révisé en v2.55.1 pour absorber l'avertissement que portait L1, continue de le porter — ce n'est pas une perte d'information, L1 n'était de toute façon visible qu'au moment de l'activation (bouton désormais hors widget).

**Incohérence fermée côté `#dose-block-banner`** : tant que le secours tourne déjà (`rescue_until > 0`, paramètre `rescueActive` de `_renderDoseBlockRow()`), l'offre **L7** et son bouton « Activer la chloration de secours » **disparaissent** de la ligne ORP du bandeau de blocage — proposer d'armer ce qui tourne déjà est le genre d'incohérence qui fait douter de tout le reste de l'affichage. La ligne **conserve** son texte de cause (la mesure EST toujours non représentative, c'est le cas nominal tant que le secours est actif) et son bouton « Ouvrir l'injection manuelle » (B3, voir [section dédiée](#raccourci-vers-linjection-manuelle-b3)).

Verdict sécurité chimique et conditions opposables : voir la section « Correctif post-production (v2.55.2) » de la spec [feature-140](../../specs/features/done/feature-140-chloration-de-secours-et-widget-des-modes-temporaires.md) et [ADR-0087](../adr/0087-ce-qu-un-mode-temporaire-a-le-droit-de-faire.md) (décision structurante déjà actée — ce correctif ne fait que déplacer l'affichage).

## Bandeau « Dosage bloqué » (feature-138, v2.54.0)

`<div class="dose-block-banner" id="dose-block-banner" hidden>`, juste **après** `#regul-alert` et avant la grille de cartes. Rend lisible, sans ouvrir le panneau développeur, **depuis combien de temps** le dosage est refusé et **pourquoi** — une ligne par voie (`#dose-block-row-ph` / `#dose-block-row-orp`), rendue par `updateDoseBlockBanner(json)`.

Motivation — incident du **2026-10-02** : après des pluies diluviennes, de la boue a couvert le fond du bassin, les deux voies ont été déclarées instables et **tout dosage automatique s'est arrêté**, chlore compris, au moment précis où la demande en chlore était maximale. Le code de refus (`phDoseBlockedCode` / `orpDoseBlockedCode`) existait déjà, mais sans durée ni affichage hors panneau dev — l'utilisateur a diagnostiqué à tort « le calcul de dispersion est bloqué » sur une valeur qui s'était en réalité déjà recalculée trois fois en deux minutes.

**Trois informations par ligne**, réutilisant **strictement** la taxonomie existante `DOSE_BLOCK_CODES` / `_doseBlockInfo()` (aucune nouvelle classification) :

- **la voie** (pH ou chlore/ORP) ;
- **la cause en clair** (même libellé français que le reste de l'UI) ;
- **la durée**, au format partagé `_elapsedText()` (« bloqué depuis 12 min », etc.), source `phDoseBlockedForS` / `orpDoseBlockedForS` — voir [API.md](../API.md#champs-ajoutés-en-feature-025-lissage-mesures--régulation-p-temporisée).

**Absent si rien n'est bloqué** — pas de bandeau permanent qui s'ignore : `box.hidden = true` dès que ni pH ni ORP n'ont de code de refus actif.

**Non-duplication avec `#regul-alert`** : si cette bannière affiche déjà une voie (`phFilterAlarm` / `orpFilterAlarm`), la ligne correspondante de `#dose-block-banner` est supprimée pour cette voie — les deux bandeaux ne répètent jamais la même information.

**Fusion en une seule ligne** « *cause* (pH et chlore) » **uniquement** si les deux voies partagent le **même code** ET le même ton `normal` ; **jamais** pour `fault` ou `unknown`, où une panne par voie doit rester lisible individuellement (deux pannes différentes qui tombent sur le même code resteraient deux pannes à traiter séparément).

**Correctif production (2026-10-05, v2.55.0+, constaté juste après le redémarrage OTA)** : deux défauts corrigés sur la voie ORP.

1. **Ton par voie manquant sur le code de garde 250.** `DOSE_BLOCK_CODES[250]` porte `tone: 'fault'` (pastille de dosage) et `toneOrp: 'normal'` (côté chlore, cette garde n'est que l'inhibition sur pH indisponible/haut — transitoire et bénigne, pas une panne). Avant correctif, `_doseBlockInfo()` ignorait `toneOrp` et codait `'fault'` en dur pour ce bandeau : un simple réamorçage de filtre affichait une ligne **rouge** d'alarme. `_doseBlockInfo()` lit désormais `toneOrp` quand `prefix === 'orp'`.
2. **Deuxième règle de fusion, bornée strictement** (s'ajoute à celle ci-dessus, ne la remplace pas) : quand `orp.code === 250` (garde de sécurité pH/ORP) **ET** `ph.code` fait partie de la liste blanche `{3, 4, 15, 18, 19, 22}` (mesure pH non représentative), les deux lignes décrivent en réalité **une seule cause** — le pH indisponible suspend le pH *et* inhibe le chlore par conséquence — et sont fusionnées. Texte imposé mot à mot : « *label pH* — dosage pH et dosage chlore suspendus tous les deux. Reprise automatique dès que les mesures sont de nouveau représentatives. », ton `normal`. Toute autre cause ORP sous le code 250 (quota journalier, filtre ORP instable…) **reste sur sa propre ligne** : fusionner là masquerait un second problème réel.
   - **Durée affichée = la plus ancienne des deux** (celle de la cause racine pH, pas de la conséquence ORP) : afficher l'âge, plus court, de la conséquence sous-estimerait la durée réelle de suspension du chlore.
   - Le bouton « Activer la chloration de secours » (voir [chaînage B4](#chaînage-depuis-le-bandeau--dosage-bloqué--b4-feature-140)) et le libellé **L7** restent attachés à la ligne fusionnée — la fusion ne les fait pas disparaître.

⚠️ **Leçon pour la suite** : une surface neuve (ce bandeau, feature-138) a réutilisé la taxonomie `DOSE_BLOCK_CODES` existante, mais n'en a repris que la moitié — le ton générique `tone`, sans l'override par voie `toneOrp` déjà présent dans la table pour le code 250. Un test ne voit pas cet oubli (le code compile, les deux champs existent) ; un œil humain devant l'écran l'attrape en une seconde. Vérifier systématiquement, pour toute nouvelle consommation de `DOSE_BLOCK_CODES`, que les variantes par voie (`toneOrp`, `pillOrp`) sont lues partout où leur équivalent générique l'est.

**Ton et accessibilité** : `role="alert"` réservé au ton `fault` (panne) ; `role="status" aria-live="polite"` sinon — ne pas sur-alerter sur du **bénin** (filtration arrêtée la nuit est le cas **normal**, pas une panne, et le bandeau ne doit jamais crier dessus).

**Péremption WebSocket** : `#dose-block-banner` fait partie de la liste marquée `.is-stale` par `_onDisconnect()` (au même titre que `#ph-card-stats` / `#orp-card-stats`) — **plus** un texte explicite (`.dose-block-banner__stale-note`, « Information non rafraîchie (connexion perdue) ») : l'opacité seule n'explique rien à un lecteur d'écran.

### Raccourci vers l'injection manuelle (B3)

Chaque ligne **peut** porter un bouton secondaire « Ouvrir l'injection manuelle », **lien de navigation pur** (comme `#regul-alert-cta`) vers `#/ph` ou `#/orp` — **jamais** de présélection de volume/durée/produit, **jamais** de bascule du mode de régulation (basculer `ph_regulation_mode`/`orp_regulation_mode` persiste la config et **coupe** la régulation automatique : le raccourci ne touche à aucun des deux). Un clic ouvre d'abord une **confirmation explicite**, distincte de celle du bouton d'injection, avec des libellés qui rappellent — selon la voie — l'absence de garde de stabilité sur l'injection manuelle, l'exigence d'un contrôle au réactif, et pour le pH qu'une injection d'acide ne se reprend pas.

Le raccourci n'est proposé que pour une **liste blanche** de causes — uniquement celles où le dosage automatique doute de la **mesure** elle-même (lecture NaN, filtre non prêt/instable, fenêtres de lissage ou de purge), jamais pour une **panne** (ton `fault`) ni pour la cause bénigne « filtration arrêtée » : sur ces causes, l'injection manuelle serait soit non pertinente, soit refusée par les mêmes gardes que le dosage automatique (elle ne contourne que l'absence de jugement de stabilité, pas les limites journalière/horaire/anti-rafale). Asymétrie voulue entre voies : proposé **par défaut sur l'ORP** (un excès de chlore se dissipe), uniquement sur le **pH** si le bandeau ne concerne que cette voie. Jamais présenté comme l'action recommandée (bouton secondaire, pas primaire).

Verdict sécurité chimique (gate `pool-chemistry`, conditions opposables C1-C6) et libellés imposés mot à mot : voir la spec [feature-138](../../specs/features/done/feature-138-dater-et-rafraichir-l-etat-de-blocage-du-dosage.md).

## Plage des mini-graphiques : journée en cours (v2.18.9)

Les mini-graphiques des cartes pH, ORP et Température n'affichent **que la journée en cours**, à partir de **minuit local**. Une fenêtre glissante de 24 h (comportement v2.18.4) faisait apparaître une portion de la veille en tête de courbe, ce qui brouillait la lecture « où en est-on aujourd'hui ».

- **Filtrage côté navigateur** : la requête au firmware reste `GET /get-history?range=24h`. Aucun changement de contrat HTTP, aucun paramètre nouveau, aucun changement firmware — les points dont le `timestamp` est antérieur à minuit local sont simplement écartés à la réception (`startOfTodaySeconds()`, `loadMiniChartData()`, app.js). Le firmware et [`docs/API.md`](../API.md#get-get-history--write) sont inchangés.
- **Filtrage avant calcul des coupures** : l'écart est mesuré entre points **déjà filtrés**, sinon le premier point du jour créerait un faux trou de plusieurs heures avec le dernier point de la veille. Le « point précédent » du chemin incrémental n'est retenu que s'il appartient lui aussi au jour courant.
- **Passage de minuit automatique** : le jour civil affiché est mémorisé (`miniChartDayStart`). Au premier rafraîchissement suivant le changement de date (cadence **5 min**), le polling bascule du mode incrémental (`?since=`) vers un **rechargement complet** qui **remplace** la série. Les points de la veille disparaissent seuls, sans action de l'utilisateur ni rechargement de la page.
- **Début de journée assumé** : entre minuit et le premier relevé, les courbes sont **vides** ; elles restent courtes pendant les premières heures. C'est le comportement demandé, pas un défaut. Le repli d'échelle existant (`applyMiniChartFallbackScale`, cadre cible ± tolérance) conserve un graphique lisible même sans point.
- **Étiquettes de l'axe X** : tous les points appartenant au jour courant, `miniChartPointLabel()` affiche l'heure seule. L'**infobulle reste datée et typée** (`jj/mm hh:mm` + mesure / moyenne horaire / moyenne journalière — acquis v2.18.4, voir section suivante).
- **Hors périmètre** : les graphiques des **écrans détail** (`/ph`, `/orp`, `/temperature`) conservent leurs plages `Tout` / `30j` / `7j` / `24h`. Les coupures de courbe aux arrêts de filtration (> 90 min) sont inchangées.
- **Fuseau horaire de référence = celui du NAVIGATEUR**, pas celui du contrôleur (`startOfTodaySeconds()` s'appuie sur `new Date().setHours(0,0,0,0)`). Les horodatages reçus sont des epoch UTC, la conversion est donc exacte ; mais consulter le tableau de bord depuis un appareil réglé sur un autre fuseau décale la coupure de « minuit » d'autant. Sans conséquence en usage normal (même fuseau que le bassin) ; c'est le même choix que pour les étiquettes horaires, qui suivent déjà la locale du navigateur.

> ⚠️ Le changement est **entièrement dans `data/app.js`** : il faut **reconstruire et reflasher le filesystem** (`./build_fs.sh` puis `./deploy.sh fs` ou `ota-fs`). Mettre à jour le firmware seul ne suffit pas.

## Mini-graphiques pH / ORP : la courbe suit le nombre affiché (feature-095, v2.38.0)

**La vraie raison du lot.** Le grand nombre des cartes pH et ORP affichait **déjà** la mesure **lissée** servie par `/data` — celle sur laquelle la régulation décide — au-dessus d'une courbe **brute**. Le nombre et la courbe ne parlaient donc pas du même signal, ce qui a produit **deux mauvais diagnostics** par le passé. La feature-095 ne « promeut » pas la lissée : elle **aligne la courbe sur le nombre qu'elle est censée expliquer**.

Depuis la v2.38.0, les mini-graphiques pH et ORP tracent **deux séries** :

| Série | Rôle | Rendu | Source `/get-history` |
|---|---|---|---|
| **Lissé (régulation)** | courbe **principale** — c'est la valeur du grand nombre, celle qui décide du dosage | trait épais (2 px), **spline**, **aire** remplie, coloration conditionnelle par segment, point de survol | `phFiltered` / `orpFiltered` |
| **Brut** | courbe **secondaire**, diagnostic sonde | trait fin (1 px), tracé **linéaire** (une spline inventerait des dépassements sur un signal bruité et dense) | `ph` / `orp` |

- **pH affiché à 2 décimales** sur ces mini-graphiques (était 1) : à 0,1 près, la quantification dépasse l'amplitude utile du signal lissé (~0,05).
- **Infobulle** : quand les deux séries existent, deux lignes préfixées `lissé` / `brut` (+ la ligne de granularité). Quand seul le brut existe, une seule ligne `brut` — l'infobulle ne disparaît **jamais** sur la portion sans lissée (ce serait un effet « panne »).
- **La carte Température n'a qu'une série** : elle passe par la même fabrique `createMiniLineChart` avec une série brute entièrement nulle. Un seul chemin de code, la série principale reste toujours l'index 2.
- **Les graphiques des écrans détail (`/ph`, `/orp`, `/temperature`) ne changent pas** : ils agrègent **1 point par jour**, granularité à laquelle la lissée vaut toujours `null` — et où elle serait de toute façon indiscernable du brut. Seuls les mini-graphiques du tableau de bord sont touchés. C'était aussi la **réduction de risque** du lot : la fonction de tracé principale (`createLineChart`) reste mono-série.

### La frontière ~8 h, et pourquoi elle ne ressemble pas à une panne

La fenêtre affichée couvre la journée en cours (jusqu'à 24 h) ; la mesure lissée, elle, n'est historisée **que sur les points 5 min**, soit **~8 h de profondeur** ([history.md](../subsystems/history.md#la-lissée-nest-historisée-que-sur-les-points-raw)). Le problème de conception n'était pas « la courbe s'arrête » — c'était « **une courbe forte apparaît de nulle part** » au milieu du graphique.

Trois dispositifs, qui font lire **un seul événement** au lieu de deux :

1. **Le brut couvre toujours 100 % de la fenêtre** — il n'y a jamais de zone vide.
2. **La lissée est une aire, pas une ligne** : une **zone qui commence** évoque une période ; une **ligne qui s'arrête** évoque une panne.
3. **L'opacité du brut bascule à la même frontière** : `0,35` (halo, sous la lissée) là où la lissée existe, `0,9` (courbe pleine, aspect d'avant la 2.38) là où elle n'existe pas. Le basculement tombe **exactement** au x où l'aire commence.

**Aucune lissée sur toute la fenêtre** (première heure après la mise à jour, journée entièrement antérieure à la 2.38) : la légende le **dit** — l'entrée « Lissé (régulation) » s'atténue et reçoit le suffixe *« (aucune sur la période) »* + un `title`. La couleur et l'opacité ne portent **jamais seules** l'information.

### Interrupteur « Brut » (`#ph-mini-raw-toggle`, `#orp-mini-raw-toggle`)

Sous chaque mini-graphique pH et ORP, une légende (`.mini-legend`) porte l'entrée « Lissé (régulation) » et un **interrupteur « Brut »** (`role="switch"`, `aria-checked`, cible tactile 44 px compensée par marge négative pour ne pas grandir la carte).

- **Un interrupteur par graphique**, indépendants. **Défaut : brut visible.**
- **Mémorisé** dans `localStorage` sous `poolctl.ui.showRaw.ph` / `poolctl.ui.showRaw.orp` (`'1'` visible / `'0'` masqué). L'état est restauré **avant** le premier chargement de données.
- **Bénéfice réel du masquage** : l'échelle Y **se resserre sur la lissée**. C'est pour cela que masquer le brut passe ses valeurs à `null` et **jamais** par `show: false` — une série cachée continuerait de dicter l'échelle.
- **Aucun appel réseau au clic** : les valeurs brutes sont déjà en mémoire (`chart.st.raw`).

> **Première utilisation de `localStorage` dans le projet.** L'accès est **entièrement protégé** (`uiPrefGet` / `uiPrefSet`, app.js) : `localStorage` peut lever **à la lecture comme à l'écriture** (Safari en navigation privée, stockage désactivé par politique). Un objet en mémoire sert de repli et un échec ne remonte **jamais** à l'utilisateur — la préférence vaut alors pour la session seulement. Tout nouveau réglage d'interface persisté doit passer par ces deux fonctions et par le préfixe `poolctl.ui.`.

## Marqueurs de dosage sur les mini-graphiques pH / ORP (bug-098, v2.47.0)

Depuis la **v2.47.0**, chaque point d'historique porte un **masque de voies** de dosage et non plus un booléen ([ADR-0072](../adr/0072-absence-de-valeur-hors-du-domaine.md), [history.md](../subsystems/history.md#masque-de-voies-de-dosage-bug-098-v2470)). Les mini-graphiques **pH et ORP** le rendent en **trois séries de marqueurs distinctes**, dessinées au ras du bas de l'aire de tracé par le plugin `dosingMarksPlugin` (hook `draw` d'uPlot) :

| Bit | Marqueur | Forme | Sens |
|---|---|---|---|
| `0x01` | pH | rectangle **plein**, brun, **rang du bas** | la pompe **pH** dosait à cet instant |
| `0x02` | chlore | rectangle **plein**, bleu, **rang du dessus** | la pompe **ORP** dosait |
| `0x04` | voie inconnue | rectangle **en creux** (contour seul), gris, **à cheval sur les deux rangs** | dosage attesté, **voie indéterminée** — point écrit par un firmware ≤ 2.46 |

- **Trois formes distinctes, pas trois teintes** : couleur **+** rang **+** plein/creux. On doit pouvoir dire **quelle pompe a dosé** d'un coup d'œil, et la couleur ne porte jamais seule l'information.
- **La voie inconnue est rendue « en creux »** — un contour, pas un aplat : c'est visuellement une **absence de réponse**, non une troisième pompe. Elle n'apparaît que sur des points antérieurs à la mise à jour, et se raréfie donc d'elle-même.
- **Le marqueur ne porte jamais seul l'information** : l'infobulle du point ajoute une ligne par bit présent — « Dosage pH », « Dosage chlore », « Dosage (voie inconnue) » (`dosingLabels()`).
- **Tester des bits, jamais l'égalité** : un bit inconnu servi par un firmware plus récent est **sans effet** ici, il ne casse ni le tracé ni l'infobulle.
- **`0` → aucun marqueur.** Un point sans dosage ne dessine rien : les marqueurs se lisent comme des événements, pas comme une série continue.
- **Un booléen `true` d'un firmware antérieur se lit `0x04`, JAMAIS `0x01`.** Le déduire « pH » inventerait la voie — c'est le défaut que bug-098 corrige côté firmware, on ne le réintroduit pas côté client (`dosingMaskFromApi()`).

**Portée exacte** — trois précisions qui évitent des attentes fausses :

- **Mini-graphiques du tableau de bord seulement.** La carte **Température** passe par la même fabrique `createMiniLineChart` avec un tableau de masques **vide** (`miniDoseSeries()` le remplit de `0`) : rien n'est tracé, un seul chemin de code pour les trois cartes.
- **Les graphiques des écrans détail (`/ph`, `/orp`, `/temperature`) ne changent pas** — même arbitrage que feature-095 : ils agrègent 1 point par jour, granularité à laquelle un marqueur de dosage ne dirait rien d'utile.
- **Tableau strictement parallèle aux labels.** `st.dose` suit `st.labels` sur les **deux** chemins d'alimentation, chargement complet (`updateMiniChart`) **et** incrémental (`appendMiniChart`) — comme `st.ts` et `st.gran`, et pour la même raison : les trois mini-graphiques partagent les points sources, chacun doit posséder sa propre copie (`slice()`).

## Tooltip daté/typé et coupures de courbe (v2.18.4, bug-points-futurs-et-ux-courbes)

Conventions partagées entre les **mini-graphiques** du dashboard (`createMiniLineChart`) et les **graphiques des écrans détail** pH/ORP/température (`createLineChart`, app.js) :

- **Tooltip toujours daté ET typé** : le titre du tooltip affiche systématiquement **date + heure** au format `jj/mm hh:mm` (`tooltipDateTime()`) — plus d'heure seule pour « aujourd'hui » —, et une ligne indique le **type de point** selon la granularité `/get-history` (`granularityLabel()` : `0` → « mesure », `1` → « moyenne horaire », `2` → « moyenne journalière »). Les étiquettes de l'**axe X** restent courtes (heure seule si aujourd'hui) — le tooltip lève l'ambiguïté sans surcharger l'axe. Implémentation : tableaux **parallèles** `st.ts` / `st.gran` à côté de `st.labels` / `st.values` (les 3 mini-charts partagent les points sources → `updateMiniChart` copie par `slice()` pour que `appendMiniChart` ne mute pas les voisins). Sur les écrans détail (agrégation 1 point/jour), le timestamp/granularité retenu est celui du point qui a fourni **la valeur du capteur concerné** ; le point live « Aujourd'hui » (`updateChart`) est daté de maintenant, granularité « mesure ». `setPoints(labels, values, ts, gran)` accepte `ts`/`gran` optionnels (appels existants sans tooltip daté restent valides).
- **Coupure de courbe aux arrêts** (mini-graphiques) : deux points consécutifs espacés de **plus de 90 min** (`CHART_GAP_SECONDS = 5400`) → une entrée `null` est insérée dans la série (uPlot lève le crayon, même convention que les mesures invalides feature-061) — les **arrêts de filtration** et coupures de courant deviennent visibles. Appliqué **aussi au chemin incrémental** (`?since=`) : le « point précédent » du premier nouveau point est le dernier point déjà affiché (`lastMiniChartTimestamp`), un arrêt à cheval sur le polling crée aussi le trou.
- **Coupure sur les écrans détail** : à 1 point/jour, le seuil de 90 min est inapplicable (deux jours consécutifs sont espacés de ~24 h) — l'équivalent est un **jour calendaire entièrement absent** entre deux jours présents → entrée `null` insérée (comparaison en UTC des clés `YYYY-MM-DD`).

## Badge multi-états des cartes pH / ORP (`#ph-sensor-badge`, `#orp-sensor-badge`)

Chaque carte pH et ORP porte un badge composé (`composeSensorBadge()`, app.js — feature-012) qui agrège jusqu'à cinq états candidats, par ordre de priorité décroissante :

| Priorité | Segment | Variant | Condition |
|---|---|---|---|
| 0 | `Capteur indisponible` | `danger` | valeur `ph` / `orp` `null` ou NaN dans le WS (capteur stale ou figé — feature-022, v2.10.0) — **gaté par `measures_valid`** (feature-061, voir ci-dessous) |
| 1 | `Filtration arrêtée` | `info` | `water_present === false` sans staleness (feature-061, v2.13.0 — voir [section dédiée](#mesures-phorp-non-représentatives--filtration-arrêtée--stabilisation-feature-061-v2130)) |
| 1 | `Signal filtration périmé` | `warning` | `water_present === false` et `filtration_state_stale === true` (feature-061) |
| 1 | `Reprise automatique dans N min` (v2.62.0 ; était `Stabilisation depuis N min` / `Re-stabilisation depuis N min` en v2.34.0, et `Stabilisation : ~N min` avant) | `info` | `water_present === true` et `measures_valid === false` (attente post-reprise, feature-061) |
| 1 | `Trop bas (X)` / `Trop élevé (X)` | `danger` | valeur hors plage (cible ± 0,2 pH / ± 150 mV) — **gaté par `measures_valid`** (feature-061) |
| 2 | `Limite journalière atteinte` | `warning` | `ph_limit_reached` / `orp_limit_reached` |
| 3 | `Cumul XX %` | `warning` | cumul journalier ≥ 75 % du quota |
| 4 | `Stock faible (X,X L)` | `warning` | `*_remaining_ml` ≤ `*_alert_threshold_ml` (seuil > 0) |

**Règle de composition** :
- **Maximum 2 segments** affichés simultanément, séparés par « · ». Au-delà de deux candidats, seuls les **deux plus prioritaires** sont retenus (ex. « hors plage + limite atteinte + stock faible » → `Trop élevé (7.8) · Limite journalière atteinte`, le stock est tronqué).
- Variant du badge : hiérarchie **`danger` > `warning` > `info`** (feature-061 — avant : `danger` sinon `warning`). La variante `info` (`.sensor-badge--info`, fond bleu) signale un **état normal informatif** — « attendre », par opposition au rouge « agir ».
- « Hors plage » et « Limite journalière atteinte » simultanés affichent désormais **les deux segments** (avant v2.9.0 : seul « hors plage » était visible).

**Segment « Capteur indisponible »** (feature-022, v2.10.0) :
- Priorité la plus haute (0) : un capteur mort passe toujours devant les autres états.
- Piloté par les **signaux WS existants** (`ph` / `orp` à `null` quand le firmware ne dispose plus d'une valeur valide — stale ou figé) : aucun champ WS nouveau.
- **Garde anti-faux-positif au boot** : le segment n'est produit que si `latestSensorData !== null` (au moins un message WS reçu) — `updateSensorBadges()` étant aussi appelé depuis `loadConfig()` avant le premier push WS.
- « Capteur indisponible » et « hors plage » sont mutuellement exclusifs (une valeur `null` n'est pas comparable à la cible).
- **Garde anti-faux-positif « mesures invalides » (feature-061)** : le segment n'est produit que si `measuresAreValid(latestSensorData)` — on ne peut pas juger une sonde (ni une valeur hors plage) quand l'eau ne circule pas. Filtration arrêtée → seul le segment d'état mesure s'affiche, jamais un faux « Capteur indisponible » rouge.

**Segment « Cumul XX % »** :
- Affiché à partir de **75 %** du quota journalier (`ph_daily_ml / max_ph_ml_per_day`, `orp_daily_ml / max_chlorine_ml_per_day`), même arithmétique que la barre de cumul des pages `/ph` et `/orp` (arrondi des entrées, pourcentage plafonné à 100).
- Masqué quand la limite est atteinte (le segment « Limite journalière atteinte » prend le relais, pas de redondance), quand la limite n'est pas configurée (quota ≤ 0), ou quand les données sont indisponibles.

## Badge de la carte Température (`#temperature-sensor-badge`, v2.10.5, étendu feature-061 itération 2 puis feature-068)

La carte Température porte un badge calqué sur les badges pH/ORP :

- **« Capteur indisponible »** (variant `danger`, v2.10.5) : affiché quand `temperature` est `null` ou NaN dans le WS (sonde DS18B20 débranchée, non identifiée ou en erreur). Même **garde anti-faux-positif au boot** (badge produit uniquement si `latestSensorData !== null`) et, depuis l'itération 2 de feature-061, même **garde anti-faux-positif « mesures invalides »** que pH/ORP (segment gaté par `measuresAreValid()` — on ne juge pas la sonde quand l'eau ne circule pas).
- **Segments d'état mesure** (feature-061 itération 2) : mêmes états que pH/ORP via `measureStateSegments()` — « Filtration arrêtée » (info bleu), « Signal filtration périmé » (warning), « Reprise automatique dans N min » (info, v2.62.0 ; était « Stabilisation depuis N min » en v2.34.0). Voir la [section dédiée](#mesures-phorp-non-représentatives--filtration-arrêtée--stabilisation-feature-061-v2130).
- **« Sonde eau non identifiée »** (variant `danger`, priorité 0, **v2.47.1**, feature-068) : aucun rôle « eau » assigné alors qu'au moins une sonde est présente sur le bus (`sondes_identified === false` **et** `sondes_detected >= 1`). État **actionnable** — aller identifier la sonde dans les réglages —, et **distinct** de « figée ».
- **« Sonde température figée »** (variant `warning`, priorité 1, **v2.47.1**, feature-068 ; fenêtre portée à 24 h en **v2.56.1**, bug-142) : `tempFrozenAlarm === true`, c'est-à-dire 43 200 lectures valides consécutives dans une bande < 0,03 °C (≈ 24 h — était 900 / 30 min). **Pas actionnable en urgence côté chimie** : le dosage n'est pas inhibé, l'erreur de pH induite vaut ≈ 0,001 pH ([ADR-0074](../adr/0074-temperature-figee-statu-quo-instrumente.md)). D'où le `warning` et non le `danger`. Le drapeau **gate en revanche le planning de filtration** (`filtration.cpp:162` et `:405`) tant qu'il est posé.
- Masqué dès que la valeur redevient valide (disparition au prochain push WS, sans reload).

**Précédence des trois états de la sonde eau** — c'est un arbitrage, pas un détail de présentation :

| Rang | État | Condition | Effet sur les autres |
|---|---|---|---|
| 1 | « Capteur indisponible » (`danger`) | `temperature` `null`/NaN | **prime sur tout** : une sonde qui ne mesure plus rien ne peut pas être « figée » — c'est aussi ce qui empêche un badge « figée » de rester à l'écran après un débranchement en cours d'épisode |
| 2 | « Sonde eau non identifiée » (`danger`) | rôle « eau » non assigné, ≥ 1 sonde détectée | **supprime** « figée » : on ne publie **jamais** « eau figée » pour un rôle inconnu |
| 3 | « Sonde température figée » (`warning`) | `tempFrozenAlarm`, rôle « eau » identifié, valeur présente | — |

Les trois segments sont gatés par `measuresAreValid()`, comme pH/ORP : filtration arrêtée, l'eau est immobile dans la canalisation et la sonde peut légitimement rendre la même valeur pendant une demi-heure — on ne juge pas la sonde quand l'eau ne circule pas.

> ⚠️ **Pourquoi la condition « non identifiée » s'appuie sur `sondes_identified` et non sur `temperature_raw`**, qui serait le discriminant naturel : `temperature_raw` est **WS-seul** (dette déclarée dans la sentinelle de parité). S'en servir aurait rejoué le piège n° 1 du projet — `latestSensorData` est **remplacé en entier** par chaque charge `/data`, le champ y disparaîtrait toutes les ~30 s, et le badge **clignoterait** entre « figée » et « non identifiée » ([ADR-0071](../adr/0071-parite-data-trame-capteurs.md)). `sondes_identified` / `sondes_detected` sont servis des **deux** côtés, et la condition retenue est **exactement** celle de la pastille `#sondes-chip` existante : l'appareil dit déjà la même chose, au même moment.
>
> **Limite assumée** : `sondes_identified` couvre les **deux** rôles (eau **et** circuit). Dans une installation où seule la sonde « circuit » manque, le badge « figée » est **supprimé à tort**. Défaut **conservateur** — on perd un badge informatif, on n'en invente pas un faux — et le sens inverse est impossible : le rôle « eau » non assigné rend toujours `sondes_identified` faux.

Pas de segments dosage/limites/calibration ici : la température n'a ni dosage, ni limites, ni calibration.

## Convention « `--` » quand un capteur est indisponible (v2.10.5)

Toute valeur capteur `null`/NaN s'affiche « **`--`** » sur **tous** les sites d'affichage numérique — jamais de conservation silencieuse de la dernière valeur connue (bug corrigé en v2.10.5 : le pattern « skip si null » figeait indéfiniment l'ancienne valeur à l'écran quand une sonde était débranchée) :

- **Métriques du tableau de bord** (`updateDashboardMetrics`, app.js) : `#m-temp`, `#m-ph`, `#m-orp` ;
- **Widgets compacts** : `#compact-ph`, `#compact-orp`, `#compact-temperature` — l'unité est conservée dans la structure (ex. `--<span class="compact-unit">°C</span>`) pour ne pas casser la mise en page ;
- **Sections détaillées** des cartes.

Cette convention est celle déjà utilisée par les pages Réglages/Calibration. **Exceptions volontaires** : les graphiques et mini-graphiques affichent des **trous** sur les points `null` (comportement correct — depuis feature-061 les `null` sont poussés dans la série pour que uPlot lève le crayon, au lieu d'être filtrés). Retour du capteur → les valeurs réapparaissent automatiquement au prochain push WS (≤ 1 cycle de rafraîchissement).

**Extension feature-061 (v2.13.0)** : la convention « `--` » s'applique aussi quand la mesure est **présente mais non représentative** (`measures_valid === false` — filtration arrêtée ou purge post-reprise). La distinction est portée par le badge : bleu « Filtration arrêtée » / « Stabilisation » (état normal) vs rouge « Capteur indisponible » (anomalie, libellé « Reprise automatique dans N min » depuis la v2.62.0, était « Stabilisation »).

## Mesures pH/ORP non représentatives — filtration arrêtée / stabilisation (feature-061, v2.13.0)

Les sondes pH/ORP étant montées dans le circuit de filtration, **filtration arrêtée = eau stagnante** : la valeur lue dérive localement et ne reflète plus le bassin. Depuis l'**itération 2** de feature-061, la **température** est traitée à l'identique (sonde DS18B20 dans la canalisation → dérive vers la température ambiante du local à l'arrêt) — même drapeau `measures_valid`, mêmes badges, mêmes trous dans les courbes. Le firmware publie le verdict (`measures_valid`, voir [measure-validity.md](../subsystems/measure-validity.md)) ; l'UI l'applique :

**Segments d'état mesure** (`measureStateSegments()`, app.js — partagés dashboard / pages pH-ORP / carte et compact température, priorité 1, variant selon le cas) :

| Condition | Segment (libellé exact) | Variant | Sens |
|---|---|---|---|
| `water_present === false`, pas de staleness | `Filtration arrêtée` | `info` (bleu) | État **normal** — attendre la reprise, rien à faire |
| `water_present === false` et `filtration_state_stale === true` | `Signal filtration périmé` | `warning` (ambré) | Anomalie du signal externe — vérifier le câblage/l'émetteur |
| `water_present === true` et `measures_valid === false` | `Reprise automatique dans N min` (arrondi minute, ≥ 1) — ou `Reprise automatique imminente` sous ~1 min ; repli `Mesure en pause, reprise automatique à venir` si le champ `measures_dose_available_in_s` manque (firmware antérieur à la v2.62.0) | `info` (bleu) | Attente post-reprise (plancher d'eau neuve ou gel du filtre capteur) — attendre |

- **Nouvelle variante CSS `.sensor-badge--info`** (fond bleu `--accent-light`, texte `#1e40af`, contraste AA) : distingue visuellement l'**état normal** (bleu = attendre) de l'**anomalie** (rouge « Capteur indisponible » = agir).
- **v2.34.0 à v2.61.x (feature-091) — historique.** La purge se terminait alors sur la **stabilité des mesures** ([ADR-0059](../adr/0059-purge-adaptative-fenetre-de-medianes.md)), sa fin n'étant pas datée à l'avance : le libellé disait « Stabilisation depuis N min », et une re-purge (mesure redevenue instable après ouverture) « Re-stabilisation depuis N min ».
- **v2.62.0 (feature-139) — retrait de l'analyse de stabilité** ([ADR-0092](../adr/0092-retrait-analyse-stabilite-porte-mesures.md), supersède l'ADR-0059 ci-dessus). Le vocabulaire « stabilisation » / « purge » / « représentativité » a été jugé **incompréhensible** par l'utilisateur ; le texte est déterministe et annonce désormais **quand** l'attente se termine (`_measuresWaitText()`, app.js), à partir du **seul** champ `measures_dose_available_in_s` publié par le firmware — **aucune** constante de plancher n'est dupliquée côté client (c'est exactement cette duplication qui avait produit un calcul faux d'un facteur 10 au moment de l'extraction de ce lot). Toujours **recalculé à chaque push WS**, aucun tick JS.
- ⚠️ Le motif de fin `2` (**ancien plafond de 90 min**, retiré en v2.62.0) n'apparaît **plus jamais** : le champ `measures_purge_last_end` ne le porte plus. Les bandeaux « Stabilisation des mesures non atteinte » des pages [pH](page-ph.md) et [ORP](page-orp.md) sont **retirés** en conséquence ; la ligne « Dernière purge » des [Réglages](page-settings.md) conserve une lecture défensive de ce code (firmware ancien ou rollback) sans jamais plus le déclencher en usage normal.
- **Valeurs « `--` »** : `#compact-ph` / `#compact-orp` / `#compact-temperature` (et les valeurs des pages pH/ORP, la carte température) affichent `--` quand `measuresAreValid()` est faux — la ligne « Cible » reste affichée.
- **Trous dans les courbes** : les points d'historique avec pH/ORP/température `null` sont **poussés `null`** dans les séries uPlot (lever de crayon = trou visible), y compris sur le chemin incrémental (`appendMiniChart`) et sur l'agrégation par jour (un jour sans mesure valide reste dans la série avec `null`, au lieu d'être sauté). La série température a rejoint ce comportement en itération 2 (mini-charts + page historique) — elle en était exclue en itération 1. Le point « Aujourd'hui » injecté depuis le temps réel n'est pas alimenté par une mesure invalide (la dernière mesure valide du jour reste).
- **Dé-duplication avec le statut dosage** (`dedupDosingReason()`, app.js) : quand le badge affiche déjà « Filtration arrêtée » / « Signal filtration périmé », le motif équivalent du `dosing-status` est masqué — le badge fait foi. Les autres motifs (stabilisation pompe, pause mélange, limites…) sont inchangés.
- **Rétrocompat** : champs absents (firmware < 2.13.0) → `measuresAreValid()` retourne `true`, rien ne change ; pas de badge avant le premier message WS (pas de faux positif au boot).
- Périmètre : la page **Calibration** et l'onglet **Avancé** ne sont **pas** gatés (lectures live nécessaires — sonde hors de l'eau / diagnostic). MQTT/HA inchangé.

Le statut `#ph-dosing-status` / `#orp-dosing-status` de chaque carte (`getDosingStopReason()`, app.js) affiche notamment « Stabilisation : X min YY s » pendant la stabilisation post-calibration. Depuis v2.9.0, il consomme les champs **par pompe** `ph_stab_remaining_s` / `orp_stab_remaining_s` (feature-006) avec repli sur le champ global `stabilization_remaining_s` (compatibilité firmware plus ancien) : chaque carte affiche la stabilisation de **sa** pompe — une calibration ORP n'affiche plus « Stabilisation » sur la carte pH, et inversement.

Depuis **v2.19.1**, le statut affiche aussi la **pause mélange hydraulique** post-injection (« Pause mélange : X min YY s ») via `ph_mix_remaining_s` / `orp_mix_remaining_s` (repli sur le booléen `ph/orp MixingDelayActive` → « Pause mélange en cours » sans compte à rebours). Avant cette version, la pause mélange n'était visible que dans le modal « État filtre » des pages pH/ORP, d'où une impression de dosage bloqué sans explication sur le tableau de bord. Ordre d'affichage dans `getDosingStopReason()` : régulation désactivée → stabilisation → **pause mélange** → présence d'eau → limites → **cause de refus firmware**.

> **v2.25.1 — la ligne de statut se tait quand le badge capteur fait déjà foi.** Quand `measures_valid === false`, le badge capteur affiche déjà « Reprise automatique dans N min » (« Stabilisation depuis N min » avant la v2.62.0) ; descendre jusqu'à la cause de refus firmware juxtaposait **deux messages contradictoires** sur la même carte. Le statut est donc masqué — mais **uniquement** pour la famille « stabilisation des mesures » (codes de refus marqués `stab` : préchauffage du filtre, mesures non représentatives). Une **panne** (sonde figée, sonde muette) ou tout autre motif reste dit, badge ou pas. La cause de refus est lue depuis `ph/orpDoseBlockedCode` (code machine) et non plus par analyse du texte français — même mécanisme que le chip d'état filtre, voir [page-ph.md](page-ph.md#chip-détat-filtre-feature-025) et [ADR-0049](../adr/0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md).

## Statut dosage des cartes pH/ORP — commandé vs effectif (v2.21.1)

> Itération 3 de bug-dosage-fantome ([ADR-0045](../adr/0045-etat-impulsion-expose-en-calendrier.md)). Avant : `data/app.js` faisait un **OU** de `ph_dosing` (session — l'intention) et `ph_inject_remaining_s` (manuel minuté) → la carte affichait « Injection en cours » pendant les **repos** du train d'impulsions ([dosage en proportionnel de temps](../subsystems/pump-controller.md#dosage-en-proportionnel-de-temps-itération-2-v2210)), alors que la pompe ne tournait pas. C'est cette conflation qui a masqué le dosage fantôme d'origine.

`updateDosingStatus(sensor, isActive, stopReason)` (app.js) rend le statut `#ph-dosing-status` / `#orp-dosing-status` selon **6 priorités décroissantes** :

| # | Condition | Affichage | Dot | Classe |
|---|-----------|-----------|-----|--------|
| 1 | Injection **manuelle** minutée (`*_inject_remaining_s` ancré > 0) | « Injection manuelle : Ns » (compte à rebours) | animé | `is-active` |
| 2 | Session ouverte **et** `phase < pulse_ms` — la pompe **TOURNE** | « Injection en cours » | animé | `is-active` |
| 3 | Session ouverte, `pulse_ms > 0`, phase ≥ pulse_ms (repos entre deux pouls) | « Prochaine impulsion : Ns » (décompte jusqu'à la fin de période) ; à l'échéance sans nouvelle donnée → « Prochaine impulsion… » | **statique** | `is-waiting` |
| 4 | Session ouverte, `pulse_ms == 0` (deadband — période sans pouls planifié) | « Dosage en attente : Ns » / « Dosage en attente… » | statique | `is-waiting` |
| 5 | Motifs existants (`getDosingStopReason()` : Stabilisation, Pause mélange, limites…) | texte du motif | aucun | — |
| 6 | Rien à afficher | vide (masqué) | — | — |

**Sémantique du dot** : dot **présent** = une séance de dosage est ouverte ; dot **animé** (pulsation) = la pompe est réellement alimentée ; dot **statique** gris (`.is-waiting`, app.css) = silence **volontaire** du train d'impulsions — la distinction visuelle entre « commandé » et « effectif ».

**Honnêteté du compte à rebours** : les phases firmware sont **relatives** au début de période — à chaque trame (WS 5 s ou `/data`), le client les **ancre** sur sa propre horloge (`dosingAnchor`, app.js) et anime à la seconde (un seul `setInterval` 1 s partagé pH+ORP, actif uniquement quand un décompte est affiché). Précision ±1-2 s (jitter de push + arrondi), resynchronisée à chaque trame. À l'échéance de la période **sans donnée fraîche** → ellipsis (« Prochaine impulsion… »), **jamais** de bascule optimiste vers « Injection en cours », jamais 0 s ni de valeur négative. La seule bascule locale autorisée est **RESTRICTIVE** : impulsion → attente quand la phase ancrée dépasse `pulse_ms` (la pompe s'est arrêtée localement, sans attendre la trame suivante) — le sens inverse exige une donnée serveur.

**Rétrocompat** : champs `pulse_*` absents (firmware < 2.21.1) → comportement historique (`ph/orp_dosing` seul ⇒ « Injection en cours »).

Champs consommés : `pulse_period_ms`, `ph/orp_pulse_session`, `ph/orp_pulse_ms`, `ph/orp_pulse_phase_ms` — contrat identique WS / `GET /data`, voir [ws-manager.md](../subsystems/ws-manager.md#champs-sensor_data-ajoutés-en-v2211--calendrier-dimpulsion-adr-0045).

## Données consommées (WebSocket `/ws`)

La carte pH lit :
- `ph`, `ph_target`, `ph_dosing`, `ph_cal_valid`
- Badge multi-états : `ph_limit_reached`, `ph_daily_ml`, `ph_remaining_ml`, `ph_alert_threshold_ml` (+ `max_ph_ml_per_day` de la config)
- Stabilisation : `ph_stab_remaining_s` (repli `stabilization_remaining_s`)
- Pause mélange : `ph_mix_remaining_s` (repli booléen `phMixingDelayActive`)
- Statut dosage v2.21.1 : `pulse_period_ms`, `ph_pulse_session`, `ph_pulse_ms`, `ph_pulse_phase_ms`, `ph_inject_remaining_s` (voir [section dédiée](#statut-dosage-des-cartes-phorp--commandé-vs-effectif-v2211))

La carte ORP lit :
- `orp`, `orp_target`, `orp_dosing`, `orp_cal_valid`
- Badge multi-états : `orp_limit_reached`, `orp_daily_ml`, `orp_remaining_ml`, `orp_alert_threshold_ml` (+ `max_chlorine_ml_per_day` de la config)
- Stabilisation : `orp_stab_remaining_s` (repli `stabilization_remaining_s`)
- Pause mélange : `orp_mix_remaining_s` (repli booléen `orpMixingDelayActive`)
- Statut dosage v2.21.1 : `pulse_period_ms`, `orp_pulse_session`, `orp_pulse_ms`, `orp_pulse_phase_ms`, `orp_inject_remaining_s`

Les cartes pH et ORP lisent aussi (feature-061 ; **v2.34.0** feature-091 ; **v2.62.0** feature-139) :
- `water_present`, `measures_valid`, `measures_stab_elapsed_s`, `measures_dose_available_in_s`, `filtration_state_stale` — segments d'état mesure + gate « Capteur indisponible » / « hors plage » + valeurs « `--` ». ⚠️ `measures_stab_remaining_s` **n'existe plus** depuis la v2.34.0 ; `measures_purge_count` **n'existe plus** depuis la v2.62.0.

La bannière de tête lit (v2.20.3) :
- `phFilterAlarm`, `orpFilterAlarm` — voir [Bannière « Régulation à l'arrêt »](#bannière--régulation-à-larrêt--v2203)

La carte Température lit :
- `temperature`
- `water_present`, `measures_valid`, `measures_stab_elapsed_s`, `measures_dose_available_in_s`, `filtration_state_stale` — segments d'état mesure + gate « Capteur indisponible » + valeur « `--` » (feature-061 itération 2 ; champs de purge renommés en v2.34.0, `measures_purge_count` retiré en v2.62.0)

La carte Filtration lit :
- `filtration_running`, `filtration_mode`, `filtration_start`, `filtration_end`
- Flag d'override optimiste côté JS : `filtrationRunningOverride` (voir mémoire projet)

La carte Éclairage lit :
- `lighting_enabled`, `lighting_schedule_enabled`, `lighting_start`, `lighting_end`

Le sous-bloc Boost (carte Filtration depuis feature-147, v2.64.0) lit :
- `boost_active` (bool), `boost_until` (epoch d'expiration) — voir [Carte Modes temporaires](#carte-modes-temporaires-feature-140)
- `boost_filtration_extended`, `boost_chlorine_boosted` (bool, feature-055) — leviers réellement actifs du Boost, pilotent sa ligne « Effet »

Le bandeau dédié de Chloration de secours (hors carte, en tête de page) lit :
- `rescue_until` (epoch, `0` si inactif), `rescue_dosing` (bool, verdict dérivé « dose réellement maintenant »), `rescue_lift` (0-3, raison de fin récente) — Chloration de secours (feature-140)

Les mini-graphiques consomment `GET /get-history?range=24h` au chargement, puis se rafraîchissent par **polling HTTP incrémental toutes les 5 minutes** (`GET /get-history?range=24h&since=<dernier timestamp>`, ajout des nouveaux points en fin de série, avec insertion de la coupure > 90 min le cas échéant) — pas via WebSocket (`loadMiniChartData`, app.js). Depuis la **v2.18.9**, la réponse est **filtrée à minuit local** côté navigateur (seule la journée en cours est tracée) et un **changement de jour civil force un rechargement complet** au lieu de l'incrément — voir [Plage « journée en cours »](#plage-des-mini-graphiques--journée-en-cours-v2189). Le paramètre `range=24h` est conservé tel quel : le contrat HTTP est inchangé. Depuis la **v2.38.0**, la réponse porte en plus `phFiltered` / `orpFiltered` (mesure lissée, `null` au-delà de ~8 h) — voir [Mini-graphiques pH / ORP](#mini-graphiques-ph--orp--la-courbe-suit-le-nombre-affiché-feature-095-v2380).

## Carte Modes temporaires (feature-140)

⚠️ **Depuis la v2.64.0 (feature-147), ce n'est plus une carte.** La carte `#dashboard-temp-modes-card` décrite ci-dessous est **supprimée** ; son unique contenu, le sous-bloc `#temp-mode-boost`, vit désormais dans la carte **Filtration** (`#dashboard-filtration-card`) — voir [Carte Filtration — quatre boutons, mode vs état, Boost intégré](#carte-filtration--quatre-boutons-mode-vs-état-boost-intégré-feature-147-v2640). **Markup, ids et comportement du Boost strictement inchangés**, seul l'emplacement a bougé : le reste de cette section (narration historique de la Chloration de secours, retirée de la carte puis du bandeau dédié) **reste exact** et n'est pas affecté par ce déménagement.

La carte **Modes temporaires** (`#dashboard-temp-modes-card`, v2.55.0 — remplace l'ex-carte Boost, même emplacement dans la grille) ne contenait, depuis la **v2.55.3** et jusqu'à sa suppression en v2.64.0, que le sous-bloc `#temp-mode-boost` (Mode Boost, feature-053, **markup et ids inchangés**) — et rien d'autre. L'état et le contrôle de la **Chloration de secours** (`#temp-mode-rescue`, introduit v2.55.0, masqué conditionnellement v2.55.1) avaient déjà été **retirés de la carte** en v2.55.2, déplacés vers un [bandeau dédié en tête de page](#bandeau--chloration-de-secours-active-rescue-active-banner-feature-140-v2552). La **ligne d'effet combiné** (`#temp-modes-effect`) et la **suggestion Boost** (`#temp-modes-suggest-boost`) qui restaient dans la carte sont à leur tour **retirées du DOM en v2.55.3** — voir [Ligne d'effet combiné, retirée en v2.55.3](#ligne-deffet-combiné-b2-feature-140--retirée-en-v2553) pour le détail et la raison de ce retrait (l'information gagne à être **scindée**, pas supprimée). La non-exclusivité Boost / secours reste réelle côté firmware — en Programmée (NVS ou via la Chloration de secours), le Boost perd sa surchloration mais conserve sa **prolongation de filtration**, utile précisément pour faire circuler une eau chargée en matière organique — mais ne fait plus l'objet d'un affichage combiné : chaque effet est désormais dit **à côté de l'objet qu'il décrit** (le Boost dans la carte, le secours dans son bandeau). Voir [ADR-0087](../adr/0087-ce-qu-un-mode-temporaire-a-le-droit-de-faire.md) pour la décision structurante derrière cette non-exclusivité et le couple mode effectif / mode publié qui la rend honnête.

### Sous-bloc Boost (feature-053)

Permet d'activer d'un geste le **Mode Boost** : surchloration temporaire du jour (filtration prolongée + cible ORP relevée) après une forte fréquentation. Décision et bornes de sécurité : [ADR-0025](../adr/0025-mode-boost.md).

- **Boost inactif** : bouton **Activer le Boost**. Au clic → `POST /boost/start`. À l'activation, un **toast adaptatif** (v2.18.1, feature-054) informe selon les leviers réellement actifs, à partir des booléens `filtration_extended` / `chlorine_boosted` renvoyés par la réponse 200 :

  | `filtration_extended` | `chlorine_boosted` | Toast |
  |---|---|---|
  | ✅ | ✅ | *info* — « Boost activé — vérifiez le taux de chlore avant la baignade. » (message nominal, le contrôleur ne mesure pas le chlore libre ppm) |
  | ❌ | ✅ | *info* — « Boost activé (surchloration seule) : la filtration n'est pas gérée par PoolController. Vérifiez le taux de chlore avant la baignade. » |
  | ✅ | ❌ | *warning* — « Boost activé (filtration prolongée seule) : la régulation ORP n'est pas en mode Automatique, le chlore n'est pas relevé. » |
  | ❌ | ❌ | *warning* — « Boost sans effet : filtration non gérée et régulation ORP non automatique. » |
- **Boost actif** (`boost_active === true`) : la carte affiche l'**heure d'expiration** dérivée de `boost_until` (prochain minuit local), une **ligne « Effet » persistante** (`#detail-boost-effect-row` / `#detail-boost-effect`, feature-055, v2.18.2 — voir ci-dessous) et un bouton **Arrêter le Boost** → `POST /boost/stop`.

**Ligne « Effet » persistante (feature-055)** — complément **persistant** du toast fugace de la feature-054 (le toast n'apparaît qu'au clic UI, jamais lors d'une activation depuis Home Assistant, et disparaît au rechargement). La ligne indique en permanence les leviers réellement actifs, à partir des booléens WS `boost_filtration_extended` / `boost_chlorine_boosted` (calculés au vol, donc valides après activation HA **et** après reload — voir [ws-manager.md §feature-055](../subsystems/ws-manager.md#champs-sensor_data-ajoutés-en-feature-055-effet-boost-persistant-v2182)) :

  | `boost_filtration_extended` | `boost_chlorine_boosted` | Libellé « Effet » | Badge |
  |---|---|---|---|
  | ✅ | ✅ | « Filtration prolongée + surchloration » | `--ok` |
  | ❌ | ✅ | « Surchloration seule » | `--ok` |
  | ✅ | ❌ | « Filtration prolongée seule » | `--ok` |
  | ❌ | ❌ | « Sans effet (filtration non gérée, ORP non automatique) » | `--warn` (orange, attire l'œil) |

  Quand le Boost est **inactif**, la ligne « Effet » est masquée (badge de carte `--off`). Au **clic sur Activer**, `updateBoostCard()` mappe `filtration_extended` / `chlorine_boosted` de la réponse `POST /boost/start` vers `latestSensorData.boost_filtration_extended` / `boost_chlorine_boosted` pour un affichage immédiat, sans attendre le prochain push WS (~5 s).

  **Texte d'aide cohérent (`#detail-boost-hint`, v2.18.3)** — le hint sous le widget ne promet plus systématiquement l'effet nominal. Quand le Boost est **actif**, il est masqué (la ligne « Effet » fait foi, plus de contradiction). Quand il est **inactif**, il devient **prospectif** (« À l'activation : … ») et reflète les mêmes booléens WS que la ligne « Effet » : filtration prolongée + cible ORP relevée / cible ORP relevée seule / filtration prolongée seule / aucun effet.
- **Expiration automatique** : le firmware désactive le Boost à minuit local et pousse `boost_active: false` en WS → la carte repasse à l'état inactif sous ≤ 5 s, sans reload.
- **Heure non synchronisée** : `POST /boost/start` renvoie `409 time_not_synced` (l'expiration à minuit serait incalculable) → l'UI affiche un message d'indisponibilité et ne bascule pas la carte.

> **Effet chlore gaté au mode ORP `automatic`** : en mode de régulation ORP Manuel / Programmé, le Boost n'étend que la filtration (la cible et la limite chlore ne sont pas relevées). Comportement porté par le firmware ([pump-controller.md §Mode Boost](../subsystems/pump-controller.md#mode-boost-feature-053)). De même, si la filtration n'est pas gérée par PoolController, le Boost ne prolonge pas la filtration. Le toast adaptatif ci-dessus signale ces cas ; côté firmware, `startBoost()` logue un `warning` par levier inerte (couvre aussi l'activation via Home Assistant).

### Sous-bloc Chloration de secours — RETIRÉ en v2.55.2, voir le bandeau dédié

Depuis la **v2.55.2**, le widget « Modes temporaires » **ne contient plus aucune trace** de la Chloration de secours : le sous-bloc `#temp-mode-rescue` (badge d'état, échéance, callout de cause, bouton Arrêter) est **retiré du DOM** dans tous les états, actif compris — ce n'est plus un masquage conditionnel (comme en v2.55.1), c'est un retrait complet. L'état et le contrôle du mode vivent désormais dans un **bandeau dédié**, `#rescue-active-banner`, en tête de page — voir [Bandeau « Chloration de secours active »](#bandeau--chloration-de-secours-active-rescue-active-banner-feature-140-v2552) plus haut, qui documente les deux textes opposables, les conditions du gate `pool-chemistry` et la fermeture de l'offre L7 sur `#dose-block-banner` quand le secours tourne déjà.

Le widget ne contient plus que le sous-bloc **Boost** (ci-dessus) — depuis la **v2.55.3**, la [ligne d'effet combiné](#ligne-deffet-combiné-b2-feature-140--retirée-en-v2553) (`#temp-modes-effect`) a elle aussi été retirée du DOM : seul le Boost subsiste dans la carte. Surcouche **non destructive** du mode Programmé existant (feature-011), verdict sécurité chimique et libellés imposés mot à mot : voir la spec [feature-140](../../specs/features/done/feature-140-chloration-de-secours-et-widget-des-modes-temporaires.md) et [ADR-0087](../adr/0087-ce-qu-un-mode-temporaire-a-le-droit-de-faire.md). Détail firmware : [pump-controller.md §Chloration de secours](../subsystems/pump-controller.md#chloration-de-secours-feature-140).

- **Mode ORP Manuel** : le refus **L8** reste affiché par le bandeau de blocage à la place du bouton « Activer la chloration de secours » (voir [Chaînage](#chaînage-depuis-le-bandeau--dosage-bloqué--b4-feature-140) ci-dessous) : *« Chloration de secours indisponible : la régulation du chlore est en mode Manuel. Repassez en Automatique ou Programmée pour l'utiliser. »* Même refus côté firmware (409 `mode_manual`) si l'activation était contournée (MQTT/HA).
- **Levée automatique, expiration, arrêt manuel** : trois fins distinctes, publiées par `rescue_lift` (0-3) avec une **fenêtre de visibilité de 90 s** — un `showToast` par événement, **une seule fois** (`_rescueLastAnnouncedLift`), indépendant de la visibilité du bandeau :
  - `1` (levée auto, 30 min de mesures ORP de nouveau fiables) → **L6**, ton `info` ;
  - `2` (expiration des 48 h) → **L9**, ton `warning` — rappelle de contrôler le chlore libre au réactif DPD **et** le pH avant de faire confiance à la mesure, la régulation automatique ayant **déjà** repris ;
  - `3` (arrêt manuel) → **aucun toast** (l'utilisateur vient d'agir).

L'**échéance** (format `JJ/MM à HH:MM`, dérivée de `rescue_until`) et la **cause de non-injection** (L3/L4/L5a/L5b) sont désormais rendues dans le texte du bandeau `#rescue-active-banner` plutôt que dans des éléments séparés (`#detail-rescue-until`, `#rescue-reason-callout` : supprimés) — voir le détail des deux variantes de texte dans la section du bandeau.

### Ligne d'effet combiné (B2, feature-140) — retirée en v2.55.3

De la v2.55.0 à la v2.55.2, le widget portait une ligne `#temp-modes-effect-row` / `#temp-modes-effect` qui étendait le patron honnête de feature-055 aux cas combinés Boost × Secours, ainsi qu'un callout `#temp-modes-suggest-boost` suggérant d'activer le Boost quand le secours était armé sans filtration prolongée.

**Les deux sont retirés du DOM en v2.55.3**, avec leurs fonctions JS (`_tempModesEffectText()`, `updateTempModesEffect()`). ⚠️ **Point à retenir** : le gate `pool-chemistry` n'a **pas** demandé de reloger la ligne d'effet combiné ailleurs — son argument est que l'information est **meilleure une fois scindée** : la ligne d'effet du Boost (`#detail-boost-effect`, feature-055) reste à côté du Boost, et les deux variantes A/B du [bandeau de secours](#bandeau--chloration-de-secours-active-rescue-active-banner-feature-140-v2552) disent ce que fait le secours à côté du secours. **Chacune est vérifiable à côté de l'objet qu'elle décrit** — ce qu'une ligne combinée ne permettait pas.

La suggestion Boost, elle, **n'a pas disparu** : le gate a **refusé** sa suppression et l'a fait **déménager** dans le bandeau de secours, en phrase ajoutée aux variantes A et B (jamais un bloc séparé) — voir la [paire de textes opposables](#bandeau--chloration-de-secours-active-rescue-active-banner-feature-140-v2552). Motif du gate : c'est le **seul endroit de l'application qui dit que la filtration prime sur le chlore**, enseignement direct du terrain (2 L/jour pendant trois jours, ORP bloqué à 261 mV pour une consigne de 530 mV) et contre-pied du réflexe « ça ne monte pas, j'en remets ». Elle reste **conditionnelle**, inchangé depuis sa première version : affichée uniquement quand le secours est armé et la filtration non déjà prolongée.

Pour mémoire, les **8 combinaisons** qu'affichait la ligne retirée (`chlorBoost` et `rescueDosing` étaient **exclusifs par construction firmware** — `boost_chlorine_boosted` devient naturellement faux dès que le mode ORP effectif n'est plus `automatic`, donc déjà faux quand le secours est armé) :

| Filtration prolongée | Surchloration (Boost) | Chloration de secours (dose) | Texte | Badge |
|---|---|---|---|---|
| ✗ | ✗ | ✗ | « Sans effet (filtration non gérée, ORP non automatique) » *(inchangé feature-055)* | `--warn` |
| ✓ | ✗ | ✗ | « Filtration prolongée seule » | `--ok` |
| ✗ | ✓ | ✗ | « Surchloration seule » | `--ok` |
| ✓ | ✓ | ✗ | « Filtration prolongée + surchloration » | `--ok` |
| ✗ | ✗ | ✓ | « Chloration de secours seule » | `--ok` |
| ✓ | ✗ | ✓ | « Filtration prolongée + chloration de secours » | `--ok` |
| — | ✓ | ✓ | **impossible** par construction — non codé, `console.warn` si jamais observé (ne plante pas) | — |

Les causes « armé mais non dosant » (L3/L4/L5) **n'entraient pas** dans cette ligne — elles restaient dans le callout du sous-bloc secours (lui-même retiré en v2.55.2, remplacé par la variante B du bandeau).

### Chaînage depuis le bandeau « Dosage bloqué » (B4, feature-140)

Sur la **ligne ORP uniquement** du bandeau de blocage (voir [section dédiée](#bandeau--dosage-bloqué--feature-138-v2540)) — le secours ne concerne jamais le pH — un bouton `btn--primary` « Activer la chloration de secours » (libellé **L7**) est placé **avant** le bouton secondaire « Ouvrir l'injection manuelle » existant. Si le mode ORP effectif est `manual`, le bouton est remplacé par le texte **L8** (pas de bouton proposé pour une action refusée).

> ⚠️ **Correctif gate `pool-chemistry` (2ᵉ passage, 2026-10-08, feature-139) — l'offre de secours RÉTRÉCIT, le raccourci d'injection manuelle N'EST PAS touché.** Avant ce lot, l'offre L7/L8 et le bouton « Ouvrir l'injection manuelle » (B3) partageaient la **même** liste blanche de causes `[3,4,15,18,19,22]` (`DOSE_BLOCK_SHORTCUT_CODES`). Les codes **18** (`ReadingNaNGateClosed`) et **22** (`FilterGateClosed`) — l'attente avant la première mesure jugée fiable après une coupure d'eau — sont désormais **retirés de l'offre de secours seule** (hint L7 et bouton). Motif chimique : cette attente est maintenant **bornée** (≤ 10 min, plus de plafond à 90 min) **et annoncée à l'écran** en clair (« Reprise automatique dans N min ») depuis le retrait de l'analyse de stabilité ([ADR-0092](../adr/0092-retrait-analyse-stabilite-porte-mesures.md)) : proposer un volume fixe de chlore pendant une attente déjà bornée et expliquée inviterait au surdosage manuel pour rien. **Le secours reste trouvable sur les vraies pannes de mesure** : codes **3** (mesure filtrée NaN), **4** (filtre non prêt), **15** et **19** (fenêtre de lissage en remplissage). Le bouton « Ouvrir l'injection manuelle » (B3), lui, **garde la liste complète** `[3,4,15,18,19,22]` — le gate distingue explicitement les deux usages : un volume fixe de secours n'est pas la même décision qu'ouvrir l'écran d'injection manuelle pour juger soi-même.

> **v2.55.2 — fermeture d'une incohérence** : si le secours tourne **déjà**
> (`rescue_until > 0`, paramètre `rescueActive` de `_renderDoseBlockRow()`),
> le bouton L7 **n'est pas rendu du tout** — proposer d'armer un mode qui
> tourne déjà est le genre d'incohérence qui fait douter de tout le reste de
> l'affichage. Le texte de cause et le bouton « Ouvrir l'injection manuelle »
> (B3) restent affichés : la mesure EST toujours non représentative pendant
> que le secours tourne, c'est le cas nominal, pas une anomalie à masquer.

> **v2.55.1 — c'est désormais le SEUL chemin d'activation du widget** (avec
> l'interrupteur MQTT/Home Assistant). Le bouton Activer a été retiré du
> sous-bloc `#temp-mode-rescue` : le clic ici déclenche **directement** la
> confirmation **L2** via `_activateRescueChlorine()`, au lieu de défiler
> jusqu'à la carte et d'y poser le focus (comportement v2.55.0, retiré). La
> confirmation **L2** reste un passage obligé dans tous les cas.

#### Chloration de secours : le mode Programmée reste l'accès permanent (v2.55.1)

Depuis que l'activation ne passe plus que par le bandeau de blocage, la
**chloration de secours n'est accessible QUE si l'appareil a lui-même
constaté un blocage** sur la voie ORP. **Hors blocage constaté**,
l'équivalent durable de la chloration de secours est le **mode
Programmée** de la régulation chlore (réglage permanent, page `/orp` —
feature-011) : un volume quotidien fixe, aveugle à l'ORP, exactement le
mécanisme que réutilise la chloration de secours en coulisses (voir
[pump-controller.md §Chloration de secours](../subsystems/pump-controller.md#chloration-de-secours-feature-140)).

⚠️ **Armer le secours de façon préventive n'a pas d'intérêt chimique** :
si les sondes sont jugées fiables, la régulation **Automatique** fait
mieux qu'un volume fixe aveugle — doser à l'aveugle quand la mesure est
bonne n'est qu'une perte de précision, jamais un gain. Le cas réellement
difficile — une sonde **dérivée mais stable** (donc jugée fiable par
l'appareil, donc **invisible** du bandeau) — ne se diagnostique **pas** en
armant le secours par précaution : il se diagnostique au **réactif DPD**
(mesure indépendante de la sonde) et se traite, le cas échéant, en
basculant la régulation chlore sur le mode **Programmée**.

### Volume saisi à l'activation (feature-141, v2.56.0)

Le volume journalier de la Chloration de secours n'est plus la constante `kRescueChlorineDailyMl` (1500 mL/j) : il est **saisi par l'utilisateur** dans le dialogue de confirmation ouvert par `_activateRescueChlorine()` (B4 ci-dessus), pré-rempli et borné. Arbitrage complet (chaîne de pré-remplissage, défauts par taille de bassin, deux clamps distincts) : [pump-controller.md §Chloration de secours](../subsystems/pump-controller.md#chloration-de-secours-feature-140), verdict `pool-chemistry` dans la spec [feature-141](../../specs/features/done/feature-141-volume-de-secours-saisi-a-l-activation.md).

- **Champ** : libellé imposé « Volume de chlore par jour (mL) », `<input type="number">` **pré-rempli** depuis `cfg.rescue_ml` (déjà résolu côté serveur — jamais `0`, repli local `1500` si absent sur un firmware ancien). Aide sous le champ, imposée mot à mot : « Pré-rempli d'après votre configuration. En cas de doute, laissez la valeur proposée. » — l'utilisateur **valide sans rien toucher** par construction.
- **Bornes de CONFORT du champ** (`_rescueVolumeInputMax(cfg)`) : `min = min(100, limite utilisateur)`, `max = min(2000, limite utilisateur si > 0)`. Ce ne sont **pas** des gardes — le clamp qui fait foi est **serveur** (`/rescue/start?ml=`), ces bornes ne font qu'éviter une incohérence visuelle évidente (ex. `min=100 max=60` si la limite de l'utilisateur est sous le plancher).
- **Avertissement B2**, révélé dynamiquement dès que la saisie dépasse **1500 mL**, imposé mot à mot : « Au-delà de 1500 mL par jour, le bénéfice n'est pas démontré : quand l'eau est chargée en matière organique, l'ORP ne remonte pas tant que le point de rupture n'est pas atteint — augmenter le volume n'y change rien. » 1500 reste le seuil d'affichage de cet avertissement, mais n'est plus un plafond — une saisie supérieure est **acceptée**, les vraies bornes restent celles ci-dessus.
- **L2 révisé** (la mention « plafonné à 1500 mL » n'a plus de sens, remplacée) : « Le dosage se fera à l'aveugle, sans contrôle de l'ORP, pendant 48 h au maximum. Le volume indiqué ci-dessous sera réparti sur la journée, sans jamais dépasser votre limite journalière. Le pH n'est jamais dosé dans ce mode. Il maintient une désinfection minimale — il ne traite pas la cause : quand l'eau est chargée en matière organique, seul le retrait mécanique des dépôts (brossage, aspiration, nettoyage du filtre) fait baisser cette charge. Désactivez-le dès que les sondes sont de nouveau fiables. »
- **Soumission** : `POST /rescue/start?ml=<saisie>` (omis si la saisie n'est pas un entier positif — le serveur résout alors lui-même la chaîne de pré-remplissage). La réponse porte `rescue_ml`, le volume **réellement armé** après clamp serveur.
- **B9 — un clamp n'est jamais silencieux** : si `rescue_ml` renvoyé diffère de la saisie, un toast `warning` l'annonce : « Volume ramené à `<N>` mL par vos limites de sécurité (valeur saisie : `<M>` mL). » Sinon, toast `info` habituel « Chloration de secours activée. »
- **Fallback navigateur sans `<dialog>`** (`window.confirm()` natif) : pas de champ éditable possible — le message affiche le volume proposé en clair (« Volume proposé : `<N>` mL. ») et c'est cette valeur pré-remplie qui est envoyée telle quelle.

**Correctif du bandeau actif et de L5a/L5b (B6)** : `_rescueEffectiveDailyMl(cfg)` lisait `let vol = 1500;` en dur — exact tant que le volume armé valait exactement 1500, **faux** dès qu'il devient variable (bassin « petit », ou saisie > 1500). La fonction lit désormais `cfg.rescue_ml` (parité stricte `/get-config` ↔ WS `config`, voir [API.md](../API.md#get-get-config--write)), avec repli `1500` **uniquement** si le champ est absent (rétrocompatibilité firmware ancien). Les libellés L5a/L5b restent **imposés mot à mot** ; seuls le seuil de bascule entre les deux et le volume affiché (`%s`) sont désormais dérivés de ce volume réel, jamais la constante.

## Actions

| Bouton | Endpoint HTTP |
|--------|--------------|
| `#filtration-mode-btn-auto` / `-manual` / `-force` / `-off` | `POST /save-config` `{filtration_mode: "auto"\|"manual"\|"force"\|"off"}` — grille 2×2, **remplace** les anciens `#detail-filtration-start`/`#detail-filtration-stop` (feature-147, v2.64.0 ; ceux-ci ne pilotaient que Marche/Arrêt, feature-146) |
| `#detail-lighting-on` | `POST /lighting/on` |
| `#detail-lighting-off` | `POST /lighting/off` |
| Boost — Activer | `POST /boost/start` (toast rappel baignade) |
| Boost — Arrêter | `POST /boost/stop` |
| Chloration de secours — Activer (depuis le bandeau « Dosage bloqué » **ou** MQTT/HA — plus de bouton dans le widget depuis v2.55.1, et l'offre disparaît du bandeau si le secours tourne déjà depuis v2.55.2) | `POST /rescue/start?ml=<volume saisi>` (confirmation L2 préalable, champ de volume — [feature-141](#volume-saisi-à-lactivation-feature-141-v2560), v2.56.0) |
| Chloration de secours — Arrêter (`#rescue-active-banner-stop`, dans le bandeau `#rescue-active-banner` depuis v2.55.2 — était `#detail-rescue-stop` dans le widget) | `POST /rescue/stop` |
| Liens engrenage | Navigation SPA (fragment `#/ph`, etc.) |

La fonction `updateFiltrationBadges()` côté JS met à jour simultanément `#detail-filtration-status` (dashboard) et `#filtration-current-status` (page filtration) pour rester cohérent.

## Cas limites

- **WebSocket déconnecté** : les valeurs affichent `--`. Les mini-graphiques conservent la dernière série.
- **Capteur pH ou ORP en erreur** : segment « Capteur indisponible » (danger, priorité 0) dans le badge multi-états (voir [Badge multi-états](#badge-multi-états-des-cartes-ph--orp-ph-sensor-badge-orp-sensor-badge)), valeur `--` (voir [Convention « `--` »](#convention-----quand-un-capteur-est-indisponible-v2105)).
- **Sonde de température en erreur** (v2.10.5) : badge « Capteur indisponible » (`#temperature-sensor-badge`), valeur `--` partout où la température s'affiche.
- **Sonde de température figée** (v2.47.1, feature-068 ; fenêtre 24 h depuis v2.56.1, bug-142) : badge orange « Sonde température figée », valeur **conservée** (elle est plausible, seulement immobile) — aucun dosage n'est modifié, mais le planning automatique de filtration n'est plus recalculé pendant l'épisode. Précédé par « Capteur indisponible » et par « Sonde eau non identifiée ». Voir [`docs/subsystems/sensors.md`](../subsystems/sensors.md#exposition-de-la-sonde-de-température-figée--feature-068).
- **Filtration arrêtée / attente post-reprise** (feature-061) : valeurs pH/ORP et — depuis l'itération 2 — température à `--`, badge bleu « Filtration arrêtée » ou « Reprise automatique dans N min » sur les trois cartes (voir [section dédiée](#mesures-phorp-non-représentatives--filtration-arrêtée--stabilisation-feature-061-v2130)).
- **Sonde non calibrée** : bandeau `calibration-alert` + chip global en haut de page.
- **Dosage en cours** : badge `dosing-status` affiché dans la carte concernée (motif filtration masqué si le badge capteur le porte déjà — dé-duplication feature-061).

Le détail complet du cumul journalier (barre de progression, mL consommés / quota) reste sur les pages `/ph` et `/orp` ; le dashboard n'en montre que le segment « Cumul XX % » à partir de 75 %.

## Fichiers

- [`data/index.html:162`](../../data/index.html:162) — structure HTML du dashboard
- [`data/app.js`](../../data/app.js) — `latestSensorData`, `updateFiltrationBadges()`, `composeSensorBadge()`, `dailyCumulSegment()`, `getDosingStopReason()`, `updateDosingStatus()` + `dosingAnchor` (v2.21.1), rendu uPlot (`createMiniLineChart`), préférences d'interface `uiPrefGet()` / `uiPrefSet()` + `bindMiniRawToggles()` / `refreshMiniLegendAbsence()` (v2.38.0) ; `updateFiltrationModeButtons()`, `phGaugePosition()`/`phGaugeStateText()`, `orpGaugePosition()`/`orpGaugeStateText()`, `updatePhGauge()`, `updateOrpGauge()` (feature-147, v2.64.0)
- [`data/uPlot.iife.min.js`](../../data/uPlot.iife.min.js), [`data/uPlot.min.css`](../../data/uPlot.min.css) — bibliothèque uPlot 1.6.32 (voir [BUILD.md](../BUILD.md#bibliothèque-de-graphiques-uplot-frontend))
- [`data/app.css`](../../data/app.css) — classes `.card--status`, `.compact-value`, `.dosing-status`, `.sensor-badge`, `.mini-legend*` (v2.38.0 — pastilles **neutres** à dessein : les traits sont colorés **par la valeur**, une pastille colorée mentirait sur la couleur réelle) ; `.rescue-active-banner`, `.rescue-active-banner--info` / `--warning` (v2.55.2, pas de variante `--critical`/`--fault` — voir [Bandeau « Chloration de secours active »](#bandeau--chloration-de-secours-active-rescue-active-banner-feature-140-v2552)) ; `.filtration-modes`, `.card__status-label--warn`, `.gauge*` (feature-147, v2.64.0)
- [`src/ws_manager.cpp`](../../src/ws_manager.cpp) — `broadcastSensorData()` pousse les champs consommés ici
- [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp) — `GET /data`, `GET /get-history`

## Specs historiques

- `feature-012-badges-dashboard-cumul-stabilisation` — segment « Cumul XX % », règle de composition du badge multi-états (max 2 segments), stabilisation par pompe (v2.9.0, périmètre réduit sur avis UX)
- `feature-022-observabilite-dosage` — segment « Capteur indisponible » (priorité 0, danger) piloté par `ph`/`orp` null dans le WS, garde anti-faux-positif au boot (v2.10.0)
- `bug-ui-valeurs-figees-capteur-absent` — convention « `--` » généralisée à tous les sites d'affichage numérique (fin du pattern « skip si null ») + badge « Capteur indisponible » température (v2.10.5)
- `bug-minicharts-ecretes-mobile` — graphiques étirés/écrêtés sur iPhone : règle CSS morte Chart.js supprimée, `ResizeObserver` durci (v2.18.3, voir note « Responsive / mobile » dans [Structure](#structure))
- `feature-068-sonde-temperature-eau-figee` — badge « Sonde température figée » + « Sonde eau non identifiée », précédence des trois états de la sonde eau, statu quo instrumenté ([ADR-0074](../adr/0074-temperature-figee-statu-quo-instrumente.md)) (v2.47.1)
- `feature-061-mesures-ph-orp-filtration-arretee` — mesures pH/ORP non représentatives sans circulation d'eau : badges info bleus, valeurs `--`, trous dans les courbes, dé-duplication dosing-status (v2.13.0) ; itération 2 : extension du même traitement à la température (carte/compact `--`, badges, courbe avec trous)
- `ux-minicharts-journee-en-cours` — mini-graphiques de l'accueil limités à la **journée en cours** (minuit local) au lieu des 24 h glissantes, filtrage navigateur avant calcul des coupures, rechargement complet au passage de minuit (v2.18.9, voir [section dédiée](#plage-des-mini-graphiques--journée-en-cours-v2189))
- `bug-dosage-fantome-duty-non-calibre` (itération 3) — statut dosage des cartes pH/ORP distinguant « commandé » (session) de « effectif » (pompe alimentée pendant un pouls) : 6 priorités d'affichage, compte à rebours de la prochaine impulsion ancré localement, dot animé/statique (v2.21.1, [ADR-0045](../adr/0045-etat-impulsion-expose-en-calendrier.md), voir [section dédiée](#statut-dosage-des-cartes-phorp--commandé-vs-effectif-v2211))
- `feature-095-historiser-la-mesure-filtree` — la **mesure lissée** (celle de la régulation) devient la courbe principale des mini-graphiques pH/ORP, le brut passe en courbe secondaire avec interrupteur mémorisé, frontière visuelle à ~8 h ; les graphiques d'historique des écrans détail restent **bruts** (v2.38.0, [ADR-0064](../adr/0064-la-courbe-suit-le-nombre-affiche.md), voir [section dédiée](#mini-graphiques-ph--orp--la-courbe-suit-le-nombre-affiché-feature-095-v2380))
- `bug-points-futurs-et-ux-courbes` — tooltip toujours daté (`jj/mm hh:mm`) et typé (mesure / moyenne horaire / moyenne journalière), mini-graphiques sur 24 h (était 3 j — plage resserrée à la journée en cours en v2.18.9), coupure de courbe > 90 min entre deux points (arrêts de filtration visibles, incrémental inclus) et jour calendaire manquant sur les écrans détail (v2.18.4, voir [section dédiée](#tooltip-datétypé-et-coupures-de-courbe-v2184-bug-points-futurs-et-ux-courbes)) ; côté firmware : garde anti-futur de l'historique ([history.md](../subsystems/history.md#garde-anti-futur--purge-des-points-futurs-v2184-bug-points-futurs-et-ux-courbes))
- `feature-147-widgets-du-tableau-de-bord-lire-avant-de-chercher` — carte Filtration : quatre boutons de mode remplacent la ligne « Mode » + Démarrer/Arrêter, en-tête distingue mode demandé et état réel de la pompe, Boost devient un sous-bloc de la carte (carte `#dashboard-temp-modes-card` supprimée) ; cartes pH/ORP : jauge de position s'ajoutant au mini-graphique, zones nommées pour le pH (bornes lues de la config, repères 7,00/7,60 non colorés), aucune zone nommée pour l'ORP — décision du gate `pool-chemistry` (v2.64.0, voir [section Carte Filtration](#carte-filtration--quatre-boutons-mode-vs-état-boost-intégré-feature-147-v2640) et [section Jauges](#jauges-de-position-ph--orp-feature-147-v2640))
