package ssatr.service;

import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;

/**
 * Ultima stare cunoscută a fiecărui device: ultima temperatură, momentul
 * ultimului mesaj, numărul de alarme etc.
 *
 * Este actualizată de thread-urile MessageProcessor și citită de CommandConsole,
 * deci accesul trebuie sincronizat.
 */
public class Dashboard {

    // TODO 5: alegeți structura de date pentru starea device-urilor
    //   (de exemplu un Map<String, ...> cu cheia deviceId)
    //   și adăugați metodele de actualizare apelate din MessageProcessor.
    //
    // Alegere: un TreeMap (sortat după deviceId, ca tabelul să aibă mereu aceeași
    // ordine), protejat de monitorul obiectului Dashboard: toate metodele publice
    // sunt synchronized. Un ConcurrentHashMap n-ar ajunge singur, pentru că
    // DeviceState are mai multe câmpuri care trebuie modificate și citite împreună.

    /** Peste atâtea secunde fără niciun mesaj, device-ul e marcat ca inactiv. */
    private static final long INACTIVE_AFTER_S = 10;

    /** Starea unui device. Accesată doar din metodele synchronized de mai jos. */
    private static class DeviceState {
        double lastTemperature = Double.NaN;   // NaN = încă nu a venit telemetrie
        long lastMessageNs;                    // System.nanoTime() la ultimul mesaj primit
        int alarms;
        String lastStatus = "-";
    }

    private final Map<String, DeviceState> devices = new TreeMap<>();

    /** Întoarce starea device-ului, creând-o la primul mesaj, și notează momentul primirii. */
    private DeviceState touch(String deviceId) {
        DeviceState state = devices.computeIfAbsent(deviceId, id -> new DeviceState());
        // vechimea se măsoară cu ceasul service-ului, nu cu timestamp-ul din mesaj:
        // ceasurile device-urilor pot fi diferite de al nostru
        state.lastMessageNs = System.nanoTime();
        return state;
    }

    /** Apelată pentru fiecare mesaj de telemetrie valid. */
    public synchronized void onTelemetry(String deviceId, double temperature) {
        touch(deviceId).lastTemperature = temperature;
    }

    /** Apelată pentru fiecare alarmă validă. */
    public synchronized void onAlarm(String deviceId, double temperature, double threshold) {
        DeviceState state = touch(deviceId);
        state.alarms++;
        state.lastTemperature = temperature;
    }

    /** Apelată pentru fiecare confirmare validă. */
    public synchronized void onStatus(String deviceId, String text) {
        touch(deviceId).lastStatus = text;
    }

    /** A trimis device-ul vreun mesaj de când rulează service-ul? */
    public synchronized boolean hasDevice(String deviceId) {
        return devices.containsKey(deviceId);
    }

    /** Afișează un tabel cu starea tuturor device-urilor. */
    public void print() {
        // TODO 5: afișați câte un rând pentru fiecare device, de exemplu:
        //   DEVICE     TEMP    ULTIMUL MESAJ   ALARME
        //   device-1   23.41   acum 1 s        2
        System.out.print(render());
    }

    /**
     * Construiește tabelul ca text. Ține monitorul doar cât copiază datele în
     * șir, iar afișarea (lentă) se face în afara lui, ca thread-urile
     * MessageProcessor să nu aștepte după consolă.
     */
    private synchronized String render() {
        if (devices.isEmpty()) {
            return "(niciun device nu a trimis mesaje)\n";
        }

        long now = System.nanoTime();
        StringBuilder sb = new StringBuilder();
        String format = "%-12s %8s   %-22s %6s   %s%n";
        sb.append(String.format(format, "DEVICE", "TEMP", "ULTIMUL MESAJ", "ALARME", "ULTIMUL STATUS"));

        for (Map.Entry<String, DeviceState> e : devices.entrySet()) {
            DeviceState s = e.getValue();
            long ageS = (now - s.lastMessageNs) / 1_000_000_000L;
            String age = "acum " + ageS + " s" + (ageS >= INACTIVE_AFTER_S ? " (inactiv)" : "");
            String temp = Double.isNaN(s.lastTemperature) ? "-"
                    : String.format(Locale.US, "%.2f", s.lastTemperature);
            sb.append(String.format(format, e.getKey(), temp, age, s.alarms, s.lastStatus));
        }
        return sb.toString();
    }
}
