## Métadonnées
- **ID** : feature-166
- **Slug** : veille-des-ancres-de-confiance-tls
- **Type** : feature
- **Statut** : todo
- **Priorité** : **haute** — ⚠️ la chaîne a changé **deux fois en deux mois**, et la prochaine migration est **déjà visible**
- **Version cible** : à déterminer (base **2.81.0**)
- **Dernière mise à jour** : 2026-10-15
- **Spec(s) liée(s)** : [`bug-165`](../done/bug-165-recherche-de-mise-a-jour-en-ligne-echoue.md) · [`bug-108`](../done/bug-108-garde-tls-ota-sur-metrique-ira-inclusive.md) · [ADR-0111](../../../docs/adr/0111-ancre-de-confiance-tls-toujours-auto-signee.md) · [ADR-0112](../../../docs/adr/0112-mbedtls-obeit-a-la-politique-psram-du-projet.md)

## Domaines touchés
- [x] OTA
- [x] Web — routes HTTP / WebSocket *(si une alerte est exposée)*

## Agents requis
- [x] spec-reader · [x] architect
- [x] **security-reviewer** — ⚠️ OBLIGATOIRE : le lot touche la **racine de confiance** de la mise à jour
- [x] embedded-developer *(si volet embarqué retenu)* · [x] test-engineer · [x] code-reviewer · [x] doc-writer
- [ ] pool-chemistry — aucun dosage

## Description
Détecter **qu'une migration de chaîne de certificats va casser la mise à jour**, ⚠️ **AVANT** que
l'appareil ne tombe — ⛔ pas après.

## Contexte — ⚠️ deux pannes en deux mois, et la troisième est annoncée

| Date | Événement | Conséquence |
|---|---|---|
| **2026-08-08** | changement de plateforme (mbedtls 2.28 → 3.6.5) | mise à jour **cassée** |
| **2026-10-14** | GitHub migre vers **Sectigo Root E46** | mise à jour **re-cassée** |
| **à venir** | Let's Encrypt retire la signature croisée de **« Root YR »** | 🚨 **téléchargement cassé** |

⚠️ **La troisième est DÉJÀ OBSERVABLE** : `objects.githubusercontent.com` sert aujourd'hui
`*.github.io ← YR1 ← **Root YR (CROISÉE par ISRG Root X1)**`. Elle ne fonctionne **que** parce que
la signature croisée existe encore. ⛔ **Le jour où elle disparaît, le téléchargement tombe** — et
cette date **ne dépend pas de nous**.

⚠️ **Les expirations ne sont PAS le risque principal** (mesuré le 2026-10-15) : ISRG Root X1 en
**2035**, USERTrust en **2038**, Sectigo en **2046**. ⇒ **Les migrations arrivent 10 ans avant les
expirations.**

### 🚨 Ce que le contrôle actuel NE fait PAS
`tools/check_github_root_ca.sh` vérifie que les certificats sont **bien formés, auto-signés,
présents et alignés sur leurs empreintes**. ⛔ **Il ne vérifie PAS qu'ils servent encore à quelque
chose.** ⇒ Un paquet parfaitement valide et devenu **inutile** passe le contrôle.

## Comportement attendu

### Volet 1 — ⚠️ LE CŒUR : vérifier contre la RÉALITÉ, en intégration continue
Un contrôle qui, **à chaque push**, depuis une machine connectée :
1. récupère **réellement** les chaînes servies par **`api.github.com`** ET
   **`objects.githubusercontent.com`** ;
2. vérifie que les ancres **embarquées** les valident, ⚠️ **en ISOLANT le magasin du système**.
   🚨 **C'EST LE PIÈGE QUI A COÛTÉ UNE SOIRÉE** : `openssl s_client -CAfile <paquet>` consulte
   **aussi** le magasin de la machine et conclut « ✔ » **à tort**. ⛔ Utiliser
   `openssl verify -no-CAfile -no-CApath -CAfile <paquet> -untrusted <intermédiaires>`.
3. **échoue** si une chaîne ne se valide plus.

⚠️ **Il échouera un jour, et c'est le but** — ⛔ ce n'est **pas** un contrôle qui doit rester vert
pour toujours. Le rédiger pour qu'un futur lecteur **comprenne qu'un échec est une INFORMATION**,
⛔ pas une panne d'outil.

### Volet 2 — alerte d'expiration **relative**
Échouer si une ancre expire dans **moins de 2 ans**. ⛔ Jamais une date en dur — ⚠️ une date figée
périme, c'est le motif d'ADR-0073.

### Volet 3 (à arbitrer) — essai périodique **côté appareil**
Un `/check-update` déclenché seul, à faible fréquence, qui **journalise** son échec.
⚠️ **Sans lui, la panne n'est visible que le jour où l'utilisateur clique.** ⛔ Mais c'est un
chemin réseau périodique : **à instruire**, ⛔ pas à ajouter sans réflexion.

### Cas limites
- ⚠️ **Pas de réseau en CI** ⇒ le contrôle doit **distinguer** « chaîne cassée » d'« injoignable ».
  ⛔ Un échec réseau qui se présente comme une migration est pire que rien.
- GitHub sert **des chaînes différentes selon le client** (ECDSA vs RSA — vérifié le 2026-10-14) :
  ⚠️ tester **les deux**.
- Un point de présence CDN peut répondre autrement : ⛔ un échec isolé n'est pas une preuve.

## Comportement actuel
⛔ **Rien.** Aucune surveillance, aucune alerte d'expiration. Une migration se découvre **le jour
où la mise à jour échoue** — et, avant bug-165, **sans même dire pourquoi**.

## Contraintes techniques
- ⛔ **Aucune régression de la voie de secours** (`POST /update`, `./deploy.sh ota-firmware`).
- ⚠️ Volet 3 : ⛔ aucune allocation sur un chemin périodique (ADR-0082), watchdog respecté.
- Le contrôle CI ne doit **pas** devenir instable : ⚠️ un contrôle qui échoue au hasard **sera
  désactivé**, et on reviendra au point de départ.

## Fichiers potentiellement concernés
`tools/check_github_root_ca.sh`, `tools/github_root_ca_vectors.py`,
`.github/workflows/hygiene-specs.yml`, `src/web_routes_ota.cpp` (volet 3),
`docs/subsystems/ota-manager.md`.

## ADR attendu ?
**Probablement non** — ce lot **applique** ADR-0111. ⚠️ **Sauf** si le volet 3 est retenu : un
appel réseau périodique vers l'extérieur est une décision structurante pour un appareil
« **offline first** ». ⇒ **Trancher**, et écrire la justification si aucun ADR n'est créé.

## Critères d'acceptation
- [ ] Le contrôle valide les ancres embarquées contre les chaînes **réellement servies**, ⚠️ **magasin système ISOLÉ**.
- [ ] Les **deux** domaines sont couverts — ⚠️ y compris celui du **téléchargement**.
- [ ] Les **deux** familles de chaînes (ECDSA **et** RSA) sont testées.
- [ ] Une ancre expirant dans **moins de 2 ans** fait échouer — ⛔ seuil **relatif**.
- [ ] ⚠️ **Une panne réseau ne se présente PAS comme une migration.**
- [ ] **Validation par mutation** : retirer une ancre nécessaire ⇒ le contrôle **meurt**.
- [ ] Le volet 3 est **tranché par écrit** — retenu ou écarté, avec son motif.

## Plan de test manuel
1. Retirer Sectigo E46 du paquet ⇒ le contrôle doit **échouer** en nommant la chaîne.
2. Couper le réseau ⇒ message **« injoignable »**, ⛔ pas « migration ».
3. Avancer artificiellement l'horizon à 20 ans ⇒ **toutes** les ancres doivent être signalées.

## Notes
⚠️ **Le motif qui justifie ce lot**, à écrire dans le contrôle lui-même :
> *Un paquet de certificats peut être parfaitement valide — bien formé, auto-signé, empreintes
> exactes — et totalement **inutile**. Le seul test qui compte est : **valide-t-il encore ce que
> le serveur envoie aujourd'hui ?***
