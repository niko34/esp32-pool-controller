#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <stddef.h>  // size_t (kDoseBlockedReasonMaxLen) — self-contained
#include <stdint.h>
#include <time.h>  // time_t (kMinValidEpoch) — self-contained : ne dépend plus d'Arduino.h en amont

// =============================================================================
// GPIO PIN ASSIGNMENTS - PCB v2
// =============================================================================
// Mapping matériel pour le circuit imprimé v2. Voir feature-019 et
// docs/adr/0012-pcb-v2-gpio-mapping.md.
// Bascule unidirectionnelle : ce code n'est plus compatible avec le PCB v1.

// Pins actifs
constexpr uint8_t kBuiltinLedPin           = 2;   // LED bleue interne (status)
constexpr uint8_t kTempSensorPin           = 5;   // OneWire DS18B20 (eau + circuit, 2 sondes)
constexpr uint8_t kI2cSdaPin               = 21;  // I2C SDA — DS3231 + EZO pH + EZO ORP (default Arduino-ESP32)
constexpr uint8_t kI2cSclPin               = 22;  // I2C SCL — DS3231 + EZO pH + EZO ORP (default Arduino-ESP32)
constexpr uint8_t kPumpPhPin               = 25;  // Pompe doseuse pH (PWM via ledc)
constexpr uint8_t kFiltrationRelayPin      = 26;  // Relais filtration (actif haut)
constexpr uint8_t kLightingRelayPin        = 27;  // Relais éclairage (actif haut)
constexpr uint8_t kPumpOrpPin              = 33;  // Pompe doseuse ORP/chlore (PWM via ledc)
constexpr uint8_t kFactoryResetButtonPin   = 35;  // Bouton factory reset (input-only, pull-up externe 10kΩ — actif bas)

// Pins réservés feature-future — déclarés mais pas de pinMode() actif dans cette feature
constexpr uint8_t kRtcSqwPin               = 23;  // RTC DS3231 SQW (open-drain, INPUT_PULLUP attendu, future feature)
constexpr uint8_t kCtnAuxPin               = 32;  // CTN_AUX MOSFET 12V tableau (OUTPUT actif haut, future feature)
constexpr uint8_t kRtcIntPin               = 36;  // RTC DS3231 INT (input-only, pull-up externe 10kΩ, future feature)

// ============================================================================
// TIMING CONSTANTS - Intervalles et délais temporels
// ============================================================================

// Delays système
constexpr unsigned long kSerialInitDelayMs = 3000;        // Attente après Serial.begin()
constexpr unsigned long kLoopDelayMs = 10;                // Délai minimal entre cycles loop()
constexpr unsigned long kOtaYieldDelayMs = 1;             // Délai yield() pendant OTA

// Watchdog
constexpr unsigned long kWatchdogTimeoutSec = 30;         // Timeout watchdog en secondes

// Intervalles de mise à jour
constexpr unsigned long kMqttPublishIntervalMs = 10000;   // 10s - Publication état MQTT
constexpr unsigned long kHealthCheckIntervalMs = 60000;   // 60s - Vérification santé système
constexpr unsigned long kDiagnosticPublishIntervalMs = 300000; // 5min - Publication diagnostic MQTT

// Tâche dédiée MQTT (cf. ADR-0011) — isole les blocages réseau (TCP retransmits, DNS lwip)
// de la régulation pH/ORP et de la filtration. Voir docs/subsystems/mqtt-manager.md.
constexpr uint32_t kMqttTaskStackSize       = 8192;       // 8 KB - publishDiscovery() sérialise 17 JSON consécutifs
constexpr uint32_t kMqttTaskPriority        = 2;          // Bas, > IDLE, < tiT (lwip) et async_tcp
constexpr int      kMqttTaskCore            = 0;          // Core 0 (loopTask sur core 1) — répartit la charge réseau
constexpr uint32_t kMqttOutQueueLength      = 32;         // File sortante (publish) — ~3s de débit nominal
constexpr uint32_t kMqttInQueueLength       = 16;         // File entrante (commandes HA)
constexpr uint32_t kMqttTaskLoopTimeoutMs   = 100;        // Timeout xQueueReceive dans mqttTask (cadence mqtt.loop())
constexpr uint32_t kMqttOfflineFlushMs      = 1000;       // Timeout flush "status=offline" avant ESP.restart() (OTA)
constexpr uint32_t kMqttClientConnectTimeoutSec = 2;      // WiFiClient::setTimeout attend des SECONDES (Arduino-ESP32 6.9.0 — WiFiClient.cpp:327, _timeout = seconds*1000). 2 s borne SO_SNDTIMEO/SO_RCVTIMEO sur le client TCP de PubSubClient.
constexpr uint32_t kMqttSocketSendTimeoutMs = 500;        // SO_SNDTIMEO socket TCP — borne write() à 500 ms (PINGREQ ~100 ms suffit, publish massif borné). Voir feature-014 IT5 / ADR-0011.
// feature-110 : taille du buffer de publication PubSubClient. Était EN DUR à
// 1024 dans MqttManager::begin() — contraire à la convention k* et, surtout,
// invisible depuis les sites qui allongent le payload de diagnostic.
// POURQUOI C'EST UN SUJET DE SÉCURITÉ D'OBSERVABILITÉ : PubSubClient::publish
// REFUSE (return false, sans rien émettre) dès que
//   bufferSize < MQTT_MAX_HEADER_SIZE(5) + 2 + longueur(topic) + longueur(payload)
// (PubSubClient.cpp:450). Un diagnostic trop long n'est donc pas tronqué : il
// DISPARAÎT EN ENTIER — hdef/ldef/xdef de bug-100 compris, soit l'observabilité
// de famine mémoire, et cela dans le silence complet puisque le retour était
// ignoré au site d'appel.
// CHIFFRES MESURÉS (et non estimés) sur le littéral réel de
// publishDiagnosticInternal(), ArduinoJson 7, harnais hôte :
//                     payload   +topic+en-tête = buffer requis
//   avant f-110  nominal  681 o        711 o
//   avant f-110  PIRE     804 o        875 o
//   après f-110  nominal  824 o        854 o
//   après f-110  PIRE     986 o       1057 o  ← DÉPASSE 1024 de 33 o
// Les 8 clés de feature-110 pèsent +182 o au pire cas (mesuré clé par clé).
// Pire cas = SSID 32 c., filtration_mode 16 c., topic de base 64 c. (aucun de
// ces trois n'est borné par /save-config), uint32 saturés, flottants à 9
// chiffres significatifs. 1536 rétablit 479 o de marge (31 %). Coût RAM : +512 o
// dans le buffer PubSubClient, alloués UNE fois dans begin(), jamais en boucle.
constexpr uint16_t kMqttPublishBufferBytes = 1536;
// Surcoût de trame MQTT au-dessus du payload, tel que PubSubClient le comptabilise :
// MQTT_MAX_HEADER_SIZE (5 o, en-tête bâti en tête de buffer) + 2 o de longueur de
// topic. Le topic lui-même s'ajoute à part (longueur dynamique).
constexpr uint16_t kMqttPublishFrameOverheadBytes = 7;
// Throttle des WARN de longueur/échec de publication du diagnostic. Le
// diagnostic sort toutes les 5 min (kDiagnosticPublishIntervalMs) : sans
// throttle, une trame durablement trop longue produirait 288 lignes par jour et
// noierait le journal qu'elle est censée rendre lisible. 1 h = 12 publications
// couvertes par ligne.
constexpr uint32_t kMqttDiagnosticWarnThrottleMs = 3600000;

// feature-109 : synthèse de la découverte Home Assistant en UNE ligne de journal
// (voir src/discovery_report_logic.h). Les 54 lignes INFO précédentes pesaient
// ≈ 5,3 Ko, soit 65 % du fichier de journal de 8 Ko (kMaxLogFileBytes, ADR-0033).
// Topic de configuration le plus long mesuré : "homeassistant/" (14) +
// "binary_sensor/poolcontroller_orp_sensor_problem/config" (53) = 67 caractères.
// On n'en garde que la QUEUE (47 caractères + NUL) : le préfixe de classe est du
// remplissage, le discriminant est en fin de chaîne. 48 o couvrent donc
// "sensor/poolcontroller_orp_sensor_problem/config" en entier, pire cas du parc.
constexpr size_t kDiscoveryFailTopicMax   = 48;   // Topic du 1er échec mémorisé (queue, NUL compris)
// Pire cas ABSOLU de la ligne de synthèse, mesuré en saturant tous les champs
// (burstSeq = 2^32-1, compteurs = 2^16-1, topic plein) : 154 octets UTF-8 pour
// la forme « interrompue », 152 pour la forme « échecs épars ». 160 o couvrent
// donc la troncature impossible, sans peser sur la pile de mqttTask
// (kMqttTaskStackSize) : le buffer de synthèse et celui du détail DEBUG ne
// coexistent jamais (scopes disjoints).
// bug-121 : la ligne porte désormais aussi le MOTIF (jusqu'à « rafale
// précédente incomplète », 31 o UTF-8) et le COÛT mesuré (octets offerts +
// plancher heap 8 bits avant/après). ⚠️ RECALCULÉ, pas recopié (ADR-0073
// s'applique à ce chiffre comme au compteur de suites de tests) — et recalculé
// une SECONDE fois en revue de code, qui avait elle-même signalé une première
// version fausse (224). Piège de saturation à éviter : `attempted`,
// `published`, `failed` et `firstFailIndex` sont des `uint16_t` (5 chiffres
// max, PAS 10 — les saturer en uint32 comme le reste de la ligne surestime le
// pire cas). `published + failed == attempted` est un invariant réel de
// discoveryTallyRecord() (chaque entité tentée est comptée dans l'un OU
// l'autre, jamais les deux) — mais cet invariant NE DÉPARTAGE PAS les deux
// formes : un état « épars » qui le respecte atteint déjà 236 o
// (published=55535, failed=10000, firstFailIndex=10000), et un état
// « interrompue » qui le respecte tout autant atteint 237 o
// (published=55535, failed=10000, firstFailIndex=55536 — SEUL firstFailIndex
// change entre les deux, l'invariant est identique des deux côtés). C'est la
// comparaison ENTRE LES DEUX MAXIMA, chacun retrouvé par recherche EXHAUSTIVE
// sur `discoveryFormatSummary()` réel (pas une reconstruction à la main du
// format) sur toute la plage valide de `firstFailIndex` (1..attempted-failed+1),
// qui tranche — pas l'invariant lui-même. L'erreur des deux évaluations
// manuelles précédentes (224, puis un « épars »=236 avancé comme le maximum
// absolu) venait d'une sous-saturation de la forme « interrompue »
// (firstFailIndex=1, published=0) qui lui faisait perdre des chiffres sans
// raison. Pire cas réel retenu : **237 o, forme « interrompue »**. 256 o
// laissent 19 o de marge sans peser sur la pile de mqttTask.
constexpr size_t kDiscoverySummaryLineMax = 256;  // Buffer pile d'UNE ligne de synthèse de rafale

// bug-121 : empreinte de la découverte HA, persistée en NVS pour survivre au
// redémarrage (DiscoveryRecord, voir src/discovery_fingerprint_logic.h).
// Namespace et clé Preferences/nvs_flash : ≤ 15 caractères chacun (contrainte
// ESP-IDF, silencieusement tronquée au-delà — vérifié : les deux tiennent).
constexpr const char* kDiscoveryNvsNamespace = "mqtt_disc";  // 9 caractères
constexpr const char* kDiscoveryNvsRecordKey = "fp_rec";     // 6 caractères — DiscoveryRecord en octets bruts (putBytes/getBytes)

// bug-121 (revue, bloquant 2) : suffixe du topic de naissance HA — le code
// hache POURTANT HA_DISCOVERY_PREFIX dans l'empreinte (il admet donc que ce
// préfixe peut changer). "homeassistant/status" en dur aux DEUX sites d'usage
// (subscribe + messageCallback) réinventait le préfixe : le jour où il change,
// l'abonnement pointe à côté et le seul chemin de récupération (A3) meurt en
// silence. Le préfixe reste HA_DISCOVERY_PREFIX (source unique, config.h) ;
// seul le suffixe est nommé ici, concaténé UNE fois (mqtt_manager.h/.cpp,
// membre _haStatusTopic) et réutilisé aux deux sites.
constexpr const char* kHaStatusTopicSuffix = "/status";

// bug-121 (revue, bloquant 1 — 2ᵉ passe) : un message de naissance HA RETENU
// (retain=true côté broker, configurable et pratiqué par une partie des
// installations) est redélivré en quelques ms APRÈS LE subscribe() de CHAQUE
// (re)connexion. Le discriminant n'est PAS « depuis la dernière rafale »
// (`_lastDiscoveryMarkMs` peut dater d'heures — un birth retenu redélivré 13
// fois sur 13 reconnexions du 08/09 aurait déclenché 13 rafales) mais « depuis
// LA CONNEXION » (`_connectedAtMs`, mqtt_manager.h) : un message retenu arrive
// sur une session TOUT JUSTE établie, un vrai redémarrage HA arrive sur une
// session déjà ancienne. Tout `homeassistant/status`==`online` reçu dans cette
// fenêtre après `_connectedAtMs` est donc DROPPÉ par messageCallback() — pas
// mis en file, pas debounce : ignoré à la source, silencieusement.
constexpr uint32_t kDiscoveryHaRestartGraceMs = 5000;

// bug-121 (revue, bloquant 1 — 1ʳᵉ passe, conservé comme second filet) :
// anti-flapping PAR-DESSUS kDiscoveryHaRestartGraceMs — couvre un HA qui
// redémarre réellement, mais en boucle rapide (chaque redémarrage passe le
// filtre grace ci-dessus car il arrive sur une session déjà établie). Mesuré
// depuis _lastDiscoveryMarkMs (posé à l'ouverture ET à la fermeture de toute
// rafale, feature-122) : un second signal HaRestart dans cette fenêtre est
// ignoré (silence, DEBUG seulement), le drapeau restant posé pour réévaluation
// au tour suivant — jamais perdu, seulement différé.
constexpr uint32_t kDiscoveryHaRestartDebounceMs = 60000;

// Intervalles capteurs (voir aussi sensors.cpp pour détails internes)
constexpr unsigned long kTempSensorIntervalMs = 2000;     // 2s - Lecture température DS18B20
constexpr unsigned long kPhOrpSensorIntervalMs = 5000;    // 5s - Lecture pH/ORP

// Délais de redémarrage
constexpr unsigned long kRestartAfterOtaDelayMs = 3000;   // 3s - Attente avant restart après OTA
constexpr unsigned long kRestartApModeDelayMs = 1000;     // 1s - Attente avant restart en mode AP

// Timeouts mutex
constexpr unsigned long kI2cMutexTimeoutMs = 2000;        // 2s - Timeout acquisition mutex I2C
constexpr unsigned long kConfigMutexTimeoutMs = 1000;     // 1s - Timeout acquisition mutex config
// feature-027 : bornage des prises de mutex (plus aucun portMAX_DELAY applicatif)
constexpr unsigned long kHistoryMutexTimeoutMs = 2000;    // 2s - Pire détenteur : consolidation + saveToFile LittleFS (~1-1,5 s)
constexpr unsigned long kLoggerMutexTimeoutMs = 100;      // 100ms - Sections critiques RAM pures (buffer circulaire logs)
constexpr size_t kLoggerConsoleLineMax = 512;             // Buffer pile d'émission console (1 ligne = 1 Serial.write atomique)
// feature-129 (recommandation P2 pool-chemistry) : borne la boucle de reprise
// de Logger::flushToDisk() (do { ... } while (_flushRedo)). Sous rafale
// d'ERROR/CRITICAL venue d'une AUTRE tâche, _flushRedo peut rester vrai en
// continu et enchaîner les passages complets (ouverture, écriture, fermeture,
// stat) sur la tâche qui détient le drapeau — potentiellement async_tcp ou
// mqttTask. 3 passages : le 1er draine le gros du retard, les 2 suivants
// absorbent la rafale concurrente ; au-delà, on rend la main et un futur
// appel (flush périodique ou prochain ERROR) reprendra exactement là.
constexpr size_t kLoggerFlushRedoMaxPasses = 3;
constexpr unsigned long kMutexTimeoutWarnThrottleMs = 60000; // 60s - Max 1 warn/min/site sur timeout mutex (statique locale par site)

// Sécurité - Factory reset bouton
constexpr unsigned long kFactoryResetButtonHoldMs = 10000; // 10s - Maintien bouton pour factory reset

// Sécurité chimique - Injection manuelle
// Borne supérieure de la durée d'une injection manuelle volumée (POST /ph/inject/start
// ou /orp/inject/start). Au-delà de 10 min, c'est probablement un usage non
// supervisé qui mérite d'être stoppé par sécurité — l'utilisateur peut toujours
// relancer une 2ᵉ injection si besoin réel de plus.
// Valeur précédente non bornée explicitement : 3600s = 1h, jugé trop long par
// pool-chemistry (risque de surdosage si filtration s'arrête en milieu de cycle).
constexpr int kManualInjectMaxDurationS = 600;             // 10 min

// Sécurité - Rate limiting
// 120 req/min = 2 req/s moyenne — couvre la navigation UI active normale
// (page /params ouverte + clics + polling /get-config + /data + /coredump/info).
// Reste une protection efficace contre les bots brute-force (auth) et DOS accidentel.
// Ancienne valeur 30 : trop basse, déclenchait des "Rate limit dépassé" en usage normal
// dès qu'on naviguait dans Paramètres → Diagnostic ou qu'on cliquait plusieurs boutons rapidement.
constexpr uint16_t kMaxRequestsPerMinute = 120;           // Limite globale par IP
constexpr unsigned long kRateLimitWindowMs = 60000;       // Fenêtre de rate limit (1 min)

// Throttle du WARN « corps de calibration non authentifié » (v2.46.1).
// MOTIF : le handler de CORPS s'exécute AVANT checkRateLimit() (appelé par
// checkAuth() dans le handler de requête), donc le quota kMaxRequestsPerMinute ne
// borne PAS ce WARN — il borne les requêtes traitées, pas les lignes déjà écrites.
// Sans throttle, un émetteur non authentifié persiste ~230 o par requête et purge
// les 6 144 o conservés du journal (kRotateKeepBytes ; fichier kMaxLogFileBytes =
// 8 192 o, logger.h:50-51) en ~27 requêtes, soit ~13 s au seul quota autorisé : la
// fenêtre forensique de ~2 h 30 (feature-109) deviendrait effaçable à volonté, alors
// que bug-080 en dépend pour tracer un redémarrage.
// ⚠️ Ce chiffrage a d'abord été écrit sur 24 Ko conservés / 32 Ko de fichier — des
// tailles HISTORIQUES, jamais celles de ce binaire. Corrigé le 2026-09-07 : la
// conclusion en est renforcée, pas affaiblie (13 s au lieu de 70). Toute reprise de
// ce calcul doit lire logger.h, jamais un commentaire.
// L'émetteur générique non borné (sendAuthRequired, toute route protégée) PRÉEXISTE
// et reste ouvert : voir bug-112.
// 1 WARN/min/route maximum, via une statique locale par site ; les tentatives
// occultées sont comptées et annoncées quand le site redevient bavard.
// Aligné sur kMutexTimeoutWarnThrottleMs (même convention).
constexpr unsigned long kCalibrationAuthWarnThrottleMs = 60000;  // 60s - 1 warn/min/route

// ============================================================================
// MEMORY & BUFFER CONSTANTS - Limites mémoire et buffers
// ============================================================================

// Limites de buffers
constexpr size_t kMaxConfigSizeBytes = 16384;             // 16KB - Taille max configuration JSON
constexpr size_t kMaxLogEntries = 200;                    // Nombre max d'entrées logs

// ============================================================================
// JOURNAL — arène circulaire en RAM interne statique (feature-129, option B′)
// ============================================================================
// Les `String` de `std::vector<LogEntry>` s'allouaient AU MILIEU du plus gros
// bloc libre du tas 8 bits et le fractionnaient (feature-129, A1) : 160 o
// alloués ont détruit 7 680 o de contiguïté en production, jusqu'à faire
// chuter le plus gros bloc sous kStaticFileMinAllocBytes et faire refuser
// toute ouverture de fichier statique. `sLogArena`/`sLogSlots` (logger.cpp,
// phase C) remplacent le vecteur de `String` par deux objets `.bss` de taille
// fixe : plus aucune allocation dynamique de journal, donc plus aucun
// fractionnement — au prix d'une réservation figée sur la ressource rare.
constexpr size_t kLogArenaBytes      = 16384;      // 82 o/entrée pour 200 entrées (mesuré : 52 o) —
                                                    // on corrige une pénurie de CONTIGUÏTÉ, pas
                                                    // d'octets : gratter 4 Ko de plus au prix de la
                                                    // marge serait se tromper de problème (feature-129).
// ⚠️ 768 et NON 256 : le recomptage agrégeant les littéraux d'UN SEUL appel est
// méthodologiquement faux pour les messages de garde chimique, construits AVANT
// l'appel (char[] ou concaténation) — l'appel ne porte plus qu'un préfixe court.
// Pire cas mesuré par pool-chemistry : ≈630 o (mqtt_manager.cpp:1614, refus de
// mode pH + phAutoManualReason). ~20% de marge. Ce plafond NE RÉSERVE AUCUN
// OCTET de `.bss` — il borne le pire cas de PROFONDEUR de l'arène, pas la
// mémoire. Précédent déjà commis puis corrigé : pump_controller.cpp:3609-3614,
// un `msg[248]` coupait "…bande mor(te)" — l'ENTRÉE du défaut qu'il devait
// rendre traçable était perdue. Plafonner à 256 réinfligerait ce défaut à tous
// les sites de garde à la fois (feature-129, condition bloquante C1).
constexpr size_t kLogMessageMaxBytes = 768;
// Marqueur de troncature ASCII STRICT (feature-129, condition bloquante C2) —
// jamais de « … » (3 o) : il faudrait le compter en octets et non en
// caractères, un piège de plus. Réservé AVANT le clamp UTF-8, jamais après
// (modèle mqtt_manager.cpp:761-762, kAlertTail).
constexpr char kLogTruncTail[]    = "... (tronque)";
// Budget d'une ligne persistée : horodatage (19) + espace (1) + niveau (≤5) +
// espace (1) + message (kLogMessageMaxBytes) + marqueur de troncature + \n/NUL
// (2). Ne réserve pas non plus d'octets de `.bss` par elle-même (utilisée pour
// dimensionner un tampon pile ponctuel côté écriture, phase C).
constexpr size_t kLogPersistLineMax  = 832;

static_assert(kLogArenaBytes >= kMaxLogEntries * 64,
              "Arène trop petite pour 200 entrées à 64 o/entrée en moyenne");
static_assert(kLogMessageMaxBytes <= kLogArenaBytes / 8,
              "Un seul message ne doit pas pouvoir occuper plus d'1/8 de l'arène");
static_assert(kLogPersistLineMax >= 19 + 1 + 5 + 1 + kLogMessageMaxBytes + sizeof(kLogTruncTail) + 2,
              "kLogPersistLineMax trop petit pour horodatage + niveau + message + marqueur");
static_assert(kLogMessageMaxBytes >= 640, "Pire cas mesure des messages de garde : 630 o");

// feature-130 (ADR-0082) : bornes HAUTES, symétriques des bornes basses
// ci-dessus. Sans elles, doubler l'arène (ou le nombre d'entrées) ne demande
// de toucher AUCUNE assertion — donc aucune justification écrite — puisque
// les asserts existants ne contraignent qu'un plancher. `sLogArena`/
// `sLogSlots` sont des objets `.bss` de taille FIXE (logger.cpp) : les
// grossir grignote la même ressource rare que feature-129 a économisée (la
// contiguïté du tas 8 bits), au bénéfice d'un budget qui n'a jamais failli
// depuis (200 entrées, 16 384 o). Un agent qui a besoin de plus DOIT remonter
// cette limite ici, explicitement — jamais en changeant seulement kLogArenaBytes
// ou kMaxLogEntries plus haut.
static_assert(kLogArenaBytes <= 16384,
              "Doubler l'arène grignote la contiguïté 8 bits que feature-129 a "
              "reconquise (ADR-0082) — justifier ici avant d'élargir.");
static_assert(kMaxLogEntries <= 200,
              "Doubler ce plafond double sLogSlots (20 o/entrée, .bss, logger.cpp) "
              "— justifier ici avant d'élargir (ADR-0082).");

// Historique de données
// 8h de données brutes (intervalle 5 min). 96 et non 72 (= 6h pile) depuis
// bug-history-doublons-troncature (revue 2.18.1) : la consolidation par créneau
// COMPLET exige qu'un point survive jusqu'à ~7h (fin de son heure + RAW_MAX_AGE
// 6h) — à 72, l'éviction affamait l'agrégation horaire. Impact budget partition
// (ADR-0033) : +24 points ≈ +2 Ko ≈ 1 bloc (pire cas 14/16, garde
// historySaveAdmit inchangée en filet).
constexpr size_t kMaxRawDataPoints = 96;
constexpr size_t kMaxHourlyDataPoints = 168;              // 7 jours de moyennes horaires (64KB partition)
constexpr size_t kMaxDailyDataPoints = 75;                // 75 jours de moyennes journalières

// Seuils mémoire
constexpr size_t kMinFreeHeapBytes = 10000;               // Seuil critique mémoire disponible (alerte « Mémoire faible » — INCHANGÉ)

// Auto-guérison mémoire (v2.9.1) — redémarrage propre sur fragmentation du tas.
// Le gel observé (web mort + logging arrêté, loopTask vivant) est causé par un plus
// gros bloc contigu qui s'effondre alors que le heap total reste correct.
// bug-080 (v2.33.0) : kRestartMaxAllocHeapBytes est désormais comparé à
// heapLargestFreeBlock8Bit() — le plus gros bloc RÉELLEMENT allouable. L'ancienne
// métrique ESP.getMaxAllocHeap() comptait l'IRAM 32 bits (~27,5 Ko figés) : son
// plancher de 8 192 o n'était matériellement JAMAIS franchissable — le crash du
// 29/08 et la famine du 02/09 sont passés sans déclenchement. 4 096 o est le
// palier auto-heal de l'échelle 8 bits (cf. bloc ÉCHELLE COMPLÈTE plus bas).
// Reste DISTINCT de kMinFreeHeapBytes (alerte sur heap total, conservée telle quelle).
constexpr uint32_t kRestartMaxAllocHeapBytes = 4096;      // Plus gros bloc 8 bits < seuil → restart auto-guérison
// = kOomReserveBytes — sous ce niveau la réserve nominale du parachute n'est plus
// allouable (bug-080, v2.33.0 ; anciennement 20 000 o sur la métrique brute).
constexpr uint32_t kLowHeapWarnBytes = 12288;             // Plus gros bloc 8 bits < seuil → WARN + flush disque one-shot
constexpr unsigned long kHeapLogIntervalMs = 300000;      // 5min - cadence mini de la trace INFO « Heap: … » (anti-spam journal)
constexpr uint32_t kHeapLogDropBytes = 8192;              // Émet aussi la trace Heap si maxBloc chute de + de 8 Ko depuis la dernière émission
// Cadence PROPRE de la trace de calibrage « Heap 8bits: … » (checkHeapHealth).
// v2.20.2 : 5 min → 30 min. À 5 min elle coûtait ~2,3 Ko/h sur un journal
// persistant de 8 Ko et participait à ramener la rétention à ~30 min, ce qui a
// détruit le post-mortem du crash du 15/08. À 30 min : ~0,24 Ko/h.
constexpr unsigned long kHeap8LogIntervalMs = 1800000;    // 30min - cadence de la trace INFO « Heap 8bits: … »

constexpr uint8_t  kAutoHealDebounceChecks = 3;           // Ticks consécutifs sous seuil avant restart (anti oscillation)
constexpr uint32_t kAutoHealMinUptimeMs = 180000;         // 3 min - uptime minimal avant tout restart auto
constexpr uint8_t  kAutoHealMaxReboots = 3;               // Plafond de reboots auto dans la fenêtre → mode maintenance
constexpr uint32_t kAutoHealRebootWindowMs = 900000;      // 15 min - fenêtre anti-boucle (tumbling, ancrée au 1er reboot)

// Échantillonnage heap adaptatif (bug-badalloc-ws-broadcast) : quand maxBloc passe
// sous kLowHeapWarnBytes, la cadence de checkHeapHealth() passe de
// kHealthCheckIntervalMs (60 s) à cette valeur → le débounce (3 ticks) confirme un
// heap critique en ~15 s au lieu de 180 s, avant que le crash ne gagne la course.
constexpr uint32_t kHeapCheckFastIntervalMs = 5000;       // 5s - cadence rapprochée sous seuil d'alerte

// Garde heap des broadcasts WebSocket (bug-badalloc-ws-broadcast) : textAll() alloue
// un shared buffer (makeSharedBuffer) SANS rattraper std::bad_alloc → abort(). Sous
// ce seuil de plus gros bloc contigu, le push est SAUTÉ (l'UI rate un cycle de 5 s,
// sans coût UX) et le débounce auto-guérison se joue.
// bug-heap-toctou (coredump 30/07) : relevé 8192 → 16384 = marge 2× vs
// kRestartMaxAllocHeapBytes. L'ancien alignement des deux seuils créait une
// zone grise où les pushes étaient autorisés ET l'auto-guérison ne s'armait
// pas — le TOCTOU (async_tcp consomme le bloc mesuré entre le check et l'alloc de
// textAll) suffisait alors à provoquer le bad_alloc.
// bug-080 (v2.33.0) : la garde lit désormais heapLargestFreeBlock8Bit() (métrique
// 8 bits réelle) et le seuil est recalibré : 6 144 o ≈ 2,4× la trame pire cas
// mesurée (2 552 o), au-dessus du palier auto-heal (4 096) — les pushes
// s'arrêtent toujours AVANT la zone rouge et l'auto-guérison garde sa fenêtre.
constexpr uint32_t kWsPushMinAllocBytes = 6144;           // Plus gros bloc 8 bits < seuil → push WS sauté
constexpr uint32_t kWsSkipWarnThrottleMs = 30000;         // 30s - throttle du warn « pushes WS sautés »

// Buffer STATIQUE (.bss) de construction des trames WS sortantes (bug-heap-toctou A2) :
// « réserver une fois, réutiliser toujours » — remplace la String temporaire par push
// (churn heap G2 résiduel : ~2 Ko alloués/libérés toutes les 5 s). Dimensionné pour le
// plus gros payload (config ~2,3 Ko mesuré) + marge de croissance. Une trame qui ne
// tient pas est SAUTÉE (jamais tronquée) et comptée (_oversizedFrames).
// Itération 3 (bug-dosage-fantome) : 3072 → 3584. La trame capteurs mesurait déjà
// ~2,8 Ko et gagne +7 champs (calendrier d'impulsion pH/ORP) ; sans marge, les
// pushes sauteraient SILENCIEUSEMENT (warn throttlé seulement). Coût : +512 o en
// .bss statique. ⚠️ Correction (feature-138, 2026-10-04) : contrairement à
// l'affirmation précédente, ce n'est PAS « hors heap 8 bits » — le .bss est en
// DRAM, et le tas 8 bits n'est jamais que le reliquat de cette même DRAM après
// soustraction du .bss/.data. Le .bss ne FRAGMENTE pas (pas d'impact sur
// max_alloc_8bit), mais il réduit bien le total disponible.
constexpr size_t kWsFrameBufBytes = 3584;                 // Taille du buffer statique de trame WS

// Récolte des clients WS zombies (investigation churn-heap §G1/G2) : un client qui
// n'ACK plus conserve une file jusqu'à 32 buffers ~1,8 Ko connexion ouverte —
// générateur n°1 du churn heap. L'état capteurs étant idempotent (chaque push est un
// snapshot complet), un client qui n'a pas consommé 4 snapshots (~20 s) est gelé →
// fermeture. En complément : keepalive PING auto pour que les zombies meurent par
// timeout ACK, et setCloseClientOnQueueFull(true) (fermeture par le fork à la
// saturation de sa file interne).
// (Récolte des clients gelés : bornée côté fork via -DWS_MAX_QUEUED_MESSAGES=5,
// platformio.ini + setCloseClientOnQueueFull — pas de constante applicative.)
constexpr uint16_t kWsKeepAlivePeriodS = 30;              // 30s - période PING auto (keepAlivePeriod, unité: secondes)

// (kWsSensorJsonReserveBytes supprimé — bug-heap-toctou A2 : les trames WS sont
// construites dans le buffer statique kWsFrameBufBytes, plus de String par push.)

// Disjoncteur de boot-loop de l'init history (v2.10.0, feature-059) : un drapeau
// NVS (crash_cnt, namespace hist_meta) est posé avant le montage de la partition
// history et levé après init complète. Si le boot précédent a crashé pendant
// cette fenêtre kHistoryInitCrashThreshold fois consécutives, la partition est
// effacée avant remontage (corruption littlefs → panic non rattrapable à la 1ʳᵉ
// écriture). Indépendant de kAutoHealMaxReboots et du mode maintenance.
constexpr uint8_t kHistoryInitCrashThreshold = 2;             // Crashs consécutifs pendant l'init → effacement partition history
constexpr const char* kHistoryCrashCountNvsKey = "crash_cnt"; // Clé NVS du compteur (namespace hist_meta, clé part_sz intacte)

// Garde d'espace de saveToFile (bug-nospc-partition-history, ADR-0033) : le pire
// cas d'occupation de la partition 64 Ko (réécriture copy-on-write de
// /history.json + /system.log + .tmp de rotation + métadonnées) dépassait la
// capacité → NOSPC structurel puis div/0 littlefs (bug amont, issue #1063).
// feature-131 (v2.53.0) : la sauvegarde est devenue ATOMIQUE (.tmp + rename,
// patron du journal ADR-0076) — l'ancien /history.json N'EST PLUS supprimé
// avant réécriture, il COEXISTE avec le .tmp pendant toute l'écriture (motif
// initial de ce commentaire, « diviser le pic COW par 2 », dissous par
// ADR-0068 : la partition history est passée de 64 Ko à 256 Ko, le pic de
// coexistence retombe à ~24 % de la partition contre ~15 % avant, l'ADR-0033
// est caduc sur ce point précis — la garde elle-même reste inchangée).
// L'admission (historySaveAdmit, module pur) compare désormais SIMPLEMENT le
// libre courant à la taille JSON + marge : aucune reprise anticipée de
// l'espace de l'ancien fichier, puisqu'il n'est jamais supprimé avant que le
// nouveau soit intégralement écrit et renommé.
constexpr size_t kHistorySaveSpaceMarginBytes = 8192;           // 2 blocs de 4096 o : compaction métadonnées near-full + arrondi bloc
constexpr unsigned long kHistorySaveWarnThrottleMs = 3600000UL; // 1 h - throttle des warn « partition pleine » et de l'INFO occupation FS

// feature-131 : écriture en flux de /history.json (plus de JsonDocument en
// RAM). Le pic d'allocation du save tombe à la taille d'UN point formaté plus
// le tampon d'écriture par blocs — voir HistoryManager::emitPersistedJson()
// et history_persist_format.h.
//
// kHistoryPersistPointMaxBytes (128) : majorant mesuré du plus long point
// persisté possible. Pire cas compté caractère par caractère sur le format de
// formatPersistedHistoryPoint() : virgule de séparation (1) + `{"t":` +
// 10 chiffres (unsigned long max, 15) + `,"p":` + valeur (jusqu'à 8, ex.
// "-999.99") (13) + `,"o":`+valeur (13) + `,"T":`+valeur (13) + `,"pf":`+valeur
// (14) + `,"of":`+valeur (14) + `,"f":false` (10) + `,"d":255` (8) + `,"g":2}`
// (7) ≈ 108 o. Arrondi à 128 pour absorber une marge sans reprendre le calcul
// à chaque champ ajouté au format (le dépassement, lui, ne perd jamais de
// données : cf. historyFormatFixed()/formatPersistedHistoryPoint(), retour 0
// symétrique entre la passe de mesure et la passe d'écriture).
constexpr size_t kHistoryPersistPointMaxBytes = 128;

// kHistoryWriteChunkBytes (512) : tampon d'accumulation du puits fichier avant
// chaque f.write() — réduit le nombre d'écritures LittleFS individuelles (un
// f.write() par point serait ~339 appels par sauvegarde dans le pire cas).
// Choisi comme multiple confortable de kHistoryPersistPointMaxBytes (4 points
// par vidage) sans peser sur la pile : 512 (tampon) + 128 (point formaté) =
// 640 o de plus sur loopTask ET sur async_tcp (POST /history/import →
// importData() → saveToFile()), négligeable face aux 16 Ko de
// CONFIG_ASYNC_TCP_STACK_SIZE et au budget de loopTask.
constexpr size_t kHistoryWriteChunkBytes = 512;

// kHistoryLoadReservePoints : réservation unique du vecteur memoryBuffer au
// chargement (loadFromFile), dérivée des plafonds de granularité existants —
// jamais recopiée en dur. Sans elle, l'escalade de capacité de std::vector
// (1→2→4→...→512) redemande jusqu'à 14 336 o CONTIGUS au passage 256→512
// (transitoire 21 504 o le temps de la copie), pour un besoin réel de
// kHistoryLoadReservePoints × sizeof(DataPoint) = 9 492 o EN UNE SEULE FOIS
// (cf. static_assert sizeof(DataPoint)==28, history.h).
constexpr size_t kHistoryLoadReservePoints =
    kMaxRawDataPoints + kMaxHourlyDataPoints + kMaxDailyDataPoints;  // 96+168+75 = 339

// kHistoryLoadMaxPoints (revue code-reviewer, feature-131) : PLAFOND DUR du
// nombre de points admis par loadFromFile() en une seule lecture. Le
// chargement en flux a perdu la borne IMPLICITE de l'ancien code — l'échec
// d'allocation du `JsonDocument` global finissait par arrêter un fichier
// pathologique. Sans plafond explicite, un /history.json corrompu ou porteur
// de doublons RAW (jamais fusionnés par dedupAggregatedPoints(), qui ignore
// RAW — cf. history.cpp) ferait croître `memoryBuffer` sans limite AU BOOT,
// avec le doublement géométrique de capacité que kHistoryLoadReservePoints
// existe précisément pour éviter.
// Facteur ×2 : marge généreuse au-dessus du nominal (339) pour absorber des
// doublons hérités de formats ≤ 2.18.0 (jusqu'à ~12/heure sur les agrégats,
// cf. bug-history-doublons-troncature) SANS autoriser une croissance non
// bornée — le fichier est de toute façon tronqué (WARNING) au-delà, jamais
// silencieusement laissé grossir.
constexpr size_t kHistoryLoadMaxPoints = 2 * kHistoryLoadReservePoints;  // 678

// Clés NVS du diagnostic NOSPC + dump de preuve (namespace hist_meta existant) :
constexpr const char* kHistoryNospcCountNvsKey  = "nospc_cnt"; // Compteur cumulé de saves refusés/échoués (uint32)
constexpr const char* kHistoryLastOkEpochNvsKey = "last_ok";   // Epoch du dernier save réussi (écrit sur échec UNIQUEMENT — usure NVS)
constexpr const char* kHistoryDumpCrc0NvsKey    = "dmp_crc0";  // CRC32 du bloc 0 dumpé par le disjoncteur (A1)
constexpr const char* kHistoryDumpCrc1NvsKey    = "dmp_crc1";  // CRC32 du bloc 1 dumpé par le disjoncteur (A1)
constexpr const char* kHistoryDumpHeadNvsKey    = "dmp_head";  // Blob : 16 premiers octets du bloc 0 (A1)

// Marge commune des gardes heap HTTP (pattern v2.10.1, généralisé par
// bug-crash-getlogs-badalloc) : la taille RÉELLE de la réponse (measureJson /
// String::reserve) + cette marge doit tenir dans le plus gros bloc contigu libre,
// sinon 503 propre — jamais de std::bad_alloc → abort(). bug-080 (v2.33.0) :
// /get-logs mesure désormais heapLargestFreeBlock8Bit() (bloc réellement
// allouable) ; la route de trace debug conserve la métrique brute (hors périmètre).
// Remplace l'ancien seuil FIXE kMinHeapForLogsResponse (24 Ko), indépendant de la
// taille réelle de la réponse, donc insuffisant (2 coredumps le prouvent).
constexpr size_t kHttpHeapMarginBytes = 4096;

// ---------------------------------------------------------------------------
// Garde 503 AVANT le premier fopen() de fichier statique (bug-080, lot B-3)
// ---------------------------------------------------------------------------
// Le PANIC du 29/08 :
//   abort() ← lock_init_generic (newlib locks.c:77) ← __retarget_lock_init_recursive
//   ← __sfp ← fopen ← VFSFileImpl ← LittleFS ← AsyncWebServerRequest::send(fs, …)
// Servir une page statique demande à la libc un verrou récursif interne (92 o) ;
// le malloc échoue, et lock_init_generic ABORTE — aucun code de retour à tester.
// Ce chemin passe par malloc(3) et non par operator new : le parachute OOM
// (std::set_new_handler) y est INOPÉRANT PAR CONSTRUCTION. La seule défense est
// de ne pas entrer dans fopen() : sous ce seuil, le handler prioritaire de
// web_server.cpp répond 503 + Retry-After au lieu de servir le fichier.
//
// Dimensionnement (coûts mesurés / relevés d'un service de fichier statique) :
// (bug-100, cycle 1 : TROIS chiffres de ce bloc étaient faux — corrigés ici
//  d'après le désassemblage de libc.a (newlib, GCC 14.2.0) et les sources IDF
//  5.5. Aucun seuil n'en dépendait ; cf. ADR-0070 §« Trois chiffres FAUX ».)
//   __sfp → xSemaphoreCreateRecursiveMutex ......  92 o  → abort() (le crash)
//     (92 o = sizeof(Queue_t), IDF 5.5 — et 92 <= kPsramExtmemThresholdBytes :
//      ce bloc reste TOUJOURS en RAM interne, la bascule PSRAM du lot B-1 ne
//      peut pas le déplacer.)
//   expansion du pool FILE (INLINÉE — __sfmoreglue n'existe PAS comme symbole,
//     une recherche par symbole aurait conclu à tort à son absence)  428 o (0x1ac)
//   esp_littlefs_open + cache lfs ..............  ~190 o → NULL propre
//   new VFSFileImpl + bloc de contrôle shared_ptr  ~64 o → bad_alloc
//   new AsyncFileResponse (mesuré le 13/08) .....  156 o → bad_alloc
//   String de chemin (×2-3) ....................  ~100 o
//   tampon stdio setvbuf 4096 ..................  PSRAM depuis le lot B-1
//   pbuf lwip TCP, 1 à 3 en vol ................ 1 622 o pièce (retransmission)
// → besoin DUR du chemin abort-prone ≈ 874 o (recompté en bug-100 sur les
//   valeurs réelles ; plus gros bloc contigu de la séquence ≈ 428 o) ;
// → besoin AVEC 2 pbuf en vol ≈ 874 + 2 × 1 622 ≈ 4 118 o.
// 8 192 = ×9,4 sur le besoin dur, ×1,99 sur le besoin avec contention TCP.
//
// Compatibilité terrain — relevés du 05/09/2026, HISTORIQUES, état d'alors
// (avant feature-129 et feature-131) : plateau calme post-B-2 13 300 o
// (+38 % au-dessus du seuil), plancher de rafale 6 requêtes 14 324 o, plancher
// absolu firmware 1 908 o (là, la garde DOIT mordre). Le plateau avait alors
// décru de façon monotone (42 996 → 26 612 → 20 468 → 15 860 → 13 300). Ces
// relevés sont CONSERVÉS tels quels : ils documentent pourquoi la garde a été
// posée, pas l'état actuel du tas.
//
// feature-132 (réexamen post-feature-131, 2026-09-20) : la décroissance
// ci-dessus est INVERSÉE, pas prolongée. Relevé du 2026-09-20 (v2.53.0, après
// feature-129 + feature-131) : ambiant instantané 34 804 o pendant la
// campagne ; planchers de fenêtre glissante observés 19 444 à 34 804 o, le
// plus bas sous rafale de 9 requêtes parallèles 19 444 o (×2,37 du seuil,
// ×4,72 du besoin avec contention) ;
// 0 refus sur 171 requêtes (19 rafales, assets + lecture de sonde), toutes en
// 200. Décision : le seuil NE CHANGE PAS — la prémisse d'un pic d'historique
// dimensionnant ce seuil était fausse (le bloc de dimensionnement ci-dessus ne
// contient aucun terme d'historique ; feature-131 a supprimé un concurrent sur
// le même tas, pas un terme de cette somme), et aucun 503 n'est observé à
// baisser. Le seul palier intermédiaire crédible (7 168) ramènerait la marge à
// ×1,74 — sous le ×1,99 revendiqué — pour un gain non mesuré, et déplacerait en
// silence le second palier dérivé de fs_guard.cpp (`probeFloorArmed`, cf. plus
// bas) de 16 384 à 14 336.
//
// Ne jamais descendre ce seuil sous 6 144 o : en dessous, le seuil devient
// indistinguable de kWsPushMinAllocBytes au diagnostic, et la marge tombe sous
// ×1,6 du besoin avec contention TCP.
// Écartés : 6 144 (identique au barreau WS, donc indistinguable au diagnostic) ;
// 7 168 (marge ×1,74, sous le ×1,99 revendiqué, pour un gain non mesuré) ;
// 12 288 (à 1 012 o du plateau calme d'alors → UI perdue sur une mauvaise minute).
//
// 6ᵉ barreau de l'échelle ADR-0058 (cf. bloc « ÉCHELLE COMPLÈTE » plus bas) —
// les 5 barreaux existants restent INCHANGÉS (condition C5 des gates B-1/B-2).
//
// Partage vérifié (feature-132) : ce seuil est consommé par 11 sites de CODE
// (hors commentaires et #include) sur 6 fichiers, plus un palier DÉRIVÉ
// implicite — toute future modification déplacerait onze comportements et un
// palier caché, pas un seul. Pas de numéros de ligne ici : ils rancissent
// (cf. le renvoi périmé que ce lot corrige) — un grep sur le nom de la
// constante les retrouve toujours :
//   fs_guard.cpp (2, dont le palier DÉRIVÉ probeFloorArmed = maxAlloc8 <
//     2 × kStaticFileMinAllocBytes, soit un second seuil implicite à 16 384 o)
//   · logger.cpp (2) · history.cpp (2) · web_server.cpp (3)
//   · web_routes_data.cpp (1) · web_helpers.cpp (1).
constexpr uint32_t kStaticFileMinAllocBytes = 8192;       // Plus gros bloc 8 bits < seuil → fichiers statiques refusés (503) — valeur revérifiée le 2026-09-20, inchangée (feature-132)

// Position du 6ᵉ barreau verrouillée à la COMPILATION, pas seulement par la suite
// native : une échelle mal ordonnée doit casser le build, pas un test.
static_assert(kStaticFileMinAllocBytes > kWsPushMinAllocBytes &&
              kStaticFileMinAllocBytes < kLowHeapWarnBytes,
              "6e barreau ADR-0058 : la garde 503 doit s'intercaler entre les pushes WS "
              "sautes et le WARN heap — sinon l'appareil rebooterait avant d'avoir "
              "jamais refuse une page.");
static_assert(kStaticFileMinAllocBytes >= 6144,
              "Interdiction ADR : ne jamais descendre ce seuil sous 6144 o, sinon il "
              "devient indistinguable de kWsPushMinAllocBytes au diagnostic et la "
              "marge tombe sous x1,6 du besoin avec contention TCP (feature-132).");

// Silence exigé avant de clore un épisode de refus (front descendant).
// C'est d'abord une GARDE DE COÛT : httpLowHeapNeedsHeapSample() n'autorise la
// loop à parcourir le tas que si un épisode est ouvert ET ce délai écoulé. Sans
// elle, update() walkerait le tas à la fréquence de la loop pendant TOUTE la
// famine — au pire moment possible, et pour bien plus cher que le filtre HTTP
// (dont la fréquence, elle, est bornée par l'arrivée des requêtes TCP).
// C'est ensuite un anti-spam : un tas qui oscille AUTOUR du seuil produirait
// sinon une paire de lignes de journal par oscillation — exactement la
// sur-journalisation qui a détruit la fenêtre forensique du crash du 15/08
// (journal persistant de 8 Ko, rétention 25-35 min).
// Aligné sur kWsSkipWarnThrottleMs.
constexpr uint32_t kHttpLowHeapEpisodeQuietMs = 30000;    // 30 s sans refus + tas rétabli → fin d'épisode

// ---------------------------------------------------------------------------
// Admission des fopen INTERNES (bug-100, ADR-0070) — 7ᵉ barreau + cadences
// ---------------------------------------------------------------------------
// Le lot B-3 (garde 503 ci-dessus) ferme le SERVICE DE FICHIERS STATIQUES. Il ne
// ferme RIEN de ce que le firmware ouvre pour son propre compte : le recensement
// de bug-100 en dénombre 14 (docs/subsystems/acces-fichiers.md), dont celui qui
// a aborté le 05/09/2026 depuis loopTask (sauvegarde de l'historique).
//
// DEUX CLASSES DE CONTENTION, deux seuils (ADR-0070 §b) :
//  - classe A — contention TCP réelle possible → kStaticFileMinAllocBytes (8 192,
//    6ᵉ barreau EXISTANT, réutilisé tel quel) : les 2 sites de saveToFile()
//    (atteignables depuis POST /history/import sur async_tcp), TOUT le journal
//    (flushToDisk() s'exécute sur la tâche QUI LOGUE), et /download-logs ;
//  - classe B — 4 sites de boot → 7ᵉ barreau NOUVEAU, 5 120. L'absence de
//    contention n'est pas supposée, elle est PROUVÉE par l'ordre de setup() :
//    history.begin() (main.cpp:270) précède mqttManager.begin() (300) et
//    webServer.begin() (316) — aucune pile TCP applicative n'est démarrée.
//
// Le 7ᵉ barreau n'est PAS un seuil de report : c'est un TRIPWIRE DE BOOT. Au
// démarrage, « reporter » n'a pas de sens — il n'y a pas de plus tard. Sous
// 5 120, history.begin() bascule historyEnabled = false pour CE boot (état déjà
// supporté, history.cpp:326-331) au lieu d'aborter. Conséquence voulue : entre
// 4 096 (auto-guérison) et 5 120, l'appareil DÉMARRE SANS HISTORIQUE PERSISTANT
// MAIS EN DOSANT. C'est un gain de disponibilité de la régulation, pas un
// confort de diagnostic — le régime alternatif est un bootloop sur abort().
// Marge : ×5,9 sur le besoin dur de 874 o (sans contention, par construction).
// Écartés : 5 120 partout (×1,24 seulement sur la classe A — trop mince pour un
// chemin sans code de retour) ; 8 192 partout (priverait les 4 sites de boot de
// toute dégradation propre alors que leur besoin réel y est de 874 o).
constexpr uint32_t kInternalFopenMinAllocBytes = 5120;    // Plus gros bloc 8 bits < seuil → fopen interne de boot refusé (tripwire)

// Position du 7ᵉ barreau verrouillée à la COMPILATION (même exigence que le 6ᵉ) :
// une échelle mal ordonnée doit casser le build, pas un test.
static_assert(kInternalFopenMinAllocBytes > kRestartMaxAllocHeapBytes &&
              kInternalFopenMinAllocBytes < kWsPushMinAllocBytes,
              "7e barreau ADR-0070 : le tripwire fopen interne doit s'intercaler entre "
              "le reboot d'auto-guerison et les pushes WS sautes — sinon l'appareil "
              "rebooterait avant d'avoir jamais demarre sans historique.");
// Marge minimale exigée sur le besoin dur d'une ouverture (874 o, valeurs
// réelles ADR-0070) : ×5,27. Verrouille le seuil contre un abaissement discret.
static_assert(kInternalFopenMinAllocBytes >= 4608,
              "7e barreau ADR-0070 : marge insuffisante sur le besoin dur d'un fopen.");

// Silence de refus exigé avant de re-mesurer le tas (donc avant d'envisager la
// sortie d'épisode). Même rôle que kHttpLowHeapEpisodeQuietMs, et même valeur :
// c'est d'abord une GARDE DE COÛT (un parcours TLSF coûte d'autant plus cher que
// le tas est fragmenté — donc le plus cher exactement pendant la famine), et
// ensuite un anti-spam de journal.
constexpr uint32_t kInternalFopenEpisodeQuietMs = 30000;  // 30 s sans refus → on re-mesure

// Plancher DUR entre deux parcours du tas au point de décision. Pendant une
// rafale (flush de journal sur ERROR, import d'historique), les demandes
// s'enchaînent à quelques millisecondes : sous ce plancher on REFUSE SANS
// MESURER. C'est l'asymétrie d'ADR-0070 §e — un refus périmé coûte une
// sauvegarde différée, une autorisation périmée coûte un abort().
constexpr uint32_t kInternalFopenProbeMinIntervalMs = 100;  // 100 ms entre deux mesures en épisode

// Escalade : nombre de refus cumulés dans un même épisode au-delà duquel on
// publie une alerte MQTT (publishAlert → warning(), JAMAIS error/critical).
// 5 : au-dessus d'un simple creux transitoire (l'historique n'essaie que toutes
// les 5 min), assez bas pour qu'une rafale de flush de journal la déclenche.
constexpr uint32_t kInternalFopenEscalateAfterRefusals = 5;

// Pagination de GET /get-logs (bug-crash-getlogs-badalloc) : page souple de 30
// entrées (~4 Ko de JSON au pire), extensible jusqu'au plafond dur UNIQUEMENT pour
// une queue d'entrées partageant la même milliseconde que la dernière incluse —
// le curseur ?since= étant strictement supérieur, les exclure les perdrait
// définitivement entre deux pages (règles d'admission : logs_page.h).
constexpr size_t kLogsMaxEntriesPerPage = 30;             // Limite souple (taille de page visée)
constexpr size_t kLogsPageHardCap = 60;                   // Plafond dur (coupe même une queue même-ms)

// Export /download-logs en flux chunked : longueur max d'une ligne formatée
// (horodatage + niveau + message — tronquée avec « … » au-delà). Passée en
// paramètre template à LogLineCarry<N> (logs_export_chunk.h, header pur).
constexpr size_t kLogExportLineMaxBytes = 256;

// Pire cas d'un point JSON de /get-history — RE-MESURÉ en bug-098 (v2.47.0),
// « dosing » passant du booléen au MASQUE DE VOIES entier (kHistDose*,
// history.h ; format de formatHistoryPointJson, history_export_chunk.h) :
//   clés + ponctuation fixes ........ 111 o (virgule de séparation comprise)
//   timestamp (10 chiffres) .......... 10 o
//   ph "%.1f" / temperature "%.1f" ... 6 + 6 o
//   phFiltered "%.2f" ................. 7 o
//   orp / orpFiltered (entiers) ....... 5 + 5 o
//   "false" (filtration) .............. 5 o
//   "dosing" %u + granularité %u .. 3 + 3 o
//   TOTAL pire cas .................. 161 o (+ NUL) → 192 laisse 31 o de marge.
// ⚠️ LES DEUX DÉCOMPTES PRÉCÉDENTS ÉTAIENT FAUX, chacun d'un côté différent :
// l'original annonçait 163 pour une partie fixe de 111 (juste : 111+52 = 163
// avec l'ancien "dosing":false), et la « rectification » de revue a porté la
// partie fixe à 114 en gardant 161 — deux chiffres inconciliables (114+52 =
// 166). Le total ci-dessus est MESURÉ, pas recopié : le pire cas absolu du
// format vaut 161 o (les deux %u saturés à "255"). Le pire cas ATTEIGNABLE
// vaut 157 o : le masque ne dépasse pas 7 (trois bits définis) et la
// granularité est bornée à DAILY = 2 — UN caractère chacun, pas trois.
// Le changement de bug-098 AGRANDIT la marge de 2 o, il ne la consomme pas.
// PIRE CAS THÉORIQUE, à connaître : si les cinq conversions saturaient leurs
// tampons de 15 caractères, le point atteindrait 213 o et dépasserait 192. Ce
// cas est INATTEIGNABLE avec des mesures plausibles (un pH ne fait pas 15
// chiffres), et depuis feature-095 le chemin de saut n'est plus muet : les
// points omis sont comptés (`st->skipped`) et un WARNING throttlé est émis à la
// clôture de la réponse (web_routes_data.cpp, kHistorySkipWarnThrottleMs). On
// documente donc la borne au lieu de surdimensionner le tampon.
// bug-heap-toctou A3 : sert de capacité du carry (LogLineCarry<N>) de la réponse
// CHUNKED — un point (ou l'en-tête/pied JSON) doit tenir dedans, jamais de JSON
// partiel émis. Coût du relèvement 128 → 192 : +64 o de pile/état par requête
// /get-history (le carry est un membre de l'état de réponse, un seul vivant à la
// fois par requête), et +64 o pour /download-logs seulement s'il partageait la
// constante — ce n'est pas le cas (kLogExportLineMaxBytes lui est propre).
// ⚠️ L'ancien commentaire affirmait qu'un dépassement était « impossible sur le
// format réel, ~120 o max » : c'est FAUX depuis feature-095 (161 o), et ce
// n'était de toute façon pas une garantie. En cas de dépassement, le point est
// SAUTÉ (web_routes_data.cpp, phase kPoints) — mais PLUS EN SILENCE depuis
// feature-095 : il est compté et signalé une fois par heure. La formulation
// « trou invisible dans les courbes », restée ici et recopiée depuis, était
// PÉRIMÉE. Toute nouvelle clé au format impose de refaire ce décompte.
constexpr size_t kHistoryPointJsonMaxBytes = 192;

// feature-095 : throttle du WARNING « points omis de /get-history » (un seul par
// réponse, et au plus un par heure toutes réponses confondues — le dashboard
// rappelle la route à chaque rafraîchissement). Même cadence que les warn de
// sauvegarde d'historique : assez rare pour ne pas noyer le journal, assez
// fréquent pour qu'un trou de courbe soit expliqué dans la même session.
constexpr unsigned long kHistorySkipWarnThrottleMs = 3600000UL;  // 1 h

// /get-history (bug-history-doublons-troncature, itération 2) : instantané
// filtré du buffer copié en UNE prise de mutex à l'ouverture de la requête
// (handler HTTP, tâche async_tcp). Timeout court : un 503 rare vaut mieux que
// 2 s d'async_tcp bloquée sur une consolidation. Remplace l'accès point par
// point sous mutex (kHistoryPointReadTimeoutMs/kHistoryChunkMaxRetries retirés)
// dont les timeouts en cours de flux tronquaient la réponse.
constexpr unsigned long kHistorySnapshotTimeoutMs = 100;

// bug-points-futurs : tolérance au-delà de « maintenant » avant de considérer
// un timestamp comme impossible (purgé au chargement). 1 h absorbe un décalage
// d'horloge bénin sans laisser passer les excursions franches.
constexpr unsigned long kHistoryFutureToleranceSeconds = 3600;

// Ring d'instrumentation heap en RAM RTC noinit (bug-heap-toctou A4) : cinétique
// d'effondrement du heap dans les ~4 dernières minutes avant un reset anormal
// (le tick santé 5-60 s la rate). 128 entrées × 8 o = 1 Ko RTC noinit ; encodage
// pur dans heap_ring_logic.h, coquille impure dans heap_ring.cpp.
constexpr size_t kHeapRingEntries = 128;                  // Capacité du ring (128 × 8 o = 1 Ko RTC noinit)
constexpr unsigned long kHeapRingSampleIntervalMs = 2000; // 2s - cadence d'échantillonnage depuis loop()
constexpr size_t kHeapRingDumpMaxEntries = 32;            // Entrées dumpées au boot post-crash (~64 s de cinétique)

// ----------------------------------------------------------------------------
// Planchers glissants MULTI-MASQUES (feature-122, lot C-1a — INSTRUMENTATION)
// ----------------------------------------------------------------------------
// Les allocations qui échouent en production portent caps = 0x080C
// (8BIT|DMA|INTERNAL) alors que toutes les gardes du projet mesurent 0x0804 :
// on suit donc les QUATRE masques de la dérivation (0x0804, 0x080C, 0x1000,
// 0x1804) plus le tas PSRAM, chacun avec son plancher depuis le boot et sur
// fenêtre. Masques et arithmétique : heap_caps_logic.h (pur).
// ⚠️ CE LOT NE CORRIGE RIEN : aucune garde ne lit ces planchers, aucune réserve
// DMA n'est posée (c'est feature-123).
//
// 5 emplacements = 4 masques de la dérivation + MALLOC_CAP_SPIRAM (A6). L'ordre
// des quatre premiers est celui des bits publiés par /get-system-info.
constexpr uint8_t kHeapFloorMaskCount = 5;
// PLAFOND DE COÛT (condition 3 du gate pool-chemistry) : 2 parcours O(blocs) par
// échantillon — le masque de référence 0x0804 + UN masque en tour de rôle. À la
// cadence de kHeapRingSampleIntervalMs, cela fait EXACTEMENT 1 parcours/s, le
// plafond fixé par le gate. Comptabilité honnête : avant ce lot, heapRingSample
// faisait 1 parcours O(blocs) (heapLargestFreeBlock8Bit) + 1 agrégat O(régions)
// (heapFree8Bit) ; le lot fond ces deux appels en un seul heap_caps_get_info et
// dépense le créneau libéré sur un second masque — le surcoût NET est donc de
// +1 parcours toutes les 2 s, et il est plafonné ici.
// Les masques 0x1000 et SPIRAM n'entrent PAS dans ce budget : ils empruntent le
// chemin O(régions) de heapProbeForCaps (leur « plus gros bloc » vaut
// kOomProbeBlockUnknown, jamais un bloc réel).
constexpr uint8_t kHeapFloorWalksPerSample = 2;
static_assert(kHeapFloorWalksPerSample * 1000UL <= kHeapRingSampleIntervalMs,
              "Plafond du gate pool-chemistry : au plus UN parcours O(blocs) du tas "
              "par seconde (interruptions coupees). Ni accelerer la cadence, ni "
              "ajouter un 3e masque a parcours complet.");

// ---------------------------------------------------------------------------
// CONDITION 5 DU GATE pool-chemistry — DÉCLENCHÉE par le relevé du 08/09/2026
// ---------------------------------------------------------------------------
// Le gate avait maintenu son GO en s'appuyant sur les ~84 µs par parcours relevés
// ailleurs dans le projet (docs/subsystems/oom-guard.md), et posé cette
// condition : « relever la durée réelle d'un parcours 0x0804 sur cible ; si la
// médiane dépasse kOomProbeWarnDurationUs = 300 µs, porter le tour de rôle à 4 s
// AVANT feature-123 ».
//
// RELEVÉ SUR CIBLE, 8 h 49 de fonctionnement, 12 673 échantillons (v2.49.0) :
//   walk = 516 µs au pire / médiane <= 512 µs
// Soit SIX FOIS le repère sur lequel le gate avait raisonné, et bien au-dessus
// des 300 µs de sa condition. Elle se déclenche donc.
//
// Le masque du tour de rôle n'est plus mesuré qu'un échantillon sur deux : la
// cadence du tour de rôle passe de 2 s à 4 s, et la période de revisite d'un
// masque de 4 s à 8 s. Coût moyen : 1,5 parcours O(blocs) par échantillon de 2 s,
// soit 0,75/s au lieu de 1/s — sous le plafond du gate, avec de la marge pour la
// première fois.
// ⚠️ Ce qui est perdu : la RÉSOLUTION du plancher 0x080C, qui passe de 4 à 8 s.
// C'est le chiffre qui dimensionnera la réserve de feature-123, et il faut le
// savoir en le lisant. Atténuation : heaps(0x080C) est un sous-ensemble de
// heaps(0x0804), dont le plancher reste mesuré à CHAQUE échantillon et le borne
// donc supérieurement — dimensionner sur 0x0804 reste conservateur.
constexpr uint8_t kHeapFloorWalkEverySamples = 2;
static_assert(kHeapFloorWalkEverySamples >= 1,
              "Un diviseur nul ferait disparaitre le tour de role : les masques "
              "0x080C et 0x1804 ne seraient JAMAIS mesures.");
static_assert(1000UL * kHeapFloorWalksPerSample <=
                  kHeapRingSampleIntervalMs * kHeapFloorWalkEverySamples,
              "Plafond du gate, exprime en moyenne : le parcours de reference (1 par "
              "echantillon) plus le tour de role (1 tous les kHeapFloorWalkEverySamples) "
              "doivent tenir sous UN parcours O(blocs) par seconde. Mesure du 08/09 : "
              "516 us au pire, la duree d'UNE occurrence n'est donc pas le probleme — "
              "c'est la frequence que le gate a demande de baisser.");
// Durée pendant laquelle un échantillon reste marqué « rafale de découverte HA »
// après le dernier repère posé par publishDiscovery(). 60 s : une rafale de ~60
// entités dure quelques secondes sur un lien sain et bien davantage sur CPL
// lossy — sous-dimensionner la fenêtre attribuerait le creux au régime calme
// qui suit la rafale.
constexpr uint32_t kMqttDiscoveryCtxWindowMs = 60000;

// ----------------------------------------------------------------------------
// Recensement du tas + lecture de contenu (feature-128, R2/R3) — INSTRUMENTATION
// STRICTEMENT À LA DEMANDE, JAMAIS PÉRIODIQUE
// ----------------------------------------------------------------------------
// `heap_caps_walk()` bloc par bloc (adresse + taille + occupé/libre), pour
// isoler ce qui découpe le grand bloc contigu (incident 2026-09-10). R2
// (recensement, adresses + tailles) est compilé inconditionnellement ; R3
// (lecture de contenu) est sous compilation conditionnelle POOL_HEAP_PEEK
// (heap_census.h) — non livré dans un binaire de release (ADR-0081).
//
// COÛT APÉRIODIQUE — comptabilité du gate pool-chemistry (condition C3) :
// aucun de ces parcours ne s'ajoute à kHeapFloorWalksPerSample (périodique,
// 0,75 parcours/s en moyenne après la condition 5 du gate ci-dessus). Le
// plafond posé par pool-chemistry pour CE lot est 0,2 parcours/s AJOUTÉS, soit
// EXACTEMENT 1 / (kHeapCensusMinIntervalMs / 1000). Décision du firmware-
// developer : R2 et R3 parcourent le même tas, au même coût O(blocs) — ils
// PARTAGENT donc le même horodatage de dernier parcours (heap_census.cpp),
// de sorte que le plafond ajouté reste 0,2 parcours/s au total, jamais 0,4
// (0,2 + 0,2) si les deux étaient sollicités en alternance. L'invariant
// « ≤ 1 parcours O(blocs)/s » reste donc lisible ET vrai en incluant ce lot :
// 0,75/s (périodique) + 0,2/s (plafond de ce lot, pas une moyenne) < 1/s.
constexpr uint8_t kHeapCensusMaxFreeBlocks = 24;    // Top-N approché des blocs libres — ~192 o .bss (24 x 8 o)
constexpr uint8_t kHeapCensusMaxUsedBlocks = 96;    // Blocs occupés dans la fenêtre demandée — ~1152 o .bss (96 x 12 o)
constexpr uint8_t kHeapCensusPageEntries = 24;      // Pagination du GET (route, hors périmètre de ce module)
// uint32_t (et non `unsigned long`, idiome kMqttDiscoveryCtxWindowMs ci-dessus) :
// consommées TELLES QUELLES par les prédicats purs uint32_t de
// heap_census_logic.h, sans cast au site d'appel.
constexpr uint32_t kHeapCensusMinIntervalMs = 5000;  // 5s - plancher PARTAGÉ recensement+lecture (S9, C3)
constexpr uint32_t kHeapCensusStaleMs = 300000;      // 5 min - au-delà : instantané périmé (GET -> 410)
static_assert(kHeapCensusMinIntervalMs >= 5000u,
              "Plafond pool-chemistry (condition C3) : au plus 0,2 parcours O(blocs) "
              "AJOUTE par seconde, PARTAGE entre le recensement (R2) et la lecture "
              "(R3) puisqu'ils cotent le meme O(blocs). Descendre sous 5000 ms "
              "depasserait le plafond approuve par le gate.");

// C1 (gate pool-chemistry, BLOQUANT) : chronométrage esp_timer_get_time()
// (JAMAIS le compteur de cycles CCOUNT — sans objet ici, la durée mesurée est
// locale à UN appel de heapCensusServiceLoop(), aucune migration de coeur entre
// deux lectures à corriger). Auto-désactivation DÉFINITIVE au-delà de ce
// plafond, sur le patron de oom_guard.cpp:408-418
// (heapIntegrityAdmitNextRun -> sProbeDisabled.store(true)).
// MOTIF : la base est MOUVANTE — `wmax` (parcours de référence 0x0804,
// heap_caps_logic.h) valait 516 µs au pire en v2.49.0 (commentaire ci-dessus)
// et 711 µs six jours plus tard (+38 %). L'instrument doit se couper
// lui-même plutôt que se fier à une estimation périmée.
constexpr uint32_t kHeapCensusMaxDurationUs = 1500;  // 1,5 ms - au-delà : DÉSACTIVÉ définitivement (R2 ET R3)

// ADR-0072 : sentinelle « non renseigné », jamais une valeur inventée. Publiée
// quand le canari du poisoning léger n'est pas conforme (bloc corrompu, ou
// CONFIG_HEAP_POISONING_LIGHT absent d'un futur build — voir le #error de
// heap_census.cpp).
constexpr uint32_t kHeapCensusAllocSizeUnknown = 0xFFFFFFFFu;

// Poisoning léger (CONFIG_HEAP_POISONING_LIGHT, épinglé par #error dans
// heap_census.cpp, idiome de heap_metrics.cpp:20-23) : chaque bloc OCCUPÉ
// commence par poison_head_t {uint32_t canary; size_t alloc_size;} = 8 o, le
// pointeur utilisateur se trouvant à ptr + kHeapPoisonHeadBytes.
// `kHeapPoisonCanaryValue` : valeur ABI de multi_heap_poisoning.c (constatée
// dans .rodata de l'objet compilé, désassemblage indépendant) — sert à
// valider structurellement un bloc avant tout déréférencement de contenu (S4).
constexpr uint32_t kHeapPoisonHeadBytes = 8;
constexpr uint32_t kHeapPoisonCanaryValue = 0xABBA1234u;

// R3 (lecture de contenu) — sous POOL_HEAP_PEEK, non livré en production
// (ADR-0081). Ces constantes existent inconditionnellement (coût nul, ce sont
// des entiers) : seul le CODE qui les consomme est conditionnel.
constexpr uint32_t kHeapPeekMaxBytes = 128;          // Longueur max par requête
constexpr uint32_t kHeapPeekMaxBytesPerBoot = 8192;  // Budget CUMULÉ depuis le boot ; épuisé -> refus jusqu'au reboot

// ============================================================================
// PARACHUTE OOM GLOBAL (bug-oom-accept-http-durcissement, crash du 11/08/2026)
// ============================================================================
// Coredump du 11/08 : std::bad_alloc non rattrapé dans async_tcp (operator
// new(356) à l'accept HTTP) → std::terminate → abort. Le parachute pré-alloue
// une réserve d'urgence au plus TÔT du setup (avant toute fragmentation) ; le
// std::set_new_handler la libère à la première famine → le new en cours est
// RE-TENTÉ et réussit (contrat new_handler) au lieu d'aborter. Logique pure :
// oom_guard_logic.h ; glue Arduino : oom_guard.{h,cpp}.

// Taille de la réserve d'urgence. 12 Ko : très au-dessus des petites allocs
// vitales (accept HTTP ~356 o, trames WS, String logs) et assez gros pour
// laisser à l'auto-guérison (kRestartMaxAllocHeapBytes, débounce ~15 s) le
// temps de déclencher un reboot PROPRE pendant la famine.
constexpr size_t kOomReserveBytes = 12288;

// Hystérésis de réarmement : la réserve est libérée quand maxBloc est ≈ épuisé
// (échec d'un new), mais n'est RE-allouée que lorsque le plus gros bloc contigu
// repasse au-dessus de ce seuil, pour ne pas osciller libération/réallocation
// autour du seuil de famine.
//
// bug-parachute-oom-jamais-rearme (2ᵉ crash du 13/08/2026, uptime 12,9 h) :
// ERREUR DE CALIBRAGE de la v2.18.5 — le seuil valait 32768 o alors que le plus
// gros bloc contigu de CET appareil en régime établi vaut 27-30 Ko (mesures
// terrain : 29 684 o à 33 min d'uptime, 27 584 o à 12,9 h). Le seuil était donc
// AU-DESSUS du régime normal : une fois la réserve consommée, elle n'était
// JAMAIS ré-allouée et le handler restait désenregistré → parachute one-shot en
// pratique, ce qui a laissé passer le bad_alloc de 156 o du 13/08 (tas pourtant
// comptabilisé sain : free=81 408 o, maxB=27 584 o).
// bug-080 (v2.33.0, gate du 02/09/2026) : 24 576 → 18 432 = kOomReserveBytes
// (12 288) + kRestartMaxAllocHeapBytes (4 096) + kOomFallbackRearmMinMaxAllocBytes
// (2 048). Le gate a REFUSÉ 16 384 : allouer la réserve nominale de 12 288 depuis
// un bloc à 16 384 laisse un reliquat < 4 096 — le parachute sèmerait son propre
// auto-heal. 18 432 < nominal terrain 21 492 o → réarmable en régime réel (défaut
// n°1 de la récidive du 13/08 : un seuil au-dessus du régime = parachute one-shot).
//
// ÉCHELLE COMPLÈTE des seuils « plus gros bloc contigu » (ordre croissant).
// bug-080 (v2.33.0) : TOUS les seuils sont désormais exprimés dans la MÊME
// métrique 8 bits (heapLargestFreeBlock8Bit — mémoire réellement allouable).
// bug-100 (ADR-0070) : l'échelle passe de SIX à SEPT barreaux. Les 6 existants
// restent INCHANGÉS en valeur ET en ordre (condition C5 des gates B-1/B-2,
// reconduite) ; le 7ᵉ (5 120) s'intercale entre l'auto-guérison et les pushes
// WS. Aucun barreau ne se modifie isolément.
// RÉVOCATION du NO GO v2.20.2 : le gate pool-chemistry avait interdit de basculer
// les gardes sur la mesure 8 bits (risque : mode maintenance = DOSAGE SUSPENDU
// atteignable en régime normal). NO GO levé par le gate du 02/09/2026 —
// l'immunité contre les faux positifs vient du DÉBOUNCE TEMPOREL (3 mesures
// consécutives, soit ≥ 10-15 s de famine continue), pas du niveau du seuil ;
// données terrain : nominal 21 492 o, creux transitoires 1 268 o pendant
// quelques secondes seulement. Décision consignée : ADR-0058.
//    2 048 o (kOomFallbackRearmMinMaxAllocBytes) → réarmement réserve de REPLI
//    4 096 o (kRestartMaxAllocHeapBytes)         → reboot d'auto-guérison
//    5 120 o (kInternalFopenMinAllocBytes)       → fopen INTERNE de boot refusé (tripwire)  ← 7ᵉ barreau, bug-100 / ADR-0070
//    6 144 o (kWsPushMinAllocBytes)              → pushes WS sautés
//    8 192 o (kStaticFileMinAllocBytes)          → fichiers statiques refusés (503) + classe A des fopen internes  ← 6ᵉ barreau, bug-080 lot B-3 (v2.41.0)
//   12 288 o (kLowHeapWarnBytes)                 → WARN heap + flush one-shot
//   18 432 o (kOomRearmMinMaxAllocBytes)         → réarmement réserve NOMINALE
//   21 492 o                                     → nominal terrain (documentaire)
constexpr uint32_t kOomRearmMinMaxAllocBytes = 18432;
constexpr unsigned long kOomRearmCheckIntervalMs = 10000;  // 10 s - cadence lente du réarmement (loop)

// Réserve de REPLI (2ᵉ palier) : si le bloc nominal de 12 Ko n'est pas allouable,
// le parachute s'arme quand même avec un bloc plus petit — « toujours armé »
// vaut mieux que « jamais réarmé » (défaut n°1 du 13/08). Les petites allocations
// vitales observées dans les trois crashs tiennent très largement dedans (accept
// HTTP 356 o, AsyncFileResponse 156 o, __cxa_allocate_exception 100 o).
// Le surclassement repli → nominal est fait par la loop dès que maxBloc repasse
// au-dessus de kOomRearmMinMaxAllocBytes ; JAMAIS l'inverse (pas d'oscillation).
//
// RECALIBRAGE v2.20.2 (bug-famine-8bits, bloc E1) — seuil 12288 → 2048 et taille
// 2048 → 1024. Les deux valeurs étaient calées sur ESP.getMaxAllocHeap() (FAUSSE :
// IRAM 32 bits comprise, figée à 27 636 o) alors que le tick lit désormais
// heapLargestFreeBlock8Bit(). Régime 8 bits RÉEL mesuré sur 5 h :
// maxBloc8 ∈ [4 084 ; 9 204], minMaxBloc8_boot = 3 060.
// INVARIANT C2 imposé par le gate pool-chemistry, vérifié ici :
//   (a) seuil de réarmement 2 048 < 3 060  → le palier de repli reste armable même
//       au pire creux observé depuis le boot (aucun trou de protection) ;
//   (b) taille allouée 1 024 ≤ 2 048 / 2   → allouer la réserve ne peut pas à soi
//       seul faire retomber le tas sous son propre seuil de réarmement ;
//   (c) 2 048 ≤ 4 084 = min(maxBloc8 observé en régime) → armable en pratique.
// kOomRearmMinMaxAllocBytes = 18 432 depuis bug-080 (v2.33.0) : désormais
// ATTEIGNABLE en régime réel (nominal terrain 21 492 o) — le surclassement
// repli → nominal redevient automatique dès que la fragmentation se résorbe.
constexpr size_t kOomFallbackReserveBytes = 1024;
constexpr uint32_t kOomFallbackRearmMinMaxAllocBytes = 2048;

// Backoff du WARN différé « allocation heap échouée » (callback diagnostic
// heap_caps_register_failed_alloc_callback, mode flag-seul — drainé par loop).
// v2.20.2 : remplace le throttle FIXE kOomFailWarnThrottleMs (30 s), qui produisait
// ~120 WARN/h et faisait tourner le journal persistant (8 Ko) en 25-35 min — le
// post-mortem du crash du 15/08 a été détruit par notre propre journalisation.
// Escalade 30 s → 5 min → 30 min (saturation), retour au palier initial après une
// heure sans nouvel échec. Logique pure : oomFailWarnNextDelayMs().
constexpr unsigned long kOomFailWarnDelayInitialMs = 30000;    // 30 s - 1er palier (signal immédiat)
constexpr unsigned long kOomFailWarnDelayMidMs = 300000;       // 5 min - 2e palier
constexpr unsigned long kOomFailWarnDelayMaxMs = 1800000;      // 30 min - palier de saturation
constexpr unsigned long kOomFailWarnDelayResetMs = 3600000;    // 1 h sans échec → retour au palier initial

// ---------------------------------------------------------------------------
// Post-mortem d'allocation en RAM RTC + verdict (bug-parachute-oom-jamais-rearme)
// ---------------------------------------------------------------------------
// Départage les deux hypothèses laissées ouvertes par le crash du 13/08 :
//   (a) consommateur très rapide dans l'angle mort de 2 s de l'anneau heap,
//   (b) corruption du tas (comptabilité saine mais l'allocateur refuse 156 o).
// Règle du verdict (oomPostMortemVerdict, module pur) : une famine capable de
// refuser une PETITE allocation refuse TOUT — donc beaucoup d'échecs et une
// comptabilité écroulée. À l'inverse, 1-2 échecs isolés avec un plus gros bloc
// annoncé au moins kOomCorruptionSaneBlockRatio × la taille demandée est
// incohérent avec une famine → suspicion de corruption.
constexpr uint32_t kOomCorruptionMaxFails = 8;        // Au-delà : famine (l'allocateur refuse en série)
constexpr uint32_t kOomCorruptionSaneBlockRatio = 4;  // maxBloc ≥ 4 × taille demandée = comptabilité saine
// Fenêtre de RAFALE (revue v2.18.8) : le compteur d'échecs est cumulé sur tout
// le boot. Sans borne temporelle, quelques échecs bénins étalés sur des heures
// concluraient « famine » sur le scénario même que ce verdict doit trancher
// (13/08 : 2 échecs, tas sain). Une vraie famine refuse tout en quelques
// secondes — 60 s couvre largement l'épisode du 11/08 (~14 s).
constexpr uint16_t kOomCorruptionBurstSpanS = 60;     // Échecs nombreux ET resserrés → famine

// Contrôle d'intégrité périodique du tas (heap_caps_check_integrity_all) —
// détecteur direct de l'hypothèse (b). Cadence LENTE (5 min) : le parcours de
// tous les blocs coûte du temps CPU et prend les verrous de l'allocateur.
// AUTOLIMITATION (condition pool-chemistry C3) : WARN au-delà de
// kHeapIntegrityWarnDurationUs, désactivation DÉFINITIVE (+ WARN one-shot)
// au-delà de kHeapIntegrityMaxDurationUs — le contrôle ne doit jamais devenir
// lui-même une source de gigue pour la régulation ni pour le watchdog.
constexpr unsigned long kHeapIntegrityCheckIntervalMs = 300000; // 5 min - cadence du contrôle d'intégrité
constexpr uint32_t kHeapIntegrityMaxDurationUs = 20000;         // 20 ms - au-delà : contrôle désactivé définitivement
constexpr uint32_t kHeapIntegrityWarnDurationUs = 5000;         // 5 ms - au-delà : WARN (contrôle conservé)

// ---------------------------------------------------------------------------
// SONDE DU TAS AU MOMENT EXACT DU REFUS (bug-080, lot B-2)
// ---------------------------------------------------------------------------
// POURQUOI : jusqu'ici le verdict du post-mortem confrontait la taille refusée à
// l'échantillon de l'ANNEAU heap, vieux de 2 s et mesuré sur des caps FIXES
// (INTERNAL|8BIT). Les 6 échecs réels du 05/09/2026 (1 622 o refusés avec
// caps=0x080C = 8BIT|DMA|INTERNAL, sans PANIC) étaient donc qualifiés
// « SUSPICION CORRUPTION » sur un maxBloc de 26 612 o périmé qui ne répondait pas
// à la question posée : le pool DMA, lui, était bien à sec. La sonde mesure
// désormais le tas AU REFUS, sur les caps DE L'ÉCHEC **et** sur une référence
// INTERNAL|8BIT — d'où le verdict neuf « FAMINE DMA (pool restreint) ».
//
// AUTOLIMITATION (mêmes principes que le contrôle d'intégrité ci-dessus, mais
// des seuils 3 à 13 fois plus serrés : la sonde s'exécute dans un chemin
// d'échec, pas dans la loop). La durée mesurée est celle du COUPLE des DEUX
// parcours, pas de chacun. Le plafond reste STRICTEMENT sous le seuil de WARN du
// contrôle d'intégrité : une sonde ne doit jamais coûter l'ordre de grandeur
// d'un parcours complet toléré ailleurs.
constexpr uint32_t kOomProbeWarnDurationUs = 300;         // 300 µs - au-delà : WARN one-shot, sonde CONSERVÉE
constexpr uint32_t kOomProbeMaxDurationUs = 1500;         // 1,5 ms - au-delà : sonde DÉSACTIVÉE définitivement
// Plancher temporel ABSOLU entre deux sondes (condition 3 du gate pool-chemistry),
// EN PLUS du jeton « un échantillon par épisode » : le backoff du WARN d'échec
// démarre à 0 ms, donc son drain immédiat pourrait réarmer le jeton quelques
// millisecondes seulement après la sonde précédente.
constexpr uint32_t kOomProbeMinIntervalMs = 1000;         // 1 s - intervalle minimal entre deux sondes
// Repli si getCpuFrequencyMhz() renvoie 0 (le hook ne fait qu'une division
// entière par cette valeur, calculée UNE fois au setup).
constexpr uint32_t kOomProbeDefaultCpuMhz = 240;          // MHz - repli de conversion cycles → µs
// Sentinelle « plus gros bloc NON RENSEIGNÉ » : pour caps & MALLOC_CAP_SPIRAM la
// sonde emprunte un chemin O(régions) (agrégats) au lieu du parcours O(blocs),
// pour BORNER A PRIORI le pire cas (condition 2 du gate) — 4 Mo de PSRAM
// contiennent des ordres de grandeur plus de blocs que la RAM interne. La valeur
// est celle que produit la quantification du champ RTC saturé (0xFFFF << 6) :
// aucun encodage spécial, aucune règle de décodage neuve.
constexpr uint32_t kOomProbeBlockUnknown = 0xFFFFu << 6;  // 4 194 240 - « non renseigné »

// Sentinelle « NOMBRE DE BLOCS non renseigné » (feature d'instrumentation
// free_blocks/allocated_blocks, ADR-0072) — SŒUR de kOomProbeBlockUnknown, mais
// PAS la même : celle-ci quantifie un nombre d'OCTETS (compatible avec
// l'encodage RTC 64 o/quantum), tandis qu'un nombre de BLOCS n'a ni la même
// unité ni le même domaine (quelques milliers tout au plus, même sur le plus
// gros tas de la cible). Les confondre afficherait un compte de blocs de
// 4 194 240 comme s'il était réel — exactement la faute qu'ADR-0072 proscrit.
// `UINT32_MAX` est hors du domaine de tout compte de blocs plausible (la RAM
// interne ne peut matériellement pas contenir 4 milliards de blocs, même au
// quantum TLSF le plus fin), donc un choix de sentinelle sûr et sans ambiguïté.
constexpr uint32_t kOomProbeBlockCountUnknown = 0xFFFFFFFFu;  // « non renseigné »

static_assert(kOomProbeWarnDurationUs < kOomProbeMaxDurationUs,
              "Le seuil de WARN de la sonde doit rester sous son plafond de desactivation");
static_assert(kOomProbeMaxDurationUs < kHeapIntegrityWarnDurationUs,
              "La sonde (chemin d'echec) doit rester bien plus courte que le WARN du "
              "controle d'integrite (loop) : elle s'execute interruptions coupees, "
              "potentiellement en rafale.");

// ---------------------------------------------------------------------------
// Seuil de bascule PSRAM des allocations ordinaires (bug-080, lot B-1, v2.35.0)
// ---------------------------------------------------------------------------
// FRAGMENTEUR IDENTIFIÉ le 04/09/2026 : la couche FS d'Arduino attache un tampon
// stdio de EXACTEMENT 4096 octets à CHAQUE fichier LittleFS ouvert
// (`libraries/FS/src/vfs_api.cpp:22` DEFAULT_FILE_BUFFER_SIZE, posé par le
// `setvbuf(_f, NULL, _IOFBF, ...)` de la ligne 250). Aggravant : `VFSImpl::exists()`
// OUVRE réellement le fichier, et AsyncStaticWebHandler::_searchFile enchaîne
// `exists()` PUIS `open()` → 2 × 4096 o par fichier statique servi. Un chargement
// d'interface (≈ 8 requêtes, jusqu'à 6 en parallèle) réclame ainsi jusqu'à ~49 Ko
// de RAM interne 8 bits EN RAFALE, sur un `free8` mesuré à 42-59 Ko.
//
// POURQUOI CE TAMPON RESTAIT INTERNE ALORS QUE 4 Mo DE PSRAM DORMENT À CÔTÉ :
// le sdkconfig des libs précompilées (IDF 5.5) porte CONFIG_SPIRAM_USE_MALLOC=y
// et CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096, et le test de
// `heap_caps_malloc_default` est « taille <= limite ⇒ RAM INTERNE » (désassemblage
// de libheap.a, `.iram1.2`, `bgeu a8, a2` — inclusif). Avec une limite à 4096, un
// malloc de 4096 tombe donc du côté INTERNE : il s'en fallait d'UN OCTET.
//
// D'où le correctif : `heap_caps_malloc_extmem_enable(kPsramExtmemThresholdBytes)`
// au setup (main.cpp, juste après initOomGuard). 4095 est CHIRURGICAL — la seule
// classe d'allocations qui change de camp est « exactement 4096 o » ; tout ce qui
// est strictement plus grand partait DÉJÀ en PSRAM d'abord, et tout ce qui est à
// 4095 o ou moins reste en RAM interne comme avant. On ne déplace pas une
// politique d'allocation, on ferme une inégalité non stricte.
// Rappel : la bascule est une PRÉFÉRENCE, pas une contrainte — si la PSRAM refuse,
// l'allocateur retombe sur la RAM interne (aucun échec supplémentaire créé).
//
// ---------------------------------------------------------------------------
// ⚠️ ANNOTATION feature-124 (S7 de bug-080, v2.50.0) : LE SEUIL PASSE À 1 024
// ---------------------------------------------------------------------------
// Le paragraphe ci-dessus est CONSERVÉ À DESSEIN : « chirurgical » était la
// décision juste à son heure (on ne déplaçait rien, on fermait une inégalité).
// Ce lot ABANDONNE cette prudence, en connaissance de cause. Ce qui a changé,
// c'est la MESURE : le 09/09/2026 à 07:10 (v2.49.3), l'instrument fin de
// feature-122 a relevé un plancher `hf.i8.mb` de 1 076 o pendant une rafale
// HTTP volontaire (`ctxAuCreux = 0x02`), avec des sondes d'échec à 948 o. Or le
// tampon TX WiFi qui échoue pèse 1 622 o : 1 076 < 1 622, donc l'échec est
// arithmétique. Le chiffre de 2 932 o sur lequel toute la stratégie avait été
// bâtie n'était PAS le creux, mais le seuil de déclenchement du refus 503.
// ⇒ Le tas interne 8 bits est la ressource rare ; la PSRAM dort à 4,18 Mo
//   libres. On DÉPLACE donc bien une politique d'allocation, cette fois.
//
// L'ENCADREMENT DE LA VALEUR — les deux bornes et leur motif.
// Règle codée, à garder en tête pour ne pas raisonner à l'envers :
// « taille <= seuil ⇒ RAM INTERNE » ; seul ce qui DÉPASSE le seuil préfère la
// PSRAM.
//  · BORNE HAUTE — strictement SOUS 1 600 : les pbufs lwIP (~1 600 o, une
//    structure par connexion TCP) sont la classe qui fragmente, et ils ne
//    basculent que si 1 600 > seuil. `lwipopts.h` retient
//    `mem_clib_malloc = malloc` avec MEM_LIBC_MALLOC=1 et MEMP_MEM_MALLOC=1
//    (CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP n'est pas activé) : TOUTES les
//    structures lwIP passent par le malloc ordinaire, donc par ce seuil.
//  · BORNE BASSE — strictement AU-DESSUS de 512 : les caches de LittleFS
//    (512 o) et le pool FILE de la libc (428 o) doivent RESTER en RAM interne.
//    Ce sont eux qui côtoient les ÉCRITURES FLASH, pendant lesquelles le cache
//    flash/PSRAM est coupé — le vrai danger de ce lot. Au-dessus de 512
//    l'exposition est quantifiée ; en dessous elle change de NATURE.
//  · Fenêtre utile [512 ; 1 599] → 1 024.
// ⚠️ NE PAS regraver le raisonnement INVERSÉ qui avait d'abord fait retenir
//   2 048 (« au-dessus de 1 622 pour capturer la classe lwIP ») : il conclut le
//   contraire de ce qu'il énonce, puisque 1 622 <= 2 048 resterait INTERNE.
//   Erreur rattrapée par le gate pool-chemistry avant tout code.
//
// ⚠️ CE QUE CE LOT NE FAIT PAS : il ne déplace AUCUN tampon WiFi. Les
// allocations qui échouent demandent `caps=0x080C` EXPLICITEMENT (1 622 o) —
// elles contournent ce seuil, et le contourneront toujours. Ce lot ne les
// déplace pas : il leur FAIT DE LA PLACE en déportant en PSRAM tout ce qui
// passe par le malloc ordinaire. Promesse plus faible qu'un correctif, à
// énoncer ainsi partout.
// Critère de succès BINAIRE : faire remonter le plancher fin `hf.i8.mb`
// AU-DESSUS de 1 622 o sous reproduction volontaire identique. Sinon le lot
// n'a pas marché, et c'est un résultat publiable.
constexpr size_t kVfsStdioBufferBytes = 4096;        // Miroir de DEFAULT_FILE_BUFFER_SIZE (vfs_api.cpp:22)
constexpr size_t kPsramExtmemThresholdBytes = 1024;  // « <= seuil ⇒ interne » : au-delà de 1 024 o ⇒ PSRAM d'abord

static_assert(kPsramExtmemThresholdBytes < kVfsStdioBufferBytes,
              "Le seuil PSRAM doit rester STRICTEMENT sous le tampon stdio VFS "
              "(4 096 o) : le test de heap_caps_malloc_default est « taille <= seuil "
              "=> RAM interne », donc un seuil >= 4096 laisserait ce tampon en RAM "
              "interne — le defaut exact que B-1 avait corrige. "
              "L'EGALITE stricte d'origine (seuil + 1 == tampon, donc seuil = 4095) "
              "est ABANDONNEE par feature-124 : elle gravait la doctrine "
              "'chirurgicale' de B-1, ou la seule classe deplacee etait 'exactement "
              "4 096 o'. Le plancher fin de 1 076 o mesure le 09/09/2026 passe SOUS "
              "le tampon TX WiFi de 1 622 o : le tas interne 8 bits est la ressource "
              "rare, et la politique d'allocation est deplacee a dessein.");
static_assert(kPsramExtmemThresholdBytes >= 512,
              "PLANCHER DE POLITIQUE (feature-124) : sous 512 o, les caches de "
              "LittleFS (512 o) et le pool FILE de la libc (428 o) basculeraient en "
              "PSRAM. Ce sont EUX qui cotoient les ecritures flash, pendant "
              "lesquelles le cache flash/PSRAM est coupe : l'exposition passerait de "
              "QUANTIFIEE a BINAIRE. Ne jamais planifier de palier sous 512.");
static_assert(kOomReserveBytes > kPsramExtmemThresholdBytes,
              "La reserve du parachute depasse le seuil : elle DOIT etre allouee "
              "avec MALLOC_CAP_INTERNAL explicite (oom_guard.cpp), jamais malloc(). "
              "Toujours vrai a 1 024 (12 288 > 1 024) — verifie par feature-124.");
// Condition C3 du gate pool-chemistry (feature-124) : le palier de REPLI du
// parachute vaut EXACTEMENT 1 024 o, soit le seuil. Il reste donc du cote
// INTERNE (test inclusif « <= seuil »), et il est de toute facon alloue par
// l'entonnoir MALLOC_CAP_INTERNAL|8BIT d'oom_guard.cpp — jamais par malloc().
// Cet assert ne protege pas AUJOURD'HUI : il ferme un palier FUTUR. Un seuil
// abaisse sous 1 024 ferait de cette taille une classe « preferee PSRAM » sans
// aucune alerte, et la prochaine main qui retirerait l'entonnoir explicite
// n'aurait plus rien pour l'avertir.
static_assert(kOomFallbackReserveBytes <= kPsramExtmemThresholdBytes,
              "Condition C3 (feature-124) : la reserve de REPLI (1 024 o) doit rester "
              "du cote INTERNE du seuil. Si vous abaissez kPsramExtmemThresholdBytes "
              "sous 1 024, RELISEZ oom_guard.cpp : la reserve n'est sauvee que par ses "
              "caps explicites, et une reserve d'urgence en PSRAM ne sauve rien d'une "
              "famine de RAM INTERNE (piege deja tombe : feature-066, prouve le "
              "05/09/2026).");

// Condition C2 du gate pool-chemistry (bug-080 B-1) : un parachute NON armé ne
// doit jamais l'être en silence. Passé ce délai sans aucune réserve (ni nominale
// ni de repli), la loop émet un CRITICAL + une alerte MQTT `oom_reserve_absent`.
// 5 min = 30 tentatives de réarmement (kOomRearmCheckIntervalMs = 10 s) : bien
// au-delà d'un creux transitoire, très en deçà des heures de vol à nu qu'ont
// duré les régimes sans filet des 13 et 15/08/2026.
constexpr unsigned long kOomNoReserveCriticalDelayMs = 300000;  // 5 min - sans réserve → CRITICAL + alerte MQTT

// Garde des sessions TLS sortantes GitHub (/check-update, /download-update),
// évaluée AVANT la création de WiFiClientSecure — prédicat pur oomTlsGateReason.
//
// CRITÈRE 1 (fail-closed) : PSRAM LIBRE requise — la mesure passée au prédicat
// est ESP.getFreePsram(), PAS ESP.getPsramSize() (taille installée, constante et
// jamais décroissante : une PSRAM saturée ouvrirait la garde alors que mbedTLS
// retomberait sur la RAM interne). Mesure terrain du 12/08/2026 sur WROOM-32
// sans PSRAM en v2.18.5 : free=87708 o et maxBloc=34804 o PASSAIENT la garde
// bicritère, puis le handshake mbedTLS (qui réclame plusieurs blocs contigus
// d'environ 16 Ko) fragmentait le tas jusqu'à faire échouer une allocation de
// 100 o — soit la fenêtre exacte du crash bad_alloc du 11/08. Sans PSRAM cet
// état est le régime NORMAL après quelques minutes : la mise à jour en ligne est
// refusée d'emblée (BOM PCB v2 = WROVER-E : 8 Mo de PSRAM installés, ~4 Mo
// adressables par la fenêtre de cache), l'upload manuel de fichier reste
// disponible. Seuil à 1 Mo de LIBRE : large sur les ~4 Mo utilisables
// (avec SPIRAM_USE_MALLOC les allocations > 4 Ko y partent), rejette 0, une
// PSRAM résiduelle inutilisable ET une PSRAM saturée.
// NOTE : valeur JAMAIS mesurée sur WROVER réel — à confirmer à la mise en
// service (relever le getFreePsram() nominal avant de figer ce seuil).
constexpr uint32_t kOtaTlsMinPsramBytes = 1048576;         // 1 Mo - PSRAM LIBRE minimale pour tenter TLS sortant
// CRITÈRE 2 : bicritère heap historique (ADR-0032) — la session TLS GitHub coûte
// ~58 Ko répartis (2×16 Ko de buffers I/O + parse X509 + budget JSON 8 Ko) →
// exige heap TOTAL suffisant ET un plus gros bloc contigu décent. Valeurs
// INCHANGÉES depuis la v2.18.5 : elles n'ont plus à porter seules la protection
// (le critère PSRAM le fait) — à RE-MESURER au swap WROVER, où les buffers
// mbedTLS > 4 Ko partent en PSRAM.
constexpr uint32_t kCheckUpdateMinFreeHeapBytes = 65536;   // Heap libre total minimal pour tenter TLS
constexpr uint32_t kCheckUpdateMinMaxAllocBytes = 20480;   // Plus gros bloc contigu minimal pour tenter TLS

// Release notes GitHub relayées telles quelles par /check-update : le corps de
// release n'est pas borné côté GitHub → tronquer (suffixe « … ») pour borner la
// réponse HTTP construite en RAM.
constexpr size_t kOtaReleaseNotesMaxChars = 2048;

// Budget NET d'allocations du JsonDocument de /check-update (allocateur
// plafonné avec crédit à la libération, bug-checkupdate-heap-tls / ADR-0032).
// La réponse GitHub est parsée en flux avec filtre (7 champs retenus) : pic
// NET ~5-6 Ko pour une release normale (corps ≤ ~2,5 Ko + doublement transitoire
// du buffer de chaîne + pool de variants ~1 Ko) ; 8 Ko couvre large. Au-delà →
// NoMemory → 500 propre au lieu d'une consommation non bornée.
constexpr size_t kOtaCheckJsonBudgetBytes = 8192;

// ============================================================================
// SENSOR CONSTANTS - Paramètres capteurs
// ============================================================================

// Échantillonnage et filtrage
constexpr int kNumSensorSamples = 3;                      // Nombre d'échantillons pour médiane (impair)

// ADS1115 timing (8 SPS = 125ms par échantillon)
constexpr unsigned long kAds1115SampleTimeMs = 125;       // Temps par échantillon à 8 SPS
constexpr unsigned long kAds1115ThreeSamplesMs = 375;     // Temps pour 3 échantillons

// DS18B20 - Sondes OneWire (feature-020)
constexpr size_t kMaxDs18b20Sondes = 2;                   // 2 sondes : eau piscine + circuit interne
constexpr size_t kSondeAddrLen = 8;                       // Adresse ROM 1-Wire 64 bits

// Clés NVS pour les adresses ROM des sondes (8 octets binaires, putBytes/getBytes)
// Stockées dans le namespace "poolctrl" comme le reste de la config.
constexpr const char* kNvsKeyOwWaterAddr = "ow_water_addr";    // Adresse sonde eau piscine
constexpr const char* kNvsKeyOwCircuitAddr = "ow_circuit_addr"; // Adresse sonde circuit interne

// Conversion temps
constexpr unsigned long kMillisToSeconds = 1000;          // Conversion ms → s
constexpr unsigned long kSecondsPerMinute = 60;           // Secondes par minute
constexpr unsigned long kMillisToMinutes = 60000;         // Conversion ms → min
constexpr unsigned long kSecondsPerHour = 3600;           // Secondes par heure

// ============================================================================
// ATLAS EZO CONSTANTS - Modules Atlas Scientific EZO Embedded I²C (PCB v2)
// ============================================================================
// Voir feature-021 (migration ADS1115 → EZO) et docs/adr/0012-pcb-v2-gpio-mapping.md.
// Bus I²C partagé avec DS3231 (kI2cSdaPin / kI2cSclPin).

constexpr uint8_t  kEzoPhAddress              = 0x63;     // EZO pH I²C address (default Atlas)
constexpr uint8_t  kEzoOrpAddress             = 0x62;     // EZO ORP I²C address (default Atlas)
constexpr uint32_t kEzoReadDelayMs            = 900;      // Délai après commande R (lecture)
constexpr uint32_t kEzoCalDelayMs             = 900;      // Délai après commande Cal,*
constexpr uint32_t kEzoRtDelayMs              = 600;      // Délai après commande RT,<temp>
constexpr uint32_t kSensorStaleTimeoutMs      = 20000;    // 20 s : timeout lecture pH/ORP stale (pool-chemistry condition #1)
constexpr int      kEzoBusFailMaxConsecutive  = 2;        // 2 échecs consécutifs I²C → blocage dosage (pool-chemistry condition #5)
constexpr unsigned long kPhSlopeQueryIntervalMs = 86400000UL; // 24h - re-query Slope,? auto (feature-024 pente sonde pH)
// v2.21.2 (bug-slope-boot-perdue) : la Slope,? de boot échouait EN SILENCE
// (échec isolé jamais journalisé) et n'était JAMAIS retentée avant le cycle
// 24 h → « Pente non disponible » pendant ~20 min après chaque reboot.
// Retentative auto tant que _phSlopeQueriedMs == 0, bornée (cf. slope_retry_logic.h).
constexpr unsigned long kPhSlopeBootRetryIntervalMs = 30000UL; // 30 s entre retentatives Slope,? de boot
constexpr uint8_t kPhSlopeBootRetryMaxAttempts = 5;            // Borne DURE (gate C1 pool-chemistry) : 5 transactions max, jamais réarmé

// ----------------------------------------------------------------------------
// COMPENSATION DE TEMPÉRATURE DE LA MESURE pH (bug-114)
// ----------------------------------------------------------------------------
// La commande `RT,<tempC>` fait calculer la correction de Nernst À BORD du
// module EZO pH. Une température de compensation fausse fausse donc le pH SANS
// toucher à la calibration — et toutes les gardes chimiques, calculées sur la
// mesure, valident la falsification.
//
// Chiffrage de référence (méthode : pente de Nernst, verdict `pool-chemistry`
// du 2026-09-07, cf. specs/features/doing/bug-114-*.md) :
//   pH_lu − pH_vrai = (pH_vrai − 7) × (T_vrai − T_comp) / T_comp   (T en kelvins)
// soit ≈ 0,7 millième de pH par °C d'erreur. Conséquences chiffrées :
//   - offset de calibration +1000 °C  → `RT,1025.0` ⇒ ≈ +0,15 pH ;
//   - offset très négatif             → T_comp → 0 K ⇒ erreur NON BORNÉE.
// D'où les trois bornes ci-dessous.

// Bornes de `mqttCfg.tempCalibrationOffset` (offset utilisateur de la sonde eau).
// Motif : un DS18B20 dérive de DIXIÈMES de degré, pas de degrés. ±5 °C laisse
// une marge d'un ordre de grandeur sur toute dérive physique crédible ; au-delà,
// ce n'est plus une calibration mais une saisie erronée (ou une NVS polluée par
// une version antérieure, qui n'imposait AUCUNE borne — cf. bug-114).
constexpr float kTempCalOffsetMin = -5.0f;
constexpr float kTempCalOffsetMax =  5.0f;

// Fenêtre de température ACCEPTÉE pour la compensation `RT` envoyée à l'EZO pH.
// Motifs (arbitrage `pool-chemistry`, NE PAS resserrer) :
//   - 0 °C  : plancher de l'eau liquide d'un bassin extérieur. Au gel, la
//             filtration est arrêtée et les mesures n'ont plus de sens.
//   - 40 °C : ~6 °C de marge au-dessus de toute eau de bassin plausible en
//             canicule. ⚠️ BORNE DE PRUDENCE, NON CONFIRMÉE PAR LA DATASHEET
//             ATLAS : la plage réellement admise par la commande `RT` n'a pas
//             été vérifiée sur le document constructeur (recherche dans le
//             dépôt le 2026-09-07 : aucune trace, cf. docs/subsystems/sensors.md
//             qui ne documente que les DÉLAIS, pas les plages).
// ⚠️ Resserrer sur 15-30 °C rejetterait des températures VRAIES (hors saison,
// canicule) et DÉGRADERAIT la compensation au lieu de la protéger.
constexpr float kEzoCompTempMinC = 0.0f;
constexpr float kEzoCompTempMaxC = 40.0f;

// Dernier étage de la cascade de repli : température de compensation par défaut
// quand aucune valeur plausible n'est disponible, même périmée.
// Motif : 25 °C est le centre de la plage d'exploitation d'un bassin ; le coût
// maximal de ce repli est ± 0,01 pH (verdict `pool-chemistry`), donc acceptable
// — mais il doit rester VISIBLE (journalisé, et exposé par un accesseur).
constexpr float kEzoFallbackTempC = 25.0f;

// Durée au-delà de laquelle le repli sur kEzoFallbackTempC cesse d'être un
// incident de lecture isolé pour devenir une PANNE (journalisation `critical`).
// Motif : le DS18B20 est relu toutes les kTempSensorIntervalMs (2 s) ; 10 min
// = 300 cycles de lecture manqués d'affilée. En deçà, une rafale d'échecs I²C
// ou un remplacement de sonde à chaud reste plausible.
constexpr unsigned long kEzoCompFallbackCriticalMs = 600000UL;  // 10 min

// Intervalle de RAPPEL de la journalisation d'une substitution de température
// de compensation. Les transitions d'étage sont journalisées immédiatement ;
// ce rappel évite qu'un repli DURABLE devienne muet, sans inonder les logs
// (la compensation est réévaluée toutes les kPhOrpSensorIntervalMs = 5 s, soit
// 12 messages/minute sans throttle).
constexpr unsigned long kEzoCompSubstitutionLogIntervalMs = 60000UL;  // 1 min

// ============================================================================
// SENSOR FILTER CONSTANTS - Lissage mesures pH/ORP (feature-025 → feature-058)
// ============================================================================
// Chaîne de filtrage (feature-058, tendance centrale) :
//   brut → rejet vraie aberration (NaN / hors-plage / spike DUR isolé)
//        → médiane sur fenêtre LONGUE → EMA lente.
// La valeur filtrée = estimateur central COURANT, jamais figée (feature-058) :
//   l'oscillation légitime soutenue n'est PLUS rejetée (fini le rejet de « saut »
//   maxStep qui écartait un côté de l'enveloppe et faisait coller/geler la valeur).
// L'instabilité (inhibition dosage) est DÉCOUPLÉE de la valeur : elle est pilotée
// par la DISPERSION de la fenêtre brute acceptée (max−min), le latch EMI et les
// rejets consécutifs — voir SensorFilter::unstable().
// Centralisé ici pour ajustement terrain. Buffer FIXE (pas d'alloc dynamique).
// Validation pool-chemistry feature-058 (conditions C1..C6 non négociables).

// --- Fenêtre médiane LONGUE (tendance centrale) ---
// Capacité physique du buffer (dimensionne float _buffer[]). FIXE, jamais dépassée.
constexpr uint8_t kSensorFilterCentralWindowMax  = 16;       // Capacité physique buffer médian (FIXE)
// Fenêtre logique effective (identique pH et ORP). À 5 s/échantillon, retard médian
// ≈ (13/2)×5 ≈ 30 s, soit +15 s vs l'ancienne fenêtre de 7 (C6). Latence tolérée :
// pH/ORP varient lentement → réponse plus lente mais plus prudente (ne chasse pas le bruit).
constexpr uint8_t kSensorFilterCentralWindow     = 13;       // Fenêtre médiane logique (retard ≈30 s, +15 s vs v1)

constexpr float   kPhEmaAlpha                    = 0.10f;    // Coefficient EMA pH (lissage lent)
constexpr float   kOrpEmaAlpha                   = 0.08f;    // Coefficient EMA ORP (lissage lent)
constexpr float   kPhFilterMin                   = 0.0f;     // Plage plausible pH min
constexpr float   kPhFilterMax                   = 14.0f;    // Plage plausible pH max
constexpr float   kOrpFilterMin                  = -1000.0f; // Plage plausible ORP min (mV)
constexpr float   kOrpFilterMax                  = 1500.0f;  // Plage plausible ORP max (mV)
constexpr uint8_t kSensorFilterMaxConsecutiveRejects = 10;   // Rejets NaN/hors-plage consécutifs → instable

// --- Dispersion (drapeau d'instabilité, C1/C3) ---
// Si (max − min) de la fenêtre brute ACCEPTÉE et PLEINE dépasse ce cap → unstable().
// Caps maintenus SOUS l'amplitude EMI observée (feature-057) pour préserver
// l'inhibition actuelle de la régulation (C1) — ne PAS assouplir.
constexpr float   kPhFilterMaxDispersion         = 0.5f;     // pH : dispersion fenêtre → instable
constexpr float   kOrpFilterMaxDispersion        = 100.0f;   // mV : dispersion fenêtre → instable

// --- Spike DUR isolé (vraie aberration, C3) ---
// Un point unique tel que |brut − médiane| dépasse ce seuil est ÉCARTÉ (n'alimente
// pas le buffer, donc EXCLU du calcul de dispersion) et ne compte PAS comme instabilité.
// Bien au-dessus de l'amplitude d'oscillation légitime : ne rejette que l'aberration franche.
// MAINTENUS EN L'ÉTAT (verdict pool-chemistry P4, bug-filtre-ph-verrouille) : le
// seuil de rejet n'est PAS la cause du verrouillage — c'était l'absence de sortie
// de secours. L'élargir dégraderait la protection contre les vraies aberrations
// (feature-058) sans rien résoudre. La sortie de secours ci-dessous suffit.
constexpr float   kPhFilterSpikeReject           = 1.0f;     // pH : spike dur isolé écarté
constexpr float   kOrpFilterSpikeReject          = 300.0f;   // mV : spike dur isolé écarté

// --- Sortie de secours : ré-amorçage sur rejets de spike SOUTENUS ET COHÉRENTS ---
// (bug-filtre-ph-verrouille-par-rejet-de-spike-sans-issue, v2.20.3)
// Un échantillon rejeté comme spike n'alimente pas le buffer → la médiane ne peut
// plus évoluer → tout échantillon suivant est rejeté à son tour : le filtre se
// verrouille DÉFINITIVEMENT sur une médiane périmée (incident 2026-08-16, dosage
// pH ET chlore à l'arrêt ≥ 3 h). Le critère qui distingue une aberration réelle
// d'un échelon légitime de la grandeur mesurée est la COHÉRENCE MUTUELLE des
// rejets : N rejets successifs proches les uns des autres décrivent une nouvelle
// valeur, pas du bruit. Trois gardes cumulatives (pool-chemistry) encadrent le
// ré-amorçage pour qu'une sonde réellement HS reste fail-closed.
constexpr uint8_t  kSensorFilterSpikeRearmCount   = 12;        // Rejets cohérents consécutifs avant ré-amorçage (≈60 s à 5 s)
constexpr uint32_t kSensorFilterRearmCooldownMs   = 1800000UL; // 30 min entre deux ré-amorçages (anti-chasse au bruit)
// Plancher de gel de l'alimentation du filtre après un front montant d'eau : même
// si la coquille déclare déjà les mesures représentatives (délai de stabilisation
// utilisateur à 0), on ne laisse JAMAIS l'eau stagnante de la chambre de mesure
// entrer dans le filtre pendant la première minute de circulation.
constexpr uint32_t kSensorFilterGateFloorMs       = 60000UL;   // 60 s

// Cohérence exigée entre les N rejets (amplitude max−min de la série rejetée).
// Largement sous le seuil de spike : une série DISPERSÉE est du bruit, pas un échelon.
constexpr float    kPhFilterRearmCoherence        = 0.20f;     // pH
constexpr float    kOrpFilterRearmCoherence       = 40.0f;     // mV
// Bande admissible de la nouvelle valeur cible : hors de cette bande, la mesure
// n'est plus physiquement plausible pour un bassin → on REFUSE de ré-amorcer
// (fail-closed : sonde suspecte), on alarme, et le dosage reste bloqué.
// ATTENTION — ces bornes sont sur l'échelle BRUTE : le filtre travaille en amont
// de l'offset logiciel, qui n'est appliqué qu'en sortie d'accesseur (ADR-0039).
// La bande effective en pH CORRIGÉ est donc [6.50 + ph_offset, 8.20 + ph_offset].
constexpr float    kPhFilterRearmMin              = 6.50f;     // pH BRUT
constexpr float    kPhFilterRearmMax              = 8.20f;     // pH BRUT
constexpr float    kOrpFilterRearmMin             = 300.0f;    // mV
constexpr float    kOrpFilterRearmMax             = 950.0f;    // mV

// --- Alarme « filtre non prêt durablement » (passe 2) ---
// Un filtre non prêt alors que la filtration tourne = AUCUNE régulation. Au-delà
// de ce délai → critical + alerte visible, répétée périodiquement tant que dure
// la panne (l'absence totale de régulation ne doit jamais être silencieuse).
constexpr uint32_t kSensorFilterNotReadyAlarmMs   = 300000UL;  // 5 min
constexpr uint32_t kSensorFilterNotReadyRepeatMs  = 1800000UL; // 30 min
// Anti-abus de la réinitialisation manuelle exposée hors debug (passe 2) : un
// reset relance un warmup complet (dosage bloqué), il ne doit pas être spammé.
constexpr uint32_t kSensorFilterUiResetMinIntervalMs = 600000UL; // 10 min

// --- Latch EMI (persistance de l'inhibition) ---
// Un bruit de niveau EMI SOUTENU (dispersion > cap sur N fenêtres pleines consécutives)
// pose un latch "instable" collant, levé UNIQUEMENT par reset()/recalibration. Distingue
// un pic de dispersion ponctuel (inhibition transitoire) d'un défaut EMI persistant.
constexpr uint8_t kSensorFilterDispersionLatchCycles = 3;    // Fenêtres en dépassement avant latch EMI
// Auto-levée du latch EMI (feature v2.8.4) : nombre de fenêtres PLEINES consécutives
// PROPRES (disp <= cap ET zéro rejet) requis avant de relâcher le latch collant.
// 12 fenêtres ≈ 60 s ; + la purge de la fenêtre médiane (~13 échantillons ≈ 65 s)
// → ≈ 2 min de signal propre avant reprise du dosage. Asymétrie VOULUE vs
// kSensorFilterDispersionLatchCycles=3 : rapide à poser (défaut EMI), lent à lever
// (anti-flapping — N < 6 rouvrirait le dosage sur une accalmie transitoire d'EMI).
// Même N pour pH et ORP.
constexpr uint8_t kSensorFilterLatchClearCycles  = 12;      // Fenêtres propres avant auto-levée latch EMI
// Âge max de la dernière mesure valide pour que le filtre soit considéré "prêt".
// Au-delà, ready() repasse false (mesure trop ancienne → fail-closed dosage).
// 4 × kPhOrpSensorIntervalMs (5 s) = 20 s, cohérent avec kSensorStaleTimeoutMs.
constexpr uint32_t kSensorFilterMaxAgeMs         = 20000;    // 20 s

// Trace capteur (ring buffer RAM, feature-057) : chaque cycle EZO enregistre un
// échantillon (pH brut/corrigé/lissé, ORP brut/lissé, T° eau).
// feature-067 : +1 champ float (phCorrected) → 300 × 28 o ≈ 8,4 Ko (+1,2 Ko).
// Fenêtre glissante ≈ 25 min à kPhOrpSensorIntervalMs (5 s). Diagnostic pur.
constexpr size_t kSensorTraceBufferSize          = 300;
// Pagination de GET /debug/sensor_trace (bug-crash-sensor-trace-badalloc) :
// 50 échantillons/page ≈ 4,5 Ko de JSON depuis feature-067 (champ phCorrected) —
// pas d'allocation contiguë démesurée dans AsyncResponseStream (le heap
// fragmenté peut descendre sous 8 Ko, ADR-0029) ; la garde measureJson +
// kSensorTraceHeapMarginBytes reste le filet en cas de heap trop fragmenté.
constexpr size_t kSensorTraceMaxSamplesPerPage   = 50;
// Marge de sécurité entre la taille mesurée de la réponse (measureJson) et le
// plus gros bloc libre 8 bits (heapLargestFreeBlock8Bit() depuis bug-080/
// v2.33.0 — la brute d'avant comptait l'IRAM) : en dessous → 503 propre au
// lieu d'un std::bad_alloc → abort() sur la tâche async_tcp.
// Alias historique (v2.10.1) de la marge commune kHttpHeapMarginBytes — même
// valeur, zéro diff comportemental (bug-crash-getlogs-badalloc).
constexpr size_t kSensorTraceHeapMarginBytes     = kHttpHeapMarginBytes;

// ============================================================================
// OFFSET LOGICIEL DE MESURE pH (feature-067)
// ============================================================================
// pH corrigé = pH brut EZO + phOffset, appliqué EN SORTIE des accesseurs
// (cf. src/ph_offset.h). La calibration EZO n'est jamais touchée.

// Bornes de l'offset — arbitrage utilisateur validé sous conditions par
// pool-chemistry. Justification chimique : l'offset décale la consigne EFFECTIVE
// (cible 7,20 avec offset +0,40 → l'eau réelle est régulée à 6,80). ±0,50 pH
// couvre un décalage d'implantation de sonde ; au-delà, le défaut vient de la
// sonde ou de la calibration EZO et l'écart pH régulé / pH réel devient
// dangereux (corrosion côté bas, chlore inactif côté haut).
constexpr float kPhOffsetMin = -0.50f;
constexpr float kPhOffsetMax =  0.50f;

// ============================================================================
// SEUILS DE SÉCURITÉ pH — SOURCE UNIQUE + SEUILS DÉRIVÉS (v2.25.0)
// ============================================================================
// Refonte « seuils fixes dérivés » : le plancher CONTINU asservi à l'offset
// (première mouture de la v2.25.0) est abandonné. Il était juste, mais trop
// complexe pour ce qu'il protégeait, et cette complexité a elle-même désactivé
// la régulation en production le 25/08/2026.
//
// Principe : UNE seule source de vérité (la plage de consigne + trois marges),
// dont TOUS les seuils dérivent en `constexpr`. Aucun seuil de sécurité pH n'est
// écrit en littéral, sauf les deux qui ont une justification chimique PROPRE
// (kPhOrpInhibit, kPhLowWarn) — et ceux-là sont commentés comme tels.
//
// RÉFÉRENTIEL — chaque seuil porte le sien dans son NOM, parce que confondre les
// deux est exactement le défaut qu'on corrige :
//   [CORRIGÉ] = pH brut + offset utilisateur (cf. ph_offset.h / ADR-0039). C'est
//               le pH RÉEL de l'eau : un seuil CHIMIQUE doit porter là-dessus.
//   [BRUT]    = mesure capteur SANS offset. Réservé aux deux BUTÉES de dernier
//               recours, qui doivent rester valides même si l'offset est FAUX.

// --- Plage de CONSIGNE pH (source unique) ---
// Fenêtre chimique d'exploitation d'un bassin, identique pour `ph_minus` et
// `ph_plus` : le type de produit choisit la POMPE à actionner, pas la fenêtre de
// pH visée — une eau à 7,80 est aussi mauvaise qu'elle ait été atteinte par
// excès de base ou par défaut d'acide.
//   - Borne BASSE 7,20 : sous cette valeur l'eau devient corrosive (attaque du
//     liner, des joints, des parties métalliques et du filtre).
//   - Borne HAUTE 7,50 : au-delà, la fraction d'acide hypochloreux (HClO, forme
//     active du chlore) s'effondre. 7,50 et NON 7,60 : à 7,60 pile, la consigne
//     elle-même atteindrait le seuil d'inhibition ORP (kPhOrpInhibit) — le
//     chlore serait suspendu EN PERMANENCE dès que la régulation tient sa
//     consigne, c'est-à-dire une panne sanitaire silencieuse. Le static_assert
//     kPhTargetMax + kPhOrpInhibitMargin <= kPhOrpInhibit interdit d'y revenir.
// Appliquées à TOUS les chemins d'écriture (HTTP, MQTT) et au chargement NVS.
constexpr float kPhTargetMin = 7.20f;
constexpr float kPhTargetMax = 7.50f;

// Pas de réglage des curseurs (consigne et offset), partagé UI / API / MQTT-HA
// et utilisé par roundPhOffsetToStep() pour la migration NVS de l'offset.
// 0,1 pH : sous le bruit résiduel du filtre (~0,05 pH), donc sans effet mesurable
// sur la régulation, et lisible par un humain.
constexpr float kPhTargetStep = 0.10f;
constexpr float kPhOffsetStep = 0.10f;

// --- Marges de sécurité (les seuils de garde en dérivent) ---
// kPhSafetyMargin — distance entre la consigne BASSE et le refus d'ACIDIFIER.
//   0,50 pH = inertie de l'acide encore « en vol » après l'arrêt de la pompe
//   (0,10-0,15 pH sur 30-50 m³) + bruit résiduel du filtre (~0,05 pH) + marge
//   d'erreur de mesure. La régulation ne doit JAMAIS buter sur ce seuil en
//   fonctionnement nominal : c'est ce qui a fermé la régulation le 25/08/2026.
// kPhHighMargin — distance entre la consigne HAUTE et le refus d'ALCALINISER.
//   ASYMÉTRIE ASSUMÉE (0,10 et non 0,50) : au-dessus de 7,6 la désinfection est
//   déjà compromise et l'inhibition ORP est active ; laisser le pH+ alcaliniser
//   0,50 pH plus haut reviendrait à autoriser le système à conduire lui-même
//   l'eau dans une impasse (chlore inactif, inhibition permanente).
//   0,20 (v2.25.0 initiale) violait cette règle qu'il énonçait : le plafond
//   tombait à 7,70, soit AU-DESSUS de kPhOrpInhibit — l'appareil autorisait
//   d'alcaliniser une eau dont il venait de décider que le chlore y était
//   inactif. Aucun argument physique ne soutient cette zone : remonter le TAC se
//   fait au bicarbonate, hors régulation, et jamais en visant un pH plus haut.
//   0,10 aligne le plafond sur l'inhibition (7,60) et rend l'invariant du filet
//   brut haut satisfiable — voir kPhCeilingRawHard.
// kPhOrpInhibitMargin — garde-fou STRUCTUREL entre la consigne haute et le seuil
//   d'inhibition ORP. Ne sert à aucun calcul de dosage : il n'existe que pour le
//   static_assert qui rend l'inhibition permanente IMPOSSIBLE à recompiler.
constexpr float kPhSafetyMargin     = 0.50f;
constexpr float kPhHighMargin       = 0.10f;
constexpr float kPhOrpInhibitMargin = 0.10f;

// Hystérésis de RELÂCHE commune à TOUTES les gardes pH (basses, hautes, butées
// brutes, inhibition ORP) : une garde déclenchée ne retombe qu'après 0,05 pH de
// retour FRANC dans le domaine sûr. Sans elle, un pH qui oscille sur un seuil
// (bruit du filtre) génère une rafale bloque/relâche → autant de refus
// comptabilisés → bascule fail-closed de la régulation sur un incident unique.
constexpr float kPhGuardReleaseHysteresis = 0.05f;

// --- Seuils à justification chimique PROPRE (non dérivés, assumés littéraux) ---
// Inhibition du dosage ORP sur pH élevé (pool-chemistry #3). Au-dessus de ~7,6 la
// fraction de HClO s'effondre (< 25 %) : l'ORP mesuré chute à concentration de
// chlore constante et la régulation ORP compense en SURDOSANT. On suspend donc le
// dosage ORP — le vrai correctif est de faire baisser le pH.
// v2.25.0 : comparé au pH [CORRIGÉ] et non plus au brut. La fraction de HClO
// dépend du pH RÉEL de l'eau, pas de la lecture non corrigée du capteur.
// Relâche à ≈ 7,55 (kPhOrpInhibit − kPhGuardReleaseHysteresis). « ≈ » car en
// float l'expression vaut 7,5499997 : à 7,55 pile la garde bloque encore. Écart
// 3e-7 pH, cinq ordres sous le bruit du filtre, et il penche du côté bloquant —
// fail-closed. Ne JAMAIS écrire le littéral 7.55f à la place de l'expression.
constexpr float kPhOrpInhibit = 7.60f;
// Alerte NON BLOQUANTE d'eau acide, sur le pH [CORRIGÉ]. Signal PRÉCOCE conservé
// (condition du gate) malgré l'abaissement du seuil de blocage à 6,70 : sous 7,0
// l'eau commence à être agressive pour l'installation bien avant qu'il faille
// refuser de doser. N'incrémente aucun compteur, ne bloque rien.
constexpr float kPhLowWarn = 7.00f;

// --- Seuils DÉRIVÉS (jamais de littéral) ---
// Refus d'ACIDIFIER (produit `ph_minus` uniquement) sur le pH [CORRIGÉ].
constexpr float kPhFloorCorrected   = kPhTargetMin - kPhSafetyMargin;    // 6,70
// Refus d'ALCALINISER (produit `ph_plus` uniquement) sur le pH [CORRIGÉ].
// Aligné sur kPhOrpInhibit par construction (static_assert ci-dessous) : on ne
// peut pas alcaliniser au-delà du point où l'on a décidé que le chlore est
// inactif.
constexpr float kPhCeilingCorrected = kPhTargetMax + kPhHighMargin;      // 7,60
// BUTÉE BASSE sur le pH [BRUT] : ultime filet contre un offset POSITIF saisi à
// tort (la régulation croit doser vers 7,20 alors que l'eau est déjà sous 6,7).
// Dérivée de l'offset MAXIMAL, elle ne peut structurellement JAMAIS mordre avant
// kPhFloorCorrected en usage légitime : 6,70 − 0,50 = 6,20. Une butée plus haute
// (6,40 avait été proposée) bloquerait AVANT le seuil corrigé pour un offset
// légitime de +0,50, rendant celui-ci inopérant — le static_assert l'interdit.
constexpr float kPhFloorRawHard     = kPhFloorCorrected - kPhOffsetMax;  // 6,20
// BUTÉE HAUTE sur le pH [BRUT] : filet symétrique contre un offset NÉGATIF saisi
// à tort, qui masquerait une eau réellement au-dessus de 7,6 et laisserait la
// régulation ORP surdoser le chlore. Dérivée de l'offset maximal :
// 7,60 + 0,50 = 8,10.
// DEUX POINTS D'APPLICATION, quel que soit le type de produit déclaré :
//   (a) inhibition du dosage ORP (isOrpInhibitedByPh) — le chlore est suspendu ;
//   (b) refus d'ALCALINISER (isPhGuardBlocking, produit `ph_plus`), symétrique de
//       la butée brute basse appliquée au refus d'acidifier. Ajouté à la
//       contre-visite de la v2.25.0 : sans (b), un offset −0,50 erroné laissait le
//       pH+ pousser l'eau au-delà de 8,10 BRUT pendant que (a) inhibait le
//       chlore — l'appareil conduisait lui-même l'eau dans une impasse
//       sanitaire. Ce refus alimente le compteur des 3 refus / 24 h en mode
//       automatique, comme la butée basse.
// Avec kPhHighMargin = 0,10 la butée brute haute coïncide exactement avec le
// plafond corrigé au pire cas d'offset (8,10 brut ⟺ 7,60 corrigé à offset
// −0,50), relâches comprises (8,05 ⟺ 7,55) : elle ne mord donc JAMAIS avant lui.
// C'est la symétrie de l'exigence posée sur la butée basse, et elle est
// satisfaite sans nouvelle constante — voir le static_assert dédié.
// Avec un produit `ph_minus`, (b) est sans objet (l'acide ne peut pas faire
// monter le pH) : seule (a) s'applique — d'où « quel que soit le produit ».
constexpr float kPhCeilingRawHard   = kPhOrpInhibit + kPhOffsetMax;      // 8,10

// --- Invariants verrouillés à la compilation ---
// Toute retouche d'une valeur ci-dessus doit relire la justification chimique,
// pas seulement compiler.
static_assert(kPhTargetMin < kPhTargetMax,
              "Bornes de consigne pH inversees");
// (ii) LE verrou du gate : interdit structurellement qu'une eau maintenue à la
// consigne haute inhibe la régulation ORP en permanence.
static_assert(kPhTargetMax + kPhOrpInhibitMargin <= kPhOrpInhibit,
              "Consigne pH haute trop proche de kPhOrpInhibit : le chlore serait "
              "inhibe en permanence a la consigne (panne sanitaire silencieuse)");
static_assert(kPhSafetyMargin > 0.0f && kPhHighMargin > 0.0f &&
                  kPhOrpInhibitMargin > 0.0f,
              "Les marges de securite pH doivent etre strictement positives");
// (iv) La butée brute basse ne doit JAMAIS mordre avant le seuil corrigé, pour
// tout offset du domaine autorisé. Écrit en INÉGALITÉ : remonter la butée (6,40)
// ne compile plus, l'abaisser reste possible.
static_assert(kPhFloorRawHard <= kPhFloorCorrected - kPhOffsetMax,
              "Butee brute basse trop haute : elle mordrait avant le seuil "
              "corrige pour un offset legitime, rendant celui-ci inoperant");
static_assert(kPhFloorCorrected < kPhLowWarn && kPhLowWarn <= kPhTargetMin,
              "kPhLowWarn doit alerter AVANT le refus de dosage et rester sous "
              "la consigne minimale");
static_assert(kPhGuardReleaseHysteresis > 0.0f &&
                  kPhGuardReleaseHysteresis < kPhHighMargin,
              "Hysteresis de relache nulle (rafale de refus) ou plus large que "
              "la plus petite marge (relache impossible)");
static_assert(kPhFloorRawHard > 0.0f,
              "Butee brute basse hors de l'echelle pH");
// (viii) Le plafond d'alcalinisation ne peut JAMAIS dépasser le seuil
// d'inhibition ORP : au-delà, l'appareil autoriserait le pH+ à pousser une eau
// dont il a déjà décidé que le chlore y était inactif — il conduirait lui-même
// l'eau dans l'impasse sanitaire que l'inhibition signale. Verrou DÉDIÉ : celui
// de kPhOrpInhibitMargin ne protège que la CONSIGNE, pas le plafond.
static_assert(kPhCeilingCorrected <= kPhOrpInhibit,
              "Plafond d'alcalinisation au-dessus de kPhOrpInhibit : le systeme "
              "pourrait alcaliniser une eau dont il a inhibe le chlore");
// (ix) Symétrique de (iv) : la butée brute haute ne doit jamais mordre avant le
// plafond corrigé, pour tout offset du domaine. Le pire cas est l'offset MINIMAL
// (négatif), qui abaisse le pH corrigé à brut donné. La tolérance absorbe l'ulp
// entre deux expressions flottantes distinctes qui valent la même chose en
// décimal (piège identique au littéral 7,55 de la relâche).
static_assert(kPhCeilingRawHard >= kPhCeilingCorrected - kPhOffsetMin - 1e-4f,
              "Butee brute haute trop basse : elle mordrait avant le plafond "
              "corrige pour un offset legitime, rendant celui-ci inoperant");

// --- Diagnostic EZO : fenêtre de « sonde saine » (v2.25.0) ---
// Sert UNIQUEMENT à nuancer le message d'escalade (3 refus / 24 h) : une sonde
// dans cette fenêtre n'est pas en cause, il faut chercher un décalage de
// référence externe (courants de fuite de la pompe de filtration, campagne de
// bruit 22-23/08/2026). Aucune décision de dosage ne dépend de ces bornes.
// Pentes : ±10 % autour du Nernst théorique (tolérance Atlas Scientific pour une
// sonde en bon état). Zéro : 15 mV ≈ 0,25 pH, sous le plus petit décalage
// INTERNE qui justifierait une recalibration.
constexpr float kPhSlopeHealthyMinPct = 90.0f;
constexpr float kPhSlopeHealthyMaxPct = 110.0f;
constexpr float kPhZeroHealthyMaxMv   = 15.0f;

// --- Compteur de refus des gardes pH (pool-chemistry #7) ---
// Une garde pH qui se déclenche à répétition signe une anomalie durable (sonde
// morte, offset saisi à l'envers, produit mal déclaré), pas un incident
// ponctuel : log critical explicite + bascule fail-closed de la régulation pH en
// manuel. v2.25.0 : SEULS les refus survenus en mode `automatic` sont comptés —
// un dosage PROGRAMMÉ refusé est signalé mais ne ferme pas la régulation.
constexpr uint8_t  kPhGuardRefusalMax      = 3;           // Refus tolérés dans la fenêtre
constexpr uint32_t kPhGuardRefusalWindowMs = 86400000UL;  // 24 h — fenêtre glissante

// Débit maximal de réémission de l'alerte « dosage pH programmé refusé ».
// Le mode Programmée n'escalade pas (il ne compte pas ses refus) : sans cette
// réémission périodique, un refus permanent serait un no-op SILENCIEUX après la
// première transition. Une heure = visible dans la journée, jamais spammé.
constexpr uint32_t kPhGuardScheduledAlertIntervalMs = 3600000UL;  // 1 h

// Taille du tampon FIXE de la cause de refus de dosage exposée à /data et au
// WebSocket (`_lastRefusalCause`, un tampon par pompe).
//
// POURQUOI UN TAMPON FIXE ET NON UNE `String` (revue v2.25.0) : la cause est
// écrite par loopTask (surveillance, escalade, canDose) et LUE depuis
// `async_tcp` (route /data). Copier une `String` réassignée par une autre tâche
// lit `buffer`/`len` pendant que l'écrivain peut libérer/réallouer le tas :
// lecture de mémoire libérée, potentiellement fatale. Un tampon fixe, membre de
// l'objet, ne peut PAS être libéré : le pire cas devient une lecture DÉCHIRÉE,
// soit un message d'affichage partiellement à jour, sans conséquence.
//
// DIMENSIONNEMENT — mesuré sur les messages RÉELLEMENT produits (inventaire des
// 23 causes de canDose(), des gardes pH/ORP et de l'escalade). Pire cas en
// OCTETS UTF-8 (les accents comptent double) :
//   64 o  « pH brut au-dessus de la butée (7.85) — dosage chlore suspendu »
//   62 o  « régulation pH basculée en manuel (refus de garde répétés) »
//   58 o  « pH brut au-dessus de la butée de sécurité (7.85 > 7.60) »
// Marge des valeurs formatées : ces messages portent 1 à 2 `String(float, 2)`.
// Une valeur nominale tient en 4 caractères (« 7.85 ») mais une mesure aberrante
// peut en prendre 8 (« -1234.57 ») → +8 o sur le message à deux flottants, soit
// 72 o au pire. 96 octets laissent donc ≥ 24 o d'entête libre, pour un coût de
// 2 × 96 = 192 octets statiques (contre 2 objets String + leurs allocations tas).
// La troncature éventuelle est faite SANS couper un caractère UTF-8 multi-octets
// (cf. utf8ClampInPlace dans utf8_clamp.cpp, feature-129) : un octet de
// continuation orphelin produirait un JSON non décodable côté client.
constexpr size_t kDoseBlockedReasonMaxLen = 96;

// Code de cause de refus exposé À CÔTÉ du texte (v2.25.1) : `phDoseBlockedCode` /
// `orpDoseBlockedCode` sur /data et le WebSocket. Le client traduit un CODE, il
// ne cherche plus des fragments de texte dans une phrase française (l'ancien
// procédé faisait matcher « indispo » de « lecture filtrée indisponible » avec
// « EZO indisponible » : cause fausse affichée sur une sonde saine).
//
// Les valeurs 0..22 sont celles de `DoseRefusal` (dosing_logic.h, contrat de
// sérialisation figé). Les deux sentinelles ci-dessous couvrent les causes qui
// ne viennent PAS de canDose() :
//   kDoseBlockedCodeNone  = dosage non refusé (le champ est publié `null`) ;
//   kDoseBlockedCodeGuard = cause posée par une GARDE de sécurité pH/ORP ou par
//     l'escalade (butée brute, pH trop élevé, bascule en manuel…). Ces messages
//     portent des valeurs runtime et ne sont pas énumérables : le client se
//     rabat sur le TEXTE, qui reste publié en parallèle.
// Valeur 250 volontairement éloignée de la fin de DoseRefusal : de nouvelles
// causes pourront s'ajouter à l'énum sans jamais heurter la sentinelle.
constexpr uint8_t kDoseBlockedCodeNone  = 0;
constexpr uint8_t kDoseBlockedCodeGuard = 250;

// Durée depuis laquelle le code de refus COURANT est inchangé, par voie
// (feature-138) : `phDoseBlockedForS` / `orpDoseBlockedForS`, publiés sur
// /data ET le WebSocket (mêmes clés, parité obligatoire). Unité : la seconde.
// `null` quand aucun refus (même convention que *DoseBlockedCode) — couvre
// aussi bien « plus de blocage » que « jamais dosé depuis le boot ».
// ⚠️ Budget d'octets (ADR-0073) : c'est ce CLAMP qui fixe le nombre de
// chiffres décimaux (5, donc "86400" sur 5 caractères), PAS le type C sous-jacent
// (un uint32_t pourrait aller jusqu'à 4294967295, 10 chiffres). Le prochain
// lecteur qui recompte le pire cas de la trame doit se baser sur ce clamp, pas
// sur la borne du type — sinon il retombe sur 10 chiffres et un faux dépassement.
constexpr uint32_t kDoseBlockedForMaxS = 86400;  // 24h - plafond de durée affichée ("≥ 24 h" au-delà, côté UI)

// Persistance exigée avant d'escalader une cause de refus de type PANNE DE
// MESURE (sonde figée, plus aucune mesure acceptée) en `warning` + alerte MQTT.
// POURQUOI UNE TEMPORISATION : à la réouverture de la porte d'alimentation du
// filtre (reprise de circulation, fin de purge), le filtre est ré-amorcé — sa
// fenêtre est vide et l'âge de la dernière mesure ACCEPTÉE est encore celui
// d'avant la fermeture. Pendant au plus un cycle de lecture EZO (5 s), l'état
// interne est donc légitimement « périmé » alors que la sonde va très bien.
// Sans temporisation, chaque démarrage de filtration produirait une alerte
// « sonde muette » — un cri au loup quotidien qui décrédibiliserait l'alerte
// réelle. 60 s = 12 cycles de lecture : aucune panne réelle n'est masquée
// (l'affichage, lui, montre la cause SANS délai).
// Depuis la revue v2.25.1, ce transitoire est ÉGALEMENT traité à la source par la
// grâce de ré-amorçage de SensorFilter::stale() (l'affichage ne clignote plus en
// rouge au démarrage). Cette temporisation reste en défense en profondeur : elle
// couvre tout autre transitoire et borne l'épisode à une panne PERSISTANTE. Elle
// s'applique à un épisode de panne CONTINUE, la nature de la panne pouvant varier
// à l'intérieur de l'épisode (cf. _sensorFaultActive, pump_controller.h).
constexpr uint32_t kDoseSensorFaultAlertDelayMs = 60000UL;

// Pause mélange hydraulique après injection (pool-chemistry feature-025).
// Distincte du timer post-calibration (_stabilizationEndMs). Gates indépendantes (OR).
// Empêche un surdosage avant homogénéisation du bassin. Gérée par timestamps (pas de delay()).
constexpr unsigned long kPhMixingDelayMs         = 900000UL;  // 15 min — pause mélange pH
constexpr unsigned long kOrpMixingDelayMs        = 1200000UL; // 20 min — pause mélange ORP

// ============================================================================
// SEUILS D'HYSTÉRÉSIS pH — PROMUS EN CONSTANTES (feature-094, condition C9)
// ============================================================================
// Ces deux valeurs vivaient UNIQUEMENT comme initialiseurs de PumpProtection
// (config.h). Vérification faite avant promotion : elles ne sont JAMAIS relues
// depuis la NVS ni exposées par l'API (/get-config, /save-config) — ce sont donc
// de facto des constantes de compilation, et non des réglages vivants. Les
// promouvoir ici permet de VERROUILLER par static_assert la cohérence entre la
// largeur de la bande de décision et le bruit résiduel du signal lent, au lieu
// d'une garde d'exécution qui ne protégerait rien.
//
// Piège documenté du projet (« config d'usine ≠ config vivante ») : si un jour
// ces seuils devenaient réglables, ces static_assert cesseraient de faire foi et
// il faudrait leur substituer un bornage à l'enregistrement.
//
// RÔLE, INCHANGÉ depuis v2.26.0 (condition C3 du gate feature-094) :
//   - kPhStartThresholdDefault : seuil d'OUVERTURE de session ET bande morte de
//     computePidPure session fermée ;
//   - kPhStopThreshold : seuil de FERMETURE, bande morte session ouverte, et
//     CIBLE de l'intégrande `(erreur − phStopThreshold)` (ph_integral_logic.h).
// Le point d'équilibre du bassin reste `consigne + kPhStopThreshold` : la porte
// de session robuste au bruit ne le déplace pas d'un millième de pH.
constexpr float kPhStartThresholdDefault = 0.05f;  // pH — démarrage de session
constexpr float kPhStopThreshold         = 0.01f;  // pH — arrêt de session / cible du PI

// Le signal lent de décision (feature-094) a un bruit résiduel σ ≈ 0,020 après
// moyennage sur kPhDecisionWindowMs. Une bande de décision plus étroite que 2σ
// serait de nouveau franchie par le seul bruit — c'est exactement le défaut que
// la feature corrige.
static_assert(kPhStartThresholdDefault - kPhStopThreshold >= 0.04f,
              "Bande de decision sous le bruit residuel du signal lent (sigma 0,020)");
// Un seuil d'arrêt nul rendrait le point fixe de l'intégration inatteignable ;
// au-delà de 0,05 il vaudrait le seuil de démarrage (hystérésis dégénérée).
static_assert(kPhStopThreshold > 0.0f && kPhStopThreshold <= 0.05f,
              "Seuil d'arret pH hors plage exploitable (0 < stop <= 0,05)");

// ============================================================================
// RÉGULATION pH — TERME INTÉGRAL (v2.26.0, gate pool-chemistry GO/conditions)
// ============================================================================
// LEVÉE de `Ki = 0` (feature-025) sur le SEUL pH. Ki_ORP reste 0 (hors périmètre)
// et Kd reste 0 PARTOUT (un dérivé amplifierait le bruit résiduel du filtre).
//
// POURQUOI : sous charge basique permanente (~325 g CO₂/j, 80 % de la charge), un
// P pur ne peut pas rejoindre sa consigne — il se stabilise sur son droop
// `e* = besoin / (kp × T)`. Vérification terrain (août 2026) : kp = 8, e* = 0,358
// ⇒ 2,86 mL/min ⇒ 1 800 mL/j pour 10,5 h de filtration ; 1 000 mL réellement
// injectés, pH TENU à 7,50-7,66 pour une consigne à 7,30. Le pH ne POUVAIT pas
// descendre : ce n'était ni la pompe, ni le produit, ni le dosage, mais la
// structure même d'un correcteur proportionnel.
//
// DIMENSIONNEMENT PAR L'AUTORITÉ, jamais « en pH·s à l'intuition » : on pose le
// débit maximal que l'intégrale seule est autorisée à demander, puis on en déduit
// integralMax = U_I,max / ki. À l'équilibre l'erreur tend vers 0, donc le terme P
// disparaît et l'intégrale fournit TOUTE la charge : son autorité doit couvrir la
// charge d'équilibre, avec un facteur 2 pour la dérive saisonnière.
//
//   vitesse | kp | Ti      | ki = kp/Ti | integralMax = U/ki
//   --------|----|---------|------------|-------------------
//   slow    |  4 | 8 h     | 1,39e-4    | 43 200 pH·s
//   normal  |  8 | 6 h     | 3,70e-4    | 16 200 pH·s
//   fast    | 12 | 4 h     | 8,33e-4    |  7 200 pH·s
//
// integralMax VARIE donc avec `regulation_speed`, exactement comme kp : l'autorité
// est constante (6 mL/min), seule la VITESSE de charge de l'intégrale change.
//
// LE PIÈGE À NE PAS REFAIRE : l'ancien `integralMax = 50.0f` (inerte tant que
// Ki valait 0) plafonnait la contribution intégrale à 3,70e-4 × 50 = 0,0185 mL/min,
// soit 0,6 % du besoin de 2,86 mL/min — saturé en 140 s. Réactiver Ki sans
// redimensionner integralMax serait un ÉCHEC SILENCIEUX : régulation « avec
// intégrale » sur le papier, droop intact dans le bassin.
//
// Ti = 2 à 3× le temps de brassage du bassin (4-6 h). Le retard de mélange
// (kPhMixingDelayMs = 15 min) est négligeable devant Ti — verrouillé ci-dessous.

// Autorité MAXIMALE du terme intégral seul (mL/min de débit moyen demandé).
// 2× la charge d'équilibre mesurée (2,86 mL/min). Toute retouche de cette valeur
// est une décision de SÉCURITÉ CHIMIQUE : elle fixe le débit qu'un capteur figé
// haut pourrait demander en permanence (borné par ailleurs par le gel d'intégrale
// sur quota journalier, les limites horaire/journalière et la pause mélange).
constexpr float kPhIntegralAuthorityMlPerMin = 6.0f;

// Constantes de temps d'intégration par vitesse (secondes).
constexpr float kPhIntegralTiSlowS   = 28800.0f;  // 8 h
constexpr float kPhIntegralTiNormalS = 21600.0f;  // 6 h
constexpr float kPhIntegralTiFastS   = 14400.0f;  // 4 h

// Kp pH par vitesse — externalisés du code métier : integralMax en DÉRIVE, les
// deux ne peuvent donc plus divorcer silencieusement.
constexpr float kPhKpSlow   = 4.0f;
constexpr float kPhKpNormal = 8.0f;
constexpr float kPhKpFast   = 12.0f;

// Ki = Kp / Ti (mL/min par pH·s).
constexpr float kPhKiSlow   = kPhKpSlow   / kPhIntegralTiSlowS;    // 1,389e-4
constexpr float kPhKiNormal = kPhKpNormal / kPhIntegralTiNormalS;  // 3,704e-4
constexpr float kPhKiFast   = kPhKpFast   / kPhIntegralTiFastS;    // 8,333e-4

// integralMax = U_I,max / ki (pH·s) — écrit sous la forme U × Ti / Kp pour rester
// exact en flottant (pas de division par une division).
constexpr float kPhIntegralMaxSlow =
    kPhIntegralAuthorityMlPerMin * kPhIntegralTiSlowS   / kPhKpSlow;    // 43 200
constexpr float kPhIntegralMaxNormal =
    kPhIntegralAuthorityMlPerMin * kPhIntegralTiNormalS / kPhKpNormal;  // 16 200
constexpr float kPhIntegralMaxFast =
    kPhIntegralAuthorityMlPerMin * kPhIntegralTiFastS   / kPhKpFast;    //  7 200

// --- C2 : marge du RESET SEC de l'intégrale ---------------------------------
// L'ancien code remettait l'intégrale à zéro dès `error < -phStopThreshold`
// (−0,01) : six heures d'accumulation effacées par 0,011 pH de dérive, à chaque
// tick. Sans cette marge, la levée de Ki n'aurait AUCUN bénéfice.
// Le seuil devient `error < -(phStartThreshold + kPhIntegralResetMargin)`, soit
// −0,15 avec les défauts (0,05 + 0,10) : une excursion FRANCHE de l'autre côté de
// la consigne (surcorrection réelle, changement de produit, apport externe), et
// non le bruit du filtre (~0,05 pH).
constexpr float kPhIntegralResetMargin = 0.10f;

// --- feature-084 (sujet 2) : le reset sec est CONSERVÉ mais TEMPORISÉ --------
// Le gate du 30/08 le conserve — c'est le seul mécanisme RAPIDE sur une inversion
// de sens (bascule ph_minus ↔ ph_plus, apport externe massif, recalibration) :
// l'oubli à demi-vie 1 h met environ 3 h à vider 8 400 pH·s, ce qui est trop lent
// pour une erreur qui a franchement changé de signe.
//
// Mais un reset TOTAL sur un transitoire de −0,15 (une pluie suffit) efface deux
// jours et demi d'apprentissage. L'excursion doit donc être CONTINUE pendant
// N minutes de temps de BRANCHE (même base que la charge : un trou d'évaluation
// n'est jamais crédité), le compteur repartant de zéro dès la moindre remontée
// au-dessus du seuil.
//
// CE DIFFÉRÉ N'EST PAS UNE PRISE DE RISQUE : pendant l'attente, `error < 0` fait
// tourner l'OUBLI exponentiel et la décharge signée. La protection est donc
// GRADUÉE (−16 % d'intégrale sur les 15 minutes), jamais suspendue ; seule la
// purge INSTANTANÉE est différée le temps de vérifier que l'excursion est réelle.
//
// 15 min = la pause mélange (kPhMixingDelayMs) : en deçà, l'excursion mesurée
// peut n'être que la dose précédente pas encore homogénéisée.
constexpr uint32_t kPhIntegralResetHoldMs = 900000UL;  // 15 min de temps de branche

// Demi-vie de l'OUBLI progressif de l'intégrale pendant une excursion sous la
// consigne (option 2 du gate, retenue — cf. le commentaire C2 de
// pump_controller.cpp pour la justification complète). Nécessaire parce que
// l'intégrale ne se décharge PAS d'elle-même : le PID n'est évalué que si
// `shouldDose`, donc plus du tout dès que l'erreur devient négative. Sans oubli,
// une intégrale pleine survivrait à une semaine de pH sous consigne (bassin
// bâché, charge basique disparue) et serait restituée d'un bloc au premier retour
// au-dessus du seuil de démarrage.
// 1 h = ordre de grandeur du brassage du bassin, six fois plus court que le Ti le
// plus rapide : l'oubli est franc devant la charge, sans jamais concurrencer la
// dynamique de régulation (il n'agit QUE hors dosage).
constexpr uint32_t kPhIntegralDecayHalfLifeMs = 3600000UL;  // 1 h

// Mise en œuvre PAR PAS FIXES plutôt que par powf(0,5 ; dt/T½) : la fonction
// powf() coûte 2 320 octets de FLASH mesurés (linkage de la bibliothèque
// mathématique), sur une ressource occupée à 97 %. La demi-vie est donc obtenue
// par répétition d'un facteur constant, exact au même titre — et vérifié à la
// COMPILATION ci-dessous.
constexpr uint32_t kPhIntegralDecayStepMs = 60000UL;  // 1 min par pas
// 0,5^(1/60) — un pas d'une minute, soixante pas par demi-vie.
constexpr float kPhIntegralDecayStepFactor = 0.9885140f;
// Rattrapage borné : au-delà de 10 demi-vies il ne reste que 0,1 % de
// l'intégrale, le plancher de 1 pH·s a déjà fermé l'épisode. Borne la boucle de
// rattrapage d'update() quand la branche automatique n'a pas tourné pendant des
// heures (600 multiplications flottantes = quelques dizaines de µs).
constexpr uint32_t kPhIntegralDecayMaxSteps = 600;

// --- Intégration SIGNÉE : cadence et quantum --------------------------------
// v2.26.0 (correction) : l'intégrale est désormais entretenue à CHAQUE tick
// automatique, sur `(erreur − phStopThreshold)` — cf. src/ph_integral_logic.h.
//
// Deux garde-fous de mise en œuvre, tous deux NUMÉRIQUES :
//
// (1) dt MAXIMAL crédité. Même valeur et même raison que la garde de computePID :
//     un trou d'évaluation (pause mélange de 15 min, garde bloquante, changement
//     de mode) ne doit JAMAIS être crédité d'un bloc — 900 s × erreur chargeraient
//     l'intégrale au plafond en un seul tour. Au-delà, le temps est REJETÉ.
constexpr uint32_t kPhIntegralMaxTickDtMs = 10000UL;  // 10 s
//
// (2) QUANTUM d'intégration. La branche automatique tourne à la cadence de loop
//     (quelques millisecondes). À 16 200 pH·s, l'ulp du flottant simple précision
//     vaut ~2e-3 : un incrément de 5 ms × 0,01 pH (5e-5) serait PERDU à
//     l'addition, et la décharge en bande morte n'existerait tout simplement pas.
//     On accumule donc le temps observé et on n'intègre que par quanta d'une
//     seconde — incrément minimal 1e-2 pH·s, très au-dessus de l'ulp. Le reliquat
//     sous le quantum est reporté : aucun temps n'est perdu.
constexpr uint32_t kPhIntegralChargeQuantumMs = 1000UL;  // 1 s

// --- C3 : gel d'intégrale sur quota journalier -------------------------------
// Fraction de la limite journalière au-delà de laquelle l'accumulation intégrale
// est GELÉE. Couvre le trou du gel existant : une sonde bloquée sur une valeur
// HAUTE (plage et fraîcheur valides — le filtre la croit bonne) présente une
// erreur constante que rien ne résorbe ; l'intégrale chargerait jusqu'à son
// plafond et y resterait. Le volume déjà injecté est le seul témoin fiable de
// cette divergence : à 80 % du quota, on cesse d'accumuler (le terme acquis reste
// servi, on n'en ajoute plus).
constexpr float kPhIntegralFreezeDailyFraction = 0.80f;

// --- C4 : péremption ---------------------------------------------------------
// Purge de l'intégrale quand le PID n'a pas été évalué depuis 24 h (mode `off` /
// `manual`, garde bloquante durable, filtration arrêtée tout l'hiver). Sans elle
// l'intégrale resterait figée des jours, puis serait servie telle quelle au
// retour en automatique, sur un bassin devenu étranger à la mesure qui l'a
// chargée.
constexpr uint32_t kPhIntegralStaleResetMs = 86400000UL;  // 24 h

// ============================================================================
// feature-084 (sujet 3) — PERSISTANCE NVS de l'intégrale pH
// ============================================================================
// L'ADR-0052 l'INTERDISAIT (« ne jamais persister — une valeur stockée serait une
// autorisation de dosage PRÉ-CHARGÉE au boot »). Le gate pool-chemistry du
// 30/08/2026 ASSOUPLIT cette interdiction, l'ADR étant amendé en conséquence :
// un redémarrage coûtait 2,4 jours de convergence, et le PANIC du 29/08
// (bug-080) n'est pas élucidé.
//
// L'assouplissement est conditionné à SEPT garanties CUMULATIVES. Aucune n'est
// facultative : retirée isolément, chacune ramène l'objection de l'ADR intacte.
//
//   R1 — PLAFOND CHIMIQUE de restauration (ci-dessous) ;
//   R2 — INVALIDATION CONTEXTUELLE : CRC + schéma + vitesse de régulation + type
//        de correction + consigne ; toute différence ⇒ zéro ;
//   R3 — PÉREMPTION 6 h (= 1 Ti), pas 24 h ;
//   R4 — CONFIRMATION PAR LA MESURE : restauration DIFFÉRÉE ;
//   R5 — ÉCRITURE bornée (usure NVS) ;
//   R6 — TRAÇABILITÉ (info à la restauration, warn au rejet) ;
//   R7 — ANTI-BOUCLE DE BOOT.
//
// R1 — PLAFOND CHIMIQUE. VERSION CORRIGÉE (gate pool-chemistry, réexamen du
// 30/08/2026 après le test signalant une marge étroite).
//
// CE QUE LE GATE A ÉTABLI. Le remplissage d'ÉQUILIBRE de l'intégrale vaut
//     I* = (charge − kp × stopThreshold) / ki
// soit, sur l'installation de référence, (2,86 − 0,08) / 3,704e-4 = 7 505 pH·s
// = 46,3 % du plafond. C'est EXACTEMENT la frontière convergence / cycle limite
// observée en simulation entre 30 % et 45 % : en dessous de I* la boucle finit
// de charger (sens sûr) ; au-dessus, elle ne peut décharger qu'à 0,013 mL/min
// par heure — d'où les six jours sans retour. Les 30 % valent donc 0,65·I* :
// la marge réelle est de 35 %, et non de 50 %.
//
// LE DÉFAUT DE FORMULATION, BLOQUANT POUR LA FLOTTE. Écrit en fraction du
// plafond, l'équilibre vaut f* = (charge − kp × stop) / autorité, soit
// (charge − 0,08) / 6. L'égalité f* = 30 % se produit à charge = 1,88 mL/min.
// TOUT BASSIN SOUS 1,88 mL/min — plus petit, bâché, TAC bas, acide plus
// concentré, hors saison — voit le plafond forfaitaire de 30 % passer AU-DESSUS
// de son propre équilibre, et l'argument « aucune restauration ne peut produire
// un surdosage net » s'effondre. À 1,5 mL/min, f* = 23,7 % : 30 % restaurerait
// 1,27× l'équilibre. Aggravant : un bassin deux fois plus petit a un gain pH
// double, donc des excursions doublées. Chiffrage du risque : si la charge
// disparaît, l'autorité parquée continue de doser pendant les 15 minutes
// AVEUGLES de la pause mélange ⇒ excursion acide de 0,19 pH au plafond plein
// (0,07 à 30 %), à comparer à kPhSafetyMargin = 0,50 — le plafond plein
// consommerait 38 % de la garde en un seul temps mort.
//
// LE PLAFOND RETENU est donc le MINIMUM de deux termes :
//   plafond = min( 0,30 × integralMax ,
//                  0,50 × (charge_OBSERVÉE − kp × stopThreshold) / ki )
// Le premier reste une BUTÉE ABSOLUE, jamais dépassée. Le second est la
// vérification À L'EXÉCUTION que le static_assert (xi) ne pouvait pas faire :
// il est calculé sur la charge RÉELLEMENT observée de CE bassin, pas sur celle
// de l'installation de référence. Sur la référence il rend 3 753 pH·s = 23 %
// (1,39 mL/min) au lieu de 30 % (1,80 mL/min).
//
// FAIL-CLOSED : la charge observée est la médiane des volumes pH journaliers sur
// 7 jours, rapportée aux minutes de filtration du jour. Moins de 3 jours
// d'historique exploitable ⇒ AUCUNE restauration. Un appareil neuf ne restaure
// donc rien, ce qui est le comportement voulu.
// ⚠️ CE CHIFFRE EST UNE DEMANDE, PAS UN VOLUME DÉLIVRÉ — distinction qui a déjà
// coûté une conclusion erronée en revue (v2.27.0). La rédaction précédente disait
// « charge MESURÉE », ce qui laissait croire à un relevé de pompe.
//
//   DEMANDE à l'équilibre (ci-dessous, 2,86 mL/min) : le BESOIN de neutralisation
//   de l'installation de référence, ~1 800 mL/j rapportés aux minutes de
//   filtration. C'est ce que la boucle CHERCHERAIT à injecter si rien ne la
//   bornait ; c'est la grandeur qui fixe le remplissage d'équilibre I*.
//
//   DÉLIVRÉ (ce que l'anneau de charge observe, cf. plus bas) : `dailyPhInjectedMl`
//   rapporté aux minutes de circulation — donc ce qui SORT réellement de la pompe,
//   structurellement borné par la limite horaire d'injection × le débit
//   d'amplitude. Sur la référence à 5 min/h, ce plafond vaut ~2,17 mL/min : le
//   délivré ne peut PAS atteindre 2,86, et le déficit demande/délivré est
//   précisément ce que le gate propose de combler en relevant la limite horaire.
//
// Les deux grandeurs ne sont donc jamais comparables directement, et le filet de
// vraisemblance de l'anneau ne s'applique qu'à la SECONDE.
constexpr float kPhEquilibriumLoadMlPerMin = 2.86f;  // DEMANDE à l'équilibre
                                                     // (2026-08), réf.
constexpr float kPhIntegralRestoreMaxFraction = 0.30f;  // butée absolue
// Fraction de l'équilibre OBSERVÉ que l'on s'autorise à restaurer. 0,50 place la
// restauration à mi-chemin de I*, dans la zone où la boucle converge (le sens
// sûr), avec une marge de 50 % au lieu des 35 % du forfait.
constexpr float kPhIntegralRestoreLoadFraction = 0.50f;
// Fenêtre et exigence minimale d'historique pour que la charge observée fasse
// foi. Sous le minimum, le plafond vaut ZÉRO (fail-closed) : mieux vaut
// réapprendre en 2,4 jours que restaurer au-dessus de l'équilibre d'un bassin
// qu'on n'a pas encore mesuré.
constexpr uint8_t kPhIntegralRestoreHistoryWindowDays = 7;
constexpr uint8_t kPhIntegralRestoreMinHistoryDays    = 3;

// v2.27.0 — L'HISTORIQUE EXISTE ENFIN : anneau NVS de 7 journées
// {date, mL pH, minutes de filtration}, archivé UNE fois par jour au basculement
// de date (cf. ph_persist_logic.h et recordPhLoadDay dans config.cpp). Jusqu'ici
// la charge observée rendait `days = 0` et RIEN n'était jamais restauré.
//
// CRITÈRE DE JOUR EXPLOITABLE : au moins kPhLoadDayMinFiltrationMin minutes de
// circulation. Sous ce seuil le rapport mL/min ne décrit plus la charge du
// bassin mais des transitoires : la purge de reprise d'eau (adaptative depuis la
// feature-091, ≥ 4 min) et la pause de mélange amputent une part fixe de la journée,
// et le budget horaire d'injection (5 min/h) rend une journée courte
// structurellement bornée par le quota, pas par la chimie. 2 h correspond au
// minimum syndical d'un cycle de filtration hivernal ; en dessous, la journée est
// écartée. Une journée à VOLUME NUL mais à circulation suffisante est en
// revanche CONSERVÉE : l'écarter biaiserait la médiane VERS LE HAUT (fail-open),
// alors que la garder ne peut que réduire le plafond de restauration.
constexpr uint16_t kPhLoadDayMinFiltrationMin = 120;  // 2 h de circulation

// Écart maximal entre deux tours de loop encore CRÉDITÉ au compteur de minutes de
// circulation. Distinct de kPhIntegralMaxTickDtMs (10 s), et VOLONTAIREMENT plus
// large : les deux compteurs ne courent pas le même risque. L'intégrale se
// protège d'un dt aberrant qui la CHARGERAIT trop (fail-open) ; le dénominateur,
// lui, ne risque que d'être TRONQUÉ — et un dénominateur tronqué SURÉVALUE la
// charge mL/min, donc le plafond de restauration. Un tour long (flush LittleFS,
// export d'historique, reconnexion réseau) perdait ainsi des minutes sans perdre
// le volume correspondant, que `accountPumpVolume` crédite lui sur le delta
// ENTIER. 60 s couvre tout ce qui n'est pas une panne franche ; au-delà, la
// saturation à 1440 min/jour reste le garde-fou.
constexpr uint32_t kPhLoadMinuteMaxTickDtMs = 60000UL;  // 60 s

// FILET DE VRAISEMBLANCE PHYSIQUE (gate pool-chemistry, 30/08/2026) — plafond
// au-delà duquel un rapport mL/min est la PREUVE d'un dénominateur corrompu, et
// non l'observation d'un bassin très chargé.
//
// AUCUNE journée ne peut afficher un DÉLIVRÉ supérieur au débit moyen soutenable
// de l'actionneur : la pompe elle-même ne le permet pas. Un rapport supérieur ne
// peut donc venir que d'un numérateur complet posé sur un dénominateur amputé —
// minutes perdues par un flush NVS inatteignable, compteur remis à zéro en cours
// de journée, enregistrement à demi écrit. La journée est ÉCARTÉE, jamais
// « corrigée » : on ne devine pas le dénominateur manquant.
//
// ⚠️ LE SEUIL EST UNE FONCTION, PAS UNE CONSTANTE. Le débit soutenable vaut
// `phSustainableAvgFlow(ampFlow, budgetMinPerHour)` — le débit d'amplitude
// CALIBRÉ de la pompe et la limite horaire CONFIGURÉE, tous deux réglables par
// l'utilisateur. Le figer serait un piège silencieux : le gate recommande
// justement de relever `phInjectionLimitMinutes` pour combler le déficit
// demande/délivré, et à 15 min/h le délivré légitime atteint ~6,5 mL/min. Contre
// un seuil figé à 2,17, TOUTES les journées deviendraient invraisemblables,
// `days` retomberait à 0 et la restauration ne s'armerait plus jamais — sans un
// mot dans le journal. La feature mourrait au moment exact où l'utilisateur
// applique la recommandation.
//
// La marge de 10 % absorbe l'arrondi du volume au mL et le décalage d'une minute
// entre les deux compteurs. Elle ne rattrape PAS un dénominateur tronqué : dans
// le scénario de référence (360 min de maintenance perdues sur 480), le rapport
// observé est 4× la vérité, très au-delà du filet.
//
// SENS DU FILTRE : il n'écarte que par le HAUT. Une journée à volume nul et
// longue circulation reste CONSERVÉE — elle tire la médiane vers le bas, donc le
// plafond de restauration aussi, c'est-à-dire du bon côté.
//
// VALEUR DE REPLI, et rien d'autre — et un PLANCHER PRUDENT DÉLIBÉRÉ, PAS une
// dérivation du défaut d'usine courant. Historiquement 5/60 × 26 mL/min (les
// défauts d'avant la v2.30.0) ; le défaut est passé à 10 min/h (feature-087)
// et cette valeur reste VOLONTAIREMENT à 2,17 : un repli sert quand la config
// est indisponible ou aberrante, il doit être fail-closed (filtre de
// vraisemblance PLUS strict, jours écartés, pas de restauration) — l'aligner
// sur l'usine (4,67) l'affaiblirait. Visa pool-chemistry du 31/08/2026.
// Employée uniquement quand le débit soutenable n'est pas calculable — pompe
// non caractérisée, configuration non chargée, valeur non finie. Ne JAMAIS
// s'en servir comme seuil nominal, ne JAMAIS l'« aligner » sur les défauts.
constexpr float kPhLoadSustainableFallbackMlPerMin = 2.17f;  // plancher délibéré
constexpr float kPhLoadDayMaxRateFactor    = 1.10f;   // tolérance d'arrondi
static_assert(kPhLoadSustainableFallbackMlPerMin > 0.0f &&
                  kPhLoadSustainableFallbackMlPerMin < kPhIntegralAuthorityMlPerMin,
              "Repli de debit soutenable incoherent avec l'autorite integrale");
static_assert(kPhLoadDayMaxRateFactor >= 1.0f && kPhLoadDayMaxRateFactor <= 1.5f,
              "Tolerance du filet de vraisemblance hors de [1,0 ; 1,5]");
static_assert(kPhLoadMinuteMaxTickDtMs >= kPhIntegralMaxTickDtMs &&
                  kPhLoadMinuteMaxTickDtMs <= 300000UL,
              "kPhLoadMinuteMaxTickDtMs doit couvrir au moins le dt de "
              "l'intégrale et rester sous 5 min");

// Version du schéma de l'anneau. Toute retouche de PhLoadDaySample / PhLoadHistory
// DOIT l'incrémenter : un anneau d'un autre schéma est rejeté (historique vide,
// donc days = 0 ⇒ fail-closed), jamais réinterprété.
constexpr uint8_t kPhLoadHistorySchema = 1;

static_assert(kPhLoadDayMinFiltrationMin >= 30 &&
                  kPhLoadDayMinFiltrationMin <= 720,
              "kPhLoadDayMinFiltrationMin hors plage raisonnable (30 min - 12 h)");

// =============================================================================
// feature-086 (v2.29.0) — ALERTE DE SOUS-DIMENSIONNEMENT DE L'ACTIONNEUR pH
// =============================================================================
// Le visa pool-chemistry du 30/08/2026 chiffre le déficit structurel du bassin
// de référence : maximum délivrable ~911 mL/j (2,17 mL/min soutenable × 420 min
// de filtration) contre un besoin de neutralisation estimé à ~1 800 mL/j.
// L'installation est limitée par l'ACTIONNEUR, pas par le correcteur. L'alerte
// signale cette situation quand elle se RÉPÈTE : N journées calendaires
// consécutives où le volume délivré colle au maximum possible, PENDANT QUE
// l'épisode C5 (« demande saturée sans atteindre la consigne ») est ouvert.
// Diagnostic pur : aucune décision de dosage n'en dépend (lecture seule).
//
// N = 3 : aligné sur le jalon d'observation terrain de la spec (3 journées
// exploitables dans l'anneau ph_load7) et sur kPhIntegralRestoreMinHistoryDays —
// une journée saturée ISOLÉE (canicule, apport ponctuel après baignade, remise
// en eau) n'alerte pas ; trois journées consécutives ne sont plus un épisode,
// c'est un régime.
constexpr uint8_t kPhActuatorLimitConsecutiveDays = 3;
// Fraction du maximum théorique (soutenable × minutes de circulation ARCHIVÉES)
// au-delà de laquelle une journée est comptée « saturée ». 0,80 et non 0,90-0,95 :
// en saturation permanente RÉELLE, le délivré observé plafonne à 84-88 % du
// maximum théorique — les pauses de mélange, le délai de purge et les plafonds
// de session amputent mécaniquement le reste (incident du 23/08 : 765-802 mL
// comptés pour ~911 mL max). Un seuil à 90-95 % ne se déclencherait donc JAMAIS.
// À l'inverse, un régime convergé (pH tenu à la consigne) délivre < 50 % du
// maximum : 0,80 reste très au-dessus, aucun faux positif à attendre de ce côté.
constexpr float kPhActuatorLimitDayFillRatio = 0.80f;
static_assert(kPhActuatorLimitConsecutiveDays >= 2 &&
                  kPhActuatorLimitConsecutiveDays <=
                      kPhIntegralRestoreHistoryWindowDays,
              "kPhActuatorLimitConsecutiveDays doit tenir dans l'anneau de 7 "
              "jours et exclure la journee isolee");
static_assert(kPhActuatorLimitDayFillRatio > 0.5f &&
                  kPhActuatorLimitDayFillRatio < 0.9f,
              "kPhActuatorLimitDayFillRatio hors de ]0,5 ; 0,9[ : au-dessus il "
              "ne se declenche jamais (delivre reel 84-88 % du max), en dessous "
              "il attrape un regime converge");

// R3 — PÉREMPTION de l'instantané (feature-093, v2.34.0) : l'âge qui compte est
// le TEMPS DE FILTRATION écoulé depuis l'écriture, pas l'heure murale. Le
// bassin n'évolue (acidification, mélange) que pendant que l'eau circule : une
// nuit sans filtration ne périme rien, et l'ancienne péremption murale de 6 h
// (v2.27.0) jetait à chaque matin l'intégrale de la veille (spec feature-093).
// Le compteur monotone de filtration (SafetyLimits::filtTotalMin, clé NVS
// filt_total) fournit cet âge à travers le reboot. Trois bornes :
//  - MaxFiltMin : 360 min de filtration = UNE constante d'intégration à vitesse
//    `normal` (au-delà, la valeur ne décrit plus la charge courante) ;
//  - MaxWallAgeS : filet MURAL de 72 h (compteur monotone à l'arrêt = appareil
//    éteint plusieurs jours, ou horloge/compteur incohérents) ;
//  - DueFiltMin : ÉCHÉANCE d'écriture forcée (P3) — au-delà de 180 min de
//    filtration depuis la dernière écriture, on persiste même si la valeur n'a
//    pas bougé de 5 % (sinon un régime stable n'est jamais rafraîchi et périme).
// Sans heure valide (NTP/RTC non synchronisés), on n'écrit ni ne restaure : un
// âge incalculable vaut un âge périmé (fail-closed).
constexpr uint32_t kPhIntegralPersistMaxFiltMin   = 360UL;     // 6 h de filtration
constexpr uint32_t kPhIntegralPersistMaxWallAgeS  = 259200UL;  // 72 h murales
constexpr uint32_t kPhIntegralPersistDueFiltMin   = 180UL;     // 3 h de filtration

// R4 — CONFIRMATION PAR LA MESURE. La valeur relue n'est PAS appliquée au boot :
// elle reste CANDIDATE jusqu'à ce que le bassin confirme lui-même qu'une charge
// acide est bien présente — filtre capteur prêt, porte de validité ouverte et
// erreur au-dessus du seuil de DÉMARRAGE pendant 5 minutes consécutives de temps
// de branche. Un pH déjà à la consigne au retour du courant ⇒ candidate rejetée.
constexpr uint32_t kPhIntegralRestoreConfirmMs = 300000UL;  // 5 min

// R4 (suite) — fenêtre d'ARMEMENT : au-delà, on renonce définitivement à lire
// l'instantané. Évite de réessayer la lecture NVS indéfiniment quand l'heure ne
// se synchronise jamais (installation hors ligne).
constexpr uint32_t kPhIntegralRestoreArmWindowMs = 600000UL;  // 10 min après boot

// R5 — ÉCRITURE. Au plus une par demi-heure ET seulement si la valeur a bougé de
// plus de 5 % du plafond depuis la dernière écriture. Ordre de grandeur : ~2
// écritures par jour de filtration en régime établi, à comparer aux 100 000
// cycles garantis d'un secteur NVS. Le flush emprunte le point de sauvegarde
// différée des compteurs journaliers (fin d'update, loopTask).
constexpr uint32_t kPhIntegralPersistMinIntervalMs = 1800000UL;  // 30 min
constexpr float    kPhIntegralPersistMinDeltaFrac  = 0.05f;      // 5 % du plafond

// R7 — ANTI-BOUCLE DE BOOT. Au-delà de 3 restaurations SANS écriture normale
// intercalée, la persistance se DÉSARME (plus aucune restauration jusqu'à la
// prochaine écriture normale, qui remet le compteur à zéro). Motif : le PANIC du
// 29/08 (bug-080) n'est pas élucidé — une boucle de redémarrage ne doit pas
// recharger indéfiniment la même dose. Le compteur vit DANS l'instantané NVS :
// il doit survivre au reboot qu'il compte.
constexpr uint8_t kPhIntegralRestoreMaxStreak = 3;

// Version du schéma de l'instantané (R2). Toute retouche de la structure, du
// sens d'un champ ou de la sémantique de la valeur DOIT l'incrémenter : un
// enregistrement de schéma différent est rejeté, jamais réinterprété.
// v2 (feature-093) : +filtTotalMin (compteur monotone de filtration à
// l'écriture, 24 octets) — un instantané v1 (20 o) est rejeté au 1er boot.
constexpr uint8_t kPhIntegralPersistSchema = 2;

// =============================================================================
// feature-085 (v2.28.0) — DOSE DE BASE APPRISE (feedforward) pH
// =============================================================================
// Le FF injecte la charge CONNUE (médiane 7 j de l'anneau de charge, la même que
// le plafond R1 ci-dessus) en TERME ADDITIF du débit de la branche automatique ;
// le PI ne traite plus que l'écart résiduel. Toute la logique est dans
// ph_feedforward_logic.h (module pur, ADR-0017). AUCUN paramètre de volume, de
// TAC ni de concentration n'entre ici (exigence utilisateur, A9 de la spec).

// PLAFOND D'ACTIONNEUR : le FF ne consomme jamais plus de 60 % du débit moyen
// soutenable (phSustainableAvgFlow — amplitude calibrée × limite horaire).
// Pourquoi 60 % : le PI GARDE au moins 40 % du soutenable pour l'écart résiduel
// (condition bloquante n°2 du gate amont). Sur l'installation de référence
// (26 mL/min à 5 min/h ⇒ soutenable 2,17 mL/min), le FF est borné à 1,30 mL/min
// pour une charge observée de 2,86 : le FF couvre ~45 % de la charge DÈS LA
// PREMIÈRE HEURE (au lieu de J+2,4 pour l'intégrale seule) et le PI conserve
// 0,87 mL/min d'autorité immédiate.
constexpr float kPhFfMaxSustainableFraction = 0.60f;

// LATCH « erreur défavorable » : erreur SIGNÉE < −kPhFfAdverseErrorPh tenue
// kPhFfLowHoldMs de temps de BRANCHE ⇒ FF retenu jusqu'à la session suivante.
// −0,05 = le seuil de DÉMARRAGE par défaut, transposé de l'autre côté de la
// consigne : une excursion franche que ni l'hystérésis ni la bande morte
// n'expliquent (apport externe, pluie, surdosage). 30 min = 2× la tenue du reset
// sec de l'intégrale (15 min) : le FF est une autorité PERMANENTE (il dose sans
// regarder l'erreur), on exige donc une preuve plus longue avant de conclure —
// mais assez courte pour satisfaire le critère de la spec (« fausse pluie » :
// annulation en < 30 min). Erreur SIGNÉE et non pH brut : le même déclencheur
// vaut pour ph_minus ET ph_plus (condition n°3 du visa).
constexpr uint32_t kPhFfLowHoldMs = 1800000UL;  // 30 min de temps de branche
constexpr float kPhFfAdverseErrorPh = 0.05f;

// PLAFOND QUOTA JOURNALIER (condition bloquante n°2 du visa) : FF = 0 dès que le
// volume pH du jour atteint 60 % de la limite journalière configurée (si > 0).
// 60 % du soutenable N'EST PAS 60 % du quota : avec 300 mL/j et 12 h de
// filtration, 0,42 mL/min suffisent à consommer 100 % du quota alors que le
// plafond d'actionneur autorise 1,30 mL/min — sans ce gel, le FF viderait seul
// la limite journalière et le PI n'aurait plus aucune marge. Même esprit que
// kPhIntegralFreezeDailyFraction (0,80) : plus strict (0,60), car le FF est une
// autorité aveugle à l'erreur, l'intégrale non.
constexpr float kPhFfDailyQuotaFraction = 0.60f;

static_assert(kPhFfMaxSustainableFraction > 0.0f && kPhFfMaxSustainableFraction < 1.0f,
              "Le FF doit laisser une marge d'actionneur au PI");
static_assert(kPhFfDailyQuotaFraction > 0.0f &&
                  kPhFfDailyQuotaFraction < kPhIntegralFreezeDailyFraction,
              "Le gel du FF doit mordre AVANT le gel d'anti-windup de l'intégrale");
static_assert(kPhFfLowHoldMs >= kPhIntegralResetHoldMs,
              "La tenue du latch FF ne doit pas être plus nerveuse que le reset sec");
static_assert(kPhFfLowHoldMs <= 1800000UL,
              "Critère de spec : le FF s'annule en <= 30 min sur fausse pluie");

// --- feature-092 (C2 du visa pool-chemistry) : FILET DE NON-DIVERGENCE -------
// Depuis feature-092, la composition du débit demandé s'écrit DEUX FOIS : dans
// le tick (computePID + feedforward + régime) et dans la fonction pure
// doseComposition(), qui alimente la carte « Débit d'injection ». La
// factorisation a été refusée à dessein — brancher la fonction pure sur `flow`
// aurait changé le dosage au premier tour de chaque session (computePID a deux
// sorties anticipées que la fonction pure ne modélise pas). La garantie
// structurelle « une seule écriture » est donc perdue : ce seuil est le filet
// qui la remplace, sous forme d'une COMPARAISON journalisée (un warn par
// session, aucune persistance, aucun compteur, aucun champ servi).
//
// POURQUOI 0,05 mL/min ET PAS UN EPSILON FLOTTANT : les deux valeurs comparées
// sont servies arrondies à 2 décimales ; un epsilon machine ferait hurler le
// journal sur du bruit d'arrondi. 0,05 mL/min est en dessous de tout écart
// STRUCTUREL possible (le moindre terme oublié pèse plusieurs dixièmes) et bien
// au-dessus du bruit d'arrondi (0,005). Ce seuil ne borne AUCUN dosage : il ne
// pilote qu'un message de journal.
constexpr float kDoseCompositionDivergenceMlPerMin = 0.05f;

// --- C5 : observabilité de la saturation ------------------------------------
// Une intégrale saturée EN SILENCE est la classe de défaut historique de ce
// projet : la régulation demande son maximum, n'y arrive pas, et personne ne le
// sait.
//
// v2.26.0 (correction) — DEUX défauts de conditionnement corrigés :
//
//  (a) le seuil portait sur le remplissage de l'anti-windup (% de integralMax).
//      Or le gel d'anti-windup immobilise l'intégrale à la valeur qui SATURE
//      l'actionneur, laquelle peut être très inférieure au plafond : avec 5 min/h
//      de budget, l'intégrale se fige vers 34 % de integralMax et l'alerte ne
//      partait JAMAIS. Le seuil porte désormais sur le RAPPORT « demande PID /
//      débit moyen soutenable » — la formulation directe de « la régulation
//      demande son maximum ». La valeur (95 %) et son encadrement sont conservés.
//
//  (b) une seule alerte critical par ÉPISODE, jamais réémise : un état anormal
//      durable devenait SILENCIEUX au bout d'une ligne de journal. D'où la sortie
//      d'hystérésis explicite et le RAPPEL périodique ci-dessous.
constexpr float    kPhIntegralSaturationPct      = 95.0f;  // entrée en saturation
// Sortie d'hystérésis : sans bande, une demande qui oscille autour du seuil
// ouvre/referme l'épisode en rafale et réarme le rappel à chaque oscillation.
// 10 points de bande ≈ 0,2 mL/min de demande avec le budget horaire par défaut.
constexpr float    kPhIntegralSaturationClearPct = 85.0f;

// Paliers de TENUE, comptés en temps de branche automatique (pas en temps mural)
// et conditionnés à « saturée ET erreur toujours au-dessus du seuil d'ARRÊT ».
//
// ===== feature-084 : RECALIBRATION IMPOSÉE PAR LE CHANGEMENT DE CADENCE ======
// La BASE DE TEMPS est CONSERVÉE (temps de branche automatique) : c'est la seule
// qui SUSPENDE l'épisode en `manual`, sous garde bloquante ou pendant un hiver
// sans filtration, au lieu de le faire mûrir tout seul sur une erreur périmée.
// Cette propriété est verrouillée par test natif et n'est pas négociable.
//
// En revanche, le TARIF de cette base a changé : le tick d'entretien est sorti de
// la conjonction `canDose(0)`, il tourne donc plus souvent. À seuils inchangés,
// les paliers mûriraient d'autant plus vite et signaleraient un transitoire de
// rattrapage légitime — le faux positif qui décrédibilise l'alerte C5, dont c'est
// la seule raison d'être. L'INTENTION de la recalibration est donc : laisser la
// FENÊTRE DE PREUVE EN TEMPS RÉEL exactement telle qu'elle était en v2.26.0,
// version validée sur le terrain.
//
// ⚠️ D'OÙ VIENT LE FACTEUR — À LIRE AVANT TOUTE RECALIBRATION FUTURE ⚠️
// Le facteur est le RAPPORT DES DEUX RAPPORTS CYCLIQUES CHIFFRÉS PAR LE GATE
// pool-chemistry du 30/08/2026, et rien d'autre. Une première rédaction avait
// retenu ×4, repris d'une analyse d'impact PRÉLIMINAIRE (« le tick passe de
// `canDose(0)` à toute la fenêtre de filtration », soit un rapport cyclique
// implicitement porté à 100 %) sans le confronter au chiffrage du gate. Résultat
// mesuré : la fenêtre réelle avant warning passait de ~8 h à ~27,6 h et celle
// avant critical à ~3,4 jours — un bidon vide détecté 3,45× PLUS LENTEMENT,
// l'inverse exact de l'intention. Règle qui en découle : ne recalibrer ces
// paliers que sur un rapport cyclique CHIFFRÉ ET ARGUMENTÉ (celui du gate l'est,
// cf. la note de vraisemblance plus bas), jamais sur une intuition d'impact.
constexpr float kPhIntegralDutyV2260 = 0.25f;  // sous canDose(0) — chiffrage gate
constexpr float kPhIntegralDutyF084  = 0.29f;  // prédicat d'entretien — idem
// Paliers de la v2.26.0, VALIDÉS SUR LE TERRAIN : ce sont eux, divisés par le
// rapport cyclique de l'époque, qui définissent les fenêtres réelles à préserver
// (2 h / 0,25 = 8 h ; 6 h / 0,25 = 24 h ; 12 h / 0,25 = 48 h).
constexpr uint32_t kPhIntegralSaturationWarnMsV2260     = 7200000UL;   // 2 h
constexpr uint32_t kPhIntegralSaturationCriticalMsV2260 = 21600000UL;  // 6 h
constexpr uint32_t kPhIntegralSaturationReminderMsV2260 = 43200000UL;  // 12 h
//
// CALCUL RETENU (facteur 0,29 / 0,25 = 1,16), arrondi à la minute ronde la plus
// proche — l'écart d'arrondi vaut +0,6 % sur la fenêtre réelle, négligeable
// devant l'incertitude du rapport cyclique lui-même :
//   warning  : 2 h × 1,16 = 2,32 h  → 2 h 20 min de branche ≈ 8,0 h réelles
//   critical : 6 h × 1,16 = 6,96 h  → 7 h     de branche ≈ 24,1 h réelles
//   rappel   : 12 h × 1,16 = 13,92 h → 14 h   de branche ≈ 48,3 h réelles
constexpr uint32_t kPhIntegralSaturationWarnMs     = 8400000UL;   // 2 h 20 → warning
// ≈ 24 h réelles, soit ~3 jours de filtration à 7 h/jour. Au-delà, une correction
// qui n'a toujours pas ramené l'erreur sous le seuil d'arrêt n'est plus un
// transitoire : c'est un bidon vide, un tube désamorcé, une fuite ou une sonde
// figée. C'est la valeur de la v2.26.0 exprimée dans la nouvelle base de temps.
constexpr uint32_t kPhIntegralSaturationCriticalMs = 25200000UL;  // 7 h → critical
// Rappel du critical tant que l'anomalie dure. ≈ 48 h réelles : l'action
// corrective est PHYSIQUE (aller voir le bidon, réamorcer, vérifier la sonde) et
// ne se fait pas plus souvent. Doit rester ≥ au palier critical (verrou (vii ter)
// ci-dessous), sans quoi la première émission réarmerait immédiatement la suivante.
constexpr uint32_t kPhIntegralSaturationReminderMs = 50400000UL;  // 14 h

// --- Invariants verrouillés à la compilation --------------------------------
// (i) L'autorité doit être strictement positive ET rester dans l'ordre de
// grandeur d'une pompe doseuse : au-delà de 30 mL/min, l'intégrale seule
// saturerait le débit d'amplitude de n'importe quelle pompe du parc.
static_assert(kPhIntegralAuthorityMlPerMin > 0.0f &&
                  kPhIntegralAuthorityMlPerMin <= 30.0f,
              "Autorite integrale pH nulle ou hors de portee d'une pompe doseuse");
// (ii) LE verrou anti-piège : ki × integralMax DOIT redonner l'autorité posée,
// pour les trois vitesses. C'est cet invariant que `integralMax = 50` violait
// d'un facteur 160 — et il le violait en SILENCE.
static_assert(kPhKiSlow * kPhIntegralMaxSlow > 0.99f * kPhIntegralAuthorityMlPerMin &&
                  kPhKiSlow * kPhIntegralMaxSlow < 1.01f * kPhIntegralAuthorityMlPerMin,
              "integralMax (slow) incoherent avec l'autorite posee : le terme "
              "integral ne pourra pas fournir la charge d'equilibre");
static_assert(kPhKiNormal * kPhIntegralMaxNormal > 0.99f * kPhIntegralAuthorityMlPerMin &&
                  kPhKiNormal * kPhIntegralMaxNormal < 1.01f * kPhIntegralAuthorityMlPerMin,
              "integralMax (normal) incoherent avec l'autorite posee");
static_assert(kPhKiFast * kPhIntegralMaxFast > 0.99f * kPhIntegralAuthorityMlPerMin &&
                  kPhKiFast * kPhIntegralMaxFast < 1.01f * kPhIntegralAuthorityMlPerMin,
              "integralMax (fast) incoherent avec l'autorite posee");
// (iii) La constante de temps la plus COURTE doit dominer le retard de mélange :
// intégrer plus vite que le bassin ne s'homogénéise, c'est intégrer sur une
// mesure qui ne reflète pas encore les doses déjà passées (surdosage garanti).
static_assert(kPhIntegralTiFastS >= 4.0f * (kPhMixingDelayMs / 1000.0f),
              "Ti trop court devant la pause melange : l'integrale chargerait "
              "sur une mesure non representative des doses deja injectees");
// (iv) Kp/Ti doivent rester ordonnés (slow plus lent que fast) — une inversion
// ferait qu'accélérer la régulation la ralentirait.
static_assert(kPhKpSlow < kPhKpNormal && kPhKpNormal < kPhKpFast,
              "Kp pH non monotones avec la vitesse de regulation");
static_assert(kPhIntegralTiFastS < kPhIntegralTiNormalS &&
                  kPhIntegralTiNormalS < kPhIntegralTiSlowS,
              "Ti pH non monotones avec la vitesse de regulation");
// (v) La marge du reset sec doit rester au-dessus du bruit résiduel du filtre
// (~0,05 pH) et sous la marge de sécurité basse : un reset qui mord avant la
// garde de plancher effacerait l'intégrale en fonctionnement nominal (défaut C2).
static_assert(kPhIntegralResetMargin >= 0.05f &&
                  kPhIntegralResetMargin < kPhSafetyMargin,
              "Marge de reset d'integrale sous le bruit du filtre (defaut C2 "
              "reproduit) ou au-dela de la marge de securite basse");
// (vi) Le gel sur quota doit laisser une réserve avant la limite dure.
static_assert(kPhIntegralFreezeDailyFraction > 0.5f &&
                  kPhIntegralFreezeDailyFraction < 1.0f,
              "Fraction de gel sur quota journalier hors de [0,5 ; 1,0[");
// (vii) Les paliers d'alerte doivent être ordonnés et le seuil de saturation
// rester sous 100 % (sinon jamais atteint en flottant).
static_assert(kPhIntegralSaturationPct > 50.0f && kPhIntegralSaturationPct < 100.0f,
              "Seuil de saturation d'integrale hors de ]50 ; 100[ %");
static_assert(kPhIntegralSaturationWarnMs < kPhIntegralSaturationCriticalMs,
              "Palier warning de saturation d'integrale au-dela du palier critical");
// (vii bis-a) feature-084 — LA FENÊTRE DE PREUVE EN TEMPS RÉEL EST L'INVARIANT.
// Les paliers sont exprimés en temps de BRANCHE ; ce qui a été validé sur le
// terrain, ce sont les fenêtres RÉELLES correspondantes. Tout changement du
// rapport cyclique du tick d'entretien doit donc être répercuté sur les paliers,
// et ce verrou l'impose : palier / rapport cyclique doit rester constant à 5 %
// près (la tolérance couvre l'arrondi des paliers à la minute ronde).
// Forme croisée (pas de division) : neuf × ancien_duty ≈ ancien × nouveau_duty.
static_assert(
    kPhIntegralSaturationWarnMs * kPhIntegralDutyV2260 >
            0.95f * kPhIntegralSaturationWarnMsV2260 * kPhIntegralDutyF084 &&
        kPhIntegralSaturationWarnMs * kPhIntegralDutyV2260 <
            1.05f * kPhIntegralSaturationWarnMsV2260 * kPhIntegralDutyF084,
    "Palier warning recalibre hors de la fenetre reelle validee en v2.26.0: "
    "verifier le rapport cyclique (facteur = duty_apres / duty_avant)");
static_assert(
    kPhIntegralSaturationCriticalMs * kPhIntegralDutyV2260 >
            0.95f * kPhIntegralSaturationCriticalMsV2260 * kPhIntegralDutyF084 &&
        kPhIntegralSaturationCriticalMs * kPhIntegralDutyV2260 <
            1.05f * kPhIntegralSaturationCriticalMsV2260 * kPhIntegralDutyF084,
    "Palier critical recalibre hors de la fenetre reelle validee en v2.26.0");
static_assert(
    kPhIntegralSaturationReminderMs * kPhIntegralDutyV2260 >
            0.95f * kPhIntegralSaturationReminderMsV2260 * kPhIntegralDutyF084 &&
        kPhIntegralSaturationReminderMs * kPhIntegralDutyV2260 <
            1.05f * kPhIntegralSaturationReminderMsV2260 * kPhIntegralDutyF084,
    "Rappel de critical recalibre hors de la fenetre reelle validee en v2.26.0");
// CE QUE SONT CES DEUX RAPPORTS CYCLIQUES, DIT EXACTEMENT (correction du gate
// pool-chemistry du 30/08/2026 : la rédaction précédente les qualifiait de
// « MESURES, pas des estimations » — c'était faux, et faux dans le paragraphe
// même qui interdit de recalibrer sur une estimation).
//
// Ce sont des ESTIMATIONS ANALYTIQUES du gate, pas des relevés de terrain :
// 0,29 vaut 7/24 exactement, soit la fenêtre de filtration quotidienne de la
// spec PRIVÉE de son facteur de disponibilité 0,85 ; 0,25 est le chiffrage
// correspondant sous `canDose(0)`.
//
// POURQUOI CELA RESTE ACCEPTABLE : le sens de l'erreur est CONSERVATEUR. Ces
// deux nombres ne servent qu'au RAPPORT 0,29/0,25 qui recalibre les paliers.
// Sur-estimer l'ANCIEN rapport cyclique raccourcit les paliers recalibrés, donc
// avance les alertes : le défaut possible est le faux positif — jamais le
// silence, qui est la classe de défaut que C5 existe pour éliminer.
//
// Bornes de vraisemblance, pour empêcher qu'une future retouche y glisse un
// chiffre invraisemblable ou fasse régresser la cadence.
static_assert(kPhIntegralDutyV2260 > 0.0f && kPhIntegralDutyV2260 <= 1.0f &&
                  kPhIntegralDutyF084 > 0.0f && kPhIntegralDutyF084 <= 1.0f &&
                  kPhIntegralDutyF084 >= kPhIntegralDutyV2260,
              "Rapports cycliques du tick d'entretien hors de ]0 ; 1] ou en "
              "regression : le predicat elargi ne peut pas ticker moins souvent");
// (vii bis) L'hystérésis d'alerte doit RELÂCHER franchement sous le seuil
// d'entrée : sans bande, une demande oscillante réarmerait le rappel sans fin.
static_assert(kPhIntegralSaturationClearPct > 50.0f &&
                  kPhIntegralSaturationClearPct < kPhIntegralSaturationPct,
              "Seuil de sortie de saturation hors de ]50 % ; seuil d'entree[");
// (vii ter) Le RAPPEL ne doit jamais devancer le premier critical, sinon la
// première émission ré-armerait immédiatement la suivante (rafale d'alertes).
static_assert(kPhIntegralSaturationReminderMs >= kPhIntegralSaturationCriticalMs,
              "Rappel de critical plus court que le palier qui le declenche");
// (vii quater) Cadence d'intégration : le quantum doit tenir dans le dt maximal
// crédité (sinon aucun quantum ne serait jamais complété en un tick), et le dt
// maximal rester du même ordre que la garde de computePID (10 s).
static_assert(kPhIntegralChargeQuantumMs > 0UL &&
                  kPhIntegralChargeQuantumMs <= kPhIntegralMaxTickDtMs,
              "Quantum de charge d'integrale superieur au dt maximal credite");
static_assert(kPhIntegralMaxTickDtMs >= 1000UL && kPhIntegralMaxTickDtMs <= 60000UL,
              "dt maximal credite a l'integrale hors de [1 s ; 60 s]");
// (viii) L'oubli doit être franc devant la constante d'intégration la plus rapide
// (sinon il concurrencerait la charge) et jamais plus rapide que l'homogénéisation
// du bassin (sinon il oublierait une dose encore « en vol »).
static_assert(kPhIntegralDecayHalfLifeMs * 4.0f <= kPhIntegralTiFastS * 1000.0f &&
                  kPhIntegralDecayHalfLifeMs >= kPhMixingDelayMs,
              "Demi-vie d'oubli de l'integrale pH incompatible avec Ti ou plus "
              "rapide que la pause melange");
// (ix) LE facteur d'oubli répété DOIT redonner la demi-vie annoncée. Sans ce
// verrou, un pas ou un facteur retouché isolément produirait un oubli
// silencieusement faux (trop lent = windup entretenu, trop rapide = charge
// d'équilibre perdue à chaque bruit sous consigne). Élévation à la puissance
// `constexpr` récursive : évaluée par le compilateur, aucun coût à l'exécution.
constexpr float constexprPowf(float base, unsigned int exponent) {
  return exponent == 0u ? 1.0f : base * constexprPowf(base, exponent - 1u);
}
static_assert(kPhIntegralDecayHalfLifeMs % kPhIntegralDecayStepMs == 0,
              "La demi-vie d'oubli doit etre un multiple entier du pas");
static_assert(
    constexprPowf(kPhIntegralDecayStepFactor,
                  (unsigned int)(kPhIntegralDecayHalfLifeMs / kPhIntegralDecayStepMs)) > 0.499f &&
        constexprPowf(kPhIntegralDecayStepFactor,
                      (unsigned int)(kPhIntegralDecayHalfLifeMs / kPhIntegralDecayStepMs)) < 0.501f,
              "Facteur d'oubli incoherent avec la demi-vie annoncee : le pas "
              "repete ne divise pas l'integrale par deux en une demi-vie");
static_assert(kPhIntegralDecayStepFactor > 0.0f && kPhIntegralDecayStepFactor < 1.0f,
              "Facteur d'oubli hors de ]0 ; 1[ : l'integrale ne decroitrait pas");
static_assert(kPhIntegralDecayMaxSteps >=
                  8 * (kPhIntegralDecayHalfLifeMs / kPhIntegralDecayStepMs),
              "Rattrapage d'oubli borne a moins de 8 demi-vies : un residu "
              "significatif pourrait survivre a une longue excursion");
// (x) feature-084 — TEMPORISATION DU RESET SEC. Elle doit couvrir au moins la
// pause mélange (sinon l'excursion observée peut n'être que la dose précédente
// pas encore homogénéisée) et rester franchement sous la demi-vie de l'oubli :
// au-delà, l'oubli aurait déjà fait le travail et le reset sec ne servirait plus
// à rien — or c'est le SEUL mécanisme rapide sur inversion de sens.
static_assert(kPhIntegralResetHoldMs >= kPhMixingDelayMs &&
                  kPhIntegralResetHoldMs < kPhIntegralDecayHalfLifeMs,
              "Temporisation du reset sec sous la pause melange (excursion non "
              "homogeneisee) ou au-dela de la demi-vie d'oubli (reset inutile)");
// (xi) feature-084 — R1 : LE VERROU CHIMIQUE DE LA PERSISTANCE. La contribution
// intégrale RESTAURABLE doit rester STRICTEMENT sous la charge d'équilibre
// mesurée du bassin : une restauration ne peut alors jamais, à elle seule,
// produire un surdosage net. C'est l'invariant qui permet d'amender l'ADR-0052 ;
// le violer rétablit l'objection « autorisation de dosage pré-chargée ».
//
// ⚠️ CE VERROU NE VAUT QUE POUR L'INSTALLATION DE RÉFÉRENCE. Il compare la butée
// absolue à UNE charge mesurée, celle du bassin de développement ; il ne dit
// rien d'un bassin de la flotte dont la charge serait plus faible (sous
// 1,88 mL/min, la butée passe au-dessus de l'équilibre). La vérification qui
// vaut pour TOUT bassin est faite À L'EXÉCUTION, sur la charge observée, par
// phPersistRestoreCap() dans ph_persist_logic.h — ce static_assert n'en est que
// le garde-fou de compilation.
static_assert(kPhIntegralRestoreMaxFraction * kPhIntegralAuthorityMlPerMin <
                  kPhEquilibriumLoadMlPerMin,
              "Plafond de restauration de l'integrale pH au-dela de la charge "
              "d'equilibre mesuree : une restauration pourrait surdoser seule");
static_assert(kPhIntegralRestoreMaxFraction > 0.0f &&
                  kPhIntegralRestoreMaxFraction <= 0.5f,
              "Fraction de restauration de l'integrale hors de ]0 ; 0,5]");
// (xi bis) Le terme dérivé de la charge OBSERVÉE doit rester dans la moitié
// convergente de l'équilibre : à I* la boucle bascule sur un cycle limite dont
// elle ne revient pas (0,013 mL/min de décharge par heure, six jours sans
// retour). 0,50 laisse 50 % de marge ; au-delà de 0,80 il n'y en aurait plus.
static_assert(kPhIntegralRestoreLoadFraction > 0.0f &&
                  kPhIntegralRestoreLoadFraction <= 0.80f,
              "Fraction de l'equilibre observe restauree hors de ]0 ; 0,8] : "
              "au-dela, la boucle s'installe sur un cycle limite");
// (xi ter) L'exigence d'historique doit tenir dans la fenêtre observée et rester
// strictement positive : à zéro jour, le fail-closed disparaîtrait et un appareil
// neuf restaurerait sur une charge inconnue.
static_assert(kPhIntegralRestoreMinHistoryDays >= 1 &&
                  kPhIntegralRestoreMinHistoryDays <=
                      kPhIntegralRestoreHistoryWindowDays,
              "Exigence d'historique de charge nulle ou superieure a la fenetre "
              "d'observation");
// (xii) R3 (feature-093) : la péremption en temps de FILTRATION ne doit pas
// dépasser une constante d'intégration (au-delà, la valeur ne décrit plus la
// charge courante) et doit rester franchement plus courte que la péremption
// d'inactivité de 24 h ; l'échéance d'écriture forcée (P3) doit précéder la
// péremption (sinon l'instantané périme avant d'être rafraîchi) ; le filet
// mural doit être au moins aussi long que la péremption de filtration (sinon il
// la masquerait et la filtration ne compterait jamais).
static_assert(kPhIntegralPersistMaxFiltMin * 60UL <= (uint32_t)kPhIntegralTiNormalS &&
                  kPhIntegralPersistMaxFiltMin * 60000UL < kPhIntegralStaleResetMs,
              "Peremption de l'instantane d'integrale au-dela d'un Ti ou de la "
              "peremption d'inactivite");
static_assert(kPhIntegralPersistDueFiltMin > 0 &&
                  kPhIntegralPersistDueFiltMin < kPhIntegralPersistMaxFiltMin,
              "Echeance d'ecriture forcee nulle ou au-dela de la peremption : "
              "l'instantane perimerait avant d'etre rafraichi");
static_assert(kPhIntegralPersistMaxWallAgeS >= kPhIntegralPersistMaxFiltMin * 60UL,
              "Filet mural plus court que la peremption de filtration : la "
              "filtration ne compterait jamais");
// (xiii) R4 : la confirmation doit durer plusieurs quanta d'intégration (sinon
// un seul tick suffirait) et rester très inférieure à la péremption (sinon la
// candidate mourrait de vieillesse avant d'avoir pu être confirmée).
static_assert(kPhIntegralRestoreConfirmMs >= 10UL * kPhIntegralChargeQuantumMs &&
                  kPhIntegralRestoreConfirmMs < kPhIntegralPersistMaxFiltMin * 60UL * 100UL,
              "Fenetre de confirmation de restauration trop courte (un tick "
              "suffirait) ou trop longue devant la peremption");
// (xiv) R5/R7 : bornes de l'écriture et du garde-fou anti-boucle.
static_assert(kPhIntegralPersistMinDeltaFrac > 0.0f &&
                  kPhIntegralPersistMinDeltaFrac < kPhIntegralRestoreMaxFraction,
              "Seuil d'ecriture NVS nul, ou superieur au plafond de restauration "
              "(l'integrale ne serait jamais persistee sous ce plafond)");
static_assert(kPhIntegralRestoreMaxStreak >= 1 && kPhIntegralRestoreMaxStreak <= 5,
              "Nombre de restaurations consecutives tolerees hors de [1 ; 5]");

// ============================================================================
// SENSOR FROZEN DETECTION — Détection capteur figé (feature-022 Passe 2)
// ============================================================================
// Un module EZO peut répondre sans erreur I²C mais retourner indéfiniment la
// MÊME valeur (électronique interne figée, sonde HS côté BNC...). La panne
// franche (NaN / stale / bus dégradé) est déjà couverte ailleurs ; ici on
// détecte la variance nulle : N lectures ACCEPTÉES consécutives contenues dans
// une bande < epsilon → capteur figé → ready() = false → dosage bloqué
// (garde FilterNotReady existante, fail-closed).
//
// Choix d'epsilon = ½ LSB du capteur (validation pool-chemistry, condition #1) :
//   Un capteur VIVANT bruite d'au moins ±1 LSB (quantification + bruit sonde +
//   bruit thermique). Avec epsilon = ½ LSB, un simple toggle de 1 LSB — même en
//   arithmétique float32 — CASSE le run : aucune eau réelle, même parfaitement
//   stable, ne peut être déclarée figée tant que la dernière décimale bouge.
//   Consigne terrain : en cas de faux positifs, DURCIR N (allonger la fenêtre),
//   ne JAMAIS élargir epsilon.
constexpr uint16_t kSensorFrozenSamples    = 30;      // 30 lectures valides à 5 s = 2,5 min
constexpr float    kSensorFrozenEpsilonPh  = 0.0005f; // ½ LSB EZO pH (résolution 0.001 pH)
constexpr float    kSensorFrozenEpsilonOrp = 0.05f;   // ½ LSB EZO ORP (résolution 0.1 mV)
// Température : DS18B20 12 bits, LSB = 0.0625 °C → epsilon 0.03 < ½ LSB.
// Fenêtre longue (900 lectures à 2 s = 30 min) : l'eau d'un bassin varie
// lentement, mais jamais à 0.0625 °C près sur 30 min avec la sonde immergée.
// Warning-only : aucun impact dosage (effets réels limités à la compensation
// EZO pH et au planning auto de filtration).
constexpr uint16_t kTempFrozenSamples  = 900;   // 900 lectures valides à 2 s = 30 min
constexpr float    kTempFrozenEpsilonC = 0.03f; // < ½ LSB DS18B20 12 bits (0.0625/2)

// ============================================================================
// FILTRATION CONSTANTS - Paramètres filtration
// ============================================================================

// Pivot horaire pour calcul durée filtration
constexpr float kFiltrationPivotHour = 13.0f;             // 13h - Pivot pour calcul durée selon température

// bug-126 — référence de température du planning auto (moyenne du cycle,
// figée à l'arrêt dans un planning EN ATTENTE, promue à minuit).
constexpr float    kFiltrationRefTempMinC = 4.0f;   // sous 4 °C : hivernage, pas de planning
constexpr float    kFiltrationRefTempMaxC = 40.0f;  // au-delà : panne de sonde (cf. static_assert)
constexpr uint32_t kFiltrationTempSampleIntervalMs = 60000UL;  // 1 échantillon/min
// Seuil de représentativité (30 échantillons = 30 min en plage, hors purge).
// L'argument n'est PAS volumétrique (la sonde est sur la ligne d'ASPIRATION :
// elle lit ce que l'aspiration prélève, faire transiter du volume ne mélange
// pas le bassin). Le seuil sert à (i) rejeter les cycles TRONQUÉS dont la
// moyenne serait dominée par un quart d'heure de transitoire, et (ii)
// couvrir une fraction représentative du cycle diurne, dont le profil varie
// avec l'ensoleillement.
constexpr uint16_t kFiltrationMinRepresentativeSamples = 30;

// La fenêtre auto ne peut plus franchir minuit : la promotion à 00:00 ne peut
// donc jamais tomber au milieu d'une plage auto (bug-126, condition C5
// pool-chemistry). Bornes sur la température MAX plausible (voir
// kFiltrationRefTempMaxC) : computeAutoWindow ne calcule un créneau qu'à
// partir d'une température ayant déjà passé isPlausibleReferenceTemp.
static_assert(kFiltrationPivotHour + kFiltrationRefTempMaxC / 4.0f < 24.0f,
  "Fenetre auto a cheval sur minuit (fin) : la promotion arreterait une pompe en marche.");
static_assert(kFiltrationPivotHour - kFiltrationRefTempMaxC / 4.0f >= 0.0f,
  "Fenetre auto a cheval sur minuit (debut).");

// feature-056 : mode d'installation « Filtration externe signalée ». Fraîcheur
// max du dernier signal d'état (HTTP/MQTT) avant fail-safe OFF (dosage refusé).
constexpr uint32_t kExternalFiltrationStaleMs = 180000UL;  // 3 min — fraîcheur max signal filtration externe (fail-safe OFF au-delà)

// ============================================================================
// PUMP CONSTANTS - Paramètres pompes doseuses
// ============================================================================

// ---------------------------------------------------------------------------
// Dosage en proportionnel de TEMPS (bug-dosage-fantome, itération 2 — option
// B+C du gate pool-chemistry). Le modèle linéaire duty↔débit est SUPPRIMÉ
// (kPumpMinFlowMlPerMin / kPumpMaxFlowMlPerMin retirés) : mesure au banc du
// 2026-08-23 — à duty 82 le modèle créditait 6,4 ml/min pour 12,0 mesurés, à
// 255 il supposait 90 ml/min pour 61,0 mesurés. Voir src/pulse_dosing_logic.h.
// ---------------------------------------------------------------------------
// Période fixe du train d'impulsions (condition B3 : période FIXE).
constexpr uint32_t kPulsePeriodMs = 60000;   // 1 min — période d'impulsion
// Pouls minimal (condition B3 : ≥ 5 s). Sous ce pouls, le débit n'est pas
// reproductible (transitoires de démarrage dominants).
//
// v2.25.3 : 5 s → 10 s (gate GO sous conditions du 28/08/2026). Motif : la
// régulation pH ne converge pas — 968 mL comptés en une journée sans effet
// mesurable sur le pH (7,50 pour une consigne à 7,30), alors que le besoin
// calculé est de 800-900 mL pour ΔpH 0,2 sur 40 m³ (composition du produit ET
// dosage constructeur concordants). Hypothèse : la PERTE FIXE par démarrage
// (relaxation puis ré-occlusion du tube, reprise d'amorçage — 0,1 à 0,5 mL sur
// une pompe dont un tour vaut 1-2 mL) fait que le volume réellement injecté est
// très inférieur au volume compté. Toutes les injections étant au pouls
// minimal, l'erreur porte sur 100 % du volume.
//
// Doubler le pouls **n'augmente PAS la dose demandée** : planPulseWithCarry
// accumule `meanFlow × period` et retire exactement V_pouls au tir, donc la
// moyenne restituée est indépendante de cette constante (vérifié par le gate
// dans pulse_dosing_logic.cpp:56-99). Ce qui change est le RENDEMENT : moins de
// démarrages pour le même volume, donc la perte fixe est amortie sur davantage
// de volume utile.
// Facteur MESURÉ en test natif (600 min à demande 1,76 mL/min) : 427 → 228 tirs,
// soit **×1,87** et non ×2 — le kick n'étant compté qu'une fois par pouls, la
// perte fixe est amortie de **47 %**, pas de 50 %.
// Effet SECONDAIRE favorable, trouvé en test : dans la bande de demande
// 2,15-4,29 mL/min, le régime proportionnel DIRECT sur-restituait 0,32 mL par
// période (kick crédité au majorant de flotte 74), écart NON borné par V_pouls
// — contrairement au régime accumulateur, qui est exact. Doubler le pouls
// ramène cette bande dans le régime accumulateur et supprime donc ce surplus.
// Conséquences chiffrées (ampFlow mesuré 25,75 mL/min) : V_pouls 2,47 → 4,61 mL,
// seuil de bascule vers le régime proportionnel direct 2,15 → 4,29 mL/min de
// demande, une impulsion toutes les ~2,7 min au lieu de ~1,4 min à demande
// 1,76 mL/min. Bolus unitaire 4,6 mL = 32 méq : ΔpH < 0,002 même sur 15 m³,
// négligeable jusqu'à ~5 m³ (réserve : spa < 2 m³, hors périmètre flotte).
//
// ⚠️ DETTE ASSUMÉE (condition n°1 du gate) : le compteur reste FAUX. En divisant
// les démarrages par deux à volume COMPTÉ constant, on augmente le volume RÉEL
// délivré sous le même plafond journalier — la limite devient légèrement moins
// conservatrice (quelques dizaines de mL/jour si la perte vaut ~0,5 mL). Cet
// allongement ATTÉNUE le défaut, il ne le corrige pas : la mesure de la perte
// fixe (24 pouls de 5 s contre 12 de 10 s, à temps de pompe égal) reste due, et
// le modèle de comptage devra être corrigé ensuite. Voir feature-078.
constexpr uint32_t kPulseMinMs = 10000;      // 10 s — pouls minimal

// feature-094 (condition n°2 du gate pool-chemistry) : le parcours d'intégrité
// du tas s'exécute INTERRUPTIONS COUPÉES et peut désormais s'intercaler juste
// avant l'énergisation d'une pompe — isPhDosing()/isOrpDosing() ont été retirés
// de sa garde (main.cpp), car depuis feature-094 ils suivent la PORTE et non le
// débit, ce qui aurait rendu le détecteur aveugle jusqu'à 3 h d'affilée.
// Le décalage induit est sans effet chimique TANT QU'il reste négligeable devant
// le pouls minimal : le rendement de pouls (ADR-0051) se mesure sur usedMs,
// compté à partir de l'énergisation EFFECTIVE, donc un pouls décalé dure la même
// chose et crédite le même volume. On verrouille ici la marge : le parcours doit
// rester sous 1 % du pouls minimal (aujourd'hui 20 ms contre 10 s, soit 500×).
// Si un jour il s'en approche, le retard redevient un sujet chimique.
// NOTE : comparaison ramenée aux microsecondes des DEUX côtés — l'expression
// proposée en revue comparait des µs à des ms.
static_assert((uint64_t)kHeapIntegrityMaxDurationUs * 100ULL <=
                  (uint64_t)kPulseMinMs * 1000ULL,
              "Parcours d'integrite du tas trop long devant le pouls minimal : "
              "le decalage d'energisation cesse d'etre negligeable (gate 094)");
// Kick de décollage : pleine puissance (duty 255) en tête de chaque pouls pour
// vaincre le couple de démarrage à froid (symptôme terrain : rotation entretenue
// OK à duty 82 mais NON-démarrage au même duty). Volume du kick comptabilisé au
// débit majorant de flotte à 255 (kPumpFlow255MlPerMin).
constexpr uint32_t kPulseKickMs = 400;       // 0,4 s — durée du kick à 255
// Débit MAJORANT de FLOTTE à duty 255 (v2.25.2 — fusion des deux anciennes
// constantes kPump1Flow255MlPerMin = 61,0 / kPump2Flow255MlPerMin = 67,0).
// Trois usages, tous dans le MÊME sens : crédit du kick de chaque impulsion,
// majorant du mode manuel ?duty=N (P4 : jamais créditer zéro sur une pompe
// alimentée) et durée d'une injection volumée sur le chemin DÉGRADÉ (pompe non
// calibrée). Surestimer ne sur-injecte jamais — les limites mordent plus tôt.
//
// POURQUOI UNE SEULE VALEUR : les 61,0 / 67,0 étaient des mesures d'UN SEUL
// exemplaire chacune, figées en dur sur un index PHYSIQUE de pompe. Attribuer
// la mesure d'un exemplaire à un index n'a aucun sens sur une flotte : un
// appareil dont la pompe 1 ressemblerait à l'exemplaire « 67 » se serait
// sous-crédité de 11 % — il aurait injecté plus que ses limites ne le croyaient.
//
// POURQUOI 74,0 — même raisonnement que kPumpTypePulseFlowMlPerMin (28,0) :
// **max réellement mesuré 68,0 + 8 % de dispersion inter-unités**. 70,0 a été
// écarté : +3 % de marge alors que l'écart DÉJÀ CONSTATÉ entre deux exemplaires
// du même lot est de 11 % — une marge inférieure à la dispersion observée est
// statistiquement nulle.
//
// La précision volumée n'est PAS achetée contre cette garde : le chemin exact
// (ampDuty == calDuty ⇒ débit PESÉ par l'utilisateur) existe déjà ; le chemin
// 255 est un repli dégradé.
constexpr float kPumpFlow255MlPerMin = 74.0f;
// feature-110 — HYGIÈNE DE LA MESURE, aucun effet sur le dosage. Le volume d'un
// tick est crédité par attribution ARRIÈRE sur [lastSafetyTimestamp, now] : au
// delà de ce seuil, on ne sait plus si la pompe a tourné tout du long (loop
// bloquée, OTA, I²C en souffrance). Ces ticks ne sont NI rejetés NI corrigés —
// la comptabilité du volume est intouchée — ils sont comptés à part, avec leur
// volume, pour pouvoir DISQUALIFIER un relevé de bidon pollué.
// CALAGE DU SEUIL : accountPumpVolume tourne à la cadence de loop()
// (kLoopDelayMs = 10 ms + le travail du tour, quelques dizaines de ms en
// pratique) — 5 s est donc deux ordres de grandeur au-dessus du jitter normal,
// et vaut la MOITIÉ du pouls minimal (kPulseMinMs = 10 s) : un tick qui couvre
// la moitié d'une impulsion entière est bien une anomalie, pas du bruit.
constexpr uint32_t kAcctDeltaAnomalyMs = 5000;
// Maximum RÉELLEMENT mesuré à duty 255 (banc du 2026-08-23 : 61,0 g/60 s sur
// l'exemplaire 1, 68,0 g/60 s sur l'exemplaire 2 — la constante historique
// portait 67,0, sous-estimation corrigée ici). N'intervient dans aucun calcul :
// verrou du static_assert ci-dessous, pour qu'une future « simplification » du
// majorant sous le max mesuré ne compile plus. Mettre à jour SEULEMENT sur une
// nouvelle mesure d'exemplaire.
constexpr float kPumpFlow255MeasuredMaxMlPerMin = 68.0f;
static_assert(kPumpFlow255MlPerMin >= kPumpFlow255MeasuredMaxMlPerMin,
              "Majorant a 255 sous le maximum mesure : le firmware "
              "sous-crediterait le kick et les injections manuelles — les "
              "limites de dosage mordraient TROP TARD");
// Plancher de l'amplitude en % (UI/config) : 40 % × 255 ≈ 102 counts, cohérent
// avec MIN_ACTIVE_DUTY = 98 (Dmin mesuré 82 + 16 counts de marge P2).
constexpr uint8_t kPumpAmpMinPct = 40;
// Plausibilité du débit calibré saisi (pesée) : au-delà, valeur rejetée
// (fail-closed FlowUncharacterized) — aucune doseuse de piscine ne débite plus.
constexpr float kPulseFlowMaxMlPerMin = 200.0f;
// feature-074 : CALIBRATION DE TYPE (défaut usine du modèle de pompe) — débit
// (ml/min) au duty de type kPumpTypeCalDuty (= 127, amplitude 50 %). Contexte
// flotte : tous les exemplaires embarquent le MÊME modèle de pompe péristaltique,
// un appareil neuf doit doser dès le premier flash sans pesée individuelle.
// Mesures de référence du 2026-08-23 (unité de référence, interpolations) :
//   - Pompe 1 : 12,0 ml/min @82 et 61,0 ml/min @255 → interpolation 24,7 @127 ;
//   - Pompe 2 : 68,0 ml/min @255 → ~27,2 @127 (échelle relevée à l'époque sur
//     67, valeur corrigée en v2.25.2 : la mesure de l'utilisateur est 68,0) ;
//   - Dispersion inter-unités constatée : ±11 % (61 vs 68 à 255, même modèle).
//
// MESURES RÉELLES du 2026-08-28 (assistant feature-075, 120 s à duty 127,
// volume lu en éprouvette — elles REMPLACENT les interpolations ci-dessus comme
// référence) : **pompe 1 = 26,0 ml/min, pompe 2 = 25,5 ml/min**. Les
// interpolations étaient donc bonnes à 4-7 % près.
//
// POURQUOI 28,0 ET NON 26,0 (le maximum mesuré) — arbitrage gate du 28/08 :
// le débit configuré agit DEUX FOIS DANS LE MÊME SENS — il fixe la durée du
// pouls (débit élevé ⇒ pompe ON moins longtemps ⇒ sous-dosage) ET le crédit des
// compteurs (débit élevé ⇒ sur-comptage ⇒ les limites mordent plus tôt).
// Surestimer est donc doublement fail-safe ; SOUS-estimer casse les deux
// protections à la fois.
// 26,0 est le max de n = 2 exemplaires du MÊME LOT, marge nulle : ce n'est pas
// une statistique de flotte. Sur péristaltique à duty 127 (zone non linéaire :
// frottement balais, tension d'alim, couple de démarrage) la dispersion attendue
// est de ±10-15 %, et un tube NEUF débite plus qu'un tube rodé (ovalisation).
// Le fait que les deux pompes diffèrent de 10 % @255 mais de 2 % @127 ne
// rassure pas : il prouve que le rapport de débit n'est PAS transposable d'un
// point de fonctionnement à l'autre — la queue de distribution est inconnue.
// 28,0 = **max mesuré 26,0 + 8 % de dispersion inter-unités**. Justification
// désormais adossée à une MESURE, non plus à une interpolation.
// Coût assumé : un appareil neuf sous-dose de ~8 % jusqu'à sa propre mesure.
// En boucle fermée cela ne crée aucun biais permanent, seulement une
// convergence plus lente — on n'affaiblit pas un filet de sécurité pour
// compenser un défaut de réglage.
constexpr float kPumpTypePulseFlowMlPerMin = 28.0f;
// Maximum RÉELLEMENT mesuré à duty 127 sur les unités de référence (28/08/2026).
// N'intervient dans aucun calcul : sert de verrou au static_assert ci-dessous,
// pour qu'une future « simplification » du défaut d'usine sous le max mesuré ne
// compile plus. Mettre à jour SEULEMENT sur une nouvelle mesure d'exemplaire.
constexpr float kPumpTypeFlowMeasuredMaxMlPerMin = 26.0f;
static_assert(kPumpTypePulseFlowMlPerMin >= kPumpTypeFlowMeasuredMaxMlPerMin,
              "Debit d'usine sous le maximum mesure : le firmware sous-compterait "
              "et sous-estimerait la duree de pouls — les deux protections tombent "
              "ensemble sur un appareil neuf jamais mesure");
// Throttle des logs de refus de l'ordonnanceur d'impulsions : critical « pompe
// non caractérisée / amplitude sous plancher » (état de config persistant) et
// info « consigne sous le pouls minimal » (deadband normal). 30 min : visible
// dans la journée sans faire tourner le journal persistant (leçon 2.20.2).
constexpr unsigned long kPulseRefusalLogIntervalMs = 1800000UL;  // 30 min

// ---------------------------------------------------------------------------
// feature-075 : ASSISTANT DE MESURE DU DÉBIT des pompes (verdict gate
// pool-chemistry feature-075). Marches pompe MINUTÉES CÔTÉ SERVEUR (l'arrêt ne
// dépend jamais du navigateur) + suspension du dosage auto pendant l'assistant.
// ---------------------------------------------------------------------------
// Purge des tuyaux à l'eau claire : à duty 255 IMPÉRATIVEMENT — c'est le SEUL
// duty dont le débit est connu d'usine (kPumpFlow255MlPerMin), donc le seul
// permettant de borner le volume de produit résiduel poussé au bassin.
constexpr uint32_t kFlowAssistPurgeDurationS = 60;       // 60 s — purge à duty 255
// Marche de mesure à l'amplitude EXACTE d'exploitation : 120 s (et non 60 s)
// pour doubler le volume lu (~50 mL au lieu de ~25) et HALVER l'erreur de
// lecture relative sur le récipient gradué.
constexpr uint32_t kFlowAssistMeasureDurationS = 120;    // 120 s — mesure minutée
// Purges max par session d'assistant : borne le volume total envoyé au bassin
// et le temps de marche hors régulation (une purge qui n'amorce pas en 3
// tentatives signale un problème matériel, pas un besoin de purge de plus).
constexpr uint8_t kFlowAssistMaxPurgesPerSession = 3;
// Timeout serveur de la SUSPENSION du dosage auto : un assistant abandonné
// (onglet fermé, WiFi tombé) ne doit jamais laisser la régulation suspendue
// indéfiniment. Réarmé à CHAQUE interaction de l'assistant (chaque requête).
constexpr unsigned long kFlowAssistSuspendTimeoutMs = 900000UL;  // 15 min

// Erreur max pour normalisation PID
constexpr float kPhMaxError           =  1.0f;   // Erreur pH maximale (unités pH)
constexpr float kOrpMaxError          = 200.0f;  // Erreur ORP maximale (mV)

// Durées de stabilisation post-calibration EZO (pool-chemistry condition #3).
// Validation pool-chemistry feature-021 :
//   - pH  : équilibre membrane verre + jonction Ag/AgCl ≈ 5 min
//   - ORP : équilibre Pt/Ag ≈ 3 min (cinétique plus rapide que pH)
// feature-091 (v2.34.0) : le réglage `stabilizationDelayMin` est RETIRÉ de ces
// armings — il ne reste que les usages post-calibration EZO et le double quota
// de minuit (durée pH). La purge de reprise d'eau est désormais pilotée par la
// STABILITÉ des mesures (kPurge*, ci-dessous), pas par une minuterie fixe.
constexpr unsigned long kStabilizationDurationPhMs  = 300000UL;  // 5 min — pH post-cal
constexpr unsigned long kStabilizationDurationOrpMs = 180000UL;  // 3 min — ORP post-cal

// =============================================================================
// feature-091 (v2.34.0) — PURGE ADAPTATIVE PAR STABILITÉ DES MESURES
// =============================================================================
// À chaque reprise d'eau (filtration qui démarre), les sondes lisent d'abord
// l'eau STAGNANTE de la canalisation, puis un transitoire de mélange. La porte
// de validité des mesures (measure_validity.*) reste FERMÉE tant que les bruts
// pH/ORP n'ont pas cessé de bouger — plutôt qu'un délai fixe réglé à la main
// (l'ancien stabilizationDelayMin : trop court on dose sur du transitoire, trop
// long on perd du temps de régulation chaque matin).
//
// Détecteur : une statistique PAR MINUTE de chaque brut (12 lectures EZO à
// 5 s), sur une fenêtre glissante de kPurgeWindowMin minutes ; « stable » =
// fenêtre pleine ET étendue (max−min des statistiques de bloc) ≤ seuil sur les
// DEUX voies. Seuils = bruit de mesure observé sur le bassin de référence
// (dispersion pH brut ~0,36 sur les LECTURES, ~0,10-0,15 sur les statistiques
// de bloc) : 0,15 pH / 15 mV.
//
// ESTIMATEUR DE BLOC (feature-097, v2.54+, amendement daté ADR-0059 §1.2) :
// MOYENNE INTERQUARTILE, plus la médiane d'origine. Mesure terrain du 20/09 :
// le pH brut de ce bassin est anti-corrélé, presque à deux niveaux
// (autocorrélation lag 1 = −0,440) — sur un tel signal la médiane n'est pas
// une moyenne robuste, c'est un INTERRUPTEUR qui bascule sur le niveau
// momentanément majoritaire. Voir purge_stability_logic.h pour le détail et le
// bloc réel mesuré. Aucun seuil ci-dessous n'a changé ; seul l'estimateur qui
// les alimente a changé. Planchers de bruit mesurés avec le nouvel
// estimateur : voir kPurgeNoiseFloorPh / kPurgeNoiseFloorOrp ci-dessous.
//
// FENÊTRE = 10 min (double arbitrage pool-chemistry du 04/09 après rejeux
// natifs) : W médianes couvrent W−1 intervalles, la pente minimale détectable
// d'une RAMPE est seuil/(W−1) moins le bruit de la médiane (±0,03). Mais le
// rinçage est un FOND EN V (descente −0,047 puis remontée +0,043 pH/min le
// 04/09) : le fond occupe une demi-fenêtre de chaque côté et la pente
// détectable devient seuil/((W−1)/2). Avec W = 4 la porte s'ouvrait à t+7-8 à
// pH ≈ 5,6 (l'incident à supprimer) ; avec W = 8 le rejeu sans bruit ouvrait
// encore à t+13 à pH 5,71 (étendue 0,145 — 5 millièmes sous le seuil) puis
// refermait à t+17 (0,303 — 3 millièmes au-dessus) : pas une marge. Avec
// W = 10 : 4,5 × 0,045 = 0,20 > 0,15 sur le V, 9 × 0,045 = 0,40 ≫ 0,15 sur la
// rampe, re-fermeture C1 dès 0,033 pH/min ; le rejeu ouvre à t+46-53 sur le
// plateau (pH 6,88). Limite connue : une dérive lente de sonde (≤ 0,017
// pH/min) est jugée stable — la régulation suit la filtrée, gardes
// butée/plancher actives. L'ORP (1,5 mV/min au rinçage → 13,5 mV < 15) est un
// critère SECONDAIRE : il ne détecte pas seul une remontée ≤ 1,6 mV/min.
// 10 = capacité de l'anneau (kPurgeStabilityMaxWindowMin) : aller au-delà
// exige d'agrandir le module pur.
constexpr uint8_t  kPurgeWindowMin        = 10;       // fenêtre de médianes (min)
constexpr uint8_t  kPurgeSamplesPerMin    = 12;       // lectures EZO par minute (5 s)
constexpr float    kPurgeStablePhRange    = 0.15f;    // étendue max pH brut (fenêtre)
constexpr float    kPurgeStableOrpRange   = 15.0f;    // étendue max ORP brut (mV)
// PLANCHER : la porte ne s'ouvre jamais avant 10 min d'eau présente, même si la
// fenêtre est déjà stable (une canalisation courte peut paraître stable alors
// que l'eau de la conduite n'a pas encore été renouvelée). ≥ fenêtre — donc
// coût d'une coupure longue ≈ 10 min + amorçage des filtres (~65 s), contre
// 15 min de minuteur fixe avant 2.34 ; la coupure ≤ 10 s (dispense, ci-dessous)
// reste à zéro.
constexpr uint32_t kPurgeFloorMs          = 600000UL; // 10 min
// PLAFOND (C2 pool-chemistry) : fail-safe si la stabilité n'arrive jamais (sonde
// bruyante, injection en cours en amont…) — la porte s'ouvre quand même à 90 min
// avec ALERTE `measures_purge_timeout` ; la surveillance de re-fermeture est
// alors DÉSARMÉE jusqu'à la prochaine reprise d'eau (sinon boucle fermer/ouvrir).
constexpr uint32_t kPurgeCapMs            = 5400000UL; // 90 min
// RE-FERMETURE (C1 pool-chemistry) : après une ouverture par stabilité ou par
// dispense, si l'étendue de la fenêtre dépasse 2× le seuil d'ouverture, la porte
// se referme (re-purge) jusqu'à un nouveau plancher + stabilité. Hystérésis
// ×2 : un simple retour au bruit d'ouverture ne referme pas.
constexpr float    kPurgeRecloseRangePh   = 0.30f;    // = 2 × kPurgeStablePhRange
constexpr float    kPurgeRecloseRangeOrp  = 30.0f;    // = 2 × kPurgeStableOrpRange
// PLANCHERS DE BRUIT (feature-097, v2.54+) — étendue max mesurée d'une fenêtre
// de 10 statistiques de bloc (moyenne interquartile) sur eau ÉTABLIE, trace du
// 2026-09-20 (`test/fixtures/f097_purge_noise_20260920.csv`, 25 min, 300
// échantillons) : pH 0,089 max, ORP 8,7 mV max — arrondis au-dessus. Compile
// time UNIQUEMENT (static_assert plus bas) : jamais lues à l'exécution, elles
// ne pilotent aucun seuil en direct — un plancher lu au runtime serait mesuré
// PENDANT la purge, donc gonflé par le transitoire lui-même (piste écartée,
// voir spec feature-097 §Options évaluées).
// ⚠️ Une seule trace, porte ouverte, mesurée le 20/09 ; marge retenue
// (kPurgeStablePhRange / kPurgeNoiseFloorPh = 0,15 / 0,10) : 1,5× SEULEMENT.
constexpr float    kPurgeNoiseFloorPh                = 0.10f;   // pH, plancher de bruit mesuré
constexpr float    kPurgeNoiseFloorOrp               = 10.0f;   // mV, idem
// Dérive de sonde MINIMALE qui DOIT ENCORE être détectée par la re-fermeture
// (C1) — l'assert plus bas garantit qu'une dérive AU MOINS aussi rapide que
// celle-ci referme la porte (kPurgeRecloseRangePh <= cette dérive × span de la
// fenêtre). Sert de base à cet invariant reliant kPurgeRecloseRangePh au span
// réel de la fenêtre, plutôt qu'à une simple égalité ×2 aveugle au nombre
// d'intervalles.
constexpr float    kPurgeMinDetectedDriftPhPerMin    = 0.035f;  // pH/min
// COUPURE COURTE (C3 pool-chemistry) : un redémarrage LOGICIEL (OTA, panic,
// watchdog) de ≤ 10 s alors que l'eau était présente ne renouvelle pas l'eau de
// la conduite — la purge est DISPENSÉE. Mémoire RTC (survit au reset, pas à la
// coupure secteur), heure valide obligatoire, au plus UNE dispense par heure.
constexpr uint32_t kShortOutageS             = 10;    // durée max d'une coupure « courte »
constexpr uint32_t kOutageWaiverMinIntervalS = 3600;  // 1 dispense max par heure

// feature-097 : bug latent corrigé — la version précédente comparait
// kPurgeFloorMs à kPurgeWindowMin * 60000UL, ce qui suppose en silence
// « 1 bloc = 1 minute murale ». La durée réelle d'un bloc dépend de
// kPurgeSamplesPerMin ET de la cadence de lecture ; l'écrire en durée réelle
// rend le calcul robuste si l'un des deux change un jour. Vaut 600 000 ms
// aujourd'hui des deux côtés : l'égalité est satisfaite, rien ne bouge.
static_assert(kPurgeFloorMs >= (uint64_t)kPurgeWindowMin * kPurgeSamplesPerMin *
                                    kPhOrpSensorIntervalMs,
              "Plancher de purge plus court que la fenetre de stabilite : "
              "la porte s'ouvrirait sur une fenetre incomplete");
static_assert(kPurgeFloorMs < kPurgeCapMs,
              "Plancher de purge au-dela du plafond");
static_assert(kPurgeCapMs >= 3600000UL,
              "Plafond de purge < 60 min : un transitoire lent declencherait "
              "l'alerte a tort");
// bug-126 — le seuil de représentativité (kFiltrationMinRepresentativeSamples,
// constants.h plus haut) doit rester atteignable même sur le cycle auto le
// PLUS COURT : computeAutoWindow clampe la durée à >= 1,0 h
// (schedule_logic.cpp), et la purge (kPurgeFloorMs) en consomme le début.
// Purge déduite, il reste 50 min en plage sur ce cycle-là : si le seuil n'y
// tenait pas, le planning se figerait indéfiniment aux températures basses
// (jamais assez d'échantillons post-purge pour produire un pending).
// Comparaison tout en MILLISECONDES (revue P2, corrections mineures) : la
// version précédente divisait kFiltrationTempSampleIntervalMs par 60000UL
// AVANT de multiplier — division entière qui vaut 1 pour tout intervalle dans
// [60 s, 120 s[, donc muette si ce réglage passait un jour à 90 s.
static_assert((uint64_t)kFiltrationMinRepresentativeSamples * kFiltrationTempSampleIntervalMs
                <= 3600000ULL - kPurgeFloorMs,
  "Seuil de representativite inatteignable sur le cycle auto le plus court.");
static_assert(kPurgeRecloseRangePh == 2.0f * kPurgeStablePhRange &&
                  kPurgeRecloseRangeOrp == 2.0f * kPurgeStableOrpRange,
              "Hysteresis de re-fermeture != 2 x seuil d'ouverture (C1)");
// feature-097 : l'égalité ×2 ci-dessus ne dit rien du SPAN réel de la fenêtre
// (W-1 intervalles) — elle ne garantit la détection de dérive « par
// coïncidence » qu'à W = 10 fixé. On lui adjoint l'invariant explicite : la
// dérive minimale qui DOIT ENCORE être détectée (kPurgeMinDetectedDriftPhPerMin),
// une fois convertie en étendue sur la durée réelle couverte par W-1
// intervalles, doit rester AU-DESSUS du seuil de re-fermeture — sinon une
// dérive à cette vitesse ne le franchirait jamais. Vaut
// 0,035 * 9 * 12 * 5000 / 60000 = 0,315 ; seuil 0,30 ⇒ 0,30 <= 0,315 ✓
// aujourd'hui (0,035 pH/min EST détectée, cf. test F5 / condition C4).
static_assert(kPurgeRecloseRangePh <=
                  kPurgeMinDetectedDriftPhPerMin *
                      (float)(kPurgeWindowMin - 1) * (float)kPurgeSamplesPerMin *
                      (float)kPhOrpSensorIntervalMs / 60000.0f,
              "Reclose pH au-dela de la derive minimale qui doit rester "
              "detectee sur le span reel de la fenetre (C1) : la geometrie a "
              "change sans que l'hysteresis ait ete revalidee");
static_assert(kPurgeStablePhRange > 0.0f && kPurgeStableOrpRange > 0.0f,
              "Seuil de stabilite nul : la porte ne s'ouvrirait qu'au plafond");
// feature-097 : l'assert qui aurait rendu visible dès le 2026-09-04 que le
// seuil d'ouverture était en-dessous du plancher de bruit mesuré (ce n'était
// PAS le cas — cf. spec feature-097 §Verdict — mais rien ne le garantissait).
static_assert(kPurgeStablePhRange > kPurgeNoiseFloorPh,
              "Seuil de stabilite pH sous le plancher de bruit mesure : la "
              "porte n'ouvrirait jamais avant le plafond");
static_assert(kPurgeStableOrpRange > kPurgeNoiseFloorOrp,
              "Seuil de stabilite ORP sous le plancher de bruit mesure : la "
              "porte n'ouvrirait jamais avant le plafond");
static_assert(kPurgeWindowMin >= 2 && kPurgeWindowMin <= 10,
              "Fenetre de stabilite hors de [2 ; 10] min");
static_assert(kPurgeSamplesPerMin >= 3 && kPurgeSamplesPerMin <= 60,
              "Nombre de lectures par minute hors de [3 ; 60]");
static_assert(kShortOutageS > 0 && kShortOutageS <= 30,
              "Coupure courte hors de ]0 ; 30] s : au-dela l'eau de la conduite "
              "peut avoir ete renouvelee sans que la purge le voie");
static_assert(kOutageWaiverMinIntervalS >= 600,
              "Intervalle minimal entre deux dispenses < 10 min");

// Anti-rafale dosage chimique (pool-chemistry feature-021, Pass 3.5).
// Limite le nombre de DÉMARRAGES de cycle de dosage par fenêtre glissante
// (indépendant des limites journalières/horaires existantes). Couvre les cas
// PID instable, oscillation autour de la cible, capteur bruité, etc.
constexpr uint8_t kMaxDosingCyclesPerMinute = 6;     // 1 cycle / 10s max
constexpr uint8_t kMaxDosingCyclesPer15Min  = 20;    // anti-emballement PID
constexpr size_t  kDosingCycleHistorySize   = 20;    // ring buffer (couvre 15min)

// Cadence du WARN « démarrage décidé mais aucun débit commandé » (v2.20.5).
// Décrit une régulation SANS EFFET : la décision est prise, aucun duty ne sort.
// Avant le correctif du double comptage, ce cas produisait un log de démarrage à
// chaque tour — bruit involontaire qui a révélé le bug ; il serait désormais
// silencieux. 5 min : assez rare pour ne pas noyer le journal (cf. l'inondation
// corrigée en 2.20.2), assez fréquent pour être vu dans la journée.
constexpr unsigned long kDosingNoFlowWarnIntervalMs = 300000UL;  // 5 min
// Nombre de tours CONSÉCUTIFS « décision de doser sans débit » avant d'alerter.
// Indispensable : le premier tick d'un cycle est nul par construction (garde
// dt > 10 s de computePID), donc alerter dès le 1er tour produirait un WARN par
// cycle, soit ~24/jour en fonctionnement SAIN. En régime normal la série vaut 1,
// le tour suivant commandant un débit.
constexpr uint16_t kDosingNoFlowWarnStreak = 5;

// Bornes dures de la garde 9 anti-cycling (maxCyclesPerDay configurable,
// feature-060 — verdict pool-chemistry 2026-07-29, GO sous conditions).
// Appliquées par clamp CÔTÉ SERVEUR (route de config) ET au chargement NVS
// via clampCyclesPerDay() (dosing_logic). Ne JAMAIS élargir sans nouveau
// passage pool-chemistry.
constexpr unsigned int kCyclesPerDayMin = 10;  // plancher : en dessous, la garde bloque un régime d'entretien sain
constexpr unsigned int kCyclesPerDayMax = 60;  // plafond : sous la cadence physique ~70/j imposée par la pause mélange

// ============================================================================
// PLAFOND DE SESSION DE DOSAGE pH (v2.26.0 — condition bloquante pool-chemistry)
// ============================================================================
// POURQUOI CES DEUX CONSTANTES EXISTENT
// -------------------------------------
// La pause mélange (kPhMixingDelayMs) n'est armée qu'à la FIN d'une session de
// dosage. Depuis l'alignement de la bande morte sur l'hystérésis (la bande vaut
// phStopThreshold dès que la session est ouverte), le correcteur PI se parque
// exactement au point où l'hystérésis coupe — `consigne + phStopThreshold` — et
// l'erreur ne repasse donc JAMAIS franchement sous le seuil d'arrêt. Conséquence
// mesurée en boucle fermée : la session ne se referme plus, notifyPhDose() n'est
// plus jamais appelé, la pause mélange CESSE D'EXISTER et l'appareil dose en
// continu sur une eau dont l'homogénéisation n'est plus jamais attendue.
//
// L'alignement lui-même est nécessaire (sans lui l'intégrale remonte à 100 % du
// plafond, le point fixe étant inatteignable) : on ne revient donc pas dessus, on
// referme son effet de bord par un PLAFOND DE SESSION explicite.
//
// DEUX CRITÈRES, PAS UN — ils ne bornent pas la même chose :
//
//   * kPhSessionMaxOnMs (temps POMPE-ON cumulé) borne le VOLUME injecté entre
//     deux homogénéisations. C'est le critère CHIMIQUE : à l'amplitude calibrée
//     (~26 mL/min mesurés au banc), 60 min de pompe-ON valent ~1,5 L d'acide,
//     soit l'ordre de grandeur d'une descente complète de pH sur un bassin de
//     30-50 m³ — au-delà, continuer sans re-regarder l'eau serait doser à
//     l'aveugle. Il se déclenche d'autant plus vite que la correction est
//     agressive, ce qui est exactement le comportement voulu.
//
//   * kPhSessionMaxWallMs (durée MURALE de session) borne la PÉREMPTION de
//     l'hypothèse d'homogénéité. En régime établi la pompe ne tourne qu'environ
//     10 % du temps : le critère pompe-ON seul ne se déclencherait qu'au bout de
//     ~10 h. Ce second critère garantit qu'une pause mélange a lieu au moins
//     toutes les 3 h, quelle que soit la demande.
//
// DIMENSIONNEMENT — la contrainte dure est de NE PAS hacher les sessions ni de
// saturer le quota `maxCyclesPerDay` (défaut 24, plancher configurable 10) :
//   - pire cas (pompe en permanence) : une session dure kPhSessionMaxOnMs et est
//     suivie de kPhMixingDelayMs ⇒ 24 h / 75 min = 19 démarrages/jour < 24 ;
//   - régime établi (~10 % de marche) : c'est le critère mural qui tranche ⇒
//     24 h / (3 h + 15 min) ≈ 7 démarrages/jour, très en deçà du plancher de 10.
// La pause mélange coûte au plus 15 min / 3 h 15, soit 7,7 % du temps de dosage :
// l'excursion de pH pendant la pause reste sous le seuil de DÉMARRAGE + sa marge,
// donc la reprise est immédiate et sans à-coup.
// CHIFFRE CORRIGÉ (gate pool-chemistry, feature-094, 05/09/2026) : cette excursion
// valait « ~0,09 pH » ici depuis l'origine. Les données la RÉFUTENT — bilan matière
// 1,35 mL/min × 15 min × 0,0004 pH/mL = 0,008 pH, et la pause sans injection de
// 50 min du 03/09 ne montre aucune montée (−0,02 ± 0,02). Le 0,09 dérivait d'une
// charge de 2,86 mL/min ; la charge réellement mesurée est de 1,35 mL/min (966 mL
// sur ~12 h le 03/09, 661 mL sur ~8 h le 04/09). NE PAS réutiliser 0,09 comme
// budget d'excursion : ce serait s'autoriser cinq fois la réalité.
constexpr unsigned long kPhSessionMaxOnMs   = 3600000UL;   // 60 min de POMPE-ON cumulé
constexpr unsigned long kPhSessionMaxWallMs = 10800000UL;  // 3 h de session (temps mural)

// Le critère mural ne doit jamais couper une session AVANT le critère pompe-ON
// (sinon le second serait décoratif) : à 100 % de marche ils coïncideraient.
static_assert(kPhSessionMaxOnMs <= kPhSessionMaxWallMs,
              "Plafond pompe-ON > plafond mural : le critère chimique deviendrait inatteignable");
// Le plafond doit rester très au-dessus du temps minimum d'injection configurable
// (défaut 30 s, cf. pumpProtection.minInjectionTimeMs) : sans cette marge, le
// plafond et la garde de temps minimum se contrediraient à chaque session.
static_assert(kPhSessionMaxOnMs >= 600000UL,
              "Plafond de session pompe-ON trop bas : conflit avec minInjectionTimeMs");
// Cadence pire cas (pompe en continu) : le plafond ne doit pas à lui seul épuiser
// le quota de démarrages journaliers, sinon il fermerait la régulation pour la
// journée — l'exact opposé du but recherché.
//
// LIMITE CONNUE (revue v2.26.0), volontairement NON corrigée ici : la borne 24
// est le quota par DÉFAUT (`maxCyclesPerDay`), alors que la plage configurable
// descend jusqu'à `kCyclesPerDayMin` = 10. Cadence pire cas réelle : 86 400 000 /
// (60 min + 15 min) = **19 démarrages/jour**. Un utilisateur qui configurerait
// 10 cycles/jour ET soutiendrait une correction en continu pourrait donc voir le
// plafond de session épuiser son quota.
// Pourquoi ne pas simplement comparer à `kCyclesPerDayMin` : l'assertion
// échouerait (19 > 10). La rendre vraie exigerait de porter `kPhSessionMaxOnMs`
// au-delà de **129 min** de pompe-ON, soit ~3,2 L injectés entre deux
// homogénéisations — ce qui contredirait le critère chimique que ce plafond
// existe précisément pour faire respecter. L'arbitrage (allonger le plafond,
// relever le plancher de quota, ou assumer le cas) relève de `pool-chemistry`,
// pas d'un ajustement d'assertion. Le régime nominal reste très en deçà :
// 6,4 démarrages/jour mesurés en simulation.
static_assert(86400000UL / (kPhSessionMaxOnMs + kPhMixingDelayMs) < 24UL,
              "Plafond de session trop court : le quota cycles/jour serait sature");
// Cadence en régime établi : au moins 8 sessions de large devant la pause mélange,
// pour que la pause reste marginale devant le temps de dosage.
static_assert(kPhSessionMaxWallMs >= 8UL * kPhMixingDelayMs,
              "Session murale trop courte devant la pause mélange (hachage des sessions)");

// ============================================================================
// PORTE DE SESSION pH ROBUSTE AU BRUIT (feature-094 / v2.36.0 — gate
// pool-chemistry « GO sous conditions » du 05/09/2026)
// ============================================================================
// Le POURQUOI complet est dans src/ph_session_logic.h. Résumé chiffré : la
// filtrée bruite à σ = 0,043 sur 60 s (crête-à-crête 15 min ≈ 0,150) pour une
// bande de décision de 0,05/0,01 — près de la consigne, c'est le BRUIT qui ouvre
// et ferme les sessions. Caractérisation du gate : τc ≈ 50 s, période dominante
// ≈ 5 min ⇒ processus quasi blanc ⇒ moyenner marche.

// L1 — FENÊTRE DU SIGNAL LENT DE DÉCISION. 10 min ramènent σ à ≈ 0,017 pour
// ≈ 7 min de retard de décision, ce qui reste très en deçà de la pause mélange
// (15 min) déjà tolérée par conception. Sert AUSSI de durée de remplissage :
// aucune décision n'est prise tant que la fenêtre n'est pas pleine
// d'échantillons post-reprise de filtration (le transitoire du 04/09 a duré
// 40 min).
constexpr uint32_t kPhDecisionWindowMs = 600000UL;  // 10 min

// C4 — MARGE DE FERMETURE DE PORTE. La porte ferme sur
// `signal lent < phStopThreshold − 0,03`, soit −0,02 : il faut une descente
// FRANCHE sous la consigne pour refermer, alors que l'ouverture reste inchangée
// à `> phStartThreshold`. Cette marge ne touche PAS la bande d'actionnement
// (bande morte du PI) : le point d'équilibre reste `consigne + phStopThreshold`.
constexpr float kPhDoorCloseMarginPh = 0.03f;

// L3/C5 — DURÉE MINIMALE DE SESSION, en temps MURAL. Une session ouverte ne se
// referme pas sur un creux de bruit : elle tient 30 min murales, sauf garde
// (C1, priorité absolue) et sauf sous-dépassement franc (C6, ci-dessous).
// POURQUOI MURAL ET NON POMPE-ON : au rapport cyclique mesuré près de la
// consigne (3,4-5,3 %), 30 min de pompe-ON dureraient ~10 h murales — la borne
// serait inatteignable et la durée minimale, décorative.
// Sur-injection bornée : pendant la prolongation le débit est plafonné au
// feedforward (2,60 mL/min sur la config vivante, C7) puis annulé dès que le
// signal lent passe sous la consigne. Sous-dépassement pire cas calculé par le
// gate : −0,058 pH → 7,242, au-dessus du plancher acide 7,20.
constexpr unsigned long kPhSessionMinWallMs = 1800000UL;  // 30 min murales

// C6 — PLANCHER DE SOUS-DÉPASSEMENT. Le signal LENT passé sous −0,05 n'est plus
// du bruit : le bassin est franchement de l'autre côté de la consigne ⇒
// fermeture immédiate, qui PRIME sur la durée minimale. Identique par
// construction au latch défavorable du feedforward (kPhFfAdverseErrorPh) :
// deux organes qui constatent le même événement physique ne doivent pas le
// constater à deux valeurs différentes (static_assert d'identité ci-dessous).
constexpr float kPhSessionUndershootFloorPh = 0.05f;

// --- Verrous de cohérence (condition C12 du gate) ---------------------------
// La durée minimale + sa pause mélange doivent tenir DANS le plafond mural,
// sinon le plafond couperait des sessions que la durée minimale prolonge encore.
static_assert(kPhSessionMinWallMs + kPhMixingDelayMs <= kPhSessionMaxWallMs,
              "Duree minimale + pause melange depassent le plafond mural de session");
// Une session doit durer au moins deux fenêtres de décision : en deçà, elle se
// refermerait avant que le signal lent ait pu constater l'effet du dosage.
static_assert(kPhSessionMinWallMs >= 2UL * (unsigned long)kPhDecisionWindowMs,
              "Duree minimale de session sous deux fenetres du signal lent");
// Identité EXIGÉE avec le seuil défavorable du feedforward (cf. ci-dessus).
static_assert(kPhSessionUndershootFloorPh == kPhFfAdverseErrorPh,
              "Plancher de sous-depassement desaccorde du latch defavorable du feedforward");
// La marge de fermeture doit être strictement positive (sinon la porte
// refermerait au point de coupure exact, régression v2.26.0) et rester très en
// deçà de la marge de purge sèche de l'intégrale (0,10), qui décrit un TOUT
// AUTRE événement (perturbation externe franche).
static_assert(0.0f < kPhDoorCloseMarginPh && kPhDoorCloseMarginPh < kPhIntegralResetMargin,
              "Marge de fermeture de porte hors plage (0 < marge < kPhIntegralResetMargin)");

// ============================================================================
// TEMPS D'ATTEINTE DE CIBLE — instrument passif (feature-096 / v2.37.0)
// ============================================================================
// Le POURQUOI complet est dans src/time_to_target_logic.h. Ces cinq valeurs
// définissent la MÉTRIQUE DE RÉFÉRENCE du plan 2026-09 : les modifier change le
// sens de toutes les journées déjà archivées, et rend les comparaisons de
// versions caduques.

// Tenue exigée dans la bande pour déclarer la cible ATTEINTE, en minutes de
// FILTRATION consécutives. 30 min = trois fois la fenêtre du signal lent de
// décision (kPhDecisionWindowMs) : une tenue plus courte serait indiscernable
// du bruit de mesure (σ = 0,043 sur 60 s, cf. feature-094).
constexpr uint16_t kTimeToTargetHoldMin = 30;

// Profondeur de la mémoire glissante : 7 journées par régulation (une semaine =
// le cycle naturel de comparaison avant/après un déploiement).
constexpr uint8_t kTimeToTargetHistoryDays = 7;

// Arrêt de filtration au-delà duquel la JOURNÉE est déclarée close (minutes
// MURALES). Couvre d'un seul critère la fin de plage horaire et le passage en
// OFF : au-delà d'une heure sans circulation, la journée de régulation est
// terminée, qu'un cycle du soir reprenne ou non.
constexpr uint16_t kTimeToTargetOffDayEndMin = 60;

// Garde de VRAISEMBLANCE sur l'écart initial e0. Au-delà, l'indicateur REFUSE
// de poser t0 (il n'invente jamais de valeur) et attend le premier échantillon
// plausible. Cas réel du 05/09/2026 : brut 5,45 avec de l'air dans le circuit,
// soit e0 = −1,75 — une mesure d'installation, pas une mesure de bassin.
constexpr float kTttMaxPlausibleE0Ph  = 1.0f;    // pH
constexpr float kTttMaxPlausibleE0Orp = 300.0f;  // mV

// La tenue doit couvrir au moins deux fenêtres du signal lent, faute de quoi
// elle attesterait d'une moyenne qui n'a pas fini de se constituer.
static_assert((unsigned long)kTimeToTargetHoldMin * 60000UL >=
                  2UL * (unsigned long)kPhDecisionWindowMs,
              "Tenue d'atteinte sous deux fenetres du signal lent de decision");

// Répartition du volume quotidien du mode "scheduled" (feature-011).
// Le volume restant est réparti uniformément par fenêtres de 15 min sur
// l'horizon de filtration restant, borné à minuit (les compteurs journaliers
// se réinitialisent à minuit). Décision pure : evaluateScheduledDose()
// (src/dosing_logic.*) ; horizon : remainingRangeMinutes() (src/schedule_logic.*).
constexpr int kScheduledWindowMinutes = 15;  // Fenêtre de répartition scheduled (windowIndex = nowMin / 15)

// ============================================================================
// BOOST CONSTANTS - Mode Boost / surchloration temporaire (feature-053)
// ============================================================================
// Surcouche temporaire NON destructive activable d'un geste, auto-expirant au
// prochain minuit local. N'a d'effet chimique que si la régulation ORP est en
// mode "automatic" (cible + limite journalière). Bornes FIGÉES par pool-chemistry
// (feature-053, double passage) — ne JAMAIS élargir sans nouveau passage.

// +60 mV sur la cible ORP : relève sensiblement le résidu chloré (effet réel)
// tout en restant dans la plage physiologique d'une eau baignable une fois le
// pic redescendu. Marge choisie prudente vs. la borne dure ORP.
constexpr float kBoostOrpDeltaMv = 60.0f;

// Plafond ABSOLU de la cible ORP effective boostée : 850 mV (PAS 900). Le code
// lève l'alerte orpAbnormal dès orp > 900 mV ; plafonner la cible à 850 laisse
// une marge de sécurité de 50 mV avant cette alerte et évite de piloter vers un
// ORP jugé anormal.
constexpr float kBoostOrpCeilingMv = 850.0f;

// ×1,5 sur la limite journalière chlore pendant le boost : donne un budget réel
// de surchloration sur la journée sans multiplier le risque de surdosage. Effet
// borné en dur par kBoostDailyHardCapMl ci-dessous.
constexpr float kBoostDailyFactor = 1.5f;

// Plafond dur ABSOLU de la limite journalière chlore effective boostée : 1000 mL.
// Quel que soit maxChlorineMlPerDay × facteur, la limite effective ne dépasse
// jamais cette valeur — garde-fou anti-surdosage même si l'utilisateur a une
// limite normale déjà élevée.
constexpr float kBoostDailyHardCapMl = 1000.0f;

// ============================================================================
// OTA INTEGRITY CONSTANTS - Vérification d'intégrité SHA-256 (feature-026)
// ============================================================================

// Empreinte SHA-256 : 32 octets = 64 caractères hexadécimaux.
// Les buffers d'affichage doivent faire kOtaSha256HexLen + 1 (terminateur).
constexpr size_t kOtaSha256HexLen = 64;

// Préfixe des digests d'assets GitHub (champ `digest` de l'API releases).
constexpr const char* kOtaSha256Prefix = "sha256:";

// ============================================================================
// NETWORK CONSTANTS - Paramètres réseau
// ============================================================================

// Ports réseau
constexpr uint16_t kHttpServerPort = 80;                  // Port serveur HTTP
constexpr uint16_t kMdnsHttpPort = 80;                    // Port mDNS pour HTTP

// mDNS
constexpr const char* kMdnsHostname = "poolcontroller";       // Nom d'hôte mDNS (sans suffixe)
constexpr const char* kMdnsFullHost = "poolcontroller.local"; // FQDN mDNS (utilisé par UI/HTTP/WS pour les liens)
// Debounce ré-annonce mDNS (v2.9.1) : en double NAT CPL, l'IP peut osciller et
// déclencher un churn MDNS.end()/begin() coûteux à chaque changement. On borne la
// fréquence de ré-annonce pour ignorer les oscillations transitoires.
constexpr unsigned long kMdnsReannounceMinIntervalMs = 30000; // 30s - intervalle mini entre 2 ré-annonces mDNS

// Wi-Fi
constexpr uint32_t kWifiConnectTimeoutMs = 15000;          // Timeout connexion Wi-Fi (ms)

// Watchdog de connectivité (v2.9.2) : sonde ICMP active vers la passerelle pour
// détecter un gel de la pile lwIP (web/mDNS/ping ICMP morts alors que le WiFi reste
// associé, heap sain) que le watchdog de tâche ne voit pas (thread tcpip lwIP non
// surveillé). Indépendant de MQTT (souvent désactivé) : le signal de vie vient du
// ping ICMP passerelle (+ succès NTP en défense en profondeur).
constexpr uint32_t kNetworkProbeIntervalMs = 30000;   // 30s - cadence sonde ICMP passerelle
constexpr uint32_t kNetworkStallSoftMs     = 120000;  // 2 min - stall → soft recover (reconnexion WiFi)
constexpr uint32_t kNetworkStallRebootMs   = 300000;  // 5 min - stall → reboot fail-safe
constexpr uint32_t kNetworkProbeTimeoutMs  = 2000;    // 2s - timeout echo ICMP

// bug-134 : réarmement plafonné pour les preuves de vie AUTO-PRODUITES par la
// récupération elle-même (IP DHCP obtenue, sonde ICMP relancée) — cf.
// net_health_logic.h (NetLifeSign, evaluateNetRearm). Sans plafond, ces preuves
// financent indéfiniment leur propre report et le reboot fail-safe n'arrive
// jamais (boucles A et B démontrées dans la spec bug-134).
constexpr uint8_t  kNetworkSelfRearmMax   = 2;        // Crédit max de réarmements auto-produits avant épuisement
constexpr uint32_t kNetworkProbeSilenceMs = 90000;    // 90s - silence sonde ICMP (3 échos manqués à kNetworkProbeIntervalMs) → recréation forcée

// ============================================================================
// TIME CONSTANTS - Valeurs temporelles de référence
// ============================================================================

// Epoch minimal considéré comme valide (heure synchronisée NTP/RTC)
constexpr time_t kMinValidEpoch = 1700000000;  // 14 nov. 2023

// ============================================================================
// PLAFONDS DE CORPS JSON PAR ROUTE — bug-113 (clos par le lot bug-101, v2.48.0)
// ============================================================================
// Trois routes utilisent AsyncCallbackJsonWebHandler, qui TAMPONNE le corps
// entier (calloc(total + 1), AsyncJson.cpp:216) AVANT d'appeler le callback,
// donc AVANT toute authentification. Le plafond de la bibliothèque vaut 16 384 o
// et n'était JAMAIS surchargé : 3 URI × 16 Ko réclamables sans identifiants.
//
// ⚠️ LE MOTIF DE CES PLAFONDS N'EST PAS « ÇA VA PLANTER » — la prémisse
// d'origine de bug-113 est FAUSSE depuis le lot B-1 de bug-080, et il faut le
// dire ici pour que personne ne la redécouvre de travers.
// Chaîne établie PAR DÉSASSEMBLAGE le 2026-09-07 (libheap.a, section .iram1.2) :
// calloc → heap_caps_malloc_default → si `size > malloc_alwaysinternal_limit`,
// l'allocateur charge 0x1400 = MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM, donc
// PSRAM D'ABORD, avec repli interne seulement si la PSRAM rend NULL. Ce seuil
// est posé à kPsramExtmemThresholdBytes par main.cpp (juste après initOomGuard).
// Un tampon de 16 Ko ne partait donc PAS dans le tas 8 bits affamé.
// (Valeur du seuil : 4 095 à l'écriture de ce commentaire, 1 024 depuis
// feature-124 — le raisonnement est inchangé, seule la frontière descend.)
// Le plafond bas reste juste, mais pour une autre raison, et elle est plus
// solide : sans lui, la sûreté de ce chemin reposerait ENTIÈREMENT sur une
// PRÉFÉRENCE d'allocateur, dont le repli interne est le contre-exemple. Ce dépôt
// s'est déjà trompé dans l'autre sens — le parachute anti-OOM a vécu en PSRAM,
// donc décoratif, depuis feature-066 (prouvé le 05/09/2026). Un plafond bas est
// vrai indépendamment de l'état de la PSRAM ; une préférence, non.
//
// ⚠️ COROLLAIRE CONTRE-INTUITIF, à ne surtout pas « optimiser » : découper les
// lots PLUS FIN ramènerait l'allocation DANS le tas affamé (« taille <= seuil
// ⇒ interne », test inclusif). Le budget de lot client ci-dessous est donc
// choisi AU-DESSUS de kPsramExtmemThresholdBytes, À DESSEIN. Énoncé par rapport
// au SEUIL et non à une valeur figée : la frontière est passée de 4 095 à 1 024
// (feature-124), et un chiffre en dur serait devenu faux.
//
// Chaque plafond est chiffré sur la CHARGE RÉELLE de sa route, jamais « par
// ordre de grandeur ».
// ----------------------------------------------------------------------------

// POST /history/import — le client découpe (kHistoryImportChunkBudgetBytes) et
// pose `mode:"append"` sur les lots suivants ; l'appareil n'a donc plus besoin
// de capacité pour une restauration complète, seulement pour UN lot.
// Décompte : enveloppe {"mode":"append","history":[…]} ≈ 32 o, puis, par point
// (7 clés seulement — `parseHistoryCsv` n'émet ni `phFiltered` ni `orpFiltered`) :
//   - point typique (nulls, %.1f) ......... ~105-109 o → ~64 points
//   - décimales déployées, MESURÉ .......... 148 o → ⌊(7168−32)/148⌋ = 48 points
//   - maximum THÉORIQUE .................... 172 o → 41 points
// Les trois chiffres viennent de l'EXÉCUTION de `parseHistoryCsv` et de
// `chunkHistoryPoints` extraites de data/app.js (2026-09-07), pas d'un décompte
// à la main :
//   · 148 o = pire cas de la fixture test/fixtures/history-worst-bytes.csv
//     (16 chiffres significatifs sur pH, ORP et température, tout positif) ;
//   · 172 o = squelette de clés (97 o) + 3 × 25 o, 25 étant la longueur JSON
//     maximale d'un double (balayage de tous les exposants : le pire est
//     `-0.0000012345678901234567`, atteint dès qu'une valeur passe sous 1e-6 —
//     donc un ORP qui traverse zéro, ou une température proche de 0 °C).
// ⚠️ HISTORIQUE DES CHIFFRES FAUX, pour ne pas les voir revenir : 211-213 o
// (supposait à tort les colonnes filtrées), 149 o (annoncé comme « le pire cas
// réellement atteignable » : c'est un cas particulier, l'ORP négatif), et 157 o
// (présenté comme une « borne conservatrice » alors que le maximum théorique de
// 172 o la DÉPASSE — ce n'était pas une borne).
// Aucun de ces chiffres, faux ou juste, n'est écrit dans le code : le découpage
// se fait par OCTETS MESURÉS côté client (jsonByteLength), jamais par un nombre
// de points. C'est ce qui a rendu l'invariant immunisé à quatre chiffres
// successivement erronés (ADR-0073) — et la raison de ne jamais remplacer cette
// mesure par une division. Vérifié : 200 points au pire cas → 5 lots, charge
// maximale 7 085 o, aucun 413.
// 8 192 o accepte ces lots avec 1 024 o de marge sur le budget client.
constexpr size_t kHistoryImportMaxBodyBytes = 8192;

// POST /wifi/connect — lit `ssid` (≤ 32 o, limite 802.11) et `password`
// (≤ 63 o, limite WPA2-PSK). Pire cas de sérialisation : chaque octet échappé
// en \uXXXX, soit ×6 → (32 + 63) × 6 = 570 o, plus les clés et la ponctuation
// ≈ 600 o. 1 024 o = ×1,7.
// ⚠️ PAS 512 : à 512 un SSID UTF-8 pathologique serait REFUSÉ — et cette route
// porte le flux de PREMIER DÉMARRAGE, celui qu'on ne peut pas contourner.
constexpr size_t kWifiConnectMaxBodyBytes = 1024;

// POST /sensors/onewire/identify — lit `address` (16 caractères hex, format
// verrouillé par parseRomHex) et `role` (≤ 7 : "water"/"circuit"). Corps réel
// ≈ 45 o. 256 o = ×5,7.
constexpr size_t kSensorIdentifyMaxBodyBytes = 256;

// Budget d'octets d'UN lot d'import côté client (data/app.js). 1 024 o sous le
// plafond de route : la marge absorbe l'enveloppe, l'échappement d'un caractère
// inattendu et la variabilité de longueur des nombres, sans jamais frôler le
// 413. Et surtout : > kPsramExtmemThresholdBytes, pour que le tamponnage garde
// la PRÉFÉRENCE PSRAM (cf. corollaire ci-dessus).
constexpr size_t kHistoryImportChunkBudgetBytes = 7168;

// Total exposé sans authentification : 8 192 + 1 024 + 256 = 9 472 o, contre
// 3 × 16 384 = 49 152 o auparavant, soit −80,7 % (1 − 9472/49152 = 0,8073).
static_assert(kHistoryImportMaxBodyBytes + kWifiConnectMaxBodyBytes +
                  kSensorIdentifyMaxBodyBytes == 9472,
              "Total expose sans authentification (bug-113) : le chiffre -80,7 % "
              "documente 9 472 o. Toute revision d'un plafond doit RECOMPTER ce "
              "total, et la doc avec.");
static_assert(kHistoryImportMaxBodyBytes < 16384 &&
                  kWifiConnectMaxBodyBytes < 16384 &&
                  kSensorIdentifyMaxBodyBytes < 16384,
              "Un plafond >= 16384 ne surcharge plus rien : c'est le defaut de "
              "AsyncCallbackJsonWebHandler (AsyncJson.cpp:122).");
static_assert(kHistoryImportChunkBudgetBytes > kPsramExtmemThresholdBytes,
              "Corollaire bug-113 : un lot <= kPsramExtmemThresholdBytes repartirait "
              "dans le tas 8 bits interne affame. Le budget client est AU-DESSUS du "
              "seuil, a dessein — ne pas le reduire pour 'faire plus sur'. "
              "NB feature-124 : ce message disait '<= 4095 o'. La marge est desormais "
              "BIEN plus large (7 168 contre un seuil de 1 024), mais l'invariant est "
              "le meme et il ne doit PAS etre reecrit avec un chiffre en dur.");
static_assert(kHistoryImportChunkBudgetBytes + 1024 == kHistoryImportMaxBodyBytes,
              "La marge de 1 024 o entre budget client et plafond de route est "
              "l'invariant qui empeche un 413 sur un lot nominal.");

#endif // CONSTANTS_H
