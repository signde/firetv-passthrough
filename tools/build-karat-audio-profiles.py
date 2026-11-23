#!/usr/bin/env python3
"""Build the one-shot Karat audio-policy helper with a local Android SDK/JDK."""
import argparse, hashlib, pathlib, subprocess, tempfile
root=pathlib.Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--android-jar',required=True);p.add_argument('--d8-jar',required=True);p.add_argument('--javac',default='javac');p.add_argument('--java',default='java');a=p.parse_args()
source=root/'src/KaratAudioProfiles.java';dest=root/'bin/karat-audio-profiles.dex'
with tempfile.TemporaryDirectory() as temp:
 subprocess.run([a.javac,'-source','8','-target','8','-cp',a.android_jar,'-d',temp,str(source)],check=True)
 subprocess.run([a.java,'-cp',a.d8_jar,'com.android.tools.r8.D8','--min-api','30','--lib',a.android_jar,'--output',temp,str(pathlib.Path(temp)/'KaratAudioProfiles.class')],check=True)
 dest.parent.mkdir(exist_ok=True);dest.write_bytes((pathlib.Path(temp)/'classes.dex').read_bytes())
(root/'src/karat-audio-profiles.sha256').write_text(''.join(hashlib.sha256(f.read_bytes()).hexdigest()+'  '+str(f.relative_to(root))+'\n' for f in (source,dest)))
