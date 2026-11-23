import android.content.Context;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import java.lang.reflect.Method;
import java.util.HashSet;
import java.util.Set;

/** One-shot refresh after HAL attachment. Does not alter saved surround preferences. */
public final class KaratAudioProfiles {
    private static Set<Integer> hdmiFormats(AudioManager manager) {
        Set<Integer> result = new HashSet<>();
        for (AudioDeviceInfo device : manager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
            if (device.getType() == AudioDeviceInfo.TYPE_HDMI)
                for (int encoding : device.getEncodings()) result.add(encoding);
        }
        return result;
    }
    public static void main(String[] args) {
        int exit = 1;
        try {
            if (android.os.Process.myUid() != 0) throw new SecurityException("Root required");
            Class<?> properties = Class.forName("android.os.SystemProperties");
            if (!"karat".equals(properties.getMethod("get", String.class).invoke(null, "ro.product.device")))
                throw new IllegalStateException("Karat only");
            Set<Integer> wanted = new HashSet<>();
            for (String arg : args) {
                if (!arg.equals("5") && !arg.equals("6")) throw new IllegalArgumentException("Only DD/DD+ expected");
                wanted.add(Integer.parseInt(arg));
            }
            android.os.Looper.prepareMainLooper();
            Class<?> activityThread = Class.forName("android.app.ActivityThread");
            Object thread = activityThread.getMethod("systemMain").invoke(null);
            Context context = (Context)activityThread.getMethod("getSystemContext").invoke(thread);
            AudioManager manager = (AudioManager)context.getSystemService(Context.AUDIO_SERVICE);
            Set<Integer> before = hdmiFormats(manager);
            if (wanted.isEmpty() || before.isEmpty() || before.containsAll(wanted)) {
                System.out.println("Karat HDMI capabilities already current; no refresh.");
                System.exit(0);
            }
            Class<?> audio = Class.forName("android.media.AudioSystem");
            int use = audio.getField("FOR_ENCODED_SURROUND").getInt(null);
            Method get = audio.getMethod("getForceUse", int.class);
            int original = (Integer)get.invoke(null, use);
            // Auto only. Respect deliberate PCM/manual/always-surround choices.
            if (original != 0) {
                System.out.println("Karat capability refresh skipped: surround policy=" + original);
                System.exit(0);
            }
            System.out.println("Refreshing stale Karat HDMI capabilities; expected=" + wanted + " before=" + before);
            AudioDeviceInfo hdmi = null;
            for (AudioDeviceInfo device : manager.getDevices(AudioManager.GET_DEVICES_OUTPUTS))
                if (device.getType() == AudioDeviceInfo.TYPE_HDMI) { hdmi=device; break; }
            if (hdmi == null) throw new IllegalStateException("HDMI disconnected before refresh");
            int deviceType = audio.getField("DEVICE_OUT_HDMI").getInt(null);
            Method connection = AudioManager.class.getMethod("setWiredDeviceConnectionState",
                int.class, int.class, String.class, String.class);
            String address = hdmi.getAddress();
            String name = hdmi.getProductName().toString();
            // Same framework re-enumeration used for surround-preference changes.
            // This toggles the logical audio route, not the physical HDMI/video link.
            try {
                connection.invoke(manager, deviceType, 0, address, name);
            } finally {
                connection.invoke(manager, deviceType, 1, address, name);
            }
            for (int attempt=0; attempt<10; attempt++) {
                Set<Integer> after = hdmiFormats(manager);
                if (after.containsAll(wanted)) {
                    System.out.println("Karat HDMI capabilities restored: " + after);
                    exit=0;
                    break;
                }
                Thread.sleep(200);
            }
            if (exit != 0) System.out.println("Karat HDMI capabilities remain incomplete.");
        } catch (Exception error) {
            System.out.println("Karat capability refresh failed: " + error);
        }
        System.exit(exit);
    }
}
