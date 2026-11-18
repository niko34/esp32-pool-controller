# ADR-0134 — La clé du point d'accès est modifiable, et la réinitialisation d'usine ramène toujours celle de l'étiquette

- **Statut** : Accepté
- **Date** : 2026-11-18
- **Décideurs** : Nicolas + Claude (`architect`, `security-reviewer`, `code-reviewer`)
- **Spec(s) liée(s)** : [bug-187](../../specs/features/doing/bug-187-mot-de-passe-du-point-d-acces-a-32-bits.md) · [ADR-0131](0131-l-etat-de-premier-demarrage-se-deduit-du-mot-de-passe.md) · [ADR-0133](0133-le-ralentissement-des-echecs-de-mot-de-passe-est-global-avec-exemption.md)

## Contexte

La clé WPA2 du point d'accès de secours (`PoolControllerAP`) était fabriquée sur **32 bits** (8 caractères hexadécimaux). Elle s'attaque **hors ligne** après capture d'une poignée de main : aucun ralentissement côté appareil ne s'y applique.

Méthode du chiffre (ADR-0073) : WPA2-PSK = PBKDF2-HMAC-SHA1, 4096 tours ; une carte graphique du commerce fait ≈ 2 millions d'essais/seconde. L'espace 2³² tombe en ≈ 36 minutes ; 12 caractères tirés dans 72 symboles avec quatre familles obligatoires (≈ 73,5 bits réels) sont hors de portée. Ces débits montent avec le temps.

Contrainte du parc : des appareils sont déjà installés, avec une clé à 32 bits **imprimée sur leur étiquette**. Le point d'accès n'est allumé qu'à l'installation ou en repli, et l'appareil n'a **pas de port série** utilisable sur le terrain : toute perte d'accès doit pouvoir se réparer sans câble.

## Décision

1. **Deux valeurs en NVS** : la clé **d'origine** (`auth_ap_org`, celle de l'étiquette) est écrite **une seule fois** à la mise en service et jamais réécrite ; la clé **courante** (`auth_ap_pwd`) est celle que le point d'accès utilise réellement.
2. **L'utilisateur peut remplacer la clé courante** (Paramètres ▸ Wi-Fi, `POST /auth/ap-password`), avec la règle des quatre familles et 12 caractères minimum.
3. **La réinitialisation d'usine restaure l'origine**, pas la courante. L'étiquette ne ment jamais, et « j'ai perdu la clé que j'avais choisie » se règle par un reset.
4. **Aucune régénération automatique du parc.** Un appareil installé garde sa clé à 32 bits ; il peut désormais en choisir une solide, et l'interface le lui conseille (`ap_password_legacy`).
5. **Deux alphabets asymétriques** : fabrication restreinte (72 symboles, sans `I l 1 O 0 o` ni `^`, touche morte sur clavier français) ; contrôle d'une clé choisie sur tout l'ASCII imprimable. Le tirage rejette les octets ≥ 216 (pas de biais de modulo).
6. **Deux planchers** : 12 pour la clé Wi-Fi (attaque hors ligne), 8 pour le mot de passe de l'appareil (attaque en ligne, protégée par le ralentissement de l'ADR-0133). La règle des quatre familles s'applique aux deux, **à la saisie seulement** : jamais à la connexion, pour ne pas enfermer dehors un mot de passe existant.

## Alternatives considérées

- **Régénérer automatiquement la clé de tout le parc à la mise à jour** (rejetée par l'utilisateur) — rendrait l'étiquette **fausse** sur chaque appareil installé, imposerait un bandeau d'avertissement et un drapeau persistant d'acquittement, et créerait un **risque d'enfermement dehors** : une régénération pendant que l'utilisateur est connecté en secours, ou avant qu'il ait relevé la nouvelle clé, le coupe de l'appareil sans autre recours que l'OTA. Trop de mécanique pour un point d'accès éteint en service normal.
- **Clé non modifiable, simplement plus longue** (rejetée) — règle le chiffre mais laisse les appareils déjà installés sans issue, et empêche l'usage d'un gestionnaire de mots de passe.
- **Clé modifiable, reset qui conserve le choix de l'utilisateur** (rejetée) — l'étiquette devient fausse, et il n'existe plus de moyen de reprendre la main sans câble.
- **Clé dérivée du matériel (identifiant de puce)** (rejetée) — prévisible par quiconque connaît l'appareil, donc pas d'entropie réelle.
- **Retenue** : origine figée + courante modifiable + reset vers l'origine.

## Conséquences

### Positives
- L'étiquette reste vraie à vie ; la reprise d'accès ne demande aucun outil.
- Les appareils installés ne subissent aucune migration forcée.
- Les nouveaux appareils sont hors de portée d'une attaque hors ligne (≈ 73,5 bits).

### Négatives / dette assumée
- Un appareil non migré garde une clé à 32 bits (36 min sur une carte) tant que son propriétaire n'en choisit pas une autre ; seul un conseil dans l'interface l'y invite.
- Le changement ne prend effet qu'au prochain démarrage du point d'accès (on ne coupe pas le réseau sur lequel l'utilisateur est assis).
- La clé reste stockée en clair en NVS (voir feature-192) ; elle est journalisée en clair à la génération, avant toute étiquette (décision tranchée dans la fiche).
- Aucun gestionnaire de mots de passe ne remplit seul la fenêtre Wi-Fi d'un téléphone : l'utilisateur la tape une fois.

### Ce que ça verrouille
- `apPasswordOrigin` n'est écrite qu'une fois ; toute évolution qui la réécrirait casserait la promesse de l'étiquette.
- La restauration d'usine doit continuer de recopier l'origine dans la courante (`resetWiFiSettings()`).
- Le plancher de 12 caractères de la clé Wi-Fi ne doit pas être aligné sur celui de l'appareil.

## Références

- Code : `src/ap_password_logic.h`, `src/auth.cpp` (`setApPassword`, `getApPasswordOrigin`), `src/config.cpp` (`reloadApPasswordFromNvs`), `src/web_routes_auth.cpp`, `src/main.cpp` (`resetWiFiSettings`)
- Tests : `test/test_native_ap_password/`
- Spec : `specs/features/doing/bug-187-mot-de-passe-du-point-d-acces-a-32-bits.md`
