# Architecture Decision Records — ESP32 Pool Controller

Ce dossier contient les décisions d'architecture structurantes du projet, au format Michael Nygard (*Architecture Decision Records*, 2011).

## Pourquoi des ADR ?

Certaines décisions techniques ne sont pas lisibles dans le code : elles expliquent **pourquoi** telle option a été retenue plutôt que telle autre. Un ADR capture le contexte, l'alternative rejetée et la conséquence assumée, pour ne pas avoir à rediscuter la question six mois plus tard.

## Règles du dossier

- **Immuable une fois accepté** : un ADR n'est pas modifié. S'il est remis en cause, on en écrit un nouveau qui le supersède (et on change le statut de l'ancien en `Superseded by ADR-XXXX`).
- **Un ADR = une décision**. Pas de document fourre-tout.
- **Format court** : contexte, décision, conséquences. Pas de tutoriel.
- **Numérotation monotone** : `ADR-0001`, `ADR-0002`, …

## Index

| # | Titre | Statut |
|---|-------|--------|
| [0001](0001-capteurs-analogiques-ads1115.md) | Capteurs pH/ORP analogiques via ADS1115 + DFRobot_PH | Superseded by ADR-0014 |
| [0002](0002-mode-programmee-volume-quotidien.md) | Mode Programmée exprimé en volume quotidien (mL), pas en cadence | Accepté |
| [0003](0003-calibration-orp-cote-client.md) | Calibration ORP calculée côté client, pas par le firmware | Superseded by ADR-0014 |
| [0004](0004-mode-regulation-enum-3-valeurs.md) | Sélecteur de mode à 3 valeurs au lieu de booléens `ph_enabled` / `orp_enabled` | Accepté |
| [0005](0005-websocket-push-sans-polling.md) | WebSocket push pour le temps réel, pas de polling périodique | Accepté |
| [0006](0006-frontend-vanilla-js.md) | Frontend en vanilla JS sans framework ni bundler | Accepté |
| [0007](0007-table-partitions-custom.md) | Table de partitions custom (SPIFFS 1088 KB, history séparée) | Superseded by ADR-0009 puis ADR-0015 |
| [0008](0008-persistance-cumuls-journaliers-nvs.md) | Persistance NVS des cumuls journaliers + reset aligné minuit local | Accepté |
| [0009](0009-partition-coredump.md) | Partition coredump dédiée + redimensionnement history (128 KB → 64 KB) | Accepté |
| [0010](0010-stabilite-mqtt-reseau.md) | Stabilité MQTT et réseau : WiFi sans power save, pré-résolution DNS, backoff non réinitialisé | Accepté |
| [0011](0011-mqtt-task-dediee.md) | MQTT déplacé dans une tâche FreeRTOS dédiée | Accepté |
| [0012](0012-mapping-gpio-pcb-v2.md) | Mapping GPIO PCB v2 (Atlas EZO + 2ᵉ DS18B20 + CTN_AUX, plus d'ADS1115) | Accepté |
| [0013](0013-identification-sondes-onewire.md) | Identification des sondes DS18B20 par adresse ROM persistée NVS + workflow utilisateur | Accepté |
| [0014](0014-migration-atlas-ezo.md) | Migration Atlas EZO pH/ORP (PCB v2) — supersedes ADR-0001 | Accepté |
| [0015](0015-partition-app-1.5mb.md) | Partition application 1.5 MB (layout v2) | Superseded by ADR-0019 |
| [0016](0016-regulation-p-temporisee-vs-pid.md) | Régulation P temporisée par défaut (Kp seul, Ki=0, Kd=0) sur mesure filtrée | Accepté (`Ki = 0` levé sur le pH par ADR-0052 ; `Kd = 0` et `Ki_ORP = 0` inchangés) |
| [0017](0017-logique-metier-pure-humble-object-testabilite.md) | Logique métier pure (Humble Object) séparée de la couche hardware pour testabilité native | Accepté |
| [0018](0018-migration-uplot.md) | uPlot au lieu de Chart.js pour les graphiques de l'UI (contrainte Flash/FS) | Accepté |
| [0019](0019-partition-app-1664k.md) | Partitions app à 1664 KB (layout v3, spiffs 576 KB) | Superseded by ADR-0024 |
| [0020](0020-budget-horaire-dosage-unique.md) | Budget horaire de dosage unique, partagé auto + manuel | Accepté |
| [0021](0021-repartition-scheduled.md) | Répartition du volume quotidien scheduled par fenêtres de 15 min alignées horloge | Accepté |
| [0022](0022-verification-integrite-ota.md) | Vérification d'intégrité OTA par SHA-256 comparé au digest de l'API GitHub | Accepté |
| [0023](0023-politique-cors-retrait.md) | Retrait complet du mécanisme CORS : politique même-origine stricte | Accepté |
| [0024](0024-partitions-layout-v4.md) | Partitions app à 1792 KB (layout v4, spiffs 320 KB) | Accepté |
| [0025](0025-mode-boost.md) | Mode Boost : surcouche temporaire « valeurs effectives » + relèvement borné de la limite chlore | Accepté |
| [0026](0026-mode-installation.md) | Mode d'installation : 3 archétypes de câblage et résolution unique de la présence d'eau | Accepté |
| [0027](0027-filtre-tendance-centrale.md) | Filtre pH/ORP recalé sur la tendance centrale (estimateur robuste + découplage valeur/latch) | Accepté |
| [0028](0028-migration-fork-esp32async.md) | Migration vers le fork maintenu ESP32Async (AsyncTCP / ESPAsyncWebServer) avec versions épinglées | Accepté |
| [0029](0029-auto-guerison-heap.md) | Auto-guérison sur fragmentation du tas (redémarrage propre) + mode maintenance anti-boucle | Accepté |
| [0030](0030-watchdog-connectivite.md) | Watchdog de connectivité (sonde ICMP passerelle + réponse graduée) | Accepté |
| [0031](0031-disjoncteur-bootloop-history.md) | Disjoncteur de boot-loop sur l'init de la partition history (détection inter-boot NVS + effacement au seuil) | Accepté |
| [0032](0032-reponses-http-bornees.md) | Réponses HTTP bornées : pagination, chunked ou allocation unique testée — gardes à seuil fixe interdites | Accepté |
| [0033](0033-budget-espace-partition-history.md) | Budget d'espace garanti sur la partition history (garde d'admission + remove-avant-write + plafonds log réduits) | Accepté |
| [0034](0034-migration-core3-idf5.md) | Migration core Arduino 3.x / ESP-IDF 5.x via pioarduino (code bi-core exécuté ; porte de bascule **franchie** par ADR-0042) | Accepté |
| [0035](0035-parachute-oom-new-handler.md) | Parachute OOM global : `std::set_new_handler` + réserve d'urgence libérable + callback IDF flag-seul | Accepté |
| [0036](0036-psram-requise-mise-a-jour-en-ligne.md) | PSRAM libre requise pour toute session TLS sortante (mise à jour en ligne GitHub) | Accepté |
| [0037](0037-postmortem-oom-et-integrite-tas.md) | Post-mortem d'allocation en RAM RTC + contrôle d'intégrité périodique du tas | Accepté |
| [0038](0038-metrique-memoire-allouable-8-bits.md) | La mémoire se mesure sur le pool 8 bits réellement allouable, jamais sur `MALLOC_CAP_INTERNAL` | Accepté |
| [0039](0039-offset-ph-applique-en-sortie-des-accesseurs.md) | Offset logiciel de mesure pH appliqué en sortie des accesseurs (filtre sur le brut, règle `*Raw`) | Accepté |
| [0040](0040-pas-de-calibrage-sur-fragmentation-pathologique.md) | On ne calibre pas des seuils sur une fragmentation pathologique (régime bi-métrique assumé) | Accepté |
| [0041](0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md) | Autorité des mesures non représentatives et sortie de secours du filtre de tendance centrale | Accepté |
| [0042](0042-core3-environnement-par-defaut.md) | Le core Arduino 3.x / IDF 5 devient l'environnement par défaut (bootloader ET application) | Accepté |
| [0043](0043-dosage-proportionnel-de-temps-amplitude-calibree.md) | Dosage en proportionnel de temps à amplitude calibrée (fail-closed, sans interpolation) | Accepté (seuil C amendé par ADR-0046, défauts de calibration amendés par ADR-0047, pouls minimal amendé par ADR-0051) |
| [0044](0044-budget-horaire-et-temps-min-en-temps-pompe-on.md) | Budget horaire et temps minimum d'injection comptés en temps POMPE-ON | Accepté |
| [0045](0045-etat-impulsion-expose-en-calendrier.md) | État d'impulsion exposé en calendrier (période / pouls / phase), pas en booléen instantané | Accepté |
| [0046](0046-accumulateur-demande-sous-pouls-minimal.md) | Accumulateur de demande sous le pouls minimal (amende le seuil C de l'ADR-0043) | Accepté |
| [0047](0047-calibration-de-type-par-defaut-et-gel-amplitude.md) | Calibration de type par défaut pour la flotte + gel de l'amplitude d'impulsion (amende le fail-closed des défauts de l'ADR-0043) | Accepté (valeurs d'usine amendées par ADR-0050) |
| [0048](0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md) | Seuils de sécurité pH **fixes**, dérivés de la plage de consigne [7,20 ; 7,50] et appliqués au pH **corrigé** ; deux butées de dernier recours sur le brut (6,20 / 8,10) ; surveillance séparée de la décision ; acquittement explicite fail-closed d'une bascule de sécurité (amende feature-067 et l'ADR-0039) | Accepté |
| [0049](0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md) | La cause de refus de dosage est transportée par un **code machine** (contrat de sérialisation figé), et discriminée sur l'**état interne du filtre** — jamais sur la fraîcheur de la lecture brute ; priorité pannes d'abord ; grâce de ré-amorçage lue par `stale()` mais pas par `ready()` | Accepté |
| [0050](0050-debit-pleine-puissance-constante-unique-de-flotte.md) | Le débit à pleine puissance est une constante **unique de flotte** (74,0 = max mesuré 68,0 + 8 % de dispersion), la marge doit dépasser la dispersion observée, les maxima mesurés deviennent des `static_assert`, aucun miroir client d'une valeur de sécurité (amende les valeurs d'usine de l'ADR-0047) | Accepté |
| [0051](0051-pouls-minimal-double-rendement-avant-comptage.md) | Pouls minimal doublé (5 s → 10 s) : améliorer le **rendement** des démarrages d'abord, corriger le modèle de **comptage** ensuite — la dose demandée est inchangée, le compteur reste faux (dette assumée, amende le pouls minimal de l'ADR-0043) | Accepté |
| [0052](0052-terme-integral-ph-dimensionne-par-autorite.md) | Levée de `Ki = 0` sur la **seule** régulation pH (un P pur sous charge permanente ne peut pas annuler son droop) : `integralMax` **dimensionné par l'autorité** `U × Ti / kp` et non « en pH·s », intégration **signée** sur `erreur − phStopThreshold` par quanta d'1 s, gel d'anti-windup **asymétrique**, aucune persistance NVS (amende le `Ki = 0` de l'ADR-0016) | Accepté (clause « aucune persistance NVS » amendée par ADR-0053 ; tout le reste inchangé) |
| [0053](0053-persistance-conditionnelle-integrale-ph.md) | Persistance **conditionnelle** de l'intégrale pH sous sept garanties cumulatives R1-R7 : ce qui est restauré n'est pas une autorisation de doser mais la **vitesse d'une correction déjà décidée par la mesure** (la valeur relue n'est jamais appliquée au démarrage — elle attend 5 min d'écart soutenu) ; plafond = `min(0,30 × integralMax ; 0,50 × équilibre OBSERVÉ)`, l'intégrité de la charge observée étant une **propriété de sécurité** (amende la clause « aucune persistance NVS » de l'ADR-0052) | Accepté |
| [0054](0054-seuil-de-vraisemblance-fonction-de-la-configuration.md) | Un **seuil de vraisemblance dérivé d'un paramètre configurable est une FONCTION de ce paramètre, jamais une constante** — le figer transforme un garde-fou en piège silencieux (le filet mL/min de l'anneau de charge suit la limite horaire configurée et le débit d'amplitude calibré ; sentinelle verrouillant les deux sens) | Accepté |
| [0055](0055-feedforward-appris-additif-plafonne.md) | Feedforward pH **appris, additif, plafonné** : la dose de base (médiane 7 j de l'anneau de charge, réutilisée) entre comme **terme additif** dans l'unique chemin de commande — jamais comme seconde branche ; plafond **double** (60 % du soutenable **et** gel à 60 % du quota journalier) ; stabilité **structurelle par hystérésis de session** (bilan de matière : le volume journalier converge vers la charge réelle quel que soit le FF) ; latch fail-closed (erreur défavorable tenue 30 min, mesure invalide, changement de contexte) ; dosage volumétrique calculé écarté (exigerait le volume du bassin, interdit par l'utilisateur) | Accepté |

## Template

Copier `_template.md` pour créer un nouvel ADR.
