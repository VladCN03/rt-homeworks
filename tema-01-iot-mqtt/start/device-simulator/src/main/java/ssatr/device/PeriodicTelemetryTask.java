package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * TASK PERIODIC, cu perioada T = periodMs: fiecare job citește senzorul și
 * publică valoarea pe topic-ul de telemetrie.
 *
 * Dacă temperatura depășește pragul, taskul NU tratează el alarma, ci doar
 * semnalează un eveniment în coada alarmEvents. Alarma este tratată de
 * taskul sporadic.
 */
public class PeriodicTelemetryTask implements Runnable {

    private final String deviceId;
    private final TemperatureSensor sensor;
    private final MqttConnection mqtt;
    private final BlockingQueue<AlarmEvent> alarmEvents;

    // volatile: sunt citite aici, dar modificate de taskul aperiodic (alt thread)
    private volatile long periodMs = 2000;
    private volatile double threshold = 28.0;

    public PeriodicTelemetryTask(String deviceId, TemperatureSensor sensor, MqttConnection mqtt,
                                 BlockingQueue<AlarmEvent> alarmEvents) {
        this.deviceId = deviceId;
        this.sensor = sensor;
        this.mqtt = mqtt;
        this.alarmEvents = alarmEvents;
    }

    @Override
    public void run() {
        // TODO 1: implementați bucla taskului periodic. Fiecare job:
        //   - citiți temperatura: sensor.read()
        //   - publicați pe Config.telemetryTopic(deviceId) mesajul "deviceId;timestamp;temperatura"
        //     (timestamp = System.currentTimeMillis(); temperatura cu punct zecimal,
        //      de ex. String.format(Locale.US, "%.2f", t), ca să poată fi citită
        //      cu Double.parseDouble în business-service)
        //   - dacă temperatura > threshold, puneți valoarea în coada alarmEvents
        //
        // Folosiți așteptare absolută, nu relativă: Thread.sleep(periodMs) după
        // fiecare job duce la derivă (perioada reală devine T + C + întârzieri).
        // Calculați eliberarea următorului job pe grilă (next = next + periodMs)
        // și așteptați doar cât a mai rămas până la ea. Pentru grilă folosiți
        // un timp monoton: System.nanoTime().
        //
        // periodMs se poate schimba în timpul rulării (comanda SET_INTERVAL),
        // deci citiți-l la fiecare job.

        // r_0: prima eliberare este chiar acum (în nanosecunde, timp monoton)
        long nextRelease = System.nanoTime();

        while (!Thread.currentThread().isInterrupted()) {
            // ---- jobul k ----
            double t = sensor.read();
            long timestampMs = System.currentTimeMillis();
            String payload = deviceId + ";" + timestampMs + ";"
                    + String.format(Locale.US, "%.2f", t);
            mqtt.publish(Config.telemetryTopic(deviceId), payload);
            System.out.println("[periodic] " + payload);

            // alarma nu se tratează aici, doar se semnalează taskului sporadic;
            // pragul se citește o singură dată, ca să compar și să raportez aceeași valoare
            double currentThreshold = threshold;
            if (t > currentThreshold) {
                alarmEvents.offer(new AlarmEvent(t, currentThreshold, timestampMs, System.nanoTime()));
            }

            // ---- așteptare absolută până la eliberarea jobului k+1 ----
            // r_{k+1} = r_k + T; periodMs se citește din nou, poate s-a schimbat
            nextRelease += periodMs * 1_000_000L;
            long remainingNs = nextRelease - System.nanoTime();
            if (remainingNs > 0) {
                try {
                    TimeUnit.NANOSECONDS.sleep(remainingNs);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();   // oprire cerută: ieșim din buclă
                }
            }
            // dacă remainingNs <= 0, jobul a depășit perioada: următorul pornește imediat
        }
    }

    public void setPeriodMs(long periodMs) {
        this.periodMs = periodMs;
    }

    public void setThreshold(double threshold) {
        this.threshold = threshold;
    }

    public long getPeriodMs() {
        return periodMs;
    }

    public double getThreshold() {
        return threshold;
    }
}
