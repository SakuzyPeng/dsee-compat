#!/usr/bin/env python3
"""No proprietary assets required."""
import pathlib
import subprocess
import sys
import tempfile
import wave

exe=str(pathlib.Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='dsee-cli-') as tmp:
    root=pathlib.Path(tmp)
    def run(*args,success=False):
        p=subprocess.run([exe,*args],capture_output=True,text=True,cwd=tmp)
        assert (p.returncode==0)==success,(args,p.stdout,p.stderr)
        return p
    run('--help',success=True)
    run('--version',success=True)
    run('unknown')
    run('inspect')
    run('inspect','--assets',str(root/'missing'))
    run('process','--assets','missing','--input','missing','--output','out.wav','--output-rate','44100')
    run('process','--assets','missing','--assets','missing')
    (root/'bad.wav').write_bytes(b'not a wav')
    run('process','--assets','missing','--input','bad.wav','--output','out.wav')
    with wave.open(str(root/'valid.wav'),'wb') as w:
        w.setparams((2,2,48000,0,'NONE','not compressed'));w.writeframes(b'\0'*4)
    run('process','--assets','missing','--input','valid.wav','--output','out.wav')
    run('process','--assets','missing','--input','valid.wav','--output','valid.wav')
    assert not (root/'out.wav').exists()
    assert not list(root.glob('*.tmp.*'))
print('No-assets CLI validation passed')
