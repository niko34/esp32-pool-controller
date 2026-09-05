## Métadonnées
- **ID** : feature-003
- **Slug** : page-ph
- **Type** : feature
- **Statut** : doing — itération 6 en préparation
- **Priorité** : normale
- **Version cible** : à définir
- **Dernière mise à jour** : 2026-04-24 (itération 6)

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] Régulation (pump_controller, PID) → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [x] Web — routes HTTP / WebSocket
- [ ] MQTT / Home Assistant
- [ ] UART / écran LVGL
- [ ] OTA
- [x] Authentification (endpoints d'action protégés)
- [x] Historique / persistance (`/api/history/ph`)
- [x] Frontend (`data/`)

## Agents requis
- [x] spec-reader
- [x] architect
- [x] pool-chemistry — calibration + dosage manuel
- [x] ux-ui-designer — design du bloc Statistiques compact, arbitrage ergonomie
- [x] embedded-developer — endpoints firmware, WS, pump_controller
- [x] web-ui-developer — vue `/ph`, graphique, calibration guidée
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Type
feature

## Description
Page dédiée **pH** (`/ph`) regroupant la mesure, la régulation (automatique, programmée, injection manuelle), l'historique des mesures et la calibration de la sonde pH. C'est le point d'entrée utilisateur pour tout ce qui concerne la mesure et le pilotage du pH de la piscine.

## Contexte
Le pH est l'un des deux paramètres chimiques régulés par le contrôleur (avec l'ORP). Une sonde pH mesure la valeur en continu, une pompe doseuse corrige le pH en injectant du pH- (acide) ou du pH+ (base) selon la configuration. La sonde nécessite une calibration régulière (recommandée tous les 3 mois) sans laquelle le dosage devient imprécis et potentiellement dangereux. L'utilisateur a besoin d'un seul écran pour :
- voir la valeur actuelle de pH mesurée, où il en est de la quantité de produit injecté dans la journée par rapport à la limite quotidienne de sécurité, et l'état de calibration de la sonde
- choisir le mode de régulation : automatique, programmée ou injection manuelle
- lancer une injection manuelle ponctuelle
- consulter l'historique des mesures
- recalibrer la sonde selon un protocole guidé, sans quitter la page

## Comportement attendu

### Architecture de la page
La vue `/ph` est accessible via le menu principal. En mode nominal, elle affiche dans cet ordre :

1. **Bloc Statistiques** — mesure pH courante et dosage du jour. **Présenté sous forme de bandeau compact sans titre** (pas nécessairement un composant `.card` comme les autres sections) : il s'agit d'un affichage d'informations en lecture, pas d'une zone d'action configurable. Le design exact (bandeau horizontal, résumé en pleine largeur, grille d'indicateurs, etc.) est décidé par l'agent `ux-ui-designer`.
2. **Carte Régulation pH** — modes de régulation (automatique, programmée, manuelle). Le **mode Automatique** contient également les informations de calibration de la sonde (date, bandeau « Calibré ») et le bouton **Calibrer** pour lancer l'assistant : la calibration n'est pertinente que pour une régulation asservie à la valeur mesurée du pH, donc indissociable du mode Automatique.
3. **Carte Historique** — graphique des mesures pH

Lorsque l'utilisateur clique sur le bouton **Calibrer** (disponible uniquement dans le sous-bloc Automatique de la carte Régulation), les cartes **Régulation** et **Historique** sont masquées et remplacées par une carte **Calibration** présentant l'assistant guidé. Le bloc **Statistiques** reste visible en permanence.

À la fin du protocole de calibration (succès complet) **ou** si l'utilisateur annule la calibration en cours, la carte Calibration disparaît et les cartes Régulation et Historique réapparaissent dans leur état antérieur.

Toutes les valeurs temps réel (mesure pH courante, dosage du jour, état d'injection, statut de calibration) proviennent du flux WebSocket déjà ouvert par l'application. **Aucun polling REST dédié à cette page.** Les actions utilisateur (sauvegarde de config, démarrage/arrêt d'injection, étapes de calibration) passent par des appels HTTP authentifiés ciblés.

---

### 1. Bloc « Statistiques »
Toujours visible, y compris pendant une calibration active.

**Forme visuelle attendue**
- **Pas de titre** (pas de `card__title`, pas de libellé « Statistiques »)
- **Compact**, en lecture seule, pas de composant `.card` classique avec ombrage et en-tête : privilégier un bandeau/bloc d'informations léger
- Densité maximale d'information utile sans chrome inutile (pas de padding vertical excessif, pas de sous-titres explicatifs)
- Design à arrêter par l'agent `ux-ui-designer` (plusieurs mises en forme possibles : bandeau horizontal, grille compacte, résumé en ligne)

**Valeur pH courante**
- Label :
  - en mode nominal : « Valeur pH actuelle »
  - pendant une calibration active : « Tension brute capteur »
- Valeur :
  - en mode nominal : pH avec 2 décimales (ex. `7.21`), ou `--` si indisponible
  - pendant une calibration active : tension brute du capteur en mV avec 1 décimale, ou `--` si indisponible
- Mention « MAJ auto toutes les 5 secondes » (cadence du push WebSocket de mesure)

**Bloc « Dosage du jour »**
- Affiche le cumul injecté depuis minuit local sous la forme `X / Y mL` où Y est la limite journalière maximale
- Barre de progression reflétant le ratio avec trois paliers de couleur :
  - vert jusqu'à 75 % de la limite
  - orange entre 75 % et 90 %
  - rouge au-delà de 90 % ou quand la limite est déclarée atteinte par le firmware
- Si la limite n'est pas configurée (0 ou non défini), affiche seulement la valeur injectée et le texte « Limite non configurée »
- Si aucune donnée n'est encore reçue, affiche `— mL` et une barre vide
- Quand la limite est atteinte côté firmware, un statut explicite s'affiche : « Limite atteinte — dosage suspendu jusqu'à minuit »
- Accessibilité : la barre utilise `role="progressbar"` avec `aria-valuenow`, `aria-valuemin`, `aria-valuemax`

> Le bloc Statistiques ne contient **pas** les informations de calibration ni le bouton Calibrer : ces éléments sont affichés dans la carte Régulation pH (sous-bloc Automatique uniquement) car ils ne sont pertinents qu'en régulation asservie.

---

### 2. Carte « Régulation pH »
Visible en mode nominal, masquée pendant une calibration active.

**Sélecteur de mode** (onglets ou groupe de boutons)
- **Automatique** — le firmware régule le pH en permanence vers la cible configurée
- **Programmée** — le firmware injecte un volume quotidien configurable (en mL), sans tenir compte de la valeur de pH mesurée ni de la cible, dans le respect des règles de sécurité (filtration active, délai minimum entre injections, limite horaire et journalière de volume)
- **Manuelle** — aucune régulation automatique ; seule l'injection manuelle ponctuelle est disponible

Le mode sélectionné est sauvegardé immédiatement (pas de bouton Sauvegarder pour le choix du mode lui-même). Les sous-blocs associés au mode actif sont seuls visibles.

**Mode Automatique**
- **Type de correction** : choix entre « pH- (acide) : baisser le pH » et « pH+ (base) : augmenter le pH »
- **pH cible** : valeur numérique entre 0 et 14, pas de 0.1, valeur par défaut 7.2
- **Bouton Sauvegarder** :
  - désactivé tant qu'aucun champ n'a été modifié (état propre)
  - actif dès qu'un champ a changé
  - affiche un spinner pendant la sauvegarde
  - bascule en état « Sauvegarde réussie » (vert) pendant 2 s après succès, puis revient à l'état propre
  - bascule en état « Erreur » (rouge) en cas d'échec, et redevient cliquable après 2 s pour réessayer

**Informations de calibration** (visibles **uniquement** dans le mode Automatique)
- Date de dernière calibration au format local, ou `—` si jamais calibrée
- Bandeau « Calibré » avec ✓ si la sonde est actuellement calibrée (état reçu via WebSocket), masqué tant qu'une calibration est en cours
- Le cas échéant, valeurs de référence retenues à la dernière calibration (comme celles aujourd'hui présentes dans la carte « Calibration de la sonde pH »)
- **Bouton « Calibrer »** : déclenche l'affichage de la carte Calibration en remplacement des cartes Régulation et Historique (voir §4). Désactivé si une calibration est déjà en cours ou si une injection pH est en cours. Placement et rendu visuel arrêtés par `ux-ui-designer` au sein du sous-bloc Automatique.
- En modes Programmée et Manuelle, ces informations et ce bouton ne sont pas affichés. Pour calibrer, l'utilisateur bascule d'abord en mode Automatique.

**Mode Programmée**
- L'utilisateur définit un **volume quotidien de produit à injecter**, exprimé en mL
  - Input numérique, plage raisonnable (ex. 0 à la limite journalière configurée), pas de 10 mL, valeur par défaut 0
  - La saisie est bornée côté UI par la limite journalière de sécurité (`max_ph_ml_per_day`) : toute valeur supérieure est refusée avec un message explicite
  - Un **hint** rappelle la limite journalière en vigueur (« limite de sécurité : Y mL »)
- Le firmware répartit l'injection de ce volume sur la journée (cadence et granularité gérées côté firmware, hors périmètre de cette carte UI)
- Le volume consommé dans la journée est déjà remonté par le bloc « Dosage du jour » de la carte Statistiques : aucun indicateur supplémentaire nécessaire dans la carte Régulation
- **Bouton Sauvegarder** : même comportement que le mode Automatique (état propre / modifié / spinner / succès 2 s / erreur 2 s)
- Si le volume quotidien est à `0`, aucune injection programmée n'est déclenchée par le firmware ; l'injection manuelle reste disponible

**Injection manuelle** (visible **uniquement** lorsque le mode Manuelle est sélectionné)
- Ce sous-bloc remplace intégralement les paramètres des autres modes quand Manuelle est actif : il n'y a ni pH cible, ni volume quotidien à saisir dans ce mode, seulement l'injection ponctuelle
- Dans les modes Automatique et Programmée, le sous-bloc Injection manuelle n'est pas affiché
- Texte explicatif : « Démarre la pompe pH à la puissance configurée. Utile pour corriger manuellement le pH ou vérifier le fonctionnement de la pompe. Cliquez à nouveau sur le bouton pour arrêter l'injection avant la fin. »
- **Volume (mL)** : input numérique entre 1 et 2000, pas de 5, valeur par défaut 50
- **Hint dynamique** : durée estimée de l'injection en fonction du volume saisi et du débit courant de la pompe
- **Bouton « ▶ Injecter »** :
  - au clic, déclenche une injection pour le volume saisi
  - pendant le démarrage : feedback transitoire (« ⏳ Démarrage — ≈ X » avec la durée estimée)
  - pendant l'injection : le bouton reflète l'état via les messages WebSocket (temps restant, pourcentage)
  - un nouveau clic pendant l'injection envoie un ordre d'arrêt
  - l'input volume est désactivé pendant le démarrage et l'injection
  - en cas d'échec du démarrage, le bouton et l'input reviennent immédiatement à l'état initial

**Contraintes de sécurité (héritées du firmware)**
- L'injection manuelle est **soumise aux mêmes limites** que la régulation automatique (cumul journalier et horaire)
- Une injection manuelle peut être refusée par le firmware si la filtration n'est pas active depuis assez longtemps, si le capteur est stale, ou si une limite est atteinte — dans ce cas, le bouton repasse à l'état initial et, si possible, un message d'erreur est remonté

---

### 3. Carte « Historique »
Visible en mode nominal, masquée pendant une calibration active.

**Graphique**
- Graphique ligne Chart.js des mesures pH dans le temps
- Bornes min/max visuelles à pH 6.8 et 7.6 (zone verte), avec un bandeau ombré indiquant la zone acceptable
- La cible configurée (`ph_target`) est matérialisée sur le graphique

**Sélecteurs de plage**
- Quatre boutons : `Tout`, `30j`, `7j`, `24h`
- Le bouton actif est visuellement distinct
- Le changement de plage recharge le graphique avec la granularité appropriée (échantillonnage brut ou agrégé selon la plage)
- Par défaut, la plage est `Tout` à l'arrivée sur la page ; la plage sélectionnée est conservée entre les visites pendant la durée de la session

---

### 4. Carte « Calibration de la sonde pH »
**Visible uniquement lorsqu'une calibration est en cours**, à la place des cartes Régulation et Historique.

**En-tête**
- Titre : « Calibration de la sonde pH »
- Sous-titre : date de la dernière calibration au format local, ou `—` si jamais calibrée

**Texte explicatif**
- Rappelle que la calibration est nécessaire pour des mesures précises
- Prévient du risque de dosage incorrect si la sonde n'est pas calibrée
- Recommande une recalibration tous les 3 mois avec des solutions pH 7.0 et pH 4.0

**Protocole guidé en 4 étapes**
Les étapes sont affichées sous forme de liste numérotée avec trois états visuels : neutre, active (en cours), completed (validée).

1. Plonger dans une solution d'étalonnage pH 7.0
2. Attendre que la tension brute du capteur se stabilise (environ 1 min)
3. Rincer et plonger la sonde dans une solution d'étalonnage pH 4.0
4. Attendre que la tension brute du capteur se stabilise (environ 1 min)

La valeur temps réel affichée pendant la calibration (tension brute du capteur) est celle de la carte Statistiques ; la carte Calibration ne duplique pas la lecture.

**Boutons**
- **Commencer la calibration** : libellé dynamique selon l'étape courante
  - Étape 1 en cours : « Étape suivante » (passe à l'étape 2)
  - Étape 2 en cours : « Calibrer pH 7.0 » (déclenche l'étalonnage côté firmware)
  - Étape 3 en cours : « Étape suivante » (passe à l'étape 4)
  - Étape 4 en cours : « Calibrer pH 4.0 » (déclenche le second étalonnage côté firmware)
- **Annuler** : visible dès qu'une calibration est active, demande confirmation avant d'annuler

**États de transition et sortie de la carte**
- Au démarrage d'une calibration, les cartes Régulation + Historique sont cachées (le bandeau « Calibré », affiché dans le sous-bloc Automatique, est donc retiré de la vue en même temps que la carte Régulation)
- Pendant un appel de calibration au firmware, le bouton est désactivé (évite le double-clic)
- En cas d'échec d'une étape d'étalonnage, une alerte explicite est affichée dans la carte Calibration (`Erreur calibration pH 7.0 : ...` ou `Erreur calibration pH 4.0 : ...`) et le protocole est réinitialisé à l'état idle ; l'utilisateur peut réessayer ou annuler pour sortir
- À la fin réussie de l'étape 4 : toutes les étapes sont marquées comme complétées, la carte Calibration disparaît et les cartes Régulation + Historique réapparaissent ; le sous-bloc Automatique de la carte Régulation met à jour son bandeau « Calibré » et sa date avec la nouvelle valeur
- Sur annulation confirmée : la carte Calibration disparaît, les cartes Régulation + Historique réapparaissent, et l'état de calibration précédent est conservé (si la sonde était calibrée auparavant, le bandeau précédent est restauré)

---

## Cas limites transverses à la page
- **WebSocket déconnecté** : les valeurs temps réel (pH courant, dosage du jour, état d'injection) affichent `--` ou l'état vide ; les sauvegardes de config et actions de calibration échouent avec un feedback erreur explicite jusqu'à reconnexion
- **Capteur pH absent ou en erreur** : la valeur affiche `--`, un badge visuel signale l'anomalie sur la page dashboard (hors périmètre de cette spec mais interconnecté)
- **Sonde non calibrée** : un bandeau d'alerte visuel peut être affiché ailleurs dans l'UI ; la page `/ph` autorise malgré tout l'ouverture de toutes les sections, la calibration étant précisément l'endroit où on résout ce problème
- **Régulation inhibée** (mode piloté, filtration arrêtée, stabilisation en cours) : le bloc Dosage du jour reste fonctionnel, l'injection manuelle peut être refusée ; un délai de stabilisation restant peut être affiché via le champ WebSocket `stabilization_remaining_s` (visibilité à préciser selon évolution UI)
- **Limite horaire atteinte** (cumul sur la dernière heure) : distincte de la limite journalière, gérée côté firmware ; pas d'affichage dédié dans cette version
- **Tentative de calibration pendant une injection en cours** : la calibration n'est pas autorisée ; le bouton Calibrer affiche un message explicatif et reste inactif tant que l'injection n'est pas terminée

## Comportement actuel
La page `/ph` existe et présente actuellement **quatre cartes toujours visibles** :
1. Régulation pH (toggle automatique + bloc « Dosage du jour » + paramètres pH cible / type de correction + bouton Sauvegarder)
2. Injection manuelle (volume + bouton Injecter)
3. Historique (graphique + sélecteurs de plage)
4. Calibration de la sonde pH (bandeau « Calibré », lecture temps réel, protocole 4 étapes, boutons)

La valeur pH courante, la date de dernière calibration et le bandeau « Calibré » sont aujourd'hui affichés dans la carte Calibration et non dans une carte Statistiques dédiée. Il n'existe pas de mode « Programmée » : la régulation automatique est pilotée par un toggle binaire on/off. L'injection manuelle est une carte autonome, distincte de la carte de régulation.

La refonte consiste à :
- créer la carte **Statistiques** qui consolide la lecture pH courante, le dosage du jour et les infos de calibration, et expose le bouton **Calibrer**
- fusionner la carte Régulation et la carte Injection manuelle dans une seule carte **Régulation pH** avec un sélecteur de mode (automatique / programmée / manuelle)
- masquer les cartes Régulation et Historique pendant la calibration, la carte **Calibration** les remplaçant ; restauration automatique en fin ou annulation

## Contraintes techniques
- ESP32, offline first
- Vanilla JS dans `data/`, pas de framework, pas de bundler
- Français dans toute l'UI
- Chart.js pour les graphes
- Budget FS `spiffs` 1088 KB — vérifier avec `./build_fs.sh`
- WebSocket pour toute donnée temps réel ; HTTP authentifié pour les actions utilisateur uniquement
- AsyncWebServer : aucun handler bloquant > 50 ms côté firmware
- La transition carte Calibration ↔ cartes Régulation + Historique s'effectue uniquement côté frontend (CSS `is-active` / display toggle), sans rechargement de la vue SPA

## Fichiers potentiellement concernés
- `data/index.html` — section `#view-ph` : création de la carte Statistiques, consolidation Régulation + Injection manuelle, ajout du conteneur Calibration conditionnel
- `data/app.js` — logique d'ouverture / fermeture de la calibration (show/hide des cartes), sélecteur de mode de régulation, déplacement de la lecture pH courante vers la carte Statistiques
- `data/app.css` — styles des cartes réorganisées, transitions d'affichage
- `src/web_routes_config.cpp` — éventuel support du mode de régulation si `Programmée` est livrée
- `src/web_routes_control.*` — endpoints d'injection pH (existants)
- `src/web_routes_calibration.cpp` — endpoints `/calibrate_ph_neutral`, `/calibrate_ph_acid` (existants)
- `src/ws_manager.cpp` — diffusion des champs `ph`, `ph_raw`, `ph_voltage_mv`, `ph_daily_ml`, `ph_limit_reached`, `ph_inject_remaining_s`, `ph_dosing`, `ph_target`, `ph_enabled`, `ph_correction_type`, `max_ph_ml_per_day`, `ph_cal_valid`, `ph_calibration_date` ; éventuel `ph_regulation_mode` si le mode programmée est livré
- `src/sensors.*` — lecture de la sonde pH (existant)
- `src/pump_controller.*` — pilotage de la pompe doseuse pH (existant)

## Éléments visibles (pour doc-writer)

### Endpoints HTTP concernés
- `POST /api/inject_ph` — démarrage injection manuelle (auth requise)
- `POST /api/inject_ph_stop` — arrêt injection en cours (auth requise)
- `POST /api/calibrate_ph_neutral` — étalonnage pH 7.0 (auth requise)
- `POST /api/calibrate_ph_acid` — étalonnage pH 4.0 (auth requise)
- `POST /api/config` — persistance `ph_enabled`, `ph_target`, `ph_correction_type` (+ éventuel `ph_regulation_mode` si livré)
- `GET /api/history/ph` — historique graphique (plages Tout / 30j / 7j / 24h)

### Champs WebSocket consommés
- Mesure : `ph`, `ph_raw`, `ph_voltage_mv`
- Dosage : `ph_daily_ml`, `ph_limit_reached`, `ph_inject_remaining_s`, `ph_dosing`
- Config : `ph_target`, `ph_enabled`, `ph_correction_type`, `max_ph_ml_per_day`
- Calibration : `ph_cal_valid`, `ph_calibration_date`
- Inhibition : `stabilization_remaining_s`

### Topics MQTT
- N/A — cette feature ne crée pas de nouveau topic, consomme les topics existants de régulation pH

### Documentation à produire / mettre à jour
- `docs/API.md` — endpoints d'injection et calibration pH (inchangés)
- `README.md` — mention de la nouvelle structure de la page `/ph`
- `CHANGELOG.md` — entrée sous la version cible décrivant la refonte

## Plan de test manuel
1. Ouvrir `/ph` → les trois cartes Statistiques / Régulation / Historique apparaissent dans l'ordre attendu
2. Vérifier le bloc Statistiques : valeur pH courante + bloc Dosage du jour uniquement (pas d'infos de calibration, pas de bouton Calibrer). Passer la carte Régulation en mode Automatique → vérifier que la date de dernière calibration, le bandeau « Calibré » (si applicable) et le bouton **Calibrer** apparaissent dans ce sous-bloc. Passer en mode Programmée puis Manuelle → ces éléments disparaissent.
3. Faire évoluer le cumul au-delà de 75 % / 90 % de la limite → les couleurs de la barre changent (vert → orange → rouge)
4. Changer le mode de régulation (Automatique → Manuelle → Automatique) → sauvegarde immédiate, affichage des sous-blocs cohérent
5. En mode Automatique, modifier le pH cible → bouton Sauvegarder actif → succès vert 2 s, puis état propre
5b. En mode Programmée, saisir un volume quotidien inférieur à la limite journalière → sauvegarde réussie ; saisir une valeur supérieure → refus avec message explicite ; saisir `0` → l'injection programmée est désactivée
6. Lancer une injection manuelle 50 mL → feedback « Démarrage ≈ X » puis temps restant → stopper avant la fin
7. En mode Automatique, cliquer sur **Calibrer** (dans le sous-bloc de la carte Régulation) → les cartes Régulation et Historique disparaissent, la carte Calibration apparaît ; le bloc Statistiques reste visible
8. Enchaîner les 4 étapes de calibration pH 7.0 puis pH 4.0 → à la fin, la carte Calibration disparaît, Régulation + Historique réapparaissent, le bandeau « Calibré » et la date dans le sous-bloc Automatique sont mis à jour
9. Relancer Calibrer, annuler à mi-protocole → après confirmation, la carte Calibration disparaît, Régulation + Historique réapparaissent, le bandeau précédent est restauré
10. Couper le WiFi (ou l'ESP) → les valeurs passent à `--`, badge WS déconnecté ; reconnecter → les valeurs reprennent

## Critères d'acceptation
- [ ] La vue `/ph` affiche en mode nominal les trois cartes **Statistiques**, **Régulation pH**, **Historique** dans cet ordre
- [ ] Le bloc Statistiques contient uniquement la valeur pH courante et le bloc « Dosage du jour » (les informations de calibration et le bouton Calibrer sont dans la carte Régulation, sous-bloc Automatique)
- [ ] Le bloc « Dosage du jour » affiche `X / Y mL` et une barre colorée selon les paliers 75 % / 90 %, alimentée en temps réel via le WebSocket sans polling
- [ ] La carte Régulation pH présente un sélecteur de mode (automatique / programmée / manuelle) dont la sélection est sauvegardée immédiatement
- [ ] En mode Automatique, le type de correction et le pH cible sont persistants et relus à l'arrivée sur la page ; les informations de calibration (date, bandeau « Calibré ») et le bouton **Calibrer** sont affichés dans ce sous-bloc uniquement
- [ ] En mode Programmée, un input volume quotidien (mL) est proposé, borné par la limite journalière de sécurité, persistant entre les visites ; la valeur 0 désactive l'injection programmée
- [ ] L'injection manuelle est visible **uniquement lorsque le mode Manuelle est actif** ; le sous-bloc démarre au clic, reflète sa durée estimée, affiche le temps restant et peut être interrompue par un second clic
- [ ] Le bloc Statistiques est compact, sans titre, non présenté comme une `.card` classique — la forme exacte est validée par le rapport de l'agent `ux-ui-designer`
- [ ] L'injection manuelle est refusée par le firmware dans les mêmes conditions que la régulation automatique (limite atteinte, filtration OFF, capteur stale)
- [ ] Le graphique d'historique affiche les quatre plages (Tout / 30j / 7j / 24h) avec un bouton actif
- [ ] Un clic sur **Calibrer** (disponible uniquement dans le sous-bloc Automatique de la carte Régulation) masque les cartes Régulation et Historique et affiche la carte Calibration à leur place ; le bloc Statistiques reste visible
- [ ] La calibration guidée enchaîne les 4 étapes avec les bons libellés, déclenche les endpoints aux bons moments, affiche une erreur en cas d'échec et met à jour la date de dernière calibration au succès
- [ ] À la fin réussie du protocole, la carte Calibration disparaît automatiquement et les cartes Régulation + Historique réapparaissent
- [ ] Annuler une calibration en cours (après confirmation) restaure l'état précédent sans perte du dernier étalonnage valide et ramène les cartes Régulation + Historique
- [ ] Tous les libellés utilisateur sont en français
- [ ] `pio run` compile sans nouveau warning
- [ ] `./build_fs.sh` passe sans dépasser la partition

## Notes pour agent
- Cette spec **remplace l'ancienne architecture 4 cartes** documentée dans la version précédente. La carte « Injection manuelle » autonome disparaît : son contenu est fusionné dans la carte Régulation pH.
- Toute évolution ultérieure de la page `/ph` doit faire l'objet d'une **nouvelle itération** dans la section « Suivi / itérations » de cette spec (cf. Itération 1 — baseline, Itération 2 — mode Programmée). La spec reste le référentiel unique pour l'écran `/ph` ; chaque itération documente les écarts par rapport à l'existant et les fichiers à toucher.
- Avant de modifier un comportement ici, vérifier qu'on ne casse pas un autre écran : la carte compacte pH du dashboard, les badges globaux de sensors, les topics MQTT de régulation pH et l'auto-discovery Home Assistant consomment les mêmes données sous-jacentes.
- La calibration touche directement la sécurité chimique (une mauvaise calibration entraîne un mauvais dosage) : toute modification du protocole de calibration ou de la logique de validation → passage obligatoire par l'agent `pool-chemistry`.
- Le bloc « Dosage du jour » existe aussi sur la page `/orp` de manière symétrique. Toute évolution de ce bloc doit être répercutée sur les deux vues simultanément pour garder la cohérence. La page ORP devra recevoir une refonte équivalente dans une spec dédiée si l'on veut maintenir la symétrie.
- Le mode **Programmée** consiste à choisir un volume quotidien en mL à injecter ; sa répartition effective dans la journée est à la charge du firmware et sort du périmètre de cette spec UI.
- Le volume programmé doit être **systématiquement borné par la limite journalière de sécurité** (`max_ph_ml_per_day`) côté UI **et** côté firmware ; la validation côté firmware est la source de vérité.

## Hors périmètre
- Refonte équivalente de la page `/orp` (à traiter dans une spec dédiée pour préserver la symétrie pH/ORP)
- Algorithme de répartition du volume quotidien programmé sur la journée (cadence, nombre d'injections, interaction avec la filtration) — logique firmware à définir séparément si nécessaire
- Affichage d'un éventuel délai de stabilisation restant (`stabilization_remaining_s`)
- Gestion de la limite horaire côté UI

---

## Suivi / itérations

### Itération 6 — 2026-04-24 — Indicateur stale data WS + toast reboot inattendu

- Commit(s) : —
- Statut : livré — tests verts, revue OK
- Fichiers modifiés : `data/app.js`, `data/app.css`
- Contexte :
  - Lors d'une déconnexion WebSocket, les valeurs affichées dans les blocs Statistiques de `/ph` et `/orp` (valeur courante pH/ORP, compteurs journaliers) restent visibles et semblent « vivantes », alors qu'elles sont figées à la dernière valeur reçue. L'utilisateur ne peut pas distinguer une donnée fraîche d'une donnée gelée.
  - Par ailleurs, quand l'ESP32 redémarre inopinément (watchdog, brownout) et se reconnecte, les compteurs reviennent à 0 sans explication. La feature-004 itération 3 corrigera la perte des compteurs au reboot, mais l'utilisateur mérite tout de même une notification explicite quand un reboot inattendu s'est produit.
  - Cette itération couvre les pages `/ph` et `/orp` simultanément (les blocs Statistiques partagent les mêmes fonctions JS `updateDailyDoseDisplay()` et les mêmes noms de blocs `.ph-stats` / `.orp-stats`).
  - Dépend de feature-004 itération 4 pour le champ `reset_reason` dans le payload WS ; le toast doit se comporter gracieusement si ce champ est absent.
- Écarts par rapport à l'existant :

  **A. `data/app.js`**
  - Dans `_onDisconnect()` : après `setNetStatus('bad', 'Déconnecté')`, ajouter la classe CSS `.is-stale` aux éléments `#ph-card-stats` et `#orp-card-stats` (blocs Statistiques compacts des deux pages).
  - Dans `loadSensorData()`, au moment où `latestSensorData` est mis à jour (succès WS ou HTTP) : retirer la classe `.is-stale` de tous les éléments portant cette classe (`document.querySelectorAll('.is-stale')`).
  - Toast reboot inattendu :
    - À chaque réception d'un message `sensor_data` via WebSocket, si le champ `reset_reason` est présent et différent de `undefined` / `null` :
      - Si sa valeur **n'est pas** `"POWER_ON"`, `"SW_RESET"` ou `"DEEP_SLEEP"` **et** si cette valeur n'a pas déjà été notifiée dans la session courante (`_lastNotifiedResetReason` variable de module) : afficher un toast dismissable « Redémarrage inattendu détecté (raison : X) ».
      - Mémoriser la valeur dans `_lastNotifiedResetReason` pour éviter la répétition.
    - Si `reset_reason` est absent du message (feature-004 itération 4 non encore déployée) : aucun toast, comportement inchangé.
    - Implémenter `showToast(message, durationMs)` si elle n'existe pas déjà, ou réutiliser le mécanisme existant de notifications.

  **B. `data/app.css`**
  - `.is-stale { opacity: 0.5; transition: opacity 0.3s ease; }`
  - `.is-stale .ph-stats__value, .is-stale .orp-stats__value { color: var(--muted); }` (ou équivalent selon les noms de classes réels des blocs)
  - Style du toast si non déjà présent : position `fixed` en bas de page, fond semi-transparent, bouton de fermeture, z-index élevé.

- Agents requis :
  - `web-ui-developer`
  - `test-engineer`
  - `code-reviewer`

- Critères d'acceptation :
  - [x] Sur `/ph` : lors d'une déconnexion WS, le bloc Statistiques passe à `opacity: 0.5` (grisé) en moins de 300 ms
  - [x] Sur `/orp` : même comportement (symétrie garantie)
  - [x] À la reconnexion WS et réception du premier push `sensor_data` : les blocs reviennent à `opacity: 1`
  - [x] Toast affiché une seule fois par session si `reset_reason` est `"WATCHDOG"`, `"BROWNOUT"` ou `"PANIC"` — non affiché pour `"POWER_ON"`, `"SW_RESET"`, `"DEEP_SLEEP"`
  - [x] Toast non affiché si `reset_reason` est absent du payload (feature-004 itération 4 non déployée)
  - [x] Toast ne se réaffiche pas à chaque push WS (mémorisation en session)
  - [x] Aucune régression sur calibration, injection manuelle, modes de régulation
  - [x] `./build_fs.sh` passe sans dépasser 1088 KB

- Hors périmètre :
  - Grisage global de toutes les valeurs capteurs (température, pH brut, ORP brut) — limité aux blocs Statistiques
  - Stale indicator sur le dashboard
  - Champ `reset_reason` côté firmware (feature-004 itération 4)

### Itération 5 — 2026-04-24 — mode Programmée : nouvel algorithme d'injection

- Commit(s) : —
- Statut : livré — `pio run` SUCCESS, code-reviewer Approved
- Contexte :
  - L'itération 2 a introduit un algorithme de répartition du volume programmé sur 24 h : `targetMlPerHour = phDailyTargetMl / 24`, converti en `targetMsPerHour` de temps de pompe. Cet algorithme produit des injections de quelques secondes par heure, invisibles pour l'utilisateur et potentiellement inférieures au temps de démarrage de la pompe. Il ignore aussi le planning de filtration (la pompe ne peut injecter que quand `canDose() = true`).
  - Le nouvel algorithme remplace la répartition temporelle par une règle simple : la pompe injecte librement pendant la filtration jusqu'à atteindre le quota journalier (`phDailyTargetMl`), en respectant uniquement les gardes de sécurité existantes.
- Écarts par rapport à l'existant (`src/pump_controller.cpp`, branche `scheduled` dans `regulatePh()`) :
  - **Supprimer** :
    - `targetMlPerHour = effectiveDailyMl / 24.0f`
    - `targetMsPerHour = (targetMlPerHour / effectiveFlowMlPerMin) * 60 000.0f`
    - Le bloc de réinitialisation quotidienne de `cyclesToday` / `cyclesDayStart` dans la branche scheduled (ce compteur n'est plus pertinent ici)
    - La condition `phDosingState.cyclesToday < pumpProtection.maxCyclesPerDay` dans `shouldDose`
    - Le log de limite de cycles (`lastCycleWarn`)
  - **Remplacer** `shouldDose` par :
    ```cpp
    // Injecter si le quota journalier n'est pas atteint ET si la limite horaire n'est pas consommée
    bool shouldDose = (safetyLimits.dailyPhInjectedMl < static_cast<float>(mqttCfg.phDailyTargetMl))
                   && phLimitOk;
    ```
  - **Conserver intégralement** :
    - `canDose()` en amont (filtration + délai de stabilisation)
    - `phSafetyOk` = `checkSafetyLimits(true)` (filet de sécurité absolu `maxPhMinusMlPerDay`)
    - Plafonnement `effectiveDailyMl` à `maxPhMinusMlPerDay` (remplacé par la condition `dailyPhInjectedMl < phDailyTargetMl` mais `phSafetyOk` reste le filet)
    - Vérification `effectiveFlowMlPerMin > 0`
    - Gestion start/stop de `phDosingState.active` (via `phActive` en fin de fonction)
    - Reset PID en sortie de branche scheduled
  - **Adapter les logs** :
    - Démarrage : `"[Scheduled] Démarrage dosage pH : quota=" + phDailyTargetMl + "mL, injecté=" + dailyPhInjectedMl + "mL"`
    - Arrêt quota journalier atteint : `"[Scheduled] Quota journalier pH atteint (" + dailyPhInjectedMl + "/" + phDailyTargetMl + "mL) — dosage suspendu jusqu'à demain"`
    - Arrêt limite horaire : déjà géré par le log existant de `phLimitOk`

- Agents requis :
  - `pool-chemistry` — validation obligatoire avant implémentation
  - `embedded-developer`
  - `test-engineer`
  - `code-reviewer`

- Critères d'acceptation :
  - [x] En mode Programmée avec `phDailyTargetMl = 20 mL` et filtration active, la pompe tourne en continu (sujet à la limite horaire) sans calcul de répartition sur 24 h
  - [x] Quand `dailyPhInjectedMl >= phDailyTargetMl`, le dosage s'arrête et un log indique le motif
  - [x] Quand la limite horaire est atteinte, le dosage s'arrête ; il reprend à la fenêtre horaire suivante si le quota journalier n'est pas encore atteint
  - [x] `maxPhMinusMlPerDay` (filet de sécurité) continue de bloquer le dosage si le quota de sécurité est dépassé, indépendamment de `phDailyTargetMl`
  - [x] `canDose()` continue de bloquer le dosage si la filtration est arrêtée
  - [x] Aucune référence à `targetMlPerHour`, `targetMsPerHour`, `cyclesToday` (branche scheduled) ne subsiste
  - [x] `pio run` compile sans nouveau warning

- Hors périmètre :
  - Modifications UI (le bloc Dosage du jour existant suffit à visualiser la progression)
  - ORP scheduled (couvert par feature-005)

### Itération 4 — 2026-04-24 — mode Programmée : dosage indépendant de la mesure pH

- Commit(s) : —
- Statut : livré — `pio run` SUCCESS, code-reviewer approuvé
- Contexte :
  - L'itération 2 a introduit la branche `scheduled` dans `regulatePh()` (`pump_controller.cpp`). Elle contient une garde sur la validité du capteur pH (lignes ~471-473) : si `currentPh` est NaN ou hors de la plage 4–10, le dosage programmée est suspendu avec un log `critical`.
  - Or, en mode Programmée, l'utilisateur a explicitement choisi d'injecter un volume quotidien fixe **sans asservissement à la mesure de pH**. Bloquer le dosage sur une valeur capteur hors plage va à l'encontre de l'intention de ce mode : si le capteur est déréglé ou en cours de calibration, le volume programmé doit quand même être injecté, dans la limite des contrôles de sécurité.
  - De même, `phTarget` (cible pH) n'est pas utilisé dans la branche `scheduled` — c'est correct — mais ce point doit être documenté explicitement dans la spec pour éviter toute régression future.
- Écarts par rapport à l'existant (fichiers firmware uniquement) :
  - **`src/pump_controller.cpp`** — branche `scheduled` dans `regulatePh()` :
    - Supprimer la garde qui bloque le dosage si `currentPh` est invalide (NaN ou hors plage 4–10). Remplacer par un simple log `warning` (sans retour anticipé) pour tracer l'anomalie sans interrompre le dosage programmé.
    - Supprimer la lecture de `currentPh` si elle n'est utilisée qu'à des fins de validation (plus aucune décision de dosage ne doit dépendre de la valeur mesurée en mode Programmée).
    - Conserver toutes les gardes qui ne dépendent pas de la mesure : filtration active (`canDose()`), délai minimum entre injections, `phLimitOk` (limite horaire), `phSafetyOk` (limite journalière cumulée), plafonnement à `maxPhMinusMlPerDay`, vérification du débit configuré, limite de cycles.
    - `phTarget` ne doit pas être lu ni comparé dans cette branche — vérifier qu'aucun code futur ne l'introduit (ajouter un commentaire explicite).
- Agents requis :
  - `pool-chemistry` — validation obligatoire avant implémentation (suppression d'une garde capteur sur le dosage)
  - `embedded-developer` — implémentation
  - `test-engineer` — compilation + rapport
  - `code-reviewer` — revue avant commit
- Critères d'acceptation :
  - [x] En mode Programmée, si le capteur pH retourne une valeur NaN ou hors plage 4–10, un log `warning` est émis mais le dosage n'est pas interrompu.
  - [x] En mode Programmée, `phTarget` n'est lu ni comparé nulle part dans la branche `scheduled`.
  - [x] Les gardes de sécurité `phLimitOk`, `phSafetyOk`, plafonnement à `maxPhMinusMlPerDay`, vérification du débit et limite de cycles sont inchangées.
  - [x] En mode Automatique, le comportement est inchangé (la valeur et la cible pH continuent de piloter le PID).
  - [x] `pio run` compile sans nouveau warning.
- Hors périmètre :
  - Toute modification UI/frontend (l'écart est purement firmware)
  - Refonte de la logique PID du mode Automatique

### Itération 3 — 2026-04-24 — raffinement ergonomique (bloc Stats compact, injection manuelle en mode Manuelle, calibration en mode Automatique)
- Commit(s) : —
- Statut : livré — build FS passe (639 KB / 1088 KB), code-reviewer approuvé
- Fichiers modifiés : `data/index.html`, `data/app.js`, `data/app.css`
- Rapport de design (ux-ui-designer, assuré par orchestrateur) :
  - Bloc Statistiques : grille 2 colonnes séparées par un diviseur vertical, pas de `.card`, pas de titre. pH courant à gauche (valeur 2rem gras), Dosage du jour à droite. Responsive : passage en colonne unique sous 500 px avec séparateur horizontal.
  - Infos calibration + bouton Calibrer placés dans `#ph-regulation-params` après le bouton Sauvegarder, séparés par un `<hr>`, dans un `#ph-calibration-info` masqué/affiché par `updatePhModeControls()` selon le mode.
  - Injection manuelle déplacée dans `#ph-inject-section` avec `display:none` par défaut, visible uniquement en mode Manuelle.
- Contexte :
  - L'itération 2 a défini la carte Statistiques comme une `.card` classique avec titre, regroupant pH courant + Dosage du jour + infos calibration + bouton Calibrer. Elle définit aussi l'injection manuelle comme sous-bloc disponible dans tous les modes de régulation.
  - Le design retenu à l'usage n'est pas satisfaisant :
    - le titre de la carte Statistiques est redondant (l'utilisateur voit des informations, pas un titre de section) ;
    - la carte est trop volumineuse visuellement ;
    - afficher les paramètres d'injection manuelle en mode Automatique/Programmée crée de la charge cognitive inutile (on ne veut pouvoir injecter « à la main » que si l'on est explicitement en mode Manuelle) ;
    - les informations de calibration et le bouton Calibrer n'ont de sens qu'en régulation asservie au pH mesuré — donc uniquement en mode Automatique. Les afficher dans le bloc Statistiques (toujours visible) est trompeur en modes Programmée et Manuelle.
- Écarts par rapport à l'itération 2 :
  1. **Bloc Statistiques compact, sans titre, pas une `.card`** : remplacer le composant carte par un bandeau/bloc d'informations allégé, validé par `ux-ui-designer`.
  2. **Bloc Statistiques réduit à 2 éléments** : pH courant + Dosage du jour uniquement. Les infos de calibration et le bouton Calibrer sont retirés.
  3. **Infos de calibration + bouton Calibrer déplacés dans le sous-bloc Automatique de la carte Régulation** : visibles uniquement quand le mode Automatique est actif ; non affichés en Programmée et Manuelle.
  4. **Injection manuelle visible uniquement en mode Manuelle** : retirer le sous-bloc Injection manuelle des modes Automatique et Programmée ; il remplace les paramètres spécifiques à ces modes quand Manuelle est actif.
- Critères d'acceptation spécifiques à cette itération :
  - [x] Le bloc Statistiques n'a pas de titre visible et n'utilise pas la classe `.card` (ou utilise une variante visuellement plus compacte)
  - [x] Le bloc Statistiques contient **uniquement** pH courant + Dosage du jour (pas d'infos calibration, pas de bouton Calibrer)
  - [x] Le bloc Statistiques tient dans une hauteur visuelle réduite par rapport à une `.card` classique (objectif : ≤ 50 % de la hauteur actuelle sur desktop)
  - [x] Les **infos de calibration (date, bandeau « Calibré ») et le bouton Calibrer** sont affichés **uniquement** dans le sous-bloc Automatique de la carte Régulation ; absents en modes Programmée et Manuelle
  - [x] À la fin d'une calibration (succès), la date + bandeau sont mis à jour dans le sous-bloc Automatique (et non dans le bloc Statistiques)
  - [x] Le sous-bloc Injection manuelle n'est affiché QUE lorsque `ph_regulation_mode === 'manual'`
  - [x] En modes Automatique et Programmée, aucun contrôle d'injection manuelle n'est visible dans la carte Régulation
  - [x] Le rapport `ux-ui-designer` est joint à l'itération et servi de référence au `web-ui-developer`
  - [x] Aucune régression : mode Programmée, sauvegarde config, calibration guidée continuent de fonctionner
  - [x] `./build_fs.sh` passe sans dépasser la partition `spiffs` (1088 KB)
- Hors périmètre de cette itération :
  - Toute modification du firmware (c'est une itération purement UI/UX)
  - Refonte équivalente du bloc Statistiques sur la page `/orp` (à traiter dans une spec dédiée pour préserver la symétrie)

### Itération 2 — 2026-04-23 — mode Programmée + refonte architecture des cartes
- Commit(s) : feature-003-page-ph (branche active)
- Statut : livré — tests verts, revue OK
- Fichiers modifiés :
  - `src/config.h` — nouveaux champs `ph_regulation_mode` (String) et `ph_daily_target_ml` (int)
  - `src/config.cpp` — sérialisation/désérialisation + migration `ph_enabled` → `ph_regulation_mode`
  - `src/pump_controller.cpp` — branche « scheduled » dans `regulatePh()`
  - `src/ws_manager.cpp` — `ph_regulation_mode` et `ph_daily_target_ml` ajoutés au payload WebSocket
  - `src/web_routes_config.cpp` — validation HTTP des deux nouveaux champs + fix log mot de passe WiFi
  - `src/mqtt_manager.cpp` — publication des deux nouveaux champs MQTT
  - `data/index.html` — segmented control `ph_regulation_mode` + champ `ph_daily_target_ml`
  - `data/app.js` — `collectConfig()`, `applyConfig()`, `updatePhModeControls()` mis à jour
- Résumé :
  - Introduction du mode **Programmée** : l'utilisateur saisit un **volume quotidien en mL** à injecter, borné par la limite journalière de sécurité (`max_ph_ml_per_day`). Répartition du volume dans la journée à la charge du firmware (hors périmètre UI).
  - **Refonte complète de l'architecture des cartes** de la page `/ph` pour passer de 4 cartes toujours visibles à la structure cible : Statistiques → Régulation → Historique, avec Calibration conditionnelle qui remplace Régulation + Historique pendant le protocole.

- Écarts par rapport à l'existant :

  **A. Config / firmware (mode Programmée)**
  - **Config** : aujourd'hui un booléen `ph_enabled` (toggle on/off) ; cible = un enum `ph_regulation_mode` à trois valeurs (`automatic` / `scheduled` / `manual`) **plus** un champ `ph_daily_target_ml` pour le volume quotidien programmé. → à ajouter dans `config.h` / `config.cpp`, dans la persistance, et dans le schéma JSON de `POST /api/config`.
  - **Migration** : les installations existantes n'ont que `ph_enabled`. Règle de migration au premier boot : `ph_enabled=true` → `ph_regulation_mode=automatic`, `ph_enabled=false` → `ph_regulation_mode=manual`. `ph_daily_target_ml` initialisé à `0`. À documenter dans `CHANGELOG.md` et `UPDATE_GUIDE.md`.
  - **Régulation firmware** : `pump_controller` actuel pilote uniquement en mode PID vers `ph_target`. Ajouter la branche « mode programmée » qui injecte `ph_daily_target_ml` réparti sur 24 h, en respectant les limites journalière et horaire, et en vérifiant la filtration active. → passage obligatoire par l'agent `pool-chemistry` avant implémentation.
  - **WebSocket** : diffuser les nouveaux champs `ph_regulation_mode` et `ph_daily_target_ml` dans `ws_manager.cpp`.
  - **Endpoint `POST /api/config`** : accepter les nouveaux champs, rejeter `ph_daily_target_ml > max_ph_ml_per_day` avec un message d'erreur exploitable côté UI.
  - **MQTT / Home Assistant** : vérifier si l'auto-discovery actuelle (switch `ph_enabled`) doit être remplacée par un `select` pour le mode et un `number` pour le volume quotidien ; sinon conserver `ph_enabled` comme miroir dérivé (`true` si mode ≠ `manual`) pour ne pas casser les automations utilisateur existantes.

  **B. Frontend (refonte architecture des cartes)**
  - **Carte Statistiques (nouvelle, en tête de vue)** — à créer dans `data/index.html` et `data/app.js` :
    - `readout` pH courant avec label dynamique : « Valeur pH actuelle » en nominal / « Tension brute capteur » pendant une calibration active ; valeur en pH 2 décimales ou mV 1 décimale selon l'état.
    - bloc « Dosage du jour » (déplacé depuis la carte Régulation, conserver la barre de progression colorée et `role="progressbar"`).
    - infos de calibration : date de dernière calibration + bandeau « Calibré » (déplacés depuis la carte Calibration).
    - bouton **Calibrer** (désactivé pendant une injection en cours ou pendant une calibration active).
  - **Carte Régulation pH** (existante, à enrichir) :
    - remplacer le toggle binaire `ph_enabled` par un **sélecteur 3 modes** (Automatique / Programmée / Manuelle) avec affichage conditionnel des sous-blocs.
    - sous-bloc **Automatique** : Type de correction + pH cible + bouton Sauvegarder.
    - sous-bloc **Programmée** : input volume quotidien (mL) + hint de limite + bouton Sauvegarder.
    - sous-bloc **Injection manuelle** : **fusionné ici** depuis la carte autonome actuelle ; visible dans TOUS les modes (Automatique, Programmée, Manuelle), pas seulement en mode Manuelle. Contient : volume + hint durée estimée + bouton ▶ Injecter.
    - retirer le bloc « Dosage du jour » (il est désormais dans Statistiques).
  - **Carte Historique** (existante, inchangée) — graphique + sélecteurs de plage.
  - **Carte Calibration de la sonde pH** (existante, à refondre en conteneur conditionnel) :
    - garder son contenu (texte explicatif, 4 étapes, boutons Commencer / Annuler).
    - **supprimer** son propre `readout` pH courant, son bandeau « Calibré » interne et sa date (tout est dans Statistiques).
    - la placer dans un conteneur masqué par défaut (`display:none`).
  - **Suppression** de la carte **Injection manuelle** autonome (son contenu est migré dans la carte Régulation).
  - **Logique JS (`data/app.js`)** :
    - clic sur **Calibrer** (carte Statistiques) → masque Régulation + Historique, affiche Calibration, bascule le label du readout vers « Tension brute capteur ».
    - fin réussie du protocole → met à jour la date + bandeau « Calibré » dans Statistiques, masque Calibration, réaffiche Régulation + Historique, restaure le label « Valeur pH actuelle ».
    - **Annuler** avec confirmation → idem sans mise à jour de la date.
    - refactoriser `updateDailyDosePh()` pour pointer vers les sélecteurs de la nouvelle carte Statistiques.
    - relecture de `ph_regulation_mode` et `ph_daily_target_ml` depuis le flux WS à l'arrivée sur la page et à chaque mise à jour.
  - **CSS (`data/app.css`)** : règles pour les transitions show/hide des cartes (pas de rechargement de vue SPA).

  **C. Documentation**
  - `docs/API.md` : ajout des champs `ph_regulation_mode`, `ph_daily_target_ml`, `ph_enabled` (miroir) dans les tableaux GET /get-config et POST /save-config.
  - `docs/UPDATE_GUIDE.md` : section migration `ph_enabled → ph_regulation_mode` ajoutée.
  - `CHANGELOG.md` : entrée [En cours] 2026-04-23 ajoutée.

  **D. Fichiers modifiés (itération 2)**
  - `src/config.h` — ajout `phRegulationMode`, `phDailyTargetMl` dans `struct MqttConfig`
  - `src/config.cpp` — `loadMqttConfig()` : migration NVS + chargement des nouveaux champs ; `saveMqttConfig()` : persistance des nouveaux champs
  - `src/pump_controller.cpp` — `update()` : remplacement de la condition `phEnabled` par `phMode == "automatic"` + ajout branche `scheduled` avec 5 gardes de sécurité validés par pool-chemistry
  - `src/ws_manager.cpp` — `_buildConfigJson()` : diffusion de `ph_regulation_mode` et `ph_daily_target_ml`
  - `src/web_routes_config.cpp` — `handleGetConfig()` : exposition des nouveaux champs ; `handleSaveConfig()` : traitement et validation des nouveaux champs (HTTP 400 si dépassement limite)
  - `src/mqtt_manager.h` — struct `MqttTopics` : ajout `phRegulationModeState`, `phDailyTargetMlState`
  - `src/mqtt_manager.cpp` — `refreshTopics()` et `publishTargetState()` mis à jour
  - `data/index.html` — sélecteur 3 boutons `#ph_regulation_mode`, bloc `#ph-params-scheduled`
  - `data/app.js` — `updatePhModeControls()`, `updatePhControls()`, `loadConfig()`, `collectConfig()`, `bindRegulationSave()`, `bindAutosave()` mis à jour

- Critères d'acceptation spécifiques à cette itération (en plus des critères généraux de la spec) :
  - [ ] La vue `/ph` affiche en mode nominal, dans l'ordre : **Statistiques → Régulation pH → Historique** (aucune carte Injection manuelle autonome, aucune carte Calibration visible).
  - [ ] La carte **Statistiques** contient les 4 éléments : pH courant, Dosage du jour, infos calibration (date + bandeau), bouton **Calibrer**.
  - [ ] Le bloc « Dosage du jour » n'apparaît **qu'une seule fois** dans la page, dans la carte Statistiques.
  - [ ] La carte Régulation contient le **sélecteur 3 modes** + sous-blocs conditionnels (Automatique : cible pH ; Programmée : volume quotidien ; Manuelle : injection ponctuelle).
  - [ ] La carte **Calibration** est **masquée par défaut** (pas de `display` visible à l'arrivée sur la page).
  - [ ] Un clic sur **Calibrer** masque Régulation + Historique et affiche Calibration ; Statistiques reste visible.
  - [ ] Pendant la calibration, le label du readout devient « Tension brute capteur » et la valeur s'affiche en mV.
  - [ ] En fin de protocole (succès ou annulation confirmée), Calibration disparaît, Régulation + Historique réapparaissent, le label redevient « Valeur pH actuelle ».
  - [ ] Le bouton **Calibrer** est désactivé si une injection pH est en cours (via `ph_dosing` ou `ph_inject_remaining_s`).
  - [x] Le mode **Programmée** fonctionne : saisie bornée par `max_ph_ml_per_day`, sauvegarde OK, rejet si > limite, firmware répartit l'injection sur 24 h, les limites de sécurité sont respectées.
  - [ ] Auto-discovery Home Assistant cohérente avec le nouveau modèle de config (switch `ph_enabled` maintenu en miroir OU remplacé par `select` + `number`, à trancher par `pool-chemistry` + `web-ui-developer`).
  - [x] `pio run` compile sans nouveau warning.
  - [x] `./build_fs.sh` passe sans dépasser la partition `spiffs` (1088 KB).

### Itération 1 — 2026-04-23 — baseline de la page `/ph`
- Commit(s) : —
- Statut : point de départ documentaire (aucune livraison associée à cette itération)
- Résumé :
  - Première version propre de la spec, servant de **référence de base** pour la page `/ph`.
  - Documente d'une part le **comportement existant** dans le firmware et l'UI actuels (4 cartes toujours visibles : Régulation pH, Injection manuelle, Historique, Calibration) — voir la section « Comportement actuel ».
  - Décrit d'autre part l'**architecture cible** souhaitée (3 cartes nominales Statistiques / Régulation / Historique + carte Calibration conditionnelle qui remplace Régulation + Historique pendant le protocole) — voir « Comportement attendu » et « Architecture de la page ».
  - Les écarts entre existant et cible (création de la carte Statistiques, fusion Régulation + Injection manuelle avec sélecteur de mode, masquage conditionnel pendant la calibration, introduction du mode Programmée) constituent le périmètre des itérations suivantes.
  - Les itérations ultérieures documenteront la livraison progressive de ces écarts, avec les commits associés et l'état des critères d'acceptation correspondants.
