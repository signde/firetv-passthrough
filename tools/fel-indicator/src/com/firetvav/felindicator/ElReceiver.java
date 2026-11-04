package com.firetvav.felindicator;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.widget.Toast;

/** Shows the EL state the dvfel module's watcher reports: extra "off" = 1 for EL off, 0 for EL on. */
public final class ElReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        boolean off = intent.getIntExtra("off", 0) != 0;
        Toast.makeText(context, off ? "FEL: EL off" : "FEL: EL on", Toast.LENGTH_SHORT).show();
    }
}
