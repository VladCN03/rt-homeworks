package ssatr.service;

import java.time.Instant;
import java.time.ZoneId;
import java.time.format.DateTimeFormatter;
import java.util.Locale;
import java.util.concurrent.BlockingQueue;

/**
 * Preia mesajele de un anumit tip ("telemetry", "alarm" sau "status") din coada
 * inbox, le salvează și actualizează dashboard-ul.
 *
 * Formatul mesajelor (câmpuri separate prin ";"):
 *   telemetry:  deviceId;timestamp;temperatura
 *   alarm:      deviceId;timestamp;temperatura;prag
 *   status:     deviceId;timestamp;text
 */
public class MessageProcessor implements Runnable {

    private final String kind;
    private final BlockingQueue<String> inbox;
    private final CsvStorage storage;
    private final Dashboard dashboard;

    public MessageProcessor(String kind, BlockingQueue<String> inbox, CsvStorage storage, Dashboard dashboard) {
        this.kind = kind;
        this.inbox = inbox;
        this.storage = storage;
        this.dashboard = dashboard;
    }

    @Override
    public void run() {
        // TODO 4: într-o buclă:
        //   - preluați un mesaj: inbox.take()
        //   - salvați mesajul în fișierul CSV: storage.append(kind, mesaj)
        //     (CsvStorage este gata implementată)
        //   - actualizați dashboard-ul, în funcție de kind
        //   - alarmele și confirmările se afișează imediat în consolă
        //   - un mesaj greșit (format invalid) nu trebuie să oprească thread-ul

        try {
            while (true) {
                String message = inbox.take();   // blocat până sosește un mesaj
                try {
                    process(message);
                } catch (IllegalArgumentException e) {
                    // mesaj invalid: îl raportăm, nu îl salvăm, și trecem la următorul
                    System.out.println("[" + kind + "] mesaj ignorat (" + e.getMessage() + "): " + message);
                }
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();   // oprire cerută: thread-ul se termină
        }
    }

    /**
     * Verifică formatul, salvează mesajul și actualizează dashboard-ul.
     * Pentru un mesaj invalid aruncă IllegalArgumentException, fără să salveze nimic.
     */
    private void process(String message) {
        // status: textul de la final poate conține orice, deci împărțim în cel mult 3 câmpuri
        String[] f = message.split(";", kind.equals("status") ? 3 : -1);
        String deviceId = f[0];
        if (deviceId.isBlank()) {
            throw new IllegalArgumentException("deviceId lipsa");
        }

        switch (kind) {
            case "telemetry": {
                requireFields(f, 3);
                parseTimestamp(f[1]);
                double temperature = parseNumber(f[2]);
                storage.append(kind, message);
                dashboard.onTelemetry(deviceId, temperature);
                break;   // telemetria nu se afișează: ar umple consola operatorului
            }
            case "alarm": {
                requireFields(f, 4);
                long timestamp = parseTimestamp(f[1]);
                double temperature = parseNumber(f[2]);
                double threshold = parseNumber(f[3]);
                storage.append(kind, message);
                dashboard.onAlarm(deviceId, temperature, threshold);
                System.out.printf(Locale.US, "!!! ALARMA %s la %s: %.2f > prag %.2f%n",
                        deviceId, TIME.format(Instant.ofEpochMilli(timestamp)), temperature, threshold);
                break;
            }
            case "status": {
                requireFields(f, 3);
                long timestamp = parseTimestamp(f[1]);
                String text = f[2];
                storage.append(kind, message);
                dashboard.onStatus(deviceId, text);
                System.out.printf("[status] %s la %s: %s%n",
                        deviceId, TIME.format(Instant.ofEpochMilli(timestamp)), text);
                break;
            }
            default:
                throw new IllegalStateException("tip de mesaj necunoscut: " + kind);
        }
    }

    /** Ora locală, de ex. 21:37:05, pentru afișarea în consolă. */
    private static final DateTimeFormatter TIME =
            DateTimeFormatter.ofPattern("HH:mm:ss").withZone(ZoneId.systemDefault());

    private static void requireFields(String[] f, int expected) {
        if (f.length != expected) {
            throw new IllegalArgumentException("asteptam " + expected + " campuri, am primit " + f.length);
        }
    }

    private static long parseTimestamp(String text) {
        try {
            return Long.parseLong(text.trim());
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException("timestamp invalid: " + text);
        }
    }

    private static double parseNumber(String text) {
        try {
            return Double.parseDouble(text.trim());
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException("numar invalid: " + text);
        }
    }
}
