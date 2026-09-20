# ADR-0084 — Ce qui prouve qu'un réseau est vivant, et ce qui autorise à redémarrer

- **Statut** : Accepté
- **Date** : 2026-09-20
- **Décideurs** : Nicolas Philippe (architect), pool-chemistry (gate C1/C2 sur le budget de reboot partagé avec le dosage), embedded-developer
- **Spec(s) liée(s)** : bug-134 (watchdog connectivité ne réarme pas sur reconnexion réussie) · amende [ADR-0030](0030-watchdog-connectivite.md)

## Contexte

Le 2026-09-19 à 15:46:37, le contrôleur a redémarré alors que le réseau
fonctionnait : une adresse IP DHCP avait été obtenue par le WiFi trois
minutes plus tôt, preuve de vie L2+L3 irréfutable, mais **rien** dans le
firmware ne l'utilisait pour réarmer l'horloge de stall du watchdog de
connectivité ([ADR-0030](0030-watchdog-connectivite.md)). Deux défauts
distincts concouraient :

1. `ARDUINO_EVENT_WIFI_STA_GOT_IP` n'appelait jamais de fonction de
   réarmement.
2. `ensureNetworkProbe()` ne recréait la session ICMP que si l'adresse de la
   passerelle **changeait** — jamais quand la session était morte avec la
   même passerelle.

Le correctif évident — réarmer sur **toute** preuve de vie, y compris celles
que la séquence de récupération produit elle-même (IP obtenue après un soft
recover, sonde ICMP relancée) — a été écarté après avoir **démontré**, pas
supposé, qu'il ouvrait deux boucles infinies qui rendent le watchdog
définitivement inopérant :

- **Boucle A (`GOT_IP`)** : `softRecoverAttempted` repasse à `false` dès que
  `msSinceNetOk < kNetworkStallSoftMs` (2 min). Sans plafond : soft recover →
  `GOT_IP` → réarme → moins de 2 min avant le prochain palier → soft
  recover… **jamais de reboot**, quelle que soit la gravité réelle du gel.
- **Boucle B (`ProbeStarted`)** : l'ancien réarmement inconditionnel au
  (re)démarrage de la sonde, couplé à sa recréation sur silence
  (`kNetworkProbeSilenceMs = 90 s`), donne — passerelle réellement morte,
  WiFi toujours associé — silence 90 s → recréation → réarmement →
  silence… **le compteur de stall ne dépasse jamais 90 s**. Même piège
  qu'A, trouvée par la revue `pool-chemistry`, une autre porte.

L'enjeu qui rend cet arbitrage important n'est **pas** le réseau lui-même :
`DosingState` ([`src/pump_controller.h:20-32`](../../src/pump_controller.h))
est **entièrement en RAM** (`usedMs`, `windowStart`, `lastStartTime`,
`cyclesToday`) — seuls les millilitres **journaliers** sont persistés en
NVS. Tout reboot remet à zéro le budget horaire de dosage, l'anti-cycling et
la pause de mélange. À une limite de 10 min/h sur le pH, deux reboots dans
la même heure autorisent jusqu'à 20 minutes d'acide avant que le seul
plafond journalier ne fasse barrage. **Retarder un reboot n'assouplit
rien** (les pompes sont coupées avant tout reboot fail-safe, condition C1
[ADR-0029](0029-auto-guerison-heap.md)) ; **en déclencher un à tort
assouplit une garde de dosage réelle.** C'est ce raisonnement, plus que la
gêne de l'utilisateur au redémarrage lui-même, qui a fait pencher la
balance vers un mécanisme de confiance gradué plutôt qu'un réarmement plat.

## Décision

**Distinguer deux classes de preuve de vie réseau, et leur appliquer un
traitement différent :**

- **Preuves EXTERNES** (`IcmpEcho` — écho ICMP passerelle ; `AppTraffic` —
  HTTP servi, WebSocket, ACK MQTT, resync NTP) : réarment l'horloge **sans
  limite** et **remettent le crédit de réarmements auto-produits à 0**.
  Elles proviennent d'un tiers (la passerelle, un client, un broker) que la
  récupération ne contrôle pas — leur confiance est totale.
- **Preuves AUTO-PRODUITES par la récupération elle-même** (`IpAcquired` —
  IP obtenue par DHCP ; `ProbeStarted` — sonde ICMP (re)démarrée) : réarment
  **seulement si un crédit n'est pas épuisé** (`kNetworkSelfRearmMax = 2`),
  puis le consomment. Crédit épuisé → **aucun réarmement** : l'horloge
  poursuit sa course vers le reboot, qui reste alors inévitable.

Implémentation : `evaluateNetRearm()` (`src/net_health_logic.{h,cpp}`,
logique pure, [ADR-0017](0017-logique-metier-pure-humble-object-testabilite.md)),
appelée exclusivement via la porte unique `notifyNetworkLifeSign()`
(`src/main.cpp`). Détail complet des six sources de réarmement et de leur
classe : [`docs/subsystems/system-health.md`](../subsystems/system-health.md#réarmement-plafonné-à-crédit--six-sources-et-leur-classe-bug-134-v2531).

En complément, la sonde ICMP est désormais recréée **sur son propre
silence** (`kNetworkProbeSilenceMs = 90 000` ms, trois échos manqués),
indépendamment d'un éventuel rebond WiFi — ce qui ferme le trou du défaut
n°2 même quand la passerelle ne change jamais. Cette recréation forcée
passe par `ProbeStarted`, donc par le même crédit plafonné.

**Règle à retenir** : *un signe de vie que la récupération fabrique
elle-même ne peut pas financer indéfiniment son propre report.*

## Alternatives considérées

- **Option A (rejetée) — Statu quo : aucun réarmement sur `GOT_IP` ni
  recréation de sonde sur silence.** C'est le défaut corrigé : preuves de
  vie réelles ignorées, reboot déclenché alors que le réseau fonctionnait.
- **Option B (rejetée) — Réarmement inconditionnel sur toute preuve de vie,
  y compris auto-produite.** La correction « naïve ». **Démontrée pire que
  le défaut d'origine** : ouvre les boucles A et B ci-dessus, qui rendent le
  watchdog **définitivement** inopérant — un vrai gel de la pile lwIP avec
  passerelle injoignable ne reboote plus jamais.
- **Option C (rejetée) — Quorum de deux signaux avant réarmement.** Exiger
  par exemple qu'un écho ICMP **et** un trafic applicatif coïncident.
  Rejetée : la nuit, il n'y a fréquemment **ni** client HTTP/WS actif **ni**
  MQTT (désactivable dans la configuration) — un quorum ferait rebooter un
  appareil dont le seul tort est d'être sans spectateur, exactement le faux
  positif que [ADR-0030](0030-watchdog-connectivite.md) cherchait à éviter
  en rendant le signal indépendant de MQTT.
- **Option D (rejetée) — A1/A2 nus, sans plafond ni crédit.** C'est l'option
  B reformulée à l'échelle du correctif proposé initialement par la spec ;
  écartée pour le même motif (boucles A et B).
- **Option E (rejetée) — Reboot inconditionnel après N soft recovers,
  indépendamment des preuves de vie ultérieures.** Plus simple, mais perd la
  capacité de reconnaître une récupération **réellement** réussie (WiFi qui
  se rétablit pour de bon) : un appareil qui enchaîne des coupures WiFi
  légitimes et brèves rebooterait à chaque fois au bout de N épisodes, même
  quand chacun se résout correctement.
- **Option F (rejetée) — Recréation périodique aveugle de la sonde,
  indépendante de tout silence constaté.** Masquerait l'observabilité (A3) :
  sans mesurer le silence réel, impossible de distinguer une sonde
  fonctionnelle d'une sonde qu'on recrée par précaution — et le coût (fuite
  de session si mal enchaîné) n'a pas de contrepartie.
- **Option G (retenue) — Classification externe/auto-produite + crédit
  plafonné + recréation sur silence propre de la sonde.** Seule option qui
  ferme les deux boucles démontrées tout en préservant la réactivité à une
  vraie récupération (crédit de 2, pas 0) et en rendant la sonde
  observable et auto-réparable sur son propre silence.

## Conséquences

### Positives

- **Fin du faux redémarrage de type 19/09** : une reconnexion WiFi qui
  obtient réellement une IP, ou une sonde relancée avec succès, réarme
  désormais l'horloge — dans les limites du crédit.
- **Les deux boucles infinies restent fermées** : validé par mutation
  (retirer le plafond sur `ProbeStarted`, ou réarmer `GOT_IP` sans passer
  par le crédit, tue un test natif — voir
  [`docs/subsystems/system-health.md`](../subsystems/system-health.md)).
- **Observabilité neuve** (A3) : `/get-system-info` publie désormais
  `icmpEchoes`, `msSinceEcho`, `msSinceNetOk`, `selfRearms`, `probeAlive` —
  purement additif, sans contrat externe verrouillé (aucun topic MQTT ni
  champ WebSocket).
- **Sécurité chimique préservée** : aucune modification de
  `pump_controller` ; le raisonnement qui a motivé le gate porte sur la
  **fréquence** des reboots (donc des remises à zéro de `DosingState`), pas
  sur la séquence fail-safe elle-même, inchangée.

### Négatives / dette assumée

- **Le critère « passerelle injoignable ⇒ reboot toujours » est plus faible
  qu'il n'y paraît en présence d'un client actif.** Une requête HTTP `/data`
  servie, ou un ACK MQTT, est une preuve **externe** : elle réarme **sans
  limite**. Si un client continue de solliciter l'appareil pendant que la
  passerelle est injoignable (cas rare mais possible : requêtes locales sur
  le même segment L2, sans routage via la passerelle), le reboot peut être
  différé indéfiniment. **Ce n'est pas une régression** — c'est déjà vrai
  sur HEAD avant ce lot, `AppTraffic` réarmant sans limite depuis
  [ADR-0030](0030-watchdog-connectivite.md) — mais la garantie ne doit pas
  être crue plus forte qu'elle ne l'est.
- **`DosingState` reste volatile.** Ce correctif réduit la **fréquence** des
  reboots inutiles, il ne supprime pas l'exposition résiduelle : deux
  reboots légitimes rapprochés (heap **et** réseau, budget partagé) remettent
  toujours à zéro le budget horaire de dosage. Persister `usedMs` et la
  fenêtre horaire est un sujet **hors périmètre**, à ouvrir en spec de
  suivi.
- **Un plafond de 2 est un compromis, pas une preuve formelle d'absence de
  boucle résiduelle.** Il a été choisi pour laisser un crédit court à une
  récupération légitime sans permettre un report indéfini ; toute preuve
  auto-produite future devra passer par la même porte, sous peine de rouvrir
  une boucle par un chemin non couvert par ce plafond.

### Ce que ça verrouille

- **Toute nouvelle source de réarmement doit se déclarer explicitement
  externe ou auto-produite** dans `NetLifeSign` (`src/net_health_logic.h`)
  avant d'appeler `notifyNetworkLifeSign()`. Un ajout qui contournerait la
  porte unique (écriture directe de `lastNetworkOkMs`) romprait
  silencieusement la garantie anti-boucle.
- **Une preuve auto-produite ne remet jamais le crédit à 0** — seule une
  preuve externe le fait. Ne pas inverser cette règle sans rouvrir
  explicitement le raisonnement des boucles A/B.
- **`kNetworkSelfRearmMax` est un budget de confiance, pas un simple
  réglage de cadence** : le modifier sans re-vérifier les deux boucles par
  mutation reproduirait l'erreur que ce lot corrige.
- La décision fondatrice d'[ADR-0030](0030-watchdog-connectivite.md) — sonde
  ICMP primaire indépendante de MQTT, réponse graduée soft→hard, budget de
  reboot partagé heap+réseau — **reste inchangée et Acceptée**. Cet ADR
  l'amende uniquement sur la question « quelles preuves de vie comptent, et
  combien de fois une preuve auto-produite peut-elle compter ».

## Références

- Code : [`src/net_health_logic.h`](../../src/net_health_logic.h) /
  [`src/net_health_logic.cpp`](../../src/net_health_logic.cpp)
  (`NetLifeSign`, `NetRearmInputs`, `NetRearmOutcome`, `evaluateNetRearm()`,
  `netProbeSeemsDead()`), [`src/main.cpp`](../../src/main.cpp)
  (`notifyNetworkLifeSign()`, `notifyNetworkActivity()`,
  `ARDUINO_EVENT_WIFI_STA_GOT_IP`, `ensureNetworkProbe()`,
  `onNetPingSuccess()`), [`src/constants.h`](../../src/constants.h)
  (`kNetworkSelfRearmMax`, `kNetworkProbeSilenceMs`),
  [`src/web_routes_config.cpp`](../../src/web_routes_config.cpp) (bloc `net`
  de `/get-system-info`)
- Tests : `test/test_native_net_health/test_net_health.cpp` — 42 cas mesurés
  le 2026-09-20 (25 préexistants + 17 pour ce lot, ADR-0073)
- Spec : `specs/features/doing/bug-134-watchdog-connectivite-ne-rearme-pas-sur-reconnexion-reussie.md`
- Docs : [`docs/subsystems/system-health.md`](../subsystems/system-health.md#watchdog-de-connectivité-v292),
  [`docs/API.md`](../API.md#get-get-system-info--write), [MQTT.md](../MQTT.md)
- Antécédent : [ADR-0030](0030-watchdog-connectivite.md) (watchdog de
  connectivité — décision fondatrice, amendée ici, non supersédée)
