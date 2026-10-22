#!/usr/bin/env python3
"""Local-only regression with user-supplied resources and reference hashes.

No firmware, reference PCM, or resource download functionality is included.
"""
import argparse
import hashlib
import json
import math
import pathlib
import struct
import subprocess
import tempfile
import wave

p=argparse.ArgumentParser()
p.add_argument('--exe',required=True)
p.add_argument('--assets',required=True)
p.add_argument('--reference-results',required=True,help='External research validation.json')
p.add_argument('--report',required=True)
a=p.parse_args()
exe=str(pathlib.Path(a.exe).resolve())
cases=json.loads(pathlib.Path(a.reference_results).read_text())['cases']
results=[]
postprocess_results=[]
with tempfile.TemporaryDirectory(prefix='dsee-integration-') as tmp:
    root=pathlib.Path(tmp)
    for kind in ['silence','impulse','tone','noise']:
        rng=12345;data=bytearray()
        for i in range(49152):
            if kind=='tone':x=0.2*math.sin(2*math.pi*1000*i/48000.0)
            elif kind=='impulse':x=0.5 if i==0 else 0
            elif kind=='noise':rng=(rng*1664525+1013904223)&0xffffffff;x=((rng>>8)/16777216.0-0.5)*0.3
            else:x=0
            data+=struct.pack('<hh',int(x*32768),int(x*0.7*32768))
        with wave.open(str(root/(kind+'.wav')),'wb') as w:
            w.setparams((2,2,48000,0,'NONE','not compressed'));w.writeframes(data)
    for c in cases:
        args=[exe,'process','--assets',a.assets,'--input',str(root/(c['stimulus']+'.wav')),
              '--output',str(root/'out.wav'),'--output-rate',str(c['output_rate']),
              '--source-profile',c['codec_metadata']]
        r=subprocess.run(args,capture_output=True,text=True,timeout=30)
        assert r.returncode==0,(args,r.stdout,r.stderr)
        stats=json.loads(r.stdout)
        with wave.open(str(root/'out.wav'),'rb') as w:
            assert (w.getnchannels(),w.getsampwidth(),w.getframerate())==(2,4,c['output_rate'])
            data=w.readframes(w.getnframes())
        expected_bytes=c['output_bytes']
        digest=hashlib.sha256(data[:expected_bytes]).hexdigest()
        assert digest==c['sha256'],(c['stimulus'],c['codec_metadata'],c['output_rate'],digest,c['sha256'])
        assert stats['input_frames']==49152 and stats['output_frames']*8==len(data)
        results.append({'stimulus':c['stimulus'],'profile':c['codec_metadata'],'rate':c['output_rate'],
                        'reference_prefix_sha256':digest,**stats})
        # Existing output must survive a repeated command unchanged.
        before=hashlib.sha256((root/'out.wav').read_bytes()).hexdigest()
        denied=subprocess.run(args,capture_output=True,text=True)
        assert denied.returncode!=0 and before==hashlib.sha256((root/'out.wav').read_bytes()).hexdigest()
        (root/'out.wav').unlink()
    # The integrated and standalone commands must produce identical PCM when
    # given the same native output/reference, including inferred latency.
    for rate in (48000,96000,192000):
        base=[exe,'process','--assets',a.assets,'--input',str(root/'noise.wav'),
              '--output-rate',str(rate)]
        subprocess.run(base+['--output',str(root/'native.wav')],check=True,capture_output=True)
        for mode in ('none','mix','adaptive','combined'):
            integrated=subprocess.run(base+['--output',str(root/'integrated.wav'),
                                            '--postprocess',mode],check=True,capture_output=True,text=True)
            standalone=subprocess.run([exe,'postprocess','--input',str(root/'native.wav'),
                                       '--reference',str(root/'noise.wav'),'--output',str(root/'standalone.wav'),
                                       '--mode',mode],check=True,capture_output=True,text=True)
            assert (root/'integrated.wav').read_bytes()==(root/'standalone.wav').read_bytes(),(rate,mode)
            stats=json.loads(standalone.stdout)
            postprocess_results.append({'rate':rate,'mode':mode,'integrated_matches':True,
                                        'output_frames':stats['output_frames'],
                                        'postprocess':stats['postprocess']})
            (root/'integrated.wav').unlink();(root/'standalone.wav').unlink()
        (root/'native.wav').unlink()
    # Verify resources are checked before the tool creates an output file.
    bad=root/'bad-assets/vendor/lib64';bad.mkdir(parents=True)
    lib=bytearray((pathlib.Path(a.assets)/'vendor/lib64/libsonydseehxwrapper.so').read_bytes());lib[-1]^=1
    (bad/'libsonydseehxwrapper.so').write_bytes(lib)
    failed=subprocess.run([exe,'process','--assets',str(root/'bad-assets'),'--input',str(root/'tone.wav'),'--output',str(root/'bad-out.wav')],capture_output=True,text=True)
    assert failed.returncode!=0 and 'SHA-256' in failed.stderr and not (root/'bad-out.wav').exists()
    assert not list(root.glob('*.tmp.*'))
pathlib.Path(a.report).write_text(json.dumps({'passed':len(results),'reference':'external research fixture hashes',
                                            'cases':results,'postprocess_cases':postprocess_results},indent=2)+'\n')
print(f'{len(results)} original-wrapper references and {len(postprocess_results)} integrated/standalone comparisons passed; output refusal and corrupt-resource checks passed')
