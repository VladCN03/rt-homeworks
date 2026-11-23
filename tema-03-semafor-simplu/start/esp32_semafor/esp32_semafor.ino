// Semafor pentru pietoni, cu buton, pe ESP32-C3 Super Mini.
//
// Placa este controlerul semaforului; lămpile sunt desenate de dashboard, care
// le aprinde după mesajele "lights" publicate de placă. Patru activități:
//
//   ISR buton BOOT ----+
//                      +--(coada butonului)--> task sporadic
//   command_received --+   (BUTTON)                 |
//                                                   v
//                                         [ starea semaforului ] <-- mutex
//                                            ^          |
//                                            |          v
//                         task periodic de control   task periodic de ieșire
//                         (fazele, T = 100 ms)       (lămpile, T = 500 ms) --> lights
//
// Gata implementate (nu trebuie modificate):
//   config.h         setările: Wi-Fi, broker, STUDENT, pinii, perioadele, duratele fazelor
//   connection.h/cpp Wi-Fi, ora exactă (SNTP), clientul MQTT, mqtt_publish(), epoch_ms()
//
// Așa cum este, schița se conectează la Wi-Fi și la broker și afișează pe
// Serial comenzile primite. Restul îl scrieți voi: TODO 0 ... TODO 3.
//
// Placa: ESP32C3 Dev Module, USB CDC On Boot: Enabled. Serial Monitor: 115200 baud.

#include "config.h"
#include "connection.h"

// Fazele semaforului (vezi tabelul și diagrama din README.md). Numele din
// PHASE_NAMES sunt cele din mesajul "lights", în aceeași ordine ca în enum.
enum Phase { CAR_GREEN, CAR_YELLOW, ALL_RED, PED_GREEN };
const char* const PHASE_NAMES[] = {"CAR_GREEN", "CAR_YELLOW", "ALL_RED", "PED_GREEN"};

// Culorile unei lămpi, cu numele din mesajul "lights".
enum Light { RED, YELLOW, GREEN };
const char* const LIGHT_NAMES[] = {"RED", "YELLOW", "GREEN"};

// TODO 0 (c): STAREA SEMAFORULUI, partajată de taskuri și protejată de un
//   mutex (xSemaphoreCreateMutex). De exemplu un struct cu:
//   - faza curentă
//   - momentul intrării în faza curentă (millis()), din care se calculează
//     cât a durat faza și cât mai are
//   - cererea pietonului (lampa AȘTEPTAȚI): setată de taskul sporadic,
//     ștearsă de taskul de control la intrarea în PED_GREEN
//   Mutexul se ține cât mai puțin: în el doar citiți sau scrieți câmpurile
//   (eventual copiați starea într-o variabilă locală). Fără mqtt_publish(),
//   Serial sau așteptări cât timp țineți mutexul.
struct TrafficState {
    Phase phase;               // faza curentă
    uint32_t phase_start_ms;   // millis() la intrarea în faza curentă
    bool ped_request;          // cererea pietonului (lampa AȘTEPTAȚI)
};

// Starea partajată: scrisă de control (faza) și de sporadic (cererea), citită
// de control și de ieșire. Orice acces se face doar cu state_mutex luat.
TrafficState state = {CAR_GREEN, 0, false};
SemaphoreHandle_t state_mutex = NULL;

// Elementul cozii butonului: o apăsare, cu momentul sosirii ei, notat de cel
// care o pune în coadă (ISR-ul sau command_received()). Prin valoare, nu pointer.
struct ButtonPress {
    TickType_t tick;  // xTaskGetTickCount[FromISR]() la sosirea apăsării
};

// capacitatea cozii butonului; bounce-ul poate produce câteva întreruperi
// la o singură apăsare, iar cele care nu mai încap se pierd (oricum ar fi respinse)
const UBaseType_t BUTTON_QUEUE_LEN = 10;
QueueHandle_t button_queue = NULL;

// Prioritățile taskurilor (între 2 și 19):
//   sporadic - lucrează foarte puțin (setează un bool) și trebuie să aprindă
//              repede lampa AȘTEPTAȚI; intervalul minim între sosiri (1 s)
//              îi limitează încărcarea, deci nu poate înfometa celelalte taskuri;
//   control  - Rate Monotonic: perioada cea mai mică (100 ms), prioritate mai mare;
//   ieșire   - Rate Monotonic: perioada mai mare (500 ms), prioritate mai mică.
const UBaseType_t PRIO_BUTTON = 5;
const UBaseType_t PRIO_CONTROL = 4;
const UBaseType_t PRIO_OUTPUT = 3;

// stiva fiecărui task, în octeți (snprintf și mqtt_publish au nevoie de stivă)
const uint32_t TASK_STACK_SIZE = 4096;

// TODO 1: TASK PERIODIC DE CONTROL, cu perioada CONTROL_PERIOD_MS.
//   La fiecare job, cu mutexul luat, decide dacă faza curentă s-a terminat și
//   trece în faza următoare, după tabelul din README.md:
//     CAR_GREEN  -> CAR_YELLOW  după CAR_GREEN_MIN_MS, doar dacă există o cerere
//     CAR_YELLOW -> ALL_RED     după CAR_YELLOW_MS
//     ALL_RED    -> PED_GREEN   după ALL_RED_MS; aici se șterge cererea pietonului
//     PED_GREEN  -> CAR_GREEN   după PED_GREEN_MS
//   La fiecare schimbare de fază notați momentul intrării în faza nouă.
//   Taskul nu publică nimic: doar actualizează starea. După ce eliberează
//   mutexul, afișează pe Serial faza nouă, dacă s-a schimbat.
//   Eliberările respectă grila r_k = r_0 + kT: așteptare absolută (xTaskDelayUntil).
//   Durata unei faze se măsoară cu un timp monoton (millis()), nu cu epoch_ms().
void control_task(void* param) {
    const TickType_t period = pdMS_TO_TICKS(CONTROL_PERIOD_MS);
    TickType_t last_release = xTaskGetTickCount();  // r_0

    for (;;) {
        bool changed = false;
        Phase new_phase;

        xSemaphoreTake(state_mutex, portMAX_DELAY);
        uint32_t now = millis();
        // diferența de uint32_t rămâne corectă și când millis() trece prin 0
        uint32_t elapsed = now - state.phase_start_ms;
        Phase next = state.phase;

        switch (state.phase) {
            case CAR_GREEN:
                if (elapsed >= CAR_GREEN_MIN_MS && state.ped_request) next = CAR_YELLOW;
                break;
            case CAR_YELLOW:
                if (elapsed >= CAR_YELLOW_MS) next = ALL_RED;
                break;
            case ALL_RED:
                if (elapsed >= ALL_RED_MS) next = PED_GREEN;
                break;
            case PED_GREEN:
                if (elapsed >= PED_GREEN_MS) next = CAR_GREEN;
                break;
        }

        if (next != state.phase) {
            state.phase = next;
            state.phase_start_ms = now;  // durata fazei noi se numără de acum
            if (next == PED_GREEN) {
                state.ped_request = false;  // pietonul primește ce a cerut
            }
            changed = true;
        }
        new_phase = state.phase;
        xSemaphoreGive(state_mutex);

        // Serial abia după ce am eliberat mutexul
        if (changed) {
            Serial.printf("[control] faza: %s\n", PHASE_NAMES[new_phase]);
        }

        // eliberarea următoare: r_k = r_0 + kT, fără derivă
        xTaskDelayUntil(&last_release, period);
    }
}

// TODO 2: TASK PERIODIC DE IEȘIRE, cu perioada OUTPUT_PERIOD_MS.
//   La fiecare job:
//   - copiază starea (cu mutexul luat, cât mai scurt)
//   - stabilește culorile celor două lămpi din faza curentă (tabelul din README.md)
//   - calculează secundele rămase din verdele pietonilor: în PED_GREEN,
//     rotunjite în sus (7,2 s rămase -> 8); 0 în celelalte faze
//   - publică: mqtt_publish("lights", "deviceId;timestamp;faza;masini;pietoni;asteptati;secunde")
//     de exemplu "esp32-a1b2c3;1727600000000;PED_GREEN;RED;GREEN;0;8"
//   - aprinde LED-ul de pe placă (LED_PIN, aprins pe LOW) cât timp lampa
//     AȘTEPTAȚI este aprinsă
//   Așteptare absolută (xTaskDelayUntil), ca și la taskul de control.
void output_task(void* param) {
    const TickType_t period = pdMS_TO_TICKS(OUTPUT_PERIOD_MS);
    TickType_t last_release = xTaskGetTickCount();  // r_0
    char msg[128];

    for (;;) {
        // 1. copia stării, cu mutexul ținut doar cât durează copierea
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        TrafficState s = state;
        uint32_t now = millis();
        xSemaphoreGive(state_mutex);

        // 2. culorile lămpilor, din faza copiată
        Light cars = RED;
        Light peds = RED;
        switch (s.phase) {
            case CAR_GREEN:  cars = GREEN;  peds = RED;   break;
            case CAR_YELLOW: cars = YELLOW; peds = RED;   break;
            case ALL_RED:    cars = RED;    peds = RED;   break;
            case PED_GREEN:  cars = RED;    peds = GREEN; break;
        }

        // 3. numărătoarea inversă: secundele rămase din PED_GREEN, rotunjite
        //    în sus (7200 ms -> 8, 300 ms -> 1); 0 în celelalte faze
        uint32_t seconds = 0;
        if (s.phase == PED_GREEN) {
            uint32_t elapsed = now - s.phase_start_ms;
            if (elapsed < PED_GREEN_MS) {
                uint32_t remaining = PED_GREEN_MS - elapsed;
                seconds = (remaining + 999) / 1000;
            }
        }

        // 4. mesajul "lights" și LED-ul plăcii (aprins pe LOW) pentru AȘTEPTAȚI
        snprintf(msg, sizeof(msg), "%s;%lld;%s;%s;%s;%d;%lu",
                 DEVICE_ID, (long long)epoch_ms(), PHASE_NAMES[s.phase],
                 LIGHT_NAMES[cars], LIGHT_NAMES[peds],
                 s.ped_request ? 1 : 0, (unsigned long)seconds);
        if (!mqtt_publish("lights", msg)) {
            Serial.println("[output] mesajul lights nu a putut fi trimis");
        }
        digitalWrite(LED_PIN, s.ped_request ? LOW : HIGH);

        // eliberarea următoare: r_k = r_0 + kT, fără derivă
        xTaskDelayUntil(&last_release, period);
    }
}

// TODO 3: TASK SPORADIC, tratează apăsările butonului pentru pietoni.
//   Apăsările vin din două surse, în aceeași coadă (coada butonului):
//   - butonul BOOT de pe placă, printr-o rutină de întrerupere (ISR, mai jos)
//   - butonul virtual din dashboard: comanda "BUTTON", pusă în coadă de
//     command_received()
//   Taskul:
//   - așteaptă o apăsare din coada butonului
//   - dacă de la ultima apăsare acceptată au trecut mai puțin de
//     MIN_INTERARRIVAL_MS, o respinge (mesaj pe Serial). Așa se filtrează și
//     contactele imperfecte ale butonului (bounce), care produc mai multe
//     întreruperi la o singură apăsare.
//   - altfel înregistrează cererea pietonului în starea semaforului (cu mutexul)
//     și afișează pe Serial că a acceptat-o
//   Momentul sosirii se ia când apăsarea intră în coadă (în ISR sau în
//   command_received()), nu când o preia taskul, și se pune în elementul cozii.
void button_task(void* param) {
    const TickType_t min_interarrival = pdMS_TO_TICKS(MIN_INTERARRIVAL_MS);
    bool have_accepted = false;   // prima apăsare se acceptă mereu
    TickType_t last_accepted = 0; // momentul ultimei apăsări acceptate

    for (;;) {
        ButtonPress press;
        // eliberat de evenimente: blocat până sosește o apăsare
        if (xQueueReceive(button_queue, &press, portMAX_DELAY) != pdTRUE) continue;

        // diferența de TickType_t rămâne corectă și când numărătorul trece prin 0
        TickType_t since = press.tick - last_accepted;
        if (have_accepted && since < min_interarrival) {
            Serial.printf("[buton] apăsare respinsă: la %lu ms după ultima acceptată (minim %lu ms)\n",
                          (unsigned long)(since * portTICK_PERIOD_MS),
                          (unsigned long)MIN_INTERARRIVAL_MS);
            continue;
        }
        have_accepted = true;
        last_accepted = press.tick;

        xSemaphoreTake(state_mutex, portMAX_DELAY);
        bool already = state.ped_request;
        state.ped_request = true;  // mai multe apăsări = tot o singură cerere
        xSemaphoreGive(state_mutex);

        Serial.printf("[buton] apăsare acceptată%s\n",
                      already ? " (cererea era deja înregistrată)" : ": cerere înregistrată");
    }
}

// TODO 3: RUTINA DE ÎNTRERUPERE a butonului BOOT, înregistrată în setup() cu
//   attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), button_isr, FALLING).
//   Scurtă: pune o apăsare în coada butonului cu xQueueSendFromISR() și atât.
//   Fără Serial, fără mqtt_publish(), fără mutex. Se declară cu IRAM_ATTR:
//
//   void IRAM_ATTR button_isr(void) { ... }
void IRAM_ATTR button_isr(void) {
    ButtonPress press;
    press.tick = xTaskGetTickCountFromISR();  // momentul sosirii, notat aici
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(button_queue, &press, &woken);  // coada plină: apăsarea se pierde
    portYIELD_FROM_ISR(woken);  // dacă a fost trezit un task mai prioritar, rulează imediat
}

// Apelată de clientul MQTT pentru fiecare comandă primită (vezi connection.h).
void command_received(const char* text) {
    Serial.printf("[mqtt] comandă primită: %s\n", text);
    // TODO 3: "BUTTON" merge în coada butonului (o apăsare, la fel ca cea din
    //   ISR), fără să vă blocați (timeout 0). Celelalte comenzi se ignoră:
    //   în această temă placa nu are alte comenzi.
    if (strcmp(text, "BUTTON") == 0) {
        ButtonPress press;
        press.tick = xTaskGetTickCount();  // momentul sosirii, notat aici
        if (xQueueSend(button_queue, &press, 0) != pdTRUE) {
            Serial.println("[mqtt] coada butonului este plină, apăsarea s-a pierdut");
        }
    } else {
        Serial.printf("[mqtt] comandă necunoscută, ignorată: %s\n", text);
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
    Serial.printf("=== tema 03: STUDENT %s, DEVICE_ID %s ===\n", STUDENT, DEVICE_ID);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // LED stins
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    wifi_connect();
    time_sync();

    // TODO 0 (a): creați mutexul stării (xSemaphoreCreateMutex) și coada
    //   butonului (xQueueCreate). Înainte de mqtt_start(): de acolo încolo pot
    //   sosi comenzi, iar command_received() pune "BUTTON" în coadă.
    //   Notați și momentul intrării în faza inițială (CAR_GREEN).
    state_mutex = xSemaphoreCreateMutex();
    button_queue = xQueueCreate(BUTTON_QUEUE_LEN, sizeof(ButtonPress));
    if (state_mutex == NULL || button_queue == NULL) {
        Serial.println("[setup] EROARE: nu s-au putut crea mutexul sau coada");
        for (;;) delay(1000);
    }
    // Taskurile nu rulează încă, deci starea se poate inițializa fără mutex.
    state.phase = CAR_GREEN;
    state.phase_start_ms = millis();
    state.ped_request = false;

    mqtt_start();

    // TODO 0 (b): porniți cele trei taskuri (xTaskCreate), apoi înregistrați
    //   întreruperea butonului (attachInterrupt). Alegeți prioritățile între
    //   2 și 19 (peste loopTask, sub taskurile sistemului); pentru cele două
    //   taskuri periodice, după regula Rate Monotonic. Stiva, în octeți: cel puțin 4096.
    bool ok = xTaskCreate(button_task, "button", TASK_STACK_SIZE, NULL, PRIO_BUTTON, NULL) == pdPASS
           && xTaskCreate(control_task, "control", TASK_STACK_SIZE, NULL, PRIO_CONTROL, NULL) == pdPASS
           && xTaskCreate(output_task, "output", TASK_STACK_SIZE, NULL, PRIO_OUTPUT, NULL) == pdPASS;
    if (!ok) {
        Serial.println("[setup] EROARE: nu s-au putut porni taskurile");
    }

    // După taskuri și coadă: de acum, fiecare apăsare pe BOOT ajunge în coadă.
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), button_isr, FALLING);
}

void loop() {
    // Totul se face în taskuri: loop() nu are nimic de făcut.
    delay(1000);
}
