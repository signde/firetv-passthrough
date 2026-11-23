import android.content.Context;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.os.Looper;
import java.lang.reflect.Method;

/** Ask Fire OS to query the connected HDMI device again after the HAL starts. */
public final class GazelleHdmiProfiles {
    public static void main(String[] args) {
        try {
            refresh();
            System.exit(0);
        } catch (Exception error) {
            System.err.println("HDMI profile refresh failed: " + error);
            System.exit(1);
        }
    }

    private static void refresh() throws Exception {
        Looper.prepareMainLooper();
        Class<?> threadClass = Class.forName("android.app.ActivityThread");
        Object thread = threadClass.getMethod("systemMain").invoke(null);
        Context context = (Context) threadClass.getMethod("getSystemContext").invoke(thread);
        AudioManager audio = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
        if (audio.isMusicActive()) {
            System.out.println("Keeping active playback; rates will refresh on HDMI reconnect.");
            return;
        }
        for (AudioDeviceInfo device : audio.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
            if (device.getType() != AudioDeviceInfo.TYPE_HDMI) continue;
            Class<?> system = Class.forName("android.media.AudioSystem");
            int output = system.getField("DEVICE_OUT_HDMI").getInt(null);
            Method connect = AudioManager.class.getMethod("setWiredDeviceConnectionState",
                    int.class, int.class, String.class, String.class);
            String address = device.getAddress();
            String name = device.getProductName().toString();
            try {
                connect.invoke(audio, output, 0, address, name);
            } finally {
                connect.invoke(audio, output, 1, address, name);
            }
            System.out.println("Requested HDMI audio profile refresh.");
            return;
        }
    }
}
