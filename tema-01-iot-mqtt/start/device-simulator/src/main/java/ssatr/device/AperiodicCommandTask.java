package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;

/**
 * TASK APERIODIC: execută comenzile primite de la business service.
 *
 * Comenzile sosesc oricând, pe MQTT, fără un interval minim între ele, și sunt
 * puse în coada commands. Fiecare comandă eliberează un job al taskului.
 * Comenzi care trebuie suportate:
 *
 *   SET_INTERVAL <ms>       schimbă perioada T a taskului periodic
 *   SET_THRESHOLD <valoare> schimbă pragul de alarmă
 *   RESET                   readuce senzorul la valoarea inițială
 *   STATUS                  raportează perioada și pragul curente
 *
 * După executarea fiecărei comenzi, device-ul publică pe Config.statusTopic(deviceId)
 * o confirmare: "deviceId;timestamp;text", de exemplu
 * "device-1;1727600000000;OK SET_INTERVAL 1000".
 */
public class AperiodicCommandTask implements Runnable {

    private final String deviceId;
    private final TemperatureSensor sensor;
    private final MqttConnection mqtt;
    private final BlockingQueue<String> commands;
    private final PeriodicTelemetryTask periodic;

    public AperiodicCommandTask(String deviceId, TemperatureSensor sensor, MqttConnection mqtt,
                                BlockingQueue<String> commands, PeriodicTelemetryTask periodic) {
        this.deviceId = deviceId;
        this.sensor = sensor;
        this.mqtt = mqtt;
        this.commands = commands;
        this.periodic = periodic;
    }

    @Override
    public void run() {
        // TODO 3: implementați taskul aperiodic:
        //   - preluați comenzile din coadă: commands.take()
        //   - separați numele comenzii de parametru: command.split(" ")
        //   - executați comanda (periodic.setPeriodMs(...), sensor.reset() etc.)
        //   - publicați confirmarea pe Config.statusTopic(deviceId)
        //   - o comandă necunoscută sau un parametru invalid nu trebuie să
        //     oprească taskul: publicați "ERROR <motiv>"
        //
        // Vezi exemplul exemple/01-taskuri/.../AperiodicTaskExample.java

        try {
            while (true) {
                // un job pentru fiecare comandă, în ordinea sosirii (FIFO)
                String command = commands.take();
                System.out.println("[aperiodic] comanda primita: " + command);

                String result;
                try {
                    result = "OK " + execute(command);
                } catch (IllegalArgumentException e) {
                    // comandă greșită: raportăm eroarea și trecem la următoarea comandă
                    // (NumberFormatException, aruncată de parseLong/parseDouble, este tot o
                    //  IllegalArgumentException)
                    result = "ERROR " + e.getMessage();
                }

                String payload = deviceId + ";" + System.currentTimeMillis() + ";" + result;
                mqtt.publish(Config.statusTopic(deviceId), payload);
                System.out.println("[aperiodic] " + payload);
            }
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();   // oprire cerută: thread-ul se termină
        }
    }

    /**
     * Execută o comandă și întoarce textul confirmării (fără "OK ").
     * Pentru o comandă invalidă aruncă IllegalArgumentException cu motivul.
     */
    private String execute(String command) {
        // "  set_interval   1000 " -> ["set_interval", "1000"], name = "SET_INTERVAL"
        // (doar numele comenzii e convertit la majuscule, parametrii rămân cum au venit)
        String[] parts = command.trim().split("\\s+");
        String name = parts[0].toUpperCase(Locale.ROOT);
        if (name.isEmpty()) {
            throw new IllegalArgumentException("comanda goala");
        }

        switch (name) {
            case "SET_INTERVAL": {
                requireArgs(parts, 1);
                long ms = parseLong(parts[1]);
                if (ms <= 0) {
                    throw new IllegalArgumentException("intervalul trebuie sa fie pozitiv: " + parts[1]);
                }
                periodic.setPeriodMs(ms);
                return "SET_INTERVAL " + ms;
            }
            case "SET_THRESHOLD": {
                requireArgs(parts, 1);
                double value = parseDouble(parts[1]);
                periodic.setThreshold(value);
                return "SET_THRESHOLD " + String.format(Locale.US, "%.2f", value);
            }
            case "RESET":
                requireArgs(parts, 0);
                sensor.reset();
                return "RESET";
            case "STATUS":
                requireArgs(parts, 0);
                return String.format(Locale.US, "STATUS interval=%d threshold=%.2f",
                        periodic.getPeriodMs(), periodic.getThreshold());
            default:
                throw new IllegalArgumentException("comanda necunoscuta: " + parts[0]);
        }
    }

    /** Verifică numărul de parametri de după numele comenzii. */
    private static void requireArgs(String[] parts, int expected) {
        int actual = parts.length - 1;
        if (actual != expected) {
            throw new IllegalArgumentException(parts[0] + " asteapta " + expected
                    + " parametri, a primit " + actual);
        }
    }

    private static long parseLong(String text) {
        try {
            return Long.parseLong(text);
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException("numar intreg invalid: " + text);
        }
    }

    private static double parseDouble(String text) {
        try {
            double value = Double.parseDouble(text);
            if (Double.isNaN(value) || Double.isInfinite(value)) {
                throw new NumberFormatException();
            }
            return value;
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException("numar invalid: " + text);
        }
    }
}
