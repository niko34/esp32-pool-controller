# ADR-0085 — La découverte Home Assistant est republiée sur changement d'empreinte, pas sur reconnexion

- **Statut** : Accepté
- **Date** : 2026-09-20
- **Décideurs** : architect, embedded-developer, debugger (verdict de causalité du 08/09)
- **Spec(s) liée(s)** : `bug-121` (republication-decouverte-ha-a-chaque-reconnexion)

## Contexte

À chaque reconnexion MQTT, `mqtt_manager.cpp` remettait `discoveryPublished =
false` puis rejouait l'intégralité des entités d'auto-discovery Home Assistant
(60 mesurées le 2026-09-20, `grep -c "publishConfig(topic);" src/mqtt_manager.cpp`)
— alors que chaque entité est publiée avec `retain = true` et donc **déjà
conservée par le courtier**. La rafale est un travail redondant, payé au
moment le plus fragile : juste après une reconnexion.

Un relevé du 08/09 a montré 4 reconnexions en 65 s produisant 4 rafales
identiques. L'hypothèse initiale — que la rafale elle-même *causait* les
déconnexions en épuisant les tampons TX WiFi — a été **réfutée** par le
`debugger` le 08/09 : la flèche va déconnexion → rafale (le drapeau est remis
à chaque connexion réussie, donc une rafale ne peut pas causer la déconnexion
qui l'a précédée), 9 rafales sur 13 n'étaient suivies d'aucune déconnexion, et
les durées de rafale **décroissaient** (270 → 166 ms) avec **0 échec** —
anti-corrélé avec un épuisement de ressource. La vraie cause des
déconnexions reste hors du périmètre de cette décision (rattachée à
`bug-080`, fragmentation du tas 8 bits).

Ce qui reste vrai indépendamment de cette réfutation : republier 60 entités
déjà retenues à chaque reconnexion est un travail sans bénéfice dans
l'immense majorité des cas, et il fallait trancher **quand** la rafale reste
nécessaire sans tomber dans « republier au cas où ».

## Décision

La rafale de découverte est déclenchée par **changement d'une empreinte 32
bits** de la configuration (`computeDiscoveryFingerprint()`, FNV-1a sur
`FIRMWARE_VERSION` + préfixe `homeassistant/` + identifiant d'appareil +
topic de base + serveur + port du courtier), **jamais par la seule
reconnexion**. L'empreinte, accompagnée d'un hachage des topics publiés, du
nombre d'entités tentées et d'un indicateur de complétude, est persistée en
NVS (`DiscoveryRecord`) pour survivre au redémarrage.

Une rafale complète a lieu si et seulement si : aucun enregistrement NVS
valide (1ʳᵉ connexion après flash/factory reset) ; l'empreinte diffère de
celle enregistrée (nouvelle version de firmware, changement de préfixe, de
device ID, de topic, de serveur ou de port) ; la rafale précédente
enregistrée était incomplète (au moins un échec) ; ou Home Assistant vient de
publier son message de naissance (`homeassistant/status` == `online`, seul
chemin de récupération automatique en cas de perte des messages retenus côté
courtier). Dans tous les autres cas, **rien n'est republié** — le silence est
le cas nominal, et il est explicitement journalisé comme tel par son absence
de ligne.

Le jeu d'entités lui-même reste figé à la compilation (`publishConfig()` par
entité, en dur dans `mqtt_manager.cpp`) : il ne peut changer qu'avec une
nouvelle version de firmware, ce qui fait de `FIRMWARE_VERSION` un majorant
sain de l'empreinte, calculable **avant** toute publication.

## Alternatives considérées

- **Sentinelle en aller-retour** (publier un message-canari et vérifier son
  retour avant de déclencher la rafale) — **rejetée pour une raison
  technique, pas de confort** : `PubSubClient` n'expose pas le drapeau
  `retain` dans son callback de réception. Il est donc impossible de
  distinguer un message retenu déjà présent (preuve que le courtier a
  conservé l'état) d'un message que l'appareil vient tout juste de publier
  lui-même, sans différer l'annonce de découverte de 1 à 3 s pour laisser le
  temps à l'écho de revenir — latence jugée disproportionnée pour un cas qui
  reste rare.
- **Purge des topics orphelins au changement de préfixe** (publier un
  `retain` vide sur les anciens topics `.../config` dès qu'un nouveau
  préfixe est détecté) — **rejetée** : elle doublerait la rafale la plus
  chère (l'ancien jeu **et** le nouveau publiés dans la foulée), pour un
  bénéfice cosmétique (des entités orphelines dans Home Assistant, sans
  risque fonctionnel). Le geste de nettoyage reste possible manuellement
  (`mosquitto_sub --remove-retained`, documenté dans `docs/MQTT.md`).
- **Republier systématiquement à chaque reconnexion, l'idempotence suffit**
  (le **choix actuel avant ce lot**) — retenue jusqu'ici par simplicité :
  aucune empreinte à calculer, aucun état à persister, et l'auto-discovery
  HA est effectivement idempotente côté client (republier une config
  identique ne casse rien). Écartée parce que l'idempotence protège la
  *correction*, pas le *coût* : chaque rafale reste ~9 Ko émis et une
  fenêtre de publication au moment le plus fragile (juste après reconnexion,
  souvent sur un tas déjà dégradé), pour un bénéfice nul dans le cas
  nominal.

## Conséquences

### Positives
- Élimine le travail redondant de republication dans l'immense majorité des
  reconnexions (0 rafale sur des reconnexions répétées sans changement de
  configuration, contre 1 systématiquement avant ce lot).
- Le coût réel d'une rafale (octets offerts, `maxBloc8` avant/après) est
  désormais mesuré et journalisé à chaque occurrence — plus seulement sa
  durée — ce qui permettra de vérifier a posteriori qu'une future rafale ne
  contribue pas à une fragmentation observée, sans avoir à le supposer.
- La récupération automatique (message de naissance HA) ferme le cas le plus
  fréquent de perte de messages retenus (redémarrage du courtier HA) sans
  action humaine.

### Négatives / dette assumée
- Un courtier **purgé** (perte des messages retenus) **sans** que Home
  Assistant redémarre n'a **aucun** chemin de récupération automatique dans
  ce lot. Le bouton de récupération manuelle prévu à l'origine
  (`mqtt_force_discovery` sur `POST /save-config`) a été retiré du périmètre
  par décision utilisateur du 2026-09-20, pour livrer en firmware seul sans
  reconstruction du filesystem LittleFS. `DiscoveryReason::Recovery` et le
  paramètre `forced` existent et sont testés, sans appelant firmware — point
  d'extension tracé, pas dette silencieuse.
- Les topics orphelins d'un changement de préfixe restent retenus côté
  courtier indéfiniment (comportement inchangé par rapport à avant ce lot,
  mais maintenant documenté explicitement plutôt que découvert).

### Ce que ça verrouille
- **Toute entité nouvelle exige un bump de `FIRMWARE_VERSION` pour atteindre
  Home Assistant** — le mécanisme ne détecte un changement du jeu d'entités
  qu'à travers la version, jamais en comparant le contenu réel publié à
  l'ancien. Un futur changement d'entité qui omettrait le bump de version ne
  republierait jamais tant que la configuration (topic/serveur/port) ne
  change pas par ailleurs.
- Le format de `DiscoveryRecord` (uint32/uint16/uint8, sans padding cette
  géométrie précise) est le contrat de layout NVS : le faire évoluer sans
  migration explicite invaliderait silencieusement tous les enregistrements
  existants (comportement de repli sûr : `storedValid = false` → `FirstBoot`
  → rafale, donc dégradé mais pas dangereux).

## Références

- Code : `src/discovery_fingerprint_logic.h` (empreinte, `DiscoveryRecord`,
  `discoveryDecide()`), `src/mqtt_manager.cpp` (`computeDiscoveryFingerprint()`,
  `connectInTask()`, `messageCallback()` pour `homeassistant/status`),
  `src/mqtt_manager.h` (`_storedRecord`, `_discoveryForceRequested`),
  `src/constants.h` (`kDiscoveryNvsNamespace`, `kDiscoveryNvsRecordKey`,
  `kDiscoverySummaryLineMax`)
- Tests : `test/test_native_discovery_fingerprint/`,
  `test/test_native_discovery_report/`
- Spec : `specs/features/doing/bug-121-republication-decouverte-ha-a-chaque-reconnexion.md`
- Doc externe : [`docs/MQTT.md`](../MQTT.md#republication--conditionnée-à-lempreinte-pas-à-chaque-reconnexion-bug-121-v2531),
  [`docs/subsystems/mqtt-manager.md`](../subsystems/mqtt-manager.md#republication-conditionnée-à-lempreinte-pas-à-la-reconnexion-bug-121-v2531)
