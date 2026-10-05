# ADR-0087 — Ce qu'un mode temporaire a le droit de faire sans qu'on le lui rappelle

- **Statut** : Accepté
- **Date** : 2026-10-05
- **Décideurs** : pool-chemistry (GO sous conditions, gate 1 et gate 2), architect
- **Spec(s) liée(s)** : feature-140 (chloration-de-secours-et-widget-des-modes-temporaires), feature-053 (mode-boost, le patron suivi)

## Contexte

Après l'incident du 2026-10-02 (pluies diluviennes, boue au fond du bassin), les
deux voies de mesure pH et ORP ont été déclarées instables et **tout dosage
automatique s'est arrêté, chlore compris**, au moment précis où la demande en
chlore était maximale. Le mécanisme capable de continuer à doser existait déjà
— le mode `scheduled` (feature-011), aveugle à la mesure par construction — mais
il n'était accessible qu'en **mutant une configuration persistée** (`orp_regulation_mode`
en NVS), un changement que rien ne borne dans le temps et que rien ne rappelle
de défaire. La feature-140 introduit la **Chloration de secours**, une
deuxième surcouche temporaire du même genre que le Mode Boost (feature-053,
[ADR-0025](0025-mode-boost.md)).

Deux questions structurantes se posent, qui ne sont pas spécifiques à cette
seule feature :

1. **Comment une surcouche qui n'écrit jamais la configuration persistée peut-elle
   gouverner une décision de dosage ?** Sans réponse écrite, chaque nouvelle
   surcouche réinventerait son propre bricolage — ou, pire, lirait directement
   la configuration persistée et resterait inerte au moment précis où elle est
   censée agir (c'est exactement ce qui a failli arriver : avec la NVS restée
   sur `"automatic"` et un pH NaN, la branche `scheduled` existante se serait
   fermée d'elle-même).
2. **Deux surcouches temporaires (Boost, Secours) peuvent-elles coexister, et si
   oui comment leurs effets se composent-ils ?** Rien dans le code ne dit si
   elles s'excluent ou se combinent — c'est une décision produit invisible à la
   lecture, qui a une conséquence concrète : en Chloration de secours, le Boost
   perd sa surchloration (il n'y a plus de cible ORP à relever) mais conserve sa
   prolongation de filtration, utile précisément quand il y a de la boue à
   faire circuler.
3. **Combien de temps un mode qui dose à l'aveugle peut-il courir avant de
   s'auto-annuler ?** Deux erreurs opposées sont possibles — un ré-armement
   quotidien oublié laisse l'eau sans chlore ; un mode oublié prolonge un
   dosage aveugle au-delà du raisonnable — et plusieurs durées crédibles
   existent (minuit local, comme le Boost ; 48 h ; 72 h).

## Décision

### 1. Mode EFFECTIF vs configuration publiée — règle opposable

> **Une lecture du mode qui décide d'injecter, de combien ou de quand lit le
> mode EFFECTIF. Une lecture qui répond à « quelle est la configuration ? »
> lit la NVS.**

Le mode EFFECTIF (`effectiveOrpModePure()`, [`src/temp_mode_logic.h`](../../src/temp_mode_logic.h))
compose la valeur NVS et l'état de la surcouche : si une surcouche de dosage
aveugle est active (et que le mode NVS n'est pas `manual`), le mode EFFECTIF
devient `Scheduled` **quelle que soit la valeur NVS**. Une surcouche future qui
voudrait gouverner une décision de dosage sans muter la configuration persistée
**doit** se brancher sur ce même accesseur, pas inventer sa propre lecture de
`mqttCfg.orpRegulationMode`. Tout chemin de **publication** d'état (HTTP
`/get-config`, WebSocket `config`, MQTT `{base}/orp_regulation_mode`) continue
de lire la NVS, inchangée — une automatisation Home Assistant qui lirait le
mode EFFECTIF croirait la configuration modifiée et pourrait la réécrire par
erreur.

### 2. Les surcouches temporaires ne sont PAS mutuellement exclusives par défaut

Le Mode Boost et la Chloration de secours **se combinent**. Un widget qui les
présenterait comme un choix unique (Aucun / Boost / Secours) serait **faux** et
priverait l'utilisateur de la prolongation de filtration du Boost au moment où
elle est la plus utile (de la boue à faire circuler). Chaque surcouche garde
son propre état (`boostState`, `RescueState`) et sa propre logique
d'expiration ; l'interface affiche l'**effet réellement obtenu**, dérivé des
deux états combinés, jamais une présomption d'exclusivité.

### 3. Durée d'expiration : 48 h glissantes, PAS un ancrage sur minuit local

**Minuit local est disqualifié techniquement** pour la Chloration de secours,
alors qu'il convient au Boost. La raison n'est pas une préférence mais une
collision structurelle : le compteur journalier de volume de chlore (
`safetyLimits.dailyOrpInjectedMl`) **remet aussi à zéro à minuit**. Une
expiration ancrée sur minuit ferait mourir le mode **exactement** au moment où
son propre budget se réarme — une activation à 10 h ne recevrait qu'une
fraction de son volume avant de mourir au reset suivant. L'échéance est donc
**epoch**, calculée `now + kRescueChlorineDurationS` au moment de
l'activation, glissante, sans lien avec le passage de minuit.

## Alternatives considérées

- **Option A (rejetée) — Lire `mqttCfg.orpRegulationMode` directement à chaque
  site de décision, sans accesseur dédié.** C'est ce qui a failli être livré :
  l'audit du code a montré **quatre sites supplémentaires** non recensés lors
  de la première passe (`mqttCfg.orpDailyTargetMl` lu en quatre endroits), et
  la feature aurait été morte à l'arrivée dans son scénario nominal (pH NaN +
  NVS restée sur `"automatic"`). Un accesseur central, testé nativement,
  élimine la classe d'erreur.
- **Option B (rejetée) — Exclusivité mutuelle entre Boost et Secours** (un
  radio-bouton plutôt que deux interrupteurs). Plus simple à présenter, mais
  faux : aurait privé l'utilisateur de la prolongation de filtration du Boost
  précisément quand la Chloration de secours est active (boue au fond du
  bassin).
- **Option C (rejetée) — Expiration à minuit local, comme le Boost.** Cohérente
  avec le patron existant et séduisante par symétrie, mais disqualifiée par la
  collision avec le rollover du compteur journalier (voir Décision §3).
- **Option D (rejetée) — 72 h.** Réduit la fréquence de ré-armement, mais
  prolonge un dosage aveugle sans réévaluation pendant trois jours pleins — le
  gate pool-chemistry juge le risque d'un mode oublié plus grave que celui
  d'un ré-armement manuel après 48 h.
- **Option E (retenue) — 48 h glissantes (epoch), surcouches combinables,
  mode EFFECTIF comme seule source de vérité pour toute décision de dosage.**

## Conséquences

### Positives
- Une seule règle (« décision ⇒ effectif, publication ⇒ NVS ») couvre tous les
  sites de lecture, présents et futurs, sans qu'il faille ré-auditer le code à
  chaque nouvelle surcouche.
- La composition Boost × Secours est **honnête** : l'interface décrit l'effet
  réellement obtenu, pas une présomption.
- Le budget de sécurité (volume de chlore) reste trivialement borné à un
  intervalle de 48 h fixe, sans interaction cachée avec le rollover journalier.

### Négatives / dette assumée
- Chaque nouvelle surcouche temporaire doit explicitement router ses sites de
  décision vers le mode EFFECTIF — ce n'est pas automatique, et un audit de code
  reste nécessaire pour la vérifier (comme cela l'a été pour celle-ci, en deux
  passes).
- Deux natures d'expiration coexistent désormais dans la même carte UI (minuit
  local pour le Boost, 48 h glissantes pour le Secours) — l'interface doit les
  afficher séparément, jamais fusionnées, pour ne pas laisser croire à une
  règle commune.

### Ce que ça verrouille
- Toute future surcouche temporaire de dosage qui ne mute pas la configuration
  persistée **doit** composer avec un accesseur « mode EFFECTIF », pas relire
  la NVS brute à chaque site.
- La non-exclusivité entre surcouches temporaires devient le défaut : une
  future surcouche qui voudrait s'exclure d'une autre devrait le justifier
  explicitement, pas le supposer.
- L'ancrage temporel d'une surcouche de dosage doit être vérifié contre les
  rollovers de compteurs de sécurité existants (journalier, horaire) avant
  d'être fixé — minuit n'est pas un choix neutre par défaut.

## Références

- Code : [`src/temp_mode_logic.h`](../../src/temp_mode_logic.h) / [`.cpp`](../../src/temp_mode_logic.cpp)
  (`effectiveOrpModePure`, `rescueActivePure`, `rescueDeadlinePure`,
  `effectiveOrpDailyTargetMlPure`), [`src/pump_controller.h`](../../src/pump_controller.h) /
  [`.cpp`](../../src/pump_controller.cpp) (`effectiveOrpMode()`,
  `getEffectiveOrpDailyTargetMl()`, `tickRescueChlorineAutoLift()`),
  [`src/config.h`](../../src/config.h) / [`.cpp`](../../src/config.cpp)
  (`RescueState`, `startRescueChlorine()`, `stopRescueChlorine()`),
  [`src/constants.h`](../../src/constants.h) (`kRescueChlorineDurationS`,
  `kRescueChlorineDailyMl`, `kRescueChlorineDailyHardCapMl`,
  `kRescueChlorineAutoLiftHoldMs`)
- Spec : `specs/features/done/feature-140-chloration-de-secours-et-widget-des-modes-temporaires.md`
- Docs : [MQTT.md](../MQTT.md), [API.md](../API.md),
  [subsystems/pump-controller.md](../subsystems/pump-controller.md),
  [subsystems/ws-manager.md](../subsystems/ws-manager.md),
  [features/page-dashboard.md](../features/page-dashboard.md)
- ADR liés : [ADR-0025](0025-mode-boost.md) (Mode Boost, le patron suivi),
  [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique
  pure testable, patron de `temp_mode_logic.h`), [ADR-0086](0086-trame-websocket-sous-ensemble-choisi-de-data.md)
  (budget d'octets de la trame WebSocket — même méthode de financement
  réutilisée ici, pas de second ADR pour ce volet)
