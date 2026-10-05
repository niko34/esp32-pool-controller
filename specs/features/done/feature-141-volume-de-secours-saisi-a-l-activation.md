# feature-141 — Le volume de la chloration de secours est saisi à l'activation

## Métadonnées
- **ID** : feature-141
- **Slug** : volume-de-secours-saisi-a-l-activation
- **Type** : feature — **paramètre de dosage rendu à l'utilisateur**
- **Statut** : doing — code, tests et revue de code OK ; documentation soldée (ce tour) ; **en attente de commit** (aucun commit effectué par doc-writer, hors périmètre de cet agent)
- **Priorité** : haute — la constante actuelle n'est juste que par coïncidence
- **Version cible** : 2.56.0
- **Dernière mise à jour** : 2026-10-05
- **Spec(s) liée(s)** : `specs/features/done/feature-140-…` (le mode lui-même) · feature-011 (mode Programmée) · ADR-0087

## Domaines touchés
- [ ] Capteurs (pH / ORP / température)
- [x] **Régulation (pump_controller, PID)** → **passage obligatoire par `pool-chemistry`**
- [ ] Filtration / Éclairage
- [x] **Web — routes HTTP / WebSocket**
- [x] **MQTT / Home Assistant**
- [ ] UART / écran LVGL
- [ ] OTA
- [ ] Authentification
- [x] **Historique / persistance** — nouvelle clé NVS
- [x] **Frontend (`data/`)**

## Agents requis
- [x] spec-reader
- [x] architect
- [x] **pool-chemistry — a DÉJÀ rendu son verdict (voir plus bas), second passage obligatoire sur le diff réel**
- [ ] ux-ui-designer — libellés et bornes déjà imposés par le gate
- [x] embedded-developer
- [x] web-ui-developer
- [x] test-engineer
- [x] code-reviewer
- [x] doc-writer

## Description

Remplacer la constante `kRescueChlorineDailyMl = 1500` par un **volume saisi par
l'utilisateur au moment d'activer** la chloration de secours, pré-rempli et borné.

## Contexte

### La constante est juste par coïncidence

Le gate l'avait fixée à 1500 mL/j parce que c'était la valeur vivante de cet
utilisateur, et qu'elle semblait valoir ≈ 3× le besoin d'entretien calculé.

⚠️ **Le gate a corrigé lui-même ce calcul le 2026-10-05**, après confrontation
aux relevés de terrain :

> « Mes 350-530 mL/j étaient le besoin d'entretien **théorique** (perte UV +
> demande de fond). Les relevés de minuit — 939 à 1379 mL/j hors jours censurés —
> mesurent la demande **réelle**, 2 à 3× supérieure. Donc 1500 n'est pas « 3× le
> besoin » : c'est à peu près sa demande. Ma constante était encore plus
> accidentelle, et c'est un argument de plus pour la rendre paramétrable. »

Consommation réelle relevée dans les resets de minuit :

```
jours mesurés   : 939 · 1044 · 1079 · 1143 · 1176 · 1178 · 1237 · 1355 · 1379 mL
jours PLAFONNÉS : 1500 · 1500 · 1500 · 1500 · 1501 · 1501   (demande ≥ plafond)
```

Pour un bassin de 20 m³ la constante serait 4× trop élevée ; pour 80 m³,
insuffisante. **Rien dans le code ne connaît le bassin.**

### Pistes écartées, et pourquoi

**Utiliser la limite journalière maximale comme cible** — écarté : une limite de
**sécurité n'est pas une consigne**. Ce serait l'exposition chimique maximale par
défaut, et le terrain la réfute (2000 mL/j × 3 jours sans que l'ORP bouge). Elle
est déjà appliquée comme plafond par la condition C3 de feature-140.

**Estimer depuis l'historique** — écarté. L'historique ne stocke **que** pH, ORP,
température et état de filtration : **aucun volume injecté**. Il faudrait un
anneau persisté de volumes quotidiens, avec gestion des jours **censurés** (un
jour au plafond est une observation tronquée : la demande réelle était ≥ plafond,
l'inclure tire la médiane vers le bas). Et surtout — **objection décisive de
l'utilisateur** — la situation peut survenir avec **peu ou pas d'historique**
(installation neuve, reset) : l'estimation ne couvre alors rien.

⚠️ **Précision factuelle** : le champ d'injection manuelle existant
(`orp_inject_ml`, `data/index.html:1390`) vaut **50 mL** par défaut et n'est
**pas persisté** — c'est une **dose ponctuelle**, pas un volume journalier. Il
sert de modèle d'interaction, jamais de source de valeur.

## Comportement attendu

Un champ de **volume journalier** dans le dialogue de confirmation d'activation :
pré-rempli, modifiable, borné.

**Chaîne de pré-remplissage**, par ordre :
1. `orp_daily_target_ml` s'il est renseigné — l'estimation faite **à froid** par
   l'utilisateur pour le mode Programmée ;
2. sinon le **défaut par taille de bassin** (`PoolSize`, `src/config.h:111`) ;
3. sinon `kRescueChlorineDailyMl`, qui ne sert plus que de **repli final**.

### Cas limites
- `rescue_ml` absent, nul ou hors bornes au chargement NVS.
- Valeur saisie supérieure à la limite journalière de l'utilisateur.
- Activation par MQTT sans volume fourni.
- Redémarrage pendant le mode : la valeur saisie doit survivre.
- Boost actif et valeur saisie proche du plafond dur.

## Comportement actuel

`kRescueChlorineDailyMl = 1500` en dur dans `constants.h`, identique pour tous
les bassins. Deux clés NVS seulement (`rescue_active`, `rescue_until`).

## Contraintes techniques
- **Clé NVS ≤ 15 caractères** : `rescue_ml` (9) convient.
  ⚠️ L'obstacle initial à un volume paramétrable était le **nom**
  (`rescue_target_ml` = 16), pas le concept.
- ⚠️ **L'ordre des opérations de `effectiveOrpDailyTargetMlPure` reste
  INCHANGÉ** : seule la **source** de `rescueDailyMl` change (NVS au lieu de la
  constante). La condition C4 de feature-140 — clamp du plafond dur **en
  dernier**, après l'amplification Boost — **ne doit pas être rouverte**.
- Logique de pré-remplissage et de bornage **pure** (ADR-0017), testable nativement.
- Budget WS : marge actuelle **39 o**. Si un champ nouveau est nécessaire, il doit
  être **financé** (ADR-0086). Vérifier d'abord s'il est réellement nécessaire —
  la valeur peut n'être lue qu'à l'ouverture du dialogue.
- Aucune allocation sur chemin périodique (ADR-0082).

## Fichiers potentiellement concernés
- `src/constants.h` — bornes, défauts par taille, repli final
- `src/config.{h,cpp}` — clé `rescue_ml`, chaîne de pré-remplissage
- `src/temp_mode_logic.{h,cpp}` — source du volume, **ordre des clamps inchangé**
- `src/web_routes_control.cpp` — paramètre de `/rescue/start` + **clamp serveur**
- `src/mqtt_manager.cpp` — volume dans la commande + **clamp serveur**
- `data/index.html`, `data/app.js` — champ, avertissement, textes
- `test/test_native_temp_mode/` — bornes, repli, clamp
- `docs/API.md`, `docs/MQTT.md`, `docs/features/page-dashboard.md`, `docs/subsystems/pump-controller.md`, `CHANGELOG.md`, guides utilisateur

## Livrables — check-list
- [x] A1 — Clé NVS `rescue_ml`, survit aux 48 h et au redémarrage
- [x] A2 — Chaîne de pré-remplissage (1 → 2 → 3), logique pure
- [x] A3 — **Défauts par taille de bassin** dans `constants.h`
- [x] A4 — **CLAMP SERVEUR** sur `/rescue/start` ET sur la commande MQTT — l'UI n'est pas une garde
- [x] A5 — `effectiveOrpDailyTargetMlPure` : **seule la source change**, ordre des clamps intouché
- [x] B1 — Champ de volume dans le dialogue, pré-rempli, bornes appliquées
- [x] B2 — Avertissement imposé au-delà de 1500 mL
- [x] B3 — L2 révisé, L5a/L5b reparamétrées (le « 1500 » en dur devient FAUX partout)
- [x] D1 — Tests natifs : bornes, chaîne de pré-remplissage, `rescue_ml` absent/nul/hors bornes, clamp
- [x] D2 — **Mutation** : supprimer le minimum ⇒ un test meurt ; replier sur 0 au lieu de la chaîne ⇒ un autre ; déplacer le clamp du plafond dur ⇒ un troisième
- [x] D3 — **Le test C4 de feature-140, toujours manquant**, doit couvrir une **valeur saisie proche du plafond** (1900 + Boost ⇒ 2000) — `test_D3_rescue_1900_plus_boost_clamped_to_2000_never_2850`
- [x] C1 — `docs/API.md`, `docs/MQTT.md`
- [x] C2 — `docs/features/page-dashboard.md`, `docs/subsystems/pump-controller.md`
- [x] C3 — `CHANGELOG.md`
- [x] C4 — Guides utilisateur (texte source mis à jour) + capture à refaire **signalée** ci-dessous (PDF non régénérés — règle du projet, génération uniquement lors des releases)

## Sécurité chimique — **verdict `pool-chemistry` du 2026-10-05 : GO SOUS CONDITIONS**

### Bornes
- **Minimum = 100 mL/j**, ni 0 ni 5. Motif : *« un secours à 20 mL/j donnerait
  l'ILLUSION d'une désinfection sans la fournir — pire que ne pas l'armer »*.
- **Pas de 50 mL**, pas de 5 : c'est un volume journalier, un pas de 5 serait de
  la fausse précision.
- **Maximum = `min(limite journalière utilisateur si > 0, kRescueChlorineDailyHardCapMl)`**.
- **Clamp SERVEUR obligatoire**, pas seulement UI. Une valeur hors bornes est
  **ramenée dans les bornes**, jamais refusée en silence.

### Dépassement de 1500 : AUTORISÉ
1500 perd son statut de plafond et devient un **défaut**. Les vraies bornes
restent C3/C4, inchangées — un bassin de 80 m³ en a légitimement besoin.

**Avertissement imposé, affiché dès que la valeur saisie dépasse 1500, mot à mot :**
> « Au-delà de 1500 mL par jour, le bénéfice n'est pas démontré : quand l'eau est
> chargée en matière organique, l'ORP ne remonte pas tant que le point de rupture
> n'est pas atteint — augmenter le volume n'y change rien. »

### Défauts par taille de bassin
Base : 3 mg/L/jour de Cl₂ (haut de la demande réelle mesurée), chlore 36°
≈ 114 g Cl₂/L ⇒ **≈ 26 mL/m³/jour**.

| Taille | Volume de référence | Défaut |
|---|---|---|
| Petit | ≈ 20 m³ | **500 mL/j** |
| Moyen | ≈ 40 m³ | **1000 mL/j** |
| Grand | ≈ 60 m³ et plus | **1500 mL/j** |

Ces valeurs passent par le **même clamp C3/C4** que tout le reste.

### Textes imposés mot à mot
- **Libellé du champ** : « Volume de chlore par jour (mL) »
- **Aide sous le champ** : « Pré-rempli d'après votre configuration. En cas de doute, laissez la valeur proposée. »
- **L2 complet révisé** (la mention « plafonné à 1500 mL » n'a plus de sens) :
  > « Le dosage se fera à l'aveugle, sans contrôle de l'ORP, pendant 48 h au maximum. Le volume indiqué ci-dessous sera réparti sur la journée, sans jamais dépasser votre limite journalière. Le pH n'est jamais dosé dans ce mode. Il maintient une désinfection minimale — il ne traite pas la cause : quand l'eau est chargée en matière organique, seul le retrait mécanique des dépôts (brossage, aspiration, nettoyage du filtre) fait baisser cette charge. Désactivez-le dès que les sondes sont de nouveau fiables. »
- **L5a** reparamétré, `%s` = volume effectif, entier :
  > « Volume quotidien atteint (%s mL). Reprise demain. Si l'ORP ne monte pas malgré ce volume, le facteur limitant n'est pas le chlore. »
- **L5b** reparamétré, `%s` = volume effectif puis volume demandé :
  > « Volume quotidien atteint (%s mL), plafonné par votre limite journalière. Reprise demain. Ce volume est inférieur aux %s mL que vous avez demandés : on ne peut pas en conclure que le chlore n'est pas le facteur limitant. »

### Conditions bloquantes
1. **Clamp serveur** — l'UI n'est pas une garde.
2. ⚠️ **`rescue_ml` absent, nul ou hors bornes au chargement NVS ⇒ repli sur la
   chaîne de pré-remplissage, JAMAIS sur 0.** *« Un secours armé à 0 mL est un
   mode qui ment. »*
3. **L'ordre des opérations de `effectiveOrpDailyTargetMlPure` reste inchangé** —
   C4 ne doit pas être rouverte.
4. Le **test dédié C4**, toujours manquant depuis feature-140, doit désormais
   couvrir une **valeur saisie proche du plafond** (1900 + Boost ⇒ 2000).

### Ergonomie imposée
L'utilisateur doit pouvoir **valider sans rien lire** : champ pré-rempli, bouton
d'activation utilisable immédiatement, **aucun champ obligatoire à remplir**.

## ⚠️ Invariant feature-090 — point à trancher par `architect` puis `pool-chemistry`

`src/regulation_defaults.h:18-19` pose un invariant explicite sur `PoolSize` :

> « PRÉSÉLECTIONNER ces défauts, modifiables ensuite. Elle n'entre **JAMAIS** dans
> un calcul de dosage, de débit, de **quota** ou de correction »

Le pré-remplissage par taille respecte la **lettre** de l'invariant sur le chemin
HTTP : la valeur est proposée dans un champ que l'utilisateur voit et peut
modifier avant de valider.

**Mais pas sur le chemin MQTT.** Un `ON` envoyé par l'interrupteur Home Assistant
arme le mode **sans que personne ne voie ni ne modifie la valeur**. La taille du
bassin déterminerait alors un **quota journalier de chlore sans intervention
humaine** — exactement ce que l'invariant interdit.

**Deux issues, à trancher** :
- **(a)** Sur le chemin MQTT sans volume fourni, **sauter l'étage « taille de
  bassin »** : `orp_daily_target_ml` s'il est renseigné, sinon
  `kRescueChlorineDailyMl` directement. `PoolSize` ne sert alors **jamais** qu'à
  pré-remplir un champ qu'un humain regarde. L'invariant est préservé à la lettre
  comme à l'esprit.
- **(b)** Accepter l'étage taille sur ce chemin et documenter l'écart à
  l'invariant, en le justifiant.

⚠️ **Ce n'est pas une régression introduite par cette spec** : aujourd'hui déjà,
un `ON` MQTT arme le mode à la constante 1500 sans que l'utilisateur la voie. Ce
qui change, c'est que la valeur **dériverait d'un paramètre de configuration du
bassin**. L'architecte doit dire explicitement comment son design se situe, et le
gate 2 vérifier le résultat.

**(a) est recommandée** par l'agent principal : elle coûte une ligne et ferme la
question au lieu de la documenter.

## Faits établis par l'analyse d'impact (2026-10-05)

### ⚠️ Les défauts par taille NE VONT PAS dans `kRegulationDefaultsBySize`
L'en-tête de `src/regulation_defaults.h` porte un visa explicite :

> « PÉRIMÈTRE ARBITRÉ par pool-chemistry (visa 31/08/2026) : uniquement du
> tuning. JAMAIS de … **quota** … ni de SafetyLimits »

Un volume journalier de secours **est** un quota de dosage. La table par taille
du secours doit donc être une **structure SÉPARÉE** (`constants.h` ou
`temp_mode_logic.h`), **jamais** une extension de `kRegulationDefaultsBySize`.
Étendre cette table violerait un arbitrage chimique vieux d'un mois.
⚠️ Signal d'alarme pour la revue : si `test_native_regulation_defaults` doit être
étendu, c'est que le périmètre a été violé.

### ⚠️ `data/app.js:4071` est un VRAI BUG, pas un commentaire
`let vol = 1500;` dans `_rescueEffectiveDailyMl()`. Cette fonction alimente le
**bandeau actif**, recalculé à **chaque rafraîchissement `/data`**, pas seulement
à l'ouverture du dialogue. Correct aujourd'hui (le volume EST 1500) ; **faux dès
qu'il devient variable** : un bassin « petit » (défaut 500) ou un volume saisi à
1900 afficherait un seuil L5 erroné.

**Correctif imposé** : exposer le volume réellement armé côté client.

⚠️ **CORRECTION du 2026-10-05 — la recommandation initiale de cette section était
FAUSSE et se contredisait avec le plan d'architecture.** Elle disait « via
`/get-config`, PAS via la trame WS », en confondant **deux budgets distincts** :
- la trame **`sensor_data`** a 39 o de marge (ADR-0086) — c'est elle qu'il ne faut
  pas toucher, et elle n'est **pas** concernée ici ;
- la trame **`config`** dispose d'environ 572 o de marge — c'est la cible.

Et surtout : `/get-config` **seul** ferait **CLIGNOTER** le champ, puisque
`window._config` est **REMPLACÉ** par la trame WS `config` (`data/app.js:2901`).
C'est la récidive « champ servi d'un seul côté » que ce projet a déjà connue
trois fois.

**Décision retenue (plan d'architecture)** : `rescue_ml` exposé **des DEUX côtés**,
`/get-config` **et** trame WS `config`, entre les marqueurs `PARITE-CONFIG`.
Coût compté : `"rescue_ml":2000,` = **17 o**, gabarit documentaire
`StaticJson<3008>` → `<3072>`. `test_native_ws_config_parity` **impose** cette
parité — c'est précisément ce qui ferme la récidive.

### Le chemin MQTT : l'option (a) est IMPOSÉE, pas seulement recommandée
Le topic Rescue est un **switch Home Assistant binaire** (`ON`/`OFF` bruts,
`mqtt_manager.cpp:2052-2080`). Aucun champ numérique ne peut y transiter sans
changer le type d'entité HA — hors périmètre. Sur `ON` sans volume : saut de
l'étage « taille de bassin », repli direct sur `orp_daily_target_ml` si renseigné,
sinon `kRescueChlorineDailyMl`. **`PoolSize` n'est jamais consultée sur ce chemin**,
ce qui préserve l'invariant feature-090 à la lettre comme à l'esprit.

### Occurrences de « 1500 » à traiter — liste établie, pas estimée
**Firmware** : `constants.h:3345-3346` (commentaire « 3× le besoin », rationale
corrigée par le gate) · `config.h:286` · `config.cpp:1361` (log) ·
`pump_controller.cpp:2667` (**le point d'implémentation central**).
**Interface** : `app.js:4064-4065`, `:4068`, **`:4071` (le bug)**, `:4113`,
`:4117` (comparaison câblée), `:4119` (L5b), `:4122` (L5a), `:4226` (L2).
**À NE PAS toucher** : `constants.h:3347` (la constante elle-même reste valide,
elle change de rôle) · `temp_mode_logic.h:94` (l'exemple 1500 × 1,5 = 2250 reste
exact) · `app.js:4074` (`hardCap = 2000`, inchangé).

### Risque de régression des tests : faible
`effectiveOrpDailyTargetMlPure` est **déjà** paramétrée en `rescueDailyMl` — aucun
changement de signature. Les tests existants utilisent une constante locale
`kRefRescueDailyMl`, pas celle du firmware. Le changement attendu est **additif** :
le test C4 manquant (1900 + Boost ⇒ 2000). Vérifié : `grep 1900` ne renvoie rien.

## Chemin MQTT — arbitrage tranché (2026-10-05)

Le gate autorisait l'héritage de la dernière valeur saisie **à condition** d'une
borne de fraîcheur de 7 jours, coûtant une seconde clé NVS `rescue_ml_at` et une
logique de péremption. Il offrait explicitement l'alternative :
*« si tu ne veux pas de cette clé, alors on n'hérite pas »*.

**DÉCISION : pas d'héritage.** Sur `ON` MQTT, la chaîne est
`orp_daily_target_ml` si renseigné → `kRescueChlorineDailyMl`. La valeur stockée
n'est **pas** réutilisée, `PoolSize` n'est **jamais** consultée.

Motif : le cas que l'héritage protégerait est étroit — il faut avoir saisi un
volume personnalisé, **et** réarmer depuis Home Assistant plutôt que l'interface,
**et** n'avoir aucun `orp_daily_target_ml` renseigné (sinon le repli rend déjà la
valeur propre de l'utilisateur). Un état daté supplémentaire à maintenir et à
tester pour un cas aussi étroit n'est pas un bon échange.

### Les deux exigences d'annonce du gate sont CONSERVÉES — elles valent seules

1. **Log d'armement nommant le volume ET SA PROVENANCE** — « cible programmée »
   ou « défaut ». Aujourd'hui `config.cpp:1358` donne le volume mais pas d'où il
   vient ; après coup, on ne peut pas savoir avec quoi le mode a tourné.
2. **Topic MQTT retenu portant le volume journalier**, avec auto-discovery HA
   (`sensor`). ⚠️ L'invisibilité est un problème **spécifiquement Home
   Assistant** : elle se corrige là où elle se produit, pas dans une interface web
   que l'utilisateur HA ne regarde pas. MQTT n'a aucune contrainte de budget.

⚠️ **Ne RIEN ajouter à la trame WebSocket pour cela** : la marge est à 39 o après
`rescue_lift`, et le bandeau porte déjà l'information pour qui regarde l'interface.

## ADR attendu ?
- **Probable** : non — le mécanisme est déjà couvert par ADR-0087, seule la
  source du volume change. Si l'architecte identifie une contrainte structurante,
  qu'il le signale.

## Éléments visibles (pour doc-writer)
- `/rescue/start` accepte un volume ; commande MQTT idem.
- Champ nouveau dans le dialogue d'activation.
- Les messages de plafond portent désormais le volume réel, plus « 1500 ».

## Critères d'acceptation
- [x] Le volume est saisi à l'activation, pré-rempli, et l'activation est possible **sans toucher au champ**
- [x] Une valeur hors bornes est **ramenée** dans les bornes côté serveur
- [x] `rescue_ml` invalide ⇒ repli sur la chaîne, **jamais 0 mL**
- [x] Une valeur > 1500 est acceptée **avec** l'avertissement imposé
- [x] Aucun message n'affiche « 1500 » en dur — seule exception **voulue** : le texte imposé de l'avertissement B2 lui-même, qui nomme 1500 comme seuil de bascule du bénéfice démontré, pas comme plafond
- [x] L'ordre des clamps est inchangé — vérifiable par `git diff` (`temp_mode_logic.cpp`/`.h` : seule la signature/provenance de `rescueDailyMl` change, la séquence amplification → clamp utilisateur → clamp dur est identique)
- [x] 1900 mL saisis + Boost ⇒ volume effectif **2000**, jamais 2850
- [x] `pio run` sans nouveau warning ; `pio test -e native` vert ; mutants tués

## Plan de test manuel
1. Activer sans toucher au champ : le pré-remplissage s'applique.
2. Saisir 1900 avec Boost actif : vérifier 2000 au plafond.
3. Saisir 10 : ramené à 100.
4. Activer par MQTT sans volume : chaîne de pré-remplissage.
5. Redémarrer pendant le mode : le volume saisi survit.

## Notes pour agent

⚠️ **Ne pas rouvrir C4.** Seule la SOURCE du volume change. L'ordre
`volume → ×1,5 Boost → clamp utilisateur → clamp plafond dur EN DERNIER` est
intouchable : l'inverser autorise 2850 mL là où le plafond en permet 2000.

⚠️ **Le « 1500 » en dur devient FAUX partout** dès que la valeur est saisie.
Chercher toutes ses occurrences dans `data/` avant de livrer.

⚠️ **L'UI n'est pas une garde.** Le clamp serveur est une condition bloquante,
pas une ceinture de sécurité optionnelle.

## Notes d'implémentation (2026-10-05)

### Décisions retenues
- **Deux fonctions pures distinctes** (`src/temp_mode_logic.h`/`.cpp`, ADR-0017) : `clampRescueDailyMlPure` (valeur explicite, bornage seul) et `resolveRescueDailyMlPure` (valeur absente/hors bornes, chaîne de pré-remplissage puis **même** clamp). Jamais fusionnées — `clampRescueDailyMlPure` est appelée une seule fois en bout de chaîne, zéro duplication du bornage.
- **`startRescueChlorine()` élargie à `(float requestedDailyMl)`** : `> 0` = saisie HTTP explicite (bornée seule) ; `<= 0` = aucune valeur fournie (MQTT `ON`, ou `?ml=` absent) — résolue **sans étage taille de bassin**, conformément à l'invariant feature-090.
- **Table de défauts par taille SÉPARÉE** (`kRescueChlorineDailyBySizeMl[3]`, `constants.h`) de `kRegulationDefaultsBySize` — le visa `pool-chemistry` du 31/08/2026 sur cette dernière interdisait explicitement tout quota ; `rescueDailyMlForDisplay()` (`config.cpp`) en est le **seul** lecteur.
- **Chemin MQTT `ON` sans volume (option (a) imposée)** : saute l'étage taille, `PoolSize` n'est **jamais** consultée — invariant feature-090 préservé à la lettre comme à l'esprit. **Pas d'héritage** de la dernière valeur saisie (arbitrage tranché : coût d'une seconde clé NVS de fraîcheur jugé disproportionné pour un cas trop étroit).
- **Clé NVS `rescue_ml`** (9 caractères, 3e champ de la même transaction Preferences que `rescue_active`/`rescue_until`) — `RescueState` étendu d'un `float dailyMl`.
- **Exposition `rescue_ml`** sur `/get-config`, trame WS `config` (parité stricte, un seul lecteur firmware `rescueDailyMlForDisplay()`) et topic MQTT dédié `{base}/rescue_ml` (sensor HA, lecture seule) — **rien** ajouté à la trame `sensor_data` (marge 39 o après `rescue_lift`, non entamée).
- **Correctif B6** : `_rescueEffectiveDailyMl(cfg)` (app.js) lisait `let vol = 1500;` en dur — vrai bug de la feature-140, correct par coïncidence tant que le volume valait exactement 1500, faux dès qu'il devient variable. Corrigé pour lire `cfg.rescue_ml`.
- **Log d'armement** nomme désormais le volume et sa provenance (« saisi » / « cible programmée » / « défaut ») — absent avant cette feature.

### Les deux verdicts `pool-chemistry`
- **Gate initial (arbitrage)** : GO SOUS CONDITIONS — plancher 100 mL/j figé, plafond `min(limite utilisateur, 2000)`, défauts par taille, textes imposés (libellé, aide, L2 révisé, L5a/L5b, avertissement B2), 4 conditions bloquantes (clamp serveur, jamais 0 au chargement NVS, ordre des clamps C3/C4 intouché, test C4 manquant à combler).
- **Gate sur le diff réel** : les 4 conditions vérifiées ligne à ligne — clamp serveur présent sur les deux chemins (HTTP + MQTT), `loadRescueState()` replie bien sur la chaîne (jamais 0), `temp_mode_logic.cpp` montre l'ordre `amplification Boost → clamp utilisateur → clamp dur` strictement inchangé, test dédié 1900+Boost⇒2000 présent et nommé. Point corrigé en gate 2 (revue de code — **bloquant, seul restant**) : la documentation (ce tour).

### Tests et mutants
Suite `test/test_native_temp_mode/` étendue de tests dédiés à `clampRescueDailyMlPure` (plancher, plafond utilisateur souverain même sous le plancher, plafond dur) et `resolveRescueDailyMlPure` (chaque étage de la chaîne, `storedMl` absent/négatif/hors plafond), plus `test_D3_rescue_1900_plus_boost_clamped_to_2000_never_2850` (la dette de test héritée de feature-140, enfin comblée). Validation par **mutation** (ADR-0073, ne pas recopier de compteur) : suppression du plancher, repli sur 0 au lieu de la chaîne, et déplacement du clamp du plafond dur font chacun mourir un test dédié — 3 mutations, 3 morts.

### Dette C4 de feature-140 soldée
Le test « valeur saisie proche du plafond » (1900 + Boost ⇒ 2000), identifié comme manquant dès la clôture de feature-140, est livré avec cette feature (D3 ci-dessus) — plus de dette ouverte sur `effectiveOrpDailyTargetMlPure`.

### ADR
Aucun créé — voir rapport doc-writer : le mécanisme reste couvert par ADR-0087 (décision structurante déjà actée), le budget de trame par ADR-0086, la pureté de la logique par ADR-0017. Seule la source d'un volume déjà borné change ; aucun des 4 critères de déclenchement d'ADR n'est rempli.

## Hors périmètre
- L'estimation automatique depuis l'historique (écartée, voir Contexte).
- Toute modification des limites journalières, du plafond dur ou de la durée de 48 h.
- Le mode Programmée et son `orp_daily_target_ml`, qui restent inchangés.
