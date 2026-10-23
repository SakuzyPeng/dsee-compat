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
p.add_argument('--reference-results',required=True,help='External validation JSON or probe JSONL with complete PCM hashes')
p.add_argument('--report',required=True)
p.add_argument('--reference-label',default='original-wrapper',help='Provenance of the supplied hashes')
p.add_argument('--core-only',action='store_true',help='Skip postprocessing comparisons (e.g. CPU emulation)')
a=p.parse_args()
exe=str(pathlib.Path(a.exe).resolve())
reference_text=pathlib.Path(a.reference_results).read_text(encoding='utf-8')
try:
    cases=json.loads(reference_text)['cases']
except json.JSONDecodeError:
    cases=[json.loads(line) for line in reference_text.splitlines() if line.strip()]
    cases=[c for c in cases if c.get('event')=='case']
for c in cases:
    c.setdefault('codec_metadata', c.get('profile'))
    c.setdefault('output_rate', c.get('rate'))
    c.setdefault('output_bytes', 49152*(c['output_rate']//48000)*8)
assert len(cases)==36 and len({(c['stimulus'],c['codec_metadata'],c['output_rate']) for c in cases})==36
input_hashes={}

results=[]
postprocess_results=[]
with tempfile.TemporaryDirectory(prefix='dsee-integration-') as tmp:
    root=pathlib.Path(tmp)/'音频 测试'
    repo=pathlib.Path(__file__).resolve().parents[1]
    assert repo not in root.resolve().parents, 'Resource fixtures must be outside the repository; set TMPDIR/TEMP to an external directory'
    root.mkdir()
    for kind in ['silence','impulse','tone','noise']:
        rng=12345;data=bytearray()
        for i in range(49152):
            if kind=='tone':x=0.2*math.sin(2*math.pi*1000*i/48000.0)
            elif kind=='impulse':x=0.5 if i==0 else 0
            elif kind=='noise':rng=(rng*1664525+1013904223)&0xffffffff;x=((rng>>8)/16777216.0-0.5)*0.3
            else:x=0
            data+=struct.pack('<hh',int(x*32768),int(x*0.7*32768))
        input_hashes[kind]=hashlib.sha256(data).hexdigest()
        with wave.open(str(root/(kind+'.wav')),'wb') as w:
            w.setparams((2,2,48000,0,'NONE','not compressed'));w.writeframes(data)
    for c in cases:
        if 'input_sha256' in c:
            assert c['input_sha256']==input_hashes[c['stimulus']], 'Input fixture differs from baseline'
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
        full_digest=hashlib.sha256(data).hexdigest()
        tail_blocks=(stats['output_frames']-49152*(c['output_rate']//48000))//(1024*(c['output_rate']//48000))
        if 'full_sha256' in c:
            assert full_digest==c['full_sha256'], (c['stimulus'],c['codec_metadata'],c['output_rate'],'full output differs')
            assert stats['output_frames']==c['output_frames'] and tail_blocks==c['tail_blocks']

        results.append({'stimulus':c['stimulus'],'profile':c['codec_metadata'],'rate':c['output_rate'],
                        'reference_prefix_sha256':digest,'input_sha256':input_hashes[c['stimulus']],
                        'full_sha256':full_digest,'tail_blocks':tail_blocks,**stats})
        # Existing output must survive a repeated command unchanged.
        before=hashlib.sha256((root/'out.wav').read_bytes()).hexdigest()
        denied=subprocess.run(args,capture_output=True,text=True)
        assert denied.returncode!=0 and before==hashlib.sha256((root/'out.wav').read_bytes()).hexdigest()
        (root/'out.wav').unlink()
    # The integrated and standalone commands must produce identical PCM when
    # given the same native output/reference, including inferred latency.
    for rate in (() if a.core_only else (48000,96000,192000)):
        base=[exe,'process','--assets',a.assets,'--input',str(root/'noise.wav'),
              '--output-rate',str(rate)]
        subprocess.run(base+['--output',str(root/'native.wav')],check=True,capture_output=True)
        for mode,automatic,objective in [('none',False,'seam'),('mix',False,'seam'),
                                         ('adaptive',False,'seam'),('combined',False,'seam'),
                                         ('mix',True,'seam'),('combined',True,'seam'),
                                         ('mix',True,'envelope'),('combined',True,'envelope')]:
            controls=['--mix','auto','--auto-mix-objective',objective] if automatic else []
            integrated=subprocess.run(base+['--output',str(root/'integrated.wav'),
                                            '--postprocess',mode]+controls,check=True,capture_output=True,text=True)
            standalone=subprocess.run([exe,'postprocess','--input',str(root/'native.wav'),
                                       '--reference',str(root/'noise.wav'),'--output',str(root/'standalone.wav'),
                                       '--mode',mode]+controls,check=True,capture_output=True,text=True)
            assert (root/'integrated.wav').read_bytes()==(root/'standalone.wav').read_bytes(),(rate,mode)
            stats=json.loads(standalone.stdout)
            postprocess_results.append({'rate':rate,'mode':mode,'automatic_mix':automatic,
                                        'auto_mix_objective':objective,'integrated_matches':True,
                                        'output_frames':stats['output_frames'],
                                        'postprocess':stats['postprocess']})
            (root/'integrated.wav').unlink();(root/'standalone.wav').unlink()
        (root/'native.wav').unlink()
    # Resource fixtures are temporary, outside the repository and any build tree.
    specs=['vendor/lib64/libsonydseehxwrapper.so','vendor/etc/dsx_param_file.bin',
           'vendor/etc/DrangeparaPCM48.bin','vendor/etc/DrangeparaAAC128.bin','vendor/etc/DrangeparaMP3_128.bin']
    assets=pathlib.Path(a.assets)
    bad_root=root/'bad-assets'
    for name in specs:
        target=bad_root/name
        target.parent.mkdir(parents=True,exist_ok=True)
        target.write_bytes((assets/name).read_bytes())
    for index,name in enumerate(specs):
        target=bad_root/name
        original=target.read_bytes()
        corrupt=bytearray(original);corrupt[-1]^=1;target.write_bytes(corrupt)
        inspected=subprocess.run([exe,'inspect','--assets',str(bad_root)],capture_output=True,text=True,encoding='utf-8',errors='replace')
        assert inspected.returncode!=0 and 'SHA-256' in inspected.stderr
        profile='pcm' if index<3 else ('aac128' if index==3 else 'mp3128')
        failed=subprocess.run([exe,'process','--assets',str(bad_root),'--source-profile',profile,
                               '--input',str(root/'tone.wav'),'--output',str(root/'bad-out.wav')],capture_output=True,text=True,encoding='utf-8',errors='replace')
        assert failed.returncode!=0 and 'SHA-256' in failed.stderr and not (root/'bad-out.wav').exists()
        target.write_bytes(original)
    # Creation needs only the selected parameters; inspect requires every profile.
    (bad_root/specs[3]).unlink();(bad_root/specs[4]).unlink()
    with wave.open(str(root/'empty.wav'),'wb') as w:
        w.setparams((2,2,48000,0,'NONE','not compressed'));w.writeframes(b'')
    empty=subprocess.run([exe,'process','--assets',str(bad_root),'--input',str(root/'empty.wav'),
                          '--output',str(root/'empty-output.wav')],capture_output=True,text=True,encoding='utf-8',errors='replace')
    assert empty.returncode==0,(empty.stdout,empty.stderr)
    assert json.loads(empty.stdout)['output_frames']==0 and (root/'empty-output.wav').stat().st_size==44
    assert not list(root.rglob('*.tmp.*'))
pathlib.Path(a.report).write_text(json.dumps({'passed':len(results),'reference':a.reference_label,
                                            'cases':results,'postprocess_cases':postprocess_results,'corrupt_resources_rejected':5,'selected_resources_only':True,'unicode_paths':True},indent=2)+'\n')
print(f'{len(results)} {a.reference_label} references and {len(postprocess_results)} integrated/standalone comparisons passed; output refusal and corrupt-resource checks passed')
