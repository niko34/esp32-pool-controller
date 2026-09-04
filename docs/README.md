# Documentation — ESP32 Pool Controller

Ce dossier contient **trois types de documentation** (+ quelques guides transverses). Bien distinguer leurs rôles est essentiel pour maintenir la cohérence au fil du temps.

## Structure

```
docs/
├── README.md            ← ce fichier (vue d'ensemble)
│
├── features/            ← Doc vivante, 1 fichier par surface UI (page)
│   ├── README.md
│   ├── page-dashboard.md
│   ├── page-filtration.md
│   ├── page-lighting.md
│   ├── page-temperature.md
│   ├── page-ph.md
│   ├── page-orp.md
│   ├── page-dosages.md
│   ├── page-settings.md
│   └── page-wizard.md
│
├── user/                ← Documents pour l'utilisateur FINAL (PDF vulgarisés, dérivés — voir user/README.md)
│   └── guide-regulation-v<version>.pdf
│
├── subsystems/          ← Doc vivante, 1 fichier par composant firmware
│   ├── README.md
│   ├── regulation-vue-d-ensemble.md   ← commencer ici pour comprendre la régulation
│   ├── pump-controller.md
│   ├── sensors.md
│   ├── filtration.md
│   ├── lighting.md
│   ├── rtc-manager.md
│   ├── ws-manager.md
│   ├── mqtt-manager.md
│   ├── history.md
│   ├── ota-manager.md
│   ├── auth.md
│   ├── logger.md
│   ├── system-health.md
│   ├── oom-guard.md
│   └── web-server.md
│
├── adr/                 ← Architecture Decision Records (immuables)
│   ├── README.md
│   ├── _template.md
│   ├── 0001-capteurs-analogiques-ads1115.md
│   ├── 0002-mode-programmee-volume-quotidien.md
│   ├── 0003-calibration-orp-cote-client.md
│   ├── 0004-mode-regulation-enum-3-valeurs.md
│   ├── 0005-websocket-push-sans-polling.md
│   ├── 0006-frontend-vanilla-js.md
│   ├── 0007-table-partitions-custom.md
│   ├── 0008-persistance-cumuls-journaliers-nvs.md
│   ├── 0009-partition-coredump.md
│   ├── 0010-stabilite-mqtt-reseau.md
│   ├── 0011-mqtt-task-dediee.md
│   ├── 0012-mapping-gpio-pcb-v2.md
│   ├── 0013-identification-sondes-onewire.md
│   ├── 0014-migration-atlas-ezo.md
│   ├── 0015-partition-app-1.5mb.md
│   ├── 0016-regulation-p-temporisee-vs-pid.md
│   ├── 0017-logique-metier-pure-humble-object-testabilite.md
│   ├── 0018-migration-uplot.md
│   ├── 0019-partition-app-1664k.md
│   ├── 0020-budget-horaire-dosage-unique.md
│   ├── 0021-repartition-scheduled.md
│   ├── 0022-verification-integrite-ota.md
│   ├── 0023-politique-cors-retrait.md
│   ├── 0024-partitions-layout-v4.md
│   ├── 0025-mode-boost.md
│   ├── 0026-mode-installation.md
│   ├── 0027-filtre-tendance-centrale.md
│   ├── 0028-migration-fork-esp32async.md
│   ├── 0029-auto-guerison-heap.md
│   ├── 0030-watchdog-connectivite.md
│   ├── 0031-disjoncteur-bootloop-history.md
│   ├── 0032-reponses-http-bornees.md
│   ├── 0033-budget-espace-partition-history.md
│   ├── 0034-migration-core3-idf5.md
│   ├── 0035-parachute-oom-new-handler.md
│   ├── 0036-psram-requise-mise-a-jour-en-ligne.md
│   ├── 0037-postmortem-oom-et-integrite-tas.md
│   ├── 0038-metrique-memoire-allouable-8-bits.md
│   ├── 0039-offset-ph-applique-en-sortie-des-accesseurs.md
│   ├── 0040-pas-de-calibrage-sur-fragmentation-pathologique.md
│   ├── 0041-autorite-mesures-non-representatives-et-sortie-de-secours-filtre.md
│   ├── 0042-core3-environnement-par-defaut.md
│   ├── 0043-dosage-proportionnel-de-temps-amplitude-calibree.md
│   ├── 0044-budget-horaire-et-temps-min-en-temps-pompe-on.md
│   ├── 0045-etat-impulsion-expose-en-calendrier.md
│   ├── 0046-accumulateur-demande-sous-pouls-minimal.md
│   ├── 0047-calibration-de-type-par-defaut-et-gel-amplitude.md
│   ├── 0048-seuils-ph-fixes-derives-de-la-plage-de-consigne.md
│   ├── 0049-code-machine-de-cause-de-refus-et-etat-interne-du-filtre.md
│   ├── 0050-debit-pleine-puissance-constante-unique-de-flotte.md
│   ├── 0051-pouls-minimal-double-rendement-avant-comptage.md
│   ├── 0052-terme-integral-ph-dimensionne-par-autorite.md
│   ├── 0053-persistance-conditionnelle-integrale-ph.md
│   ├── 0054-seuil-de-vraisemblance-fonction-de-la-configuration.md
│   ├── 0055-feedforward-appris-additif-plafonne.md
│   ├── 0056-sous-dimensionnement-actionneur-3-jours-80-pct.md
│   ├── 0057-taille-bassin-preselection-jamais-entree-de-calcul.md
│   ├── 0058-gardes-heap-metrique-8-bits.md
│   ├── 0059-purge-adaptative-fenetre-de-medianes.md
│   ├── 0060-fraicheur-en-temps-de-filtration.md
│   └── 0061-bascule-psram-au-dela-de-4095-octets-reserve-parachute-en-interne.md
│
├── API.md               ← Référence REST + WebSocket
├── MQTT.md              ← Topics MQTT + auto-discovery Home Assistant
├── BUILD.md             ← Build firmware + filesystem
└── UPDATE_GUIDE.md      ← Procédure OTA
```

## Les trois types et quand les utiliser

### 1. `specs/features/` (au niveau racine du projet, **pas ici**)

**Spécifications éphémères**. Un cycle par feature : `todo/` → `doing/` → `done/`. Contiennent :
- contexte & motivation,
- les décisions prises au moment de l'implémentation,
- les itérations et retours utilisateur,
- les risques identifiés.

Une fois en `done/`, servent d'**archive historique** — on les relit pour comprendre « pourquoi on a fait ça » mais elles ne sont plus mises à jour.

### 2. `docs/features/` — par **surface UI**

Documentation **vivante** d'une page. Doit toujours décrire l'état courant. Contient :
- rôle de la page,
- structure (cartes, toggles, boutons),
- données consommées (champs WS + `/get-config`),
- actions déclenchées (endpoints + niveau d'auth),
- règles firmware appliquées,
- cas limites.

**À mettre à jour au même commit que la UI**.

### 3. `docs/subsystems/` — par **composant firmware**

Documentation **vivante** d'un composant (une classe C++ singleton). Contient :
- rôle du composant,
- API publique,
- algorithme / machine à états,
- interactions avec les autres composants,
- endpoints HTTP / topics MQTT exposés,
- cas limites.

**À mettre à jour au même commit que le code**.

### 4. `docs/adr/` — décisions architecturales

Un ADR est **immuable**. On ne modifie pas un ADR accepté : si une décision change, on crée un nouvel ADR qui supersede l'ancien. Utiliser [_template.md](adr/_template.md) comme base.

À créer dès qu'on prend une décision qui :
- contraint d'autres parties du système,
- n'est pas évidente à lire dans le code,
- a des alternatives crédibles qu'il faut documenter.

## Workflow type

1. Nouvelle feature demandée → créer `specs/features/todo/feature-NNN-xxx.md` à partir du template.
2. Spec validée → `spec-reader` la déplace vers `specs/features/doing/`.
3. Implémentation (agents `architect` → `embedded-developer` / `web-ui-developer` → `pool-chemistry` si chimie → `test-engineer` → `code-reviewer`).
4. Si une décision structurante est prise → nouvel ADR dans `docs/adr/`.
5. `doc-writer` met à jour **en même temps** :
   - les pages de `docs/features/` concernées,
   - les composants de `docs/subsystems/` concernés,
   - `CHANGELOG.md`,
   - `API.md` / `MQTT.md` si contrat changé.
6. Spec déplacée vers `specs/features/done/`.

## Conventions

- **Langue** : français, sauf identifiants code (anglais).
- **Liens** : chemins relatifs depuis le fichier courant, format `[Label](chemin/fichier.md)` ou `[fichier.ext:123](../src/fichier.cpp)` pour pointer une ligne.
- **Citations code** : toujours avec la ligne (`[`file.h:42`](../src/file.h:42)`) pour survivre aux renames et permettre au lecteur de vérifier rapidement.
- **Vérifiabilité** : chaque affirmation technique doit être reliée à un fichier source — pas de documentation-from-memory.
