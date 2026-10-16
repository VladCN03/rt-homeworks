package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;

/**
 * TASK SPORADIC: tratează alarmele de temperatură.
 *
 * Evenimentele sosesc în coada alarmEvents (puse de taskul periodic).
 * Intervalul minim între sosiri este MIN_INTERARRIVAL_MS: între două eliberări
 * ale taskului (două alarme publicate) trebuie să treacă cel puțin atât.
 * Intervalul este impus de cod: evenimentele care sosesc mai des sunt
 * respinse, ca să nu fie inundat brokerul.
 */
public class SporadicAlarmTask implements Runnable {

    static final long MIN_INTERARRIVAL_MS = 5000;

    private final String deviceId;
    private final MqttConnection mqtt;
    private final BlockingQueue<AlarmEvent> alarmEvents;

    public SporadicAlarmTask(String deviceId, MqttConnection mqtt, BlockingQueue<AlarmEvent> alarmEvents) {
        this.deviceId = deviceId;
        this.mqtt = mqtt;
        this.alarmEvents = alarmEvents;
    }

    @Override
    public void run() {
        // TODO 2: implementați taskul sporadic:
        //   - așteptați un eveniment: alarmEvents.take()
        //   - dacă de la ultima eliberare au trecut mai puțin de
        //     MIN_INTERARRIVAL_MS, respingeți evenimentul (afișați un mesaj)
        //   - altfel publicați pe Config.alarmTopic(deviceId) mesajul
        //     "deviceId;timestamp;temperatura;prag"
        //     (pragul curent poate fi transmis împreună cu temperatura sau
        //      citit din PeriodicTelemetryTask; alegeți și justificați în raport)
        //
        // Vezi exemplul exemple/01-taskuri/.../SporadicTaskExample.java
        //
        // Alegere: pragul vine împreună cu temperatura, în AlarmEvent. Dacă l-am
        // citi aici din PeriodicTelemetryTask, un SET_THRESHOLD primit
        // între detectare și publicare ar raporta alt prag decât cel depășit.

        boolean released = false;   // a existat deja o eliberare (o alarmă publicată)?
        long lastReleaseNs = 0;     // momentul sosirii ultimului eveniment acceptat

        try {
            while (true) {
                AlarmEvent e = alarmEvents.take();   // blocat până sosește un eveniment

                // intervalul se măsoară între SOSIRI (momentul detectării), nu între
                // momentele în care thread-ul a apucat să scoată evenimentul din coadă
                long sinceLastMs = (e.arrivalNs() - lastReleaseNs) / 1_000_000;
                if (released && sinceLastMs < MIN_INTERARRIVAL_MS) {
                    System.out.printf(Locale.US, "[sporadic] RESPINS %.2f: au trecut doar %d ms (minim %d ms)%n",
                            e.temperature(), sinceLastMs, MIN_INTERARRIVAL_MS);
                    continue;
                }

                // eveniment acceptat: o nouă eliberare a taskului sporadic
                released = true;
                lastReleaseNs = e.arrivalNs();

                String payload = deviceId + ";" + e.timestampMs() + ";"
                        + String.format(Locale.US, "%.2f;%.2f", e.temperature(), e.threshold());
                mqtt.publish(Config.alarmTopic(deviceId), payload);
                System.out.println("[sporadic] ALARMA " + payload);
            }
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();   // oprire cerută: thread-ul se termină
        }
    }
}
