// The complete setter is identical in all three reviewed Gazelle HAL binaries.
// Preserve every parameter except Fire OS restoring Best Available (5).
function bypassParameters(parameters) {
  return parameters.replace(/(^|;)hdmi_format=5(?=;|$)/g, '$1hdmi_format=6');
}

function retainGazelleBypass(hal, hooks, enabled, event) {
  const code = hal.base.add(0x1eb10);
  if (Checksum.compute('sha256', code.readByteArray(4776)) !==
      '5aede2474a2289a3a02067ca6a6c34147a389710129f14a2e871c0ee6bb354f0')
    throw new Error('Unsupported audio mode setter');
  hooks.push(Interceptor.attach(code.add(1), {
    onEnter(args) {
      if (!enabled()) return;
      const original = args[1].readUtf8String();
      const applied = bypassParameters(original);
      if (applied === original) return;
      // The HAL copies the parameter string before returning from this call.
      this.parameters = Memory.allocUtf8String(applied);
      args[1] = this.parameters;
      event('retain-bypass', {original, applied});
    }
  }));
}
