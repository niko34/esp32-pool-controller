# ADR-0117 — Un routage dynamique générique ne sert jamais un actionneur

- **Statut** : Accepté
- **Date** : 2026-10-18
- **Décideurs** : l'utilisateur (décision de **supprimer** plutôt que de garder sous garde,
  2026-10-18) · `security-reviewer` (**GO**, 2026-10-18 — recensement complet des chemins vers
  `setManualPump()`, aucune route légitime ne dépendait du filet `onNotFound`)
  ⚠️ **`pool-chemistry` n'a PAS été saisi, volontairement** : ce lot **retire** un chemin de
  dosage et n'en ajoute aucun. La règle du projet (*« tout NOUVEAU chemin de code vers
  `pump_controller` → validation `pool-chemistry` »*) ne s'applique pas à un retrait, et la
  direction du changement est strictement protectrice. ⛔ Dit ici plutôt que laissé supposer —
  un décideur nommé sans verdict tracé vaut moins que l'aveu de son absence.
- **Spec(s) liée(s)** : [bug-170](../../specs/features/done/bug-170-routes-duty-demarrent-une-pompe-sans-aucune-garde.md) · [feature-163](../../specs/features/done/feature-163-refus-de-dosage-pendant-un-etalonnage.md) (son recensement a trouvé ce chemin) · [ADR-0115](0115-une-garde-sur-etat-partage-vit-au-point-dusage-final.md) (principe voisin, angle différent)

## Contexte

`POST /pump1/duty/:duty` et `POST /pump2/duty/:duty` n'étaient pas déclarées comme des routes
`AsyncWebServer` ordinaires : elles vivaient dans `handleDynamicPumpRoutes()`, appelée depuis le
handler générique `server->onNotFound(...)` — un filtre de chaîne (`url.startsWith("/pump1/duty/")`)
exécuté pour **toute** requête qui ne correspondait à aucune route déclarée. Ce mécanisme a permis
à un chemin complet de démarrage de pompe doseuse de passer **sans aucune garde** (ni présence
d'eau, ni limite journalière/horaire, ni étalonnage — voir `bug-170`) pendant des mois, alors que
la route voisine `/pumpN/on` passait déjà par `injectionAllowedOrReject()`.

Ce n'était pas un oubli isolé sur ces deux routes précises. C'est le **cinquième** chemin de
démarrage de pompe découvert par recensement en instruisant feature-163 — et il a été trouvé
**après coup**, par relecture manuelle, précisément parce qu'il ne figure dans **aucune** liste de
routes déclarées (`setupControlRoutes()`, les fichiers `web_routes_*.cpp`). Un grep sur
`server->on(` ou une revue des handlers enregistrés ne le voit pas : il faut savoir que
`onNotFound` cache un second routage pour aller le chercher.

## Décision

Un handler `onNotFound` ne sert **que** le 404.

⚠️ **L'absolu est VOULU, et il a été choisi contre la version restreinte** (« … ne doit y être
ajouté *pour commander un actionneur* »). Deux raisons : il ne coûte rien — aucun besoin
fonctionnel de routage dynamique n'a été identifié dans ce projet — et surtout une règle
restreinte obligerait chaque futur contributeur à juger lui-même si ce qu'il route est
« inoffensif ». ⛔ C'est exactement ce jugement-là qui a échoué : les routes supprimées
paraissaient inoffensives, puisqu'elles n'étaient qu'un outil de test.

⇒ Aucun routage dynamique dérivé d'une inspection de chaîne d'URL (`url.startsWith(...)`,
découpage manuel d'un segment numérique, etc.) n'y est ajouté. ⚠️ **A fortiori** pour commander un
actionneur physique (pompe doseuse, relais de filtration, éclairage).
Toute route qui démarre, arrête ou règle la puissance d'un actionneur se déclare **explicitement**
via `server->on(...)`, aux côtés des autres routes du même domaine — visible dans l'inventaire des
routes, revue par le même gate `pool-chemistry` que ses voisines, et donc **structurellement
incapable** d'échapper au recensement qui a fini par la trouver.

## Alternatives considérées

- **Garder le routage dynamique et y ajouter la garde manquante** (`injectionAllowedOrReject()`
  dans `handleDynamicPumpRoutes()`) — **rejetée** : corrige l'instance trouvée, ne corrige pas le
  mécanisme. Rien n'empêche une future route ajoutée au même handler de reproduire exactement le
  même défaut, et aucun outil de recensement de routes ne la verrait avant qu'un audit manuel ne
  tombe dessus — exactement ce qui s'est produit ici.
- **Supprimer les deux routes sans statuer sur le mécanisme** — **rejetée en l'état** : aurait
  fermé l'instance sans empêcher la récidive. Retenue **en complément** de la règle ci-dessus
  (les routes étaient un reliquat sans appelant, cf. `bug-170`), pas à sa place.
- **Interdire tout routage dynamique dans `onNotFound()` pour un actionneur, et le documenter
  comme règle opposable** — **retenue** : ne coûte rien (aucun besoin fonctionnel identifié pour
  ce mécanisme), et empêche par construction qu'un futur raccourci de développement y loge une
  commande d'actionneur non gardée.

## Conséquences

### Positives
- Toute route qui agit sur un actionneur apparaît dans l'inventaire standard des routes
  (`server->on(...)`), lisible par grep, revue de code et tout futur outil de recensement.
- `onNotFound()` redevient un handler à une seule responsabilité (le 404), plus simple à auditer.

### Négatives / dette assumée
- Aucune dette identifiée : le routage dynamique n'avait pas d'usage fonctionnel actif (vérifié :
  rien dans `data/` ni aucun script n'appelait ces routes).

### Ce que ça verrouille
- Un futur besoin de route paramétrée par segment d'URL (`/pumpN/xxx/:valeur`) devra se déclarer
  explicitement — `AsyncWebServer` le permet nativement (routes avec capture), ce n'est pas une
  limitation technique qui justifiait le détour par `onNotFound`.
- Toute tentative de réintroduire un routage dynamique au même endroit pour un actionneur doit
  être relue comme une régression de cette décision, pas comme un raccourci neutre.

## Références

- Code : `src/web_server.cpp` (`onNotFound`, commentaire daté du retrait) · `src/web_routes_control.cpp` (ancien `handleDynamicPumpRoutes()`, supprimé le 2026-10-18)
- Spec : [bug-170](../../specs/features/done/bug-170-routes-duty-demarrent-une-pompe-sans-aucune-garde.md)
- Doc externe : [docs/API.md](../API.md) (retrait documenté), [docs/subsystems/pump-controller.md](../subsystems/pump-controller.md#test-de-pompe-g4--injectionallowedorreject)


## Note de lecture — les documents antérieurs

⚠️ **Deux ADR acceptés décrivent encore ces routes au présent** — mesuré, pas supposé
(`grep` sur `docs/adr/`) : [ADR-0020](0020-budget-horaire-dosage-unique.md) et
[ADR-0115](0115-une-garde-sur-etat-partage-vit-au-point-dusage-final.md).
Ils sont **antérieurs au retrait**, et le fond d'un ADR accepté est **immuable**.
⛔ Ne pas les réécrire — c'est cette note qui lève l'ambiguïté.

## Vérification

⭐ Cette règle est **contrôlée mécaniquement**, pas seulement écrite :
`tools/check_no_dynamic_pump_routes.sh`, job CI `no-dynamic-pump-routes`.
Il vérifie que `handleDynamicPumpRoutes` n'est pas réapparue et que le corps de `onNotFound`
ne contient ni `startsWith`, ni `url()`, ni `setManualPump`.
⚠️ **Validé par mutation** : réintroduire `if (req->url().startsWith("/pump1/duty/"))` dans
`onNotFound` fait échouer le script.
