# ADR-0074 — Une sonde de température figée ne suspend rien : statu quo instrumenté

- **Statut** : Accepté
- **Date** : 2026-09-07
- **Décideurs** : pool-chemistry (verdict et chiffrage), architect, embedded-developer, test-engineer, code-reviewer, doc-writer
- **Spec(s) liée(s)** : [feature-068](../../specs/features/done/feature-068-sonde-temperature-eau-figee.md), [bug-114](../../specs/features/done/bug-114-compensation-temperature-sans-borne-de-plausibilite.md)

## Contexte

Le journal de l'incident du 2026-08-16 porte cette ligne :

```
[SENSOR_FROZEN] Sonde T° eau figée : 900 lectures dans une bande < 0.03 °C autour de
26.6 °C — compensation pH et planning filtration sur valeur figée
```

Le message **annonce un enjeu** : la compensation de température du pH et le planning automatique de filtration travaillent sur une valeur morte. `feature-068` a donc été écrite pour trancher entre trois traitements — statu quo instrumenté (A), repli explicite sur 25 °C (B), ou **fail-closed** : suspendre la régulation pH tant que la sonde est figée (C).

**La prémisse de la spec s'est révélée quantitativement fausse**, et c'est le chiffrage demandé en préalable de la décision (livrable A1) qui l'a établi. Sans cet ADR, le message de log continuera d'annoncer un enjeu que la physique ne soutient pas, et l'option C sera rouverte par le prochain lecteur.

### Le chiffrage, qui est le corps de cette décision

**Méthode** (ADR-0073 : le chiffre porte sa méthode, et celle-ci est refaite à chaque reprise, jamais recopiée). Pente de Nernst :

```
S(T) = 2,303 · R · T / F = 59,16 mV/pH à 25 °C (298,15 K)
```

Le module Atlas EZO pH calcule `pH = 7 + (E_iso − E) / S(T_comp)`, où `T_comp` est la température qu'on lui envoie par `RT,<tempC>`. À **potentiel d'électrode donné**, remplacer `T_vrai` par `T_comp` déplace donc la lecture de :

> **`pH_lu − pH_vrai = (pH_vrai − 7) × (T_vrai − T_comp) / T_comp`**  (T en **kelvins**)

Soit, autour de 25 °C et pour un bassin à pH 7,2 : **≈ 0,7 millième de pH par °C d'erreur de compensation**.

**Le cas relevé en production** — 26,6 °C figé contre 25,0 °C réel, pH **brut** ≈ 6,80 (l'offset de mesure est de +0,50, cf. [ADR-0039](0039-offset-ph-applique-en-sortie-des-accesseurs.md)) :

```
(6,80 − 7) × (298,15 − 299,75) / 299,75 = +0,00107 pH
```

**+0,0011 pH.** À comparer aux seuils **réels** de cet appareil :

| Seuil | Valeur | Source | Erreur de compensation nécessaire pour l'atteindre |
|---|---|---|---|
| Bande morte la plus **serrée** | **0,01 pH** | `kPhStopThreshold` (arrêt de session / cible du PI) | **≈ 15 °C** |
| Bande morte la plus **large** | **0,05 pH** | `kPhStartThresholdDefault` (ouverture de session) | **≈ 75 °C** |
| Consigne pH de l'appareil | 7,30 | configuration vivante | — |
| Plancher de sécurité acide | 6,70 corrigé / **6,20 brut** | [ADR-0048](0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md) | — |
| Inhibition ORP sur pH brut | 7,60 | feature-067 | — |

Les deux valeurs d'erreur se refont par la formule ci-dessus : `ΔT = ΔpH × T_comp / (pH_vrai − 7)`, avec `pH_vrai = 6,80` et `T_comp = 299,75 K` ⇒ `0,01 × 299,75 / 0,20 = 15,0 °C` et `0,05 × 299,75 / 0,20 = 74,9 °C`.

**Deux à trois ordres de grandeur** sous le premier seuil qui produirait un effet observable. L'erreur est **chimiquement non signifiante**.

**Sens de l'erreur**, qui compte autant que son amplitude : `T_comp` **trop haute** ⇒ pente surestimée ⇒ lecture **tirée vers 7**. Le brut étant **sous** 7, le pH est lu **trop basique** ⇒ **léger sur-dosage** d'acide (ici +0,001 pH, soit ~0 mL). Symétrie inversée si le brut passait au-dessus de 7 : la lecture serait alors trop acide et le dosage sous-évalué.

### Le bornage déjà en place

Les lectures DS18B20 sont **déjà** filtrées à `(−55 ; 125)` °C, avec rejet de `85.0` (valeur de power-on reset du scratchpad) et de `DEVICE_DISCONNECTED_C` — [`src/sensors.cpp:324-326`](../../src/sensors.cpp). Même **aux bornes** de cette fenêtre, l'erreur de pH induite plafonne à **0,05-0,07 pH**. Une sonde figée sur une valeur plausible est donc, par construction, un cas **moins** sévère que ceux déjà couverts.

## Décision

**Une sonde de température d'eau figée est rendue VISIBLE, et rien d'autre n'est fait.**

| Consommateur de la température | Traitement retenu |
|---|---|
| Compensation `RT` de l'EZO pH | **inchangée** — la valeur figée continue de partir sur le bus |
| Planning automatique de filtration | **inchangé** — et il n'y avait rien à coder (voir plus bas) |
| Régulation pH / dosage | **inchangés** — aucun chemin de `pump_controller` ne lit le drapeau |
| Observabilité | **ajoutée** : champ `/data` + WebSocket `tempFrozenAlarm`, topic MQTT retenu `<base>/temp_sensor_frozen` + `binary_sensor` HA `device_class: problem`, badge UI sur le tableau de bord et la page Température |

C'est l'**option A instrumentée**. On instrumente, on ne corrige pas : il n'y a rien à corriger.

**Sortie de l'état figé : immédiate, sans hystérésis.** Le drapeau est purement informatif ; un battement ne coûte rien chimiquement, et une hystérésis ajouterait un second seuil à régler. Le seul anti-battement du lot est un **maintien de 60 s à la publication MQTT** (`kMqttTempFrozenHoldMs`, [`src/mqtt_manager.h`](../../src/mqtt_manager.h)) — une entité Home Assistant qui clignote est inutilisable dans une automatisation. Ce maintien vit **sur ce seul chemin de publication** : `/data`, la trame WS et l'UI voient la valeur **instantanée**.

### Le planning de filtration était DÉJÀ immunisé

C'est le point qui a évité d'écrire du code, et il doit être écrit pour que personne ne le réécrive :

| Garde | Emplacement | Effet |
|---|---|---|
| `isnan(referenceTemp)` ⇒ `return` | [`src/filtration.cpp:86`](../../src/filtration.cpp) | pas de température valide ⇒ **planning conservé** |
| Bande morte de **1 °C** sur `_lastScheduledTemp` | [`src/filtration.cpp:89`](../../src/filtration.cpp) | une température **figée** ne recalcule **rien** : l'écart est nul, donc sous la bande morte |
| Durée clampée à **[1 h ; 24 h]** | [`src/schedule_logic.cpp:64-67`](../../src/schedule_logic.cpp) | même une température absurde produit un créneau borné |

**Pour le planning, « statu quo » et « fail-closed » sont identiques EN FAIT.** L'option C n'aurait ajouté aucun comportement sur cet axe — seulement du code et un second seuil.

### ⛔ L'interdit à graver : « suspendre la compensation » n'existe pas

Ne pas envoyer `RT` ([`src/atlas_ezo.cpp:149-155`](../../src/atlas_ezo.cpp)) **ne suspend rien**. La lecture retombe sur `R`, et le module EZO **conserve sa dernière température interne** — silencieusement périmée, **invisible et indatable** côté firmware. C'est **pire que n'importe quel repli** : le repli, lui, est journalisé, borné et exposé par un accesseur.

Il y a donc **toujours** une valeur envoyée dans `RT`, et l'étage de la cascade qui l'a produite est **toujours** exposé. La cascade de [`temp_compensation_logic.h`](../../src/temp_compensation_logic.h) ne rend jamais `NaN` — un test le vérifie explicitement.

## Alternatives considérées

- **Option C — fail-closed : suspendre la régulation pH automatique tant que la sonde est figée** (**REJETÉE**, et le motif est le cœur de cet ADR). Le coût du **faux positif** est réel là où le bénéfice est de l'ordre du millième de pH : pH non corrigé ⇒ le pH **monte** ⇒ la fraction de HOCl **chute** (pKa 7,54 : **52 %** à pH 7,50, **41 %** à pH 7,70) ⇒ **sous-chloration réelle en quelques heures**. Et la boucle ORP **ne peut pas compenser** : elle **sature déjà son quota horaire de 10 min/h** sur cet appareil ([ADR-0056](0056-sous-dimensionnement-actionneur-3-jours-80-pct.md)), et l'inhibition ORP sur pH brut à **7,60** la **couperait** de toute façon. On échangerait **0,001 pH** d'erreur de mesure contre une **sous-chloration effective**. **La prudence apparente était du mauvais côté.**
- **Option B — repli explicite sur `kEzoFallbackTempC` = 25 °C dès que le figé est détecté** (rejetée) — remplacerait une valeur **peut-être juste** (26,6 °C, plausible pour l'eau du bassin) par une valeur **connue comme approximative**, pour un gain nul : les deux sont à ~0,001 pH l'une de l'autre. Le repli 25 °C garde tout son sens quand la mesure est **absente ou implausible** (c'est l'étage 3 de la cascade de `bug-114`) ; il n'en a aucun quand elle est **présente et dans la fenêtre**.
- **Verser la température figée dans l'alerte `alerts/sensor_frozen`** (rejetée — voir [`docs/MQTT.md`](../MQTT.md#topics-et-entités-ajoutés-en-feature-022-problème-capteur-phorp-v2100)) — la sémantique **établie** de ce canal est « une mesure de régulation est morte, **donc le dosage est compromis** » : un pH/ORP figé rend le filtre non prêt et **bloque** l'injection. Y ajouter un événement **sans effet sur le dosage** déclencherait des automatisations Home Assistant **existantes** : contrat externe cassé sans bump de version. La température garde donc son topic dédié.
- **Durcir la détection (allonger N) pour éliminer le risque de faux positif** (sans objet ici) — la détection reste à `kTempFrozenSamples = 900` (≈ 30 min à 2 s) et `kTempFrozenEpsilonC = 0,03 °C` (< ½ LSB du DS18B20 12 bits). Comme le drapeau ne déclenche **rien**, le coût d'un faux positif est un badge de trop, pas une décision fausse : il n'y avait pas de seuil à durcir.

## Conséquences

### Positives

- **L'état est observable de bout en bout** sans qu'aucune décision n'en dépende : `/data`, WebSocket, MQTT retenu, badge UI. Un épisode de 30 min qui ne laissait qu'une ligne de log laisse désormais une trace **relisable à froid**.
- **Le diagnostic distingue enfin les trois causes d'un écart de compensation.** Le topic retenu `<base>/diagnostic` porte `ezo_comp_t` (température **effectivement** envoyée dans `RT`, donc après offset borné **et** cascade) et `ezo_comp_stage` (`mesure` / `derniere_valeur_plausible` / `repli_defaut`). Comparé à `temperature`, cela sépare une valeur **figée** (constante, étage `mesure`), un **repli** (étage ≠ `mesure`) et un **offset** mal réglé (écart constant).
- **Le vrai défaut a été trouvé en cherchant celui-là.** `bug-114` visait la sonde ; le chiffrage a montré que la sonde était bornée et que le trou était ailleurs — l'**offset de calibration**, écrit **sans borne** : `+1000` ⇒ `RT,1025.0` ⇒ **≈ +0,15 pH**, et un offset très négatif fait tendre `T_comp` vers **0 K** ⇒ erreur **non bornée**. C'est une entrée de **configuration**, donc **durable en NVS**. Bornée depuis (voir « Ce que ça verrouille »).
- **Aucun seuil de régulation nouveau n'a été introduit.** L'option C en aurait demandé au moins deux (délai avant suspension, hystérésis de reprise), chacun à régler et à défendre.

### Négatives / dette assumée

- **Le message de log de `feature-022` reste alarmant** (« compensation pH et planning filtration sur valeur figée ») alors que la conséquence est de l'ordre du millième de pH. Il n'a pas été réécrit dans ce lot : il est **factuellement exact** et le contexte est désormais ici. Un futur lecteur qui n'ouvre pas cet ADR rouvrira la question — c'est précisément ce que cet ADR est censé arrêter.
- **La borne haute de la fenêtre `RT`, 40 °C, est une borne de PRUDENCE, NON CONFIRMÉE par la datasheet Atlas.** Recherche faite dans le dépôt le 2026-09-07 : **aucune trace** de la plage réellement admise par la commande `RT` ([`docs/subsystems/sensors.md`](../subsystems/sensors.md) ne documente que les **délais** de transaction). Elle est choisie pour laisser ~6 °C de marge au-dessus de toute eau de bassin plausible en canicule. ⚠️ **Ne jamais la présenter comme une valeur constructeur** : une borne attribuée à un document que personne n'a lu est exactement le mécanisme qui a produit une série de valeurs fausses dans ce dépôt **en une seule journée** — [ADR-0073](0073-chiffre-consigne-porte-sa-methode.md) en consigne **onze** au moment de sa rédaction, **dix-huit** à la clôture du 2026-09-07. Si la datasheet est un jour consultée, reprendre `constants.h`, `test_native_temp_compensation` et cette section — en repassant par `pool-chemistry`.
- **Le badge « figée » peut disparaître à tort** dans une installation où seule la sonde **circuit** n'est pas identifiée : la condition UI s'appuie sur `sondes_identified`, qui couvre les **deux** rôles. Défaut **conservateur** — on perd un badge informatif, on n'en invente pas un faux — et le sens inverse est impossible (rôle « eau » non assigné ⇒ `sondes_identified` faux). Le discriminant naturel, `temperature_raw`, est **WS-seul** : s'en servir aurait rejoué le piège de parité `/data` ↔ WS ([ADR-0071](0071-parite-data-trame-capteurs.md)) et fait **clignoter** le badge toutes les ~30 s.
- **La marge de la trame WebSocket tombe à 43 o.** `tempFrozenAlarm` coûte **24 o** au pire cas structurel (`"tempFrozenAlarm":false,` = 1+15+1+1+5+1) ; trame `3 517 + 24 = 3 541 o` sous `kWsFrameBufBytes = 3584`. **Acceptable pour ce lot, pas pour le suivant** : le prochain champ de cette trame devra consommer la réserve documentée de **49 o** (`ph_integral` + `ph_integral_term`, sans consommateur client depuis feature-092) **ou** relever `kWsFrameBufBytes` — décision d'**ADR**, pas de routine.

### Ce que ça verrouille

- **L'absence d'effet du drapeau « température figée » sur le dosage et sur la filtration.** Deux sentinelles structurelles le tiennent : `pump_controller.cpp` ne référence **aucun** des nouveaux symboles, et `DoseInputs` ne porte **aucun** champ de température ([`test/test_native_temp_compensation/`](../../test/test_native_temp_compensation/test_temp_compensation.cpp)). Y toucher fait rougir la suite **avant** de changer un comportement chimique, et exige un passage par `pool-chemistry`.
- **L'interdit `RT`** : il y a toujours une valeur envoyée. « Suspendre la compensation » n'est pas une option disponible, ni ici, ni ailleurs.
- **Les bornes de la compensation**, désormais **source unique** dans [`src/constants.h`](../../src/constants.h) : offset utilisateur `[−5 ; +5]` °C (`kTempCalOffsetMin/Max` — un DS18B20 dérive de **dixièmes** de degré), fenêtre `RT` `[0 ; 40]` °C (`kEzoCompTempMinC/MaxC`), repli `kEzoFallbackTempC = 25` °C (coût maximal **±0,01 pH**). ⚠️ **Ne pas resserrer la fenêtre sur 15-30 °C** : cela rejetterait des températures **vraies** (hors saison, canicule) et **dégraderait** la compensation au lieu de la protéger.
- **Les QUATRE points de clamp de l'offset**, dont aucun n'est un doublon : ils couvrent quatre **provenances** distinctes de la valeur — écriture ([`web_routes_config.cpp`](../../src/web_routes_config.cpp), `handleSaveConfig`), chargement NVS ([`config.cpp`](../../src/config.cpp), `loadMqttConfig` — sans lui, `/get-config` exposerait une valeur que le firmware n'applique **pas**, et la règle du dépôt « toujours lire `/get-config` avant tout diagnostic chiffré » deviendrait un **piège** ; un diagnostic chiffré a déjà déraillé sur ce motif le 2026-09-06), émission de la valeur de **mesure** ([`sensors.cpp`](../../src/sensors.cpp), `_readDs18b20s` — affichage, MQTT, historique) et **émission `RT`** ([`sensors.cpp`](../../src/sensors.cpp), `getWaterTemperature` — le **seul** site qui alimente la commande envoyée à la sonde). **Ne pas fusionner les deux derniers.**
- **Le contrat externe d'`alerts/sensor_frozen`** : « le dosage est compromis ». Aucun événement sans effet sur le dosage n'y entre.

> **Note additive 2026-09-13 (bug-126, [ADR-0079](0079-planning-filtration-fige-a-l-arret-promu-a-minuit.md)) — le mécanisme d'immunisation du planning change, la conclusion ne bouge pas**
>
> Le tableau « Le planning de filtration était DÉJÀ immunisé » ci-dessus citait une **bande morte de 1 °C sur `_lastScheduledTemp`** comme l'une des deux propriétés fondant le verdict « une sonde figée ne change rien au planning ». **Ce champ n'existe plus** : `bug-126` a retiré `_lastScheduledTemp` en même temps qu'il déplaçait le calcul du planning de « pendant la marche » à « à l'arrêt, appliqué à minuit » ([ADR-0079](0079-planning-filtration-fige-a-l-arret-promu-a-minuit.md)).
>
> Il est remplacé par **deux gardes strictement plus fortes**, qui ne dépendent plus d'un champ RAM perdu à chaque redémarrage :
>
> 1. `isSampleEligible()` ([`src/schedule_logic.h`](../../src/schedule_logic.h)) rejette tout échantillon tant que `sensors.isTemperatureFrozen()` est vrai — une sonde figée ne nourrit même plus la moyenne du cycle ;
> 2. une fenêtre calculée identique à la fenêtre courante ne produit **aucun** planning en attente (`writePendingSchedule()`, [`src/filtration.cpp`](../../src/filtration.cpp)) — remplaçant fonctionnel de l'ancienne bande morte, qui ne peut plus se réinitialiser au reboot.
>
> **La conclusion de cet ADR est inchangée** : une sonde de température figée reste rendue visible et rien d'autre n'est fait ; le planning de filtration reste, en fait, immunisé contre une sonde figée — par un mécanisme différent et plus robuste. La ligne `isnan(referenceTemp) ⇒ return` du tableau ci-dessus reste, elle, exacte (portée désormais par `resolveReferenceTemp()` / `selectFiltrationRefTemp()`, verdict `NoSample`).

## Références

- Code : [`src/temp_compensation_logic.h`](../../src/temp_compensation_logic.h) (logique pure : clamp, fenêtre, cascade — [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)), [`src/sensors.cpp`](../../src/sensors.cpp) (`_resolveEzoCompensationTemp`, clamps 3/4 et 4/4), [`src/constants.h`](../../src/constants.h) (bornes et leurs motifs), [`src/filtration.cpp`](../../src/filtration.cpp) et [`src/schedule_logic.cpp`](../../src/schedule_logic.cpp) (le planning déjà immunisé), [`src/atlas_ezo.cpp`](../../src/atlas_ezo.cpp) (`RT,<t>` — l'interdit), [`src/mqtt_manager.h`](../../src/mqtt_manager.h) (`kMqttTempFrozenHoldMs`)
- Tests : [`test/test_native_temp_compensation/test_temp_compensation.cpp`](../../test/test_native_temp_compensation/test_temp_compensation.cpp) — chiffrage de Nernst **refait** dans le test, cascade, sentinelles structurelles des quatre points de clamp (visés **dans leur fonction**, sur code **dépouillé de ses commentaires**) et de la non-régression dosage/planning
- Doc : [`docs/subsystems/sensors.md`](../subsystems/sensors.md#compensation-t-du-ph), [`docs/API.md`](../API.md), [`docs/MQTT.md`](../MQTT.md), [`docs/features/page-temperature.md`](../features/page-temperature.md), [`docs/features/page-dashboard.md`](../features/page-dashboard.md)
- Specs : [`feature-068`](../../specs/features/done/feature-068-sonde-temperature-eau-figee.md), [`bug-114`](../../specs/features/done/bug-114-compensation-temperature-sans-borne-de-plausibilite.md)
- ADR liés : [ADR-0073](0073-chiffre-consigne-porte-sa-methode.md) (tout chiffre porte sa méthode — appliqué ici au chiffrage de Nernst), [ADR-0039](0039-offset-ph-applique-en-sortie-des-accesseurs.md) (offset pH en sortie — d'où le brut à 6,80 pour un affiché à 7,30), [ADR-0056](0056-sous-dimensionnement-actionneur-3-jours-80-pct.md) (quota ORP déjà saturé — pourquoi la boucle chlore ne peut pas compenser), [ADR-0071](0071-parite-data-trame-capteurs.md) (parité `/data` ↔ WS — pourquoi le badge n'utilise pas `temperature_raw`), [ADR-0041](0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md) (mesures non représentatives — le gate `measures_valid` du badge), [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (logique pure testable en natif)
