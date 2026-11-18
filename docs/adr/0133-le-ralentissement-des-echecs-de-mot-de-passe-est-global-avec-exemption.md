# ADR-0133 — Le ralentissement des échecs de mot de passe est global pour les adresses inconnues, avec exemption des adresses ayant prouvé le mot de passe

- **Statut** : Accepté
- **Date** : 2026-11-18
- **Décideurs** : Nicolas + Claude (`architect`, `security-reviewer` passes P1 et P2, `code-reviewer`)
- **Spec(s) liée(s)** : [bug-189](../../specs/features/done/bug-189-aucun-verrouillage-sur-les-essais-de-mot-de-passe-en-ligne.md) · [ADR-0127](0127-le-bornage-du-journal-d-authentification-est-global-par-emetteur.md) · [ADR-0132](0132-le-mot-de-passe-se-compare-par-empreintes-en-temps-fixe.md)

## Contexte

`kMaxRequestsPerMinute = 120` plafonne le **débit** par adresse, pas la **sanction** : un essai de mot de passe raté ne coûtait rien de plus que le suivant. Soit **172 800 essais par jour et par adresse** ; un dictionnaire d'un million de mots s'épuisait en ~6 jours depuis une seule adresse, et la borne se multiplie par le nombre d'adresses que l'attaquant prend sur le réseau.

Contrainte rédhibitoire : **l'OTA est le seul canal de reprise** (pas de port série). Un propriétaire durablement exclu de son appareil = appareil **perdu**. Un « verrouillage de compte » classique est donc exclu ; il faut un ralentissement qui s'efface seul par écoulement du temps.

Contrainte de plate-forme : AsyncWebServer n'admet aucun handler bloquant > 50 ms. Le ralentissement ne peut pas être un `delay()` : c'est le **débit** qui doit s'effondrer, par un refus immédiat.

## Décision

⭐ **La clé du ralentissement est GLOBALE pour les adresses inconnues ; une adresse ayant prouvé le mot de passe n'est freinée que par sa propre porte.**

Trois portes, dans le module pur `src/auth_fail_backoff_logic.h` (ADR-0017), à capacité fixe, sans allocation ni journal :

| Porte | Paramètres |
|---|---|
| Par adresse IP | 8 créneaux, franchise **3** essais, plafond **60 s**, pardon **300 s** |
| Globale | franchise **10**, plafond **10 s**, pardon **60 s** |
| Exemption | **4** adresses ayant prouvé le mot de passe : elles échappent à la porte globale, chacune gardant **sa propre** porte par adresse |

Refus **immédiat** : HTTP **429** + `Retry-After: N` + corps `{"error":"…","retryAfter":N}`. Le pardon est **incrémental** (un échec effacé par période), sans échéance absolue : l'attente est au plus le plafond, par construction, même au repliement de `millis()`.

⛔ **La garde reste HORS de la chaîne d'admission** (`checkAuth`, `decideAdmission`). `src/auth.cpp:338` n'examine le jeton que si le quota passe : y placer la garde **désarmerait le jeton** et enfermerait le propriétaire dehors. Elle est posée dans les handlers `/auth/login` (après `checkRateLimit`, avant `deserializeJson`) et `/auth/change-password` (après l'analyse, avant `passwordMatches` et `checkTokenAuth`, et seulement pour l'essai par mot de passe actuel, hors état vierge). Un jeton valide passe donc **toujours**.

Tout `kAllow` de `/auth/change-password` vide la table d'exemption puis y réinscrit l'auteur (sauf adresse inconnue).

## Alternatives considérées

| Option | Protection | Propriétaire pendant l'attaque |
|---|---|---|
| Par adresse seule (rejetée) | ⛔ ×2,4 à 50 adresses : **rate le critère** | jamais gêné |
| Globale simple (rejetée) | ×1 000 | ⛔ **famine totale** |
| ⭐ Globale + exemption (retenue) | ×1 000 | passe, **si déjà connecté depuis le dernier redémarrage** |

Le « ×120 » de l'option par adresse est un mirage : il vaut contre **une** adresse, 2,4 contre cinquante.

**Leçon chiffrée.** La conception initiale annonçait « au pire 10 s d'attente pour le propriétaire ». **Simulé : 0 connexion acceptée sur 40, en 10 minutes** : l'attaquant, qui frappe plus vite, prend le créneau libéré à chaque fois. C'est ce qui a motivé la table d'exemption. ⇒ Un chiffre exact en l'absence d'adversaire peut être faux en présence d'adversaire.

**Débits mesurés** (simulation de la sémantique exacte du module, 7 jours, 24 stratégies) : **8 642 essais/jour** au maximum sur la porte globale, **1 441/jour** par adresse, soit un facteur **20×** (et **1 000×** face à 50 adresses). Aucune stratégie « rafale puis silence » ne bat l'attaque simple (le pardon incrémental fait que se taire ne rend jamais plus d'un essai par période). Une première affirmation de revue (~16 100/jour) était fausse et a été retirée après mesure.

## Conséquences

### Positives
- Un dictionnaire d'un million de mots passe de ~6 jours à ~115 jours (porte globale).
- Aucun handler ne bloque ; le refus ne construit aucun `JsonDocument`.
- Le ralentissement s'efface seul ; borne dure dans `constants.h`.

### Négatives / dette assumée — ⛔ ce que la conception ne couvre PAS

1. **8 640 essais/jour** en attaque distribuée, jusqu'à **14 400** si l'attaquant tient les quatre adresses exemptées.
2. Un **mot de passe faible** : une liste de 10 000 mots tombe en **1,2 jour**.
3. L'écoute du mot de passe **en clair** sur le Wi-Fi (décision produit assumée).
4. ⚠️ **Famine du propriétaire hors table pendant une attaque active** : après un **redémarrage** (la table est en RAM), depuis un **nouveau client**, ou depuis la **page de secours OTA** (interface cassée ⇒ redémarrage ⇒ table vide). L'exemption **réduit** ce risque, elle ne le supprime pas.
5. **NAT partagé** avec l'attaquant : la porte par adresse monte à 60 s, famine possible.
6. ⚠️ **`/save-config` vide la table sans réinscrire l'auteur** (`src/web_routes_config.cpp:1645` → `setPassword`). Vérifié : aucune page n'emploie ce chemin et le jeton n'y est pas renouvelé, donc la session survit.
7. Le modèle de test **n'est pas le handler réel**, et la garde CI `check_auth_fail_guard.sh` **lit le source** : un appel indirect (pointeur, macro) lui échapperait. Les handlers HTTP ne se compilent pas en natif : brancher la garde sur le chemin du jeton laisserait la suite native verte (mutant survivant consigné).

### Ce que ça verrouille
- Toute nouvelle route d'authentification par mot de passe doit appeler les trois fonctions du module et rester hors `checkAuth`.
- Les constantes `kAuthFail*` (voir `docs/subsystems/auth.md`) : les changer change les débits ci-dessus, à re-simuler.
- Un avenir « verrouillage persistant » en NVS devra porter une borne de durée dure et ne pas enfermer le propriétaire.

## Références

- Code : `src/auth_fail_backoff_logic.h`, `src/web_routes_auth.cpp`, `src/constants.h` (`kAuthFail*`), `tools/check_auth_fail_guard.sh`
- Tests : `test/test_native_auth_fail_backoff/`
- Spec : [bug-189](../../specs/features/done/bug-189-aucun-verrouillage-sur-les-essais-de-mot-de-passe-en-ligne.md)
- Doc : [`docs/subsystems/auth.md`](../subsystems/auth.md)
