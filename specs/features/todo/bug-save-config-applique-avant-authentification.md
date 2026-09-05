# Bug — `/save-config` applique et persiste la configuration AVANT la vérification d'authentification

- **Type** : bug de sécurité (format minimal `specs/modele.md`)
- **Statut** : todo
- **Priorité** : haute
- **Version cible** : à définir
- **Créée** : 2026-08-15 (relevé par `code-reviewer` pendant le cycle feature-067,
  signalé comme PRÉ-EXISTANT et hors périmètre de ce cycle)
- **Domaine** : Web — routes HTTP, Authentification, et indirectement Régulation
- **Specs liées** : feature-067 (offset pH — c'est elle qui a rendu le défaut visible)

## Symptôme

Dans `src/web_routes_config.cpp`, `handleSaveConfig()` est le **handler de corps**
de la route `POST /save-config`. Il **applique la configuration en mémoire ET la
persiste** (`saveMqttConfig()`, vers la ligne 597) — alors que `REQUIRE_AUTH` n'est
évalué que dans le **handler de requête**, exécuté APRÈS (vers la ligne 873).

Conséquence : un client non authentifié qui envoie un corps JSON valide voit sa
configuration **appliquée et écrite en NVS**, avant que la requête ne soit refusée.
Le refus HTTP arrive trop tard : l'effet de bord a déjà eu lieu.

## Pourquoi ça compte davantage depuis la feature-067

`ph_offset` ajoute à cette route un paramètre qui **décale la consigne effective de
la régulation pH** : écrire un offset décale le pH réel visé par le dosage d'acide.
Les autres paramètres exposés par cette route (cibles pH/ORP, limites de dosage,
mode de régulation) étaient déjà sensibles ; ce nouveau champ élargit la surface.

Les filets de sécurité chimique livrés en feature-067 (plancher acide sur le pH
brut, inhibition ORP) bornent les conséquences, mais ne suppriment pas le défaut.

## Correctif attendu

1. Déplacer la vérification d'authentification **avant** toute application ou
   persistance — soit en la faisant dans le handler de corps dès le premier
   fragment reçu, soit en différant l'application jusqu'au handler de requête
   (le corps serait alors seulement mis en tampon, ce qui est déjà le cas).
2. Vérifier le comportement des autres routes construites sur le même patron
   (handler de corps + handler de requête) : `POST /update`, calibration,
   toute route acceptant un corps JSON.
3. Ne pas casser le flux de premier démarrage (assistant de configuration), qui
   peut légitimement écrire une configuration avant qu'un mot de passe existe —
   à vérifier explicitement.

## Critères d'acceptation

- [ ] Une requête `POST /save-config` non authentifiée ne modifie NI la
      configuration en mémoire, NI la NVS (vérifiable : relire `/get-config`
      après la tentative)
- [ ] Le flux de premier démarrage reste fonctionnel
- [ ] Les autres routes à handler de corps sont auditées et le résultat consigné
- [ ] `pio run` bi-core verts, `pio test -e native` sans régression

## Mise à jour du statut (2026-08-27) — cœur livré, spec MAINTENUE

Le défaut principal a été **corrigé au passage** du cycle de refonte des seuils
pH (v2.25.0) : `saveConfigBodyAuthorized()` vérifie l'authentification **avant**
toute mutation, le gate d'acquittement 409 est le premier bloc du handler, et
`configMutex` est pris puis rendu sur chaque sortie. Confirmé par deux revues de
code successives : « auth `/save-config` non régressée, gate 409 avant toute
mutation ».

**Cette spec reste néanmoins en `todo/`** : deux de ses critères ne sont pas
remplis, et les cocher serait faux.

- [ ] **Audit des autres routes à handler de corps** — non fait. C'est le
  critère qui donne sa valeur à cette spec : le motif « corps appliqué avant
  vérification » n'a aucune raison d'être limité à `/save-config`, et rien ne
  garantit aujourd'hui qu'il ne se répète pas ailleurs. Tant que l'audit n'est
  pas conduit et son résultat consigné, le risque est inconnu — pas absent.
- [ ] **Build bi-core** — seul `esp32dev_core3` a été vérifié. L'environnement
  `esp32dev` (core 2) n'a pas pu être compilé (échec réseau PlatformIO lors du
  cycle, hors code).

Le premier critère est donc le vrai reste à faire, et il est indépendant du
correctif déjà livré.
