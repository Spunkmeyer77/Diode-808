# builds "DIODE 808.amxd" (unfrozen) from the UNIT 808 patcher JSON
import json, re, copy, struct, sys
SRC = sys.argv[1]; OUT = sys.argv[2]
d = json.load(open(SRC)); P = d['patcher']
boxes = {b['box']['id']: b for b in P['boxes']}
ORDER = ["bd","sd","lt","mt","ht","rs","cp","cb","cy","ch","oh","lc","mc","hc","cl"]
drop = set(i for i in boxes if re.match(r'^(d_|dv_d_|p_d_|v_)', i) or (i.startswith('g_') and i != 'g_seq') or i == 'sqacc')
P['boxes'] = [b for b in P['boxes'] if b['box']['id'] not in drop]
P['lines'] = [l for l in P['lines'] if l['patchline']['source'][0] not in drop and l['patchline']['destination'][0] not in drop
              and not (l['patchline']['destination'][0].startswith('ac_') and l['patchline']['destination'][1] == 1)
              and not (l['patchline']['source'][0].startswith('c_') or (l['patchline']['source'][0] == 'g_seq' and l['patchline']['source'][1] < 15))]
def add(box): P['boxes'].append({'box': box})
def line(s, so, dd, di): P['lines'].append({'patchline': {'source': [s, so], 'destination': [dd, di]}})
# engine
add({'id': 'eng', 'maxclass': 'newobj', 'text': 'diode808~', 'numinlets': 15, 'numoutlets': 2, 'outlettype': ['signal', ''], 'patching_rect': [20.0, 300.0, 1000.0, 22.0]})
line('eng', 0, 'out', 0); line('eng', 0, 'out', 1)
for k, v in enumerate(ORDER):
    line('g_seq', k, 'eng', k)
    line('c_' + v, 0, 'eng', k)
    b = boxes['ac_' + v]['box']; b['text'] = 'expr 0.5+0.5*($f1>=100)'; b['numinlets'] = 1
# sequencer: trigger value 0.5 normal / 1 accent, +4 flam (flam masks at 320 + pattern*16 + voice)
g = boxes['g_seq']['box']['patcher']
cb = [x for x in g['boxes'] if x['box']['id'] == 'cb'][0]['box']
code = cb['code']
code = code.replace('Param accamt(0.6, min=0, max=1);\n', '')
code = code.replace('amp = acb + (1 - acb)*((1 - accamt) + accamt*90/127);', 'amp = 0.5 + 0.5*acb;')
for i in range(1, 16):
    code = code.replace('vb%d = bit(peek(pd, base + %d), pos)*' % (i, i), 'vb%d = bit(peek(pd, base + %d), pos)*' % (i, i))
    code = code.replace('out%d = fire*vb%d;' % (i, i), 'out%d = fire*(vb%d + 4*(vb%d > 0)*bit(peek(pd, 320 + base + %d), pos));' % (i, i, i, i))
assert 'accamt' not in code
cb['code'] = code
# js files
for b in P['boxes']:
    x = b['box']
    if x.get('text', '').startswith('js unit808_engine_v19.js'): x['text'] = x['text'].replace('unit808_engine_v19.js', 'diode808_engine.js')
    if x.get('filename') == 'unit808_seq_v19.js': x['filename'] = 'diode808_seq.js'
# dials (hidden, drawn by the panel): varname, long name, short, min, max, init, unitstyle, target message
DIALS = [
 ('d_ac','Accent','Accent',0,1,0.5,'control accent'), ('d_flam','Flam','Flam',0,1,0.4,'flam'),
 ('d_vol','Master Volume','Volume',0,1,0.7,'control volume'), ('d_noise','Noise Level','Noise',20,350,190,'cal noiseRms'),
 ('d_bd_lvl','BD Level','Level',0,1,0.8,'control bdLevel'), ('d_bd_tone','BD Tone','Tone',0,1,0.5,'control bdTone'),
 ('d_bd_decay','BD Decay','Decay',0,1,0.5,'control bdDecay'), ('d_bd_tune','Kick Tune','Tune',-12,12,0,'cal kickTune'),
 ('d_sd_lvl','SD Level','Level',0,1,1.0,'control sdLevel'), ('d_sd_tone','SD Tone','Tone',0,1,0.5,'control sdTone'),
 ('d_sd_snappy','SD Snappy','Snappy',0,1,0.5,'control sdSnappy'), ('d_sd_rev','Snare Revision','SD Rev',None,None,1,'cal lateSnare'),
 ('d_lt_lvl','LT/LC Level','Level',0,1,0.8,'control ltLevel'), ('d_lt_tun','LT/LC Tuning','Tuning',0,1,0.5,'control ltTuning'),
 ('d_mt_lvl','MT/MC Level','Level',0,1,0.8,'control mtLevel'), ('d_mt_tun','MT/MC Tuning','Tuning',0,1,0.5,'control mtTuning'),
 ('d_ht_lvl','HT/HC Level','Level',0,1,0.8,'control htLevel'), ('d_ht_tun','HT/HC Tuning','Tuning',0,1,0.5,'control htTuning'),
 ('d_rs_lvl','RS/CL Level','Level',0,1,0.8,'control rsLevel'), ('d_cp_lvl','CP Level','Level',0,1,0.8,'control cpLevel'),
 ('d_cp_rev','Clap Revision','CP Rev',None,None,1,'cal lateClap'), ('d_cp_off','Clap Offset','Offset',0,100,37,'cal cpOffset'),
 ('d_cb_lvl','CB Level','Level',0,1,0.8,'control cbLevel'), ('d_cy_lvl','CY Level','Level',0,1,0.8,'control cyLevel'),
 ('d_cy_tone','CY Tone','Tone',0,1,0.5,'control cyTone'), ('d_cy_decay','CY Decay','Decay',0,1,0.5,'control cyDecay'),
 ('d_oh_lvl','OH Level','Level',0,1,0.8,'control ohLevel'), ('d_oh_decay','OH Decay','Decay',0,1,0.5,'control ohDecay'),
 ('d_ch_lvl','CH Level','Level',0,1,0.8,'control chLevel')]
params = {}
for n, (var, lng, sh, lo, hi, init, msg) in enumerate(DIALS):
    y = 40.0 + n * 50
    va = {'parameter_initial': [init], 'parameter_initial_enable': 1, 'parameter_longname': lng, 'parameter_shortname': sh, 'parameter_modmode': 0}
    if lo is None: va.update({'parameter_type': 2, 'parameter_enum': ['Early', 'Late'], 'parameter_mmax': 1})
    else: va.update({'parameter_type': 0, 'parameter_mmin': float(lo), 'parameter_mmax': float(hi), 'parameter_unitstyle': 1})
    add({'id': var, 'varname': var, 'maxclass': 'live.dial', 'numinlets': 1, 'numoutlets': 2, 'outlettype': ['', 'float'], 'parameter_enable': 1,
         'patching_rect': [1500.0, y, 44.0, 45.0], 'saved_attribute_attributes': {'valueof': va}})
    add({'id': 'p_' + var, 'maxclass': 'newobj', 'text': 'prepend ' + msg, 'numinlets': 1, 'numoutlets': 1, 'outlettype': [''], 'patching_rect': [1560.0, y, 150.0, 22.0]})
    add({'id': 'dv_' + var, 'maxclass': 'newobj', 'text': 'prepend dv ' + var, 'numinlets': 1, 'numoutlets': 1, 'outlettype': [''], 'patching_rect': [1720.0, y, 150.0, 22.0]})
    line(var, 0, 'p_' + var, 0); line('p_' + var, 0, 'eng', 0); line(var, 0, 'dv_' + var, 0); line('dv_' + var, 0, 'sq_ui', 0)
    params[var] = [lng, sh, 0]
P['parameters'] = {k: v for k, v in P.get('parameters', {}).items() if not k.startswith('d_')}
P['parameters'].update(params)
P['dependency_cache'] = [{'name': 'diode808_seq.js', 'bootpath': '.', 'type': 'TEXT', 'implicit': 1},
                         {'name': 'diode808_engine.js', 'bootpath': '.', 'type': 'TEXT', 'implicit': 1},
                         {'name': 'diode808~.mxo', 'type': 'iLaX'}]
txt = json.dumps(d, indent=1).encode()
# unfrozen .amxd: ampf header, instrument device ('iiii'), meta, ptch chunk
hdr = b'ampf' + struct.pack('<I', 4) + b'iiii' + b'meta' + struct.pack('<I', 4) + struct.pack('<I', 7) + b'ptch' + struct.pack('<I', len(txt) + 1)
open(OUT, 'wb').write(hdr + txt + b'\0')
print('boxes', len(P['boxes']), 'lines', len(P['lines']))
