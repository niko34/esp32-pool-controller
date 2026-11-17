# ADR-0131 — L'état de premier démarrage se déduit du mot de passe, pas d'un drapeau

- **Statut** : Accepté
- **Date** : 2026-11-17
- **Décideurs** : l'utilisateur (axe « preuve de présence physique » : *« rien de plus »*), `security-reviewer` et `architect` (conception ; arbitrage sur `/save-config` tranché pour la suppression de l'exemption)
- **Spec(s) liée(s)** : [bug-185](../../specs/features/done/bug-185-fenetre-de-premier-demarrage-ouverte-indefiniment.md) · [bug-184](../../specs/features/done/bug-184-authentification-basic-accepte-un-mot-de-passe-qui-n-est-pas-le-bon.md) · [ADR-0130](0130-l-appareil-n-accepte-que-le-jeton-pas-http-basic.md)

## Contexte

Jusqu'à la v2.92.0, « l'appareil est en premier démarrage » se lisait dans un **drapeau mémorisé** (`wizardCompleted`). Ce drapeau était posé par `POST /auth/complete-wizard`, route **sans aucune garde**. Trois conséquences :

1. Un inconnu sur le réseau local pouvait **refermer** la fenêtre sans avoir choisi de mot de passe. `/auth/login` ne refusait `admin` que pendant le premier démarrage : `admin`/`admin` valait alors **pour toujours**, et le propriétaire ne voyait plus son assistant.
2. Pendant la fenêtre, le corps anonyme de `/save-config` écrivait aussi `auth_enabled`, le mot de passe et le **courtier MQTT** (qui pilote les consignes de dosage).
3. Le jeton n'était jamais régénéré : un jeton pris pendant la fenêtre survivait au choix du mot de passe.

## Décision

**Un droit anonyme n'existe que si aucun mot de passe n'a jamais été choisi, et il se limite à en choisir un.** L'état de sécurité se déduit du **mot de passe lui-même** (« vaut `admin`, ou vide »), jamais d'un bit qu'un tiers peut poser.

- Deux notions **distinctes**, ne jamais les confondre : `factoryPassword` (accorde ou refuse les droits) et `firstBoot` (pilote l'affichage de l'assistant). Les confondre était le défaut.
- `/auth/login` refuse le mot de passe d'usine (`403`) **quel que soit** `firstBoot`.
- `/auth/change-password` est anonyme **uniquement** à l'état vierge ; il **régénère le jeton** et rend le nouveau.
- `/auth/complete-wizard` exige le jeton et refuse (`409`) tant que le mot de passe est celui d'usine.
- L'exemption anonyme de `/save-config` est **supprimée** ; `GET /get-config` passe en `WRITE`.
- Un compteur `kAuthEpoch` fait régénérer le jeton **une fois** à la mise à jour (les jetons déjà pris ne sont pas retirés par le seul correctif).
- Migration auto-réparatrice : `wizardCompleted` vrai **et** mot de passe d'usine ⇒ l'assistant se rouvre.

La logique est dans un module pur, [`src/auth_bootstrap_logic.h`](../../src/auth_bootstrap_logic.h), testé en natif (ADR-0017).

## Alternatives considérées

- **Régénérer le jeton seulement** (rejetée) : tant que `complete-wizard` est ouverte, fermer la fenêtre ne ferme rien — ça débloque même `/auth/login` sur le mot de passe d'usine.
- **Borner la fenêtre dans le temps** (rejetée) : un appareil alimenté en stock chez un revendeur ou un client arriverait **non amorçable** — l'impasse déjà rencontrée sur bug-184.
- **Supprimer `/auth/complete-wizard`** (rejetée) : l'assistant et les clients existants l'appellent ; elle est conservée comme filet idempotent, désormais protégée.
- **Garder l'exemption de `/save-config` « parce que la fenêtre se referme plus tôt »** (rejetée) : l'attaque tient en **une** requête ; une fenêtre plus courte reste une fenêtre de plusieurs semaines pour un appareil en stock. Rien n'en a besoin : l'assistant prend son jeton à `/auth/change-password`.
- **Preuve de présence physique** (bouton, code affiché, point d'accès dédié) (rejetée par l'utilisateur : « rien de plus ») : aucun geste nouveau au déballage.

## Corollaire imposé par la décision — `GET /auth/status` publie `virgin`

⚠️ Faire porter les droits sur le mot de passe **ne suffit pas côté serveur** : les pages doivent
savoir sur quoi brancher. Elles n'avaient que `firstBoot`, qui vaut
`!wizardCompleted || factoryPassword` — donc **vrai** sur un appareil déjà possédé dont l'assistant
a été interrompu.

⛔ **Conséquence mesurée** : ces pages proposaient alors le formulaire **anonyme** de choix de mot
de passe, que le serveur refuse désormais. La page de secours OTA — **le dernier filet de
réparation** — devenait une **impasse**, et la seule sortie restante était l'appui long sur le
bouton, qui efface étalonnages, consignes et limites.

⇒ `GET /auth/status` publie une clé **additive**, `virgin` = « aucun mot de passe n'a jamais été
choisi ». ⛔ **Alternative écartée** : collapser `firstBoot` sur `factoryPassword`. Elle était plus
courte d'une ligne, mais elle rendait `wizardCompleted` **mort** et la branche de fermeture de
`/auth/complete-wizard` **inatteignable** — deux constructions mortes dans un module de sécurité.

## Conséquences

### Positives
- `admin`/`admin` ne rend plus jamais de jeton, quel que soit l'ordre des appels.
- Un jeton pris pendant la fenêtre cesse de fonctionner dès que le mot de passe est choisi.
- Un appareil déjà piégé se répare à la mise à jour.

### Négatives / dette assumée
- 🔴 **Ce lot ne ferme PAS « quiconque est sur le réseau local »** : entre la mise sous tension d'un appareil neuf et le choix du mot de passe, **le premier arrivé gagne**. L'attaque devient **visible** (l'assistant du propriétaire est refusé) et **réversible** (appui long sur le bouton physique). Décision de l'utilisateur.
- Changer le mot de passe déconnecte les autres sessions ; à la mise à jour 2.93.0, tout le monde se reconnecte une fois ; un `POOL_TOKEN` figé dans un script tiers devient périmé. MQTT et Home Assistant ne sont pas touchés.
- La rotation du jeton protège de l'**émission**, pas de l'**écoute** : le serveur reste en HTTP clair (décision du 2026-11-17).

### Ce que ça verrouille
- Aucun droit anonyme ne doit dépendre d'un drapeau modifiable par une requête.
- Incrémenter `kAuthEpoch` n'est permis que pour une raison de sécurité nommée (chaque incrément déconnecte tout le monde).
- Porte laissée ouverte, sans coût aujourd'hui : l'étiquette de chaque appareil porte déjà le code du point d'accès ; en faire le mot de passe de départ (à la place de `admin`) fermerait le risque résiduel pour ~15 lignes et aucun matériel.

## Références

- Code : [`src/auth_bootstrap_logic.h`](../../src/auth_bootstrap_logic.h), [`src/auth.cpp`](../../src/auth.cpp), [`src/web_routes_auth.cpp`](../../src/web_routes_auth.cpp), `kAuthEpoch` dans [`src/constants.h`](../../src/constants.h)
- Tests : `test/test_native_auth_bootstrap/`, `test/test_native_auth_admission/`
- Doc : [`docs/subsystems/auth.md`](../subsystems/auth.md), [`docs/API.md`](../API.md)
