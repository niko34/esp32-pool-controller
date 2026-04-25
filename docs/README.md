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
│   └── page-settings.md
│
├── subsystems/          ← Doc vivante, 1 fichier par composant firmware
│   ├── README.md
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
│   └── 0008-persistance-cumuls-journaliers-nvs.md
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
