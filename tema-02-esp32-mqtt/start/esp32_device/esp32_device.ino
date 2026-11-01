// Device-ul IoT din tema 01, de data aceasta pe un ESP32-C3 Super Mini.
//
// Aceeași logică și același protocol ca device-simulator din tema 01, cu trei
// taskuri FreeRTOS în loc de trei thread-uri Java:
//
//   task periodic --(coada de alarme)--> task sporadic
//        |                                     |
//        +---- telemetry ---> MQTT <--- alarm -+
//                              |
//                          commands --> command_received() --(coada de comenzi)--> task aperiodic ---> status
//
// Gata implementate (nu trebuie modificate):
//   config.h         setările: Wi-Fi, broker, STUDENT, valorile inițiale; DEVICE_ID
//                    se citește din adresa MAC a plăcii
//   connection.h/cpp Wi-Fi, ora exactă (SNTP), clientul MQTT, mqtt_publish(), epoch_ms()
//
// Așa cum este, schița se conectează la Wi-Fi și la broker și afișează pe
// Serial comenzile primite. Taskurile le scrieți voi: TODO 0 ... TODO 3.
// Exemplele de pornire sunt în laboratorul 0 din rt-labs-esp32c3; vezi
// exemple/README.md din folderul temei.
//
// Placa: ESP32C3 Dev Module, USB CDC On Boot: Enabled. Serial Monitor: 115200 baud.

#include <errno.h>

#include "config.h"
#include "connection.h"

// Temperatura cipului, în °C, de la senzorul intern al ESP32-C3.
// Este temperatura siliciului, nu a camerei: imediat după pornirea Wi-Fi-ului
// poate fi în jur de 50 °C, apoi scade în câteva minute spre 35-40 °C.
// Rezoluția este de aproximativ 1 °C. Ca să provocați alarme, puneți pragul
// cu SET_THRESHOLD cu un grad sub valoarea citită.
float read_temperature(void) {
    return temperatureRead();
}

// Elementele cozilor. xQueueSend copiază elementul, deci ele conțin datele
// însele (prin valoare), nu pointeri.
struct AlarmEvent {
    float temperature;  // temperatura citită de jobul periodic
    float threshold;    // pragul valabil în momentul citirii
};

struct Command {
    char text[COMMAND_MAX_LEN];  // copia textului primit în command_received()
};

// capacitatea cozilor, în elemente
const UBaseType_t ALARM_QUEUE_LEN = 10;
const UBaseType_t COMMAND_QUEUE_LEN = 10;

// Prioritățile taskurilor (între 2 și 19, vezi setup()). Planificatorul rulează
// mereu taskul gata de execuție cu prioritatea cea mai mare:
//   sporadic  - alarmele sunt cele mai urgente: când jobul periodic pune un
//               eveniment în coadă, taskul sporadic îl preemptează și îl
//               publică imediat;
//   periodic  - eliberările trebuie să respecte grila, deci trece înaintea
//               comenzilor;
//   aperiodic - comenzile nu au termen strict: rulează în fundal, în timpul
//               rămas liber.
const UBaseType_t PRIO_SPORADIC = 4;
const UBaseType_t PRIO_PERIODIC = 3;
const UBaseType_t PRIO_APERIODIC = 2;

// stiva fiecărui task, în octeți (snprintf cu "%.2f" are nevoie de stivă)
const uint32_t TASK_STACK_SIZE = 4096;

// Perioada și pragul: citite de taskul periodic, modificate de taskul aperiodic.
volatile uint32_t period_ms = PERIOD_MS_INITIAL;
volatile float threshold = THRESHOLD_INITIAL;

QueueHandle_t alarm_queue = NULL;    // periodic -> sporadic, elemente AlarmEvent
QueueHandle_t command_queue = NULL;  // command_received() -> aperiodic, elemente Command

// TODO 1: TASK PERIODIC, cu perioada T (inițial PERIOD_MS_INITIAL).
//   Fiecare job:
//   - citește temperatura: read_temperature()
//   - publică telemetria: mqtt_publish("telemetry", "deviceId;timestamp;temperatura")
//     (timestamp = epoch_ms(), tipărit cu %lld; temperatura cu "%.2f")
//   - dacă temperatura depășește pragul (inițial THRESHOLD_INITIAL), pune
//     un eveniment în coada de alarme, fără să se blocheze (timeout 0).
//     Taskul periodic NU tratează alarma: o tratează taskul sporadic.
//   Eliberările respectă grila r_k = r_0 + kT: folosiți așteptare absolută
//   (xTaskDelayUntil), nu relativă (delay, vTaskDelay), ca să nu apară derivă.
//   Perioada și pragul se schimbă prin comenzi (taskul aperiodic), deci sunt
//   variabile globale volatile, citite la fiecare job.
void periodic_task(void* param) {
    Serial.println("[periodic] pornit");
    // r_0: momentul primei eliberări; xTaskDelayUntil îl mută la r_k = r_0 + kT
    TickType_t last_release = xTaskGetTickCount();
    uint32_t job = 0;
    for (;;) {
        job++;
        // un singur citit pe job, ca jobul să lucreze cu o valoare consecventă
        float limit = threshold;
        float t = read_temperature();

        char msg[96];
        snprintf(msg, sizeof(msg), "%s;%lld;%.2f", DEVICE_ID, (long long)epoch_ms(), t);
        mqtt_publish("telemetry", msg);
        Serial.printf("[periodic] job %lu la %lu ms: %s\n", (unsigned long)job, (unsigned long)millis(), msg);

        if (t > limit) {
            AlarmEvent ev = {t, limit};
            // timeout 0: dacă coada e plină, jobul nu așteaptă, evenimentul se pierde
            if (xQueueSend(alarm_queue, &ev, 0) != pdPASS) {
                Serial.println("[periodic] coada de alarme e plină, eveniment pierdut");
            }
        }

        // așteptare absolută: până la last_release + T, apoi last_release += T
        xTaskDelayUntil(&last_release, pdMS_TO_TICKS(period_ms));
    }
}

// TODO 2: TASK SPORADIC, tratează alarmele de temperatură.
//   - așteaptă un eveniment din coada de alarme
//   - dacă de la ultima alarmă publicată au trecut mai puțin de
//     MIN_INTERARRIVAL_MS, respinge evenimentul (afișați un mesaj pe Serial)
//   - altfel publică: mqtt_publish("alarm", "deviceId;timestamp;temperatura;prag")
//   Pentru intervale folosiți un timp monoton, millis(), nu epoch_ms().
void sporadic_task(void* param) {
    Serial.println("[sporadic] pornit");
    bool published_before = false;  // prima alarmă nu are o alarmă anterioară
    uint32_t last_published = 0;    // millis() la ultima alarmă publicată
    for (;;) {
        AlarmEvent ev;
        // blocat fără limită până sosește un eveniment (ca take() din Java)
        xQueueReceive(alarm_queue, &ev, portMAX_DELAY);
        uint32_t now = millis();

        // scăderea pe uint32_t rămâne corectă și când millis() trece prin 0
        uint32_t since_last = now - last_published;
        if (published_before && since_last < MIN_INTERARRIVAL_MS) {
            Serial.printf("[sporadic] alarmă respinsă: %.2f > %.2f, la %lu ms după ultima (minim %lu)\n",
                          ev.temperature, ev.threshold, (unsigned long)since_last,
                          (unsigned long)MIN_INTERARRIVAL_MS);
            continue;
        }

        char msg[96];
        snprintf(msg, sizeof(msg), "%s;%lld;%.2f;%.2f", DEVICE_ID, (long long)epoch_ms(), ev.temperature,
                 ev.threshold);
        mqtt_publish("alarm", msg);
        published_before = true;
        last_published = now;
        Serial.printf("[sporadic] alarmă publicată: %s\n", msg);
    }
}

// TODO 3: TASK APERIODIC, execută comenzile primite de la business service.
//   Comenzile ajung în command_received() (mai jos), care le pune în coada de
//   comenzi; fiecare comandă eliberează un job al taskului:
//     SET_INTERVAL <ms>       schimbă perioada T a taskului periodic
//     SET_THRESHOLD <valoare> schimbă pragul de alarmă
//     RESET                   readuce perioada și pragul la valorile inițiale
//     STATUS                  raportează perioada și pragul curente
//   După fiecare comandă publică o confirmare:
//     mqtt_publish("status", "deviceId;timestamp;OK SET_INTERVAL 1000")
//   O comandă necunoscută sau un parametru invalid nu oprește taskul: se
//   publică "ERROR <motiv>".

// Verifică numărul de parametri de după numele comenzii; la nepotrivire scrie
// motivul în result și întoarce false.
static bool require_args(const char* name, int actual, int expected, char* result, size_t size) {
    if (actual != expected) {
        snprintf(result, size, "%s asteapta %d parametri, a primit %d", name, expected, actual);
        return false;
    }
    return true;
}

// Execută o comandă, la fel ca execute() din AperiodicCommandTask (tema 01).
// Scrie în result textul confirmării, fără "OK " / "ERROR " în față, și întoarce
// true dacă a executat comanda, false dacă e invalidă (result conține motivul).
// command este modificat: strtok_r taie textul în cuvinte.
static bool execute_command(char* command, char* result, size_t size) {
    const char* separators = " \t\r\n";
    char* save = NULL;
    char* name = strtok_r(command, separators, &save);
    if (name == NULL) {
        snprintf(result, size, "comanda goala");
        return false;
    }
    // parametrii: păstrăm primul, dar îi numărăm pe toți
    char* arg = NULL;
    int argc = 0;
    for (char* word; (word = strtok_r(NULL, separators, &save)) != NULL; argc++) {
        if (argc == 0) arg = word;
    }

    // numele comenzii fără să conteze literele mari/mici, ca toUpperCase() din Java
    if (strcasecmp(name, "SET_INTERVAL") == 0) {
        if (!require_args(name, argc, 1, result, size)) return false;
        char* end;
        errno = 0;
        long ms = strtol(arg, &end, 10);
        if (*end != '\0' || errno == ERANGE) {
            snprintf(result, size, "numar intreg invalid: %s", arg);
            return false;
        }
        if (ms <= 0) {
            snprintf(result, size, "intervalul trebuie sa fie pozitiv: %s", arg);
            return false;
        }
        period_ms = (uint32_t)ms;
        snprintf(result, size, "SET_INTERVAL %ld", ms);
        return true;
    }
    if (strcasecmp(name, "SET_THRESHOLD") == 0) {
        if (!require_args(name, argc, 1, result, size)) return false;
        char* end;
        float value = strtof(arg, &end);
        if (*end != '\0' || !isfinite(value)) {
            snprintf(result, size, "numar zecimal invalid: %s", arg);
            return false;
        }
        threshold = value;
        snprintf(result, size, "SET_THRESHOLD %.2f", value);
        return true;
    }
    if (strcasecmp(name, "RESET") == 0) {
        if (!require_args(name, argc, 0, result, size)) return false;
        // senzorul intern nu se poate reseta: revin perioada și pragul inițiale
        period_ms = PERIOD_MS_INITIAL;
        threshold = THRESHOLD_INITIAL;
        snprintf(result, size, "RESET");
        return true;
    }
    if (strcasecmp(name, "STATUS") == 0) {
        if (!require_args(name, argc, 0, result, size)) return false;
        snprintf(result, size, "STATUS interval=%lu threshold=%.2f", (unsigned long)period_ms, threshold);
        return true;
    }
    snprintf(result, size, "comanda necunoscuta: %s", name);
    return false;
}

void aperiodic_task(void* param) {
    Serial.println("[aperiodic] pornit");
    for (;;) {
        // un job pentru fiecare comandă, în ordinea sosirii (FIFO)
        Command cmd;
        xQueueReceive(command_queue, &cmd, portMAX_DELAY);
        Serial.printf("[aperiodic] comanda primita: %s\n", cmd.text);

        char result[96];
        bool ok = execute_command(cmd.text, result, sizeof(result));

        char msg[160];
        snprintf(msg, sizeof(msg), "%s;%lld;%s %s", DEVICE_ID, (long long)epoch_ms(), ok ? "OK" : "ERROR", result);
        mqtt_publish("status", msg);
        Serial.printf("[aperiodic] %s\n", msg);
    }
}

// Apelată de clientul MQTT pentru fiecare comandă primită (vezi connection.h).
void command_received(const char* text) {
    Serial.printf("[mqtt] comandă primită: %s\n", text);
    // text e valid doar în timpul apelului: copiem textul în elementul cozii
    // (trunchiat la COMMAND_MAX_LEN - 1 caractere)
    Command cmd;
    snprintf(cmd.text, sizeof(cmd.text), "%s", text);
    // timeout 0: rulăm pe taskul clientului MQTT, care nu are voie să aștepte
    if (xQueueSend(command_queue, &cmd, 0) != pdPASS) {
        Serial.println("[mqtt] coada de comenzi e plină, comandă pierdută");
    }
}

void setup() {
    Serial.begin(115200);
    // Fără așteptare la scriere: cu placa la USB, dar fără Serial Monitor deschis,
    // fiecare Serial.printf() ar aștepta până la 2 s să fie citit și ar întârzia
    // taskul care scrie. Așa, textul pe care nu-l citește nimeni se pierde.
    Serial.setTxTimeoutMs(0);
    uint32_t t = millis();
    while (!Serial && millis() - t < 3000) delay(10);  // așteptăm Serial Monitor, cel mult 3 s
    delay(200);
    Serial.println();
    Serial.printf("=== tema 02: STUDENT %s, DEVICE_ID %s ===\n", STUDENT, DEVICE_ID);

    wifi_connect();
    time_sync();

    // TODO 0 (a): creați cele două cozi (xQueueCreate): coada de alarme și
    //   coada de comenzi. Înainte de mqtt_start(): de acolo încolo pot sosi
    //   comenzi, iar command_received() le pune în coada de comenzi.
    alarm_queue = xQueueCreate(ALARM_QUEUE_LEN, sizeof(AlarmEvent));
    command_queue = xQueueCreate(COMMAND_QUEUE_LEN, sizeof(Command));
    if (alarm_queue == NULL || command_queue == NULL) {
        Serial.println("[setup] EROARE: nu s-au putut crea cozile");
        for (;;) delay(1000);
    }

    mqtt_start();

    // TODO 0 (b): porniți cele trei taskuri (xTaskCreate). După mqtt_start(),
    //   ca primele mesaje publicate să nu se piardă. Alegeți prioritățile
    //   între 2 și 19: peste loopTask (1), sub taskurile sistemului pentru
    //   evenimente și Wi-Fi (20, 23). Stiva, în octeți: cel puțin 4096.
    bool ok = xTaskCreate(sporadic_task, "sporadic", TASK_STACK_SIZE, NULL, PRIO_SPORADIC, NULL) == pdPASS
           && xTaskCreate(periodic_task, "periodic", TASK_STACK_SIZE, NULL, PRIO_PERIODIC, NULL) == pdPASS
           && xTaskCreate(aperiodic_task, "aperiodic", TASK_STACK_SIZE, NULL, PRIO_APERIODIC, NULL) == pdPASS;
    if (!ok) {
        Serial.println("[setup] EROARE: nu s-au putut porni taskurile");
    }
}

void loop() {
    // Totul se face în taskuri: loop() nu are nimic de făcut.
    delay(1000);
}
