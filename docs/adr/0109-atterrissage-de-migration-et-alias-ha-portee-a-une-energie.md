# ADR-0109 — L'atterrissage de migration privilégie l'état déjà en place ; une rupture visible d'un contrat Home Assistant prime sur un alias silencieux

- **Statut** : Accepté
- **Date** : 2026-10-14
- **Décideurs** : `architect`, `embedded-developer`, `web-ui-developer`, `code-reviewer`
- **Spec(s) liée(s)** : [`feature-162`](../../specs/features/done/feature-162-carte-eclairage-choisir-le-mode.md)

## Contexte

`feature-162` remplace le triplet d'état de l'éclairage (`enabled` + `scheduleEnabled` + un
`manualOverride` **caché**, jamais persisté ni publié) par un champ unique `lightingCfg.mode`
(`on`/`off`/`schedule`), seule autorité — application d'[ADR-0107](0107-etat-visible-et-persistant-plutot-que-suspension-invisible.md)
et d'[ADR-0108](0108-basculer-le-mode-est-une-commande-injecter-est-un-geste.md) à l'éclairage.
Deux décisions de ce lot comportent une alternative crédible et verrouillent un comportement futur,
d'où cet ADR court.

### (a) Vers quel mode atterrit une installation existante ?

Les deux anciens booléens NVS (`light_enabled`, `light_sched_en`) doivent être traduits en un seul
mode au premier démarrage sur ce firmware. Si les deux valaient `true` (programmation active
**et** un forçage manuel resté en RAM au moment du dernier reboot), vers quel mode migrer ?

### (b) Que devient le select Home Assistant « Mode Éclairage » (2 options) ?

Le select existant (`bug-ha-eclairage-select`) n'exposait que Programmation/Désactivé — il ne
pouvait pas représenter le nouveau mode Allumé. Le remplacer par un select à 3 options casse toute
automatisation HA qui enverrait encore l'ancien libellé.

## Décision

**(a) La migration privilégie l'état `light_sched_en`** : si la programmation était activée,
l'installation atterrit en **Programme**, jamais en Allumé, même si un forçage manuel était
présent en RAM à l'instant du dernier reboot. Codifié dans la fonction pure
`migrateLightingMode(lightEnabled, lightScheduleEnabled)` — `light_sched_en` prime
inconditionnellement.

**(b) L'ancien select 2 options est supprimé** (dépublié via charge retenue vide), **pas** aliasé
vers les 3 nouvelles options. Un nouveau select 3 options (`Allumé`/`Éteint`/`Programme`) est publié
à une **nouvelle** entité (`unique_id` différent). Aucune tentative de faire correspondre
« Désactivé » à l'une des trois nouvelles valeurs.

## Alternatives considérées

### (a) Atterrissage de migration

- **Option A (rejetée)** — atterrir en **Éteint** par défaut dans le cas ambigu (programmation
  active + override actif), au nom du principe « fail-safe = éteint ». Rejetée : ce choix **perd**
  silencieusement une programmation que l'utilisateur avait délibérément configurée, pour un état
  transitoire (un forçage manuel) qui **n'a jamais survécu** à un redémarrage sur le firmware
  précédent non plus — l'argument « fail-safe » ne protège ici rien de réel, puisque le firmware
  actuel produit **déjà** ce même atterrissage en Programme à chaque redémarrage dans ce cas.
- **Option B (rejetée)** — demander à l'utilisateur de reconfirmer son mode au premier démarrage
  (bandeau de choix). Rejetée : aucune des trois autres migrations de ce projet (mode filtration,
  mode installation, mode régulation pH/ORP) n'a exigé de confirmation humaine pour une migration
  qui préserve le comportement déjà observable ; en demander une ici créerait une incohérence de
  traitement sans bénéfice de sécurité (pas de dosage chimique en jeu).
- **Option C (retenue)** — `light_sched_en` prime. Elle **reproduit exactement** le comportement
  que le firmware produit déjà à chaque redémarrage dans le seul cas ambigu possible : zéro
  régression, zéro confirmation requise.

### (b) Entité Home Assistant

- **Option A (rejetée)** — garder le select 2 options et lui ajouter un alias ou un mapping
  implicite (« Désactivé » → « Éteint »). Rejetée : décider **en silence** que « Désactivé »
  signifie désormais « Éteint » est un **changement de sens invisible** — une automatisation HA qui
  envoyait « Désactivé » pour couper la lumière continuerait de fonctionner, mais son **sens**
  aurait changé sans qu'aucun signal ne le dise. C'est précisément le défaut que ce lot corrige
  côté firmware (un état qui change de comportement sans que l'affichage le signale) — le
  reproduire côté Home Assistant serait incohérent avec le motif du lot.
- **Option B (retenue)** — suppression explicite de l'ancienne entité (dépubliée), nouvelle entité
  à `unique_id` distinct. **Rupture visible** : une automatisation qui envoie l'ancien libellé
  échoue de façon audible (entité disparue de HA), plutôt que de continuer à fonctionner avec un
  sens silencieusement différent.

## Conséquences

### Positives
- Aucune régression de comportement observable sur la migration NVS — le cas ambigu produit
  exactement ce que le firmware actuel produit déjà.
- Aucune automatisation Home Assistant ne peut se retrouver à agir sur un sens qu'elle ne connaît
  pas : soit elle utilise le protocole stable (jetons bruts `on`/`off`/`schedule`, inchangés), soit
  elle échoue visiblement et se corrige.

### Négatives / dette assumée
- Une automatisation HA qui pilotait l'ancien select par libellé français doit être **mise à
  jour manuellement** — rupture assumée, communiquée dans `docs/MQTT.md` et le CHANGELOG.
- Le critère « une installation dont l'override était actif se retrouve en Allumé/Éteint, jamais
  en Programme » ne peut être garanti qu'**à partir du premier démarrage sur ce nouveau
  firmware** — pas pendant la migration elle-même, qui ne voit que les deux booléens déjà
  persistés (documenté dans `docs/subsystems/lighting.md`, pas tu).

### Ce que ça verrouille
- Toute future migration d'un état multi-booléen vers un enum unique doit choisir l'atterrissage
  qui **reproduit le comportement déjà observable aujourd'hui**, pas un repli « fail-safe » abstrait
  qui ignorerait ce que le firmware fait déjà dans le cas ambigu.
- Toute suppression d'une entité Home Assistant dont le **sens** change doit être une suppression
  explicite (nouvelle entité, ancienne dépubliée) — jamais un alias qui ferait porter un sens
  différent à un libellé ou un payload déjà en usage.

## Références

- Code : [`src/schedule_logic.h`](../../src/schedule_logic.h) / [`.cpp`](../../src/schedule_logic.cpp)
  (`migrateLightingMode`, `LightingMode`) · [`src/config.cpp`](../../src/config.cpp) (migration
  one-shot NVS) · [`src/mqtt_manager.cpp`](../../src/mqtt_manager.cpp) (dépublication de l'ancien
  select, nouveau select 3 options)
- Spec : [`specs/features/done/feature-162-carte-eclairage-choisir-le-mode.md`](../../specs/features/done/feature-162-carte-eclairage-choisir-le-mode.md)
- Doc externe : [`docs/subsystems/lighting.md`](../subsystems/lighting.md) ·
  [`docs/MQTT.md`](../MQTT.md)
