# bug-119 — `loadSensorData()` ne résout rien : trois chemins morts qui ont l'air vivants

## Métadonnées
| | |
|---|---|
| Type | bug (frontend — code mort **syntaxiquement correct**) |
| Statut | todo |
| Priorité | Modérée — mais l'un des trois chemins morts est un **garde-fou de dosage** |
| Version cible | à définir |
| Relevé par | agent frontend, 2026-09-07, pendant le correctif de `bug-082` ; confirmé par lecture directe |
| Specs liées | `bug-082` (l'autre mort du même garde-fou), `feature-075` (l'assistant de débit) |

## Domaines touchés
- [x] Frontend (`data/app.js`) — `loadSensorData()` et ses appelants
- [ ] Régulation — [ ] Capteurs — [ ] Web — [ ] MQTT — [ ] OTA

## Agents requis
- [x] architect · [x] web-ui-developer · [x] code-reviewer · [x] doc-writer
- [x] **`pool-chemistry`** — l'un des chemins morts est le garde-fou « mesure de débit
      interrompue ». Le visa doit dire si sa résurrection change quelque chose au dosage.

## Le défaut
`loadSensorData()` est `async` et ses appelants en attendent une valeur. Mais **il
n'existe aucun `return json` dans tout `data/app.js`** (vérifié par grep) : l'IIFE
interne du chemin réseau ne retourne rien, donc la promesse résout `undefined`.

Conséquence à `data/app.js:9505` :
```js
const d = await loadSensorData({ force: true, source: "flow-wizard" });
fa = d && d.flow_assist;          // → fa est TOUJOURS null
```

**Trois comportements sont donc morts** dans `verifyRunEnd()` :
1. la détection de `fa.aborted` — le **garde-fou** « la mesure de débit a été
   interrompue par le serveur » ;
2. la reprise du chronomètre sur la phase serveur (`fa.active && fa.phase === phase`) ;
3. le recalage d'horloge qui en dépend.

## Pourquoi c'est insidieux
Le code est **syntaxiquement correct**, il n'échoue **jamais**, il ne journalise rien.
Le `try/catch` qui l'entoure attrape une exception qui n'arrive pas. Vu de l'extérieur,
la fonction paraît complète et défensive — elle ne fait simplement rien.

C'est la même famille que les défauts du 2026-09-07 : **un garde-fou visible fait
passer le relecteur**. Ici il n'est pas mal placé, il est **inerte**.

## Lien avec `bug-082` — le même garde-fou avait DEUX morts
`bug-082` a établi que `aborted` était neutralisé parce que la trame WebSocket écrasait
`latestSensorData` avant la saisie de l'utilisateur. Son correctif (parité de `flow255`
et `aborted`) ressuscite ce chemin-là : `saveMeasuredFlow()` lit `latestSensorData`, que
le rafraîchissement forcé met à jour **par effet de bord**.

Mais la lecture par **valeur de retour**, dans `verifyRunEnd()`, reste morte. Les deux
morts étaient indépendantes, et une seule est corrigée.

## Pourquoi le correctif n'a pas été fait dans `bug-082`
Ajouter `return json;` **change le contrat de tous les appelants** et réactive un chemin
qui n'a **jamais été éprouvé** — la reprise de chronomètre sur phase serveur n'a
probablement jamais tourné. C'est un lot à part entière, avec son propre risque de
régression, pas un ajout opportuniste en fin de cycle. Décision de l'agent frontend,
validée.

## Comportement attendu
À trancher par l'`architect` :
1. `return json;` (et `return latestSensorData;` sur le chemin WS) — le contrat que les
   appelants supposent déjà. Puis **éprouver** les trois chemins ressuscités, dont deux
   n'ont probablement jamais tourné.
2. Ou supprimer l'attente de valeur chez les appelants et lire `latestSensorData`
   explicitement — plus honnête sur ce que la fonction fait vraiment, mais laisse
   `verifyRunEnd` sans sa reprise de chronomètre.

Le choix n'est pas cosmétique : l'option 1 **active du code jamais exécuté** sur un
chemin qui touche une mesure de débit servant ensuite au dosage.

## Critères d'acceptation
- [ ] Les appelants qui attendent une valeur en reçoivent une, ou cessent d'en attendre
- [ ] Les trois comportements de `verifyRunEnd()` sont **éprouvés**, pas seulement
      rebranchés — une simulation par cas
- [ ] Le garde-fou `aborted` refuse bien la saisie sur une mesure interrompue, par les
      **deux** chemins (valeur de retour **et** `latestSensorData`)
- [ ] Aucune régression sur les autres appelants de `loadSensorData` (`:6656`, `:9717`)
- [ ] Visa `pool-chemistry` sur la résurrection du garde-fou

## Notes pour agent
Chercher d'autres `await` dont la valeur est utilisée alors que la fonction appelée ne
retourne rien : ce défaut est **invisible en lecture locale** et ne se voit qu'en
vérifiant le contrat de retour à la source. Un grep de `= await ` croisé avec l'absence
de `return` dans les fonctions visées est la méthode.
