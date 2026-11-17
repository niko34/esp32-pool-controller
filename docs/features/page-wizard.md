# Assistant de premier démarrage — `/wizard.html`

- **Fichier UI** : [`data/wizard.html`](../../data/wizard.html) (page autonome, hors SPA)
- **URL** : `http://192.168.4.1/wizard.html` (mode AP) ou `http://poolcontroller.local/wizard.html`

## Rôle

Assistant d'**onboarding** affiché au premier démarrage (ou après un factory reset) : il guide la mise en service initiale avant l'accès à l'interface principale. L'assistant se rouvre tant que le mot de passe est celui d'usine (bug-185, v2.93.0 : l'état se déduit du mot de passe, [ADR-0131](../adr/0131-l-etat-de-premier-demarrage-se-deduit-du-mot-de-passe.md)). Sa seule requête anonyme est `POST /auth/change-password` (étape 1), qui rend le jeton ; **toutes** les écritures suivantes, dont `/save-config`, passent par `authFetch`. La fin est marquée par `POST /auth/complete-wizard`, désormais protégée par le jeton (`409` si le mot de passe est encore celui d'usine) — voir [`docs/subsystems/auth.md`](../subsystems/auth.md).

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
   > **v2.34.0 (feature-091)** : le texte d'aide de cette carte énumère désormais « limites d'injection, vitesse de correction, **volumes quotidiens** » — il annonçait un « délai de stabilisation » qui n'existe plus (la purge des mesures n'est plus réglable, [ADR-0059](../adr/0059-purge-adaptative-fenetre-de-medianes.md)). L'assistant **n'a jamais envoyé** `stabilization_delay_min` : la charge `POST /save-config` est **inchangée**.

**Sauvegarde** (`saveStepInstall()`, [`data/wizard.html`](../../data/wizard.html)) : **un seul `POST /save-config` combiné** `{ "install_mode": …, "pool_size": … }`. Le firmware évalue `pool_size` **avant** les clés individuelles et applique les défauts de la taille (voir [`docs/API.md`](../API.md#post-save-config--critical)).

- **Pas de dialogue avant/après** au premier démarrage : il n'y a **rien à écraser** (contrairement au changement de taille depuis la page Paramètres, qui affiche un récapitulatif).
- **Ancien firmware** : la clé `pool_size` est simplement **ignorée** (aucune erreur, `install_mode` traité normalement).

## Cas limites

- Étape Wi-Fi passée sans connexion → l'appareil reste en AP `PoolControllerAP` (`192.168.4.1`), le reste de l'assistant fonctionne.
- Le stepper interdit de sauter une étape ; « Précédent » ne rejoue pas les sauvegardes déjà faites.

## Fichiers

- [`data/wizard.html`](../../data/wizard.html) — page complète (HTML + CSS + JS inline)
- [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) — `/save-config` (bloc `pool_size`, feature-090)
- [`src/web_routes_auth.cpp`](../../src/web_routes_auth.cpp) — `/auth/change-password`, `/auth/complete-wizard` ; [`src/auth_bootstrap_logic.h`](../../src/auth_bootstrap_logic.h)
