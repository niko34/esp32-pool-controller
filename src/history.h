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

// bug-098 : bits de `DataPoint::doseMask`. Ils sont PERSISTES dans
// /history.json (cle courte "d") et servis par /get-history (cle "dosing") :
// ces valeurs sont un CONTRAT, elles ne se renumerotent pas.
constexpr uint8_t kHistDosePh  = 0x01;  // pompe pH active a l'instant du point
constexpr uint8_t kHistDoseOrp = 0x02;  // pompe ORP (chlore) active
// TROISIEME ETAT, et c'est le coeur du correctif. Un point ecrit par un
// firmware <= 2.46 ne portait qu'un booleen « il y a eu un dosage », sans la
// voie : le relire en kHistDosePh INVENTERAIT un dosage pH sur une injection
// ORP seule, donnee fausse ET favorable, que consolidateData() figerait ensuite
// dans les agregats DAILY pour 75-90 jours. On dit donc « dosage, voie
// indeterminee », jamais `false` (cf. time_to_target_logic.h : mieux vaut une
// journee manquante qu'une journee fausse).
constexpr uint8_t kHistDoseUnknownChannel = 0x04;

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
  // bug-098 : les DEUX voies de dosage dans UN champ de bits (kHistDose*), et
  // non deux booleens dont un seul survivait a la serialisation (`obj["d"] =
  // phDosing || orpDosing` puis `phDosing = point["d"]` : la voie etait perdue,
  // et l'ORP seul se relisait en dosage pH). L'idiome du depot : une grandeur
  // DISCRETE porte son absence dans un bit adjacent extensible, jamais dans une
  // valeur de la grandeur elle-meme. 0 = aucun dosage ; bits 3-7 libres (une
  // voie de plus ne coutera pas un octet de DataPoint).
  uint8_t doseMask;
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
  // bug-101 : ramène memoryBuffer sous les plafonds de constants.h, granularité
  // par granularité, en évinçant les PLUS ANCIENS (le buffer doit être TRIÉ par
  // timestamp à l'appel : l'ordre du vecteur EST l'ordre chronologique).
  //
  // APPELÉ UNIQUEMENT PAR appendData() — et ce n'est pas un oubli.
  // Le pas 4 de consolidateData() fait le même bornage pour HOURLY et DAILY, et
  // recordDataPoint() le fait pour RAW : ces deux chemins restent INCHANGÉS.
  // Le mode « remplacer » (importData) reste borné par le plafond de corps de la
  // route (kHistoryImportMaxBodyBytes), donc n'a pas besoin de ce filet. Seul
  // l'ajout par lots peut faire franchir un plafond sans passer par aucun des
  // deux chemins existants.
  // ⚠️ Suppose le mutex DÉJÀ pris par l'appelant.
  void trimToCaps();
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

  /**
   * @brief Ajoute un lot de points à l'historique EXISTANT (bug-101, v2.48.0).
   *
   * Mode « ajouter » de POST /history/import, distinct du mode « remplacer »
   * (importData) : c'est lui qui permet à un client de restaurer une sauvegarde
   * de taille quelconque en lots successifs sous kHistoryImportMaxBodyBytes,
   * sans que chaque lot écrase le précédent.
   *
   * RÈGLE DE COLLISION — (timestamp, granularité) identique ⇒ le point ENTRANT
   * ÉCRASE le point vivant, TOUTES GRANULARITÉS RAW COMPRISE. Justification
   * complète dans history_import_logic.h ; en une phrase : l'écrasement donne
   * l'IDEMPOTENCE EXACTE, donc une reprise après coupure réseau sûre, là où une
   * moyenne équipondérée (historyMergeMeanFinite) fabriquerait une valeur qui
   * n'a jamais été mesurée. ⚠️ Ne pas espérer que dedupAggregatedPoints()
   * couvre ce chemin : il ignore explicitement les points RAW, dont un export
   * porte jusqu'à kMaxRawDataPoints (96).
   *
   * Fusion EN PLACE dans memoryBuffer, par recherche linéaire : AUCUN tampon
   * supplémentaire n'est alloué. Le seul contigu du chemin reste le vecteur
   * existant (339 × 28 = 9 492 o, verrouillé par le static_assert ci-dessus).
   *
   * Prend le mutex historique UNE SEULE FOIS, comme importData : la séquence
   * lecture-modification-écriture-sauvegarde est atomique, un lot ne peut pas
   * s'entrelacer avec le recordDataPoint() de la boucle.
   *
   * @param pts        Points à fusionner (non vide).
   * @param outTotal   [sortie, optionnel] Nombre de points DÉTENUS par
   *                   l'appareil après le lot (taille du buffer).
   * @param outDropped [sortie, optionnel] Nombre de points ÉVINCÉS par
   *                   trimToCaps() pendant cet appel, c'est-à-dire les plus
   *                   anciens de chaque granularité au-delà du plafond. Non nul
   *                   signifie « une partie de ce que vous avez envoyé, ou de ce
   *                   qui était là, ne tient pas » — à remonter au client
   *                   TOUJOURS, même à zéro : la perte muette est le défaut
   *                   d'origine de bug-101.
   * @return true si le lot a été fusionné ; false si l'historique est désactivé,
   *         si `pts` est vide, ou si le mutex n'a pas pu être pris (dans ce
   *         dernier cas RIEN n'a été modifié).
   */
  bool appendData(const std::vector<DataPoint>& pts, size_t* outTotal,
                  size_t* outDropped);
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
