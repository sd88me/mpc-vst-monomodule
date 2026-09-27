#!/usr/bin/env python3
"""Writes params.json for Monomodule One (VST index = order; append only, never reorder once shipped).
Keys match mnm_engine.cpp. Every knob is a raw 0..127 kit byte, as in upstream One."""
import json
import os
import sys

FX = "--fx" in sys.argv   # Monomodule FX: the FX machines only, no note-related globals; writes fx/params.json

MACHINES = (["THRU", "REVERB", "CHORUS", "DYNAMIX", "RINGMOD", "PHASER", "FLANGER"] if FX else
            ["GND", "GND SIN", "GND NOIS", "SID 6581", "SWAVE SAW", "SWAVE PULS", "SWAVE ENS", "DPRO WAVE",
             "DPRO BBOX", "DPRO DDRW", "DPRO DENS", "FM+ STAT", "FM+ PAR", "FM+ DYN", "VO-6"])
PAGES = [("syn", "SYN", ["A", "B", "C", "D", "E", "F", "G", "H"]),
         ("amp", "AMP", ["ATK", "HOLD", "DEC", "REL", "DIST", "VOL", "PAN", "PORT"]),
         ("filt", "FILT", ["BASE", "WDTH", "HPQ", "LPQ", "ATK", "DEC", "BOFS", "WOFS"]),
         ("efx", "EFX", ["EQF", "EQG", "SRR", "DTIM", "DSND", "DFB", "DBAS", "DWID"])]
DEF = {"amp": [0, 0, 127, 127, 64, 64, 64, 0] if FX else [0, 0, 64, 64, 64, 64, 64, 0], "filt": [0, 127, 0, 0, 0, 32, 64, 64],
       "efx": [64, 64, 0, 64, 64, 28, 0, 127], "syn": [0, 0, 0, 0, 0, 0, 0, 64]}

params = [{"key": "machine", "name": "Machine", "options": MACHINES, "default": 1 if FX else 4},
          {"key": "level", "name": "Level", "min": 0, "max": 127, "default": 100, "display": "int"}]
sections = [("MACHINE", ["machine", "level"])]
for key, label, names in PAGES:
    keys = []
    for k in range(8):
        params.append({"key": "%s%d" % (key, k), "name": "%s %s" % (label, names[k]), "min": 0, "max": 127,
                       "default": DEF[key][k], "display": "int"})
        keys.append("%s%d" % (key, k))
    sections.append((label, keys))
for l in range(3):
    keys = []
    for k in range(8):
        params.append({"key": "lfo%d_%d" % (l + 1, k), "name": "LFO%d %d" % (l + 1, k + 1), "min": 0, "max": 127,
                       "default": [0, 64, 0, 0, 1, 64, 0, 0][k], "display": "int"})
        keys.append("lfo%d_%d" % (l + 1, k))
    sections.append(("LFO%d" % (l + 1), keys))
# hidden helper params for the skin, appended after the knobs (never reorder): the machine list's open flag, each LFO's
# PAGE list index (DEST's names follow it) and the LFO2|LFO3 tab
params.append({"key": "machine__open", "name": "Machine List", "options": ["Closed", "Open"], "default": 0, "popup_of": "machine"})
for l in range(3):
    params.append({"key": "lfo%d_pagesel" % (l + 1), "name": "LFO%d Page" % (l + 1),
                   "options": ["PTCH", "SYNT", "AMP", "FILT", "EFFX", "LFO1", "LFO2", "LFO3", "MIDI"], "default": 0})
params.append({"key": "lfo23tab", "name": "LFO Tab", "options": ["LFO2", "LFO3"], "default": 0})
PAGES9 = ["PTCH", "SYNT", "AMP", "FILT", "EFFX", "LFO1", "LFO2", "LFO3", "MIDI"]
params.append({"key": "lfo23dest", "name": "LFO2/3 Dest View", "options": ["%s %s" % (t, p) for t in ("LFO2", "LFO3") for p in PAGES9], "default": 0})
# presets: prev/next step through this machine's presets (Init first, then the sounds of the dumps); the name is live text
params.append({"key": "preset_prev", "name": "Preset Prev", "min": 0, "max": 1, "momentary": True})
params.append({"key": "preset_next", "name": "Preset Next", "min": 0, "max": 1, "momentary": True})
params.append({"key": "preset_name", "name": "Preset", "min": 0, "max": 0, "display": "string"})
# globals (not part of a preset): master tune in Hz and the filter key tracking of the hardware's KIT > ASSIGN > KEY
if not FX:
  # randomise (momentary; the wrapper springs it back to 0/OFF after firing -- see docs/PORTING.md's "step_of" note
  # for the mechanism). SYN excludes H (TUNE on every pitched machine); AMP+FILT excludes AMP VOL; LFO1+LFO2 has no exceptions.
  params.append({"key": "randomize_syn", "name": "Randomise SYN", "options": ["OFF", "ON"], "default": 0, "momentary": True, "hold_ms": 900})
  params.append({"key": "randomize_ampfilt", "name": "Randomise AMP/FILT", "options": ["OFF", "ON"], "default": 0, "momentary": True, "hold_ms": 900})
  params.append({"key": "randomize_lfo", "name": "Randomise LFO1/2", "options": ["OFF", "ON"], "default": 0, "momentary": True, "hold_ms": 900})
  params.append({"key": "master_tune", "name": "Master Tune", "min": 400, "max": 440, "default": 440, "unit": "Hz", "display": "int"})
  params.append({"key": "lpf_key", "name": "LPF Key Track", "options": ["OFF", "ON"], "default": 1})
  params.append({"key": "hpf_key", "name": "HPF Key Track", "options": ["OFF", "ON"], "default": 1})
out = os.path.join("fx", "params.json") if FX else "params.json"
os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
json.dump({"name": "Monomodule FX" if FX else "Monomodule One", "params": params,
           "sections": [{"label": a, "keys": b} for a, b in sections]}, open(out, "w"), indent=1)
print(len(params), "params")
