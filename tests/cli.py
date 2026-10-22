#!/usr/bin/env python3
"""No proprietary assets required."""
import pathlib
import json
import os
import struct
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
    # The installed program's audio path uses only native code. These generated
    # WAV checks use Python's standard library solely as a test driver.
    run('postprocess','--help',success=True)
    def wav(name, rate=96000, width=4, pcm=None):
        if pcm is None:
            pcm=struct.pack('<iiiiiiii',-2147483648,2147483647,-1,1,0,0,123456,-234567)
        with wave.open(str(root/name),'wb') as w:
            w.setparams((2,width,rate,0,'NONE','not compressed'));w.writeframes(pcm)
        return pcm
    original=wav('pcm32.wav')
    result=run('postprocess','--input','pcm32.wav','--output','copy.wav','--mode','none',success=True)
    assert json.loads(result.stdout)['output_frames']==4
    with wave.open(str(root/'copy.wav'),'rb') as w:
        assert (w.getnchannels(),w.getsampwidth(),w.getframerate())==(2,4,96000)
        assert w.readframes(w.getnframes())==original
    # PATH deliberately has no Python, FFmpeg or other executables.
    p=subprocess.run([exe,'postprocess','--input','pcm32.wav','--reference','pcm32.wav',
                      '--output','native.wav','--reference-delay','0'],cwd=tmp,
                     env={**os.environ,'PATH':str(root/'no-programs')},capture_output=True,text=True)
    assert p.returncode==0,(p.stdout,p.stderr)
    with wave.open(str(root/'native.wav'),'rb') as w:
        assert w.readframes(w.getnframes())==original
    wav('empty.wav',pcm=b'')
    run('postprocess','--input','empty.wav','--output','empty-out.wav','--mode','none',success=True)
    # Non-mixing modes use the processed branch even when the requested mix is zero.
    report_pcm=wav('report.wav',pcm=struct.pack('<ii',268435456,-268435456)*4096)
    wav('silent-reference.wav',pcm=b'\0'*len(report_pcm))
    for mode,strength in [('none','1'),('adaptive','1'),('adaptive','0')]:
        for source,expected in [('report.wav',report_pcm),('empty.wav',b'')]:
            destination=f'report-{mode}-{strength}-{source}'
            result=run('postprocess','--input',source,'--reference','silent-reference.wav',
                       '--output',destination,'--mode',mode,'--mix','0',
                       '--envelope-strength',strength,'--reference-delay','0',success=True)
            analysis=json.loads(result.stdout)['postprocess']
            assert analysis['effective_mix']==1 and analysis['auto_mix'] is None
            assert analysis['parameters']['mix']==0
            with wave.open(str(root/destination),'rb') as w:
                assert w.readframes(w.getnframes())==expected
    wav('pcm16.wav',rate=44100,width=2,pcm=struct.pack('<hhhh',123,-456,789,-1000))
    run('postprocess','--input','pcm32.wav','--reference','pcm16.wav','--output','resampled.wav',
        '--mode','mix','--mix','0','--reference-delay','0',success=True)
    run('postprocess','--input','pcm16.wav','--output','bad-rate.wav','--mode','none')
    run('postprocess','--input','pcm32.wav','--output','no-reference.wav')
    run('postprocess','--input','pcm32.wav','--output','wrong.m4a','--mode','none')
    run('postprocess','--input','pcm32.wav','--output','pcm32.wav','--mode','none')
    before=(root/'copy.wav').read_bytes()
    run('postprocess','--input','pcm32.wav','--output','copy.wav','--mode','none')
    assert (root/'copy.wav').read_bytes()==before
    (root/'link.wav').symlink_to(root/'missing-target')
    run('postprocess','--input','pcm32.wav','--output','link.wav','--mode','none')
    assert (root/'link.wav').is_symlink()
    for option,value in [('--mix','nan'),('--mix','1.01'),('--mix','0.5x'),('--curve','bogus'),
                         ('--mode','bogus'),('--reference-delay','1.5'),('--reference-delay',str(2**63)),
                         ('--envelope-strength','-1'),('--smoothing-ms','inf'),
                         ('--fit-high','16000'),('--crossover-high','48000')]:
        run('postprocess','--input','pcm32.wav','--reference','pcm32.wav','--output','bad.wav',
            option,value)
    run('postprocess','--input','pcm32.wav','--reference','pcm32.wav','--output','too-short.wav')
    # A clipping error happens after the writer opens, and must remove its temp file.
    wav('hot.wav',pcm=struct.pack('<ii',int(.9*2**31),int(.9*2**31))*2000)
    run('postprocess','--input','hot.wav','--reference','hot.wav','--output','clipped.wav',
        '--mix','0','--reference-gain-db','12','--reference-delay','0')
    assert not (root/'clipped.wav').exists()
    # A sparse PCM16 input can fit RIFF while its PCM32 output would not. Reject
    # it from the header, without reading/allocating gigabytes of silent audio.
    frames=(2**32-1-36)//8+1
    size=frames*4
    with (root/'large.wav').open('wb') as f:
        f.write(struct.pack('<4sI4s4sIHHIIHH4sI',b'RIFF',36+size,b'WAVE',b'fmt ',16,
                            1,2,96000,96000*4,4,16,b'data',size))
        f.truncate(44+size)
    result=run('postprocess','--input','large.wav','--output','large-out.wav','--mode','none')
    assert 'RIFF' in result.stderr and not (root/'large-out.wav').exists()
    (root/'large.wav').unlink()
    # Native processing retains its strict input contract and opt-in controls.
    run('process','--assets','missing','--input','valid.wav','--output','out.wav','--mix','0.5')
    run('process','--assets','missing','--input','valid.wav','--output','out.wav','--postprocess','mix')
    assert not (root/'out.wav').exists()
    assert not list(root.glob('*.tmp.*'))
    run('postprocess','--input','pcm32.wav','--output','bad-auto.wav','--mode','none','--mix','auto')
    run('postprocess','--input','pcm32.wav','--output','bad-auto.wav','--auto-mix-min','0.2')
    result=run('postprocess','--input','pcm32.wav','--reference','pcm32.wav','--output','auto.wav',
               '--mix','auto','--reference-delay','0','--auto-mix-fallback','0.37',success=True)
    analysis=json.loads(result.stdout)['postprocess']
    assert analysis['effective_mix']==0.37 and analysis['auto_mix']['fallback']
    assert analysis['parameters']['mix']=='auto'
    assert analysis['parameters']['auto-mix-objective']=='seam'
    run('postprocess','--input','pcm32.wav','--reference','pcm32.wav','--output','bad-auto.wav',
        '--mix','auto','--auto-mix-objective','invalid')
print('No-assets CLI validation passed')
