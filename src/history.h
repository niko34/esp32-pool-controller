#ifndef HISTORY_H
#define HISTORY_H

#include <Arduino.h>
#include <vector>
#include <LittleFS.h>
#include <freertos/semphr.h>
#include "constants.h"

enum Granularity : uint8_t {
  RAW = 0,      // Point de données brut (5 minutes)
  HOURLY = 1,   // Moyenne horaire
  DAILY = 2     // Moyenne journalière
};

struct DataPoint {
  unsigned long timestamp;
  float ph;
  float orp;
  float temperature;
  // feature-095 : mesures FILTRÉES (médiane + EMA) telles que vues par la
  // régulation à l'instant de l'enregistrement. NaN = absence (filtre non
  // amorcé, porte de mesure fermée, agrégat HOURLY/DAILY) — JAMAIS 0, et
  // jamais de repli sur le brut.
  // INITIALISEURS OBLIGATOIRES : DataPoint est construit sans initialisation
  // explicite dans /import-history et dans avgPoint de consolidateData() ;
  // sans « = NAN » on persisterait de la mémoire non initialisée, affichée
  // ensuite comme une mesure réelle.
  float phFiltered = NAN;
  float orpFiltered = NAN;
  bool filtrationActive;
  bool phDosing;
  bool orpDosing;
  Granularity granularity;
};

// REVUE feature-095 (mineur n°1) : la taille est VERROUILLÉE, pas supposée.
// C'est elle qui chiffre l'allocation contiguë de /get-history — 339 points ×
// 28 o = 9 492 o demandés d'UN SEUL BLOC en contexte async_tcp, sur un appareil
// dont le plus gros bloc libre tourne autour de 28-30 Ko (bug-080, famine
// 8 bits). Tout champ ajouté ici déplace ce chiffre : le static_assert force à
// le recompter au lieu de le découvrir en production.
static_assert(sizeof(DataPoint) == 28,
              "sizeof(DataPoint) a change : recompter l'allocation contigue de "
              "/get-history (339 points) et le budget de la partition history");

class HistoryManager {
private:
  // Limites de stockage
  static const size_t MAX_RAW_POINTS = kMaxRawDataPoints;      // 8h de données brutes (5 min, cf. constants.h)
  static const size_t MAX_HOURLY_POINTS = kMaxHourlyDataPoints;  // 15 jours de moyennes horaires
  static const size_t MAX_DAILY_POINTS = kMaxDailyDataPoints;    // 75 jours de moyennes journalières
  static const unsigned long RAW_MAX_AGE = 21600UL;        // 6 heures (en secondes)
  static const unsigned long HOURLY_MAX_AGE = 1296000UL;   // 15 jours (en secondes)
  static const unsigned long DAILY_MAX_AGE = 7776000UL;    // 90 jours (en secondes)

  std::vector<DataPoint> memoryBuffer;
  SemaphoreHandle_t _mutex = nullptr;
  unsigned long lastSave = 0;
  unsigned long lastRecord = 0;
  const unsigned long RECORD_INTERVAL = 300000; // 5 minutes
  const unsigned long SAVE_INTERVAL = 300000; // 5 minutes
  bool historyEnabled = true;
  // feature-059 : partition effacée par le disjoncteur de boot-loop pendant begin()
  // (consommé par setup() pour publier l'alerte MQTT différée history_partition_reset).
  bool _partitionAutoErased = false;
  bool legacyHistoryPending = false;
  unsigned long legacyMaxTimestamp = 0;
  bool _preNtpPending = false;  // Points enregistrés avant sync NTP (timestamps uptime provisoires)
  // bug-nospc : epoch du dernier saveToFile réussi (0 si horloge non synchro).
  // Tenu en RAM ; persisté en NVS (last_ok) UNIQUEMENT sur échec de save — usure NVS.
  unsigned long _lastOkEpoch = 0;

  void saveToFile();
  void loadFromFile();
  void consolidateData();
  // bug-history-doublons-troncature (A2) : fusionne les agrégats ADJACENTS de
  // même (timestamp, granularité ≠ RAW) dans memoryBuffer — champs moyennés
  // équipondérés (historyMergeMeanFinite), booléens en OR. PRÉ-REQUIS : buffer
  // trié par timestamp (appelé après chaque std::sort). Assainit aussi, au
  // chargement, les doublons persistés par les versions ≤ 2.18.0.
  void dedupAggregatedPoints();
  void migrateLegacyHistory(unsigned long nowEpoch);
  void _applyPreNtpCorrection(unsigned long ntpEpoch, unsigned long uptimeSec);

public:
  void begin();
  void update();
  void recordDataPoint();

  std::vector<DataPoint> getLastHours(int hours);
  std::vector<DataPoint> getLastDay();
  std::vector<DataPoint> getAllData();
  // bug-history-doublons-troncature (itération 2) : INSTANTANÉ filtré du buffer
  // en UNE prise de mutex (kHistorySnapshotTimeoutMs) — remplace l'accès point
  // par point de /get-history (getPointByIndex), dont les timeouts en cours de
  // flux provoquaient des réponses tronquées (interaction micro-chunks/lib).
  // Copie les points avec timestamp >= cutoff (0 = tous) et > since (0 =
  // désactivé) dans `out` (vidé d'abord). Coût borné : ≤ kMaxRawDataPoints +
  // kMaxHourlyDataPoints + kMaxDailyDataPoints points (~11 Ko). Retour false :
  // mutex indisponible (l'appelant répond 503, cas rare).
  bool snapshotFiltered(std::vector<DataPoint>& out, unsigned long cutoff,
                        unsigned long since);
  bool importData(const std::vector<DataPoint>& dataPoints);
  // feature-027 : retourne false si le mutex n'a pas pu être pris (historique occupé)
  bool clearHistory();
  // feature-059 : true si le disjoncteur de boot-loop a effacé la partition
  // history pendant begin() (alerte MQTT publiée ensuite depuis setup()).
  bool partitionWasAutoErased() const { return _partitionAutoErased; }
};

extern HistoryManager history;

/**
 * @brief Nombre de sauvegardes/chargements d'historique refusés pour FAMINE
 *        MÉMOIRE depuis le boot (bug-100, ADR-0070).
 *
 * ⚠️ À ne pas confondre avec le compteur NVS `kHistoryNospcCountNvsKey`, qui
 * compte les refus d'admission d'ESPACE (« partition pleine », ADR-0033). Les
 * deux pannes ont des causes et des remèdes différents : elles restent
 * discernables, dans le journal comme dans le payload `<base>/diagnostic`.
 */
uint32_t historyFopenDeferralCount();

#endif // HISTORY_H
