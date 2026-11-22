## Métadonnées
- **ID** : bug-211
- **Slug** : un-bidon-vide-est-annonce-comme-un-stock-faible
- **Type** : bug de libellé **+ ALERTE neuve**. ⚠️ Recadrée **deux fois** le 2026-11-22 : « libellé seul » → « B, avec arrêt du dosage » → ✅ **« alerte forte, AUCUN arrêt »** (décision finale, voir § Arbitrage). ⛔ **Aucune coupure de dosage n'est introduite.**
- **Statut** : **done** — implémenté, tests verts, revue passée et constats corrigés, rendu vérifié sur capture 390 px (reste en `doing/` tant qu'aucun commit ne le référence)
- **Priorité** : à définir
- **Version cible** : **v3.0.3**
- **Dernière mise à jour** : 2026-11-22
- **Spec(s) liée(s)** : [bug-199](../done/bug-199-deux-conventions-decimales-sur-le-meme-ecran.md)
  (⭐ **c'est son étude d'ergonomie qui a sorti ce sujet de son périmètre, à dessein**)

## Domaines touchés
- [x] Frontend (`data/`) — un libellé, à **deux** endroits
- [ ] Régulation · [ ] Capteurs · [ ] MQTT · [ ] Web

## Agents requis
- [x] **pool-chemistry** — 🔴 **OBLIGATOIRE** : le comportement du dosage change
- [x] orchestrator · [x] architect · [x] ux-ui-designer · [x] web-ui-developer · [x] **embedded-developer** · [x] test-engineer · [x] code-reviewer
- [x] **doc-writer** — ⚠️ un libellé **visible** change

## Description
Quand le bidon est **vide**, l'écran annonce **« Stock faible (0,0 L) »**.
⇒ ⭐ *« Faible » dit « pensez à en racheter ». « Vide » dit « la régulation ne peut plus doser ».*
⛔ Ce n'est pas la même information, et c'est la seconde qui compte.

## Observé — 2026-11-22, sur l'appareil réel
Capture du tableau de bord à 390 px, carte **ORP** : **« Stock faible (0.0 L) »**, dans l'état
**courant** de la machine — ⛔ pas un cas limite à provoquer.

## 🔬 Ce que « vide » veut dire RÉELLEMENT — lu dans le code le 2026-11-22

⛔ La fiche exigeait de ne pas écrire le texte sans avoir lu ça. Voici la réponse.

### 1. L'appareil ne mesure RIEN — il soustrait
`src/web_routes_data.cpp:774` et `:778`, `src/ws_manager.cpp:1152` et `:1156`,
`src/mqtt_manager.cpp:829` :
```
remaining_ml = max(0, contenanceDéclarée − volumeInjectéCompté)
```
⇒ Deux grandeurs **déclarées ou comptées**, ⛔ **aucune mesure de niveau**.

### 2. 🔴 « 0,0 L » ne veut PAS dire « vide » — la valeur SATURE
Le `max(0, …)` **borne à zéro**. ⇒ L'écran dit « 0,0 L » dès que le compteur **atteint ou
dépasse** la contenance déclarée, et il **y reste**, potentiellement des jours.
⭐ Le fait réel n'est pas *« le bidon est vide »* mais *« **le compteur est arrivé au bout de
ce qui a été déclaré** »*. ⚠️ Ce sont deux énoncés différents, et seul le second est vrai.

### 3. 🔴 Le bidon ne bloque RIEN — l'appareil dose toujours à 0,0 L
`grep ContainerVolumeMl` sur `src/` : la contenance n'apparaît **que** dans
`web_routes_data.cpp`, `ws_manager.cpp`, `mqtt_manager.cpp` et `config.*` —
⛔ **jamais dans `pump_controller.cpp` ni `dosing_logic.h`**.
⚠️ Les `remainingMl` de `pump_controller.cpp:4867` et `:5346` sont le **reliquat JOURNALIER**,
⛔ sans aucun rapport avec le bidon.
⇒ **À « 0,0 L », la régulation continue d'injecter.** C'est le fait que l'écran ne dit pas.

### 4. ⚠️ Et le compteur DÉRIVE
Deux campagnes relevées sur la pompe ORP : **+5,11 %** puis **+6,25 %**. ⛔ Ni le modèle en %
ni le modèle par démarrage n'est constant. Le point faible est **la contenance déclarée**.

### ⇒ Conséquence pour le texte
⛔ **« Bidon vide » est une affirmation que l'appareil n'a pas les moyens de faire.** Ce serait
**le même défaut que « Hors gel »** ([ADR-0141](../../../docs/adr/0141-l-appareil-est-temoin-du-froid-jamais-agent.md)) :
un mot qui promet une connaissance absente. ⭐ Le vrai défaut n'est pas « faible » au lieu de
« vide » : **c'est que l'écran parle du BIDON alors que l'appareil ne connaît que son COMPTEUR.**

## 🔴 DIRECTION ARRÊTÉE PAR L'UTILISATEUR — « B », le 2026-11-22
> *« B. C'est le choix de la prudence. »*

**B** = le message dit la vérité du compteur, **ET** le compteur à zéro devient **une raison de
s'arrêter ou d'alerter fortement** — ⇒ **le comportement de la machine change**.

### ⚠️ Objection posée à l'utilisateur, et pourquoi elle ne clôt pas le sujet
**La prudence ne pointe pas dans un seul sens.** Un compteur qui **sur-compte** de 5 à 6 %
**atteint zéro AVANT que le bidon ne soit vide**. ⇒ Si zéro arrête le dosage, l'appareil
**cesse de chlorer un bassin qui a encore du produit**.
⭐ Et une eau qui n'est plus désinfectée est **le seul des deux sens qui touche à la santé des
baigneurs**. ⇒ *« Arrêter » est prudent côté pompe, imprudent côté eau.*
⚠️ C'est exactement le principe posé par l'utilisateur lui-même : **une garde qui bloque plus
qu'elle ne protège est un défaut**.
⇒ **La direction B est retenue** ; c'est **`pool-chemistry` qui arbitre la FORME** (arrêt franc ·
arrêt après une marge au-delà du zéro · alerte forte sans arrêt).


## ✅ ARBITRAGE — décision FINALE du 2026-11-22 : **alerte forte, AUCUN arrêt**

### Le chemin, parce qu'il est instructif
1. Direction **A** (dire la vérité, rien d'autre) vs **B** (zéro = raison de s'arrêter).
2. L'utilisateur choisit **B** — *« c'est le choix de la prudence »*.
3. ⚠️ Objection posée : la prudence **ne pointe pas dans un seul sens**. Un compteur qui
   **sur-compte** atteint zéro **avant** que le bidon ne soit vide ⇒ arrêter, c'est **cesser de
   chlorer un bassin qui a encore du produit**.
4. `pool-chemistry` rend un **GO SOUS CONDITIONS asymétrique** : ⛔ jamais d'arrêt sur l'ORP,
   arrêt sur le pH **au-delà d'une marge** (×1,10, plancher +500 ml).
5. 🔴 **L'utilisateur tranche la contradiction interne du rapport** : *« pourquoi arrêter le
   dosage si la pompe péristaltique ne s'abîme pas ? »*
   ⇒ Le rapport écrit lui-même, en son point 1, qu'un arrêt **« n'ajoute aucune protection
   sanitaire : il achète seulement l'attention de l'utilisateur »** — ⛔ **puis recommande
   quand même l'arrêt.** Les deux ne tiennent pas ensemble.
6. ✅ **Décision retenue : AUCUN arrêt, sur aucun axe.** Alerte forte et permanente à la place.

### Pourquoi l'arrêt perd
| Ce qu'il protégerait | Verdict |
|---|---|
| la **pompe** | ⛔ non — une péristaltique est **auto-amorçante**, tolère la marche à sec indéfiniment, et l'usure du tube est la même pleine ou vide |
| la **chimie** | ⛔ non — le sur-dosage est **déjà borné** par les limites par injection / horaire / journalière |
| l'**attention** de l'utilisateur | ✅ oui — ⛔ mais c'est le rôle d'une alerte, **pas d'un arrêt** |

**Et son coût est réel.** 🔴 **Les deux axes sont COUPLÉS** : arrêter l'acide fait monter le pH ;
au-delà de `kPhOrpInhibit`, **l'inhibition coupe AUSSI la boucle ORP**. ⇒ On créerait une
**coupure de désinfection par ricochet (~48 h)**, pour signaler un bidon dont on n'est **même pas
sûr** qu'il soit vide.
⭐ C'est exactement le principe maison : **une garde qui bloque plus qu'elle ne protège est un
défaut.**

### Le meilleur argument POUR l'arrêt, et pourquoi il ne suffit pas
Bidon réellement vide ⇒ l'appareil **croit corriger** : il commande des injections qui ne
délivrent rien **et incrémente son compteur de volume injecté** à chaque fois — ⚠️ il **abîme la
donnée même** qui sert à mesurer la dérive.
⇒ Réel, mais ça justifie de **ne plus COMPTER**, ⛔ pas de **s'ARRÊTER**. Deux choses différentes.
ⓘ À verser à la réflexion sur le biais de comptage.

### 🔴 ERREUR DE FAIT dans le rapport `pool-chemistry` — corrigée ici
Il écrit : *« la régulation est un **P pur** (`Ki = 0`, feature-025), pas de terme intégral ⇒ pas
de windup ⇒ pas de bolus au réamorçage »*.
⛔ **FAUX pour l'acide.** `src/constants.h:2380` : « **LEVÉE de `Ki = 0` (feature-025) sur le SEUL
pH**. Ki_ORP reste 0 ». ⇒ **Le pH A un terme intégral** depuis la v2.26.0 ; seul l'**ORP** est en
P pur.
ⓘ Ça ne renverse pas la décision (on n'arrête rien), ⚠️ **mais** : un terme intégral qui continue
d'agir sur un bidon vide **accumule une demande non servie**. ⇒ **À vérifier avant de coder** :
le gel d'intégrale (`kPhIntegralFreezeDailyFraction = 0,80`, `src/constants.h:2534`) couvre-t-il
ce cas, ou faut-il l'armer sur l'évidence « je dose et rien ne bouge » ?

### 🔴 PIÈGE VÉRIFIÉ — une contenance à 0 est acceptée
`src/web_routes_config.cpp:1714` : `phContainerVolumeMl = max(0.0f, …)` ⇒ **0 est une valeur
valide**. Et `max(0, 0 − injecté)` vaut **0**. ⇒ Un appareil non configuré est **« compteur au
bout » dès le premier démarrage**. ⛔ Toute alerte doit exclure ce cas (contenance < 1 000 ml, ou
suivi désactivé), sinon elle crie dans le vide chez chaque nouveau client.

### ✅ Le fait MESURÉ, à préférer au compteur
`*_ttt_state == 3` = `kTttStateMissed` (`src/time_to_target_logic.h:65`) : **« journée close sans
que la cible ait été tenue »**. Déjà servi sur `/data` (`web_routes_data.cpp:806/810`) et WS
(`ws_manager.cpp:1312`).
⚠️ **Nuance à ne pas écraser** : c'est un verdict **à la JOURNÉE**, ⛔ pas un signal immédiat.
⇒ **Deux ou trois journées consécutives** en état 3 **pendant qu'on dose** sont la vraie signature
d'un bidon vide — ⭐ sans matériel, sans déclaration, **sans dépendre de la dérive**.
⇒ **Le compteur dit quand s'inquiéter ; la mesure dit si c'est vrai.**

### Ce qui reste à faire, et ce qui est écarté
- ✅ **Les deux emplois du libellé** (`data/app.js:5215` et `:5405`) — ⛔ indissociables.
- ✅ Une **alerte forte et permanente**, sur les **deux** axes, qui ne s'éteint qu'à la
  confirmation de remplissage (geste existant `ph_reset_container` / `orp_reset_container`).
- ✅ **Texte** : ⛔ « Bidon vide » **interdit** — l'appareil connaît son **compteur**, pas son
  bidon. Même défaut que « Hors gel »
  ([ADR-0141](../../../docs/adr/0141-l-appareil-est-temoin-du-froid-jamais-agent.md)).
- ⛔ **ÉCARTÉ** : tout arrêt de dosage, sur les deux axes. Les conditions C2, C4 et C6 du rapport
  `pool-chemistry` **tombent avec lui** ; C3 (contenance 0), C5 (jamais silencieux), C7 (sortie
  par le geste existant) et C8 (texte) **restent**.


## 🔴 Les DEUX emplois — ⛔ indissociables
| Fichier | Ce qui s'affiche | Où |
|---|---|---|
| `data/app.js:5215` (⚠️ était 5143) | `Stock faible (${…} L)` | bandeau de la carte pH/ORP du **tableau de bord** |
| `data/app.js:5405` (⚠️ était 5336) | `status.textContent = isLow ? 'Stock faible' : 'OK'` | **badge** des cartes de dosage |

⛔ **Traiter l'un sans l'autre est interdit** : on obtiendrait « Bidon vide » d'un côté et
« Stock faible » de l'autre, pour le même état. ⭐ C'est **exactement** la seconde forme que le
projet combat — *un design system appliqué à moitié rend l'interface MOINS cohérente qu'avant*.

## Comportement attendu (proposition de l'ergonomie, ⛔ à valider)
- reste **nul** (ou arrondi à 0,0 L, c'est-à-dire sous ~50 mL) ⇒ **« Bidon vide »**
- sinon ⇒ **« Stock faible (0,3 L) »** *(inchangé)*
⚠️ Le seuil « sous 50 mL » est une **proposition**, ⛔ pas une mesure. À arrêter.

## ⚠️ Ce qu'il faut vérifier avant de rédiger le texte
🔴 **Que se passe-t-il réellement quand le bidon est vide ?** ⛔ Ne pas l'écrire sans l'avoir lu
dans le code. L'appareil **compte** un volume délivré, ⛔ il ne **mesure** pas le niveau du bidon.
⇒ « Vide » signifie ici *« le compteur dit qu'il ne reste rien »*, ⛔ pas *« l'appareil a constaté
que le bidon est vide »*. ⚠️ **Le texte ne doit pas affirmer plus que ça.**
ⓘ Et le projet a déjà mesuré que ce compteur **dérive** : biais relevé sur la pompe ORP, deux
campagnes, ⛔ ni le modèle en % ni le modèle par démarrage n'est constant.

## Contraintes
- ⛔ **Aucune logique de dosage touchée** : seuls des libellés changent.
- ⛔ Le badge reste un `state-badge--warn` : ⚠️ ne pas inventer un troisième état visuel.
- ⛔ Pas de point décimal (acquis de bug-199). ⚠️ « Bidon vide » **n'affiche aucun nombre** : c'est
  aussi ce qui le rend plus clair que « 0,0 L ».
- 390 px d'abord — ⭐ « Bidon vide » (10 car.) est **plus court** que « Stock faible (0,3 L) ».

## Livrables — check-list
- [x] A1 — lire le code : d'où vient le « reste », et ce que « vide » veut **honnêtement** dire
      *(fait, et consigné dans la section « 🔬 Ce que “vide” veut dire RÉELLEMENT »)*
- [x] B1 — les **deux** emplois changés **ensemble**
      ⚠️ **Résultat différent de l'hypothèse de la fiche** : le second emploi
      (`status.textContent = isLow ? 'Stock faible' : 'OK'`) visait
      `#product-<prefix>-status`, **un id absent de tout le HTML** — `$()` rendait `null`, le
      `if` était toujours faux. ⇒ **CODE MORT, supprimé.** Il n'y a donc **qu'un seul** emploi
      réel, celui du tableau de bord, et l'incohérence que la fiche redoutait **ne pouvait pas
      se produire**.
- [x] D1 — vecteurs : reste nul · reste faible · reste normal · reste **absent**
      (`test/decimal_format_vectors.js`, logique extraite en fonction pure `bidonCounterState()`)
- [x] D2 — capture 390 px, avant / après *(c'est elle qui a fait écarter la rédaction
      « constat + ajout », qui tenait sur six lignes de rouge)*
- [x] C1 — `docs/features/page-dashboard.md`, `page-dosages.md`, `CHANGELOG.md`
      **+ au-delà du prévu** : [ADR-0142](../../../docs/adr/0142-un-compteur-de-bidon-sature-alerte-il-n-arrete-jamais-le-dosage.md),
      `docs/DESIGN_SYSTEM.md` §5.9bis, `docs/user/src/guide-utilisation.html`, `src/version.h` (v3.0.3)

## Suivi / itérations

### Itération unique — 2026-11-22 — **tests verts ; revue passée, constats corrigés**

**Portée réellement livrée** — ⛔ zéro ligne de firmware.

| Fichier | Ce qui change |
|---|---|
| `data/app.js` | `bidonCounterState()` (fonction **pure**, cascade `configure`/`zero`/`low`/`ok`) ; `buildSensorSegments()` branché dessus ; `setBadge()` sait rendre un `<a>` pour un segment porteur d'un `href` ; seuil d'alerte affiché en litres entiers ; suppression du bloc mort `#product-<prefix>-status` |
| `data/app.css` | `.sensor-badge__cta` (cible 44 px, `color:inherit`, `:focus-visible`) et `.sensor-badge__explain` |
| `data/index.html` | `step="0.5"` → `step="1"` sur `#ph_alert_threshold_l` et `#orp_alert_threshold_l` |
| `test/decimal_format_vectors.js` | vecteurs des quatre états + mutation |
| `src/version.h` | `3.0.2` → `3.0.3` |
| docs | `CHANGELOG.md`, `docs/features/page-dashboard.md`, `page-dosages.md`, `docs/DESIGN_SYSTEM.md`, `docs/user/src/guide-utilisation.html`, `docs/adr/0142-*.md` + `docs/adr/README.md` |

**Décisions formalisées** : [ADR-0142](../../../docs/adr/0142-un-compteur-de-bidon-sature-alerte-il-n-arrete-jamais-le-dosage.md)
— le non-arrêt du dosage et l'interdiction du mot « vide » sont des **décisions**, pas des
tournures ; elles ne sont lisibles **nulle part** dans le code (c'est une non-action).


#### Ce que la revue de code a trouvé — et ce qui a été corrigé (2026-11-22)

La revue a rendu **Request changes** avec trois constats bloquants. Les trois étaient **réels**,
vérifiés dans le code avant correction.

| # | Constat | Correction |
|---|---|---|
| B1 | La cascade ne lisait **jamais** `*_tracking_enabled`, qui vaut **`false` en sortie d'usine** (`src/config.h:327`). Un appareil neuf affichait une invitation permanente — et le lien menait à une page dont `applyProductTracking()` masque précisément le corps de carte : **un cul-de-sac**. C'est la condition **C3 du rapport `pool-chemistry`**, retenue par cette fiche et non implémentée. | Garde `=== true` dans `buildSensorSegments()` |
| B2 | Faux positif **au démarrage** : `loadConfig()` appelle `updateSensorBadges()` alors que `latestSensorData` vaut encore `null` ⇒ branche `configure` sur un appareil pourtant configuré, et **indéfiniment** si `/data` ne répond pas. | **Le même garde** le ferme (`undefined !== true`) |
| B3 | Le texte affichait « le compteur dépasse la réalité de **5 à 6 %** » — chiffre relevé sur la **seule pompe ORP**, montré aussi sur l'axe pH, et démenti par la fiche elle-même (« ni le modèle en % ni le modèle par démarrage n'est constant »). ⚠️ **Le lot bannissait « vide » puis réintroduisait un chiffre non étayé trois mots plus loin.** | Fourchette retirée, du code **et** de la doc |

Trois constats mineurs ont été traités dans le même lot :

- **m1** — `.sensor-badge__link` : déclaré deux fois en CSS, employé **zéro** fois. **Supprimé** — garder
  les deux aurait ajouté une troisième forme là où le design system demande d'en retirer une.
- **m2** — le seuil d'alerte affiché **mentait** : 2 500 ml s'affichait « 3 », et 300 ml s'affichait
  « **0** », qui signifie *pas d'alerte* dans la sémantique de cette même interface. L'aide sous le
  champ donne désormais la valeur exacte quand elle n'est pas un nombre entier de litres. ⛔ Aucune
  réécriture silencieuse de la configuration.
- **m3** — priorité **0,5 → 2,5** : un pH hors plage (priorité 1) et les limites de dosage (2) passent
  désormais **avant** le consommable. « Les plafonds protègent, le reste informe. »

**Deux vecteurs neufs**, tous deux **tués par la mutation** (`tracked = true` ⇒ les deux tombent) :
« suivi de produit désactivé ⇒ AUCUN segment de stock » et « démarrage (aucune donnée reçue) ⇒
AUCUN segment de stock ». **53/53 vecteurs conformes**, 2 734 cas natifs verts, `data/app.js`
restauré à l'identique après mutation (`diff` vide).

#### Le rendu, regardé et non déduit — livrable D2

Captures à **390 px**, état forcé via `ui_shot.sh -n -f` :
`/tmp/claude-0/-root-esp32-pool-controller/998bfba8-fb0a-4232-bd9b-381cf4a0a742/scratchpad/b211/`

⛔ **Un défaut que seule l'image a montré** : le chevron de l'invitation, posé en `::after`,
**tombait seul sur une deuxième ligne et débordait du cadre** (la règle `.sensor-badge--stacked > a`,
plus spécifique, rend le lien `block`). Deux tentatives en CSS ont échoué ; la bonne réponse était
d'adopter **le motif déjà en place dans l'application** — `<span aria-hidden="true">›</span>` à
l'intérieur du lien, comme les cinq liens « Tous les réglages › » de `data/index.html` — construit
par `document.createElement()`, donc **sans aucune interpolation HTML** (garde dédiée verte).

⛔ **Et un fait relevé sur l'appareil réel** pendant la capture : `orp_remaining_ml = 0` et
`orp_ttt_state = 3`. Le compteur du bidon de chlore **est effectivement au bout**, et la cible du
jour n'est pas tenue — l'état que ce lot vient d'apprendre à nommer est **en cours sur le bassin**.

**Resté ouvert, écrit et non traité** :
1. ⚠️ La page **Produits** (`/dosages`) affiche **toujours « 0,0 L »** quand le compteur sature.
2. ⚠️ L'invitation « Contenance du bidon à renseigner » est un segment `info` : `composeSensorBadge()`
   ne garde un `info` que si **aucun** `danger`/`warning` n'existe pour ce capteur ⇒ elle est
   **masquée** quand le capteur est déjà en alerte. Arbitrage accepté, aucune exception à la
   règle de composition.
3. ⓘ « **Ne plus COMPTER** au-delà du zéro » (le vrai dommage d'un bidon réellement vide : le
   compteur continue de s'incrémenter et pollue la donnée de dérive) — **non traité**, à verser
   à la réflexion sur le biais de comptage.
4. ⓘ Question ouverte par la fiche et **non refermée** : le gel d'intégrale pH
   (`kPhIntegralFreezeDailyFraction`) couvre-t-il le cas « je dose et rien ne bouge » ? ⛔ Aucune
   ligne de firmware n'a été touchée, la question reste entière.
