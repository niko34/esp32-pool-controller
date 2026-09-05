## Type
feature

## Description
Afficher, dans la carte « Régulation » des vues pH et ORP de l'UI, la quantité de produit injectée depuis le début de la journée ainsi que la limite journalière maximale définie dans les paramètres de sécurité. Par la même occasion, clarifier le libellé du toggle de régulation automatique en remplaçant « Gérer la régulation » par « Gérer automatiquement la régulation ».

## Contexte
Les pompes doseuses (pH- et chlore) sont bornées par des limites journalières (`maxPhMinusMlPerDay`, `maxChlorineMlPerDay`) déjà appliquées côté firmware. Les compteurs `dailyPhInjectedMl` / `dailyOrpInjectedMl` sont mis à jour en temps réel et exposés par le backend, mais l'utilisateur n'a aujourd'hui aucun moyen de voir où il en est de son budget quotidien sans aller fouiller dans les logs ou dans MQTT. L'information est utile pour :
- comprendre pourquoi la régulation est suspendue (limite atteinte)
- détecter un surdosage anormal (consommation qui grimpe trop vite)
- anticiper un ajustement de la limite ou une recalibration

## Comportement attendu
Dans chaque vue pH et ORP, **à l'intérieur de la carte « Régulation »** (sous le toggle « Gérer automatiquement la régulation » et les paramètres associés), afficher un sous-bloc « Dosage du jour » avec :

- Un libellé clair : « Injecté aujourd'hui »
- La valeur : `<injecté> / <max> mL` (ex. `123 / 300 mL`)
- Une barre de progression (`<progress>` ou div CSS) représentant le ratio injecté / max
- Un code couleur sur la barre :
  - vert jusqu'à 75 %
  - orange entre 75 % et 90 %
  - rouge au-delà de 90 % (ou si la limite est atteinte / dépassée)
- Un sous-texte d'état optionnel quand la limite est atteinte : « Limite atteinte — dosage suspendu jusqu'à minuit »

Comportement dynamique — **tout passe par le WebSocket, aucun polling REST** :
- **Règle** : les vues pH et ORP ne doivent déclencher aucun nouveau `fetch('/api/...')` pour alimenter ce bloc. Toutes les valeurs lues proviennent exclusivement du tunnel WebSocket déjà ouvert (`ws_manager`).
- Cumul instantané : `ph_daily_ml` / `orp_daily_ml` + flags `ph_limit_reached` / `orp_limit_reached`, lus depuis le message `state` du WS (déjà diffusés, cf. `src/ws_manager.cpp` l. 148-151).
- Plafonds : `max_ph_ml_per_day` / `max_chlorine_ml_per_day`, lus depuis le message `config` du WS (déjà diffusés, cf. `src/ws_manager.cpp` l. 227-228). À la modification dans la page config, le serveur renvoie un nouveau message `config` sur le WS — l'UI se met à jour sans polling.
- **Si un champ manque** : côté frontend, ne pas compenser par un `fetch()` — remonter le manque et ajouter le champ dans le `_buildStateJson()` ou `_buildConfigJson()` de `src/ws_manager.cpp`, puis déclencher un broadcast lorsqu'il change. Cette feature-ci n'en a pas besoin (vérifié), mais la règle doit tenir pour toute évolution future.
- Reset visuel automatique à minuit local : les compteurs backend sont remis à zéro à minuit local et la nouvelle valeur part dans le `state` WS suivant ; côté UI il suffit de refléter ce qui arrive.

Libellé du toggle de régulation :
- Dans `#view-ph`, remplacer le texte « Gérer la régulation pH » du `<p class="row__title" id="label-ph-enabled">` par « Gérer automatiquement la régulation pH »
- Dans `#view-orp`, remplacer le texte « Gérer la régulation ORP » du `<p class="row__title" id="label-orp-enabled">` par « Gérer automatiquement la régulation ORP »
- L'ajout de « automatiquement » lève l'ambiguïté avec l'injection manuelle qui reste disponible ci-dessous

Cas limites :
- Si la limite max est à 0 ou non définie : afficher uniquement la valeur injectée sans barre de progression, avec un texte « Limite non configurée »
- Si le tracking est désactivé côté config : masquer le bloc ou afficher un état « Suivi désactivé »
- Si la connexion WebSocket est perdue : griser les valeurs, ne pas laisser une valeur figée sans indication

## Comportement actuel
Aucun affichage du cumul journalier injecté ni de la limite dans les vues pH et ORP. L'information existe côté backend et dans les logs mais n'est pas visible dans l'interface.

## Contraintes techniques
- ESP32, offline first (aucun appel réseau extérieur ajouté)
- Vanilla JS, pas de lib supplémentaire
- Respecter le budget FS `spiffs` 1088 KB (modification légère attendue : quelques nœuds HTML, CSS et ~30 lignes de JS)
- Accessibilité : `role="progressbar"`, `aria-valuenow`, `aria-valuemin`, `aria-valuemax` ; contraste suffisant sur tous les états
- Rétrocompatibilité : pas de modification du schéma WS ni des endpoints existants (les champs sont déjà diffusés)
- Ne pas bloquer de handler `AsyncWebServer`, ne pas publier de nouveau topic MQTT dans cette feature (à voir dans une évolution ultérieure si utile)

## Fichiers potentiellement concernés
- `data/index.html` — nouveaux blocs dans `#view-ph` et `#view-orp`
- `data/app.js` — mise à jour du rendu à la réception des messages WS (`ph_daily_ml`, `orp_daily_ml`) et lecture des limites depuis la config déjà chargée
- `data/app.css` — styles du bloc « Dosage du jour » et des trois états de la barre (vert/orange/rouge)
- Potentiellement `data/index.html` + `data/app.js` — petite synchro après sauvegarde des limites dans la vue config pour rafraîchir l'affichage

## Critères d'acceptation
- [x] La carte « Régulation » de la vue pH affiche le cumul journalier injecté et la limite max sous la forme `X / Y mL`
- [x] La carte « Régulation » de la vue ORP affiche le cumul journalier injecté et la limite max sous la forme `X / Y mL`
- [x] ~~Le libellé du toggle devient « Gérer automatiquement la régulation pH » dans la vue pH et « Gérer automatiquement la régulation ORP » dans la vue ORP~~ **OBSOLÈTE** — ce toggle n'existe plus : remplacé par le sélecteur de modes Automatique/Programmée/Manuelle qui lève l'ambiguïté visée (cf. Clôture)
- [x] Une barre de progression visuelle reflète le ratio avec les trois paliers de couleur (75 %, 90 %)
- [x] Les valeurs se mettent à jour en temps réel via le flux WebSocket existant
- [x] Les vues pH et ORP n'émettent **aucun** appel `fetch('/api/...')` pour alimenter ce bloc (vérifiable dans l'onglet Network du navigateur : seuls les frames WS apportent les données)
- [x] Si un champ venait à manquer côté serveur, il est ajouté dans le `state` ou `config` du `ws_manager` plutôt que via une nouvelle route HTTP
- [x] Quand la limite est atteinte côté firmware, l'UI affiche clairement que le dosage est suspendu jusqu'au prochain cycle quotidien
- [x] Après modification de la limite dans la vue config, la nouvelle valeur se reflète dans les vues pH et ORP sans avoir à recharger la page
- [x] Aucun warning de compilation nouveau, `pio run` reste vert
- [x] `./build_fs.sh` passe, la taille du FS reste sous la limite ~~des 1088 KB~~ (576 KB depuis le layout v3, ADR-0019)
- [x] Accessibilité : la barre utilise `role="progressbar"` et annonce correctement sa valeur aux lecteurs d'écran
- [x] Les cas limites sont gérés : limite non configurée, tracking désactivé, WS déconnecté

## Notes pour agent
- Pas de modification du backend attendue : les données sont déjà exposées dans le WS — message `state` (`ph_daily_ml`, `orp_daily_ml`, `ph_limit_reached`, `orp_limit_reached` : `src/ws_manager.cpp` l. 148-151) et message `config` (`max_ph_ml_per_day`, `max_chlorine_ml_per_day` : `src/ws_manager.cpp` l. 227-228). Vérifier avant de coder qu'aucun renommage récent n'a cassé ces clés.
- **Interdit sur cette feature** : tout `fetch('/api/data')`, `fetch('/api/config')` ou timer de polling dans les vues pH et ORP pour alimenter le bloc « Dosage du jour ». Si un champ manque, on l'ajoute côté WS, pas via REST.
- Ne pas dupliquer la logique déjà présente dans la vue `/dosages` (qui concerne le niveau du bidon, pas le cumul quotidien) : les deux informations restent distinctes.
- Feature purement UI : pas d'impact sur la régulation ni sur le dosage → passage par `pool-chemistry` non requis, revue par `web-ui-developer` + `code-reviewer` suffit.
- Penser à vérifier le comportement autour du reset à minuit local (observation pendant 24 h ou test manuel en forçant `dailyPhInjectedMl` via une commande debug si disponible).

---

## Clôture (2026-07-04) — DÉJÀ IMPLÉMENTÉE
Audit du code : la feature est **entièrement en place** (constaté en v2.5.2) :
- Blocs « Dosage du jour » `#daily-dose-ph`/`#daily-dose-orp` dans les vues pH et ORP, format `X / Y mL` (app.js `updateDailyDoseDisplay`, l.2295+).
- Barre de progression avec les paliers exacts de la spec (ok / warning ≥75 % / danger ≥90 % ou limite atteinte), `role=progressbar` + `aria-valuenow`.
- « Limite atteinte — dosage suspendu jusqu'à minuit » et « Limite non configurée » gérés (wording identique à la spec).
- Alimentation via l'état partagé (`latestSensorData.ph_daily_ml`/`orp_daily_ml`/`*_limit_reached` + `window._config.max_*`), sans polling dédié.
- **Partie obsolète** : le renommage du toggle « Gérer la régulation » — ce toggle n'existe plus, remplacé par le sélecteur de modes Automatique/Programmée/Manuelle qui lève l'ambiguïté visée.

La spec avait été implémentée sans être déplacée. Statut : **done** (rien à faire).
