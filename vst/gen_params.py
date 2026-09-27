#!/usr/bin/env python3
"""Writes params.json for Monomodule One (VST index = order; append only, never reorder once shipped).
Keys match mnm_engine.cpp. Every knob is a raw 0..127 kit byte, as in upstream One."""
import json

MACHINES = ["GND", "GND SIN", "GND NOIS", "SID 6581", "SWAVE SAW", "SWAVE PULS", "SWAVE ENS", "DPRO WAVE",
            "DPRO BBOX", "DPRO DDRW", "DPRO DENS", "FM+ STAT", "FM+ PAR", "FM+ DYN", "VO-6"]
PAGES = [("syn", "SYN", ["A", "B", "C", "D", "E", "F", "G", "H"]),
         ("amp", "AMP", ["ATK", "HOLD", "DEC", "REL", "DIST", "VOL", "PAN", "PORT"]),
         ("filt", "FILT", ["BASE", "WDTH", "HPQ", "LPQ", "ATK", "DEC", "BOFS", "WOFS"]),
         ("efx", "EFX", ["EQF", "EQG", "SRR", "DTIM", "DSND", "DFB", "DBAS", "DWID"])]
DEF = {"amp": [0, 0, 64, 64, 64, 64, 64, 0], "filt": [0, 127, 0, 0, 0, 32, 64, 64],
       "efx": [64, 64, 0, 64, 64, 28, 0, 127], "syn": [0, 0, 0, 0, 0, 0, 0, 64]}

params = [{"key": "machine", "name": "Machine", "options": MACHINES, "default": 4},
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
                       "default": 0, "display": "int"})
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
json.dump({"name": "Monomodule One", "params": params,
           "sections": [{"label": a, "keys": b} for a, b in sections]}, open("params.json", "w"), indent=1)
print(len(params), "params")
