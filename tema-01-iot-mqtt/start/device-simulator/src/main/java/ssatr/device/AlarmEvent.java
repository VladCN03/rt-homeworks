package ssatr.device;

/**
 * Evenimentul pus de taskul periodic în coada de alarme.
 *
 * Conține tot ce trebuie pentru mesajul de alarmă, capturat în momentul în
 * care a fost detectată depășirea:
 *  - temperature, threshold: valorile de atunci (pragul se poate schimba între
 *    timp prin SET_THRESHOLD, dar alarma trebuie să raporteze pragul depășit);
 *  - timestampMs: ora reală, pentru mesaj (System.currentTimeMillis());
 *  - arrivalNs: momentul sosirii pe ceasul monoton (System.nanoTime()), folosit
 *    pentru verificarea intervalului minim între sosiri.
 */
public record AlarmEvent(double temperature, double threshold, long timestampMs, long arrivalNs) {
}
