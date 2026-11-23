// MediaTek uses a const String8 reference, unlike Gazelle's C-string setter.
// Keep the caller's object intact and use libutils to own the replacement.
function karatBypassParameters(parameters) {
  return parameters.replace(/(^|;)hdmi_format=5(?=;|$)/g, '$1hdmi_format=6');
}

function retainKaratBypass(hal, hooks, enabled, event) {
  const setter = hal.getExportByName('_ZN7android17AudioALSAHardware13setParametersERKNS_7String8E');
  // Complete setter fingerprints from the already supported RS8145–RS8182 HALs.
  // Full HAL/SPDIF pairs are independently verified before this function runs.
  const fingerprints = [
    [5528, 'b4240d90a5b5eaacf1c7748e3dd2f16ca25049c22ed247392858bba099e3ed01'],
    [5596, 'b563d551d80719c917b01353a01c0a84951b4abed5e78984d6606adb181bac15'],
    [5752, 'c0057b497acc35dbff72793ac208a88e60ea578cd521d02a49e34c606aee6acc'],
    [5752, '4ab93ef33d251d6182bf4fc7997bf9a4566a3916b51050ecdd2a434e9514fbf9'],
    [5752, 'e7a66e8ccdb23219ffb2542d546c498e1e823d9cc8cd516d0e538acadd896606'],
  ];
  if (!fingerprints.some(([size, hash]) =>
    Checksum.compute('sha256', setter.readByteArray(size)) === hash))
    throw new Error('Unsupported Karat audio mode setter');
  const utils = Process.getModuleByName('libutils.so');
  const construct = new NativeFunction(utils.getExportByName('_ZN7android7String8C1EPKc'),
    'void', ['pointer', 'pointer']);
  const destroy = new NativeFunction(utils.getExportByName('_ZN7android7String8D1Ev'),
    'void', ['pointer']);
  hooks.push(Interceptor.attach(setter, {
    onEnter(args) {
      if (!enabled()) return;
      const original = args[1].readPointer().readUtf8String();
      const applied = karatBypassParameters(original);
      if (applied === original) return;
      this.text = Memory.allocUtf8String(applied);
      this.parameters = Memory.alloc(Process.pointerSize);
      construct(this.parameters, this.text);
      args[1] = this.parameters;
      event('retain-bypass', {device: 'karat', original, applied});
    },
    onLeave() {
      if (this.parameters) destroy(this.parameters);
    }
  }));
}
