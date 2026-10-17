package ssatr.service;

import java.util.Scanner;

/**
 * Consola operatorului: citește comenzi de la tastatură.
 *
 *   show                               afișează dashboard-ul
 *   <deviceId> <COMANDA> [parametru]   trimite o comandă unui device, de exemplu
 *                                      device-1 SET_INTERVAL 1000
 *   help                               afișează comenzile
 *   exit                               oprește serviciul
 */
public class CommandConsole implements Runnable {

    private final MqttConnection mqtt;
    private final Dashboard dashboard;

    public CommandConsole(MqttConnection mqtt, Dashboard dashboard) {
        this.mqtt = mqtt;
        this.dashboard = dashboard;
    }

    @Override
    public void run() {
        Scanner scanner = new Scanner(System.in);
        printHelp();

        while (scanner.hasNextLine()) {
            String line = scanner.nextLine().trim();

            if (line.isEmpty()) {
                continue;
            } else if (line.equals("exit")) {
                break;
            } else if (line.equals("help")) {
                printHelp();
            } else if (line.equals("show")) {
                dashboard.print();
            } else {
                sendCommand(line);
            }
        }
    }

    private void sendCommand(String line) {
        // TODO 6: separați deviceId de restul liniei
        //   ("device-1 SET_INTERVAL 1000" -> "device-1" și "SET_INTERVAL 1000")
        //   și publicați comanda pe Config.commandTopic(deviceId).
        //   Afișați un mesaj de eroare dacă linia nu are formatul corect.

        // "device-1   SET_INTERVAL 1000" -> ["device-1", "SET_INTERVAL 1000"]
        // limita 2: primul cuvânt e deviceId, tot restul liniei e comanda
        String[] parts = line.split("\\s+", 2);
        if (parts.length < 2) {
            System.out.println("Format: <deviceId> <COMANDA> [parametru], de exemplu: device-1 SET_INTERVAL 1000");
            return;
        }
        String deviceId = parts[0];
        String command = parts[1];

        // deviceId devine un nivel din topic: "+" și "#" sunt wildcard-uri (interzise la
        // publicare, Paho ar arunca o excepție), iar "/" ar adăuga niveluri în plus
        if (deviceId.matches(".*[+#/].*")) {
            System.out.println("deviceId invalid (nu poate contine + # /): " + deviceId);
            return;
        }

        // comanda nu e validată aici: device-ul o verifică și răspunde cu OK sau ERROR
        if (!dashboard.hasDevice(deviceId)) {
            System.out.println("atentie: " + deviceId + " nu a trimis inca mesaje; comanda se trimite oricum");
        }
        mqtt.publish(Config.commandTopic(deviceId), command);
        System.out.println("-> " + deviceId + ": " + command);
    }

    private void printHelp() {
        System.out.println("Comenzi: show | <deviceId> SET_INTERVAL <ms> | <deviceId> SET_THRESHOLD <valoare>");
        System.out.println("         <deviceId> RESET | <deviceId> STATUS | help | exit");
    }
}
