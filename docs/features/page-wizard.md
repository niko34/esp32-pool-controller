# Assistant de premier démarrage — `/wizard.html`

- **Fichier UI** : [`data/wizard.html`](../../data/wizard.html) (page autonome, hors SPA)
- **URL** : `http://192.168.4.1/wizard.html` (mode AP) ou `http://poolcontroller.local/wizard.html`

## Rôle

Assistant d'**onboarding** affiché au premier démarrage (ou après un factory reset) : il guide la mise en service initiale avant l'accès à l'interface principale. La fin de l'assistant est marquée côté firmware par `POST /auth/complete-wizard` (voir [`docs/subsystems/auth.md`](../subsystems/auth.md)). L'exemption d'authentification « premier démarrage » de `/save-config` couvre ce parcours (voir [`docs/API.md`](../API.md#authentification-du-corps-de-requête-durcissement-v2250)).

## Déroulé — 4 étapes

| # | Étape | Contenu / endpoint |
|---|-------|--------------------|
| 1 | **Mot de passe** | Mot de passe administrateur (≥ 8 caractères, 1 chiffre, 1 spécial — checklist temps réel + jauge de force) |
| 2 | **Wi-Fi** | Connexion au réseau (optionnelle — scan, saisie, ou rester en AP) |
| 3 | **Installation et bassin** | Mode d'installation **+ taille du bassin** (voir ci-dessous) |
| 4 | **Heure** | NTP (timezone, serveur) ou heure manuelle |

## Étape 3 — « Installation et bassin » (feature-056 / feature-090)

Étape renommée « **Installation et bassin** » en v2.32.0 (feature-090, était « Installation »). Deux radiogroupes de cartes :

1. **Mode d'installation** (`install_mode` : `managed` / `powered` / `external`, défaut `managed`) — le câblage réel, qui garantit qu'aucun produit n'est injecté sans circulation d'eau. Mêmes 3 archétypes que l'onglet Installation de la [page Paramètres](page-settings.md#installation-panel-install-feature-056) ([ADR-0026](../adr/0026-mode-installation.md)).
2. **Taille du bassin** (`pool_size` : `small` / `medium` / `large`, défaut `medium` coché) — libellés « Moins de 20 m³ » / « 20 à 60 m³ » / « Plus de 60 m³ ». Le choix **présélectionne** les défauts de régulation de la taille ([ADR-0057](../adr/0057-taille-bassin-preselection-jamais-entree-de-calcul.md)) ; tous restent modifiables ensuite dans Paramètres → Régulation.

**Sauvegarde** (`saveStepInstall()`, [`data/wizard.html`](../../data/wizard.html)) : **un seul `POST /save-config` combiné** `{ "install_mode": …, "pool_size": … }`. Le firmware évalue `pool_size` **avant** les clés individuelles et applique les défauts de la taille (voir [`docs/API.md`](../API.md#post-save-config--critical)).

- **Pas de dialogue avant/après** au premier démarrage : il n'y a **rien à écraser** (contrairement au changement de taille depuis la page Paramètres, qui affiche un récapitulatif).
- **Ancien firmware** : la clé `pool_size` est simplement **ignorée** (aucune erreur, `install_mode` traité normalement).

## Cas limites

- Étape Wi-Fi passée sans connexion → l'appareil reste en AP `PoolControllerAP` (`192.168.4.1`), le reste de l'assistant fonctionne.
- Le stepper interdit de sauter une étape ; « Précédent » ne rejoue pas les sauvegardes déjà faites.

## Fichiers

- [`data/wizard.html`](../../data/wizard.html) — page complète (HTML + CSS + JS inline)
- [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) — `/save-config` (bloc `pool_size`, feature-090)
- [`src/auth_manager.cpp`](../../src/) — `/auth/complete-wizard`, exemption premier démarrage
