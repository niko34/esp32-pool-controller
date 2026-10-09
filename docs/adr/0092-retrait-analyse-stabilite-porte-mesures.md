# ADR-0092 — Le verrou de la porte de mesure devient une durée, pas une analyse de stabilité

- **Statut** : Accepté
- **Date** : 2026-10-08
- **Décideurs** : gate `pool-chemistry` (instruction du 2026-10-08, puis remise en cause de conception demandée par l'utilisateur le même jour, GO sur le retrait), `architect`, `embedded-developer`, `test-engineer` (deux défauts trouvés par rejeu/mutation à l'extraction des modules purs), `code-reviewer` (2ᵉ passage — remesure du latch à la reprise, restriction de l'offre de chloration de secours)
- **Spec(s) liée(s)** : [feature-139](../../specs/features/done/feature-139-battement-de-la-porte-de-mesure-au-seuil.md) ; suite distincte : [feature-145](../../specs/features/todo/feature-145-butee-de-plausibilite-sur-l-orp-brut.md) (butée de plausibilité sur l'ORP brut)

## Contexte

Depuis la v2.34.0 ([ADR-0059](0059-purge-adaptative-fenetre-de-medianes.md)), la purge de reprise d'eau (sas avant de redéclarer les mesures pH/ORP représentatives du bassin) se terminait sur un **verdict de stabilité** : les bruts pH et ORP devaient cesser de bouger, jugé par l'étendue (max − min) d'une fenêtre glissante de 10 statistiques de bloc, avec un seuil d'ouverture (0,15 pH / 15 mV), une hystérésis de re-fermeture ×2 (0,30 pH / 30 mV, condition C1) et un plafond fail-safe de 90 min (condition C2).

Un relevé du 2026-10-08 a chiffré un **battement** de ce mécanisme : sur une demi-journée, ~75 min de blocage côté pH et ~20 min côté ORP, dont **65 % sans aucun arrêt de filtration à l'origine** — une re-fermeture pouvait s'armer sur la seule analyse de variation, sans qu'aucune eau n'ait stagné. L'instruction demandée au gate `pool-chemistry` devait d'abord trancher **quel réglage** ajuster (persistance avant re-fermeture, borne du nombre de re-purges, hystérésis dépendante du bruit).

**L'instruction a tourné court.** Le gate a établi un fait plus radical que la question posée : le mécanisme est un détecteur d'**étendue**. Il ne se ferme que pendant que la mesure **bouge**. Or une mesure **durablement fausse** — une sonde qui lit faux de façon stable, par exemple 7,90 au lieu de 7,40 — produit par définition une étendue **faible** : la porte s'**ouvre** devant elle. Le mécanisme est **structurellement aveugle au cas même qui motivait sa conservation**. Ce n'est pas une seconde couche de sécurité ; c'est un filtre de transitoire, dont le seul coût réel est du temps de régulation perdu.

Chiffrage du compromis réel (gate du 2026-10-08) :

| | |
|---|---|
| Dosage entièrement faux, 1 h au plafond horaire | **0,07 pH** dans 40 m³ (pire cas universel, 30 m³ / TAC 80 : ~0,2 pH/h) |
| 24 h au plafond journalier | **0,57 pH** — *ça*, c'est significatif |
| Coût du blocage mesuré | régulation **P pure** ⇒ **52 % de porte fermée DOUBLE l'écart résiduel**, même sans jamais atteindre un plafond |

On empêchait une erreur de 0,07 pH au prix d'une dérive de plusieurs dixièmes — un ordre de grandeur défavorable. Inventaire des autres filets : les butées chimiques pH (brute 6,20/8,10, corrigée 6,70/7,60, immédiates, insensibles au bruit) couvrent déjà **tout** ce que le détecteur de stabilité couvrait, et mieux. L'ORP n'a, lui, **aucune** butée de plausibilité sur la mesure brute — un manque préexistant, non créé par ce retrait, que le détecteur ne couvrait de toute façon pas (aveugle au stable).

Les seuils eux-mêmes n'étaient par ailleurs **jamais calés qu'une fois, sur un seul bassin** (trace du 20/09, marge annoncée 1,5× dans le commentaire du code) — non universels par construction, pour un appareil qui vise toutes les piscines.

## Décision

**Le verrou de la porte de mesure change de nature : d'une statistique locale (étendue de la mesure) vers une durée universelle (temps écoulé depuis l'arrêt d'eau).**

Sont **retirés** :
- le critère de stabilité (étendue des blocs ≤ 0,15 pH / 15 mV) comme condition d'ouverture ;
- la re-fermeture (C1) et son hystérésis ×2 ;
- le plafond de 90 min (C2) et son alerte MQTT `measures_purge_timeout` ;
- le module pur `purge_stability_logic` et sa suite de tests.

Sont **conservés** :
- un plancher **fixe** de 10 min (`kPurgeFloorMs`, inchangé — physique de renouvellement d'eau en conduite, universelle) ;
- une **grâce d'arrêt d'eau court** (`kWaterStopGraceS`, renommée depuis `kShortOutageS`, portée de 10 s à **180 s**) sous laquelle le plancher est sauté — justification **physique**, pas statistique : renouvellement au droit des sondes majoré à 71 s (60 m de DN50 à 6 m³/h), en 180 s d'eau immobile ΔpH ≈ 0 et ΔORP ≪ 1 mV, marge 3 à 5× avant le premier effet mesurable ;
- toutes les butées chimiques pH et les plafonds horaire/journalier de dosage, **inchangés** — ils portent désormais tout le risque résiduel qu'un détecteur de stabilité aurait pu couvrir (et qu'il ne couvrait en réalité pas).

**Un seul chemin de dispense** : que l'arrêt d'eau ait traversé un redémarrage ou non, c'est toujours la même comparaison « durée d'absence < `kWaterStopGraceS` » qui décide — la mémoire RTC de continuité (C3) ne fait que porter la datation de l'arrêt à travers un reboot, ce n'est pas une branche séparée. Le quota « une dispense par heure » (`kOutageWaiverMinIntervalS`) est retiré : il ne bloquait plus que le cas légitime d'un critère de durée explicite.

**La frontière de grâce est fail-closed, stricte** : à durée == `kWaterStopGraceS` pile, c'est déjà un arrêt long (pas de dispense). Les deux chemins (en marche, à travers un redémarrage) sont alignés **mot pour mot** sur cette frontière et testés pour leur **égalité**, pas seulement leurs valeurs.

**Nouveau contrat public** : `measures_purge_count` est retiré (le mécanisme de re-purge n'existe plus) ; `measures_purge_last_end` voit son domaine réduit à `{0, 1, 3}` (les valeurs `2` plafond et `4` re-purge sont **réservées, non réattribuées**) ; un nouveau champ `measures_dose_available_in_s` expose le délai avant que le dosage automatique redevienne possible (max du reste de plancher de purge et du reste de plancher de gel du filtre capteur — remplace un calcul côté client faux d'un facteur 10).

## Alternatives considérées

- **Persistance avant re-fermeture (N fenêtres consécutives)** (rejetée, réfutée par le gate) — les fenêtres se recouvrent à 90 % : un seul bloc aberrant maintient l'étendue haute pendant 10 fenêtres. Une persistance de 2 ou 3 n'aurait pas filtré le battement observé.
- **Supprimer le plancher après une re-fermeture** (rejetée, réfutée) — `purgeStabilityReset()` repart d'un anneau vide : supprimer le plancher ne gagnait aucune minute, les 19-25 min observées étaient le temps que l'étendue mette à redescendre, pas le plancher.
- **Hystérésis dépendante du bruit observé** (écartée, déjà écartée en feature-097 pour la même raison) — un seuil calculé pendant la purge serait gonflé par le transitoire lui-même, boucle auto-réalisatrice.
- **Garder le détecteur mais exiger les deux voies (pH ET ORP) pour refermer** (écartée) — aurait détruit le seul invariant écrit de la re-fermeture pH (c'était, de fait, le détecteur de dérive lente du pH, lié par `static_assert` à `kPurgeMinDetectedDriftPhPerMin`).
- **Ajouter une butée de plausibilité sur l'ORP brut comme prérequis au retrait** (écartée comme prérequis, sur décision explicite de l'utilisateur) — le manque est réel et préexistant, mais le coût du transitoire matinal qu'elle couvrirait (~0,5 ppm de chlore, borné et auto-corrigé) ne justifie pas de remplacer un mécanisme par un autre avant d'avoir fini de retirer le premier. Reste une suite distincte à instruire pour ses propres raisons.
- **Garder le mécanisme et seulement relâcher ses seuils** (écartée) — aurait laissé un détecteur d'étendue structurellement aveugle à son propre cas d'usage, sur des seuils toujours non universels.

## Conséquences

### Positives
- Le dosage automatique redevient disponible en quelques minutes après un arrêt d'eau ordinaire, au lieu de vingt, et ne s'interrompt plus en cours de journée sur une simple fluctuation de mesure.
- Les messages affichés annoncent désormais **quand** le dosage reprend (« Reprise automatique dans N min »), vocabulaire jugé plus simple par l'utilisateur que « stabilisation » / « purge » / « représentativité ».
- Deux défauts réels trouvés et corrigés **pendant** ce lot, par extraction en module pur et test de mutation — pas par relecture : (1) un calcul de délai restant qui ne courait pas dans tous les états (faux trois fois d'affilée, toujours la même cause) ; (2) une asymétrie d'une seconde entre les deux chemins de dispense, désormais épinglée par un test d'égalité des deux chemins.
- La chloration de secours reste trouvable sur les vraies pannes de capteur (codes 3, 4, 15, 19) et n'est plus proposée pour une attente ordinaire déjà bornée et annoncée.

### Négatives / dette assumée
- **Il n'existe plus aucun filet qui détecte une mesure pH/ORP durablement fausse mais stable**, en dehors des butées chimiques pH et des plafonds de dosage. Assumé : l'ancien mécanisme ne couvrait de toute façon pas ce cas.
- **La voie ORP reste sans aucune butée de plausibilité sur la mesure brute** — manque préexistant, non traité par ce lot (voir suite distincte proposée).
- Une coupure secteur de durée inconnue vaut toujours un arrêt long (fail-closed) : la mémoire RTC ne lui survit pas.
- `docs/adr/0059-*.md` et son amendement du 2026-09-20 (estimateur de bloc, moyenne interquartile) décrivent un mécanisme désormais supprimé — conservés pour mémoire (la découverte de l'anti-corrélation du signal pH brut, elle, reste valable indépendamment du code qui l'a motivée).

### Ce que ça verrouille
1. **Le verrou change de nature** — d'une statistique locale (étendue de blocs) vers une **durée universelle** (grâce d'arrêt d'eau). Tout futur critère de représentativité qui voudrait réintroduire une analyse de variation doit d'abord répondre à l'objection centrale de cet ADR : un détecteur d'étendue est aveugle à une mesure durablement fausse.
2. **Fail-closed sur la durée inconnue** — un arrêt d'eau non daté (mémoire RTC absente/corrompue, motif de reset hors liste blanche, heure invalide, coupure secteur) vaut toujours un arrêt long. Ceci verrouille tout futur chemin qui voudrait « deviner » une durée au lieu de la mesurer ou de refuser la dispense.
3. **Doctrine de contrat public** : un champ devenu structurellement **constant** (ici `measures_purge_count`, qui serait resté figé à 0) est **retiré**, pas conservé figé — le conserver publierait une forme stable qui ment. À l'inverse, un champ dont le **domaine se rétrécit** (ici `measures_purge_last_end`) est conservé **sans renumérotation** : les valeurs devenues inatteignables restent réservées, pour qu'un consommateur externe cesse de matcher plutôt que de faire un faux-positif sur une autre cause.
4. **Non-retour** : le réglage utilisateur de l'attente (`stabilization_delay_min`, retiré par l'ADR-0059) ne revient pas — c'est la moitié de cette décision d'origine qui survit intacte à ce retrait.

## Références

- Code : [`src/measure_validity_logic.h`](../../src/measure_validity_logic.h) / [`.cpp`](../../src/measure_validity_logic.cpp) (machine d'état, C2/C3), [`src/dose_available_logic.h`](../../src/dose_available_logic.h) (délai avant dosage, nouveau module pur), [`src/measure_validity.cpp`](../../src/measure_validity.cpp) (coquille, mémoire RTC, journal `[STAB]`), [`src/constants.h`](../../src/constants.h) (bloc `kPurgeFloorMs` / `kWaterStopGraceS` et `static_assert`), [`src/web_routes_data.cpp`](../../src/web_routes_data.cpp), [`src/ws_manager.cpp`](../../src/ws_manager.cpp) (contrat `/data` + WS), [`data/app.js`](../../data/app.js) (`_measuresWaitText()`, `measureStateSegments()`, offre de chloration de secours restreinte)
- Tests : [`test/test_native_measure_validity/`](../../test/test_native_measure_validity/), [`test/test_native_dose_available/`](../../test/test_native_dose_available/)
- Spec : [feature-139](../../specs/features/done/feature-139-battement-de-la-porte-de-mesure-au-seuil.md)
- Doc : [subsystems/measure-validity.md](../subsystems/measure-validity.md), [subsystems/sensors.md](../subsystems/sensors.md), [API.md](../API.md), [MQTT.md](../MQTT.md), [features/page-ph.md](../features/page-ph.md), [features/page-orp.md](../features/page-orp.md), [features/page-dashboard.md](../features/page-dashboard.md), [features/page-settings.md](../features/page-settings.md)
- ADR liés : **Supersède** [ADR-0059](0059-purge-adaptative-fenetre-de-medianes.md) (la purge se termine sur la stabilité jugée par une fenêtre de médianes — thèse inversée ici ; ce qui survit : le retrait du réglage utilisateur `stabilization_delay_min`) ; [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md) (modules purs) ; [ADR-0041](0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md) (porte d'alimentation des filtres, inchangée par ce lot) ; [ADR-0048](0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md) (butées chimiques pH, désormais seul filet contre une mesure durablement fausse)
